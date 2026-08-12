// Mithril-Wrapper GL entry points -- S3 vertex/buffer domain (M2-VK, M3).
// glGen/Bind/DeleteVertexArrays, glGen/Bind/DeleteBuffers, buffer uploads,
// mapping and queries, glVertexAttribPointer/Divisor/constant 1-4s and the
// attribute getters. Owns the shared VAO/VBO name tables (internal.h).

#include "internal.h"

#include <algorithm>
#include <cstring>
#include <utility>

// Shared tables (declared extern in internal.h; the draw path reads
// them through the header).
std::unordered_map<GLuint, VAOData> g_vaos;
std::unordered_map<GLuint, BufferData> g_buffers;
GLuint g_next_vao = 1, g_next_buffer = 1;
GLuint g_bound_vao = 0;
GLuint g_bound_array_buffer = 0;
GLuint g_bound_element_buffer = 0;

// program id -> Vulkan program handle (created lazily on first draw by the
// draw path; erased by the shader-lifecycle path on glDeleteProgram).
std::unordered_map<GLuint, uint64_t> g_vk_programs;

// Stage F (S3): transform feedback CPU counting state.
bool g_tfb_active = false;
uint64_t g_tfb_primitives = 0;
std::unordered_map<GLenum, std::array<IndexedBinding, kMaxIndexedBindings>>
    g_indexed_bindings;

extern "C" {

// ---- vertex arrays / buffers / draw (milestone M2-VK) -----------------------

namespace {
GLuint NewName(std::unordered_map<GLuint, VAOData>& table, GLuint& next) {
    while (table.count(next)) ++next;
    table.emplace(next, VAOData{});
    return next++;
}

} // namespace

void APIENTRY glGenVertexArrays(GLsizei n, GLuint* arrays) {
    if (n < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; ++i) arrays[i] = NewName(g_vaos, g_next_vao);
}

void APIENTRY glDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
    if (n < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; ++i) {
        auto it = g_vaos.find(arrays[i]);
        if (it == g_vaos.end()) continue;
        if (g_bound_vao == arrays[i]) g_bound_vao = 0;
        g_vaos.erase(it);
    }
}

void APIENTRY glBindVertexArray(GLuint array) {
    if (array != 0 && !g_vaos.count(array)) { PUSH_ERROR(GL_INVALID_OPERATION); return; }
    g_bound_vao = array;
}

GLboolean APIENTRY glIsVertexArray(GLuint array) {
    return g_vaos.count(array) ? GL_TRUE : GL_FALSE;
}

void APIENTRY glGenBuffers(GLsizei n, GLuint* buffers) {
    if (n < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; ++i) {
        while (g_buffers.count(g_next_buffer)) ++g_next_buffer;
        buffers[i] = g_next_buffer++;
        g_buffers.emplace(buffers[i], BufferData{});
    }
}

void APIENTRY glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    if (n < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; ++i) {
        auto it = g_buffers.find(buffers[i]);
        if (it == g_buffers.end()) continue;
        if (g_bound_array_buffer == buffers[i]) g_bound_array_buffer = 0;
        if (g_bound_element_buffer == buffers[i]) g_bound_element_buffer = 0;
        g_buffers.erase(it);
    }
}

GLboolean APIENTRY glIsBuffer(GLuint buffer) {
    return g_buffers.count(buffer) ? GL_TRUE : GL_FALSE;
}

void APIENTRY glBindBuffer(GLenum target, GLuint buffer) {
    if (buffer != 0 && !g_buffers.count(buffer)) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    switch (target) {
        case GL_ARRAY_BUFFER: g_bound_array_buffer = buffer; break;
        case GL_ELEMENT_ARRAY_BUFFER: g_bound_element_buffer = buffer; break;
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
            return;
    }
}

void APIENTRY glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    (void)usage;
    GLuint* bound = nullptr;
    switch (target) {
        case GL_ARRAY_BUFFER: bound = &g_bound_array_buffer; break;
        case GL_ELEMENT_ARRAY_BUFFER: bound = &g_bound_element_buffer; break;
        default: PUSH_ERROR(GL_INVALID_ENUM); return;
    }
    if (*bound == 0) { PUSH_ERROR(GL_INVALID_OPERATION); return; }
    if (size < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    auto it = g_buffers.find(*bound);
    if (data) {
        it->second.data.assign((const uint8_t*)data, (const uint8_t*)data + size);
    } else {
        it->second.data.assign((size_t)size, 0);
    }
}

void APIENTRY glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    GLuint* bound = nullptr;
    switch (target) {
        case GL_ARRAY_BUFFER: bound = &g_bound_array_buffer; break;
        case GL_ELEMENT_ARRAY_BUFFER: bound = &g_bound_element_buffer; break;
        default: PUSH_ERROR(GL_INVALID_ENUM); return;
    }
    if (*bound == 0) { PUSH_ERROR(GL_INVALID_OPERATION); return; }
    if (offset < 0 || size < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    auto it = g_buffers.find(*bound);
    if (offset + size > (GLintptr)it->second.data.size()) {
        PUSH_ERROR(GL_INVALID_VALUE);
        return;
    }
    std::memcpy(it->second.data.data() + offset, data, size);
}

