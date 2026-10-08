#pragma once

#include "tu_common.h"

struct tu_cmd_buffer;
struct tu_image_view;
struct tu_fdm_snapshot;

bool tu_deferred_needs_render_pass(const VkRenderPassBeginInfo *info);
bool tu_deferred_needs_rendering(const VkRenderingInfo *info);
void tu_init_deferred_dispatch(struct tu_device *device);
void tu_deferred_execute(struct tu_cmd_buffer *primary,
                         const struct tu_cmd_buffer *source);
void tu_deferred_capture_render_pass(struct tu_cmd_buffer *cmd,
                                     const VkRenderPassBeginInfo *info);
void tu_deferred_capture_rendering(struct tu_cmd_buffer *cmd,
                                   const VkRenderingInfo *info);
