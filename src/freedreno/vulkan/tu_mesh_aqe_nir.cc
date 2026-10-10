#include "tu_mesh_aqe_nir.h"

#include "nir/nir_builder.h"

static bool
aqe_output_access(nir_intrinsic_instr *intr, nir_variable **var,
                  nir_deref_instr **element)
{
   if (intr->intrinsic != nir_intrinsic_store_deref)
      return false;
   *element = nir_src_as_deref(intr->src[0]);
   *var = nir_deref_instr_get_variable(*element);
   return *var && (*var)->data.mode == nir_var_shader_out &&
          (*element)->deref_type == nir_deref_type_array &&
          nir_deref_instr_parent(*element)->deref_type == nir_deref_type_var;
}

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

static bool
lower_aqe_intrinsic(nir_builder *b, nir_intrinsic_instr *intr, void *)
{
   b->cursor = nir_before_instr(&intr->instr);
   nir_variable *var;
   nir_deref_instr *element;
   if (aqe_output_access(intr, &var, &element)) {
      nir_def *group = nir_channel(b, nir_load_workgroup_id(b), 0);
      bool position = var->data.location == VARYING_SLOT_POS;
      nir_def *offset = nir_iadd(b, nir_imul_imm(b, group, position ? 48 : 8),
         nir_imul_imm(b, element->arr.index.ssa, position ? 16 : 8));
      nir_def *value = intr->src[1].ssa;
      if (!position)
         value = nir_u2u16(b, value);
      nir_store_global(b, value, aqe_address(b, position ? 4 : 8, offset),
                       .write_mask = nir_intrinsic_write_mask(intr),
                       .align_mul = position ? 16u : 8u);
      nir_instr_remove(&intr->instr);
      return true;
   }
   if (intr->intrinsic == nir_intrinsic_set_vertex_and_primitive_count) {
      nir_push_if(b, nir_ieq_imm(b,
         nir_channel(b, nir_load_local_invocation_id(b), 0), 0));
      nir_def *group = nir_channel(b, nir_load_workgroup_id(b), 0);
      nir_def *counts = aqe_address(b, 10, nir_imul_imm(b, group, 4));
      nir_def *vertices = nir_iadd(b, counts,
         nir_u2u64(b, nir_imul_imm(b, aqe_constant(b, 1, 1), 4)));
      nir_store_global(b, intr->src[1].ssa, counts, .align_mul = 4);
      nir_store_global(b, intr->src[0].ssa, vertices, .align_mul = 4);
      nir_pop_if(b, NULL);
      nir_instr_remove(&intr->instr);
      return true;
   }
   if (intr->intrinsic == nir_intrinsic_load_num_workgroups) {
      nir_def *value = nir_vec3(b, aqe_constant(b, 1, 15),
         aqe_constant(b, 1, 16), aqe_constant(b, 1, 17));
      nir_def_replace(&intr->def, value);
      return true;
   }
   if (intr->intrinsic == nir_intrinsic_load_local_invocation_index) {
      nir_def_replace(&intr->def,
         nir_channel(b, nir_load_local_invocation_id(b), 0));
      return true;
   }
   return false;
}

bool
tu_aqe_lower_mesh(nir_shader *ms)
{
   if (ms->info.stage != MESA_SHADER_MESH ||
       ms->info.mesh.max_vertices_out != 3 ||
       ms->info.mesh.max_primitives_out != 1 ||
       ms->info.mesh.primitive_type != MESA_PRIM_TRIANGLES ||
       ms->info.workgroup_size[0] != 32 ||
       ms->info.workgroup_size[1] != 1 ||
       ms->info.workgroup_size[2] != 1 || ms->info.shared_size ||
       ms->info.num_ubos || ms->info.num_ssbos || ms->info.num_images ||
       ms->info.num_textures)
      return false;
   nir_foreach_variable_with_modes(var, ms, nir_var_shader_out) {
      if (!glsl_type_is_array(var->type) ||
          var->data.compact || var->data.location_frac ||
          (var->data.location != VARYING_SLOT_POS &&
           var->data.location != VARYING_SLOT_PRIMITIVE_INDICES))
         return false;
      const glsl_type *type = glsl_get_array_element(var->type);
      if (glsl_get_bit_size(type) != 32 ||
          glsl_get_vector_elements(type) !=
             (var->data.location == VARYING_SLOT_POS ? 4 : 3))
         return false;
   }
   nir_foreach_function_impl(impl, ms) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex)
               return false;
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_load_deref:
            case nir_intrinsic_store_deref:
            case nir_intrinsic_copy_deref:
            case nir_intrinsic_load_workgroup_id:
            case nir_intrinsic_load_local_invocation_id:
            case nir_intrinsic_load_local_invocation_index:
            case nir_intrinsic_load_num_workgroups:
            case nir_intrinsic_load_workgroup_size:
            case nir_intrinsic_set_vertex_and_primitive_count:
            case nir_intrinsic_barrier:
               break;
            default:
               return false;
            }
            if (intr->intrinsic == nir_intrinsic_store_deref ||
                intr->intrinsic == nir_intrinsic_load_deref ||
                intr->intrinsic == nir_intrinsic_copy_deref) {
               nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
               if (deref->modes & ~(nir_var_shader_out | nir_var_function_temp |
                                    nir_var_shader_temp))
                  return false;
               if (deref->modes & nir_var_shader_out) {
                  nir_variable *var;
                  if (!aqe_output_access(intr, &var, &deref))
                     return false;
               }
               if (intr->intrinsic == nir_intrinsic_copy_deref &&
                   (nir_src_as_deref(intr->src[1])->modes &
                    ~(nir_var_function_temp | nir_var_shader_temp)))
                  return false;
            }
         }
      }
   }
   nir_shader_intrinsics_pass(ms, lower_aqe_intrinsic, nir_metadata_none, NULL);
   NIR_PASS(_, ms, nir_remove_dead_derefs);
   NIR_PASS(_, ms, nir_remove_dead_variables, nir_var_shader_out, NULL);
   ms->info.stage = MESA_SHADER_COMPUTE;
   ms->info.next_stage = MESA_SHADER_NONE;
   memset(&ms->info.cs, 0, sizeof(ms->info.cs));
   ms->info.inputs_read = 0;
   ms->info.outputs_written = 0;
   ms->info.outputs_read = 0;
   ms->info.per_primitive_outputs = 0;
   return true;
}

nir_shader *
tu_aqe_build_vs(const nir_shader_compiler_options *options)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options,
                                                 "aqe_vertex_fetch");
   nir_variable *input = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "position");
   input->data.location = VERT_ATTRIB_GENERIC0;
   nir_variable *output = nir_variable_create(b.shader, nir_var_shader_out,
                                              glsl_vec4_type(), "position");
   output->data.location = VARYING_SLOT_POS;
   nir_store_var(&b, output, nir_load_var(&b, input), 0xf);
   return b.shader;
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
