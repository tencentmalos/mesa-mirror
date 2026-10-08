#include "../tu_qcom_surface.h"

#include <array>
#include <assert.h>
#include <stdio.h>

using namespace tu_qcom_surface;
using metadata = std::array<uint8_t, metadata_size>;

static void
put(metadata &data, size_t offset, uint64_t value, size_t size = 4)
{
   for (size_t i = 0; i < size; ++i)
      data[offset + i] = value >> (i * 8);
}

static metadata
fixture(unsigned layers = 1)
{
   metadata data{};
   put(data, 8, 270336 * layers + metadata_size, 8);
   for (const auto &field : std::array<std::array<uint32_t, 2>, 25>{{
           {16, 4096}, {20, 28}, {24, 1}, {28, 1}, {36, 1}, {40, layers},
           {44, 1}, {48, 266240 * layers}, {56, 1}, {68, 266240}, {76, 1280},
           {80, 266240}, {88, 4}, {92, 257}, {96, 193}, {100, 320}, {104, 208},
           {108, 1}, {112, 266240 * layers}, {120, 4096}, {128, 64}, {132, 4096},
           {5260, 0x5143}, {5264, 7}, {5268, 0x70d510},
        }})
      put(data, field[0], field[1]);
   put(data, 5272, 0xd5100314);
   put(data, 5276, 4);
   put(data, 5280, 1);
   put(data, 5284, 3);
   put(data, 5292, metadata_size);
   put(data, 5296, signature);
   return data;
}

int main()
{
   unsigned checks = 0;
   const uint64_t allocation_size = 278528;
   auto good = fixture();
   layout decoded{};
   auto check = [&](const metadata &data, result expected) {
      assert(decode(data.data(), data.size(), allocation_size, decoded) == expected);
      ++checks;
   };
   check(good, result::valid);
   assert(decoded.data_offset == 0 && decoded.flags_offset == 266240);
   assert(decoded.width == 257 && decoded.height == 193 && decoded.pitch == 1280);
   auto stereo = fixture(2);
   put(stereo, 20, 29);
   assert(decode(stereo.data(), stereo.size(), 548864, decoded) == result::valid);
   assert(decoded.layers == 2 && decoded.format == 29);
   ++checks;
   for (size_t size = 0; size < metadata_size; ++size) {
      assert(decode(good.data(), size, allocation_size, decoded) != result::valid);
      assert(decode(good.data() + metadata_size - size, size,
                    allocation_size, decoded) != result::valid);
      checks += 2;
   }
   for (size_t offset : {16, 20, 24, 28, 32, 36, 40, 44, 48, 56, 60, 68,
                        76, 80, 88, 100, 104, 108, 112, 120, 128, 132,
                        5260, 5264, 5268, 5272, 5276, 5280, 5284, 5288, 5292}) {
      auto bad = good;
      put(bad, offset, UINT32_MAX);
      check(bad, result::invalid);
   }
   for (size_t offset : {8, 60, 68, 80, 112, 120, 132}) {
      auto bad = good;
      put(bad, offset, UINT64_MAX, 8);
      check(bad, result::invalid);
   }
   for (size_t offset : {8, 16, 24, 28, 36, 40, 44, 48, 56, 68, 76,
                        80, 88, 92, 96, 100, 104, 108, 112, 120, 128, 132}) {
      auto bad = good;
      put(bad, offset, 0);
      check(bad, result::invalid);
   }
   for (uint64_t size : {0ull, 5299ull, 5300ull, 275635ull}) {
      assert(decode(good.data(), good.size(), size, decoded) == result::invalid);
      ++checks;
   }
   auto absent = good;
   put(absent, 5296, 0);
   check(absent, result::absent);
   printf("%u QCOM surface metadata checks passed\n", checks);
}
