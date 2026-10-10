#include "tu_mesh_aqe_nir.h"
#include "tu_mesh_aqe.h"

#include "nir/nir_builder.h"

struct aqe_lowering_state {
   struct tu_aqe_layout layout;
   struct tu_aqe_vertex_io io;
};

bool
tu_aqe_fragment_supported(const nir_shader *fs)
{
   if (!fs)
      return true;
   nir_foreach_variable_with_modes(var, fs, nir_var_shader_in) {
      if (var->data.per_primitive ||
          var->data.location == VARYING_SLOT_PRIMITIVE_ID ||
          var->data.location == VARYING_SLOT_LAYER ||
          var->data.location == VARYING_SLOT_VIEWPORT ||
          var->data.location == VARYING_SLOT_CLIP_DIST0 ||
          var->data.location == VARYING_SLOT_CLIP_DIST1 ||
          var->data.location == VARYING_SLOT_CULL_DIST0 ||
          var->data.location == VARYING_SLOT_CULL_DIST1)
         return false;
   }
   return !BITSET_TEST(fs->info.system_values_read, SYSTEM_VALUE_PRIMITIVE_ID) &&
          !BITSET_TEST(fs->info.system_values_read, SYSTEM_VALUE_LAYER_ID);
}

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
lower_aqe_workgroup_id(nir_builder *b, nir_intrinsic_instr *intr, void *)
{
   if (intr->intrinsic != nir_intrinsic_load_workgroup_id)
      return false;
   b->cursor = nir_after_instr(&intr->instr);
   nir_def *linear = nir_iadd(b, nir_channel(b, &intr->def, 0),
      nir_imul(b, aqe_constant(b, 1, 0), aqe_constant(b, 1, 1)));
   nir_def *x = aqe_constant(b, 1, 15);
   nir_def *y = aqe_constant(b, 1, 16);
   nir_def *yz = nir_udiv(b, linear, x);
   nir_def *value = nir_vec3(b, nir_umod(b, linear, x), nir_umod(b, yz, y),
                               nir_udiv(b, yz, y));
   nir_def_rewrite_uses_after(&intr->def, value);
   return true;
}

static bool
lower_aqe_intrinsic(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const auto *state = (const struct aqe_lowering_state *) data;
   const struct tu_aqe_layout *layout = &state->layout;
   b->cursor = nir_before_instr(&intr->instr);
   nir_variable *var;
   nir_deref_instr *element;
   if (aqe_output_access(intr, &var, &element)) {
      if (var->data.location == VARYING_SLOT_PSIZ) {
         nir_instr_remove(&intr->instr);
         return true;
      }
      nir_def *group = nir_channel(b, nir_load_workgroup_id(b), 0);
      bool vertex = var->data.location != VARYING_SLOT_PRIMITIVE_INDICES;
      unsigned group_stride = vertex ? layout->max_vertices * layout->vertex_stride :
                                         layout->index_stride;
      unsigned element_stride = vertex ? layout->vertex_stride :
         layout->topology == TU_AQE_POINTS ? 2u : (8u - 2 * layout->topology);
      nir_def *offset = nir_iadd(b, nir_imul_imm(b, group, group_stride),
         nir_imul_imm(b, element->arr.index.ssa, element_stride));
      if (vertex)
         offset = nir_iadd_imm(b, offset, 16 * state->io.slots[var->data.location]);
      nir_def *value = intr->src[1].ssa;
      unsigned write_mask = nir_intrinsic_write_mask(intr);
      if (!vertex) {
         if (layout->topology != TU_AQE_POINTS) {
            nir_def *components[4];
            unsigned count = value->num_components;
            for (unsigned i = 0; i < count; i++)
               components[i] = nir_channel(b, value, i);
            components[count] = nir_imm_int(b, 0);
            value = nir_vec(b, components, count + 1);
            write_mask |= BITFIELD_BIT(count);
         }
         value = nir_u2u16(b, value);
      }
      nir_store_global(b, value, aqe_address(b, vertex ? 4 : 8, offset),
                       .write_mask = write_mask,
                       .align_mul = vertex ? 16u : 2u);
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
   if (intr->intrinsic == nir_intrinsic_load_draw_id) {
      nir_def_replace(&intr->def, nir_load_global(b, 1, 32,
         aqe_address(b, 2, nir_imm_int(b, 0)), .align_mul = 4));
      return true;
   }
   if (intr->intrinsic == nir_intrinsic_load_base_workgroup_id) {
      nir_def_replace(&intr->def, nir_imm_zero(b, 3, intr->def.bit_size));
      return true;
   }
   if (intr->intrinsic == nir_intrinsic_load_local_invocation_index) {
      nir_def_replace(&intr->def,
         nir_channel(b, nir_load_local_invocation_id(b), 0));
      return true;
   }
   return false;
}

static nir_shader *
build_vertex_bridge(const nir_shader *ms, const struct tu_aqe_vertex_io *io)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, ms->options,
                                                 "aqe_vertex_fetch");
   bool position = false;
   nir_foreach_variable_with_modes(var, ms, nir_var_shader_out) {
      if (var->data.location == VARYING_SLOT_PRIMITIVE_INDICES ||
          var->data.location == VARYING_SLOT_PSIZ)
         continue;
      nir_variable *out = nir_variable_clone(var, b.shader);
      out->type = glsl_get_array_element(var->type);
      nir_shader_add_variable(b.shader, out);
      nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                               glsl_uvec4_type(), "attribute");
      in->data.location = VERT_ATTRIB_GENERIC0 + io->slots[var->data.location];
      unsigned components = glsl_get_vector_elements(out->type);
      nir_store_var(&b, out, nir_trim_vector(&b, nir_load_var(&b, in), components),
                    BITFIELD_MASK(components));
      position |= var->data.location == VARYING_SLOT_POS;
   }
   if (!position) {
      nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                                glsl_vec4_type(), "position");
      out->data.location = VARYING_SLOT_POS;
      nir_store_var(&b, out, nir_imm_vec4(&b, 2, 2, 0, 1), 0xf);
   }
   if (ms->info.mesh.primitive_type == MESA_PRIM_POINTS) {
      nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                                glsl_float_type(), "point_size");
      out->data.location = VARYING_SLOT_PSIZ;
      nir_store_var(&b, out, nir_imm_float(&b, 1), 1);
   }
   return b.shader;
}

