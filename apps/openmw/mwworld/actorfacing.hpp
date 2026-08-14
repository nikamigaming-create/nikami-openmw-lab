#ifndef GAME_MWWORLD_ACTORFACING_H
#define GAME_MWWORLD_ACTORFACING_H

#include <osg/Math>
#include <osg/Quat>

namespace MWWorld
{
    /// Convert gameplay yaw to the model/physics basis used by an imported actor.
    [[nodiscard]] inline float getActorModelYaw(float gameplayYaw, bool tes4Npc, bool falloutNpc) noexcept
    {
        if (tes4Npc)
            return gameplayYaw + osg::PI_2f;
        if (falloutNpc)
            // FO3/FNV human skeletons face local -X after their accumulation
            // root is neutralised. Rotate that authored axis onto OpenMW's
            // gameplay +Y heading at the render/physics boundary.
            return gameplayYaw + osg::PI_2f;
        return gameplayYaw;
    }

    /// Fallout's Bip01 controller is an accumulation/root-motion channel. Its sampled rotation must not redefine
    /// the model-space forward axis; gameplay owns actor heading and the rendered hierarchy keeps its NIF bind basis.
    [[nodiscard]] inline osg::Quat getActorAnimationRootRotation(
        const osg::Quat& sampledRotation, const osg::Quat& bindRotation, bool falloutNpc) noexcept
    {
        return falloutNpc ? bindRotation : sampledRotation;
    }
}

#endif
