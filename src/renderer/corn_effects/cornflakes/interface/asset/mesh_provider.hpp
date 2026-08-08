#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace whiteout::cornflakes {

class IMeshResourceProvider {
public:
    IMeshResourceProvider() = default;
    virtual ~IMeshResourceProvider() = default;

    IMeshResourceProvider(const IMeshResourceProvider&) = delete;
    IMeshResourceProvider& operator=(const IMeshResourceProvider&) = delete;
    IMeshResourceProvider(IMeshResourceProvider&&) = delete;
    IMeshResourceProvider& operator=(IMeshResourceProvider&&) = delete;

    virtual std::span<const std::byte> readMesh(std::string_view path) = 0;
};

}