// ---- buffer queries / mapping (M3) -----------------------------------------

BufferData* BoundBufferForTarget(GLenum target, GLenum* error) {
    GLuint* bound = nullptr;
    switch (target) {
        case GL_ARRAY_BUFFER: bound = &g_bound_array_buffer; break;
        case GL_ELEMENT_ARRAY_BUFFER: bound = &g_bound_element_buffer; break;
        default: *error = GL_INVALID_ENUM; return nullptr;
    }
    if (*bound == 0) { *error = GL_INVALID_OPERATION; return nullptr; }
    auto it = g_buffers.find(*bound);
    if (it == g_buffers.end()) { *error = GL_INVALID_OPERATION; return nullptr; }
    return &it->second;
}

void APIENTRY glCopyBufferSubData(GLenum readtarget, GLenum writetarget,
                                  GLintptr readoffset, GLintptr writeoffset,
                                  GLsizeiptr size) {
    GLenum err = GL_NO_ERROR;
    BufferData* src = BoundBufferForTarget(readtarget, &err);
    if (err) { PUSH_ERROR(err); return; }
    err = GL_NO_ERROR;
    BufferData* dst = BoundBufferForTarget(writetarget, &err);
    if (err) { PUSH_ERROR(err); return; }
    if (readoffset < 0 || writeoffset < 0 || size < 0 ||
        readoffset + size > (GLintptr)src->data.size() ||
        writeoffset + size > (GLintptr)dst->data.size()) {
        PUSH_ERROR(GL_INVALID_VALUE);
        return;
    }
    std::memmove(dst->data.data() + writeoffset, src->data.data() + readoffset,
                 size);
}

void APIENTRY glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params) {
    GLenum err = GL_NO_ERROR;
    BufferData* b = BoundBufferForTarget(target, &err);
    if (err) { PUSH_ERROR(err); return; }
    switch (pname) {
        case GL_BUFFER_SIZE: *params = (GLint)b->data.size(); break;
        case GL_BUFFER_USAGE: *params = GL_STATIC_DRAW; break;
        case GL_BUFFER_ACCESS: *params = GL_WRITE_ONLY; break;
        case GL_BUFFER_MAPPED: *params = GL_FALSE; break;
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glGetBufferParameteri64v(GLenum target, GLenum pname, GLint64* params) {
    switch (pname) {
        case GL_BUFFER_SIZE: {
            GLenum err = GL_NO_ERROR;
            BufferData* b = BoundBufferForTarget(target, &err);
            if (err) { PUSH_ERROR(err); return; }
            *params = (GLint64)b->data.size();
            break;
        }
        default: PUSH_ERROR(GL_INVALID_ENUM);
    }
}

void APIENTRY glGetBufferPointerv(GLenum target, GLenum pname, void** params) {
    if (pname != GL_BUFFER_MAP_POINTER) { PUSH_ERROR(GL_INVALID_ENUM); return; }
    GLenum err = GL_NO_ERROR;
    BufferData* b = BoundBufferForTarget(target, &err);
    if (err) { PUSH_ERROR(err); return; }
    *params = b->data.data();
}

void APIENTRY glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                                 void* data) {
    GLenum err = GL_NO_ERROR;
    BufferData* b = BoundBufferForTarget(target, &err);
    if (err) { PUSH_ERROR(err); return; }
    if (offset < 0 || size < 0 || offset + size > (GLintptr)b->data.size()) {
        PUSH_ERROR(GL_INVALID_VALUE);
        return;
    }
    std::memcpy(data, b->data.data() + offset, size);
}

void* APIENTRY glMapBuffer(GLenum target, GLenum access) {
    if (access != GL_READ_WRITE && access != GL_WRITE_ONLY && access != GL_READ_ONLY) {
        PUSH_ERROR(GL_INVALID_ENUM);
        return nullptr;
    }
    GLenum err = GL_NO_ERROR;
    BufferData* b = BoundBufferForTarget(target, &err);
    if (err) { PUSH_ERROR(err); return nullptr; }
    if (b->data.empty()) { PUSH_ERROR(GL_OUT_OF_MEMORY); return nullptr; }
    return b->data.data();
}

void* APIENTRY glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length,
                                GLbitfield access) {
    (void)access;
    if (offset < 0 || length < 0) { PUSH_ERROR(GL_INVALID_VALUE); return nullptr; }
    GLenum err = GL_NO_ERROR;
    BufferData* b = BoundBufferForTarget(target, &err);
    if (err) { PUSH_ERROR(err); return nullptr; }
    if (offset + length > (GLintptr)b->data.size()) { PUSH_ERROR(GL_INVALID_VALUE); return nullptr; }
    return b->data.data() + offset;
}

GLboolean APIENTRY glUnmapBuffer(GLenum target) {
    GLenum err = GL_NO_ERROR;
    BoundBufferForTarget(target, &err);
    if (err) { PUSH_ERROR(err); return GL_FALSE; }
    return GL_TRUE;  // host-coherent staging: nothing to flush
}

