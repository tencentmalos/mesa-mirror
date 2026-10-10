#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "tu_mesh_aqe.h"
#include "tu_mesh_aqe_nir.h"
#include "tu_mesh_aqe_state.h"
#include "tu_queue_scope.h"
#include "freedreno_pm4.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"
#include "nir/nir_builder.h"

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
   CHECK(!tu_aqe_layout_for(0, 1, TU_AQE_TRIANGLES, 16, 256, 256, &l));
   CHECK(!tu_aqe_layout_for(129, 1, TU_AQE_TRIANGLES, 16, 256, 256, &l));
   CHECK(!tu_aqe_layout_for(128, 65, TU_AQE_TRIANGLES, 16, 256, 256, &l));
   CHECK(!tu_aqe_layout_for(128, 64, TU_AQE_TRIANGLES, UINT32_MAX - 15, 256, 256, &l));
   CHECK(!tu_aqe_layout_for(128, 64, TU_AQE_TRIANGLES, 17, 256, 256, &l));
   CHECK(!tu_aqe_layout_for(3, 1, (tu_aqe_topology)3, 16, 256, 256, &l));
   for (auto topology : {TU_AQE_TRIANGLES, TU_AQE_LINES, TU_AQE_POINTS}) {
      for (unsigned v : {1u, 3u, 4u, 64u, 128u}) {
         for (unsigned p : {1u, 2u, 64u}) {
            CHECK(tu_aqe_layout_for(v, p, topology, 16, 256, 256, &l));
            CHECK(l.regions[TU_AQE_OUTPUT0].size == 2 * 256 * v * 16);
            unsigned elements = topology == TU_AQE_POINTS ? 1 : 4u - topology;
            CHECK(l.index_stride == ((p * elements + 3) & ~3u) * 2);
         }
      }
   }
   CHECK(!tu_aqe_layout_for(3, 1, TU_AQE_TRIANGLES, 16, 0, 256, &l));
   CHECK(!tu_aqe_layout_for(3, 1, TU_AQE_TRIANGLES, 16, 257, 256, &l));
   CHECK(!tu_aqe_layout_for(3, 1, TU_AQE_TRIANGLES, 16, UINT32_MAX, 256, &l));
   CHECK(!tu_aqe_layout_for(3, 1, TU_AQE_TRIANGLES, 16, 256, UINT32_MAX, &l));
   for (unsigned k = 1; k <= 256; k++) {
      CHECK(tu_aqe_layout_for(3, 1, TU_AQE_TRIANGLES, 16, k, 256, &l));
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
   CHECK(tu_aqe_layout_for(3, 1, TU_AQE_TRIANGLES, 16, 256, 256, &layout));
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
   uint32_t direct[TU_AQE_PACKET_DWORDS];
   memcpy(direct, p, sizeof(p));
   d.indirect = {0x345600000000ull, 12};
   CHECK(tu_aqe_build_triangle(&d, &layout, h, p));
   CHECK(p[0] == pm4_pkt7_hdr(0x7a, TU_AQE_INDIRECT_PACKET_DWORDS - 1));
   CHECK(p[1] == 7 && address(p + 2) == d.indirect.iova);
   CHECK(!memcmp(p + 4, direct + 5, (TU_AQE_PACKET_DWORDS - 5) * 4));
   CHECK(p[TU_AQE_INDIRECT_PACKET_DWORDS] == 0);
   d.indirect = {};
   CHECK(tu_aqe_build_triangle(&d, &layout, h, p));
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
   d.groups[0] = 65536; reject();
   d.groups[0] = UINT32_MAX; reject();
   d.indirect = {0x345600000001ull, 12}; reject();
   d.indirect = {(1ull << 49) - 4, 12}; reject();
   d.indirect = {0, 12}; reject();
   d.indirect = {0x345600000000ull, 11}; reject();
   d.metadata = {0, sizeof(tu_aqe_draw_metadata)}; reject();
   d.metadata = {0x345600000001ull, sizeof(tu_aqe_draw_metadata)}; reject();
   d.metadata = {0x345600000000ull, sizeof(tu_aqe_draw_metadata) - 1}; reject();
   d.metadata = {good.arena.iova, sizeof(tu_aqe_draw_metadata)}; reject();
   d.metadata = {(1ull << 49) - 16, sizeof(tu_aqe_draw_metadata)}; reject();
   d.metadata = {0x345600000000ull, sizeof(tu_aqe_draw_metadata)};
   CHECK(tu_aqe_build_triangle(&d, &layout, h, p));
   CHECK(address(p + 26) == d.metadata.iova && p[28] == sizeof(tu_aqe_draw_metadata));
   d.indirect = {0x456700000000ull, 12};
   CHECK(tu_aqe_build_triangle(&d, &layout, h, p));
   CHECK(address(p + 25) == d.metadata.iova && p[27] == sizeof(tu_aqe_draw_metadata));
   d = good;
   const uint32_t dimensions[][3] = {
      {255, 1, 1}, {256, 1, 1}, {257, 1, 1}, {17, 17, 3},
      {65535, 3, 1}, {16384, 256, 1},
   };
   for (const auto &groups : dimensions) {
      memcpy(d.groups, groups, sizeof(groups));
      CHECK(tu_aqe_build_triangle(&d, &layout, h, p));
      CHECK(!memcmp(p + 2, groups, sizeof(groups)));
   }
   d.groups[2] = 2; reject();
   d.groups[0] = d.groups[1] = d.groups[2] = 65535; reject();
   layout.regions[TU_AQE_COUNTS].offset += 32; reject();
}

