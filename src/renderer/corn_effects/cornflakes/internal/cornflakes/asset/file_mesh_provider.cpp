
#include <cornflakes/interface/asset/file_mesh_provider.hpp>

#include <filesystem>
#include <fstream>
#include <utility>

namespace whiteout::cornflakes {

namespace {

bool readWholeFile(const std::filesystem::path& p, std::vector<std::byte>& out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) {
        return false;
    }
    std::ifstream in(p, std::ios::binary);
    if (!in.good()) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (!out.empty()) {
        in.read(reinterpret_cast<char*>(out.data()), size);
    }
    return in.good() || in.eof();
}

}

void FileMeshProvider::addSearchRoot(std::string root) {
    if (root.empty()) {
        return;
    }
    for (const auto& existing : m_roots) {
        if (existing == root) {
            return;
        }
    }
    m_roots.push_back(std::move(root));
}

void FileMeshProvider::addSearchRootWithParents(std::string dir, u32 parentLevels) {
    std::filesystem::path p{dir};
    addSearchRoot(p.string());
    for (u32 i = 0U; i < parentLevels; ++i) {
        const auto parent = p.parent_path();
        if (parent.empty() || parent == p) {
            break;
        }
        p = parent;
        addSearchRoot(p.string());
    }
}

std::span<const std::string> FileMeshProvider::triedPaths(std::string_view path) const {
    const auto it = m_triedPaths.find(std::string{path});
    if (it == m_triedPaths.end()) {
        return {};
    }
    return {it->second.data(), it->second.size()};
}

std::span<const std::byte> FileMeshProvider::readMesh(std::string_view path) {
    const std::string key{path};
    if (const auto it = m_cache.find(key); it != m_cache.end()) {
        return {it->second.data(), it->second.size()};
    }

    std::vector<std::byte> bytes;
    std::vector<std::string> tried;
    const std::filesystem::path asGiven{key};
    if (asGiven.is_absolute()) {
        tried.push_back(asGiven.string());
        readWholeFile(asGiven, bytes);
    }
    const auto filename = asGiven.filename();
    const bool hasDirs = asGiven.has_parent_path();
    for (const auto& root : m_roots) {
        const std::filesystem::path base{root};
        for (int form = 0; form < (hasDirs ? 2 : 1) && bytes.empty(); ++form) {
            const auto full = base / (form == 0 ? asGiven : filename);
            tried.push_back(full.string());
            readWholeFile(full, bytes);
        }
        if (!bytes.empty()) {
            break;
        }
    }
    if (!bytes.empty()) {
        ++m_loadedCount;
    } else {
        m_triedPaths.emplace(key, std::move(tried));
    }
    const auto [it, _] = m_cache.emplace(key, std::move(bytes));
    return {it->second.data(), it->second.size()};
}

}
