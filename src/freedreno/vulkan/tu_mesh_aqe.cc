#include "tu_mesh_aqe.h"

#include <limits.h>
#include <string.h>

static bool
append_region(struct tu_aqe_layout *layout, enum tu_aqe_region region,
              uint64_t bytes)
{
   uint64_t size = (bytes + 31) & ~uint64_t(31);
   if (size > UINT32_MAX || size + layout->size > UINT32_MAX)
      return false;
   layout->regions[region] = {layout->size, (uint32_t) size};
   layout->size += size;
   return true;
}

bool
tu_aqe_layout_for(uint32_t max_vertices, uint32_t max_primitives,
                   enum tu_aqe_topology topology, uint32_t vertex_stride,
                   uint32_t mesh_capacity, uint32_t task_capacity,
                   struct tu_aqe_layout *layout)
{
   if (!layout || !mesh_capacity || mesh_capacity > 256 ||
       !task_capacity || task_capacity > 256 ||
       !max_vertices || max_vertices > 128 ||
       !max_primitives || max_primitives > 64 ||
       topology < TU_AQE_TRIANGLES || topology > TU_AQE_POINTS ||
       vertex_stride < 16 || vertex_stride % 16)
      return false;

   struct tu_aqe_layout result = {};
   result.mesh_capacity = mesh_capacity;
   result.task_capacity = task_capacity;
   result.max_vertices = max_vertices;
   result.max_primitives = max_primitives;
   result.topology = topology;
   result.vertex_stride = vertex_stride;
   uint32_t indices = max_primitives * (topology == TU_AQE_POINTS ? 1u : 4u - topology);
   result.index_stride = ((indices + 3) & ~3u) * 2;
   uint64_t n = 256ull * mesh_capacity;
   if (!append_region(&result, TU_AQE_INDICES, 24 * n) ||
       !append_region(&result, TU_AQE_AUXILIARY, (n >> 2) & ~1ull) ||
       !append_region(&result, TU_AQE_COUNTS, 16ull * mesh_capacity) ||
       !append_region(&result, TU_AQE_TASK_PAYLOAD, 2ull * task_capacity * 16384) ||
       !append_region(&result, TU_AQE_TASK_RECORDS, 48ull * task_capacity) ||
       !append_region(&result, TU_AQE_OUTPUT0, 2ull * mesh_capacity * max_vertices * vertex_stride) ||
       !append_region(&result, TU_AQE_OUTPUT1, 0))
      return false;
   *layout = result;
   return true;
}

static bool
valid_bo(const struct tu_aqe_bo *bo, unsigned alignment)
{
   const uint64_t limit = 1ull << 49;
   return bo->iova && !(bo->iova & (alignment - 1)) && bo->size &&
          bo->iova < limit && bo->size <= limit - bo->iova;
}

static void
write_address(uint32_t *dst, uint64_t iova)
{
   dst[0] = iova;
   dst[1] = iova >> 32;
}

bool
tu_aqe_build_triangle(const struct tu_aqe_triangle_draw *draw,
                       const struct tu_aqe_layout *layout,
                       uint32_t header[TU_AQE_HEADER_DWORDS],
                       uint32_t packet[TU_AQE_PACKET_DWORDS])
{
   if (!draw || !layout || !header || !packet)
      return false;
   struct tu_aqe_layout expected;
   if (!tu_aqe_layout_for(layout->max_vertices, layout->max_primitives,
                          layout->topology, layout->vertex_stride,
                          layout->mesh_capacity, layout->task_capacity, &expected) ||
       memcmp(layout, &expected, sizeof(expected)) ||
       !valid_bo(&draw->arena, 32) || !valid_bo(&draw->parameters, 4) ||
       draw->arena.size < layout->size ||
       draw->parameters.size < 4 * TU_AQE_HEADER_DWORDS ||
       draw->state_offset < 4 * TU_AQE_HEADER_DWORDS ||
       draw->state_offset % 4 || !draw->state_dwords ||
       draw->state_dwords >= (1u << 20) ||
       uint64_t(draw->state_offset) + 4ull * draw->state_dwords >
          draw->parameters.size)
      return false;
   if (draw->arena.iova < draw->parameters.iova + draw->parameters.size &&
       draw->parameters.iova < draw->arena.iova + draw->arena.size)
      return false;

   if (draw->metadata.iova || draw->metadata.size) {
      if (!valid_bo(&draw->metadata, 16) ||
          draw->metadata.size < sizeof(struct tu_aqe_draw_metadata) ||
          draw->metadata.size > UINT32_MAX ||
          (draw->metadata.iova < draw->arena.iova + draw->arena.size &&
           draw->arena.iova < draw->metadata.iova + draw->metadata.size))
         return false;
   }

   if (draw->indirect.iova) {
      if (!valid_bo(&draw->indirect, 4) || draw->indirect.size < 12)
         return false;
   } else {
      if (draw->indirect.size)
         return false;
      uint64_t total = 1;
      for (unsigned i = 0; i < 3; i++) {
         if (!draw->groups[i] || draw->groups[i] > 65535)
            return false;
         total *= draw->groups[i];
      }
      if (total > (1u << 22))
         return false;
   }

   uint32_t h[TU_AQE_HEADER_DWORDS] = {};
   uint32_t p[TU_AQE_PACKET_DWORDS] = {};
   write_address(h + 4, draw->parameters.iova + draw->state_offset);
   h[6] = draw->state_dwords;
   h[0] = layout->topology << 5;
   h[7] = layout->index_stride / 2;
   h[9] = layout->max_vertices * layout->vertex_stride;
   h[15] = layout->max_vertices;
   h[16] = layout->max_primitives;
   h[17] = 1;
   h[19] = 0x7d;

   p[0] = 0x707a001f;
   p[1] = 5;
   memcpy(p + 2, draw->groups, sizeof(draw->groups));
   write_address(p + 5, draw->parameters.iova);
   p[7] = 0x40;
   p[9] = layout->task_capacity;
   p[10] = layout->mesh_capacity;
   const enum tu_aqe_region order[] = {
      TU_AQE_COUNTS, TU_AQE_INDICES, TU_AQE_OUTPUT0, TU_AQE_OUTPUT1,
      TU_AQE_AUXILIARY, TU_AQE_TASK_PAYLOAD, TU_AQE_TASK_RECORDS,
   };
   for (unsigned i = 0; i < TU_AQE_REGION_COUNT; i++) {
      const struct tu_aqe_span *span = &layout->regions[order[i]];
      write_address(p + 11 + i * 3, draw->arena.iova + span->offset);
      p[13 + i * 3] = span->size;
   }
   if (draw->metadata.iova) {
      write_address(p + 26, draw->metadata.iova);
      p[28] = draw->metadata.size;
   }
   if (draw->indirect.iova) {
      p[0] = 0x707a801e;
      p[1] = 7;
      write_address(p + 2, draw->indirect.iova);
      memmove(p + 4, p + 5, (TU_AQE_PACKET_DWORDS - 5) * 4);
      p[TU_AQE_INDIRECT_PACKET_DWORDS] = 0;
   }
   memcpy(header, h, sizeof(h));
   memcpy(packet, p, sizeof(p));
   return true;
}
