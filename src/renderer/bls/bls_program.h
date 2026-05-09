#pragma once

#include "common_types.h"
#include "bls_permuter.h"
#include "bls_shader_cache.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace whiteout::flakes::renderer::bls {

struct BlsProgram {
    GxShaderID   id;
    BlsShader*   vs = nullptr;
    BlsShader*   ps = nullptr;

    bool IsValid() const { return vs != nullptr && ps != nullptr; }
};

struct BlsProgramDef {
    GxShaderID  id;
    const char* vsName;
    const char* psName;
};

class BlsProgramCatalog {
public:
    explicit BlsProgramCatalog(BlsShaderCache* cache);
    ~BlsProgramCatalog();

    const BlsProgram* Load(const BlsProgramDef& def);

    const BlsProgram* Get(GxShaderID id) const;
    void              Clear();

private:
    BlsShaderCache*                                          cache_ = nullptr;
    std::unordered_map<u8, std::unique_ptr<BlsProgram>> programs_;
};

}
