#include "tu_mesh_aqe_resources.h"
#include "tu_mesh_aqe.h"
#include "tu_device.h"
#include "tu_knl.h"
#include "vk_sync.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); abort(); } } while (0)

static uint64_t next_iova = 0x100000000ull;
static unsigned allocated, released, syncs_released;
static bool allocation_fails;

VkResult
tu_bo_init_new_explicit_iova(struct tu_device *, struct vk_object_base *,
                            struct tu_bo **out, uint64_t size, uint64_t,
                            uint64_t, VkMemoryPropertyFlags,
                            enum tu_bo_alloc_flags flags, struct tu_sparse_vma *, const char *)
{
   CHECK(flags == TU_BO_ALLOC_INTERNAL_RESOURCE);
   if (allocation_fails)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   *out = (struct tu_bo *)calloc(1, sizeof(struct tu_bo));
   (*out)->iova = next_iova;
   (*out)->size = size;
   next_iova += ALIGN_POT(size, 4096);
   allocated++;
   return VK_SUCCESS;
}

void
tu_bo_finish(struct tu_device *, struct tu_bo *bo)
{
   released++;
   free(bo);
}

static void
finish_sync(struct vk_device *, struct vk_sync *sync)
{
   CHECK(sync);
   syncs_released++;
}

static void VKAPI_CALL
free_sync(void *, void *memory)
{
   free(memory);
}

int main()
{
   struct tu_device dev = {};
   dev.vk.alloc.pfnFree = free_sync;
   struct tu_aqe_resources a = {}, b = {}, secondary = {};
   struct tu_aqe_layout layout;
   CHECK(tu_aqe_layout_for(3, 1, TU_AQE_TRIANGLES, 16, 256, 256, &layout));
   allocation_fails = true;
   CHECK(tu_aqe_resources_require(&dev, &a, layout.size) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
   CHECK(!a.arena);
   allocation_fails = false;
   CHECK(tu_aqe_resources_require(&dev, &a, layout.size) == VK_SUCCESS);
   CHECK(tu_aqe_resources_require(&dev, &b, layout.size) == VK_SUCCESS);
   CHECK(tu_aqe_resources_require(&dev, &secondary, layout.size) == VK_SUCCESS);
   CHECK(a.arena->iova + a.arena->size <= b.arena->iova);
   CHECK(b.arena->iova + b.arena->size <= secondary.arena->iova);
   auto *original = a.arena;
   CHECK(tu_aqe_resources_require(&dev, &a, layout.size) == VK_SUCCESS);
   CHECK(a.arena == original && allocated == 3);

   uint32_t address[2] = {};
   tu_aqe_resources_relocate(&a, address, 128);
   allocation_fails = true;
   CHECK(tu_aqe_resources_require(&dev, &a, layout.size * 2) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
   CHECK(a.arena == original && !address[0] && !address[1]);
   allocation_fails = false;
   CHECK(tu_aqe_resources_require(&dev, &a, layout.size * 2) == VK_SUCCESS);
   CHECK(a.arena != original && allocated == 4 && released == 1);
   CHECK((address[0] | (uint64_t(address[1]) << 32)) == a.arena->iova + 128);

   tu_aqe_resources_add_secondary(&a, &secondary);
   tu_aqe_resources_add_secondary(&b, &secondary);
   struct util_dynarray owners = UTIL_DYNARRAY_INIT;
   tu_aqe_resources_collect(&a, &owners);
   tu_aqe_resources_collect(&b, &owners);
   CHECK(util_dynarray_num_elements(&owners, struct tu_aqe_resources *) == 3);
   util_dynarray_fini(&owners);

   auto *completion = (struct tu_aqe_submission *)calloc(1, sizeof(struct tu_aqe_submission));
   completion->refs = 2;
   completion->sync = (struct vk_sync *)calloc(1, sizeof(struct vk_sync));
   static const struct vk_sync_type sync_type = {.size = sizeof(struct vk_sync), .finish = finish_sync};
   completion->sync->type = &sync_type;
   a.last_submission = b.last_submission = completion;
   tu_aqe_resources_finish(&dev, &a);
   CHECK(!a.arena && !a.last_submission && !a.secondaries.size && !a.relocations.size);
   CHECK(released == 2 && syncs_released == 0 && secondary.arena);
   CHECK(tu_aqe_resources_require(&dev, &a, layout.size) == VK_SUCCESS);
   CHECK(allocated == 5 && a.arena->iova >= secondary.arena->iova + secondary.arena->size);
   tu_aqe_resources_finish(&dev, &b);
   CHECK(syncs_released == 1 && secondary.arena);
   tu_aqe_resources_finish(&dev, &secondary);
   tu_aqe_resources_finish(&dev, &a);
   CHECK(allocated == released);
   return 0;
}
