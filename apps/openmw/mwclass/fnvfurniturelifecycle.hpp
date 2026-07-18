#ifndef GAME_MWCLASS_FNVFURNITURELIFECYCLE_H
#define GAME_MWCLASS_FNVFURNITURELIFECYCLE_H

#include <cmath>
#include <string_view>
#include <vector>

#include "esm4npc.hpp"

namespace MWClass
{
    // The AI package owns the furniture phase while it is active. Scene reloads and
    // animation-controller recreation may reset the public actor state, but they must
    // not silently advance or release the package's claim.
    enum class FalloutFurniturePackagePhase
    {
        Approach,
        Entering,
        Seated
    };

    struct FalloutFurnitureLifecycleAction
    {
        FalloutFurnitureState mPublishedState = FalloutFurnitureState::None;
        bool mPublishState = false;
        bool mMaintainAnchor = false;
    };

    constexpr FalloutFurnitureState getPublishedFalloutFurnitureState(FalloutFurniturePackagePhase phase)
    {
        switch (phase)
        {
            case FalloutFurniturePackagePhase::Approach:
                return FalloutFurnitureState::Approaching;
            case FalloutFurniturePackagePhase::Entering:
                return FalloutFurnitureState::Entering;
            case FalloutFurniturePackagePhase::Seated:
                return FalloutFurnitureState::Seated;
        }
        return FalloutFurnitureState::None;
    }

    constexpr FalloutFurnitureLifecycleAction reconcileFalloutFurnitureLifecycle(
        FalloutFurniturePackagePhase phase, FalloutFurnitureState publishedState)
    {
        const FalloutFurnitureState desiredState = getPublishedFalloutFurnitureState(phase);
        return {
            .mPublishedState = desiredState,
            .mPublishState = publishedState != desiredState,
            .mMaintainAnchor = phase == FalloutFurniturePackagePhase::Seated,
        };
    }

    constexpr bool needsFalloutFurnitureIdleRefresh(
        FalloutFurnitureState state, std::string_view currentIdle)
    {
        return (state == FalloutFurnitureState::Seated) != (currentIdle == "chairsit");
    }

    constexpr bool shouldRetainFalloutFurnitureClaim(
        FalloutFurnitureState state, bool placementValid, bool sameFurniture)
    {
        return state != FalloutFurnitureState::None && placementValid && sameFurniture;
    }

    inline bool needsFalloutFurnitureAnchorRecovery(const FalloutFurnitureLifecycleAction& action,
        const osg::Vec3f& currentPosition, float currentYaw, const osg::Vec3f& anchorPosition, float anchorYaw,
        float positionTolerance = 0.01f, float yawTolerance = 0.001f)
    {
        if (!action.mMaintainAnchor)
            return false;

        const osg::Vec3f positionDelta = currentPosition - anchorPosition;
        constexpr float twoPi = 6.28318530717958647692f;
        const float yawDelta = std::remainder(currentYaw - anchorYaw, twoPi);
        return action.mPublishState || positionDelta.length2() > positionTolerance * positionTolerance
            || std::abs(yawDelta) > yawTolerance;
    }
}

#endif // GAME_MWCLASS_FNVFURNITURELIFECYCLE_H
