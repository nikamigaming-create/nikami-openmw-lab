#ifndef GAME_MWWORLD_ACTORFACING_H
#define GAME_MWWORLD_ACTORFACING_H

#include <osg/Math>

namespace MWWorld
{
    /// Convert gameplay yaw to the model/physics basis used by an imported actor.
    [[nodiscard]] inline float getActorModelYaw(float gameplayYaw, bool tes4Npc, bool falloutActor) noexcept
    {
        if (tes4Npc)
            return gameplayYaw + osg::PI_2f;
        if (falloutActor)
            return gameplayYaw - osg::PI_2f;
        return gameplayYaw;
    }
}

#endif
