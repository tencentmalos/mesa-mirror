#ifndef TU_MESH_AQE_NIR_H
#define TU_MESH_AQE_NIR_H

#include "nir/nir.h"

nir_shader *tu_aqe_build_triangle_cs(const nir_shader_compiler_options *options);
struct tu_aqe_vertex_io {
   uint8_t slots[VARYING_SLOT_MAX];
   uint8_t count;
};
bool tu_aqe_fragment_supported(const nir_shader *fs);
bool tu_aqe_lower_mesh(nir_shader *ms, struct tu_aqe_vertex_io *io_out = nullptr,
                       nir_shader **vs_out = nullptr, nir_shader *fs = nullptr);
nir_shader *tu_aqe_build_vs(const nir_shader_compiler_options *options, bool points = false);

#endif
