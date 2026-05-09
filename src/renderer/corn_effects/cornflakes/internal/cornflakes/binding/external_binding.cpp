#include <cornflakes/interface/binding/external_binding.hpp>

namespace whiteout::cornflakes {

const ExternalBinding* findBindingByName(std::span<const ExternalBinding> bindings,
                                         std::string_view name) noexcept {
    for (const auto& b : bindings) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}

const FunctionBinding* findFunctionByName(std::span<const FunctionBinding> bindings,
                                          std::string_view symbolName) noexcept {
    for (const auto& b : bindings) {
        if (b.symbolName == symbolName) {
            return &b;
        }
    }
    return nullptr;
}

} // namespace whiteout::cornflakes