void APIENTRY glFlushMappedBufferRange(GLenum target, GLintptr offset,
                                       GLsizeiptr length) {
    (void)target; (void)offset; (void)length;
}

void APIENTRY glEnableVertexAttribArray(GLuint index) {
    if (index >= kMaxAttribs) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    g_vaos[g_bound_vao].attribs[index].enabled = true;
}

void APIENTRY glDisableVertexAttribArray(GLuint index) {
    if (index >= kMaxAttribs) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    g_vaos[g_bound_vao].attribs[index].enabled = false;
}

void APIENTRY glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                    GLboolean normalized, GLsizei stride,
                                    const void* pointer) {
    if (index >= kMaxAttribs) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (size < 1 || size > 4) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (type != GL_FLOAT && type != GL_HALF_FLOAT && type != GL_DOUBLE &&
        type != GL_BYTE && type != GL_UNSIGNED_BYTE && type != GL_SHORT &&
        type != GL_UNSIGNED_SHORT && type != GL_INT && type != GL_UNSIGNED_INT) {
        PUSH_ERROR(GL_INVALID_ENUM);
        return;
    }
    if (stride < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    AttribData& a = g_vaos[g_bound_vao].attribs[index];
    a.enabled = true;
    a.size = size;
    a.type = type;
    a.normalized = normalized;
    a.stride = stride;
    a.offset = (GLsizeiptr)pointer;
    a.buffer = g_bound_array_buffer;
    a.is_pointer = true;
}

void APIENTRY glVertexAttribIPointer(GLuint index, GLint size, GLenum type,
                                     GLsizei stride, const void* pointer) {
    if (index >= kMaxAttribs) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (size < 1 || size > 4) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (type != GL_BYTE && type != GL_UNSIGNED_BYTE && type != GL_SHORT &&
        type != GL_UNSIGNED_SHORT && type != GL_INT && type != GL_UNSIGNED_INT) {
        PUSH_ERROR(GL_INVALID_ENUM);
        return;
    }
    if (stride < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    AttribData& a = g_vaos[g_bound_vao].attribs[index];
    a.enabled = true;
    a.size = size;
    a.type = type;
    a.normalized = GL_FALSE;
    a.stride = stride;
    a.offset = (GLsizeiptr)pointer;
    a.buffer = g_bound_array_buffer;
    a.is_pointer = true;
}

void APIENTRY glVertexAttribDivisor(GLuint index, GLuint divisor) {
    if (index >= kMaxAttribs) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    g_vaos[g_bound_vao].attribs[index].divisor = divisor;
}

// ---- generic (constant) vertex attributes -----------------------------------

// Constant values apply when the array is *disabled*; setting them must not
// change the enable bit (GL 4.46).
void SetConstantAttrib(GLuint index, const GLfloat* v, GLsizei n) {
    if (index >= kMaxAttribs || n < 1 || n > 4) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    AttribData& a = g_vaos[g_bound_vao].attribs[index];
    a.is_pointer = false;
    for (GLsizei i = 0; i < n; ++i) a.constant[i] = v[i];
    for (GLsizei i = n; i < 4; ++i) a.constant[i] = i == 3 ? 1.0f : 0.0f;
}

void APIENTRY glVertexAttrib1f(GLuint index, GLfloat x) {
    const GLfloat v[1] = {x}; SetConstantAttrib(index, v, 1); }
void APIENTRY glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y) {
    const GLfloat v[2] = {x, y}; SetConstantAttrib(index, v, 2); }
void APIENTRY glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z) {
    const GLfloat v[3] = {x, y, z}; SetConstantAttrib(index, v, 3); }
void APIENTRY glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    const GLfloat v[4] = {x, y, z, w}; SetConstantAttrib(index, v, 4); }
void APIENTRY glVertexAttrib1fv(GLuint index, const GLfloat* v) { SetConstantAttrib(index, v, 1); }
void APIENTRY glVertexAttrib2fv(GLuint index, const GLfloat* v) { SetConstantAttrib(index, v, 2); }
void APIENTRY glVertexAttrib3fv(GLuint index, const GLfloat* v) { SetConstantAttrib(index, v, 3); }
void APIENTRY glVertexAttrib4fv(GLuint index, const GLfloat* v) { SetConstantAttrib(index, v, 4); }

