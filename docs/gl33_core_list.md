# GL 3.3 Core Profile 全量清单

来源：MobileGL include/GL/glcorearb.h（Khronos 官方），GL_VERSION_1_0..3_3 累计。

> 实现状态（M6 stage E 完成）：S1 51/55、S2 60/71、S3 76/114、S4 42/42、S5 24/24、S6 33/36 已真实现（`src/gl/state.cpp`/`shader.cpp`/`vertex.cpp`/`draw.cpp`/`texture.cpp`/`fbo.cpp`/`sync.cpp`/`query.cpp`/`sampler.cpp` + `src/shader/`（glsl/reflect/registry 三 TU）+ `src/vk/`（engine/dispatch/target/pipeline/fbo/draw/query 七 TU + texture）），合计 284 真导出；其余为 stub（返回 `GL_INVALID_OPERATION` + 日志）。M5 验收：`fbo_smoke`（state pipeline 17 断言 + FBO 纹理/RBO/blit + MRT 双附件回读/单 drawBuffer 门控 + MSAA 4x resolve，29 行 ok / 28 断言）。M6 stage C 验收：`sync_smoke`。M6 stage D 验收：`query_smoke`（S6 query 13 函数 + 深度遮挡/时间戳计数 + primitive restart + provoking vertex，60 断言，lavapipe 通过）。M6 stage E 验收：`sampler_smoke`（S6 sampler 14 函数 + 采样器/纹理解耦 + 绑定/回退渲染，lavapipe 通过）。余 ConditionalRender 2、S3 剩余 38 个 stub 待启动。
> S3 剩余 38 个 stub（无绘制影响）：TransformFeedback 系 5（Begin/End/GetVarying/BindBufferBase/Range）、glPointParameter* 4、整型属性 setter 系 20（glVertexAttribI1..I4 各变体）、打包 setter 系 8（glVertexAttribP*）、glVertexAttrib4Nub。

## S1 状态/使能/基础查询 — 55

```
glBlendColor              glBlendEquation           glBlendEquationSeparate   glBlendFunc               glBlendFuncSeparate       glClampColor            
glClear                   glClearBufferfi           glClearBufferfv           glClearBufferiv           glClearBufferuiv          glClearColor            
glClearDepth              glClearStencil            glColorMask               glColorMaski              glCullFace                glDepthFunc             
glDepthMask               glDepthRange              glDisable                 glDisablei                glEnable                  glEnablei               
glFinish                  glFlush                   glFrontFace               glGetBooleanv             glGetDoublev              glGetError              
glGetFloatv               glGetInteger64v           glGetIntegerv             glGetMultisamplefv        glGetPointerv             glGetString             
glGetStringi              glHint                    glIsEnabled               glIsEnabledi              glLineWidth               glLogicOp               
glPointSize               glPolygonMode             glPolygonOffset           glSampleCoverage          glSampleMaski             glScissor               
glStencilFunc             glStencilFuncSeparate     glStencilMask             glStencilMaskSeparate     glStencilOp               glStencilOpSeparate     
glViewport              
```

## S2 着色器/程序/Uniform — 71

```
glAttachShader            glBindAttribLocation      glBindFragDataLocation    glBindFragDataLocationIndexed  glCompileShader           glCreateProgram         
glCreateShader            glDeleteProgram           glDeleteShader            glDetachShader            glGetActiveAttrib         glGetActiveUniform      
glGetActiveUniformBlockName  glGetActiveUniformBlockiv  glGetActiveUniformName    glGetActiveUniformsiv     glGetAttachedShaders      glGetAttribLocation     
glGetFragDataIndex        glGetFragDataLocation     glGetProgramInfoLog       glGetProgramiv            glGetShaderInfoLog        glGetShaderSource       
glGetShaderiv             glGetUniformBlockIndex    glGetUniformIndices       glGetUniformLocation      glGetUniformfv            glGetUniformiv          
glGetUniformuiv           glIsProgram               glIsShader                glLinkProgram             glShaderSource            glUniform1f             
glUniform1fv              glUniform1i               glUniform1iv              glUniform1ui              glUniform1uiv             glUniform2f             
glUniform2fv              glUniform2i               glUniform2iv              glUniform2ui              glUniform2uiv             glUniform3f             
glUniform3fv              glUniform3i               glUniform3iv              glUniform3ui              glUniform3uiv             glUniform4f             
glUniform4fv              glUniform4i               glUniform4iv              glUniform4ui              glUniform4uiv             glUniformBlockBinding   
glUniformMatrix2fv        glUniformMatrix2x3fv      glUniformMatrix2x4fv      glUniformMatrix3fv        glUniformMatrix3x2fv      glUniformMatrix3x4fv    
glUniformMatrix4fv        glUniformMatrix4x2fv      glUniformMatrix4x3fv      glUseProgram              glValidateProgram       
```

