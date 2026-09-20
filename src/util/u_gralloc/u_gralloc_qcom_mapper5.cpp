/* SPDX-License-Identifier: MIT */
#include <cstddef>
#include <cstdlib>
#include <new>
#include <cerrno>
#include <dlfcn.h>
#include <android/hardware/graphics/mapper/IMapper.h>
#include "util/log.h"
#include "u_gralloc_internal.h"
#include "u_gralloc_mapper5_metadata.h"

struct qcom_mapper5 {
   u_gralloc base;
   void *library;
   AIMapper *mapper;
};

/* Standalone NDK builds cannot link libui's C++ GraphicBufferMapper API. Use
 * the stable-C Mapper 5 HAL on QCOM instead of guessing private-handle fields.
 * Older devices without this HAL retain their existing gralloc backend.
 */
static int
get_buffer_info(u_gralloc *base, u_gralloc_buffer_handle *hnd,
                u_gralloc_buffer_basic_info *out)
{
   if (!hnd->handle || hnd->handle->numFds <= 0 ||
       is_hal_format_yuv(hnd->hal_format))
      return -ENOTSUP; /* Color-space metadata is not implemented here yet. */

   auto *gr = reinterpret_cast<qcom_mapper5 *>(base);
   buffer_handle_t imported = nullptr;
   if (gr->mapper->v5.importBuffer(hnd->handle, &imported) != AIMAPPER_ERROR_NONE)
      return -EINVAL;
   struct Release {
      AIMapper *mapper;
      buffer_handle_t handle;
      ~Release() { mapper->v5.freeBuffer(handle); }
   } release{gr->mapper, imported};

   uint8_t metadata[16384];
   auto get = [&](int64_t type) -> size_t {
      int32_t n = gr->mapper->v5.getStandardMetadata(imported, type, metadata,
                                                   sizeof(metadata));
      return n > 0 && size_t(n) <= sizeof(metadata) ? n : 0;
   };
   using namespace mapper5_metadata;
   size_t size = get(7); /* PIXEL_FORMAT_FOURCC */
   if (!size || !scalar(metadata, size, 7, out->drm_fourcc)) return -EINVAL;
   size = get(8); /* PIXEL_FORMAT_MODIFIER */
   if (!size || !scalar(metadata, size, 8, out->modifier)) return -EINVAL;
   size = get(10); /* ALLOCATION_SIZE */
   if (!size || !scalar(metadata, size, 10, out->alloc_size) || !out->alloc_size)
      return -EINVAL;
   size = get(5); /* LAYER_COUNT */
   if (!size || !scalar(metadata, size, 5, out->layer_count) || !out->layer_count)
      return -EINVAL;
   Planes layout{};
   size = get(15); /* PLANE_LAYOUTS */
   if (!size || !planes(metadata, size, out->alloc_size, layout)) return -EINVAL;

   int fd_index = 0;
   out->num_planes = layout.count;
   for (int i = 0; i < layout.count; ++i) {
      if (i > 0 && layout.offsets[i] == 0) ++fd_index;
      if (fd_index >= hnd->handle->numFds) return -EINVAL;
      out->offsets[i] = layout.offsets[i];
      out->strides[i] = layout.strides[i];
      // Retain the caller-owned fd, not a handle freed by Release above.
      out->fds[i] = hnd->handle->data[fd_index];
   }
   return 0;
}

static int destroy(u_gralloc *base)
{
   auto *gr = reinterpret_cast<qcom_mapper5 *>(base);
   dlclose(gr->library);
   delete gr;
   return 0;
}

extern "C" u_gralloc *u_gralloc_qcom_mapper5_create(void)
{
#if defined(__LP64__)
   const char *path = "/vendor/lib64/hw/mapper.qti.so";
#else
   const char *path = "/vendor/lib/hw/mapper.qti.so";
#endif
   void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!library) return nullptr;
   auto load = reinterpret_cast<decltype(&AIMapper_loadIMapper)>(
      dlsym(library, "AIMapper_loadIMapper"));
   AIMapper *mapper = nullptr;
   if (!load || load(&mapper) != AIMAPPER_ERROR_NONE || !mapper ||
       mapper->version < AIMAPPER_VERSION_5 || !mapper->v5.importBuffer ||
       !mapper->v5.freeBuffer || !mapper->v5.getStandardMetadata) {
      dlclose(library);
      return nullptr;
   }
   auto *gr = new (std::nothrow) qcom_mapper5{};
   if (!gr) { dlclose(library); return nullptr; }
   gr->library = library;
   gr->mapper = mapper;
   gr->base.ops.get_buffer_basic_info = get_buffer_info;
   gr->base.ops.destroy = destroy;
   mesa_logi("Using QCOM Mapper 5 stable-C metadata");
   return &gr->base;
}
