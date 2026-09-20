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
 std::printf("MAPPER5_METADATA_PASS checks=%u\n",checks);
}