void APIENTRY glVertexAttrib1d(GLuint index, GLdouble x) { GLfloat f[1]; f[0]=(GLfloat)x; SetConstantAttrib(index, f, 1); }
void APIENTRY glVertexAttrib2d(GLuint index, GLdouble x, GLdouble y) { GLfloat f[2]; f[0]=(GLfloat)x; f[1]=(GLfloat)y; SetConstantAttrib(index, f, 2); }
void APIENTRY glVertexAttrib3d(GLuint index, GLdouble x, GLdouble y, GLdouble z) { GLfloat f[3]; f[0]=(GLfloat)x; f[1]=(GLfloat)y; f[2]=(GLfloat)z; SetConstantAttrib(index, f, 3); }
void APIENTRY glVertexAttrib4d(GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w) { GLfloat f[4]; f[0]=(GLfloat)x; f[1]=(GLfloat)y; f[2]=(GLfloat)z; f[3]=(GLfloat)w; SetConstantAttrib(index, f, 4); }
void APIENTRY glVertexAttrib1dv(GLuint index, const GLdouble* v) { GLfloat f[1]; f[0]=(GLfloat)v[0]; SetConstantAttrib(index, f, 1); }
void APIENTRY glVertexAttrib2dv(GLuint index, const GLdouble* v) { GLfloat f[2]; f[0]=(GLfloat)v[0]; f[1]=(GLfloat)v[1]; SetConstantAttrib(index, f, 2); }
void APIENTRY glVertexAttrib3dv(GLuint index, const GLdouble* v) { GLfloat f[3]; f[0]=(GLfloat)v[0]; f[1]=(GLfloat)v[1]; f[2]=(GLfloat)v[2]; SetConstantAttrib(index, f, 3); }
void APIENTRY glVertexAttrib4dv(GLuint index, const GLdouble* v) { GLfloat f[4]; f[0]=(GLfloat)v[0]; f[1]=(GLfloat)v[1]; f[2]=(GLfloat)v[2]; f[3]=(GLfloat)v[3]; SetConstantAttrib(index, f, 4); }

void APIENTRY glVertexAttrib1s(GLuint index, GLshort x) { GLfloat f[1]; f[0]=(GLfloat)x; SetConstantAttrib(index, f, 1); }
void APIENTRY glVertexAttrib2s(GLuint index, GLshort x, GLshort y) { GLfloat f[2]; f[0]=(GLfloat)x; f[1]=(GLfloat)y; SetConstantAttrib(index, f, 2); }
void APIENTRY glVertexAttrib3s(GLuint index, GLshort x, GLshort y, GLshort z) { GLfloat f[3]; f[0]=(GLfloat)x; f[1]=(GLfloat)y; f[2]=(GLfloat)z; SetConstantAttrib(index, f, 3); }
void APIENTRY glVertexAttrib4s(GLuint index, GLshort x, GLshort y, GLshort z, GLshort w) { GLfloat f[4]; f[0]=(GLfloat)x; f[1]=(GLfloat)y; f[2]=(GLfloat)z; f[3]=(GLfloat)w; SetConstantAttrib(index, f, 4); }
void APIENTRY glVertexAttrib1sv(GLuint index, const GLshort* v) { GLfloat f[1]; f[0]=(GLfloat)v[0]; SetConstantAttrib(index, f, 1); }
void APIENTRY glVertexAttrib2sv(GLuint index, const GLshort* v) { GLfloat f[2]; f[0]=(GLfloat)v[0]; f[1]=(GLfloat)v[1]; SetConstantAttrib(index, f, 2); }
void APIENTRY glVertexAttrib3sv(GLuint index, const GLshort* v) { GLfloat f[3]; f[0]=(GLfloat)v[0]; f[1]=(GLfloat)v[1]; f[2]=(GLfloat)v[2]; SetConstantAttrib(index, f, 3); }
void APIENTRY glVertexAttrib4sv(GLuint index, const GLshort* v) { GLfloat f[4]; f[0]=(GLfloat)v[0]; f[1]=(GLfloat)v[1]; f[2]=(GLfloat)v[2]; f[3]=(GLfloat)v[3]; SetConstantAttrib(index, f, 4); }

void APIENTRY glVertexAttrib1i(GLuint index, GLint x) { GLfloat f[1]; f[0]=(GLfloat)x; SetConstantAttrib(index, f, 1); }
void APIENTRY glVertexAttrib2i(GLuint index, GLint x, GLint y) { GLfloat f[2]; f[0]=(GLfloat)x; f[1]=(GLfloat)y; SetConstantAttrib(index, f, 2); }
void APIENTRY glVertexAttrib3i(GLuint index, GLint x, GLint y, GLint z) { GLfloat f[3]; f[0]=(GLfloat)x; f[1]=(GLfloat)y; f[2]=(GLfloat)z; SetConstantAttrib(index, f, 3); }
void APIENTRY glVertexAttrib4i(GLuint index, GLint x, GLint y, GLint z, GLint w) { GLfloat f[4]; f[0]=(GLfloat)x; f[1]=(GLfloat)y; f[2]=(GLfloat)z; f[3]=(GLfloat)w; SetConstantAttrib(index, f, 4); }
void APIENTRY glVertexAttrib1iv(GLuint index, const GLint* v) { GLfloat f[1]; f[0]=(GLfloat)v[0]; SetConstantAttrib(index, f, 1); }
void APIENTRY glVertexAttrib2iv(GLuint index, const GLint* v) { GLfloat f[2]; f[0]=(GLfloat)v[0]; f[1]=(GLfloat)v[1]; SetConstantAttrib(index, f, 2); }
void APIENTRY glVertexAttrib3iv(GLuint index, const GLint* v) { GLfloat f[3]; f[0]=(GLfloat)v[0]; f[1]=(GLfloat)v[1]; f[2]=(GLfloat)v[2]; SetConstantAttrib(index, f, 3); }
void APIENTRY glVertexAttrib4iv(GLuint index, const GLint* v) { GLfloat f[4]; f[0]=(GLfloat)v[0]; f[1]=(GLfloat)v[1]; f[2]=(GLfloat)v[2]; f[3]=(GLfloat)v[3]; SetConstantAttrib(index, f, 4); }

