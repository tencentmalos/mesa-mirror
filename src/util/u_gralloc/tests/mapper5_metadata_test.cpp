/* SPDX-License-Identifier: MIT */
#include "../u_gralloc_mapper5_metadata.h"
#include <vector>
#include <cstdio>
#include <string>
using namespace mapper5_metadata;
std::vector<uint8_t> bytes;
template<class T> void put(T v) { auto p=reinterpret_cast<uint8_t*>(&v); bytes.insert(bytes.end(),p,p+sizeof(v)); }
void text(const char*s){put<int64_t>(strlen(s));bytes.insert(bytes.end(),s,s+strlen(s));}
void header(int64_t key){bytes.clear();text("android.hardware.graphics.common.StandardMetadataType");put(key);}
void layout(){header(15);put<int64_t>(1);put<int64_t>(1);text("android.hardware.graphics.common.PlaneLayoutComponentType");put<int64_t>(1024);put<int64_t>(0);put<int64_t>(8);for(int64_t v:{0,32,7680,1920,1080,8294400,1,1})put(v);}
void ubwc_layout(){
 header(15);put<int64_t>(2);
 for(int i=0;i<2;++i){
  put<int64_t>(i?5:4);
  for(int c=0;c<4;++c){
   text("android.hardware.graphics.common.PlaneLayoutComponentType");
   put<int64_t>(c==3?1073741824:1024<<c);put<int64_t>(i?0:c*8);put<int64_t>(i?0:8);
  }
  if(i){text("QTI");put<int64_t>(INT32_MIN);put<int64_t>(0);put<int64_t>(0);}
  if(i){for(int64_t v:{0,0,128,1920,1080,36864,0,0})put(v);}
  else {for(int64_t v:{36864,32,7680,1920,1080,8355840,1,1})put(v);}
 }
}
#define CHECK(x) do { ++checks; if(!(x)){std::printf("FAIL %d %s\n",__LINE__,#x);return 1;} }while(0)
int main(){
 unsigned checks=0; uint64_t val=9;
 header(8);put<uint64_t>(0);
 CHECK(scalar(bytes.data(),bytes.size(),8,val) && val==0);
 CHECK(!scalar(bytes.data(),bytes.size(),7,val));
 for(size_t i=0;i<bytes.size();i++) CHECK(!scalar(bytes.data(),i,8,val));
 bytes.push_back(0);CHECK(!scalar(bytes.data(),bytes.size(),8,val));
 bytes[8]^=1;CHECK(!scalar(bytes.data(),bytes.size()-1,8,val));
 Planes p{};layout();
 CHECK(planes(bytes.data(),bytes.size(),8298496,p));
 CHECK(p.count==1 && p.offsets[0]==0 && p.strides[0]==7680 && p.sizes[0]==8294400);
 for(size_t i=0;i<bytes.size();i++) CHECK(!planes(bytes.data(),i,8298496,p));
 CHECK(!planes(bytes.data(),bytes.size(),8294399,p));
 bytes.push_back(0);CHECK(!planes(bytes.data(),bytes.size(),8298496,p));
 layout();int64_t n=5;memcpy(bytes.data()+69,&n,8);CHECK(!planes(bytes.data(),bytes.size(),8298496,p));
 layout();n=-1;memcpy(bytes.data()+77,&n,8);CHECK(!planes(bytes.data(),bytes.size(),8298496,p));
 layout();n=INT64_MAX;memcpy(bytes.data()+85,&n,8);CHECK(!planes(bytes.data(),bytes.size(),8298496,p));
 layout();n=0;memcpy(bytes.data()+bytes.size()-48,&n,8);CHECK(!planes(bytes.data(),bytes.size(),8298496,p));
 layout();n=-1;memcpy(bytes.data()+bytes.size()-64,&n,8);CHECK(!planes(bytes.data(),bytes.size(),8298496,p));
 ubwc_layout();
 CHECK(bytes.size()==968);
 CHECK(planes(bytes.data(),bytes.size(),8396800,p));
 CHECK(p.count==2 && !p.qti_metadata[0] && p.qti_metadata[1]);
 auto saved=p;
 CHECK(qti_rgb32_ubwc(p));
 CHECK(p.count==1 && p.offsets[0]==0 && p.strides[0]==7680 && p.sizes[0]==8392704);
 for(size_t i=0;i<bytes.size();++i) CHECK(!planes(bytes.data(),i,8396800,p));
 CHECK(!planes(bytes.data(),bytes.size(),8392703,p));
 for(int change=0;change<9;++change){
  p=saved;
  switch(change){
   case 0:p.offsets[0]+=4096;break;
   case 1:p.offsets[1]=4096;break;
   case 2:p.strides[1]=64;break;
   case 3:p.sizes[1]+=4096;break;
   case 4:p.sizes[0]-=4096;break;
   case 5:p.widths[1]=1919;break;
   case 6:p.qti_metadata[1]=false;break;
   case 7:p.increments[0]=16;break;
   case 8:p.strides[0]=7679;break;
  }
  CHECK(!qti_rgb32_ubwc(p));
 }
 ubwc_layout();bytes[std::string(reinterpret_cast<const char *>(bytes.data()),bytes.size()).find("QTI")]='X';
 CHECK(!planes(bytes.data(),bytes.size(),8396800,p));
 header(12);text("QTI");put<int64_t>(10);int64_t compression=0;
 CHECK(extendable(bytes.data(),bytes.size(),12,"QTI",compression) && compression==10);
 CHECK(!extendable(bytes.data(),bytes.size(),12,"unknown",compression));
 for(size_t i=0;i<bytes.size();++i)CHECK(!extendable(bytes.data(),i,12,"QTI",compression));
 std::printf("MAPPER5_METADATA_PASS checks=%u\n",checks);
}
