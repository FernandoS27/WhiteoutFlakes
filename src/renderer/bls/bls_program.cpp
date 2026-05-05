#include "bls_program.h"

namespace WhiteoutDex::bls {

BlsProgramCatalog::BlsProgramCatalog(BlsShaderCache* cache) : cache_(cache) {}

BlsProgramCatalog::~BlsProgramCatalog() { Clear(); }

const BlsProgram* BlsProgramCatalog::Load(const BlsProgramDef& def) {
    if (!cache_) return nullptr;

    if (auto it = programs_.find(static_cast<u8>(def.id)); it != programs_.end()) {
        return it->second.get();
    }

    BlsShader* vs = (def.vsName && def.vsName[0])
        ? cache_->Acquire(gfx::ShaderStage::Vertex, def.vsName) : nullptr;
    BlsShader* ps = (def.psName && def.psName[0])
        ? cache_->Acquire(gfx::ShaderStage::Pixel,  def.psName) : nullptr;

    if (!vs || !ps) {
        if (vs) cache_->Release(vs);
        if (ps) cache_->Release(ps);
        return nullptr;
    }

    auto program = std::make_unique<BlsProgram>();
    program->id = def.id;
    program->vs = vs;
    program->ps = ps;

    BlsProgram* raw = program.get();
    programs_.emplace(static_cast<u8>(def.id), std::move(program));
    return raw;
}

const BlsProgram* BlsProgramCatalog::Get(GxShaderID id) const {
    auto it = programs_.find(static_cast<u8>(id));
    return (it != programs_.end()) ? it->second.get() : nullptr;
}

void BlsProgramCatalog::Clear() {
    if (!cache_) { programs_.clear(); return; }
    for (auto& [id, program] : programs_) {
        if (program->vs) cache_->Release(program->vs);
        if (program->ps) cache_->Release(program->ps);
    }
    programs_.clear();
}

}
