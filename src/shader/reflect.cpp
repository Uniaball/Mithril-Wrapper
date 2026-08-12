// Mithril-Wrapper shader module -- program/uniform/attribute reflection
// (M2-S2). SPIRV-Cross reflects the linked stage SPIR-V so
// glGetUniformLocation/glGetAttribLocation and the uniform getters answer
// honestly. Stage F adds uniform-block introspection: every uniform block
// (explicit `uniform Block { .. }` or the synthetic mithril_GlobalBlock
// wrapper) is recorded with its std140 size, binding and member offsets so
// the glGetActiveUniformBlock*/glGetUniformIndices family can answer.

#include <shader/shader.h>

#include <GL/glcorearb.h>

#include <spirv_cross.hpp>

#include <util/log.h>

#include <algorithm>
#include <string>
#include <vector>

namespace mithril::shader {

namespace {

// Per-stage block info collected during SPIRV-Cross iteration; merged into
// Program::uniform_blocks afterwards.
struct StageBlock {
    std::string name;
    GLenum binding = 0;
    GLint data_size = 0;
    bool referenced_by_vs = false;
    bool referenced_by_fs = false;
    std::unordered_map<std::string, GLint> member_offset;  // name -> byte offset
};

// Merge one stage's block resources into `blocks` (dedup by name, union of
// members). Returns the block index for `name` (-1 when not a block member
// name is asked; used to look up member ownership).
GLint BlockIndex(const std::string& name,
                 const std::vector<StageBlock>& blocks) {
    for (size_t i = 0; i < blocks.size(); ++i)
        if (blocks[i].name == name) return (GLint)i;
    return -1;
}

} // namespace

void ReflectProgram(Program& prog) {
    prog.uniforms.clear();
    prog.uniform_by_name.clear();
    prog.uniform_by_location.clear();
    prog.attrib_locations.clear();
    prog.samplers.clear();
    prog.uniform_blocks.clear();
    prog.frag_data.clear();

    // Collected per-stage, merged below (same block across stages => one
    // block index; member names unique across the program by construction).
    std::vector<StageBlock> blocks;

    auto reflect_stage = [&](const std::vector<uint32_t>& words, bool vs) {
        if (words.empty()) return;
        try {
            spirv_cross::Compiler compiler(words.data(), words.size());
            spirv_cross::ShaderResources res = compiler.get_shader_resources();

            // Uniform block members ($Global wrap -> original GL uniform names).
            for (auto& r : res.uniform_buffers) {
                const spirv_cross::SPIRType& t = compiler.get_type(r.base_type_id);
                StageBlock sb;
                sb.name = r.name;
                sb.binding = (GLenum)compiler.get_decoration(
                    r.id, spv::DecorationBinding);
                sb.data_size = (GLint)compiler.get_declared_struct_size(t);
                for (uint32_t i = 0; i < t.member_types.size(); ++i) {
                    std::string name = compiler.get_member_name(r.base_type_id, i);
                    if (name.empty()) continue;
                    sb.member_offset[name] =
                        (GLint)compiler.get_member_decoration(
                            r.base_type_id, i, spv::DecorationOffset);
                    prog.uniform_by_name[name] = -1;  // placeholder; location below
                }
                GLint idx = BlockIndex(sb.name, blocks);
                if (idx < 0) {
                    blocks.push_back(std::move(sb));
                    idx = (GLint)(blocks.size() - 1);
                } else {
                    for (auto& kv : sb.member_offset)
                        blocks[idx].member_offset.emplace(kv.first, kv.second);
                    blocks[idx].data_size =
                        std::max(blocks[idx].data_size, sb.data_size);
                }
                if (vs) blocks[idx].referenced_by_vs = true;
                else blocks[idx].referenced_by_fs = true;
            }
            // Samplers / standalone uniforms.
            auto add_sampler = [&](spirv_cross::Resource& r) {
                std::string name = r.name;
                if (name.empty()) return;
                if (prog.uniform_by_name.find(name) == prog.uniform_by_name.end())
                    prog.uniform_by_name[name] = -1;
                uint32_t binding = compiler.get_decoration(r.id, spv::DecorationBinding);
                bool dup = false;
                for (auto& s : prog.samplers)
                    if (s.name == name) { dup = true; break; }
                if (!dup)
                    prog.samplers.push_back({name, GL_SAMPLER_2D, binding, -1});
            };
            for (auto& r : res.sampled_images) add_sampler(r);
            for (auto& r : res.separate_images) add_sampler(r);
            for (auto& r : res.separate_samplers) add_sampler(r);

            // Attributes: location + name.
            for (auto& r : res.stage_inputs) {
                if (r.name.empty()) continue;
                int loc = static_cast<int>(compiler.get_decoration(r.id, spv::DecorationLocation));
                prog.attrib_locations[r.name] = loc;
            }
        } catch (const std::exception& e) {
            ML_LOG_WARN("SPIRV-Cross reflection failed: %s", e.what());
        }
    };

    reflect_stage(prog.vertex_spirv, true);
    reflect_stage(prog.fragment_spirv, false);

    // Assign stable locations in a deterministic order (alphabetical).
    std::vector<std::string> names;
    names.reserve(prog.uniform_by_name.size());
    for (auto& kv : prog.uniform_by_name) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    for (size_t i = 0; i < names.size(); ++i) {
        Uniform u;
        u.name = names[i];
        u.location = static_cast<GLint>(i);
        prog.uniform_by_location[static_cast<GLint>(i)] = prog.uniforms.size();
        prog.uniform_by_name[names[i]] = static_cast<GLint>(i);
        prog.uniforms.push_back(std::move(u));
    }
    for (auto& s : prog.samplers) {
        auto it = prog.uniform_by_name.find(s.name);
        s.location = it == prog.uniform_by_name.end() ? -1 : it->second;
        for (auto& u : prog.uniforms)
            if (u.name == s.name) u.type = s.type;
    }

    // Wire per-uniform block ownership, then emit Program::uniform_blocks in
    // the same deterministic (alphabetical) order as the uniforms.
    std::sort(blocks.begin(), blocks.end(),
              [](const StageBlock& a, const StageBlock& b) {
                  return a.name < b.name;
              });
    for (auto& u : prog.uniforms) {
        for (size_t bi = 0; bi < blocks.size(); ++bi) {
            auto it = blocks[bi].member_offset.find(u.name);
            if (it != blocks[bi].member_offset.end()) {
                u.block_index = (GLint)bi;
                u.block_offset = it->second;
                break;
            }
        }
    }
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        UniformBlock ub;
        ub.name = blocks[bi].name;
        ub.binding = blocks[bi].binding;
        ub.data_size = blocks[bi].data_size;
        ub.referenced_by_vs = blocks[bi].referenced_by_vs;
        ub.referenced_by_fs = blocks[bi].referenced_by_fs;
        for (size_t ui = 0; ui < prog.uniforms.size(); ++ui)
            if (prog.uniforms[ui].block_index == (GLint)bi)
                ub.members.push_back((GLint)ui);
        prog.uniform_blocks.push_back(std::move(ub));
    }
}

} // namespace mithril::shader
