#include "renderer/particle/emitter_factory.h"

#include "renderer/particle/base/child_model_emitter.h"
#include "renderer/particle/base/model_particle_emitter.h"
#include "renderer/particle/base/particle2_emitter.h"
#include "renderer/particle/d3/d3_emitter.h"
#include "renderer/particle/sc2/sc2_model_particle_emitter.h"

namespace whiteout::flakes::renderer::particle {

namespace EmitterFactory {

std::unique_ptr<Emitter2> Create(std::shared_ptr<const EmitterDesc> desc,
                                 const core::ParticleBehavior& behavior, u32 seed,
                                 ChildModelOwner child) {
    const bool sc2 = desc && desc->family == EmitterDesc::Family::Sc2;
    std::unique_ptr<Emitter2> em;
    if (desc && desc->output == ParticleOutput::ChildModel) {
        if (sc2)
            em = std::make_unique<Sc2ModelParticleEmitter>(child.owner, child.emitterId,
                                                           std::move(child.allocHandle));
        else if (desc->childModelKind == EmitterDesc::ChildModelKind::M2)
            em = std::make_unique<ModelParticleEmitter>(child.owner, child.emitterId,
                                                        std::move(child.allocHandle));
        else
            em = std::make_unique<ChildModelEmitter>(child.owner, child.emitterId,
                                                     std::move(child.allocHandle));
    } else {
        em = std::make_unique<Emitter2>();
    }
    em->SetDesc(std::move(desc));
    if (!sc2)
        em->SetBehavior(behavior);
    em->SetSeed(seed);
    return em;
}

std::unique_ptr<d3::Emitter> CreateD3(std::shared_ptr<const d3::EmitterDesc> desc, i32 bone,
                                      const Matrix44f& offset, u32 seed, ChildModelOwner child) {
    auto em = std::make_unique<d3::Emitter>();
    em->SetD3Desc(std::move(desc));
    em->SetSystemSeed(seed);
    em->SetAttachBone(bone);
    em->SetAttachOffset(offset);
    if (child.allocHandle)
        em->SetChildOwner(child.owner, child.emitterId, std::move(child.allocHandle));
    return em;
}

} // namespace EmitterFactory

} // namespace whiteout::flakes::renderer::particle
