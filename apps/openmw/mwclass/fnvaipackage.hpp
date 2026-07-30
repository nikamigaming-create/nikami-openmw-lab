#ifndef OPENMW_MWCLASS_FNVAI_PACKAGE_H
#define OPENMW_MWCLASS_FNVAI_PACKAGE_H

#include <optional>

namespace MWWorld
{
    class Ptr;
}

namespace MWClass
{
    constexpr std::optional<float> getFnvTravelCompletionRadius(int authoredRadius)
    {
        // A zero PLDT radius means "use the engine's normal reachable
        // endpoint completion", not "reach the exact marker origin".
        // Positive radii are explicit content tolerances; retain a small
        // pathfinding floor so they remain practically reachable.
        if (authoredRadius <= 0)
            return std::nullopt;
        return authoredRadius > 8 ? static_cast<float>(authoredRadius) : 8.f;
    }

    // Re-evaluate the authored Fallout package list for a live NPC. This is
    // the runtime contract behind the GECK EvaluatePackage/evp command.
    bool requestFnvAiPackageEvaluation(const MWWorld::Ptr& ptr);

    // Clear every current behavior (including combat, pursuit, pathing, and
    // furniture use) before re-evaluating the authored package list. This is
    // the stronger runtime contract behind the GECK ResetAI command.
    bool resetFnvAiState(const MWWorld::Ptr& ptr);

    // Creature-side implementation used by the actor-generic entry point.
    bool requestFnvCreatureAiPackageEvaluation(const MWWorld::Ptr& ptr);
    bool resetFnvCreatureAiState(const MWWorld::Ptr& ptr);
}

#endif
