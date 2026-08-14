// Mithril-Wrapper GL entry points -- S1 state domain (milestone M1).
// Capabilities, viewport/scissor, clear state, face/polygon, depth,
// stencil, blend, color mask, logic op/hint/pixel store, and the S1
// getters. These functions are excluded from the generated stub table;
// see MGL_IMPL in scripts/gen_gl_stubs.py.

#include "internal.h"

// Choose a small helper for capability writes shared with glEnable/glDisable.
static bool CapValid(GLenum cap) {
    switch (cap) {
        case GL_DEPTH_TEST:
        case GL_STENCIL_TEST:
        case GL_BLEND:
        case GL_DITHER:
        case GL_CULL_FACE:
        case GL_SCISSOR_TEST:
        case GL_POLYGON_OFFSET_FILL:
        case GL_SAMPLE_ALPHA_TO_COVERAGE:
        case GL_SAMPLE_COVERAGE:
        case GL_MULTISAMPLE:
        case GL_RASTERIZER_DISCARD:
        case GL_PROGRAM_POINT_SIZE:
        case GL_LOGIC_OP_MODE:
        case GL_PRIMITIVE_RESTART:
            return true;
        default:
            return false;
    }
}

static void SetCap(GLenum cap, bool on) {
    auto& st = s::GetState();
    uint32_t idx = st.caps.Normalize(cap);
    if (idx >= s::kMaxCaps) {
        PUSH_ERROR(GL_INVALID_ENUM);
        return;
    }
    st.caps.bits[idx] = on;
}

// --- capabilities -----------------------------------------------------------

// M8 device diag: count clear events (total + color clears) so the periodic
// target diag can tell whether a black target comes from the clear path or
// the draw path. Defined OUTSIDE the extern "C" block below (C-linkage
// declarations in internal.h are C++; see GetGlDrawCalls in draw.cpp).
static uint64_t g_gl_clears = 0;
static uint64_t g_gl_color_clears = 0;
uint64_t GetGlClears() { return g_gl_clears; }
uint64_t GetGlColorClears() { return g_gl_color_clears; }

static void NoteClear(GLbitfield mask) {
    ++g_gl_clears;
    if (mask & GL_COLOR_BUFFER_BIT) ++g_gl_color_clears;
}

extern "C" {

void APIENTRY glEnable(GLenum cap) {
    if (!CapValid(cap)) { PUSH_ERROR(GL_INVALID_ENUM); return; }
    SetCap(cap, true);
}

void APIENTRY glDisable(GLenum cap) {
    if (!CapValid(cap)) { PUSH_ERROR(GL_INVALID_ENUM); return; }
    SetCap(cap, false);
}

void APIENTRY glEnablei(GLenum cap, GLuint index) {
    if (index != 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    glEnable(cap);
}

void APIENTRY glDisablei(GLenum cap, GLuint index) {
    if (index != 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    glDisable(cap);
}

GLboolean APIENTRY glIsEnabled(GLenum cap) {
    if (!CapValid(cap)) { PUSH_ERROR(GL_INVALID_ENUM); return GL_FALSE; }
    return s::GetState().caps.Test(cap) ? GL_TRUE : GL_FALSE;
}

GLboolean APIENTRY glIsEnabledi(GLenum cap, GLuint index) {
    if (index != 0) { PUSH_ERROR(GL_INVALID_VALUE); return GL_FALSE; }
    return glIsEnabled(cap);
}

// ---- noise cancellers -------------------------------------------------------

void APIENTRY glFinish() {
    if (!g_dirty_textures.empty()) FlushDirtyTextureUploads();
    v::SubmitFlush(true);
}
void APIENTRY glFlush() {
    if (!g_dirty_textures.empty()) FlushDirtyTextureUploads();
    v::SubmitFlush(false);
}

// ---- viewport / scissor ----------------------------------------------------

void APIENTRY glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    auto& st = s::GetState();
    st.viewport.x = x; st.viewport.y = y;
    st.viewport.w = width; st.viewport.h = height;
    if (v::IsInitialized())
        v::SetViewport((float)x, (float)y, (float)width, (float)height);
}

void APIENTRY glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    auto& st = s::GetState();
    st.scissor.x = x; st.scissor.y = y;
    st.scissor.w = width; st.scissor.h = height;
    if (v::IsInitialized())
        v::SetScissor((float)x, (float)y, (float)width, (float)height);
}

// ---- clear state -----------------------------------------------------------

void APIENTRY glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    auto& st = s::GetState();
    st.clear_color[0] = r; st.clear_color[1] = g;
    st.clear_color[2] = b; st.clear_color[3] = a;
    if (v::IsInitialized())
        v::SetClearColor(r, g, b, a);
}

