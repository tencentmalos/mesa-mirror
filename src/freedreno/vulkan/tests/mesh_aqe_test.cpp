#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "tu_mesh_aqe.h"
#include "tu_mesh_aqe_nir.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); abort(); } } while (0)

static uint64_t
address(const uint32_t *p)
{
   return p[0] | uint64_t(p[1]) << 32;
}

static void
test_layout()
{
   tu_aqe_layout l;
   CHECK(!tu_aqe_triangle_layout(0, 256, &l));
   CHECK(!tu_aqe_triangle_layout(257, 256, &l));
   CHECK(!tu_aqe_triangle_layout(UINT32_MAX, 256, &l));
   CHECK(!tu_aqe_triangle_layout(256, UINT32_MAX, &l));
   for (unsigned k = 1; k <= 256; k++) {
      CHECK(tu_aqe_triangle_layout(k, 256, &l));
      uint64_t end = 0;
      for (const auto &r : l.regions) {
         CHECK(r.offset == end);
         CHECK(!(r.offset % 32) && !(r.size % 32));
         end += r.size;
      }
      CHECK(end == l.size);
      CHECK(l.regions[TU_AQE_COUNTS].size / 2 >= 8 * k);
      CHECK(l.regions[TU_AQE_INDICES].size / 2 >= 8 * k);
      CHECK(l.regions[TU_AQE_OUTPUT0].size / 2 == 48 * k);
   }
   CHECK(l.regions[TU_AQE_INDICES].size == 0x180000);
   CHECK(l.regions[TU_AQE_AUXILIARY].offset == 0x180000);
   CHECK(l.regions[TU_AQE_COUNTS].offset == 0x184000);
   CHECK(l.regions[TU_AQE_TASK_PAYLOAD].offset == 0x185000);
   CHECK(l.regions[TU_AQE_TASK_RECORDS].offset == 0x985000);
   CHECK(l.regions[TU_AQE_OUTPUT0].offset == 0x988000);
   CHECK(l.size == 0x98e000);
}

static void
test_packet(const char *directory)
{
   tu_aqe_layout layout;
   CHECK(tu_aqe_triangle_layout(256, 256, &layout));
   tu_aqe_triangle_draw d = {
      .arena = {0x123400000000ull, layout.size},
      .parameters = {0x234500000000ull, 4096},
      .state_offset = 104,
      .state_dwords = 37,
      .groups = {1, 1, 1},
   };
   uint32_t h[TU_AQE_HEADER_DWORDS], p[TU_AQE_PACKET_DWORDS];
   CHECK(tu_aqe_build_triangle(&d, &layout, h, p));
   uint32_t expected[TU_AQE_HEADER_DWORDS] = {};
   expected[4] = 104;
   expected[5] = 0x2345;
   expected[6] = 37;
   expected[7] = 4;
   expected[9] = 48;
   expected[15] = 3;
   expected[16] = 1;
   expected[17] = 1;
   expected[19] = 0x7d;
   CHECK(!memcmp(h, expected, sizeof(h)));
   CHECK(p[0] == 0x707a001f && p[1] == 5);
   CHECK(p[7] == 0x40 && p[8] == 0 && p[9] == 256 && p[10] == 256);
   CHECK(address(p + 5) == d.parameters.iova);
   const unsigned offsets[] = {0x184000, 0, 0x988000, 0x98e000,
                               0x180000, 0x185000, 0x985000};
   const unsigned sizes[] = {0x1000, 0x180000, 0x6000, 0, 0x4000, 0x800000, 0x3000};
   for (unsigned i = 0; i < 7; i++) {
      CHECK(address(p + 11 + 3 * i) == d.arena.iova + offsets[i]);
      CHECK(p[13 + 3 * i] == sizes[i]);
   }
   if (directory) {
      char path[4096];
      snprintf(path, sizeof(path), "%s/triangle.packet", directory);
      FILE *f = fopen(path, "wb");
      CHECK(f && fwrite(p, sizeof(p), 1, f) == 1);
      CHECK(fclose(f) == 0);
   }
   auto good = d;
   auto reject = [&]() {
      memset(h, 0xaa, sizeof(h));
      memset(p, 0xaa, sizeof(p));
      CHECK(!tu_aqe_build_triangle(&d, &layout, h, p));
      for (auto word : h) CHECK(word == 0xaaaaaaaa);
      for (auto word : p) CHECK(word == 0xaaaaaaaa);
      d = good;
   };
   d.arena.iova++; reject();
   d.arena.size--; reject();
   d.arena.iova = (1ull << 49) - 32; reject();
   d.arena.iova = 0; reject();
   d.parameters.iova = good.arena.iova; reject();
   d.parameters.size = 251; reject();
   d.parameters.size = UINT64_MAX; reject();
   d.state_offset = 100; reject();
   d.state_offset = 105; reject();
   d.state_dwords = 1u << 20; reject();
   d.state_dwords = 0; reject();
   d.groups[0] = 0; reject();
   d.groups[0] = 257; reject();
   d.groups[0] = UINT32_MAX; reject();
   d.groups[0] = 16; d.groups[1] = 16;
   CHECK(tu_aqe_build_triangle(&d, &layout, h, p));
   d.groups[2] = 2; reject();
   layout.regions[TU_AQE_COUNTS].offset += 32; reject();
}

