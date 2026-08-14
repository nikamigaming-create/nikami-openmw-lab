#ifndef GAME_MWWORLD_ACTORFACING_H
#define GAME_MWWORLD_ACTORFACING_H

#include <osg/Math>
#include <osg/Vec3f>

namespace MWWorld
{
    /// Convert gameplay yaw to the model/physics basis used by an imported actor.
    [[nodiscard]] inline float getActorModelYaw(
        float gameplayYaw, bool tes4Npc, bool falloutNpc, bool falloutCreature) noexcept
    {
        // Imported humanoid rigs author forward on local +X. OSG applies the basis conversion around -Z, so
        // +90 degrees maps the rendered front to the gameplay +Y forward used by movement and combat.
        if (tes4Npc || falloutNpc)
            return gameplayYaw + osg::PI_2f;

        // FNV creature rigs use the opposite authored planar basis from humanoids.
        if (falloutCreature)
            return gameplayYaw - osg::PI_2f;
        return gameplayYaw;
    }

    /// Return the authored visual-front axis consumed by the model-basis yaw conversion above.
    [[nodiscard]] inline osg::Vec3f getActorModelLocalForward(
        bool tes4Npc, bool falloutNpc, bool falloutCreature) noexcept
    {
        if (tes4Npc || falloutNpc)
            return osg::Vec3f(-1.f, 0.f, 0.f);
        if (falloutCreature)
            return osg::Vec3f(1.f, 0.f, 0.f);
        return osg::Vec3f(0.f, 1.f, 0.f);
    }
}

#endif