## S3 Buffer/VAO/顶点/Draw — 114

```
glBeginTransformFeedback  glBindBuffer              glBindBufferBase          glBindBufferRange         glBindVertexArray         glBufferData            
glBufferSubData           glCopyBufferSubData       glDeleteBuffers           glDeleteVertexArrays      glDisableVertexAttribArray  glDrawArrays            
glDrawArraysInstanced     glDrawElements            glDrawElementsBaseVertex  glDrawElementsInstanced   glDrawElementsInstancedBaseVertex  glDrawRangeElements     
glDrawRangeElementsBaseVertex  glEnableVertexAttribArray  glEndTransformFeedback    glFlushMappedBufferRange  glGenBuffers              glGenVertexArrays       
glGetBufferParameteri64v  glGetBufferParameteriv    glGetBufferPointerv       glGetBufferSubData        glGetTransformFeedbackVarying  glGetVertexAttribIiv    
glGetVertexAttribIuiv     glGetVertexAttribPointerv  glGetVertexAttribdv       glGetVertexAttribfv       glGetVertexAttribiv       glIsBuffer              
glIsVertexArray           glMapBuffer               glMapBufferRange          glMultiDrawArrays         glMultiDrawElements       glMultiDrawElementsBaseVertex
glPointParameterf         glPointParameterfv        glPointParameteri         glPointParameteriv        glUnmapBuffer             glVertexAttrib1d        
glVertexAttrib1dv         glVertexAttrib1f          glVertexAttrib1fv         glVertexAttrib1s          glVertexAttrib1sv         glVertexAttrib2d        
glVertexAttrib2dv         glVertexAttrib2f          glVertexAttrib2fv         glVertexAttrib2s          glVertexAttrib2sv         glVertexAttrib3d        
glVertexAttrib3dv         glVertexAttrib3f          glVertexAttrib3fv         glVertexAttrib3s          glVertexAttrib3sv         glVertexAttrib4Nbv      
glVertexAttrib4Niv        glVertexAttrib4Nsv        glVertexAttrib4Nub        glVertexAttrib4Nubv       glVertexAttrib4Nuiv       glVertexAttrib4Nusv     
glVertexAttrib4bv         glVertexAttrib4d          glVertexAttrib4dv         glVertexAttrib4f          glVertexAttrib4fv         glVertexAttrib4iv       
glVertexAttrib4s          glVertexAttrib4sv         glVertexAttrib4ubv        glVertexAttrib4uiv        glVertexAttrib4usv        glVertexAttribDivisor   
glVertexAttribI1i         glVertexAttribI1iv        glVertexAttribI1ui        glVertexAttribI1uiv       glVertexAttribI2i         glVertexAttribI2iv      
glVertexAttribI2ui        glVertexAttribI2uiv       glVertexAttribI3i         glVertexAttribI3iv        glVertexAttribI3ui        glVertexAttribI3uiv     
glVertexAttribI4bv        glVertexAttribI4i         glVertexAttribI4iv        glVertexAttribI4sv        glVertexAttribI4ubv       glVertexAttribI4ui      
glVertexAttribI4uiv       glVertexAttribI4usv       glVertexAttribIPointer    glVertexAttribP1ui        glVertexAttribP1uiv       glVertexAttribP2ui      
glVertexAttribP2uiv       glVertexAttribP3ui        glVertexAttribP3uiv       glVertexAttribP4ui        glVertexAttribP4uiv       glVertexAttribPointer   
```

## S4 纹理 — 42

```
glActiveTexture           glBindTexture             glCompressedTexImage1D    glCompressedTexImage2D    glCompressedTexImage3D    glCompressedTexSubImage1D
glCompressedTexSubImage2D  glCompressedTexSubImage3D  glCopyTexImage1D          glCopyTexImage2D          glCopyTexSubImage1D       glCopyTexSubImage2D     
glCopyTexSubImage3D       glDeleteTextures          glGenTextures             glGenerateMipmap          glGetCompressedTexImage   glGetTexImage           
glGetTexLevelParameterfv  glGetTexLevelParameteriv  glGetTexParameterIiv      glGetTexParameterIuiv     glGetTexParameterfv       glGetTexParameteriv     
glIsTexture               glPixelStoref             glPixelStorei             glTexBuffer               glTexImage1D              glTexImage2D            
glTexImage2DMultisample   glTexImage3D              glTexImage3DMultisample   glTexParameterIiv         glTexParameterIuiv        glTexParameterf         
glTexParameterfv          glTexParameteri           glTexParameteriv          glTexSubImage1D           glTexSubImage2D           glTexSubImage3D         
```

