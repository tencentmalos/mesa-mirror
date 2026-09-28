#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace tu_qcom_surface {

constexpr size_t metadata_size = 5300;
constexpr uint32_t signature = 0xde5cda7a;

enum class result { absent, valid, invalid };

struct layout {
   uint64_t data_offset, data_size, flags_offset, flags_size;
   uint32_t width, height, pitch, flags_pitch, format, layers;
};

static inline uint32_t
u32(const uint8_t *data, size_t offset)
{
   return uint32_t(data[offset]) | uint32_t(data[offset + 1]) << 8 |
          uint32_t(data[offset + 2]) << 16 | uint32_t(data[offset + 3]) << 24;
}

static inline uint64_t
u64(const uint8_t *data, size_t offset)
{
   return u32(data, offset) | uint64_t(u32(data, offset + 4)) << 32;
}

static inline result
decode(const uint8_t *data, size_t size, uint64_t allocation_size, layout &out)
{
   if (size < 4 || u32(data, size - 4) != signature)
      return result::absent;
   if (size != metadata_size || allocation_size < metadata_size ||
       u32(data, 5292) != metadata_size)
      return result::invalid;

   const uint8_t device_uuid[16] = {
      0x43, 0x51, 0, 0, 7, 0, 0, 0, 0x10, 0xd5, 0x70, 0,
      0x14, 3, 0x10, 0xd5,
   };
   const uint8_t driver_uuid[16] = {4, 0, 0, 0, 1, 0, 0, 0, 3};
   if (memcmp(data + 5260, device_uuid, 16) ||
       memcmp(data + 5276, driver_uuid, 16) ||
       u32(data, 16) != 4096 || (u32(data, 20) != 28 && u32(data, 20) != 29) ||
       u32(data, 24) != 1 || u32(data, 28) != 1 ||
       u32(data, 32) != 0 || u32(data, 36) != 1 ||
       u32(data, 40) < 1 || u32(data, 40) > 2 || u32(data, 44) != 1 ||
       u32(data, 56) != 1 || u32(data, 88) != 4 ||
       u32(data, 108) != 1)
      return result::invalid;

   layout candidate = {
      u64(data, 60), u64(data, 68), u64(data, 112), u64(data, 120),
      u32(data, 92), u32(data, 96), u32(data, 76), u32(data, 128),
      u32(data, 20), u32(data, 40),
   };
   const uint64_t body_size = allocation_size - metadata_size;
   if (!candidate.width || !candidate.height || !candidate.pitch ||
       !candidate.flags_pitch || candidate.data_offset > body_size ||
       candidate.data_size > (body_size - candidate.data_offset) / candidate.layers ||
       candidate.flags_offset > body_size ||
       candidate.flags_size > (body_size - candidate.flags_offset) / candidate.layers ||
       !candidate.data_size || !candidate.flags_size ||
       candidate.data_offset % 4096 || candidate.flags_offset % 4096 ||
       candidate.data_size > UINT32_MAX || candidate.flags_size > UINT32_MAX ||
       candidate.data_offset > UINT32_MAX || candidate.flags_offset > UINT32_MAX ||
       candidate.pitch % 256 || candidate.flags_pitch % 64 ||
       candidate.data_size * candidate.layers != u64(data, 48) ||
       candidate.data_size != u64(data, 80) ||
       candidate.flags_size != u64(data, 132) ||
       u32(data, 100) < candidate.width || u32(data, 104) < candidate.height ||
       uint64_t(u32(data, 100)) * 4 != candidate.pitch ||
       uint64_t(candidate.pitch) * u32(data, 104) > candidate.data_size ||
       (candidate.data_offset < candidate.flags_offset + candidate.flags_size * candidate.layers &&
        candidate.flags_offset < candidate.data_offset + candidate.data_size * candidate.layers))
      return result::invalid;

   const uint64_t surface_size = u64(data, 8);
   if (surface_size > allocation_size || surface_size < metadata_size ||
       candidate.data_offset + candidate.data_size * candidate.layers > surface_size - metadata_size ||
       candidate.flags_offset + candidate.flags_size * candidate.layers > surface_size - metadata_size)
      return result::invalid;

   out = candidate;
   return result::valid;
}

}
