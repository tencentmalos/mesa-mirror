#ifndef TU_MESH_AQE_RESOURCES_H
#define TU_MESH_AQE_RESOURCES_H

#include "util/u_dynarray.h"
#include <vulkan/vulkan_core.h>

struct tu_device;
struct tu_bo;
struct tu_queue;
struct vk_sync;

struct tu_aqe_submission {
   unsigned refs;
   struct tu_queue *queue;
   struct vk_sync *sync;
};

struct tu_aqe_resources {
   struct tu_bo *arena;
   struct tu_aqe_submission *last_submission;
   struct util_dynarray secondaries;
};

VkResult tu_aqe_resources_require(struct tu_device *dev,
                                  struct tu_aqe_resources *resources,
                                  uint64_t size);
void tu_aqe_resources_finish(struct tu_device *dev,
                             struct tu_aqe_resources *resources);
void tu_aqe_resources_add_secondary(struct tu_aqe_resources *resources,
                                    struct tu_aqe_resources *secondary);
void tu_aqe_resources_collect(struct tu_aqe_resources *resources,
                              struct util_dynarray *owners);
void tu_aqe_submission_unref(struct tu_device *dev,
                             struct tu_aqe_submission *submission);

#endif
