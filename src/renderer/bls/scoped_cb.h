#pragma once

#include "whiteout/flakes/types.h"
#include "gfx/gfx.h"

namespace whiteout::flakes::renderer::bls {

template <class T>
class ScopedCb {
public:
    ScopedCb(gfx::IGFXDevice* gfx, gfx::BufferHandle handle)
        : gfx_(gfx), handle_(handle),
          ptr_(gfx && handle != gfx::BufferHandle::Invalid
                   ? static_cast<T*>(gfx->MapBuffer(handle))
                   : nullptr) {}

    ~ScopedCb() {
        if (ptr_) gfx_->UnmapBuffer(handle_);
    }

    ScopedCb(const ScopedCb&)            = delete;
    ScopedCb& operator=(const ScopedCb&) = delete;
    ScopedCb(ScopedCb&&)                 = delete;
    ScopedCb& operator=(ScopedCb&&)      = delete;

    explicit operator bool() const { return ptr_ != nullptr; }

    T* operator->()             { return ptr_; }
    const T* operator->() const { return ptr_; }
    T& operator*()              { return *ptr_; }
    const T& operator*()  const { return *ptr_; }
    T* get()                    { return ptr_; }
    const T* get()        const { return ptr_; }

private:
    gfx::IGFXDevice*  gfx_;
    gfx::BufferHandle handle_;
    T*                ptr_;
};

}