static void
test_stage(const ir3_shader_variant *v, const char *directory)
{
   tu_aqe_bo binary = {0x345600000000ull, v->info.size};
   tu_aqe_stage stage;
   CHECK(tu_aqe_build_triangle_stage(v, &binary, &stage));
   CHECK(stage.dwords == 39 && stage.ndrange == 0x7d);
   CHECK(stage.words[0] == pm4_pkt7_hdr(CP_CONTEXT_REG_BUNCH, 36));
   CHECK(stage.words[37] == 0x48a9d401 && stage.words[38] == 0x7d);
   auto reg = [&](unsigned address) {
      unsigned hits = 0;
      uint32_t result = 0;
      for (unsigned i = 1; i < 37; i += 2) {
         if (stage.words[i] == address) { hits++; result = stage.words[i + 1]; }
      }
      CHECK(hits == 1);
      return result;
   };
   CHECK((uint64_t(reg(0xa9b5)) << 32 | reg(0xa9b4)) == binary.iova);
   CHECK(reg(0xa9bc) * 128 <= binary.size);
   CHECK(((reg(0xa9cd) & 0xff) * 4) == v->constlen);
   CHECK(reg(0xa9cd) & 0x100);
   CHECK((reg(0xa9c2) & 0xff) == ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID));
   CHECK((reg(0xa9c2) >> 24) == ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID));
   CHECK(((reg(0xa9b0) >> 7) & 0x3f) == v->info.max_reg + 1);
   CHECK(reg(0xa9b6) == 0 && reg(0xa9b7) == 0 && reg(0xa9b8) == 0);
   CHECK(reg(0xa9b9) == 0 && reg(0xa9bd) == 0);

   tu_aqe_layout layout;
   CHECK(tu_aqe_layout_for(3, 1, TU_AQE_TRIANGLES, 16, 256, 256, &layout));
   tu_aqe_triangle_draw draw = {
      .arena = {0x123400000000ull, layout.size},
      .parameters = {0x234500000000ull, 4 * (TU_AQE_HEADER_DWORDS + stage.dwords)},
      .state_offset = 104, .state_dwords = stage.dwords, .groups = {1, 1, 1},
   };
   uint32_t blob[TU_AQE_HEADER_DWORDS + 64] = {}, packet[TU_AQE_PACKET_DWORDS];
   CHECK(tu_aqe_build_triangle(&draw, &layout, blob, packet));
   CHECK(blob[19] == stage.ndrange && blob[6] == stage.dwords);
   memcpy(blob + TU_AQE_HEADER_DWORDS, stage.words, stage.dwords * 4);
   if (directory) {
      char path[4096];
      snprintf(path, sizeof(path), "%s/triangle.stage", directory);
      FILE *f = fopen(path, "wb");
      CHECK(f && fwrite(stage.words, stage.dwords * 4, 1, f) == 1);
      CHECK(fclose(f) == 0);
      snprintf(path, sizeof(path), "%s/triangle.parameters", directory);
      f = fopen(path, "wb");
      CHECK(f && fwrite(blob, draw.parameters.size, 1, f) == 1);
      CHECK(fclose(f) == 0);
   }

   auto variant = *v;
   const auto good_binary = binary;
   auto reject = [&]() {
      memset(&stage, 0xa5, sizeof(stage));
      CHECK(!tu_aqe_build_triangle_stage(&variant, &binary, &stage));
      const auto *bytes = reinterpret_cast<const unsigned char *>(&stage);
      for (unsigned i = 0; i < sizeof(stage); i++) CHECK(bytes[i] == 0xa5);
      variant = *v;
      binary = good_binary;
   };
   binary.iova += 4; reject();
   binary.size--; reject();
   binary.iova = (1ull << 49) - 128; reject();
   binary.iova = 0; reject();
   variant.local_size[0] = 64; reject();
   variant.local_size_variable = true; reject();
   variant.type = MESA_SHADER_VERTEX; reject();
   variant.pvtmem_size = 4; reject();
   variant.shared_size = 4; reject();
   variant.bindless_ubo = true; reject();
   variant.constlen = 4; reject();
   variant.constlen = 17; reject();
   variant.constlen = 132; reject();
   variant.need_driver_params = true; reject();
   variant.constant_data_size = 4; reject();
   variant.num_samp = 1; reject();
   variant.instrlen = UINT32_MAX; reject();
   variant.info.max_reg = 63; reject();
   variant.info.max_half_reg = 63; reject();
}

