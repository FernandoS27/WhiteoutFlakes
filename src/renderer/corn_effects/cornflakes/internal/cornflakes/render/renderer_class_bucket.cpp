#include <cornflakes/core/determinism.hpp>
#include <cornflakes/render/renderer_class_bucket.hpp>

namespace whiteout::cornflakes {

RendererClassBuckets bucketByClass(std::span<const RenderPacket> packets) {
    RendererClassBuckets out;
    out[0].cls = RendererClass::Billboard;
    out[1].cls = RendererClass::Ribbon;
    out[2].cls = RendererClass::Mesh;
    out[3].cls = RendererClass::Light;

    for (const auto& p : packets) {
        const auto idx = static_cast<std::size_t>(p.cls);
        if (idx < out.size()) {
            out[idx].packets.push_back(&p);
        }
    }
    return out;
}

} // namespace whiteout::cornflakes