void APIENTRY glVertexAttrib1ui(GLuint index, GLuint x) { GLfloat f[1]; f[0]=(GLfloat)x; SetConstantAttrib(index, f, 1); }
void APIENTRY glVertexAttrib2ui(GLuint index, GLuint x, GLuint y) { GLfloat f[2]; f[0]=(GLfloat)x; f[1]=(GLfloat)y; SetConstantAttrib(index, f, 2); }
void APIENTRY glVertexAttrib3ui(GLuint index, GLuint x, GLuint y, GLuint z) { GLfloat f[3]; f[0]=(GLfloat)x; f[1]=(GLfloat)y; f[2]=(GLfloat)z; SetConstantAttrib(index, f, 3); }
void APIENTRY glVertexAttrib4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w) { GLfloat f[4]; f[0]=(GLfloat)x; f[1]=(GLfloat)y; f[2]=(GLfloat)z; f[3]=(GLfloat)w; SetConstantAttrib(index, f, 4); }
void APIENTRY glVertexAttrib1uiv(GLuint index, const GLuint* v) { GLfloat f[1]; f[0]=(GLfloat)v[0]; SetConstantAttrib(index, f, 1); }
void APIENTRY glVertexAttrib2uiv(GLuint index, const GLuint* v) { GLfloat f[2]; f[0]=(GLfloat)v[0]; f[1]=(GLfloat)v[1]; SetConstantAttrib(index, f, 2); }
void APIENTRY glVertexAttrib3uiv(GLuint index, const GLuint* v) { GLfloat f[3]; f[0]=(GLfloat)v[0]; f[1]=(GLfloat)v[1]; f[2]=(GLfloat)v[2]; SetConstantAttrib(index, f, 3); }
void APIENTRY glVertexAttrib4uiv(GLuint index, const GLuint* v) { GLfloat f[4]; f[0]=(GLfloat)v[0]; f[1]=(GLfloat)v[1]; f[2]=(GLfloat)v[2]; f[3]=(GLfloat)v[3]; SetConstantAttrib(index, f, 4); }

void APIENTRY glVertexAttrib4bv(GLuint index, const GLbyte* v) { GLfloat f[4]; for (int i=0;i<4;++i) f[i]=(GLfloat)v[i]; SetConstantAttrib(index, f, 4); }
void APIENTRY glVertexAttrib4ubv(GLuint index, const GLubyte* v) { GLfloat f[4]; for (int i=0;i<4;++i) f[i]=(GLfloat)v[i]; SetConstantAttrib(index, f, 4); }
void APIENTRY glVertexAttrib4usv(GLuint index, const GLushort* v) { GLfloat f[4]; for (int i=0;i<4;++i) f[i]=(GLfloat)v[i]; SetConstantAttrib(index, f, 4); }
void APIENTRY glVertexAttrib4Nbv(GLuint index, const GLbyte* v) { GLfloat f[4]; for (int i=0;i<4;++i) f[i]=(GLfloat)v[i]/127.0f; SetConstantAttrib(index, f, 4); }
void APIENTRY glVertexAttrib4Nsv(GLuint index, const GLshort* v) { GLfloat f[4]; for (int i=0;i<4;++i) f[i]=(GLfloat)v[i]/32767.0f; SetConstantAttrib(index, f, 4); }
void APIENTRY glVertexAttrib4Niv(GLuint index, const GLint* v) { GLfloat f[4]; for (int i=0;i<4;++i) f[i]=(GLfloat)v[i]/2147483647.0f; SetConstantAttrib(index, f, 4); }
void APIENTRY glVertexAttrib4Nubv(GLuint index, const GLubyte* v) { GLfloat f[4]; for (int i=0;i<4;++i) f[i]=(GLfloat)v[i]/255.0f; SetConstantAttrib(index, f, 4); }
void APIENTRY glVertexAttrib4Nusv(GLuint index, const GLushort* v) { GLfloat f[4]; for (int i=0;i<4;++i) f[i]=(GLfloat)v[i]/65535.0f; SetConstantAttrib(index, f, 4); }
void APIENTRY glVertexAttrib4Nuiv(GLuint index, const GLuint* v) { GLfloat f[4]; for (int i=0;i<4;++i) f[i]=(GLfloat)v[i]/4294967295.0f; SetConstantAttrib(index, f, 4); }

// ---- integer generic constants (glVertexAttribI*, S3) ----------------------
// Keep the exact integer values (glGetVertexAttribIiv/Iuiv answer them) and
// mirror a float copy for the draw path.

