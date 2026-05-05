#pragma once

#include "common_types.h"
#include "types.h"

#include <memory>

namespace WhiteoutDex {

namespace io { struct SndEntry; }

class ISoundEmitter {
public:
    virtual ~ISoundEmitter() = default;

    virtual void Play(const io::SndEntry& entry, const Vector3f& worldPos) = 0;

    virtual void SetVolume(f32) {}
    virtual f32  GetVolume() const { return 1.0f; }
};

class NullSoundEmitter final : public ISoundEmitter {
public:
    void Play(const io::SndEntry&, const Vector3f&) override {}
};

inline std::unique_ptr<ISoundEmitter> MakeNullSoundEmitter() {
    return std::make_unique<NullSoundEmitter>();
}

}
