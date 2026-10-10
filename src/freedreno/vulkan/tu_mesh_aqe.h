#ifndef TU_MESH_AQE_H
#define TU_MESH_AQE_H

#include <stdint.h>

#define TU_AQE_HEADER_DWORDS 26
#define TU_AQE_PACKET_DWORDS 32
#define TU_AQE_INDIRECT_PACKET_DWORDS 31
#define TU_AQE_CONSTANT_VEC4S 5

enum tu_aqe_region {
   TU_AQE_INDICES,
   TU_AQE_AUXILIARY,
   TU_AQE_COUNTS,
   TU_AQE_TASK_PAYLOAD,
   TU_AQE_TASK_RECORDS,
   TU_AQE_OUTPUT0,
   TU_AQE_OUTPUT1,
   TU_AQE_REGION_COUNT,
};

struct tu_aqe_span {
   uint32_t offset;
   uint32_t size;
};

enum tu_aqe_topology {
   TU_AQE_TRIANGLES,
   TU_AQE_LINES,
   TU_AQE_POINTS,
};

struct tu_aqe_layout {
   struct tu_aqe_span regions[TU_AQE_REGION_COUNT];
   uint32_t size;
   uint32_t mesh_capacity;
   uint32_t task_capacity;
   uint32_t max_vertices;
   uint32_t max_primitives;
   enum tu_aqe_topology topology;
   uint32_t vertex_stride;
   uint32_t index_stride;
};

struct tu_aqe_bo {
   uint64_t iova;
   uint64_t size;
};

struct tu_aqe_draw_metadata {
   uint32_t draw_id;
   uint32_t view_index;
   uint32_t reserved[2];
   uint32_t push_constants[64];
};

struct tu_aqe_triangle_draw {
   struct tu_aqe_bo arena;
   struct tu_aqe_bo parameters;
   uint32_t state_offset;
   uint32_t state_dwords;
   uint32_t groups[3];
   struct tu_aqe_bo indirect;
   struct tu_aqe_bo metadata;
};

bool tu_aqe_layout_for(uint32_t max_vertices, uint32_t max_primitives,
                       enum tu_aqe_topology topology, uint32_t vertex_stride,
                       uint32_t mesh_capacity, uint32_t task_capacity,
                       struct tu_aqe_layout *layout);
bool tu_aqe_build_triangle(const struct tu_aqe_triangle_draw *draw,
                           const struct tu_aqe_layout *layout,
                           uint32_t header[TU_AQE_HEADER_DWORDS],
                           uint32_t packet[TU_AQE_PACKET_DWORDS]);

#endif
