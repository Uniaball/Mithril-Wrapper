// Mithril-Wrapper GL entry points -- S6 sampler objects (milestone M6 stage E).
// glGenSamplers..glGetSamplerParameter* family. Sampler object table (name
// pool + CPU SamplerState mirror), per-texture-unit binding table
// (g_sampler_units), and the dirty-set + FlushDirtySamplerUploads replay
// mechanism that mirrors texture.cpp's g_dirty_textures path. Each sampler
// object owns its own resident VkSampler via v::UpdateSampler; the draw path
// pairs it with the bound texture's image view, falling back to the texture's
// baked sampler when no sampler object is bound (sampler_id == 0).
//
// Semantics follow MobileGL MG_Impl/GLImpl/Sampler/GL_Sampler.cpp: glGenSamplers
// creates the object immediately (glIsSampler is GL_TRUE before any bind);
// glBindSampler validates the unit range and sampler name; glSamplerParameter*
// dispatch through one shared setter; glGetSamplerParameter* through one
// shared getter.

#include "internal.h"

#include <cstdint>

#include <util/log.h>

namespace {

// Project the CPU-side SamplerState into the engine's TexSamplerInfo, the
// same shape texture.cpp's ToSamplerInfo produces for TexState.
v::TexSamplerInfo ToSamplerInfo(const SamplerState& st) {
    v::TexSamplerInfo si;
    si.mag = st.mag_filter == GL_NEAREST ? v::TexFilter::Nearest
                                         : v::TexFilter::Linear;
    bool mip = st.min_filter == GL_NEAREST_MIPMAP_NEAREST ||
               st.min_filter == GL_NEAREST_MIPMAP_LINEAR ||
               st.min_filter == GL_LINEAR_MIPMAP_NEAREST ||
               st.min_filter == GL_LINEAR_MIPMAP_LINEAR;
    si.mip = mip;
    si.min = (st.min_filter == GL_NEAREST ||
              st.min_filter == GL_NEAREST_MIPMAP_NEAREST)
                 ? v::TexFilter::Nearest
                 : v::TexFilter::Linear;
    si.wrap_s = st.wrap_s;
    si.wrap_t = st.wrap_t;
    si.wrap_r = st.wrap_r;
    return si;
}

// Mark `id` as needing a (re)upload and try it immediately when the backend
// is already up. Updates made before vk::EnsureInit are replayed by
// FlushDirtySamplerUploads at the first draw.
void MarkSamplerDirty(GLuint id) {
    g_dirty_samplers.insert(id);
    if (v::IsInitialized()) FlushDirtySamplerUploads();
}

} // namespace

void FlushDirtySamplerUploads() {
    std::vector<GLuint> ids(g_dirty_samplers.begin(), g_dirty_samplers.end());
    for (GLuint id : ids) {
        auto it = g_samplers.find(id);
        if (it == g_samplers.end()) { g_dirty_samplers.erase(id); continue; }
        // UpdateSampler is a no-op until the backend exists; ids touched
        // while !IsInitialized() stay dirty for the flush at the first draw.
        v::UpdateSampler(id, ToSamplerInfo(it->second));
        if (v::IsInitialized()) g_dirty_samplers.erase(id);
    }
}

// ---- shared table storage (declared in internal.h) ------------------------

std::unordered_map<GLuint, SamplerState> g_samplers;
std::array<GLuint, kMaxTexUnits> g_sampler_units{};
GLuint g_next_sampler = 1;
std::unordered_set<GLuint> g_dirty_samplers;

