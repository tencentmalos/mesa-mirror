#include <initializer_list>

#include "tu_mesh.h"
#include "nir/nir_builder.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"

static void
check(bool condition)
{
   if (!condition)
      abort();
}

static void
test_lowering(const nir_shader_compiler_options *options, bool task,
              bool queries, unsigned x, unsigned y, unsigned z)
{
   nir_builder b = nir_builder_init_simple_shader(
      task ? MESA_SHADER_TASK : MESA_SHADER_MESH, options, "mesh_query_test");
   b.shader->info.workgroup_size[0] = x;
   b.shader->info.workgroup_size[1] = y;
   b.shader->info.workgroup_size[2] = z;
   if (task) {
      nir_launch_mesh_workgroups(&b, nir_imm_ivec3(&b, 0, 0, 0));
      tu_mesh_state state = {};
      tu_mesh_lower_ts(b.shader, &state, queries);
   } else {
      b.shader->info.mesh.max_vertices_out = 3;
      b.shader->info.mesh.max_primitives_out = 1;
      b.shader->info.mesh.primitive_type = MESA_PRIM_TRIANGLES;
      nir_set_vertex_and_primitive_count(&b, nir_imm_int(&b, 0),
                                         nir_imm_int(&b, 0), nir_imm_int(&b, 0));
      tu_mesh_io io;
      tu_mesh_gather_io(b.shader, &io);
      tu_mesh_lower_ms(b.shader, &io, 0, queries);
   }
   nir_validate_shader(b.shader, "mesh invocation queries");
   unsigned atomics = 0;
   unsigned primitive_atomics = 0;
   nir_foreach_block (block, nir_shader_get_entrypoint(b.shader)) {
      nir_foreach_instr (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_global_atomic)
            continue;
         check(nir_intrinsic_atomic_op(intr) == nir_atomic_op_iadd);
         check(intr->def.bit_size == 64);
         nir_def *address = intr->src[0].ssa;
         if (nir_def_instr(address)->type == nir_instr_type_alu) {
            nir_alu_instr *add = nir_instr_as_alu(nir_def_instr(address));
            if (add->op == nir_op_iadd && nir_src_is_const(add->src[1].src) &&
                nir_src_as_uint(add->src[1].src) == TU_MESH_QUERY_PRIMITIVES * 8) {
               check(!task);
               primitive_atomics++;
               continue;
            }
         }
         check(nir_src_as_uint(intr->src[1]) == x * y * z);
         check(nir_cf_node_is_last(&block->cf_node));
         nir_if *condition = nir_cf_node_as_if(block->cf_node.parent);
         if (nir_src_is_const(condition->condition)) {
            check(x * y * z == 1 && nir_src_as_bool(condition->condition));
         } else {
            nir_alu_instr *compare = nir_instr_as_alu(nir_def_instr(condition->condition.ssa));
            check(compare->op == nir_op_ieq);
            nir_intrinsic_instr *invocation = nir_def_as_intrinsic(compare->src[0].src.ssa);
            check(invocation->intrinsic == nir_intrinsic_load_local_invocation_index);
            check(nir_src_as_uint(compare->src[1].src) == 0);
         }
         if (!task) {
            nir_alu_instr *add = nir_instr_as_alu(nir_def_instr(address));
            check(add->op == nir_op_iadd);
            check(nir_src_as_uint(add->src[1].src) == TU_MESH_QUERY_MESH_INVOCATIONS * 8);
            address = add->src[0].src.ssa;
         }
         check(nir_def_as_intrinsic(address)->intrinsic == nir_intrinsic_load_global);
         atomics++;
      }
   }
   check(atomics == unsigned(queries));
   check(primitive_atomics == unsigned(queries && !task));
   ralloc_free(b.shader);
}

static void
test_atomic_compile(ir3_compiler *compiler)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, ir3_get_compiler_options(compiler), "mesh_query_atomic64");
   b.shader->info.workgroup_size[0] = 40;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   nir_push_if(&b, nir_ieq_imm(&b, nir_load_local_invocation_index(&b), 0));
   nir_def *address = nir_load_global(&b, 1, 64, nir_imm_int64(&b, 0x100000030),
                                     .align_mul = 8);
   nir_global_atomic(&b, 64, nir_iadd_imm(&b, address, 8), nir_imm_int64(&b, 40),
                     .atomic_op = nir_atomic_op_iadd);
   nir_pop_if(&b, NULL);
   nir_validate_shader(b.shader, "atomic64 query compiler input");
   ir3_shader_options options = {};
   ir3_shader_key key = {};
   NIR_PASS(_, b.shader, nir_lower_compute_system_values, NULL);
   ir3_finalize_nir(compiler, &options.nir_options, b.shader);
   ir3_shader *shader = ir3_shader_from_nir(compiler, b.shader, &options);
   ir3_shader_variant *variant = ir3_shader_create_variant(shader, &key, true);
   check(variant != NULL);
   check(variant->disasm_info.disasm != NULL);
   check(strstr(variant->disasm_info.disasm,
                "atomic.g.cmpxchg.untyped.1d.u64.1.g") != NULL);
   puts(variant->disasm_info.disasm);
   check(variant->info.size > 0);
   ralloc_free(variant);
   ir3_shader_destroy(shader);
}

int
main()
{
   glsl_type_singleton_init_or_ref();
   fd_dev_id id = { .chip_id = 0x44050a31 };
   ir3_compiler_options options = { .disable_cache = true };
   ir3_compiler *compiler = ir3_compiler_create(NULL, &id, fd_dev_info_raw(&id), &options);
   check(compiler != NULL);
   const unsigned sizes[][3] = {{1, 1, 1}, {10, 4, 1}, {1, 4, 6}, {128, 1, 1}};
   for (bool task : {false, true}) {
      for (bool queries : {false, true}) {
         for (auto &size : sizes)
            test_lowering(ir3_get_compiler_options(compiler), task, queries,
                          size[0], size[1], size[2]);
      }
   }
   test_atomic_compile(compiler);
   ir3_compiler_destroy(compiler);
   glsl_type_singleton_decref();
   puts("mesh_query_test: 16 lowering cases and A840 atomic64 compilation passed");
   return 0;
}
