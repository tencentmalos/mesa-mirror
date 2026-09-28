static VkResult
x11_android_create_mem(const struct wsi_swapchain *chain,
                       const struct wsi_image_info *info,
                       struct wsi_image *base)
{
   struct x11_image *image = (struct x11_image *)base;
   VK_FROM_HANDLE(vk_device, device, chain->device);
   uint32_t format;
   switch (info->create.format) {
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      format = 5;
      break;
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
      format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
      break;
   default:
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   AHardwareBuffer_Desc desc = {
      .width = info->create.extent.width,
      .height = info->create.extent.height,
      .layers = 1,
      .format = format,
      .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
               AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER,
   };
   if (AHardwareBuffer_allocate(&desc, &image->ahb))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   VkAndroidHardwareBufferPropertiesANDROID props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
   };
   VkResult result = device->dispatch_table.GetAndroidHardwareBufferPropertiesANDROID(
      chain->device, image->ahb, &props);
   if (result != VK_SUCCESS)
      return result;
   if (!props.memoryTypeBits)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   VkImportAndroidHardwareBufferInfoANDROID import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
      .buffer = image->ahb,
   };
   VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &import,
      .image = base->image,
   };
   VkMemoryAllocateInfo allocate = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated,
      .allocationSize = props.allocationSize,
      .memoryTypeIndex = ffs(props.memoryTypeBits) - 1,
   };
   return chain->wsi->AllocateMemory(chain->device, &allocate, &chain->alloc, &base->memory);
}

VkResult
wsi_x11_configure_android_image(const struct wsi_swapchain *chain,
                                const VkSwapchainCreateInfoKHR *info,
                                struct wsi_image_info *image_info)
{
   VkResult result = wsi_configure_image(chain, info,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID, image_info);
   if (result == VK_SUCCESS) {
      image_info->image_type = WSI_IMAGE_TYPE_ANDROID;
      image_info->color_space = info->imageColorSpace;
      image_info->create_mem = x11_android_create_mem;
   }
   return result;
}

static VkResult
x11_android_image_attach(struct x11_swapchain *chain, struct x11_image *image)
{
   int sockets[2];
   if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets))
      goto fail;
   struct timeval timeout = { .tv_sec = 5 };
   setsockopt(sockets[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
   AHardwareBuffer_Desc desc;
   AHardwareBuffer_describe(image->ahb, &desc);
   image->pixmap = xcb_generate_id(chain->conn);
   xcb_void_cookie_t cookie = xcb_dri3_pixmap_from_buffers_checked(
      chain->conn, image->pixmap, chain->window, 1,
      chain->extent.width, chain->extent.height,
      desc.stride * 4, 0, 0, 0, 0, 0, 0, 0,
      chain->depth, 32, 1255, &sockets[1]);
   xcb_flush(chain->conn);
   struct pollfd poll_fd = { .fd = sockets[0], .events = POLLIN };
   uint8_t ready;
   bool sent = poll(&poll_fd, 1, 5000) > 0 &&
               read(sockets[0], &ready, 1) == 1 &&
               AHardwareBuffer_sendHandleToUnixSocket(image->ahb, sockets[0]) == 0;
   close(sockets[0]);
   xcb_generic_error_t *error = xcb_request_check(chain->conn, cookie);
   if (!sent || error) {
      free(error);
      goto fail;
   }
   return VK_SUCCESS;
fail:
   if (image->pixmap)
      xcb_free_pixmap(chain->conn, image->pixmap);
   wsi_destroy_image(&chain->base, &image->base);
   if (image->ahb) {
      AHardwareBuffer_release(image->ahb);
      image->ahb = NULL;
   }
   return VK_ERROR_INITIALIZATION_FAILED;
}
