// Mithril-Wrapper GL entry points -- S6 sync objects (milestone M6 stage C).
// glFenceSync family: each GLsync wraps a dedicated VkFence (created via
// v::CreateGLSync) that fires after all commands recorded before the sync.
// When the backend is unavailable (no Vulkan / init failed) the handle's
// fence is 0 and every operation degrades to MobileGL mode: already-signaled,
// never blocking.

#include "internal.h"

#include <mutex>
#include <set>

namespace {

// Mirror of the opaque `struct __GLsync` that GLsync is defined as; the
// public typedef in glcorearb.h is `struct __GLsync *`.
struct SyncObj {
    uint64_t fence = 0;            // vk-side fence handle (0 = degraded)
    GLenum condition = GL_SYNC_GPU_COMMANDS_COMPLETE;
    GLbitfield flags = 0;
};

std::mutex g_sync_mutex;
std::set<GLsync> g_syncs;

GLsync Find(GLsync s) {
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    auto it = g_syncs.find(s);
    if (it == g_syncs.end()) return nullptr;
    return *it;
}

} // namespace

extern "C" {

GLsync APIENTRY glFenceSync(GLenum condition, GLbitfield flags) {
    if (condition != GL_SYNC_GPU_COMMANDS_COMPLETE) {
        PUSH_ERROR(GL_INVALID_ENUM);
        return nullptr;
    }
    if (flags != 0) {
        PUSH_ERROR(GL_INVALID_VALUE);
        return nullptr;
    }
    auto* s = new SyncObj;
    s->condition = condition;
    s->flags = flags;
    if (!g_dirty_textures.empty()) FlushDirtyTextureUploads();
    s->fence = v::CreateGLSync();
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    g_syncs.insert(reinterpret_cast<GLsync>(s));
    return reinterpret_cast<GLsync>(s);
}

GLboolean APIENTRY glIsSync(GLsync sync) {
    if (!sync) return GL_FALSE;
    return Find(sync) != nullptr ? GL_TRUE : GL_FALSE;
}

void APIENTRY glDeleteSync(GLsync sync) {
    if (!sync) return;
    SyncObj* s;
    {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        auto it = g_syncs.find(sync);
        if (it == g_syncs.end()) {
            PUSH_ERROR(GL_INVALID_VALUE);
            return;
        }
        g_syncs.erase(it);
        s = reinterpret_cast<SyncObj*>(sync);
    }
    v::DestroyGLSync(s->fence);
    delete s;
}

GLenum APIENTRY glClientWaitSync(GLsync sync, GLbitfield flags,
                                 GLuint64 timeout) {
    GLsync live = Find(sync);
    if (!live) {
        PUSH_ERROR(GL_INVALID_VALUE);
        return GL_WAIT_FAILED;
    }
    if (flags & ~GL_SYNC_FLUSH_COMMANDS_BIT) {
        PUSH_ERROR(GL_INVALID_VALUE);
        return GL_WAIT_FAILED;
    }
    SyncObj* s = reinterpret_cast<SyncObj*>(live);
    if (flags & GL_SYNC_FLUSH_COMMANDS_BIT) v::SubmitFlush(false);
    if (s->fence == 0) return GL_ALREADY_SIGNALED;   // degraded
    if (v::CheckGLSync(s->fence)) return GL_ALREADY_SIGNALED;
    if (timeout == 0) return GL_TIMEOUT_EXPIRED;
    return v::WaitGLSync(s->fence, timeout) ? GL_CONDITION_SATISFIED
                                            : GL_TIMEOUT_EXPIRED;
}

void APIENTRY glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    GLsync live = Find(sync);
    if (!live) {
        PUSH_ERROR(GL_INVALID_VALUE);
        return;
    }
    if (flags != 0 || timeout != GL_TIMEOUT_IGNORED) {
        PUSH_ERROR(GL_INVALID_VALUE);
        return;
    }
    SyncObj* s = reinterpret_cast<SyncObj*>(live);
    if (s->fence != 0) v::WaitGLSync(s->fence, UINT64_MAX);
}

void APIENTRY glGetSynciv(GLsync sync, GLenum pname, GLsizei count,
                          GLsizei* length, GLint* values) {
    GLsync live = Find(sync);
    if (!live) {
        PUSH_ERROR(GL_INVALID_VALUE);
        return;
    }
    if (count < 1 || !values) {
        PUSH_ERROR(GL_INVALID_VALUE);
        return;
    }
    SyncObj* s = reinterpret_cast<SyncObj*>(live);
    switch (pname) {
        case GL_OBJECT_TYPE:    values[0] = GL_SYNC_FENCE; break;
        case GL_SYNC_CONDITION: values[0] = (GLint)s->condition; break;
        case GL_SYNC_FLAGS:     values[0] = (GLint)s->flags; break;
        case GL_SYNC_STATUS:
            values[0] = (s->fence == 0 || v::CheckGLSync(s->fence))
                            ? GL_SIGNALED : GL_UNSIGNALED;
            break;
        default:
            PUSH_ERROR(GL_INVALID_ENUM);
            return;
    }
    if (length) *length = 1;
}

} // extern "C"