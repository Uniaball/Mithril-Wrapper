# Mithril-Wrapper 实现清单（CHECKLIST）

> 状态：**M8 真机问题修复中（iOS 黑屏/红屏排查 → UBO 对齐修复 + 诊断计数 + `multiubo_smoke` 双 CI 接入）；342/342 GL 符号全真（0 stub）；14 个冒烟 Linux + macOS CI 双跑通过。** M6 及以前完成（Linux 冒烟测试通过）。
> 配套文档：`docs/gl33_core_list.md`（GL 3.3 core 342 函数分组）、`docs/egl_list.md`（EGL 符号清单）。
> 测试：`tests/contract_smoke.c`（EGL 契约）、`tests/state_smoke.c`（GL 状态机）、`tests/shader_smoke.c`（着色器管线）、`tests/draw_smoke.c`（GL→Vulkan→读回全链）、`tests/texture_smoke.c`（M4 纹理全链）、`tests/fbo_smoke.c`（M5 状态管线 + S5 FBO/MRT/MSAA）、`tests/multiframe_smoke.c`（M6 帧环：64 帧交替绘制红/蓝三角形、每帧重写 VBO，async flush + 每 7 帧 sync finish 交错，逐帧中心色 + 对侧暗双点读回，152 项断言）、`tests/render3d_smoke.c`（M5 3D 场景：俯视地板网格 + 旋转立方体，60 帧动画逐帧像素断言 + 帧间差异断言，导出 `tests/render3d/frame_*.ppm`，CI 用 ffmpeg 合成 MP4 上传）、`tests/sync_smoke.c`（M6 stage C S6 sync 对象）、`tests/query_smoke.c`（M6 stage D S6 query 对象 + primitive restart）、`tests/sampler_smoke.c`（M6 stage E S6 sampler 对象）、`tests/multiubo_smoke.c`（M8 单帧多 draw UBO 偏移：三 tint 逐 draw 一次读回），均需 lavapipe/llvmpipe；`tests/swapchain_smoke.c`（M6 present，Linux 离屏降级 / macOS MoltenVK 真 swapchain），**14 个冒烟 Linux + macOS CI 双跑（test-macos-metal 全量接入，测试用 `__APPLE__` 分支加载 dylib）**。
> M2 已完成：`src/shader/`（Shader/Program 对象表、glslang 编译缓存 .glsl→SPIR-V、SPIRV-Cross 反射 uniform/attrib）、GLSL 150 自动升级到 330 core（含采样器存在时自动升 450 以启用 layout(binding=...) 注入，声明序 1 起绑定）、`gl_VertexID/gl_InstanceID` 重写、松散 uniform 折入合成 UBO（ANGLE 模式）、`glUniform*` 全系 + 矩阵 setter/getter；`src/vk/`（dlsym 动态加载 Vulkan loader/ICD、instance 级函数经真实 instance 句柄解析、UBO 反射 VS+FS 双阶段合并 + 动态 UBO 池、staging 顶点缓冲、renderpass + 清屏 + 帧读回）；`draw_smoke` 全链通过。
> M3 已完成：S3 组 76/114 真实现——顶点属性全家族（glVertexAttribPointer/IPointer、常量系 1-4、Divisor、Enable/Disable）、attribute 查询（fv/dv/iv/Iiv/Iuiv/Pointerv）、buffer 映射/查询家族（MapBuffer/Range、Unmap、FlushMapped、GetBufferParameteriv/i64v/Pointerv/SubData、CopyBufferSubData）、全部 10 个 draw 入口（DrawArrays/Instanced、DrawElements/BaseVertex/Instanced/InstancedBaseVertex、DrawRangeElements/BaseVertex、MultiDrawArrays/Elements/BaseVertex）；引擎新增双顶点流（VERTEX+INSTANCE binding）、索引缓冲（UBYTE/USHORT/UINT 统一扩 UINT32 staging）、TriangleStrip/Fan 拓扑、实例化（CPU 按 divisor 复制行，无需 EXT）；非 4 字节对齐 stride/offset 由 CPU 打包规整；draw_smoke 扩展 8 断言通过。
> **M4 完成（S4 42/42）**：`src/vk/texture.cpp`——staging→CmdCopyBufferToImage 全 mip 逐切片上传、常驻 TexObj 表、1x1 白色 dummy 兜底、采样器描述符绑定；image 类型扩展（2D/1D、3D volume、2D/1D array、cubemap，image/view/sampler 按类型创建，wrap_r 接入）；GL 端 `src/gl/texture.cpp` S4 全量 42 函数真实现——对象表（Gen/Delete/Is/Bind/Active）、TexImage1D/2D/3D + TexSub1D/2D/3D（cubemap face 归位、array 分层）、GetTexImage（PACK 对齐回读）、GenerateMipmap（每切片盒式滤波）、TexParameter/GetTexParameter 全系、GetTexLevelParameter（宽/高/深/压缩）、S3TC（DXT1/3/5）CPU 解压 + 原始压缩镜像 + GetCompressedTexImage、CopyTexImage1D/2D + CopyTexSubImage1D/2D/3D（帧读回）、glTexBuffer（buffer 引用 → 1xN 镜像上传）、glPixelStoref；**texture_smoke 26 断言全过**（采样/mip/dummy 白/GetTexImage 往返/DXT1 解压/3D 切片/数组分层/cubemap 6 面+mip/拷贝/texBuffer）。CI 已接 texture_smoke。
> **M5 完成（S5 FBO 全量 24 + MRT + MSAA）**：状态管线接入 Vulkan——深度附件（D24S8，renderpass 第二附件 + framebuffer 挂载）、`PipelineState` 快照（GL->vk 枚举映射 ToVkCompare/ToVkBlend/StencilOp/ColorMask）烘焙进 pipeline 缓存 key（`StateSignature`，含 depth/blend/scissor/cull/frontFace/stencil 全域含读/写掩码与 ref）与 `VkPipelineDepthStencil/ColorBlend/Rasterization...State`；深度清除统一为显式 `CmdClearDepthStencilImage`（depth 附件 loadOp=LOAD，修掉原先 `LOAD_OP_CLEAR` 无 clearValue 把 depth 清 0 的 bug）；动态 scissor 每 draw 下发（Y 翻转 + clamp，修 GL 左下/Vulkan 左上原点差）；`glClear(mask)`/`glDepthFunc`/`glDepthMask`/`glBlendFunc`/`glScissor`/`glColorMask`/`glCullFace`/`glFrontFace`/`glStencilFunc(Op/Mask)`/`glPolygonMode` 接入后端；`glStencilMask` 独立写掩码、depth view 补 S8 aspect、renderpass stencil loadOp=LOAD、stencil ref 入 pipeline key、GL→VK frontFace 取反。**S5 FBO/渲染缓冲 24 函数**：GL 层 `FbState`（color[8] 多附件槽 @attachment0..7 + depth，draw_bufs/read_buf，complete/dirty），`glGenFramebuffers→glGetFramebufferAttachmentParameteriv` 全系 + `glDrawBuffers/glDrawBuffer/glReadBuffer`；Vk 层 `FboObj`：每附件 FboSlot→ImageView、尺寸推 framebuffer 尺寸、rp_sig 含附件数/采样数。**MRT**：renderpass N 附件 + framebuffer N 视图、pipeline `attachmentCount=color_count` 每附件独立 blend（`draw_mask` 排除位写掩码清零）、显式 clear 每附件（仅 draw buffer 选中）、读回按 `read_buf` 挑附件。**MSAA**：renderbuffer `samples>1` → `rasterizationSamples` + resolve 附件（单采样转储）+ clear 写颜色与 resolve + 读回 resolve 图。**fbo_smoke 29 行 ok（28 断言）**：17 状态断言 + FBO 纹理/RBO/blit + MRT 双附件读回/单 drawBuffer 门控 + MSAA 4x 读回。回归：八个冒烟全过，M2/M3/M4 未破坏。
> **M6 stage C 完成（S6 sync 6/36）**：`src/gl/sync.cpp`——`glFenceSync`/`glDeleteSync`/`glIsSync`/`glClientWaitSync`/`glWaitSync`/`glGetSynciv` 六个真实现。GLsync 以 `struct SyncObj` 承载 `uint64_t fence`（Vk 句柄）+ 互斥锁保护的存活表；`glFenceSync` 校验 condition==GL_SYNC_GPU_COMMANDS_COMPLETE（否则 INVALID_ENUM→0）与 flags==0（否则 INVALID_VALUE→0），fence=`v::CreateGLSync()`；`glClientWaitSync` 限制 flags 仅 GL_SYNC_FLUSH_COMMANDS_BIT（违反→INVALID_VALUE+WAIT_FAILED），带位先 flush，无 fence 降级→`GL_ALREADY_SIGNALED`，`GL_TIMEOUT_IGNORED` 直传 UINT64_MAX；`glWaitSync` 校验 flags==0 && timeout==GL_TIMEOUT_IGNORED，有 fence 才阻塞（对齐 MobileGL no-op 语义）；`glGetSynciv` 四 pname（OBJECT_TYPE/CONDITION/FLAGS/STATUS）纯状态读不 flush，降级 STATUS 恒 GL_SIGNALED。**Vk 引擎侧 `src/vk/draw.cpp` `CreateGLSync()`**：`EnsureInit` 惰性启动 → `g.frame_dirty` 先 `SubmitFlush(false)`（未提交帧先 flush）→ 每条 GLsync 分配专用命令缓冲（Begin/End 空 CB）+ 独立 VkFence，CB 随 fence 一起提交（队列按序位于全部先前工作之后 → 语义 = GL_SYNC_GPU_COMMANDS_COMPLETE）。注意不能用 0 命令缓冲的空批：lavapipe 立即触发，但 MoltenVK 只在已提交的 MTLCommandBuffer 完成时推进 fence，空批永不触发 → macOS 超时；`glsyncs` 表（handle→{fence,cmd}）管理生命周期；`CheckGLSync`=`vkGetFenceStatus`（补齐 dispatch 表）；`WaitGLSync`=`vkWaitForFences(超时)`；`DestroyGLSync`=先 vkWaitForFences(UINT64_MAX) 再 FreeCommandBuffers+DestroyFence。**sync_smoke 全过**：引擎启动前 fence 阻塞等待可成功 + 查询合法、绘制+flush 后 fence 全 ivar 查询/等待满足/等待后 STATUS=SIGNALED/读回顺序一致（读回容差接受 0.2/0.3/0.9→51/76.5/229.5 两方向舍入，如 76↔77、229↔230，lavapipe/MoltenVK 方向相反）、零超时轮询语义、waitSync 校验、错误路径（非法 condition→0+INVALID_ENUM、DeleteSync 后操作→WAIT_FAILED+INVALID_VALUE、DeleteSync(0) no-op、bogus handle→IsSync false）。Linux + macOS CI 均接入 sync_smoke，**macOS MoltenVK 真机 CI 通过**。
> **M6 stage D 完成（S6 query 13/36 + primitive restart + provoking vertex）**：`src/gl/query.cpp`——GL 查询对象表（gen/delete/is/begin/end/queryCounter/五个 getter），`glBeginQuery` 校验 target（SAMPLES_PASSED/ANY_SAMPLES_PASSED/TIME_ELAPSED 等）与活动表（每 target 至多一活动 query，重复 begin→INVALID_OPERATION）、`glEndQuery` 无活动→INVALID_OPERATION、ANY_SAMPLES_PASSED 结果归一化非零→1；`src/vk/query.cpp`——QueryObj 包装 VkQueryPool 槽（每帧 occ_pool/ts_pool 各 4096 槽），遮挡查询在 Draw() 内经 `AllocDrawOccSlot` 给范围内每条 draw 记一槽（CmdBeginQuery/CmdEndQuery 括住），RetireFrame 栅栏后 `RetireFrameQueries` 用 GetQueryPoolResults 汇总；SAMPLES_PASSED 依赖 occlusionQueryPrecise 特性（无则降级读 0）、ANY_SAMPLES_PASSED 不依赖；TIME_ELAPSED = begin/end 两 vkCmdWriteTimestamp 差值 × timestampPeriod（`ts_valid_mask` 按 timestampValidBits 截断）；GL_TIMESTAMP 单写 × period；阻塞读在 `GetQueryResult64(wait)` 内 SubmitFlush(true)。**`glPrimitiveRestartIndex`**：GL 层 DrawElementsImpl 在 GL_PRIMITIVE_RESTART 开启时把 restart_index 改写为 0xFFFFFFFF，DrawCommon v_count 跳过标记；pipeline `ia.primitiveRestartEnable` + StateSignature `|PR`。**`glProvokingVertex`**：GL 状态 + pipeline key `|PV`，设备声明 VK_EXT_provoking_vertex 时 VkPhysicalDeviceProvokingVertexFeaturesEXT.provokingVertexLast + 光栅状态 EXT 链。**query_smoke 60 断言全过**（lavapipe：SAMPLES_PASSED 全屏三角 253888、深度遮挡 0 样本 / 前置 1、empty query 0、GL_CURRENT_QUERY 回读、TIME_ELAPSED 非零、GL_TIMESTAMP 非零、restart 两独立三角 + 无 restart fan 覆盖中部、错误路径全套）。Linux CI 已接 query_smoke。
> **M6 stage E 完成（S6 sampler 14/36）**：`src/gl/sampler.cpp`——GL sampler 对象表 + 14 函数真实现（`glGenSamplers`/`glDeleteSamplers`/`glIsSampler`/`glBindSampler` + `glSamplerParameter{f,fv,i,iv,Iiv,Iuiv}` 6 + `glGetSamplerParameter{fv,iv,Iiv,Iuiv}` 4，共 14；含错误路径全套、参数往返校验）。采样器状态与纹理解耦：GL sampler 对象经引擎常驻 `VkSampler` 表自拥采样器；引擎描述符绑定由 `(binding, tex_id)` 升级为 `(binding, sampler_id, tex_id)`（`src/vk/engine.h` 新增 `SamplerBind` 结构），GL draw 路径把 `sampler_id` 一路下传；绑定 sampler 对象时配对该 `VkSampler` 与纹理 image view，未绑定（`sampler_id==0`）时回退纹理自带 sampler（`glTexParameteri` 烘焙）。引擎新增 API：`UpdateSampler(uint64_t, const TexSamplerInfo&)`/`DestroyResidentSampler(uint64_t)`/`GetResidentSampler(uint64_t)`。**sampler_smoke 全过**（lavapipe：生命周期 gen/IsSampler/delete、bind 错误路径、参数往返、getter/setter 错误路径、绑定 sampler 对象渲染（NEAREST）vs 纹理回退、NEAREST/LINEAR 可观测差异、delete 解绑）。Stage E — Sampler Objects 验收项：
>   - [x] sampler 对象表 + 14 entry points（`src/gl/sampler.cpp`）
>   - [x] 引擎 `SamplerBind` + 常驻 `VkSampler` 表（`src/vk/engine.h`）
>   - [x] GL draw 路径下传 `sampler_id`
>   - [x] `sampler_smoke` 本地通过
>   - [x] CI 接入（test-linux lavapipe + test-macos-metal MoltenVK 均已加 `sampler_smoke` 编译+运行步骤，grep `SAMPLER SMOKE ALL PASSED`）
> 待办：~~M6 stage F~~（收尾完成：全部 stub 转真 + multiframe_smoke 绘制交替逐帧读回 + query_smoke 条件渲染断言 + CI/文档同步，见上）。
> **M8 真机问题修复进行中**：动/静 UBO 对齐修复（b195a5b）——`src/vk/draw.cpp` 每帧动态 UBO 偏移由硬编码 `AlignUp(..., 16)` 改为 `AlignUp(frame.ubo_next, g.ubo_align)`（`src/vk/internal.h` 启动时从 `VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment` 读取并打日志：lavapipe=16、macOS Metal=16、MoltenVK/iOS=256）。根因：旧码在 iOS 上每帧第二条 draw 起 uniform 块落在未对齐偏移（Metal 按 256 对齐的恒定缓冲偏移），零化/垃圾矩阵 → 几何塌缩黑屏，且无 GL/VK 报错——lavapipe 与 macOS CI 均无法复现（对齐都是 16）。配套设备端诊断计数器（`stats_draws_vk/skipped/pipe_fail/ubo_wrap`、`last_frame_ops`，`Present()` 周期转储 + swapchain 冒烟可读），把设备黑屏日志变成二分定位：`stats_draws_vk` 增长而画面全黑 → 渲染路径（pipeline/UBO）；`stats_draws_vk` 不动 → GL 取数层。**`tests/multiubo_smoke.c`**：单帧三次带 uniform 的 draw（红/绿/蓝 tint，共用 VAO/VBO 逐 draw 重传），一次读回断言三处颜色各自保持——正是 iOS 上被破坏的调用形态；test-macos-metal（MoltenVK）与 test-linux（lavapipe）双接（构建 + grep `MULTIUBO SMOKE ALL PASSED` 断言步骤）。前序 M8 修复：`SetTargetSize` 后新 target 未转 `IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL` → 设备红屏（b688aec）；`glTexSubImage2D` 风暴合并为单次 region 上传（b727577）。

