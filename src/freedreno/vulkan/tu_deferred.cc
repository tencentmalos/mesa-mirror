#include "tu_deferred.h"

#include "tu_cmd_buffer.h"
#include "tu_device.h"
#include "tu_image.h"
#include "tu_pass.h"
#include "vk_cmd_queue.h"
#include "vk_util.h"

bool
tu_deferred_needs_render_pass(const VkRenderPassBeginInfo *info)
{
   VK_FROM_HANDLE(tu_render_pass, pass, info->renderPass);
   return pass->has_fdm;
}

bool
tu_deferred_needs_rendering(const VkRenderingInfo *info)
{
   const auto *fdm = (const VkRenderingFragmentDensityMapAttachmentInfoEXT *)
      vk_find_struct_const(info->pNext, RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT);
   return fdm && fdm->imageView != VK_NULL_HANDLE;
}

struct tu_fdm_snapshot {
   struct tu_fdm_snapshot *next;
   const struct vk_cmd_queue_entry *entry;
   const uint8_t *data;
};

static void
tu_deferred_capture(struct tu_cmd_buffer *cmd, const struct tu_image_view *view)
{
   if (vk_command_buffer_has_error(&cmd->vk))
      return;

   if (!cmd->fdm_snapshots_ctx) {
      cmd->fdm_snapshots_ctx = ralloc_context(nullptr);
      if (!cmd->fdm_snapshots_ctx) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return;
      }
   }
   auto *snapshot = rzalloc(cmd->fdm_snapshots_ctx, struct tu_fdm_snapshot);
   if (!snapshot) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   snapshot->entry = list_last_entry(&cmd->vk.cmd_queue.cmds,
                                     struct vk_cmd_queue_entry, cmd_link);
   if (view && !(view->vk.create_flags &
                 VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DEFERRED_BIT_EXT)) {
      size_t pitch = (size_t)view->vk.extent.width * view->image->layout[0].cpp;
      size_t layer_size = pitch * view->vk.extent.height;
      auto *data = (uint8_t *)ralloc_size(snapshot, layer_size * view->vk.layer_count);
      if (!data) {
         ralloc_free(snapshot);
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return;
      }
      for (uint32_t layer = 0; layer < view->vk.layer_count; layer++) {
         for (uint32_t y = 0; y < view->vk.extent.height; y++) {
            memcpy(data + layer_size * layer + pitch * y,
                   (const uint8_t *)view->image->map + view->view.offset +
                      view->view.layer_size * layer + (size_t)view->view.pitch * y,
                   pitch);
         }
      }
      snapshot->data = data;
   }
   if (cmd->fdm_snapshots_tail)
      cmd->fdm_snapshots_tail->next = snapshot;
   else
      cmd->fdm_snapshots = snapshot;
   cmd->fdm_snapshots_tail = snapshot;
}

void
tu_deferred_capture_render_pass(struct tu_cmd_buffer *cmd,
                                const VkRenderPassBeginInfo *info)
{
   VK_FROM_HANDLE(tu_render_pass, pass, info->renderPass);
   VK_FROM_HANDLE(tu_framebuffer, fb, info->framebuffer);
   const auto *attachments = (const VkRenderPassAttachmentBeginInfo *)
      vk_find_struct_const(info->pNext, RENDER_PASS_ATTACHMENT_BEGIN_INFO);
   uint32_t a = pass->fragment_density_map.attachment;
   const struct tu_image_view *view = nullptr;
   if (a != VK_ATTACHMENT_UNUSED)
      view = attachments ? tu_image_view_from_handle(attachments->pAttachments[a])
                         : fb->attachments[a];
   tu_deferred_capture(cmd, view);
}

void
tu_deferred_capture_rendering(struct tu_cmd_buffer *cmd,
                              const VkRenderingInfo *info)
{
   const auto *fdm = (const VkRenderingFragmentDensityMapAttachmentInfoEXT *)
      vk_find_struct_const(info->pNext, RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT);
   tu_deferred_capture(cmd, fdm ? tu_image_view_from_handle(fdm->imageView) : nullptr);
}

void
tu_deferred_execute(struct tu_cmd_buffer *primary,
                    const struct tu_cmd_buffer *source)
{
   if (vk_command_buffer_has_error(&source->vk)) {
      vk_command_buffer_set_error(&primary->vk, source->vk.record_result);
      return;
   }
   const struct tu_fdm_snapshot *snapshot = source->fdm_snapshots;
   list_for_each_entry(struct vk_cmd_queue_entry, entry,
                       &source->vk.cmd_queue.cmds, cmd_link) {
      if (snapshot && snapshot->entry == entry) {
         primary->fdm_host_snapshot = snapshot->data;
         snapshot = snapshot->next;
      }
      vk_cmd_queue_execute_command(entry, tu_cmd_buffer_to_handle(primary),
                                    &primary->device->deferred_dispatch);
      if (vk_command_buffer_has_error(&primary->vk))
         return;
   }
}
