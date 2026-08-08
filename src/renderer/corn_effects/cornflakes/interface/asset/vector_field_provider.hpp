#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace whiteout::cornflakes {

class IVectorFieldProvider {
public:
    IVectorFieldProvider() = default;
    virtual ~IVectorFieldProvider() = default;

    IVectorFieldProvider(const IVectorFieldProvider&) = delete;
    IVectorFieldProvider& operator=(const IVectorFieldProvider&) = delete;
    IVectorFieldProvider(IVectorFieldProvider&&) = delete;
    IVectorFieldProvider& operator=(IVectorFieldProvider&&) = delete;

    virtual std::span<const std::byte> readVectorField(std::string_view path) = 0;
};

}
