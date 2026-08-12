// Mithril-Wrapper shader module -- GLSL -> SPIR-V compilation and
// program/uniform/attribute reflection (milestone M2-S2).
//
// glslang compiles desktop GLSL (Core Profile) to Vulkan SPIR-V; SPIRV-Cross
// reflects the linked program so glGetUniformLocation/glGetAttribLocation and
// the uniform getters can answer honestly. The Vulkan backend (M2+) consumes
// the cached SPIR-V words directly at vkCreateShaderModule time.

#pragma once

#include <GL/glcorearb.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mithril::shader {

// One GLSL shader object (GL_VERTEX_SHADER / GL_FRAGMENT_SHADER / ...).
struct Shader {
    GLuint id = 0;
    GLenum type = GL_VERTEX_SHADER;
    std::string source;
    bool compiled = false;
    std::string info_log;
    std::vector<uint32_t> spirv;   // SPIR-V words (empty until compiled)
};

// A reflected program uniform. `value` caches the last glUniform* write so
// glGetUniform* can answer; the Vulkan backend later uploads it into a UBO.
struct Uniform {
    std::string name;
    GLenum type = GL_FLOAT;
    GLint location = -1;
    std::vector<float> value;
    // Uniform-block introspection (S2): index of the owning block in
    // Program::uniform_blocks (-1 = standalone / sampler) and the member's
    // std140 byte offset inside that block (0 for standalone uniforms).
    GLint block_index = -1;
    GLint block_offset = 0;
};

// One reflected uniform block (explicit `uniform Block { .. }` declarations
// and the synthetic mithril_GlobalBlock wrapper alike).
struct UniformBlock {
    std::string name;
    GLenum binding = 0;
    GLint data_size = 0;         // std140 struct size in bytes
    std::vector<GLint> members;  // indices into Program::uniforms
    bool referenced_by_vs = false;
    bool referenced_by_fs = false;
};

// Original-view fragment data binding: name -> (color number, index) recorded
// by glBindFragDataLocation/Indexed (answered by glGetFragData*).
struct FragDataBind {
    GLuint color = 0;
    GLuint index = 0;
};

// A sampled-image uniform: the sampler's Vulkan binding (assigned by
// assign_sampler_bindings during compilation, 1-based) and the GL texture
// unit value written by glUniform1i.
struct SamplerRef {
    std::string name;
    GLenum type = GL_SAMPLER_2D;
    uint32_t binding = 0;      // Vulkan descriptor binding for this sampler
    GLint location = -1;
};

struct Program {
    GLuint id = 0;
    std::vector<GLuint> attached;          // attached shader ids
    bool linked = false;
    std::string info_log;
    std::vector<Uniform> uniforms;         // active uniforms (index == GL index)
    std::unordered_map<std::string, GLint> uniform_by_name;    // name -> location
    std::unordered_map<GLint, size_t> uniform_by_location;     // location -> uniforms idx
    std::unordered_map<std::string, GLint> attrib_locations;   // name -> location
    // Vertex input kind per attribute location (0 = float, 1 = int, 2 = uint),
    // reflected from the vertex stage so the backend picks a matching
    // vertex-input format (MoltenVK rejects a Float2 format feeding an int2
    // shader input: "Cannot convert attribute from MTLAttributeFormatFloat2 to
    // int2 or uint2.").
    std::unordered_map<GLint, int> attrib_kinds;
    std::vector<SamplerRef> samplers;      // M4: active sampler uniforms
    // S2 stage F: uniform-block introspection + fragment data bindings.
    std::vector<UniformBlock> uniform_blocks;  // index == GL block index
    std::unordered_map<std::string, FragDataBind> frag_data;   // GL name -> binding
    // S3 stage F: transform-feedback varying capture list (glTransformFeedbackVaryings).
    std::vector<std::string> tfb_varyings;
    GLenum tfb_buffer_mode = GL_INTERLEAVED_ATTRIBS;
    std::vector<uint32_t> vertex_spirv;    // linked stage SPIR-V
    std::vector<uint32_t> fragment_spirv;
};

// Compile `source` for `stage` into SPIR-V words. Returns false and fills
// `info` with the glslang diagnostics on failure. Thread-safe; results cached
// by (stage, source) hash.
bool CompileStage(GLenum stage, const std::string& source,
                  std::vector<uint32_t>& out_spirv, std::string& out_info);

// Reflect a linked program: extract uniform + attribute names/locations from
// the stage SPIR-V via SPIRV-Cross. Always succeeds (empty result on malformed
// SPIR-V).
void ReflectProgram(Program& prog);

// Object tables (single shared context).
Shader* GetShader(GLuint id);
Program* GetProgram(GLuint id);
GLuint NewShader(GLenum type);
GLuint NewProgram();
void DeleteShader(GLuint id);
void DeleteProgram(GLuint id);

} // namespace mithril::shader