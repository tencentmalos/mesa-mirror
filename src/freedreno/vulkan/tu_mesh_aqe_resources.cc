#include "tu_mesh_aqe_resources.h"
#include "tu_device.h"
#include "tu_knl.h"
#include "vk_sync.h"

VkResult
tu_aqe_resources_require(struct tu_device *dev,
                         struct tu_aqe_resources *resources, uint64_t size)
{
   if (resources->arena) {
      assert(resources->arena->size >= size);
      return VK_SUCCESS;
   }
   return tu_bo_init_new(dev, NULL, &resources->arena, size,
                         TU_BO_ALLOC_INTERNAL_RESOURCE, "command buffer AQE arena");
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