static void
test_lowering(ir3_compiler *compiler)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_MESH, ir3_get_compiler_options(compiler), "application_mesh");
   b.shader->info.mesh.max_vertices_out = 3;
   b.shader->info.mesh.max_primitives_out = 1;
   b.shader->info.mesh.primitive_type = MESA_PRIM_TRIANGLES;
   b.shader->info.workgroup_size[0] = 32;
   b.shader->info.workgroup_size[1] = b.shader->info.workgroup_size[2] = 1;
   nir_variable *pos = nir_variable_create(b.shader, nir_var_shader_out,
      glsl_array_type(glsl_vec4_type(), 3, 0), "positions");
   pos->data.location = VARYING_SLOT_POS;
   nir_variable *idx = nir_variable_create(b.shader, nir_var_shader_out,
      glsl_array_type(glsl_vector_type(GLSL_TYPE_UINT, 3), 1, 0), "indices");
   idx->data.location = VARYING_SLOT_PRIMITIVE_INDICES;
   nir_set_vertex_and_primitive_count(&b, nir_imm_int(&b, 3), nir_imm_int(&b, 1),
                                      nir_imm_int(&b, 1));
   nir_def *lane = nir_load_local_invocation_index(&b);
   nir_variable *group_var = nir_variable_create(b.shader, nir_var_system_value,
      glsl_vector_type(GLSL_TYPE_UINT, 3), "gl_WorkGroupID");
   group_var->data.location = SYSTEM_VALUE_WORKGROUP_ID;
   nir_def *group = nir_load_var(&b, group_var);
   nir_def *dimensions = nir_load_num_workgroups(&b);
   nir_push_if(&b, nir_ult_imm(&b, lane, 3));
   nir_store_array_var(&b, pos, lane,
      nir_vec4(&b, nir_u2f32(&b, nir_channel(&b, group, 0)),
               nir_u2f32(&b, nir_channel(&b, group, 1)),
               nir_u2f32(&b, nir_channel(&b, group, 2)),
               nir_u2f32(&b, nir_iadd(&b, nir_channel(&b, dimensions, 2),
                                     nir_load_draw_id(&b)))), 0xf);
   nir_pop_if(&b, NULL);
   nir_push_if(&b, nir_ieq_imm(&b, lane, 0));
   nir_store_array_var_imm(&b, idx, 0, nir_imm_ivec3(&b, 2, 0, 1), 7);
   nir_pop_if(&b, NULL);

   NIR_PASS(_, b.shader, nir_lower_system_values);
   for (unsigned location : {VARYING_SLOT_PRIMITIVE_ID, VARYING_SLOT_LAYER,
                             VARYING_SLOT_VIEWPORT, VARYING_SLOT_CLIP_DIST0,
                             VARYING_SLOT_CULL_DIST0, VARYING_SLOT_VAR0}) {
      nir_builder fb = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
         ir3_get_compiler_options(compiler), "rejected_fragment");
      nir_variable *in = nir_variable_create(fb.shader, nir_var_shader_in,
                                               glsl_int_type(), "primitive_input");
      in->data.location = location;
      in->data.per_primitive = location == VARYING_SLOT_VAR0;
      CHECK(!tu_aqe_fragment_supported(fb.shader));
      ralloc_free(fb.shader);
   }
   for (unsigned location : {VARYING_SLOT_PRIMITIVE_ID, VARYING_SLOT_LAYER,
                             VARYING_SLOT_VIEWPORT, VARYING_SLOT_CLIP_DIST0,
                             VARYING_SLOT_CULL_DIST0, VARYING_SLOT_VAR0}) {
      nir_shader *rejected = nir_shader_clone(NULL, b.shader);
      nir_builder rb = nir_builder_at(nir_after_cf_list(&nir_shader_get_entrypoint(rejected)->body));
      const bool distance = location == VARYING_SLOT_CLIP_DIST0 ||
                            location == VARYING_SLOT_CULL_DIST0;
      const glsl_type *type = distance ? glsl_float_type() : glsl_int_type();
      nir_variable *out = nir_variable_create(rejected, nir_var_shader_out,
         glsl_array_type(type, distance ? 3 : 1, 0), "rejected_output");
      out->data.location = location;
      out->data.per_primitive = !distance;
      nir_store_array_var_imm(&rb, out, 0,
                             distance ? nir_imm_float(&rb, 1.0f) : nir_imm_int(&rb, 1), 1);
      CHECK(!tu_aqe_lower_mesh(rejected));
      CHECK(rejected->info.stage == MESA_SHADER_MESH);
      ralloc_free(rejected);
   }
   b.shader->info.mesh.max_vertices_out = 129;
   CHECK(!tu_aqe_lower_mesh(b.shader));
   CHECK(b.shader->info.stage == MESA_SHADER_MESH);
   b.shader->info.mesh.max_vertices_out = 3;
   b.shader->info.mesh.max_primitives_out = 65;
   CHECK(!tu_aqe_lower_mesh(b.shader));
   b.shader->info.mesh.max_primitives_out = 1;
   b.shader->info.mesh.primitive_type = MESA_PRIM_TRIANGLE_STRIP;
   CHECK(!tu_aqe_lower_mesh(b.shader));
   b.shader->info.mesh.primitive_type = MESA_PRIM_TRIANGLES;
   b.shader->info.shared_size = 4;
   CHECK(!tu_aqe_lower_mesh(b.shader));
   b.shader->info.shared_size = 0;
   b.shader->info.num_ssbos = 1;
   CHECK(!tu_aqe_lower_mesh(b.shader));
   b.shader->info.num_ssbos = 0;
   nir_shader *bad_point_size = nir_shader_clone(NULL, b.shader);
   nir_builder bad = nir_builder_at(nir_after_cf_list(&nir_shader_get_entrypoint(bad_point_size)->body));
   nir_variable *ps = nir_variable_create(bad_point_size, nir_var_shader_out,
      glsl_array_type(glsl_float_type(), 3, 0), "point_size");
   ps->data.location = VARYING_SLOT_PSIZ;
   nir_store_array_var_imm(&bad, ps, 0, nir_imm_float(&bad, 2.0f), 1);
   CHECK(!tu_aqe_lower_mesh(bad_point_size));
   ralloc_free(bad_point_size);
   pos->data.location = VARYING_SLOT_TEX0;
   CHECK(!tu_aqe_lower_mesh(b.shader));
   pos->data.location = VARYING_SLOT_POS;
   nir_shader *varying = nir_shader_clone(NULL, b.shader);
   nir_builder vb = nir_builder_at(nir_after_cf_list(&nir_shader_get_entrypoint(varying)->body));
   nir_builder fb = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
      ir3_get_compiler_options(compiler), "varying_fragment");
   const glsl_type *types[] = {glsl_vec4_type(), glsl_vec2_type(),
                               glsl_int_type(), glsl_uint_type(), glsl_float_type()};
   for (unsigned i = 0; i < ARRAY_SIZE(types); i++) {
      nir_variable *out = nir_variable_create(varying, nir_var_shader_out,
         glsl_array_type(types[i], 3, 0), "user_output");
      out->data.location = VARYING_SLOT_VAR0 + i;
      if (i == 2 || i == 3)
         out->data.interpolation = INTERP_MODE_FLAT;
      unsigned components = glsl_get_vector_elements(types[i]);
      nir_store_array_var_imm(&vb, out, 0, nir_imm_zero(&vb, components, 32),
                              BITFIELD_MASK(components));
      if (i < 4) {
         nir_variable *in = nir_variable_create(fb.shader, nir_var_shader_in,
                                                   types[i], "user_input");
         in->data.location = VARYING_SLOT_VAR0 + i;
         in->data.interpolation = out->data.interpolation;
      }
   }
   struct tu_aqe_vertex_io io;
   nir_shader *bridge = NULL;
   CHECK(tu_aqe_lower_mesh(varying, &io, &bridge, fb.shader));
   CHECK(io.count == 5 && io.slots[VARYING_SLOT_VAR0 + 4] == UINT8_MAX);
   nir_validate_shader(varying, "AQE varying writes");
   nir_validate_shader(bridge, "AQE typed vertex bridge");
   unsigned bridge_inputs = 0, flat_outputs = 0;
   nir_foreach_variable_with_modes(var, bridge, nir_var_shader_in)
      bridge_inputs++;
   nir_foreach_variable_with_modes(var, bridge, nir_var_shader_out)
      flat_outputs += var->data.interpolation == INTERP_MODE_FLAT;
   CHECK(bridge_inputs == 5 && flat_outputs == 2);
   ralloc_free(varying);
   ralloc_free(bridge);
   ralloc_free(fb.shader);
   CHECK(tu_aqe_lower_mesh(b.shader));
   nir_validate_shader(b.shader, "lowered application AQE mesh");
   CHECK(b.shader->info.stage == MESA_SHADER_COMPUTE);
   unsigned stores = 0, index_stores = 0;
   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            auto *intr = nir_instr_as_intrinsic(instr);
            CHECK(intr->intrinsic != nir_intrinsic_set_vertex_and_primitive_count);
            CHECK(intr->intrinsic != nir_intrinsic_store_deref);
            CHECK(intr->intrinsic != nir_intrinsic_load_base_workgroup_id);
            CHECK(intr->intrinsic != nir_intrinsic_load_draw_id);
            if (intr->intrinsic == nir_intrinsic_store_global) {
               stores++;
               index_stores += intr->src[0].ssa->bit_size == 16;
            }
         }
      }
   }
   CHECK(stores == 4 && index_stores == 1);
   ir3_shader_options options = {};
   options.const_allocs.max_const_offset_vec4 = TU_AQE_CONSTANT_VEC4S;
   ir3_finalize_nir(compiler, &options.nir_options, b.shader);
   auto *shader = ir3_shader_from_nir(compiler, b.shader, &options);
   ir3_shader_key key = {};
   auto *v = ir3_shader_create_variant(shader, &key, true);
   CHECK(v && !v->need_driver_params);
   tu_aqe_stage stage;
   tu_aqe_bo binary = { 0x123400000000ull, v->info.size };
   CHECK(tu_aqe_build_triangle_stage(v, &binary, &stage));
   ralloc_free(v);
   ir3_shader_destroy(shader);
   nir_shader *vs = tu_aqe_build_vs(ir3_get_compiler_options(compiler));
   nir_validate_shader(vs, "AQE hardware vertex fetch");
   ralloc_free(vs);
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
   test_lowering(compiler);
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
   test_stage(v, directory);
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