void APIENTRY glClearDepth(GLdouble depth) { s::GetState().clear_depth = depth; }

void APIENTRY glClearStencil(GLint sval) { s::GetState().clear_stencil = sval; }

void APIENTRY glClear(GLbitfield mask) {
    const GLbitfield valid = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    if (mask & ~valid) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (!mask) return;
    NoteClear(mask);
    v::EnsureInit();
    auto& st = s::GetState();
    v::SetClearColor(st.clear_color[0], st.clear_color[1], st.clear_color[2],
                     st.clear_color[3]);
    v::SetClearDepth(st.clear_depth);
    v::SetClearStencil(st.clear_stencil);
    v::SetClearAttachment(-1);
    v::SetClearMask(mask);
}

// ---- glClearBuffer* (per-attachment clears, GL 3.3) -----------------------

// glClearBuffer* replaces the pending clear wholesale (glClear semantics):
// the specified buffer(s) are cleared to the given value and the previously
// pending clear is discarded. Drawbuffer is validated against the number of
// color attachments / 0 for depth/stencil (GL 3.3 core rules).

void APIENTRY glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
    if (!value) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    auto& st = s::GetState();
    switch (buffer) {
        case GL_COLOR: {
            if (drawbuffer < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
            NoteClear(GL_COLOR_BUFFER_BIT);
            st.clear_color[0] = value[0]; st.clear_color[1] = value[1];
            st.clear_color[2] = value[2]; st.clear_color[3] = value[3];
            v::EnsureInit();
            v::SetClearColor(value[0], value[1], value[2], value[3]);
            v::SetClearAttachment(drawbuffer);
            v::SetClearMask(GL_COLOR_BUFFER_BIT);
            return;
        }
        case GL_DEPTH:
            if (drawbuffer != 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
            NoteClear(GL_DEPTH_BUFFER_BIT);
            st.clear_depth = value[0];
            v::EnsureInit();
            v::SetClearDepth(value[0]);
            v::SetClearAttachment(-1);
            v::SetClearMask(GL_DEPTH_BUFFER_BIT);
            return;
        case GL_STENCIL:
            if (drawbuffer != 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
            NoteClear(GL_STENCIL_BUFFER_BIT);
            st.clear_stencil = (GLint)value[0];
            v::EnsureInit();
            v::SetClearStencil((GLint)value[0]);
            v::SetClearAttachment(-1);
            v::SetClearMask(GL_STENCIL_BUFFER_BIT);
            return;
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value) {
    if (!value) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    auto& st = s::GetState();
    switch (buffer) {
        case GL_COLOR: {
            if (drawbuffer < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
            NoteClear(GL_COLOR_BUFFER_BIT);
            // Integer color formats: mirror the raw ints as RGBA8 floats.
            float f[4];
            for (int i = 0; i < 4; ++i) f[i] = (float)value[i];
            st.clear_color[0] = f[0]; st.clear_color[1] = f[1];
            st.clear_color[2] = f[2]; st.clear_color[3] = f[3];
            v::EnsureInit();
            v::SetClearColor(f[0], f[1], f[2], f[3]);
            v::SetClearAttachment(drawbuffer);
            v::SetClearMask(GL_COLOR_BUFFER_BIT);
            return;
        }
        case GL_STENCIL:
            if (drawbuffer != 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
            NoteClear(GL_STENCIL_BUFFER_BIT);
            st.clear_stencil = value[0];
            v::EnsureInit();
            v::SetClearStencil(value[0]);
            v::SetClearAttachment(-1);
            v::SetClearMask(GL_STENCIL_BUFFER_BIT);
            return;
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {
    if (!value) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    auto& st = s::GetState();
    switch (buffer) {
        case GL_COLOR: {
            if (drawbuffer < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
            NoteClear(GL_COLOR_BUFFER_BIT);
            float f[4];
            for (int i = 0; i < 4; ++i) f[i] = (float)value[i];
            st.clear_color[0] = f[0]; st.clear_color[1] = f[1];
            st.clear_color[2] = f[2]; st.clear_color[3] = f[3];
            v::EnsureInit();
            v::SetClearColor(f[0], f[1], f[2], f[3]);
            v::SetClearAttachment(drawbuffer);
            v::SetClearMask(GL_COLOR_BUFFER_BIT);
            return;
        }
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth,
                              GLint stencil) {
    if (buffer != GL_DEPTH_STENCIL) { PUSH_ERROR(GL_INVALID_ENUM); return; }
    if (drawbuffer != 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    NoteClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    auto& st = s::GetState();
    st.clear_depth = depth;
    st.clear_stencil = stencil;
    v::EnsureInit();
    v::SetClearDepth(depth);
    v::SetClearStencil(stencil);
    v::SetClearAttachment(-1);
    v::SetClearMask(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

// ---- face / polygon --------------------------------------------------------

void APIENTRY glCullFace(GLenum mode) {
    switch (mode) {
        case GL_FRONT: case GL_BACK: case GL_FRONT_AND_BACK:
            s::GetState().cull_face = mode; break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glFrontFace(GLenum mode) {
    switch (mode) {
        case GL_CW: case GL_CCW:
            s::GetState().front_face = mode; break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glPolygonMode(GLenum face, GLenum mode) {
    if (face != GL_FRONT_AND_BACK) { PUSH_ERROR(GL_INVALID_ENUM); return; }
    switch (mode) {
        case GL_POINT: case GL_LINE: case GL_FILL:
            s::GetState().polygon_mode = mode; break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glLineWidth(GLfloat width) {
    if (width > 0.0f) s::GetState().line_width = width;
    else PUSH_ERROR(GL_INVALID_VALUE);
}

void APIENTRY glPointSize(GLfloat size) {
    if (size > 0.0f) s::GetState().point_size = size;
    else PUSH_ERROR(GL_INVALID_VALUE);
}

void APIENTRY glPolygonOffset(GLfloat factor, GLfloat units) {
    auto& st = s::GetState();
    st.poly_offset_factor = factor;
    st.poly_offset_units = units;
}

void APIENTRY glSampleCoverage(GLfloat value, GLboolean invert) {
    if (value < 0.0f || value > 1.0f) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    auto& st = s::GetState();
    st.sample_coverage_value = value;
    st.sample_coverage_invert = invert;
}

void APIENTRY glSampleMaski(GLuint index, GLbitfield mask) {
    if (index >= 32) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    s::GetState().sample_masks[index] = mask;
}

// ---- glGetMultisamplefv (GL 3.3) -------------------------------------------
// Mirrors MobileGL: only GL_SAMPLE_POSITION is answerable on the degraded
// path; every sample of a multisample target sits at the pixel centre.

void APIENTRY glGetMultisamplefv(GLenum pname, GLuint index, GLfloat* val) {
    if (!val) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (pname != GL_SAMPLE_POSITION) { PUSH_ERROR(GL_INVALID_ENUM); return; }
    if (index >= 64) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    val[0] = 0.5f; val[1] = 0.5f;
}

// ---- glPointParameter* (GL 3.3 point rasterization params) ----------------

void APIENTRY glPointParameterf(GLenum pname, GLfloat param) {
    auto& st = s::GetState();
    switch (pname) {
        case GL_POINT_FADE_THRESHOLD_SIZE: st.point_fade_threshold = param; break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glPointParameterfv(GLenum pname, const GLfloat* params) {
    if (!params) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    glPointParameterf(pname, params[0]);
}

void APIENTRY glPointParameteri(GLenum pname, GLint param) {
    auto& st = s::GetState();
    switch (pname) {
        case GL_POINT_SPRITE_COORD_ORIGIN:
            if (param != GL_UPPER_LEFT && param != GL_LOWER_LEFT) {
                PUSH_ERROR(GL_INVALID_VALUE);
                return;
            }
            st.point_sprite_origin = (GLenum)param;
            break;
        case GL_POINT_FADE_THRESHOLD_SIZE:
            glPointParameterf(pname, (GLfloat)param);
            break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glPointParameteriv(GLenum pname, const GLint* params) {
    if (!params) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    glPointParameteri(pname, params[0]);
}

// ---- depth mask/depth func: they act on the GLSL compoe depth test --------

void APIENTRY glDepthFunc(GLenum func) {
    switch (func) {
        case GL_NEVER: case GL_LESS: case GL_EQUAL: case GL_LEQUAL:
        case GL_GREATER: case GL_NOTEQUAL: case GL_GEQUAL: case GL_ALWAYS:
            s::GetState().depth.func = func; break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glDepthMask(GLboolean mask) { s::GetState().depth.mask = mask; }

void APIENTRY glDepthRange(GLdouble n, GLdouble f) {
    auto& st = s::GetState();
    st.depth.range[0] = n; st.depth.range[1] = f;
}

// ---- stencil ---------------------------------------------------------------

static bool StencilOpValid(GLenum op) {
    switch (op) {
        case GL_KEEP: case GL_ZERO: case GL_REPLACE: case GL_INCR:
        case GL_INCR_WRAP: case GL_DECR: case GL_DECR_WRAP: case GL_INVERT:
            return true;
        default:
            return false;
    }
}

void APIENTRY glStencilFunc(GLenum func, GLint ref, GLuint mask) {
    auto& st = s::GetState();
    st.stencil_front.func = func; st.stencil_front.ref = ref; st.stencil_front.mask = mask;
    st.stencil_back = st.stencil_front;
}

void APIENTRY glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) {
    auto& st = s::GetState();
    switch (face) {
        case GL_FRONT: st.stencil_front.func = func; st.stencil_front.ref = ref;
                       st.stencil_front.mask = mask; break;
        case GL_BACK:  st.stencil_back.func = func; st.stencil_back.ref = ref;
                       st.stencil_back.mask = mask; break;
        case GL_FRONT_AND_BACK: st.stencil_front.func = func; st.stencil_front.ref = ref;
                                st.stencil_front.mask = mask;
                                st.stencil_back = st.stencil_front; break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glStencilMask(GLuint mask) {
    auto& st = s::GetState();
    st.stencil_front.write_mask = mask;
    st.stencil_back.write_mask = mask;
}

void APIENTRY glStencilMaskSeparate(GLenum face, GLuint mask) {
    auto& st = s::GetState();
    switch (face) {
        case GL_FRONT: st.stencil_front.write_mask = mask; break;
        case GL_BACK:  st.stencil_back.write_mask = mask; break;
        case GL_FRONT_AND_BACK:
            st.stencil_front.write_mask = mask; st.stencil_back.write_mask = mask; break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) {
    if (!StencilOpValid(fail) || !StencilOpValid(zfail) || !StencilOpValid(zpass)) {
        PUSH_ERROR(GL_INVALID_ENUM); return;
    }
    auto& st = s::GetState();
    st.stencil_front.op_fail = fail; st.stencil_front.op_zfail = zfail; st.stencil_front.op_zpass = zpass;
    st.stencil_back = st.stencil_front;
}

void APIENTRY glStencilOpSeparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass) {
    if (!StencilOpValid(fail) || !StencilOpValid(zfail) || !StencilOpValid(zpass)) {
        PUSH_ERROR(GL_INVALID_ENUM); return;
    }
    auto& st = s::GetState();
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) {
        st.stencil_front.op_fail = fail; st.stencil_front.op_zfail = zfail; st.stencil_front.op_zpass = zpass;
    }
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) {
        st.stencil_back.op_fail = fail; st.stencil_back.op_zfail = zfail; st.stencil_back.op_zpass = zpass;
    }
}

// ---- blend -----------------------------------------------------------------

void APIENTRY glBlendFunc(GLenum src, GLenum dst) {
    auto& st = s::GetState();
    st.blend.src_rgb = st.blend.src_alpha = src;
    st.blend.dst_rgb = st.blend.dst_alpha = dst;
}

void APIENTRY glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
    auto& st = s::GetState();
    st.blend.src_rgb = srcRGB; st.blend.dst_rgb = dstRGB;
    st.blend.src_alpha = srcAlpha; st.blend.dst_alpha = dstAlpha;
}

void APIENTRY glBlendEquation(GLenum mode) {
    auto& st = s::GetState();
    st.blend.eq_rgb = st.blend.eq_alpha = mode;
}

void APIENTRY glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {
    auto& st = s::GetState();
    st.blend.eq_rgb = modeRGB; st.blend.eq_alpha = modeAlpha;
}

void APIENTRY glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    auto& st = s::GetState();
    st.blend.color[0] = r; st.blend.color[1] = g;
    st.blend.color[2] = b; st.blend.color[3] = a;
}

// ---- color mask ---------------------------------------------------------------

void APIENTRY glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    auto& st = s::GetState();
    st.color_wmask[0] = r;
    st.color_wmask[1] = g;
    st.color_wmask[2] = b;
    st.color_wmask[3] = a;
}

void APIENTRY glColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    if (index != 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    glColorMask(r, g, b, a);
}

// ---- logic op / hint / pixel store -----------------------------------------

void APIENTRY glLogicOp(GLenum op) {
    switch (op) {
        case GL_CLEAR: case GL_AND: case GL_AND_REVERSE: case GL_COPY:
        case GL_AND_INVERTED: case GL_NOOP: case GL_XOR: case GL_OR:
        case GL_NOR: case GL_EQUIV: case GL_INVERT: case GL_OR_REVERSE:
        case GL_COPY_INVERTED: case GL_OR_INVERTED: case GL_NAND: case GL_SET:
            s::GetState().logicop = op; break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glHint(GLenum target, GLenum mode) {
    if (target == GL_FRAGMENT_SHADER_DERIVATIVE_HINT) {
        s::GetState().hint_derivative = mode;
    } else {
        PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glPixelStorei(GLenum pname, GLint param) {
    auto& st = s::GetState();
    switch (pname) {
        case GL_PACK_ALIGNMENT: case GL_UNPACK_ALIGNMENT:
            if (param != 1 && param != 2 && param != 4 && param != 8) {
                PUSH_ERROR(GL_INVALID_VALUE); return;
            }
            if (pname == GL_PACK_ALIGNMENT) st.pixels.pack_alignment = param;
            else st.pixels.unpack_alignment = param;
            break;
        case GL_PACK_ROW_LENGTH:  st.pixels.pack_row_length = param; break;
        case GL_UNPACK_ROW_LENGTH: st.pixels.unpack_row_length = param; break;
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glClampColor(GLenum target, GLenum clamp) {
    if (target != GL_CLAMP_READ_COLOR) {
        PUSH_ERROR(GL_INVALID_ENUM); return;
    }
    if (clamp != GL_TRUE && clamp != GL_FALSE && clamp != GL_FIXED_ONLY) {
        PUSH_ERROR(GL_INVALID_ENUM); return;
    }
    s::GetState().clamp_color_mode = clamp;
}

GLenum APIENTRY glGetError() { return s::GetState().errors.Pop(); }

const GLubyte* APIENTRY glGetString(GLenum name) {
    switch (name) {
        case GL_VENDOR:   return reinterpret_cast<const GLubyte*>("Mithril-Wrapper");
        case GL_RENDERER: return reinterpret_cast<const GLubyte*>("Vulkan on Metal (MoltenVK)");
        case GL_VERSION:  return reinterpret_cast<const GLubyte*>("3.3 Core Profile Mithril");
        case GL_SHADING_LANGUAGE_VERSION:
                          return reinterpret_cast<const GLubyte*>("3.30 Mithril");
        case GL_EXTENSIONS: return reinterpret_cast<const GLubyte*>("");
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
            return nullptr;
    }
}

const GLubyte* APIENTRY glGetStringi(GLenum name, GLuint index) {
    if (name != GL_EXTENSIONS) { PUSH_ERROR(GL_INVALID_ENUM); return nullptr; }
    (void)index;
    // No extensions registered in M1; any index is out of range.
    PUSH_ERROR(GL_INVALID_VALUE);
    return nullptr;
}

void APIENTRY glGetBooleanv(GLenum pname, GLboolean* data) {
    if (!data) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    const auto& st = s::GetState();
    switch (pname) {
        case GL_DEPTH_TEST:     *data = st.caps.Test(GL_DEPTH_TEST) ? GL_TRUE : GL_FALSE; break;
        case GL_STENCIL_TEST:   *data = st.caps.Test(GL_STENCIL_TEST) ? GL_TRUE : GL_FALSE; break;
        case GL_BLEND:          *data = st.caps.Test(GL_BLEND) ? GL_TRUE : GL_FALSE; break;
        case GL_CULL_FACE:      *data = st.caps.Test(GL_CULL_FACE) ? GL_TRUE : GL_FALSE; break;
        case GL_SCISSOR_TEST:   *data = st.caps.Test(GL_SCISSOR_TEST) ? GL_TRUE : GL_FALSE; break;
        case GL_MULTISAMPLE:    *data = st.caps.Test(GL_MULTISAMPLE) ? GL_TRUE : GL_FALSE; break;
        case GL_DITHER:         *data = st.caps.Test(GL_DITHER) ? GL_TRUE : GL_FALSE; break;
        case GL_RASTERIZER_DISCARD: *data = st.caps.Test(GL_RASTERIZER_DISCARD) ? GL_TRUE : GL_FALSE; break;
        case GL_SAMPLE_COVERAGE: *data = st.caps.Test(GL_SAMPLE_COVERAGE) ? GL_TRUE : GL_FALSE; break;
        case GL_POLYGON_OFFSET_FILL: *data = st.caps.Test(GL_POLYGON_OFFSET_FILL) ? GL_TRUE : GL_FALSE; break;
        case GL_LOGIC_OP_MODE:       *data = st.caps.Test(GL_LOGIC_OP_MODE) ? GL_TRUE : GL_FALSE; break;
        case GL_DEPTH_WRITEMASK: *data = st.depth.mask; break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glGetFloatv(GLenum pname, GLfloat* data) {
    if (!data) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    const auto& st = s::GetState();
    switch (pname) {
        case GL_LINE_WIDTH:  *data = st.line_width; break;
        case GL_POINT_SIZE:  *data = st.point_size; break;
        case GL_POINT_FADE_THRESHOLD_SIZE: *data = st.point_fade_threshold; break;
        case GL_VIEWPORT:
            data[0] = (GLfloat)st.viewport.x; data[1] = (GLfloat)st.viewport.y;
            data[2] = (GLfloat)st.viewport.w; data[3] = (GLfloat)st.viewport.h;
            break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glGetIntegerv(GLenum pname, GLint* data) {
    if (!data) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    const auto& st = s::GetState();
    switch (pname) {
        case GL_MAX_TEXTURE_SIZE: *data = 16384; break;
        case GL_MAX_3D_TEXTURE_SIZE: *data = 2048; break;
        case GL_MAX_CUBE_MAP_TEXTURE_SIZE: *data = 16384; break;
        case GL_MAX_ARRAY_TEXTURE_LAYERS: *data = 2048; break;
        case GL_MAX_VIEWPORT_DIMS:
            data[0] = 16384; data[1] = 16384; break;
        case GL_VIEWPORT:
            data[0] = st.viewport.x; data[1] = st.viewport.y;
            data[2] = st.viewport.w; data[3] = st.viewport.h;
            break;
        case GL_SCISSOR_BOX:
            data[0] = st.scissor.x; data[1] = st.scissor.y;
            data[2] = st.scissor.w; data[3] = st.scissor.h;
            break;
        case GL_NUM_EXTENSIONS: *data = 0; break;
        case GL_MAJOR_VERSION: *data = 3; break;
        case GL_MINOR_VERSION: *data = 3; break;
        case GL_CONTEXT_PROFILE_MASK: *data = GL_CONTEXT_CORE_PROFILE_BIT; break;
        case GL_SAMPLE_MASK: *data = st.sample_masks[0] ? 1 : 0; break;
        case GL_POINT_SPRITE_COORD_ORIGIN: *data = st.point_sprite_origin; break;
        case GL_MAX_SAMPLES: *data = 64; break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glGetInteger64v(GLenum pname, GLint64* data) {
    if (!data) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (pname == GL_MAJOR_VERSION) *data = 3;
    else if (pname == GL_MINOR_VERSION) *data = 3;
    else PUSH_ERROR(GL_INVALID_ENUM);
}

void APIENTRY glGetDoublev(GLenum pname, GLdouble* data) {
    if (!data) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    const auto& st = s::GetState();
    switch (pname) {
        case GL_DEPTH_RANGE: data[0] = st.depth.range[0]; data[1] = st.depth.range[1]; break;
        case GL_DEPTH_CLEAR_VALUE: *data = st.clear_depth; break;
        case GL_COLOR_CLEAR_VALUE:
            data[0] = st.clear_color[0]; data[1] = st.clear_color[1];
            data[2] = st.clear_color[2]; data[3] = st.clear_color[3];
            break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glGetPointerv(GLenum pname, void** params) {
    (void)pname;
    (void)params;
    // All pointer queries are removed from the core profile; error out.
    PUSH_ERROR(GL_INVALID_ENUM);
}

} // extern "C"
