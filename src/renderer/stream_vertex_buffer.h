#pragma once

// A CPU-writable vertex buffer, refilled every frame and grown to fit. The
// particle side streams — multi-texture, the refraction mask and Diablo III —
// each keep one.

#include "gfx/gfx.h"
#include "whiteout/flakes/types.h"

#include <algorithm>

namespace whiteout::flakes::renderer {

class StreamVertexBuffer {
public:
    /// @brief Room for @p count vertices of type @p V, handed to `fill(V*)`.
    ///
    /// Outgrowing the buffer replaces it with one at least twice the size and
    /// never under @p floor, so a rising count does not reallocate every frame.
    /// False when the device gives back no buffer or no mapping.
    template <typename V, typename Fill>
    bool Upload(gfx::IGFXDevice& dev, i32 count, i32 floor, Fill&& fill) {
        if (buffer_ == gfx::BufferHandle::Invalid || count > capacity_) {
            const i32 size = std::max({count, floor, capacity_ * 2});
            Release(dev);
            gfx::BufferDesc bd;
            bd.size = static_cast<u64>(sizeof(V)) * static_cast<u64>(size);
            bd.usage = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
            bd.ringSlotsHint = 4; // mapped once per frame
            buffer_ = dev.CreateBuffer(bd);
            if (buffer_ == gfx::BufferHandle::Invalid)
                return false;
            capacity_ = size;
        }
        void* mapped = dev.MapBuffer(buffer_);
        if (!mapped)
            return false;
        fill(static_cast<V*>(mapped));
        dev.UnmapBuffer(buffer_);
        return true;
    }

    gfx::BufferHandle Handle() const {
        return buffer_;
    }

    void Release(gfx::IGFXDevice& dev) {
        if (buffer_ != gfx::BufferHandle::Invalid)
            dev.Destroy(buffer_);
        buffer_ = gfx::BufferHandle::Invalid;
        capacity_ = 0;
    }

private:
    gfx::BufferHandle buffer_ = gfx::BufferHandle::Invalid;
    i32 capacity_ = 0;
};

} // namespace whiteout::flakes::renderer
