# Mithril-Wrapper

在 iOS 上为 Minecraft Java（LWJGL 3）提供 OpenGL 3.3 Core 实现的自研渲染库。
GL → Vulkan → Metal（MoltenVK），无 OpenGL ES 参与。

## 状态

- **M0 基建已交付**：CMake 双分支工程、EGL 44 符号 + 契约冒烟、GL 342 符号导出、CI build.yml、契约文档。
- **M1 状态引擎完成**：`src/state/`（全局 Context、错误 FIFO、capability 表）；`src/gl/state.cpp`（S1 组 48 函数真实现：glClear/glViewport/glEnable/glGetString「3.3 Core Profile」/glGetError 等）；生成脚本 `scripts/gen_gl_stubs.py` 支持实现排除名单重新生成 stub。
- **M2 完成：着色器管线 + Vulkan 后端接通**：
  - `src/shader/`：glslang GLSL→SPIR-V + SPIRV-Cross 反射（编译缓存、松散 uniform 折入合成 UBO、GLSL 150 自动升级 330；按域拆分 `glsl.cpp`/`reflect.cpp`/`registry.cpp`）；`src/gl/shader.cpp` S2 组约 60 函数真实现（shader 生命周期、link/use、glUniform* 全系、getter）；shader_smoke 通过。
  - `src/vk/`：dlsym 动态加载 Vulkan loader/ICD（libvulkan → MoltenVK）；instance 级函数经真实 instance 句柄解析（全局 GIPA 只保证全局函数）；UBO 反射 VS+FS 双阶段合并 → 动态 UBO 池；staging 顶点缓冲；renderpass + 清屏 + 帧读回。
  - `tests/draw_smoke.c` 全链通过（llvmpipe）：GL 层着色 + glDrawArrays → Vulkan 绘制 → glReadPixels 校验（白三角形、tint 驱动变色、背景色）。
- **M3 完成：顶点数据**：S3 组 76/114 真实现（顶点属性全家族：pointer/IPointer/常量 1-4 系/Divisor、buffer 映射与查询家族、10 个 draw 入口：DrawArrays(Instanced)/DrawElements(Instanced/BaseVertex/Range 双变体)/MultiDraw 全系）；引擎新增双顶点流（顶点+实例）、索引缓冲（统一 UINT32 staging）、TriangleStrip/Fan 拓扑；实例化采用 CPU 逐实例打包（divisor 行复制）；非 4 字节对齐 stride/offset 由 CPU 规整为 float32 打包；`draw_smoke` 扩展 8 个 M3 断言全部通过。
- **M4 纹理完成（S4 42/42 函数）**：`src/vk/texture.cpp` 上传路径扩展（staging→CmdCopyBufferToImage 全 mip 逐切片、image/view/sampler 覆盖 2D/1D、3D volume、2D/1D array、cubemap、wrap_r、白 dummy 兜底）；`src/gl/texture.cpp` 全量真实现（TexImage1D/2D/3D + TexSub 全系含 cubemap face/array 分层、GetTexImage PACK 回读、GenerateMipmap 逐切片滤波、TexParameter/GetTexParameter/GetTexLevelParameter 全系、S3TC DXT1/3/5 CPU 解压 + GetCompressedTexImage、CopyTexImage/CopyTexSubImage 帧读回、glTexBuffer、glPixelStoref）；`texture_smoke` 26 断言全通过（llvmpipe：红纹理采样/mip/dummy 白/GetTexImage 往返/3D 切片/数组分层/cubemap 6 面/拷贝/texBuffer/压缩）。
- **M5 完成（S5 FBO 全量 24 + MRT + MSAA）**：
  - stage A+B 状态管线：`src/vk/pipeline.cpp` 深度附件 D24S8、`PipelineState` 烘焙进 pipeline 缓存 key、显式清除、动态 scissor Y 翻转、depth/blend/cull/frontFace/stencil 域、colorMask/polygon；GL 侧 `BuildPipelineState` 快照接入。
  - stage C S5 FBO/渲染缓冲：`src/vk/fbo.cpp`（renderbuffer 表 CreateRbImage/Rb view、FBO 表 SetFramebuffer + 懒重建 Vk framebuffer/renderpass，`ResolveDrawFbo` 脏检测 + 纹理重传跟随、`BlitFramebuffer`）；`draw.cpp` SubmitFlush 按 target（默认或 FBO）清屏-渲染-回读，readback buffer 按目标尺寸重建，read/draw 分离绑定；`src/gl/fbo.cpp` 对象表 + 24 函数真实现。
  - **MRT**：`src/gl/fbo.cpp` color[8] 多附件槽 + `glDrawBuffers`/`glDrawBuffer`/`glReadBuffer` 真实现；Vk 层 renderpass N 附件 + 附件 N 视图、pipeline `attachmentCount=附件数` 每附件独立 blend（draw_mask 未选型号清写掩码）、显式 clear 逐附件（仅 draw buffer 选中）、读回按 read_buf 挑附件。
  - **MSAA**：renderbuffer `samples>1` → `rasterizationSamples` + resolve 附件（单采样转储）、clear 写颜色与 resolve、读回 resolve 图；`ToVkSampleCount` 映射 1/2/4/8/16/32/64。
  - `tests/fbo_smoke.c` 29 行 ok（28 断言）：17 状态断言 + S5 FBO 纹理/RBO/blit + MRT 双附件读回与单 drawBuffer 门控 + MSAA 4x resolve 回读；八冒烟（contract/state/shader/draw/texture/fbo/3d/render3d）回归全过。