static void
test_shader(const char *directory)
{
   glsl_type_singleton_init_or_ref();
   fd_dev_id id = {.chip_id = 0x44050000};
   ir3_compiler_options compiler_options = {.disable_cache = true};
   ir3_compiler *compiler = ir3_compiler_create(
      NULL, &id, fd_dev_info_raw(&id), &compiler_options);
   CHECK(compiler);
   nir_shader *nir = tu_aqe_build_triangle_cs(ir3_get_compiler_options(compiler));
   nir_validate_shader(nir, "AQE triangle");
   ir3_shader_options options = {};
   options.const_allocs.max_const_offset_vec4 = TU_AQE_CONSTANT_VEC4S;
   ir3_finalize_nir(compiler, &options.nir_options, nir);
   ir3_shader *shader = ir3_shader_from_nir(compiler, nir, &options);
   ir3_shader_key key = {};
   ir3_shader_variant *v = ir3_shader_create_variant(shader, &key, true);
   CHECK(v && v->info.size && v->disasm_info.disasm);
   puts(v->disasm_info.disasm);
   fflush(stdout);
   CHECK(v->constlen >= TU_AQE_CONSTANT_VEC4S);
   CHECK(ir3_const_state(v)->num_driver_params == 0);
   for (const auto &allocation : ir3_const_state(v)->allocs.consts)
      CHECK(!allocation.size_vec4 || allocation.offset_vec4 >= TU_AQE_CONSTANT_VEC4S);
   CHECK(ir3_const_state(v)->allocs.max_const_offset_vec4 >= TU_AQE_CONSTANT_VEC4S);
   CHECK(strstr(v->disasm_info.disasm, "c1.x"));
   CHECK(strstr(v->disasm_info.disasm, "c2.x"));
   CHECK(strstr(v->disasm_info.disasm, "c2.z"));
   CHECK(strstr(v->disasm_info.disasm, "stg"));
   CHECK(strstr(v->disasm_info.disasm, "stg.u16"));
   if (directory) {
      char path[4096];
      snprintf(path, sizeof(path), "%s/triangle.cs", directory);
      FILE *f = fopen(path, "wb");
      CHECK(f && fwrite(v->bin, v->info.size, 1, f) == 1);
      CHECK(fclose(f) == 0);
   }
   ralloc_free(v);
   ir3_shader_destroy(shader);
   ir3_compiler_destroy(compiler);
   glsl_type_singleton_decref();
}

int
main(int argc, char **argv)
{
   const char *directory = argc == 2 ? argv[1] : NULL;
   test_layout();
   test_packet(directory);
   test_shader(directory);
   puts("AQE layout, relocation, rejection and A830 shader compilation passed");
}