void SetConstantAttribI(GLuint index, const GLint* v, GLsizei n) {
    if (index >= kMaxAttribs || n < 1 || n > 4) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    AttribData& a = g_vaos[g_bound_vao].attribs[index];
    a.is_pointer = false;
    for (GLsizei i = 0; i < n; ++i) {
        a.constant_i[i] = v[i];
        a.constant[i] = (GLfloat)v[i];
    }
    for (GLsizei i = n; i < 4; ++i) {
        a.constant_i[i] = i == 3 ? 1 : 0;
        a.constant[i] = i == 3 ? 1.0f : 0.0f;
    }
}

void APIENTRY glVertexAttribI1i(GLuint index, GLint x) { GLint v[1] = {x}; SetConstantAttribI(index, v, 1); }
void APIENTRY glVertexAttribI2i(GLuint index, GLint x, GLint y) { GLint v[2] = {x, y}; SetConstantAttribI(index, v, 2); }
void APIENTRY glVertexAttribI3i(GLuint index, GLint x, GLint y, GLint z) { GLint v[3] = {x, y, z}; SetConstantAttribI(index, v, 3); }
void APIENTRY glVertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w) { GLint v[4] = {x, y, z, w}; SetConstantAttribI(index, v, 4); }
void APIENTRY glVertexAttribI1iv(GLuint index, const GLint* v) { SetConstantAttribI(index, v, 1); }
void APIENTRY glVertexAttribI2iv(GLuint index, const GLint* v) { SetConstantAttribI(index, v, 2); }
void APIENTRY glVertexAttribI3iv(GLuint index, const GLint* v) { SetConstantAttribI(index, v, 3); }
void APIENTRY glVertexAttribI4iv(GLuint index, const GLint* v) { SetConstantAttribI(index, v, 4); }
void APIENTRY glVertexAttribI4bv(GLuint index, const GLbyte* v) { GLint f[4]; for (int i=0;i<4;++i) f[i]=v[i]; SetConstantAttribI(index, f, 4); }
void APIENTRY glVertexAttribI4sv(GLuint index, const GLshort* v) { GLint f[4]; for (int i=0;i<4;++i) f[i]=v[i]; SetConstantAttribI(index, f, 4); }
void APIENTRY glVertexAttribI4ubv(GLuint index, const GLubyte* v) { GLint f[4]; for (int i=0;i<4;++i) f[i]=v[i]; SetConstantAttribI(index, f, 4); }
void APIENTRY glVertexAttribI4usv(GLuint index, const GLushort* v) { GLint f[4]; for (int i=0;i<4;++i) f[i]=v[i]; SetConstantAttribI(index, f, 4); }

void APIENTRY glVertexAttribI1ui(GLuint index, GLuint x) { GLuint v[1] = {x}; SetConstantAttribI(index, (const GLint*)v, 1); }
void APIENTRY glVertexAttribI2ui(GLuint index, GLuint x, GLuint y) { GLuint v[2] = {x, y}; SetConstantAttribI(index, (const GLint*)v, 2); }
void APIENTRY glVertexAttribI3ui(GLuint index, GLuint x, GLuint y, GLuint z) { GLuint v[3] = {x, y, z}; SetConstantAttribI(index, (const GLint*)v, 3); }
void APIENTRY glVertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w) { GLuint v[4] = {x, y, z, w}; SetConstantAttribI(index, (const GLint*)v, 4); }
void APIENTRY glVertexAttribI1uiv(GLuint index, const GLuint* v) { SetConstantAttribI(index, (const GLint*)v, 1); }
void APIENTRY glVertexAttribI2uiv(GLuint index, const GLuint* v) { SetConstantAttribI(index, (const GLint*)v, 2); }
void APIENTRY glVertexAttribI3uiv(GLuint index, const GLuint* v) { SetConstantAttribI(index, (const GLint*)v, 3); }
void APIENTRY glVertexAttribI4uiv(GLuint index, const GLuint* v) { SetConstantAttribI(index, (const GLint*)v, 4); }

// ---- packed generic constants (glVertexAttribP*, S3) -----------------------
// Decode 2_10_10_10 packed values into float constants (per MobileGL; the
// draw path only sees the decoded floats).

