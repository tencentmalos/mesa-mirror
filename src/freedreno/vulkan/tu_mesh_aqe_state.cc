#include "tu_mesh_aqe_state.h"

#include "ir3/ir3_shader.h"
#include "fd6_hw.h"
#include "freedreno_pm4.h"

#include <string.h>

bool
tu_aqe_build_triangle_stage(const struct ir3_shader_variant *v,
                            const struct tu_aqe_bo *binary,
                            struct tu_aqe_stage *stage)
{
   if (!v || !binary || !stage || !v->compiler || !v->compiler->dev_id ||
       v->compiler->dev_id->chip_id != 0x44050000 ||
       v->type != MESA_SHADER_COMPUTE || !v->bin || !v->info.size ||
       !v->instrlen || uint64_t(v->instrlen) * 128 > v->info.size ||
       v->info.max_reg < -1 || v->info.max_reg >= 63 ||
       v->info.max_half_reg < -1 || v->info.max_half_reg >= 63 ||
       ir3_shader_branchstack_hw(v) > 127 ||
       v->local_size_variable || v->local_size[0] != 32 ||
       v->local_size[1] != 1 || v->local_size[2] != 1 ||
       v->pvtmem_size || v->shared_size || v->num_uavs || v->num_samp ||
       v->bindless_tex || v->bindless_samp || v->bindless_ibo || v->bindless_ubo ||
       v->constant_data_size || v->need_driver_params ||
       !v->compiler->info->props.load_shader_consts_via_preamble ||
       v->constlen < TU_AQE_CONSTANT_VEC4S || v->constlen > 128 ||
       v->constlen % 4 || !binary->iova || binary->iova % 128 ||
       binary->iova >= (1ull << 49) ||
       binary->size > (1ull << 49) - binary->iova ||
       binary->size < v->info.size)
      return false;

   const struct ir3_const_state *state = ir3_const_state(v);
   if (!state || state->num_driver_params || state->driver_params_ubo.size ||
       state->ubo_state.num_enabled || state->push_consts_type != IR3_PUSH_CONSTS_NONE ||
       state->allocs.max_const_offset_vec4 < TU_AQE_CONSTANT_VEC4S)
      return false;
   for (const auto &allocation : state->allocs.consts) {
      if (allocation.size_vec4 && allocation.offset_vec4 < TU_AQE_CONSTANT_VEC4S)
         return false;
   }
   for (unsigned i = 0; i < v->inputs_count; i++) {
      if (!v->inputs[i].sysval ||
          (v->inputs[i].slot != SYSTEM_VALUE_WORKGROUP_ID &&
           v->inputs[i].slot != SYSTEM_VALUE_LOCAL_INVOCATION_ID))
         return false;
   }

   tu_aqe_stage result = {};
   unsigned n = 1;
   auto reg = [&](uint32_t address, uint32_t value) {
      result.words[n++] = address;
      result.words[n++] = value;
   };
   enum a6xx_threadsize threads = v->info.double_threadsize ? THREAD128 : THREAD64;
   uint32_t control = A6XX_SP_CS_CNTL_0_HALFREGFOOTPRINT(v->info.max_half_reg + 1) |
                      A6XX_SP_CS_CNTL_0_FULLREGFOOTPRINT(v->info.max_reg + 1) |
                      A6XX_SP_CS_CNTL_0_BRANCHSTACK(ir3_shader_branchstack_hw(v)) |
                      A6XX_SP_CS_CNTL_0_THREADSIZE(threads);
   if (v->cs.round_robin_mode) control |= A6XX_SP_CS_CNTL_0_COMPUTERRMODEEN;
   if (v->early_preamble) control |= A6XX_SP_CS_CNTL_0_EARLYPREAMBLE;
   if (v->mergedregs) control |= A6XX_SP_CS_CNTL_0_MERGEDREGS;
   reg(REG_A7XX_SP_CS_VGS_CNTL, 0);
   reg(REG_A6XX_SP_CS_PROGRAM_COUNTER_OFFSET, 0);
   reg(REG_A6XX_SP_CS_BASE, binary->iova);
   reg(REG_A6XX_SP_CS_BASE + 1, binary->iova >> 32);
   reg(REG_A6XX_SP_CS_INSTR_SIZE, v->instrlen);
   reg(REG_A6XX_SP_CS_CNTL_0, control);
   reg(REG_A6XX_SP_CS_CNTL_1, A6XX_SP_CS_CNTL_1_SHARED_SIZE(1));
   reg(REG_A7XX_SP_CS_WGE_CNTL,
       A7XX_SP_CS_WGE_CNTL_LINEARLOCALIDREGID(regid(63, 0)) |
       A7XX_SP_CS_WGE_CNTL_THREADSIZE(threads) |
       A7XX_SP_CS_WGE_CNTL_WORKGROUPRASTORDERZFIRSTEN |
       A7XX_SP_CS_WGE_CNTL_WGTILEWIDTH(4) |
       A7XX_SP_CS_WGE_CNTL_WGTILEHEIGHT(17));
   reg(REG_A6XX_SP_CS_WIE_CNTL_0,
       A6XX_SP_CS_WIE_CNTL_0_WGIDCONSTID(ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID)) |
       A6XX_SP_CS_WIE_CNTL_0_WGSIZECONSTID(regid(63, 0)) |
       A6XX_SP_CS_WIE_CNTL_0_WGOFFSETCONSTID(regid(63, 0)) |
       A6XX_SP_CS_WIE_CNTL_0_LOCALIDREGID(ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID)));
   reg(REG_A7XX_SP_CS_WIE_CNTL_1,
       A7XX_SP_CS_WIE_CNTL_1_LINEARLOCALIDREGID(regid(63, 0)) |
       A7XX_SP_CS_WIE_CNTL_1_THREADSIZE(threads) |
       A7XX_SP_CS_WIE_CNTL_1_WORKITEMRASTORDER(v->cs.force_linear_dispatch ?
          WORKITEMRASTORDER_LINEAR : WORKITEMRASTORDER_TILED));
   reg(REG_A6XX_SP_CS_PVT_MEM_PARAM, 0);
   reg(REG_A6XX_SP_CS_PVT_MEM_BASE, 0);
   reg(REG_A6XX_SP_CS_PVT_MEM_BASE + 1, 0);
   reg(REG_A6XX_SP_CS_PVT_MEM_SIZE, 0);
   reg(REG_A6XX_SP_CS_PVT_MEM_STACK_OFFSET, 0);
   reg(REG_A6XX_SP_CS_CONFIG, A6XX_SP_CS_CONFIG_ENABLED);
   reg(REG_A7XX_SP_CS_CONST_CONFIG,
       A7XX_SP_CS_CONST_CONFIG_CONSTLEN(v->constlen) | A7XX_SP_CS_CONST_CONFIG_ENABLED);
   reg(REG_A8XX_SP_CS_HYSTERESIS, 0);
   result.words[0] = pm4_pkt7_hdr(CP_CONTEXT_REG_BUNCH, n - 1);
   result.ndrange = A7XX_SP_CS_NDRANGE_0_KERNELDIM(1) |
                    A7XX_SP_CS_NDRANGE_0_LOCALSIZEX(31);
   result.words[n++] = pm4_pkt4_hdr(REG_A7XX_SP_CS_NDRANGE_0, 1);
   result.words[n++] = result.ndrange;
   result.dwords = n;
   *stage = result;
   return true;
}