---

## 1. 项目定位

在 iOS 上为 Minecraft Java（LWJGL 3）提供自研 OpenGL 实现：

- **目标 GL**：OpenGL 3.3 Core Profile（仅实现必要的子集，由 Amethyst 启动器注释确认：`gl_bridge.m:127` "Mithril 是 OpenGL 3.3 Core Profile"）
- **渲染后端**：自研 GL → Vulkan → Metal（MoltenVK），无 OpenGL ES 参与
- **交付物**：`libmithril.dylib`（arm64），由 GitHub Actions macOS runner 构建，集成进 Amethyst 启动器
- **开发循环**：本地 Linux 编译 `.so` 验证逻辑，CI 出 dylib
- **参考实现**（仅参考，从零实现）：`~/Desktop/projects/dg/DesktopGlues`、`~/Desktop/opencode/mobilegl/MobileGL`（MobileGL，GL→Vulkan 完整实现）、`~/Desktop/opencode/air/Amethyst-iOS-MyRemastered`（启动器桥接契约）

## 2. 桥接契约（Amethyst 启动器侧，硬性要求）

来源：`Natives/ctxbridges/gl_bridge.m`（已逐行核对）。

### 2.1 必须导出的 EGL 符号（gl_bridge.m:73-97 逐个 dlsym）

