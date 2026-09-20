/* SPDX-License-Identifier: MIT */
#ifndef U_GRALLOC_MAPPER5_METADATA_H
#define U_GRALLOC_MAPPER5_METADATA_H
#include <cstdint>
#include <cstring>
#include <climits>
#include <cstddef>

namespace mapper5_metadata {
/* Android StandardMetadataType encoding uses fixed-width values and a checked
 * namespace/type header. Bound every read, string and collection before use.
 */
struct Reader {
   const uint8_t *p;
   size_t left;
   bool valid = true;
   template <typename T> bool read(T &v) {
      if (!valid || sizeof(T) > left) return valid = false;
      memcpy(&v, p, sizeof(T)); p += sizeof(T); left -= sizeof(T); return true;
   }
   bool string(const char *expected = nullptr) {
      int64_t n;
      if (!read(n) || n < 0 || uint64_t(n) > left || n > 256) return valid = false;
      if (expected && (size_t(n) != strlen(expected) || memcmp(p, expected, n)))
         return valid = false;
      p += n; left -= n; return true;
   }
   bool header(int64_t type) {
      int64_t actual;
      return string("android.hardware.graphics.common.StandardMetadataType") &&
             read(actual) && actual == type;
   }
   bool done() const { return valid && left == 0; }
};
template <typename T>
bool scalar(const void *data, size_t size, int64_t type, T &out) {
   Reader r{static_cast<const uint8_t *>(data),size};
   return r.header(type) && r.read(out) && r.done();
}
struct Planes {
   int count;
   int offsets[4];
   int strides[4];
   uint64_t sizes[4];
};
inline bool planes(const void *data, size_t size, uint64_t allocation, Planes &out) {
   Reader r{static_cast<const uint8_t *>(data),size};
   int64_t count;
   if (!r.header(15) || !r.read(count) || count < 1 || count > 4) return false;
   out.count = count;
   for (int i = 0; i < out.count; ++i) {
      int64_t components;
      if (!r.read(components) || components < 0 || components > 16) return false;
      for (int64_t c = 0; c < components; ++c) {
         int64_t kind, offset, bits;
         if (!r.string() || !r.read(kind) || !r.read(offset) || !r.read(bits) ||
             offset < 0 || bits < 0) return false;
      }
      int64_t offset, increment, stride, width, height, total, sub_x, sub_y;
      if (!r.read(offset) || !r.read(increment) || !r.read(stride) ||
          !r.read(width) || !r.read(height) || !r.read(total) ||
          !r.read(sub_x) || !r.read(sub_y)) return false;
      if (offset < 0 || offset > INT_MAX || stride <= 0 || stride > INT_MAX ||
          increment < 0 || width <= 0 || height <= 0 || total <= 0 ||
          sub_x <= 0 || sub_y <= 0 || uint64_t(offset) > allocation ||
          uint64_t(total) > allocation - uint64_t(offset)) return false;
      out.offsets[i] = offset; out.strides[i] = stride; out.sizes[i] = total;
   }
   return r.done();
}
}
#endif
