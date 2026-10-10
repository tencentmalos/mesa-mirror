#ifndef TU_MESH_AQE_STATE_H
#define TU_MESH_AQE_STATE_H

#include "tu_mesh_aqe.h"

struct ir3_shader_variant;

struct tu_aqe_stage {
   uint32_t words[64];
   uint32_t dwords;
   uint32_t ndrange;
};

bool tu_aqe_build_triangle_stage(const struct ir3_shader_variant *variant,
                                  const struct tu_aqe_bo *binary,
                                  struct tu_aqe_stage *stage);

#endif