18 个：`eglBindAPI eglChooseConfig eglCreateContext eglCreateWindowSurface eglDestroyContext eglDestroySurface eglGetConfigAttrib eglGetCurrentContext eglGetCurrentSurface eglGetDisplay eglGetError eglGetPlatformDisplay eglInitialize eglMakeCurrent eglReleaseThread eglSwapBuffers eglSwapInterval eglTerminate`

另需：`eglGetProcAddress`（LWJGL/GLFW/SDL3 解析 GL 扩展函数）、`eglGetConfigs`、`eglQueryString`（SDL3 dump）。建议完整导出 EGL 1.5 全 44 个 + EXT 别名（见 `docs/egl_list.md`）。

### 2.2 行为契约（gl_bridge.m:110-257）

1. `eglGetDisplay(EGL_DEFAULT_DISPLAY)` → 非 NULL display
2. `eglInitialize(display, NULL, NULL)` → 成功
3. `eglChooseConfig` 对以下属性**必须返回 ≥1 个配置**（gl_bridge.m:133-141，否则 `assert(bundle->config)` 崩溃）：
   - `EGL_RED_SIZE=8, GREEN=8, BLUE=8, ALPHA=8, DEPTH=24`
   - `EGL_SURFACE_TYPE = EGL_WINDOW_BIT | EGL_PBUFFER_BIT`
   - `EGL_RENDERABLE_TYPE = EGL_OPENGL_BIT`（desktop GL 分支，Mithril 走此路径）
