#pragma once

#include <cornflakes/interface/asset/mesh_provider.hpp>
#include <cornflakes/interface/asset/vector_field_provider.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whiteout::cornflakes {

class FileMeshProvider final : public IMeshResourceProvider, public IVectorFieldProvider {
public:
    FileMeshProvider() = default;

    void addSearchRoot(std::string root);

    void addSearchRootWithParents(std::string dir, u32 parentLevels = 3U);

    std::span<const std::byte> readMesh(std::string_view path) override;

    std::span<const std::byte> readVectorField(std::string_view path) override {
        return readMesh(path);
    }

    std::size_t loadedCount() const noexcept {
        return m_loadedCount;
    }

    std::span<const std::string> triedPaths(std::string_view path) const;

    std::span<const std::string> searchRoots() const noexcept {
        return {m_roots.data(), m_roots.size()};
    }

private:
    std::vector<std::string> m_roots;
    std::unordered_map<std::string, std::vector<std::byte>> m_cache;
    std::unordered_map<std::string, std::vector<std::string>> m_triedPaths;
    std::size_t m_loadedCount = 0U;
};

}
