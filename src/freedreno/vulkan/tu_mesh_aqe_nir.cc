#include "tu_mesh_aqe_nir.h"

#include "nir/nir_builder.h"

static nir_def *
aqe_constant(nir_builder *b, unsigned components, unsigned dword)
{
   return nir_load_const_ir3(b, components, 32, nir_imm_int(b, 0), .base = dword);
}

static nir_def *
aqe_address(nir_builder *b, unsigned dword, nir_def *offset)
{
   return nir_iadd(b, nir_pack_64_2x32(b, aqe_constant(b, 2, dword)),
                    nir_u2u64(b, offset));
}

nir_shader *
tu_aqe_build_triangle_cs(const nir_shader_compiler_options *options)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, options, "aqe_triangle");
   b.shader->info.workgroup_size[0] = 32;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   nir_def *group = nir_channel(&b, nir_load_workgroup_id(&b), 0);
   nir_def *lane = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
   nir_push_if(&b, nir_ieq_imm(&b, lane, 0));
   nir_def *counts = aqe_address(&b, 10, nir_imul_imm(&b, group, 4));
   nir_def *vertices = nir_iadd(
      &b, counts, nir_u2u64(&b, nir_imul_imm(&b, aqe_constant(&b, 1, 1), 4)));
   nir_store_global(&b, nir_imm_int(&b, 1), counts, .align_mul = 4);
   nir_store_global(&b, nir_imm_int(&b, 3), vertices, .align_mul = 4);
   nir_def *indices = nir_u2u16(&b, nir_imm_ivec4(&b, 0, 1, 2, 0));
   nir_store_global(&b, indices, aqe_address(&b, 8, nir_imul_imm(&b, group, 8)),
                    .write_mask = 0xf, .align_mul = 8);
   nir_pop_if(&b, NULL);

   nir_push_if(&b, nir_ult_imm(&b, lane, 3));
   nir_def *x = nir_bcsel(&b, nir_ieq_imm(&b, lane, 0), nir_imm_float(&b, -0.75),
                         nir_bcsel(&b, nir_ieq_imm(&b, lane, 1),
                                   nir_imm_float(&b, 0.75), nir_imm_float(&b, 0)));
   nir_def *y = nir_bcsel(&b, nir_ieq_imm(&b, lane, 2),
                         nir_imm_float(&b, 0.75), nir_imm_float(&b, -0.75));
   nir_def *position = nir_vec4(&b, x, y, nir_imm_float(&b, 0), nir_imm_float(&b, 1));
   nir_def *offset = nir_iadd(&b, nir_imul_imm(&b, group, 48),
                             nir_imul_imm(&b, lane, 16));
   nir_store_global(&b, position, aqe_address(&b, 4, offset),
                    .write_mask = 0xf, .align_mul = 16);
   nir_pop_if(&b, NULL);
   return b.shader;
}