4. `eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID)` → 成功
5. `eglBindAPI(EGL_OPENGL_API)` → EGL_TRUE（Mithril 走 desktop GL 分支 gl_bridge.m:161-163）
6. `eglCreateWindowSurface(display, config, (EGLNativeWindowType)CALayer*, NULL)` — **native window 类型 = CALayer***（gl_bridge.m:171）
7. `eglCreateContext(display, config, share_ctx, {EGL_CONTEXT_CLIENT_VERSION, 3})`（gl_bridge.m:178-183）
8. `eglMakeCurrent` 支持解绑：`eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)`（gl_bridge.m:194-200）
9. `eglSwapBuffers` / `eglSwapInterval`（interval 受 `POJAV_DISABLE_VSYNC=1` 强制为 0，gl_bridge.m:224-231）
10. 终止顺序：`eglMakeCurrent(EGL_NO_SURFACE)` → `eglDestroySurface` → `eglDestroyContext` → `eglTerminate` → `eglReleaseThread`（gl_bridge.m:249-257）

### 2.3 GL 侧要求

- LWJGL 通过 `-Dorg.lwjgl.opengl.libname=libmithril.dylib` **直接 dlsym GL 符号**（JavaLauncher.m:825-870），因此所有需用 GL 函数必须真实导出为符号（default visibility）
- `eglGetProcAddress` + `glXGetProcAddress`/`glXGetProcAddressARB` 作为 LWJGL 解析回退
- `GL_VERSION` 字符串需含 `3.3 Core Profile`（MC 1.17+ 拒绝无 Core Profile 的实现）；`glGetStringi` 需可用（扩展查询，Sodium/Iris）
- 关键扩展声明（Sodium/Iris 依赖）：`GL_ARB_buffer_storage`、`GL_ARB_draw_buffers_blend` 等（LTW 注释 gl_bridge.m:60-68 明确）