- **M6 stage B 完成：swapchain / present（CAMetalLayer → Metal）**：
  - `src/vk/swapchain.cpp`：`SetNativeLayer`（objc 校验 CAMetalLayer）→ 懒建 `VK_EXT_metal_surface` + `VK_KHR_swapchain`；`Present()` 工作线程/同步：acquire（image-available 信号量）→ 离屏 target blit 到 swapchain 图 → render-finished 信号量 → QueuePresent，fence 同步保单一多帧槽安全；OUT_OF_DATE 自重建；present 能力/队列校验；`mithril_has_swapchain` ABI 探针。
  - `src/vk/dispatch.cpp`：Apple 下启用 surface/metal_surface/portability_enumeration 实例扩展（含 `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`）+ swapchain 设备扩展，加载 WSI 入口；macOS loader 候选加 `libvulkan.dylib`。
  - `src/egl/exports.cpp`：window surface 透传 CAMetalLayer、`eglSwapBuffers`→`Present()`、`eglSwapInterval`→vsync、`eglQuerySurface`→present 尺寸。
  - macOS CI：Homebrew `vulkan-loader`+`molten-vk`，`tests/swapchain_smoke.c` 用 objc 建真 CAMetalLayer 走全链路并断言 `mithril_has_swapchain`。
  - Linux（无 Metal）：全部保守降级为 offscreen no-op，既有冒烟不受影响。
- **M6 stage C 完成（S6 GLsync 同步对象）**：
  - `src/gl/sync.cpp`：`glFenceSync`/`glDeleteSync`/`glIsSync`/`glClientWaitSync`/`glWaitSync`/`glGetSynciv` 六函数真实现（存活表 + 互斥锁，condition/flags 校验，降级语义）。
  - `src/vk/draw.cpp` `CreateGLSync`：每条 GLsync 分配专用空命令缓冲 + 独立 VkFence 一起提交（按序位于先前全部工作之后 → `GL_SYNC_GPU_COMMANDS_COMPLETE`）。必须用真实命令缓冲：0 命令缓冲的空批在 MoltenVK 永不触发 fence（lavapipe 会，曾掩盖该问题）。
  - `tests/sync_smoke.c` 全过（lavapipe + macOS MoltenVK 真机 CI 均绿）：引擎启动前 fence、绘制后等待满足/读回顺序、零超时轮询、waitSync 校验、错误路径；读回容差接受 0.2/0.3/0.9 两方向舍入（lavapipe/MoltenVK 相反）。
- **M6 stage D 完成（S6 Query 对象 + primitive restart + provoking vertex）**：
  - `src/vk/query.cpp`：QueryObj 包装每帧 VkQueryPool 槽（occ/ts 各 4096）；遮挡查询在 Draw() 内给范围内每条 draw 记槽（CmdBeginQuery/End 括住），RetireFrame 栅栏后 `RetireFrameQueries` 汇总；SAMPLES_PASSED 依赖 occlusionQueryPrecise（无则降级读 0），ANY_SAMPLES_PASSED 不依赖；TIME_ELAPSED = begin/end 双时间戳差值 × timestampPeriod；GL_TIMESTAMP 单写 × period；阻塞读走 SubmitFlush(true)。
  - `src/gl/query.cpp`：`glGenQueries`/`glDeleteQueries`/`glIsQuery`/`glBeginQuery`/`glEndQuery`/`glQueryCounter`/`glGetQueryiv`/`glGetQueryObject{iv,uiv,i64v,ui64v}` 13 函数（每 target 至多一活动 query、错误路径全套、ANY 结果归一化 0/1）。
  - `glPrimitiveRestartIndex`：GL 层改写 restart index→0xFFFFFFFF，DrawCommon v_count 跳过标记，pipeline `ia.primitiveRestartEnable` + key `|PR`。
  - `glProvokingVertex`：入 PipelineState key `|PV`，设备声明 VK_EXT_provoking_vertex 时启用 provokingVertexLast（LAST 与 GL 默认一致，无扩展时保持隐式行为）。
  - `tests/query_smoke.c` 60 断言全过（lavapipe：SAMPLES_PASSED=253888、深度遮挡 0/前置 1、TIME_ELAPSED/GL_TIMESTAMP 非零、restart 两独立三角 + 无 restart fan 覆盖、错误路径全套）。
