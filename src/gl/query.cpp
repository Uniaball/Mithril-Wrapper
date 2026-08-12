// Mithril-Wrapper GL entry points -- S6 query objects (milestone M6 stage D).
// glGenQueries..glGetQueryObject* family over the Vulkan query backend
// (v::BeginOcclusionQuery / v::BeginTimeElapsedQuery / v::QueryCounterTimestamp).
// Each query name maps to one vk-side handle; when the backend is unavailable
// the handle is 0 and every result degrades to 0 (immediately available, not
// cached), matching the MobileGL graceful-fallback contract.
// Also implements glPrimitiveRestartIndex / glProvokingVertex (pure state that
// the draw path folds into the pipeline cache key).

#include "internal.h"

#include <cstdint>
#include <unordered_map>

namespace {

struct GLQuery {
    bool in_use = false;
    uint64_t vk = 0;         // backend handle (0 = degraded)
    GLenum target = 0;       // GL_SAMPLES_PASSED / GL_TIME_ELAPSED / ...
    bool active = false;     // BeginQuery seen, EndQuery not yet
    uint64_t result = 0;     // cached GL_QUERY_RESULT
    bool result_cached = false;
};

std::unordered_map<GLuint, GLQuery> g_queries;
GLuint g_next_query = 1;

// Active query id per target (GL_CURRENT_QUERY): only one query per target may
// be active at a time (glBeginQuery on an already-active target is an error).
std::unordered_map<GLenum, GLuint> g_active_query;

GLQuery* Find(GLuint id) {
    auto it = g_queries.find(id);
    return (it != g_queries.end() && it->second.in_use) ? &it->second : nullptr;
}

// Valid glBeginQuery targets (GL 3.3 core). The transform-feedback/primitive
// counters are accepted but degrade (their result reads 0).
bool ValidBeginTarget(GLenum target) {
    switch (target) {
        case GL_SAMPLES_PASSED:
        case GL_ANY_SAMPLES_PASSED:
        case GL_TIME_ELAPSED:
        case GL_PRIMITIVES_GENERATED:
        case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
            return true;
        default:
            return false;
    }
}

uint64_t BeginBackendQuery(GLenum target) {
    switch (target) {
        case GL_SAMPLES_PASSED:
        case GL_ANY_SAMPLES_PASSED:
            return v::BeginOcclusionQuery(target);
        case GL_TIME_ELAPSED:
            return v::BeginTimeElapsedQuery();
        default:
            return 0;   // accepted target without a backend (degrades)
    }
}

void EndBackendQuery(GLenum target, uint64_t vk) {
    switch (target) {
        case GL_SAMPLES_PASSED:
        case GL_ANY_SAMPLES_PASSED:
            v::EndOcclusionQuery(vk);
            break;
        case GL_TIME_ELAPSED:
            v::EndTimeElapsedQuery(vk);
            break;
        default:
            break;
    }
}

// GL_QUERY_RESULT for the query (blocking). ANY_SAMPLES_PASSED normalises the
// non-zero sample sum to 1, per GL semantics.
bool ReadResult(GLQuery& q, uint64_t* out) {
    if (q.result_cached) { *out = q.result; return true; }
    uint64_t raw = 0;
    if (!v::GetQueryResult64(q.vk, true, &raw)) {
        *out = 0;
        return false;
    }
    if (q.target == GL_ANY_SAMPLES_PASSED) raw = raw ? 1 : 0;
    q.result = raw;
    q.result_cached = true;
    *out = raw;
    return true;
}

} // namespace