### 2.4 环境变量与路径

| 变量 | 值 | 位置 |
|---|---|---|
| `AMETHYST_RENDERER` | `libmithril.dylib`（触发 Mithril 路径的唯一值） | gl_bridge.m:31 |
| `SDL_EGL_LIBRARY` / `SDL_OPENGL_LIBRARY` | `@rpath/libmithril.dylib`（SDL3 窗口后端时） | JavaLauncher.m:945-949 |
| `POJAV_DISABLE_VSYNC` | `1` → swap interval 0 | gl_bridge.m:224 |

- 产物位置：app bundle `Frameworks/libmithril.dylib`；加载用 `@rpath/libmithril.dylib`
- Mach-O 要求：arm64、TWOLEVEL、`install name = @rpath/libmithril.dylib`、符号 default visibility
- **macOS 侧导出白名单**：参考 MobileGL `ExportedSymbols.txt` 模式：`_egl*`、`_gl[A-Z0-9]*`、`_glX*`，隐藏 glslang/spirv 等内部 C++ 符号，避免污染 host（`-exported_symbols_list`）

## 3. GL 函数子集（已生成，见 docs/gl33_core_list.md）

- **基础**：从 MobileGL 自带 Khronos `glcorearb.h` 提取 `GL_VERSION_1_0`..`GL_VERSION_3_3` 累计 → **342 个函数**
- **交叉验证**：
  - desktopglues（libdesktopglues.so，3062 gl 导出）：342 全部覆盖 ✅
  - MobileGL `Definitions.cpp`（2766 导出）：342 全部覆盖 ✅（glFinish/glFlush 为独立导出形式）
  - LWJGL core 类（GL11C..GL33C，712 函数）含 4.x 函数，超出范围剔除
