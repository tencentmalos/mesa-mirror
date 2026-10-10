#include "tu_mesh_aqe_resources.h"
#include "tu_device.h"
#include "tu_knl.h"
#include "vk_sync.h"

VkResult
tu_aqe_resources_require(struct tu_device *dev,
                         struct tu_aqe_resources *resources, uint64_t size)
{
   if (resources->arena && resources->arena->size >= size)
      return VK_SUCCESS;
   struct tu_bo *arena;
   VkResult result = tu_bo_init_new(dev, NULL, &arena, size,
      TU_BO_ALLOC_INTERNAL_RESOURCE, "command buffer AQE arena");
   if (result != VK_SUCCESS)
      return result;
   util_dynarray_foreach (&resources->relocations, struct tu_aqe_relocation, reloc) {
      const uint64_t address = arena->iova + reloc->offset;
      reloc->words[0] = address;
      reloc->words[1] = address >> 32;
   }
   if (resources->arena)
      tu_bo_finish(dev, resources->arena);
   resources->arena = arena;
   return VK_SUCCESS;
}

void
tu_aqe_resources_relocate(struct tu_aqe_resources *resources,
                          uint32_t *words, uint64_t offset)
{
   assert(resources->arena && offset <= resources->arena->size);
   struct tu_aqe_relocation reloc = {words, offset};
   util_dynarray_append(&resources->relocations, reloc);
}

void
tu_aqe_submission_unref(struct tu_device *dev,
                        struct tu_aqe_submission *submission)
{
   if (submission && p_atomic_dec_zero(&submission->refs)) {
      vk_sync_destroy(&dev->vk, submission->sync);
      free(submission);
   }
}

void
tu_aqe_resources_finish(struct tu_device *dev,
                        struct tu_aqe_resources *resources)
{
   if (resources->arena)
      tu_bo_finish(dev, resources->arena);
   tu_aqe_submission_unref(dev, resources->last_submission);
   util_dynarray_fini(&resources->secondaries);
   util_dynarray_fini(&resources->relocations);
   *resources = {};
}

void
tu_aqe_resources_add_secondary(struct tu_aqe_resources *resources,
                               struct tu_aqe_resources *secondary)
{
   if (secondary->arena || secondary->secondaries.size)
      util_dynarray_append(&resources->secondaries, secondary);
}

void
tu_aqe_resources_collect(struct tu_aqe_resources *resources,
                         struct util_dynarray *owners)
{
   if (resources->arena) {
      bool present = false;
      util_dynarray_foreach (owners, struct tu_aqe_resources *, owner)
         present |= *owner == resources;
      if (!present)
         util_dynarray_append(owners, resources);
   }
   util_dynarray_foreach (&resources->secondaries, struct tu_aqe_resources *, secondary)
      tu_aqe_resources_collect(*secondary, owners);
}
