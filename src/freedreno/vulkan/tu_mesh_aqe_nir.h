#ifndef TU_MESH_AQE_NIR_H
#define TU_MESH_AQE_NIR_H

#include "nir/nir.h"

nir_shader *tu_aqe_build_triangle_cs(const nir_shader_compiler_options *options);

#endif