- 分组（docs/gl33_core_list.md，按实现里程碑）：
    - S1 状态/使能/基础查询：55（glEnable/glClear/glViewport/glBlend/glDepth/glStencil/glGet*…）
    - S2 着色器/程序/Uniform：71（glCreateShader→glUseProgram、glUniform* 全系、程序反射）
    - S3 Buffer/VAO/顶点/Draw：114（glGenBuffers→glDrawArrays/glDrawElements 全系、glVertexAttrib* 全系）
    - S4 纹理：42（glGenTextures→glTexImage2D/glTexSubImage2D/glGenerateMipmap；**S4 全量 42 真实现完成，texture_smoke 26 断言通过**）
    - S5 FBO/渲染缓冲：24（glGenFramebuffers→glGetFramebufferAttachmentParameteriv 全系 + glDrawBuffer/glDrawBuffers/glReadBuffer + glRenderbufferStorageMultisample；**S5 全量 24 真实现完成，MRT + MSAA 支持，fbo_smoke 29 断言通过**）
    - S6 同步/Query/Sampler：36（glGenQueries/glFenceSync/glSamplerParameter 系；**S6 36/36 全部完成：sync 6 + query 13（含 ConditionalRender 2 降级实现）+ sampler 14，query_smoke/sampler_smoke 通过**）
  - 342 全部归入以上六组，无遗漏
- 未实现函数：导出为 stub，返回 `GL_INVALID_OPERATION` + 日志（MobileGL `DECLARE_GL_FUNCTION_STUB_HEAD` 模式）
- 最终裁剪依据：M7 用 apitrace 抓 Minecraft 真实调用 → 与 desktopglues 清单交叉

## 4. 架构设计（从零实现）