## S5 FBO/渲染缓冲 — 24

```
glBindFramebuffer         glBindRenderbuffer        glBlitFramebuffer         glCheckFramebufferStatus  glDeleteFramebuffers      glDeleteRenderbuffers   
glDrawBuffer              glDrawBuffers             glFramebufferRenderbuffer  glFramebufferTexture      glFramebufferTexture1D    glFramebufferTexture2D  
glFramebufferTexture3D    glFramebufferTextureLayer  glGenFramebuffers         glGenRenderbuffers        glGetFramebufferAttachmentParameteriv  glGetRenderbufferParameteriv
glIsFramebuffer           glIsRenderbuffer          glReadBuffer              glReadPixels              glRenderbufferStorage     glRenderbufferStorageMultisample
```
> ✅ 24/24 真实现完成（MRT 多附件 + MSAA resolve，见 CHECKLIST.md M5 段落）。

## S6 同步/Query/Sampler — 36

> ✅ sync 6/36（M6 stage C）真实现完成：`glFenceSync glDeleteSync glIsSync glClientWaitSync glWaitSync glGetSynciv`（src/gl/sync.cpp，GLsync 包装 VkFence；未提交帧先 flush；无后端降级按 MobileGL always-signaled 模式）；sync_smoke 通过。
> ✅ Query 13/36（M6 stage D）真实现完成：`glGenQueries glDeleteQueries glIsQuery glBeginQuery glEndQuery glGetQueryiv glGetQueryObjectiv/uiv/i64v/ui64v glQueryCounter`（src/gl/query.cpp + src/vk/query.cpp：每查询包装 VkQueryPool 槽；SAMPLES_PASSED/ANY_SAMPLES_PASSED 遮挡计数（含深度测试 0/非 0 判定，SAMPLES_PASSED 依赖 occlusionQueryPrecise、ANY 不依赖）、TIME_ELAPSED（begin/end 双时间戳差值 × timestampPeriod）、GL_TIMESTAMP（glQueryCounter 单写）；降级语义同 sync）；+ `glPrimitiveRestartIndex`（GL 层改写 restart index→0xFFFFFFFF，DrawCommon v_count 跳过标记）+ `glProvokingVertex`（入 PipelineState key，VK_EXT_provoking_vertex 存在时启 provokingVertexLast）；query_smoke 60 断言通过。
> ✅ Sampler 14/36（M6 stage E）真实现完成：`glGenSamplers glDeleteSamplers glIsSampler glBindSampler glSamplerParameter{f,fv,i,iv,Iiv,Iuiv} glGetSamplerParameter{fv,iv,Iiv,Iuiv}`（src/gl/sampler.cpp + 引擎常驻 VkSampler 表：采样器状态与纹理解耦，`SamplerBind` 升级 (binding, sampler_id, tex_id)，GL draw 路径下传 sampler_id，无 sampler 对象（sampler_id==0）时回退纹理自带 sampler）；引擎新增 UpdateSampler/DestroyResidentSampler/GetResidentSampler；sampler_smoke 通过。余 ConditionalRender 2 待启动。

```
glBeginConditionalRender      glBeginQuery ✅            glBindSampler ✅            glClientWaitSync ✅         glDeleteQueries ✅          glDeleteSamplers ✅       
glDeleteSync ✅               glEndConditionalRender    glEndQuery ✅              glFenceSync ✅              glGenQueries ✅             glGenSamplers ✅          
glGetQueryObjecti64v ✅       glGetQueryObjectiv ✅      glGetQueryObjectui64v ✅   glGetQueryObjectuiv ✅      glGetQueryiv ✅             glGetSamplerParameterIiv ✅
glGetSamplerParameterIuiv ✅    glGetSamplerParameterfv ✅  glGetSamplerParameteriv ✅  glGetSynciv ✅              glIsQuery ✅                glIsSampler ✅            
glIsSync ✅                   glPrimitiveRestartIndex ✅ glProvokingVertex ✅       glQueryCounter ✅           glSamplerParameterIiv ✅      glSamplerParameterIuiv ✅ 
glSamplerParameterf ✅          glSamplerParameterfv ✅     glSamplerParameteri ✅      glSamplerParameteriv ✅      glTransformFeedbackVaryings   glWaitSync ✅              
```

## S7 其余 — 0

```
```