void SetConstantAttribP(GLuint index, GLenum type, GLboolean normalized,
                        const GLuint* v, GLsizei n) {
    if (index >= kMaxAttribs || n < 1 || n > 4) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (type != GL_INT_2_10_10_10_REV && type != GL_UNSIGNED_INT_2_10_10_10_REV) {
        PUSH_ERROR(GL_INVALID_ENUM);
        return;
    }
    AttribData& a = g_vaos[g_bound_vao].attribs[index];
    a.is_pointer = false;
    GLfloat f[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    GLuint val = v[0];
    if (type == GL_INT_2_10_10_10_REV) {
        // Sign-extend each 10-bit field (w is 2 bits).
        GLint x = (GLint)(val << 22) >> 22;
        GLint y = (GLint)((val << 12) & 0xFFFFFFFFu) >> 22;
        GLint z = (GLint)((val << 2) & 0xFFFFFFFFu) >> 22;
        GLint w = (GLint)((val >> 30) & 3u);
        if (w >= 2) w -= 4;
        GLint comp[4] = {x, y, z, w};
        for (GLsizei i = 0; i < 4; ++i) {
            GLfloat divisor = i == 3 ? 1.0f : 511.0f;
            f[i] = normalized ? std::max(-1.0f, comp[i] / divisor)
                              : (GLfloat)comp[i];
        }
    } else {
        GLuint comp[4] = {val & 0x3FFu, (val >> 10) & 0x3FFu,
                          (val >> 20) & 0x3FFu, (val >> 30) & 0x3u};
        for (GLsizei i = 0; i < 4; ++i) {
            GLfloat divisor = i == 3 ? 3.0f : 1023.0f;
            f[i] = normalized ? comp[i] / divisor : (GLfloat)comp[i];
        }
    }
    for (GLsizei i = 0; i < 4; ++i) {
        a.constant[i] = f[i];
        a.constant_i[i] = (GLint)f[i];
    }
}

void APIENTRY glVertexAttribP1ui(GLuint index, GLenum type, GLboolean normalized, GLuint value) {
    SetConstantAttribP(index, type, normalized, &value, 1);
}
void APIENTRY glVertexAttribP2ui(GLuint index, GLenum type, GLboolean normalized, GLuint value) {
    SetConstantAttribP(index, type, normalized, &value, 2);
}
void APIENTRY glVertexAttribP3ui(GLuint index, GLenum type, GLboolean normalized, GLuint value) {
    SetConstantAttribP(index, type, normalized, &value, 3);
}
void APIENTRY glVertexAttribP4ui(GLuint index, GLenum type, GLboolean normalized, GLuint value) {
    SetConstantAttribP(index, type, normalized, &value, 4);
}
void APIENTRY glVertexAttribP1uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value) {
    if (!value) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    SetConstantAttribP(index, type, normalized, value, 1);
}
void APIENTRY glVertexAttribP2uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value) {
    if (!value) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    SetConstantAttribP(index, type, normalized, value, 2);
}
void APIENTRY glVertexAttribP3uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value) {
    if (!value) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    SetConstantAttribP(index, type, normalized, value, 3);
}
void APIENTRY glVertexAttribP4uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value) {
    if (!value) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    SetConstantAttribP(index, type, normalized, value, 4);
}

// ---- transform feedback (S3, CPU-counted) -----------------------------------

// -- helpers shared with draw.cpp for the CPU primitive counter --

void APIENTRY glBeginTransformFeedback(GLenum primitiveMode) {
    if (primitiveMode != GL_POINTS && primitiveMode != GL_LINES &&
        primitiveMode != GL_TRIANGLES) {
        PUSH_ERROR(GL_INVALID_ENUM);
        return;
    }
    if (g_tfb_active) { PUSH_ERROR(GL_INVALID_OPERATION); return; }
    g_tfb_active = true;
    g_tfb_primitives = 0;
}

void APIENTRY glEndTransformFeedback(void) {
    if (!g_tfb_active) { PUSH_ERROR(GL_INVALID_OPERATION); return; }
    g_tfb_active = false;
    // CPU count: write the accumulated primitive count into every buffer
    // bound under GL_TRANSFORM_FEEDBACK_BUFFER (interleaved mode writes to
    // all bound buffers, matching the GL contract for one varying).
    auto it = g_indexed_bindings.find(GL_TRANSFORM_FEEDBACK_BUFFER);
    if (it != g_indexed_bindings.end()) {
        uint64_t count = g_tfb_primitives;
        for (auto& b : it->second) {
            if (!b.buffer) continue;
            auto bit = g_buffers.find(b.buffer);
            if (bit == g_buffers.end()) continue;
            GLsizeiptr off = b.offset;
            if (off < 0 || off + (GLsizeiptr)4 > (GLsizeiptr)bit->second.data.size())
                continue;
            if (b.size < 0 || (b.size > 0 && b.size >= 4))
                std::memcpy(bit->second.data.data() + off, &count, 4);
        }
    }
    g_tfb_primitives = 0;
}

bool IndexedTargetValid(GLenum target, GLenum* error) {
    switch (target) {
        case GL_TRANSFORM_FEEDBACK_BUFFER:
        case GL_UNIFORM_BUFFER:
            return true;
        default:
            *error = GL_INVALID_ENUM;
            return false;
    }
}

void APIENTRY glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
    GLenum err = GL_NO_ERROR;
    if (!IndexedTargetValid(target, &err)) { PUSH_ERROR(err); return; }
    if (index >= kMaxIndexedBindings) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (buffer != 0 && !g_buffers.count(buffer)) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    g_indexed_bindings[target][index] = {buffer, 0, -1};
}

