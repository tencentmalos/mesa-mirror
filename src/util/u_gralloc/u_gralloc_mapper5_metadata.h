/* SPDX-License-Identifier: MIT */
#ifndef U_GRALLOC_MAPPER5_METADATA_H
#define U_GRALLOC_MAPPER5_METADATA_H
#include <cstdint>
#include <cstring>
#include <climits>
#include <cstddef>
#include <string_view>

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
   bool string(std::string_view &out) {
      int64_t n;
      if (!read(n) || n < 0 || uint64_t(n) > left || n > 256) return valid = false;
      out = {reinterpret_cast<const char *>(p), size_t(n)};
      p += n; left -= n; return true;
   }
   bool string(const char *expected = nullptr) {
      std::string_view value;
      return string(value) && (!expected || value == expected);
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
inline bool extendable(const void *data, size_t size, int64_t type,
                       const char *name, int64_t &value) {
   Reader r{static_cast<const uint8_t *>(data), size};
   return r.header(type) && r.string(name) && r.read(value) && r.done();
}
struct Planes {
   int count;
   int offsets[4];
   int strides[4];
   uint64_t sizes[4];
   bool qti_metadata[4]{};
   int64_t widths[4], heights[4], increments[4];
};
inline bool planes(const void *data, size_t size, uint64_t allocation, Planes &out) {
   Reader r{static_cast<const uint8_t *>(data),size};
   int64_t count;
   if (!r.header(15) || !r.read(count) || count < 1 || count > 4) return false;
   out = {};
   out.count = count;
   for (int i = 0; i < out.count; ++i) {
      int64_t components;
      if (!r.read(components) || components < 0 || components > 16) return false;
      for (int64_t c = 0; c < components; ++c) {
         int64_t kind, offset, bits;
         std::string_view name;
         if (!r.string(name) || !r.read(kind) || !r.read(offset) || !r.read(bits) ||
             offset < 0 || bits < 0) return false;
         if (name == "QTI" && (kind == INT32_MIN || kind == uint64_t(1) << 31))
            out.qti_metadata[i] = true;
      }
      int64_t offset, increment, stride, width, height, total, sub_x, sub_y;
      if (!r.read(offset) || !r.read(increment) || !r.read(stride) ||
          !r.read(width) || !r.read(height) || !r.read(total) ||
          !r.read(sub_x) || !r.read(sub_y)) return false;
      if (offset < 0 || offset > INT_MAX || stride <= 0 || stride > INT_MAX ||
          increment < 0 || width <= 0 || height <= 0 || total <= 0 ||
          (out.qti_metadata[i] ? (increment != 0 || sub_x != 0 || sub_y != 0)
                               : (sub_x <= 0 || sub_y <= 0)) ||
          uint64_t(offset) > allocation ||
          uint64_t(total) > allocation - uint64_t(offset)) return false;
      out.offsets[i] = offset; out.strides[i] = stride; out.sizes[i] = total;
      out.widths[i] = width; out.heights[i] = height; out.increments[i] = increment;
   }
   return r.done();
}
inline bool qti_rgb32_ubwc(Planes &p) {
   if (p.count != 2 || p.qti_metadata[0] || !p.qti_metadata[1] ||
       p.offsets[1] != 0 || p.increments[0] != 32 ||
       p.widths[0] > INT_MAX || p.heights[0] > INT_MAX ||
       p.widths[0] != p.widths[1] || p.heights[0] != p.heights[1] ||
       p.strides[0] % 256 || uint64_t(p.strides[0]) < uint64_t(p.widths[0]) * 4)
      return false;
   auto align = [](uint64_t n, uint64_t a) { return (n + a - 1) / a * a; };
   const uint64_t meta_pitch = align((p.widths[0] + 15) / 16, 64);
   const uint64_t meta_height = align((p.heights[0] + 3) / 4, 16);
   const uint64_t meta_size = align(meta_pitch * meta_height, 4096);
   const uint64_t pixel_size = align(uint64_t(p.strides[0]) * align(p.heights[0], 16), 4096);
   if (uint64_t(p.strides[1]) != meta_pitch || p.sizes[1] != meta_size ||
       uint64_t(p.offsets[0]) != meta_size || p.sizes[0] != pixel_size)
      return false;
   p.offsets[0] = 0;
   p.sizes[0] += p.sizes[1];
   p.count = 1;
   return true;
}

}
#endif