extern "C" {

void APIENTRY glGenQueries(GLsizei n, GLuint* ids) {
    if (n < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (n == 0 || !ids) return;
    for (GLsizei i = 0; i < n; ++i) {
        while (g_next_query == 0 || g_queries.count(g_next_query))
            ++g_next_query;
        ids[i] = g_next_query++;
        g_queries[ids[i]].in_use = true;
    }
}

void APIENTRY glDeleteQueries(GLsizei n, const GLuint* ids) {
    if (n < 0) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; ++i) {
        GLuint id = ids[i];
        if (id == 0) continue;
        auto it = g_queries.find(id);
        if (it == g_queries.end() || !it->second.in_use) continue;
        // Deleting an active query ends it (becomes inactive).
        if (it->second.active) {
            for (auto& kv : g_active_query)
                if (kv.second == id) kv.second = 0;
            EndBackendQuery(it->second.target, it->second.vk);
        }
        v::DeleteBackendQuery(it->second.vk);
        g_queries.erase(it);
    }
}

GLboolean APIENTRY glIsQuery(GLuint id) {
    return Find(id) ? GL_TRUE : GL_FALSE;
}

void APIENTRY glBeginQuery(GLenum target, GLuint id) {
    if (!ValidBeginTarget(target)) { PUSH_ERROR(GL_INVALID_ENUM); return; }
    GLQuery* q = Find(id);
    if (!q) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (g_active_query[target] != 0) {
        PUSH_ERROR(GL_INVALID_OPERATION);   // a query is already active here
        return;
    }
    if (q->active) { PUSH_ERROR(GL_INVALID_OPERATION); return; }
    v::EnsureInit();
    q->target = target;
    q->vk = BeginBackendQuery(target);
    q->active = true;
    q->result_cached = false;
    g_active_query[target] = id;
}

void APIENTRY glEndQuery(GLenum target) {
    auto it = g_active_query.find(target);
    if (it == g_active_query.end() || it->second == 0) {
        PUSH_ERROR(GL_INVALID_OPERATION);   // no active query on this target
        return;
    }
    GLQuery* q = Find(it->second);
    if (!q || !q->active) { PUSH_ERROR(GL_INVALID_OPERATION); return; }
    EndBackendQuery(target, q->vk);
    q->active = false;
    q->result_cached = false;
    it->second = 0;
}

void APIENTRY glQueryCounter(GLuint id, GLenum target) {
    if (target != GL_TIMESTAMP) { PUSH_ERROR(GL_INVALID_ENUM); return; }
    GLQuery* q = Find(id);
    if (!q) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    v::EnsureInit();
    q->target = target;
    q->vk = v::QueryCounterTimestamp();
    q->active = false;
    q->result_cached = false;
}

void APIENTRY glGetQueryiv(GLenum target, GLenum pname, GLint* params) {
    if (!params) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    switch (pname) {
        case GL_CURRENT_QUERY:
            params[0] = (GLint)g_active_query[target];
            break;
        case GL_QUERY_COUNTER_BITS:
            switch (target) {
                case GL_SAMPLES_PASSED:
                case GL_ANY_SAMPLES_PASSED:
                case GL_TIME_ELAPSED:
                case GL_TIMESTAMP:
                case GL_PRIMITIVES_GENERATED:
                case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
                    params[0] = 64;
                    break;
                default:
                    PUSH_ERROR(GL_INVALID_ENUM);
                    return;
            }
            break;
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
            return;
    }
}

// Shared body for the four glGetQueryObject* result getters. `fmt` picks the
// write width; the value is always read as uint64 from the backend.
static void GetQueryObject(GLuint id, GLenum pname, void* params, int fmt) {
    GLQuery* q = Find(id);
    if (!q) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    switch (pname) {
        case GL_QUERY_RESULT_AVAILABLE:
            if (q->active || q->vk == 0 || v::IsQueryResultAvailable(q->vk)) {
                if (fmt == 1) *(GLint*)params = GL_TRUE;
                else if (fmt == 2) *(GLuint*)params = GL_TRUE;
                else if (fmt == 3) *(GLint64*)params = GL_TRUE;
                else *(GLuint64*)params = GL_TRUE;
            } else {
                if (fmt == 1) *(GLint*)params = GL_FALSE;
                else if (fmt == 2) *(GLuint*)params = GL_FALSE;
                else if (fmt == 3) *(GLint64*)params = GL_FALSE;
                else *(GLuint64*)params = GL_FALSE;
            }
            break;
        case GL_QUERY_RESULT: {
            uint64_t value = 0;
            if (q->vk == 0) value = 0;   // degraded
            else if (!ReadResult(*q, &value)) value = 0;
            if (fmt == 1) *(GLint*)params = (GLint)value;
            else if (fmt == 2) *(GLuint*)params = (GLuint)value;
            else if (fmt == 3) *(GLint64*)params = (GLint64)value;
            else *(GLuint64*)params = (GLuint64)value;
            break;
        }
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
            return;
    }
}

void APIENTRY glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {
    GetQueryObject(id, pname, params, 1);
}

void APIENTRY glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) {
    GetQueryObject(id, pname, params, 2);
}

void APIENTRY glGetQueryObjecti64v(GLuint id, GLenum pname, GLint64* params) {
    GetQueryObject(id, pname, params, 3);
}

void APIENTRY glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params) {
    GetQueryObject(id, pname, params, 4);
}

void APIENTRY glPrimitiveRestartIndex(GLuint index) {
    s::GetState().restart_index = index;
}

void APIENTRY glProvokingVertex(GLenum mode) {
    switch (mode) {
        case GL_FIRST_VERTEX_CONVENTION:
        case GL_LAST_VERTEX_CONVENTION:
            s::GetState().provoking_vertex = mode;
            break;
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
    }
}

// ---- conditional rendering (S6) --------------------------------------------
// MobileGL semantics: the whole conditional-render machinery is accepted but
// never gates draws (the backend renders unconditionally). State is tracked
// so glBegin/glEnd pairing rules are honoured.

namespace {
bool g_cond_render_active = false;
}

void APIENTRY glBeginConditionalRender(GLuint id, GLenum mode) {
    switch (mode) {
        case GL_QUERY_WAIT:
        case GL_QUERY_NO_WAIT:
        case GL_QUERY_BY_REGION_WAIT:
        case GL_QUERY_BY_REGION_NO_WAIT:
            break;
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
            return;
    }
    if (!Find(id)) { PUSH_ERROR(GL_INVALID_VALUE); return; }
    if (g_cond_render_active) { PUSH_ERROR(GL_INVALID_OPERATION); return; }
    g_cond_render_active = true;
}

void APIENTRY glEndConditionalRender(void) {
    if (!g_cond_render_active) { PUSH_ERROR(GL_INVALID_OPERATION); return; }
    g_cond_render_active = false;
}

} // extern "C"