void APIENTRY glBindBufferRange(GLenum target, GLuint index, GLuint buffer,
                                GLintptr offset, GLsizeiptr size) {
    GLenum err = GL_NO_ERROR;
    if (!IndexedTargetValid(target, &err)) { PUSH_ERROR(err); return; }
    if (index >= kMaxIndexedBindings) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (offset < 0 || size < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (buffer != 0 && !g_buffers.count(buffer)) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    g_indexed_bindings[target][index] = {buffer, offset, size};
}

// ---- attribute queries ------------------------------------------------------

void GetConstantAttrib(const AttribData& a, GLfloat* out) {
    out[0] = a.constant[0]; out[1] = a.constant[1];
    out[2] = a.constant[2]; out[3] = a.constant[3];
}
void APIENTRY glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat* params) {
    if (index >= kMaxAttribs) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    const AttribData& a = g_vaos[g_bound_vao].attribs[index];
    switch (pname) {
        case GL_VERTEX_ATTRIB_ARRAY_ENABLED: params[0] = a.enabled ? 1.f : 0.f; break;
        case GL_VERTEX_ATTRIB_ARRAY_SIZE: params[0] = (GLfloat)a.size; break;
        case GL_VERTEX_ATTRIB_ARRAY_STRIDE: params[0] = (GLfloat)a.stride; break;
        case GL_VERTEX_ATTRIB_ARRAY_TYPE: params[0] = (GLfloat)a.type; break;
        case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: params[0] = a.normalized ? 1.f : 0.f; break;
        default: {
            const GLfloat* v = a.constant.data();
            for (int i = 0; i < 4; ++i) params[i] = v[i];
        }
    }
}
void APIENTRY glGetVertexAttribdv(GLuint index, GLenum pname, GLdouble* params) {
    GLfloat f[4]; glGetVertexAttribfv(index, pname, f);
    int n = (pname == GL_VERTEX_ATTRIB_ARRAY_ENABLED || pname == GL_VERTEX_ATTRIB_ARRAY_SIZE ||
             pname == GL_VERTEX_ATTRIB_ARRAY_STRIDE || pname == GL_VERTEX_ATTRIB_ARRAY_TYPE ||
             pname == GL_VERTEX_ATTRIB_ARRAY_NORMALIZED) ? 1 : 4;
    for (int i = 0; i < n; ++i) params[i] = (GLdouble)f[i];
}
void APIENTRY glGetVertexAttribiv(GLuint index, GLenum pname, GLint* params) {
    GLfloat f[4]; glGetVertexAttribfv(index, pname, f);
    int n = (pname == GL_VERTEX_ATTRIB_ARRAY_ENABLED || pname == GL_VERTEX_ATTRIB_ARRAY_SIZE ||
             pname == GL_VERTEX_ATTRIB_ARRAY_STRIDE || pname == GL_VERTEX_ATTRIB_ARRAY_TYPE ||
             pname == GL_VERTEX_ATTRIB_ARRAY_NORMALIZED) ? 1 : 4;
    for (int i = 0; i < n; ++i) params[i] = (GLint)f[i];
}
void APIENTRY glGetVertexAttribIiv(GLuint index, GLenum pname, GLint* params) {
    if (index >= kMaxAttribs) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    const AttribData& a = g_vaos[g_bound_vao].attribs[index];
    switch (pname) {
        case GL_VERTEX_ATTRIB_ARRAY_ENABLED: params[0] = a.enabled ? 1 : 0; break;
        case GL_VERTEX_ATTRIB_ARRAY_SIZE: params[0] = a.size; break;
        case GL_VERTEX_ATTRIB_ARRAY_STRIDE: params[0] = a.stride; break;
        case GL_VERTEX_ATTRIB_ARRAY_TYPE: params[0] = (GLint)a.type; break;
        case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: params[0] = a.normalized ? 1 : 0; break;
        default:
            for (int i = 0; i < 4; ++i) params[i] = a.constant_i[i];
    }
}
void APIENTRY glGetVertexAttribIuiv(GLuint index, GLenum pname, GLuint* params) {
    if (index >= kMaxAttribs) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    const AttribData& a = g_vaos[g_bound_vao].attribs[index];
    switch (pname) {
        case GL_VERTEX_ATTRIB_ARRAY_ENABLED: params[0] = a.enabled ? 1u : 0u; break;
        case GL_VERTEX_ATTRIB_ARRAY_SIZE: params[0] = (GLuint)a.size; break;
        case GL_VERTEX_ATTRIB_ARRAY_STRIDE: params[0] = (GLuint)a.stride; break;
        case GL_VERTEX_ATTRIB_ARRAY_TYPE: params[0] = (GLuint)a.type; break;
        case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: params[0] = a.normalized ? 1u : 0u; break;
        default:
            for (int i = 0; i < 4; ++i) params[i] = (GLuint)a.constant_i[i];
    }
}
void APIENTRY glGetVertexAttribPointerv(GLuint index, GLenum pname, void** pointer) {
    if (index >= kMaxAttribs) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) { PUSH_ERROR(GL_INVALID_ENUM); return; }
    *pointer = (void*)(uintptr_t)g_vaos[g_bound_vao].attribs[index].offset;
}

} // extern "C"