```
LWJGL (Minecraft Java) — dlsym libmithril.dylib
        │
        ▼
GL 分发表（src/gl/gl_dispatch.cpp，手写 gl* 符号，未实现返回 INVALID_OPERATION+日志）
EGL 层（src/egl/，44 符号，display/config/context/surface 生命周期 + thread_local 当前状态）
        │  桥接契约：CAMetalLayer 作为 native window（iOS）
        ▼
对象与状态管理层（src/state/）
   ├─ 对象表：VAO/VBO/IBO/Texture/FBO/Program/Query → Vulkan 对象 + 名称池（虚拟 ID，参照 MobileGL buffer.cpp 模式）
   ├─ 状态跟踪：blend/depth/stencil/cull/viewport/scissor/顶点布局/当前 program/纹理单元（参照 MobileGL GLState）
   ├─ 错误队列：GLenum 错误栈（glGetError 语义）
   └─ 着色器：glslang GLSL→SPIR-V；SPIRV-Cross 反射 → uniform/采样器绑定（iOS 无 GLSL 编译能力，必须走 glslang）
        │
        ▼
Vulkan 后端（src/vk/，经 MoltenVK → Metal → CAMetalLayer）
   ├─ pipeline 工厂：状态哈希 key → VkPipeline 缓存（program_hash, blend, depth, stencil, raster, 顶点布局, 附件格式, sample_count, cull）
   ├─ 命令记录：每帧一个 VkCommandBuffer，draw 时录制，present 前提交；双缓冲 + fence
   ├─ 显式同步：upload→shader barrier、帧 fence
   ├─ 差异修复：非 4 字节对齐 stride/offset → staging 规整（MoltenVK portability 限制）
   └─ swapchain：CAMetalLayer → VkSwapchain（VK_EXT_metal_surface）
```

### 4.1 关键设计决策

| 决策 | 选择 |
|---|---|
| GL 版本 | 3.3 Core Profile，仅实现必要子集 |
| EGL 虚拟后端 | 自实现 44 符号，config 生成器（单 display、单 config 满足契约属性即可） |
| 上下文 | 单渲染上下文（MC 单窗口场景） |
| 着色器 | glslang（vendored 或系统库）编译 GLSL→SPIR-V，SPIRV-Cross 做反射；无运行时 GLSL 编译 |
| 正确性标准 | MC 画面跑通即可，不做 CTS |
| 对象 ID | 虚拟 ID → 驱动 ID 映射，名称池复用（MobileGL buffer.cpp 模式） |
| 未实现函数 | stub 返回 GL_INVALID_OPERATION + 日志（不产生崩溃） |

### 4.2 源码目录规划

```
~/Desktop/projects/mithril/
├── CMakeLists.txt            # 双分支：Linux .so / macOS(CI) dylib
├── src/
│   ├── egl/                  # EGL 实现 + 导出符号
│   ├── gl/                   # gl* 分发表 + stub
│   ├── state/                # 对象表 + 状态跟踪 + 错误栈
│   ├── vk/                   # Vulkan 后端（pipeline 工厂/命令记录/swapchain/同步）
│   ├── shader/               # glslang 集成 + SPIRV-Cross 反射
│   └── util/                 # 日志、哈希、平台封装
├── third_party/              # glslang、SPIRV-Cross、Vulkan 头（vendored）
├── .github/workflows/build.yml
├── docs/                     # 本清单 + 分组清单
└── output/                   # CI 产物归档（本机占位）
```