extern "C" {

// ---- sampler objects ------------------------------------------------------

void APIENTRY glGenSamplers(GLsizei n, GLuint* samplers) {
    if (n < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (!samplers || n == 0) return;
    for (GLsizei i = 0; i < n; ++i) {
        while (g_samplers.count(g_next_sampler)) ++g_next_sampler;
        samplers[i] = g_next_sampler++;
        // glGenSamplers creates the object immediately: glIsSampler is
        // GL_TRUE before any bind (matches MobileGL GenSamplers_State).
        g_samplers.emplace(samplers[i], SamplerState{});
    }
}

void APIENTRY glDeleteSamplers(GLsizei n, const GLuint* samplers) {
    if (n < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (!samplers) return;
    for (GLsizei i = 0; i < n; ++i) {
        GLuint id = samplers[i];
        if (id == 0) continue;
        // Unbind this sampler from any texture unit it is bound to.
        for (auto& u : g_sampler_units)
            if (u == id) u = 0;
        g_dirty_samplers.erase(id);
        g_samplers.erase(id);
        v::DestroyResidentSampler(id);
    }
}

GLboolean APIENTRY glIsSampler(GLuint sampler) {
    return g_samplers.count(sampler) ? GL_TRUE : GL_FALSE;
}

void APIENTRY glBindSampler(GLuint unit, GLuint sampler) {
    if (unit >= kMaxTexUnits) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (sampler != 0 && !g_samplers.count(sampler)) {
        PUSH_ERROR(GL_INVALID_OPERATION);   // not a generated name
        return;
    }
    g_sampler_units[unit] = sampler;
}

// ---- sampler parameters ---------------------------------------------------

// Shared setter for the six glSamplerParameter* entry points. Validates the
// sampler name (GL_INVALID_OPERATION) and the pname (GL_INVALID_ENUM), mutates
// the SamplerState, then marks the sampler dirty so the engine VkSampler is
// (re)created on the next flush (or immediately if the backend is up).
static void SetSamplerParam(GLuint sampler, GLenum pname, GLint v) {
    if (!g_samplers.count(sampler)) {
        PUSH_ERROR(GL_INVALID_OPERATION);
        return;
    }
    SamplerState& st = g_samplers[sampler];
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: st.min_filter = (GLenum)v; break;
        case GL_TEXTURE_MAG_FILTER: st.mag_filter = (GLenum)v; break;
        case GL_TEXTURE_WRAP_S: st.wrap_s = (GLenum)v; break;
        case GL_TEXTURE_WRAP_T: st.wrap_t = (GLenum)v; break;
        case GL_TEXTURE_WRAP_R: st.wrap_r = (GLenum)v; break;
        case GL_TEXTURE_MIN_LOD:
        case GL_TEXTURE_MAX_LOD:
        case GL_TEXTURE_LOD_BIAS:
        case GL_TEXTURE_BASE_LEVEL:
        case GL_TEXTURE_MAX_LEVEL:
            break;   // accepted; sampler objects don't honour LOD bias / level
                     // clamps beyond what TexSamplerInfo already encodes.
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
            return;
    }
    MarkSamplerDirty(sampler);
}

void APIENTRY glSamplerParameteri(GLuint sampler, GLenum pname, GLint param) {
    SetSamplerParam(sampler, pname, param);
}

void APIENTRY glSamplerParameteriv(GLuint sampler, GLenum pname,
                                   const GLint* param) {
    if (!param) return;
    SetSamplerParam(sampler, pname, param[0]);
}

void APIENTRY glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param) {
    SetSamplerParam(sampler, pname, (GLint)param);
}

void APIENTRY glSamplerParameterfv(GLuint sampler, GLenum pname,
                                    const GLfloat* param) {
    if (!param) return;
    SetSamplerParam(sampler, pname, (GLint)param[0]);
}

void APIENTRY glSamplerParameterIiv(GLuint sampler, GLenum pname,
                                     const GLint* param) {
    if (!param) return;
    SetSamplerParam(sampler, pname, param[0]);
}

void APIENTRY glSamplerParameterIuiv(GLuint sampler, GLenum pname,
                                      const GLuint* param) {
    if (!param) return;
    SetSamplerParam(sampler, pname, (GLint)param[0]);
}

// ---- sampler queries -----------------------------------------------------

// Shared getter for the four glGetSamplerParameter* entry points. Mirrors
// glGetTexParameteriv's accepted pnames and defaults (MIN_LOD=0, MAX_LOD=1000).
static void GetSamplerParam(GLuint sampler, GLenum pname, GLint* out) {
    if (!g_samplers.count(sampler)) {
        PUSH_ERROR(GL_INVALID_OPERATION);
        return;
    }
    if (!out) return;
    SamplerState& st = g_samplers[sampler];
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: *out = (GLint)st.min_filter; break;
        case GL_TEXTURE_MAG_FILTER: *out = (GLint)st.mag_filter; break;
        case GL_TEXTURE_WRAP_S: *out = (GLint)st.wrap_s; break;
        case GL_TEXTURE_WRAP_T: *out = (GLint)st.wrap_t; break;
        case GL_TEXTURE_WRAP_R: *out = (GLint)st.wrap_r; break;
        case GL_TEXTURE_MIN_LOD: *out = 0; break;
        case GL_TEXTURE_MAX_LOD: *out = 1000; break;
        case GL_TEXTURE_LOD_BIAS: *out = 0; break;
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
            return;
    }
}

void APIENTRY glGetSamplerParameteriv(GLuint sampler, GLenum pname,
                                       GLint* params) {
    GetSamplerParam(sampler, pname, params);
}

void APIENTRY glGetSamplerParameterfv(GLuint sampler, GLenum pname,
                                       GLfloat* params) {
    GLint v = 0;
    GetSamplerParam(sampler, pname, &v);
    if (params) *params = (GLfloat)v;
}

void APIENTRY glGetSamplerParameterIiv(GLuint sampler, GLenum pname,
                                        GLint* params) {
    GetSamplerParam(sampler, pname, params);
}

void APIENTRY glGetSamplerParameterIuiv(GLuint sampler, GLenum pname,
                                         GLuint* params) {
    GLint v = 0;
    GetSamplerParam(sampler, pname, &v);
    if (params) *params = (GLuint)v;
}

} // extern "C"