bool
tu_aqe_lower_mesh(nir_shader *ms, struct tu_aqe_vertex_io *io_out,
                  nir_shader **vs_out, nir_shader *fs)
{
   if (ms->info.stage != MESA_SHADER_MESH ||
       !ms->info.mesh.max_vertices_out || ms->info.mesh.max_vertices_out > 128 ||
       !ms->info.mesh.max_primitives_out || ms->info.mesh.max_primitives_out > 64 ||
       (ms->info.mesh.primitive_type != MESA_PRIM_TRIANGLES &&
        ms->info.mesh.primitive_type != MESA_PRIM_LINES &&
        ms->info.mesh.primitive_type != MESA_PRIM_POINTS) ||
       ms->info.workgroup_size[0] != 32 ||
       ms->info.workgroup_size[1] != 1 ||
       ms->info.workgroup_size[2] != 1 || ms->info.shared_size ||
       ms->info.num_ubos || ms->info.num_ssbos || ms->info.num_images ||
       ms->info.num_textures || !tu_aqe_fragment_supported(fs))
      return false;
   const unsigned vertices_per_primitive =
      ms->info.mesh.primitive_type == MESA_PRIM_TRIANGLES ? 3 :
      ms->info.mesh.primitive_type == MESA_PRIM_LINES ? 2 : 1;
   nir_foreach_variable_with_modes(var, ms, nir_var_shader_out) {
      bool indices = var->data.location == VARYING_SLOT_PRIMITIVE_INDICES;
      bool user = var->data.location >= VARYING_SLOT_VAR0 &&
                  var->data.location < VARYING_SLOT_MAX;
      if (!glsl_type_is_array(var->type) ||
          var->data.compact || var->data.location_frac ||
          (var->data.per_primitive && !indices) ||
          (var->data.location != VARYING_SLOT_POS &&
           var->data.location != VARYING_SLOT_PSIZ &&
           !indices && !user))
         return false;
      const glsl_type *type = glsl_get_array_element(var->type);
      if ((!glsl_type_is_scalar(type) && !glsl_type_is_vector(type)) ||
          glsl_get_bit_size(type) != 32 ||
          (!user && glsl_get_vector_elements(type) !=
             (var->data.location == VARYING_SLOT_POS ? 4 :
              var->data.location == VARYING_SLOT_PSIZ ? 1 : vertices_per_primitive)))
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
            case nir_intrinsic_load_base_workgroup_id:
            case nir_intrinsic_load_local_invocation_id:
            case nir_intrinsic_load_local_invocation_index:
            case nir_intrinsic_load_num_workgroups:
            case nir_intrinsic_load_draw_id:
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
                  if (var->data.location == VARYING_SLOT_PSIZ &&
                      (!nir_src_is_const(intr->src[1]) ||
                       nir_src_as_float(intr->src[1]) != 1.0f))
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
   if (fs) {
      nir_remove_unused_varyings(ms, fs);
      NIR_PASS(_, ms, nir_remove_dead_variables, nir_var_shader_temp, NULL);
   }
   struct aqe_lowering_state state = {};
   memset(state.io.slots, UINT8_MAX, sizeof(state.io.slots));
   state.io.slots[VARYING_SLOT_POS] = 0;
   state.io.count = 1;
   nir_foreach_variable_with_modes(var, ms, nir_var_shader_out) {
      if (var->data.location >= VARYING_SLOT_VAR0) {
         if (state.io.count == 32)
            return false;
         state.io.slots[var->data.location] = state.io.count++;
      }
   }
   if (!tu_aqe_layout_for(ms->info.mesh.max_vertices_out,
                          ms->info.mesh.max_primitives_out,
                          (enum tu_aqe_topology) (3 - vertices_per_primitive),
                          state.io.count * 16, 256, 256, &state.layout))
      return false;
   if (vs_out)
      *vs_out = build_vertex_bridge(ms, &state.io);
   if (io_out)
      *io_out = state.io;
   nir_shader_intrinsics_pass(ms, lower_aqe_workgroup_id, nir_metadata_none, NULL);
   nir_shader_intrinsics_pass(ms, lower_aqe_intrinsic, nir_metadata_none, &state);
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
tu_aqe_build_vs(const nir_shader_compiler_options *options, bool points)
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
   if (points) {
      nir_variable *size = nir_variable_create(b.shader, nir_var_shader_out,
                                                glsl_float_type(), "point_size");
      size->data.location = VARYING_SLOT_PSIZ;
      nir_store_var(&b, size, nir_imm_float(&b, 1.0f), 1);
   }
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
