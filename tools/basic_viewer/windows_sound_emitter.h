#pragma once

#include "common_types.h"
#include "sound_emitter.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace whiteout::flakes::io { class IContentProvider; }

namespace whiteout::flakes {

using namespace whiteout::flakes::io;
using namespace whiteout::flakes::renderer;

class WindowsSoundEmitter : public ISoundEmitter {
public:
    explicit WindowsSoundEmitter(const IContentProvider* content);
    ~WindowsSoundEmitter() override;

    WindowsSoundEmitter(const WindowsSoundEmitter&)            = delete;
    WindowsSoundEmitter& operator=(const WindowsSoundEmitter&) = delete;

    void Play(const io::SndEntry& entry, const Vector3f& worldPos) override;

    void  SetVolume(f32 v) override;
    f32 GetVolume() const override;

private:
    const IContentProvider* content_ = nullptr;

    std::mutex                 mu_;
    std::vector<u8>            currentBuffer_;
    std::atomic<f32>           volume_{1.0f};
};

}
