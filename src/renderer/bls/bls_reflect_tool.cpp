#include "common_types.h"
#include "bls_container.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using namespace WhiteoutDex;

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static bool ReadFile(const fs::path& p, std::vector<u8>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    out.resize(static_cast<usize>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), out.size());
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <bls-file> <perm-index>\n", argv[0]);
        return 1;
    }
    fs::path input = argv[1];
    i32 which      = std::atoi(argv[2]);

    std::vector<u8> bytes;
    if (!ReadFile(input, bytes)) {
        std::fprintf(stderr, "cannot read %s\n", input.string().c_str());
        return 1;
    }
    WhiteoutDex::bls::BlsContainer bls;
    std::string err;
    if (!bls.Load(bytes, &err)) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }
    if (which < 0 || (usize)which >= bls.PermuteCount()) {
        std::fprintf(stderr, "permute %d out of range (0..%zu)\n", which, bls.PermuteCount());
        return 1;
    }

    auto view = bls.Permute(which);
    std::printf("# file=%s perm=%d size=%u\n",
                input.filename().string().c_str(), which, (u32)view.dxbc.size());

    ComPtr<ID3DBlob> dasm;
    HRESULT hr = D3DDisassemble(view.dxbc.data(), view.dxbc.size(),
                                D3D_DISASM_ENABLE_DEFAULT_VALUE_PRINTS, nullptr, &dasm);
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3DDisassemble failed: 0x%08lx\n", static_cast<long>(hr));
        return 1;
    }
    std::fwrite(dasm->GetBufferPointer(), 1, dasm->GetBufferSize(), stdout);
    return 0;
}