- **M6 stage E 完成（S6 Sampler 对象）**：
  - `src/gl/sampler.cpp`：`glGenSamplers`/`glDeleteSamplers`/`glIsSampler`/`glBindSampler` + `glSamplerParameter{f,fv,i,iv,Iiv,Iuiv}` 6 函数 + `glGetSamplerParameter{fv,iv,Iiv,Iuiv}` 4 函数（共 14 函数；对象表 + 错误路径全套、参数往返）。
  - 采样器状态与纹理解耦：GL sampler 对象经引擎常驻 `VkSampler` 表自拥采样器；引擎描述符绑定由 `(binding, tex_id)` 升级为 `(binding, sampler_id, tex_id)`（`src/vk/engine.h` 新增 `SamplerBind`）；绑定 sampler 对象时配对该 `VkSampler` 与纹理 image view，未绑定（`sampler_id==0`）时回退纹理自带 sampler（`glTexParameteri` 烘焙）。
  - 引擎新增 API：`UpdateSampler(uint64_t, const TexSamplerInfo&)`/`DestroyResidentSampler(uint64_t)`/`GetResidentSampler(uint64_t)`。
  - `tests/sampler_smoke.c` 全过（lavapipe：生命周期、bind 错误路径、参数往返、getter/setter 错误路径、绑定 sampler 对象渲染（NEAREST）vs 纹理回退、NEAREST/LINEAR 可观测差异、delete 解绑）。

## 快速构建（Linux 开发循环）

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
# 产物: output/libmithril.so
```

## 冒烟测试

```sh
gcc -o tests/contract_smoke tests/contract_smoke.c -ldl
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/contract_smoke   # EGL 契约
gcc -o tests/state_smoke tests/state_smoke.c -ldl
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/state_smoke      # GL 状态机
gcc -o tests/shader_smoke tests/shader_smoke.c -ldl
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/shader_smoke     # 着色器管线
gcc -o tests/draw_smoke tests/draw_smoke.c -ldl
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/draw_smoke       # GL→Vulkan 全链三角（需 lavapipe/loader）
gcc -o tests/texture_smoke tests/texture_smoke.c -ldl
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/texture_smoke   # M4 纹理全链（需 lavapipe/loader）
gcc -o tests/fbo_smoke tests/fbo_smoke.c -ldl
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/fbo_smoke      # M5 S5 FBO/渲染缓冲 + MRT + MSAA + 状态管线（depth/scissor/blend/cull/stencil/colorMask）
gcc -o tests/3d_smoke tests/3d_smoke.c -ldl -lm
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/3d_smoke       # 3D 深度排序 + 透视投影（mat4 uniform 全链）
gcc -o tests/render3d_smoke tests/render3d_smoke.c -ldl -lm
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/render3d_smoke # 俯视场景：地板网格 + 立方体 + 像素断言，导出 tests/render3d.ppm
gcc -o tests/swapchain_smoke tests/swapchain_smoke.c -ldl
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/swapchain_smoke # M6 swapchain 契约 + surface 查询（无 Metal 时离屏降级）
gcc -o tests/sync_smoke tests/sync_smoke.c -ldl -lm
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/sync_smoke     # M6 S6 GLsync 同步对象（需 lavapipe/loader）
gcc -o tests/query_smoke tests/query_smoke.c -ldl
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/query_smoke   # M6 S6 查询对象 + primitive restart（需 lavapipe/loader）
gcc -o tests/sampler_smoke tests/sampler_smoke.c -ldl
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/sampler_smoke # M6 S6 sampler 对象生命周期 + 绑定/回退渲染（需 lavapipe/loader）
python3 scripts/ppm_render.py tests/render3d.ppm tests/render3d.png # PPM→PNG
```

> 本开发容器 ldd 找不到 libstdc++/libm/libgcc_s，运行 .so 相关程序需
> 显式 `LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu`。

## 契约文档
- `CHECKLIST.md` — 桥接契约（Amethyst GL bridge 硬性要求）+ 里程碑规划 + 架构
- `docs/gl33_core_list.md` — GL 3.3 core 342 函数分组
- `docs/egl_list.md` — EGL 导出符号清单

## 目录结构
```
src/egl     EGL 层（44 符号，display/config/context/surface 生命周期）
src/gl      分发电层（exports.cpp 生成 + 按域拆分的真实现 state/shader/vertex/draw）
src/shader  glslang GLSL→SPIR-V + SPIRV-Cross 反射（M2 完成）
src/state   GL 状态引擎（Context 结构、错误队列、capability 表）
src/vk      Vulkan 后端（dlsym 加载器、离屏渲染、动态 UBO 池、读回；engine/dispatch/target/swapchain/pipeline/fbo/draw 按域拆分）
scripts/    gen_gl_stubs.py（stub 生成器）、exported_symbols.txt
tests/      contract_smoke.c / state_smoke.c / shader_smoke.c / draw_smoke.c / texture_smoke.c / fbo_smoke.c / 3d_smoke.c / render3d_smoke.c / swapchain_smoke.c / sync_smoke.c / query_smoke.c / sampler_smoke.c
```