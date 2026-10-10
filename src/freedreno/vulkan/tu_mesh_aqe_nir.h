#ifndef TU_MESH_AQE_NIR_H
#define TU_MESH_AQE_NIR_H

#include "nir/nir.h"

nir_shader *tu_aqe_build_triangle_cs(const nir_shader_compiler_options *options);
bool tu_aqe_lower_mesh(nir_shader *ms);
nir_shader *tu_aqe_build_vs(const nir_shader_compiler_options *options, bool points = false);

#endif