## 5. 里程碑

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M0 基建** | CMake 双分支工程；EGL 44 符号骨架 + config 契约；GL 342 stub 分发表；CI build.yml（macOS runner + MoltenVK + glslang）；Linux 编 .so 验证；iOS 分支 CAMetalLayer→swapchain 纯色 | Linux .so 可加载、EGL 契约通过、CI 产出 dylib |
| **M1 注入与 Context** | eglMakeCurrent 语义完整；glClear/glViewport/glClearColor 实现；GL_VERSION="3.3 Core Profile" 字符串 | 真机 demo 背景色正确 |
| **M2 Shader 管线** | glShaderSource→glslang→SPIR-V→vkShaderModule；UBO 管理；glDrawArrays 三角形 | 带色三角形 | ✅ S2 层完成（glslang+SPIRV-Cross、shader_smoke 通过）+ Vulkan 后端接通（`src/vk/`：dlsym 加载器、instance GIPA 解析、UBO 双阶段反射、动态 UBO 池、staging 顶点缓冲、renderpass/清屏/读回）；draw_smoke 全链通过（llvmpipe） |
| **M3 顶点数据** | VAO/VBO、stride/offset 规整、glDrawElements | 用 App 数据绘制 | ✅ S3 76/114 完成（属性全系/映射查询/draw 十入口/双流+索引+拓扑+实例化）；draw_smoke 扩展 8 断言通过 |
| **M4 纹理** | glTexImage2D 上传、格式/swizzle、采样器、mipmap | 贴图正确 | ✅ S4 42/42 完成（纹理对象表、TexImage/Sub/压缩/CopyTex/GetTexImage/GenerateMipmap/texBuffer）；texture_smoke 26 断言通过 |
| **M5 状态与管线** | depth/stencil/blend/cull/FBO/MSAA → pipeline 缓存 | 完整三维画面 | ✅ S5 FBO/渲染缓冲 24 函数 + MRT（多附件/每附件 blend/read_buf 读回）+ MSAA（resolve 附件/readres）；fbo_smoke 29 断言全过 |
| **M6 同步** | 双 CMD buffer、barrier、glFinish/glFlush；swapchain/present；GLsync 同步对象 | 多帧不花屏 | 🔵 M6 六个 stage 全部完成（验收测试全过）：stage A 帧环（`multiframe_smoke` 64 帧交替绘制红/蓝三角形、每帧重写 VBO，async flush + 每 7 帧 sync finish 交错，逐帧中心色 + 对侧暗双点读回，152 项断言，Linux 通过）+ stage B swapchain/present（`swapchain.cpp`：CAMetalLayer → VK_EXT_metal_surface + VK_KHR_swapchain，acquire→blit→present 同步，`mithril_has_swapchain` 探针；macOS CI 经 MoltenVK 真 swapchain `swapchain_smoke` 通过）+ **stage C S6 sync 对象** + **stage D S6 query 对象** + **stage E S6 sampler 对象** + **stage F 收尾（S1 补 5/S2 补 11/S3 补 38/S4 补 3/S6 补 2 → 342/342 全真实现，0 stub）** |
| **M7 收窄** | apitrace 实测 MC 调用 → 裁剪/补漏 | 与 GL 参考输出截图一致 |
| **M8 集成** | 打包进 Amethyst，真机验证 | 完整可玩 | 🔵 进行中：真机黑屏根因（UBO 对齐）已修 + 诊断计数器 + `multiubo_smoke` 双 CI 接入；红屏（SetTargetSize 布局）、texSubImage 风暴修复完成；待打包进 Amethyst 真机验证 |

## 6. 技术风险与应对

| 风险 | 应对 |
|---|---|
| MoltenVK portability（4 字节对齐、动态状态缺失、VK 1.3 不全） | staging 规整；只用 MoltenVK 支持子集；验证层排查 |
| glslang 输出与 MoltenVK 后端不兼容 | 参考 desktopglues/MobileGL 已验证着色器路径；必要时 MSL 变通 |
| iOS 无系统 GL 可回落 | 全部 proc/surface/present 自处理；详细日志 |
| LWJGL 大量 Get/查询函数需"诚实作答" | 参照 desktopglues getter.cpp 的虚值策略（MobileGL 更完整） |
| Amethyst 版本耦合 | 严格对照 gl_bridge.m 契约；保持 ABI 稳定 |

## 7. 关键参考（文件:行号索引）

| 参考点 | 位置 |
|---|---|
| EGL 18 符号 dlsym | Amethyst `Natives/ctxbridges/gl_bridge.m:73-97` |
| eglChooseConfig 契约属性 | 同上 `:133-141` |
| desktop GL 分支（EGL_OPENGL_BIT/API） | 同上 `:126-167` |
| CALayer 作为 native window | 同上 `:171` |
| context 属性 {CLIENT_VERSION,3} | 同上 `:178-183` |
| 终止顺序 | 同上 `:249-257` |
| LWJGL libname 注入 | Amethyst `Natives/JavaLauncher.m:825-870` |
| SDL3 EGL 重定向 | 同上 `:928-964` |
| GL 3.3 core 官方函数表 | MobileGL `include/GL/glcorearb.h`（GL_VERSION_1_0..3_3） |
| GL 符号导出宏模式 | MobileGL `MG_Impl/GLImpl/Exporting/Definitions.cpp:25-58` |
| EGL 导出模式 | MobileGL `MG_Impl/EGLImpl/Exporting/Definitions.cpp` |
| macOS 导出白名单 | MobileGL `MG_Impl/DyldInterpose/ExportedSymbols.txt` + CMakeLists.txt:462-473 |
| iOS 构建分支 | MobileGL `CMakeLists.txt:493-502`（MOBILEGL_IOS + MOBILEGL_VULKAN_LIBRARY） |
| 对象表/状态管理 | MobileGL `MG_State/`（buffer.cpp 虚拟 ID 模式、GLState/EGLState 分离） |
| getter 虚值策略 | DesktopGlues `MobileGlues-cpp/gl/getter.cpp` |
