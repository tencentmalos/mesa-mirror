import os
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'tu_image.cc'
text = source.read_text(encoding='utf-8')
begin = text.index('void\ntu_fragment_density_map_sample(')
end = text.index('\nVKAPI_ATTR', begin)
prelude = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CLAMP(x,a,b) std::clamp<int32_t>(x,a,b)
constexpr unsigned TILE6_LINEAR=0, MIN_FDM_TEXEL_SIZE_LOG2=5, MAX_FDM_TEXEL_SIZE_LOG2=10;
struct tu_frag_area { float width, height; };
struct tu_image { struct { unsigned tile_mode, cpp; } layout[1]; void *map; };
struct tu_image_view {
   tu_image *image;
   struct { struct { unsigned width, height; } extent; unsigned layer_count; } vk;
   struct { unsigned pitch, offset, layer_size, format; } view;
   unsigned swizzle[4];
};
static unsigned util_logbase2_ceil(unsigned n) {
   unsigned s=0; while ((1u << s) < n) ++s; return s;
}
static const void *expected_pixel;
static unsigned reads;
static void util_format_unpack_rgba(unsigned, float *out, const void *pixel, unsigned n) {
   if (pixel != expected_pixel || n != 1) {
      fprintf(stderr,"FDM sampled outside the expected view layer\n");
      std::exit(2);
   }
   ++reads;
   const auto *p = static_cast<const uint8_t *>(pixel);
   out[0]=p[0]/255.0f; out[1]=p[1]/255.0f; out[2]=0; out[3]=1;
}
static void pipe_swizzle_4f(float *out, const float *in, const unsigned *) {
   memcpy(out,in,4*sizeof(float));
}
'''
test = r'''
int main() {
   uint8_t bytes[8192]; memset(bytes,255,sizeof(bytes));
   tu_image image{{{TILE6_LINEAR,2}}, bytes};
   tu_image_view view{&image, {{8,8},1}, {64,128,4096,0}, {}};
   tu_frag_area area{};
   expected_pixel = bytes+128+2*3+64*5;
   for (unsigned layer : {0u,1u,7u}) {
      tu_fragment_density_map_sample(&view,nullptr,96,160,256,256,layer,&area);
      assert(area.width==1 && area.height==1);
   }
   view.vk.layer_count=2;
   for (unsigned layer : {0u,1u}) {
      expected_pixel=bytes+128+4096*layer+2*3+64*5;
      tu_fragment_density_map_sample(&view,nullptr,96,160,256,256,layer,&area);
      assert(area.width==1 && area.height==1);
   }
   view.vk.layer_count=1;
   expected_pixel=bytes+128;
   tu_fragment_density_map_sample(&view,nullptr,-4,-4,256,256,1,&area);
   for (unsigned layer : {0u,1u,7u}) {
      expected_pixel=bytes+16*5+2*3;
      tu_fragment_density_map_sample(&view,bytes,96,160,256,256,layer,&area);
      assert(area.width==1 && area.height==1);
   }
   view.vk.layer_count=2;
   for (unsigned layer : {0u,1u}) {
      expected_pixel=bytes+16*(8*layer+5)+2*3;
      tu_fragment_density_map_sample(&view,bytes,96,160,256,256,layer,&area);
      assert(area.width==1 && area.height==1);
   }
   assert(reads==11);
   puts("11 production FDM sampling checks passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'test.cpp'
    cpp.write_text(prelude + text[begin:end] + test, encoding='utf-8', newline='\n')
    binary = Path(directory) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1',
                    '-fsanitize=undefined', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