static void
test_queue_barrier_scope()
{
   glsl_type_singleton_init_or_ref();
   const nir_shader_compiler_options options = {};
   for (bool multiple_queues : {true, false}) {
      for (mesa_scope scope : {SCOPE_DEVICE, SCOPE_QUEUE_FAMILY, SCOPE_WORKGROUP}) {
         nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
                                                       &options, "queue_scope");
         nir_variable *var = nir_variable_create(b.shader, nir_var_mem_global,
                                                glsl_uint_type(), "counter");
         nir_deref_instr *deref = nir_build_deref_var(&b, var);
         nir_def *one = nir_imm_int(&b, 1);
         for (unsigned i = 0; i < 2; i++) {
            nir_barrier(&b, SCOPE_NONE, scope, NIR_MEMORY_RELEASE, nir_var_mem_global);
            nir_intrinsic_instr *atomic = nir_intrinsic_instr_create(
               b.shader, nir_intrinsic_deref_atomic);
            atomic->num_components = 1;
            atomic->src[0] = nir_src_for_ssa(&deref->def);
            atomic->src[1] = nir_src_for_ssa(one);
            nir_intrinsic_set_atomic_op(atomic, nir_atomic_op_iadd);
            nir_def_init(&atomic->instr, &atomic->def, 1, 32);
            nir_builder_instr_insert(&b, &atomic->instr);
            nir_barrier(&b, SCOPE_NONE, scope, NIR_MEMORY_ACQUIRE, nir_var_mem_global);
         }
         const bool optimized = nir_opt_acquire_release_barriers(
            b.shader, tu_acquire_release_max_scope(multiple_queues));
         const bool expected = scope == SCOPE_WORKGROUP ||
                               (!multiple_queues && scope == SCOPE_QUEUE_FAMILY);
         CHECK(optimized == expected);
         unsigned barriers = 0;
         nir_foreach_block(block, b.impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type == nir_instr_type_intrinsic &&
                   nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_barrier)
                  barriers++;
            }
         }
         CHECK(barriers == (expected ? 2u : 4u));
         ralloc_free(b.shader);
      }
   }
   glsl_type_singleton_decref();
}

int
main(int argc, char **argv)
{
   const char *directory = argc == 2 ? argv[1] : NULL;
   test_layout();
   test_packet(directory);
   test_queue_barrier_scope();
   test_shader(directory);
   puts("AQE layout, relocation, rejection and A830 shader compilation passed");
}
