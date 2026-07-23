#ifndef GAME_MWWORLD_ACTORFACING_H
#define GAME_MWWORLD_ACTORFACING_H

#include <osg/Math>

namespace MWWorld
{
    /// Convert gameplay yaw to the model/physics basis used by an imported actor.
    [[nodiscard]] inline float getActorModelYaw(float gameplayYaw, bool tes4Npc) noexcept
    {
        return gameplayYaw + (tes4Npc ? osg::PI_2f : 0.f);
    }
}

#endif
