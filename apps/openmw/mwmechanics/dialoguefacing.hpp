#ifndef OPENMW_MECHANICS_DIALOGUEFACING_H
#define OPENMW_MECHANICS_DIALOGUEFACING_H

namespace MWMechanics
{
    struct DialogueFacingPolicy
    {
        bool mTrackHead = false;
        bool mStopMovement = false;
        bool mTurnBody = false;
    };

    constexpr DialogueFacingPolicy makeDialogueFacingPolicy(
        bool dialogueModeActive, bool actorAvailable, bool actorInPlayerCell, bool furnitureConstrained)
    {
        const bool active = dialogueModeActive && actorAvailable && actorInPlayerCell;
        return {
            .mTrackHead = active,
            .mStopMovement = active,
            .mTurnBody = active && !furnitureConstrained,
        };
    }

    constexpr bool shouldClearDialogueFacing(int actorId, int activeActorId, int previousActorId)
    {
        return actorId >= 0 && actorId != activeActorId && actorId == previousActorId;
    }
}

#endif
