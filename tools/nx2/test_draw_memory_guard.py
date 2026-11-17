#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the production draw-memory guard with a fake mapping backend.

Checks range arithmetic and rejection behavior, not Vulkan execution or GPU-generated
indirect record contents. Requires g++; runs with ASan/UBSan (LSan disabled under ptrace).
"""
from pathlib import Path
import os
import subprocess
import tempfile

os.chdir(Path(__file__).resolve().parents[2])
s=Path('src/video_core/renderer_vulkan/vk_rasterizer.cpp').read_text()
body=s[s.index('bool RasterizerVulkan::ValidateDrawMemory'):s.index('void RasterizerVulkan::Draw(bool')]
prefix=r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>
#include <cassert>
#include <cstdio>
using u32=uint32_t; using u64=uint64_t; using GPUVAddr=u64;
#define LOG_WARNING(...) ((void)0)
struct VkDrawIndexedIndirectCommand { u32 fields[5]; };
struct VkDrawIndirectCommand { u32 fields[4]; };
struct Maxwell { enum class IndexFormat {UnsignedByte,UnsignedShort,UnsignedInt}; };
struct Memory { u64 begin=0x1000,end=0x9000,hole=0; bool IsFullyMappedRange(u64 a,u64 n) {return a>=begin && a<end && n<=end-a && (!hole || a>=hole+0x1000 || (a<=hole && n<=hole-a)); }};
struct Index { u64 start=0x1000,end=0x2000; u32 first=0,count=3; Maxwell::IndexFormat format=Maxwell::IndexFormat::UnsignedInt; u64 StartAddress() const{return start;} u64 EndAddress() const{return end;} };
struct Params {bool is_byte_count=false,is_indexed=false,include_count=false; u64 count_start_address=0x8000,indirect_start_address=0x3000; size_t buffer_size=16,max_draw_counts=1,stride=0;};
struct Engine {struct Manager {struct State {Index index_buffer; std::vector<u32> inline_index_draw_indexes;} draw_state; Params indirect_state;} draw_manager;};
struct RasterizerVulkan {Memory* gpu_memory; Engine* maxwell3d; bool ValidateDrawMemory(bool,bool);};
'''
tests=r'''
int main(){
 Memory m; Engine e; RasterizerVulkan r{&m,&e};
 auto& i=e.draw_manager.draw_state.index_buffer; auto& p=e.draw_manager.indirect_state;
 assert(r.ValidateDrawMemory(true,false));
 i.count=0xffffffff; assert(!r.ValidateDrawMemory(true,false)); i.count=3;
 i.first=0xffffffff; assert(!r.ValidateDrawMemory(true,false)); i.first=0;
 i.end=i.start-1; assert(!r.ValidateDrawMemory(true,false)); i.end=0x2000;
 i.start=0x9000; i.end=0xa000; assert(!r.ValidateDrawMemory(true,false));
 e.draw_manager.draw_state.inline_index_draw_indexes={0,1,2}; assert(r.ValidateDrawMemory(true,false));
 e.draw_manager.draw_state.inline_index_draw_indexes.clear(); i.start=0x1000; i.end=0x5000; i.count=3072; m.hole=0x2000;
 assert(!r.ValidateDrawMemory(true,false)); i.start=0x2400; i.count=3; assert(!r.ValidateDrawMemory(true,false)); m.hole=0; i.start=0x1000; i.count=3; i.end=0x2000;
 assert(r.ValidateDrawMemory(false,true));
 p.buffer_size=15; assert(!r.ValidateDrawMemory(false,true)); p.buffer_size=16;
 p.indirect_start_address=UINT64_MAX-8; assert(!r.ValidateDrawMemory(false,true)); p.indirect_start_address=0x3000;
 p.include_count=true; p.count_start_address=0x8fff; assert(!r.ValidateDrawMemory(false,true)); p.count_start_address=0x8000;
 assert(r.ValidateDrawMemory(false,true));
 p.max_draw_counts=2; p.stride=16; p.buffer_size=32; assert(r.ValidateDrawMemory(false,true));
 p.stride=12; assert(!r.ValidateDrawMemory(false,true)); p.stride=17; assert(!r.ValidateDrawMemory(false,true));
 p.max_draw_counts=UINT64_MAX; assert(!r.ValidateDrawMemory(false,true));
 p.max_draw_counts=1; p.stride=0; p.buffer_size=20; p.is_indexed=true;
 i.count=100000; assert(r.ValidateDrawMemory(true,true));
 p.buffer_size=19; assert(!r.ValidateDrawMemory(true,true));
 puts("Draw guard: mapped, holes, wraparound, inline, indirect stride/count and bounds passed");
}
'''
with tempfile.TemporaryDirectory(prefix="lichii-draw-guard-") as directory:
    source = Path(directory) / "guard.cpp"
    binary = Path(directory) / "guard"
    source.write_text(prefix + body + tests)
    subprocess.run(["g++", "-std=c++20", "-fsanitize=address,undefined",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
