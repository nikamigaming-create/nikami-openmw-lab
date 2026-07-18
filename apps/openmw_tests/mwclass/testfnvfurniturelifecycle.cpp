#include <apps/openmw/mwclass/fnvfurniturelifecycle.hpp>

#include <gtest/gtest.h>

namespace
{
    using MWClass::FalloutFurniturePackagePhase;
    using MWClass::FalloutFurnitureState;

    TEST(FnvFurnitureLifecycleTest, UnloadReloadReassertsSeatedPhaseAndAnchorOwnership)
    {
        const MWClass::FalloutFurnitureLifecycleAction action = MWClass::reconcileFalloutFurnitureLifecycle(
            FalloutFurniturePackagePhase::Seated, FalloutFurnitureState::Approaching);

        EXPECT_EQ(action.mPublishedState, FalloutFurnitureState::Seated);
        EXPECT_TRUE(action.mPublishState);
        EXPECT_TRUE(action.mMaintainAnchor);

        const osg::Vec3f anchor(10.f, 20.f, 30.f);
        EXPECT_TRUE(MWClass::needsFalloutFurnitureAnchorRecovery(action, anchor, 0.5f, anchor, 0.5f));
    }

    TEST(FnvFurnitureLifecycleTest, DialogueInterruptionDoesNotResetTheActivePackagePhase)
    {
        const MWClass::FalloutFurnitureLifecycleAction action = MWClass::reconcileFalloutFurnitureLifecycle(
            FalloutFurniturePackagePhase::Seated, FalloutFurnitureState::Seated);

        EXPECT_EQ(action.mPublishedState, FalloutFurnitureState::Seated);
        EXPECT_FALSE(action.mPublishState);
        EXPECT_TRUE(action.mMaintainAnchor);

        const osg::Vec3f anchor(-4.f, 2.f, 8.f);
        EXPECT_FALSE(MWClass::needsFalloutFurnitureAnchorRecovery(action, anchor, -1.f, anchor, -1.f));
    }

    TEST(FnvFurnitureLifecycleTest, SeatedAnchorRecoversPositionAndWrappedYawDrift)
    {
        const MWClass::FalloutFurnitureLifecycleAction seated = MWClass::reconcileFalloutFurnitureLifecycle(
            FalloutFurniturePackagePhase::Seated, FalloutFurnitureState::Seated);
        const osg::Vec3f anchor(1.f, 2.f, 3.f);

        EXPECT_TRUE(MWClass::needsFalloutFurnitureAnchorRecovery(
            seated, osg::Vec3f(1.f, 2.f, 3.02f), 0.25f, anchor, 0.25f));
        EXPECT_TRUE(MWClass::needsFalloutFurnitureAnchorRecovery(seated, anchor, 0.5f, anchor, 0.25f));
        EXPECT_FALSE(MWClass::needsFalloutFurnitureAnchorRecovery(
            seated, anchor, 0.25f + 6.28318530717958647692f, anchor, 0.25f));

        const MWClass::FalloutFurnitureLifecycleAction entering = MWClass::reconcileFalloutFurnitureLifecycle(
            FalloutFurniturePackagePhase::Entering, FalloutFurnitureState::Entering);
        EXPECT_FALSE(MWClass::needsFalloutFurnitureAnchorRecovery(
            entering, osg::Vec3f(100.f, 200.f, 300.f), 2.f, anchor, 0.25f));
    }

    TEST(FnvFurnitureLifecycleTest, FurnitureStateChangesInvalidateTheIdleAnimationCache)
    {
        EXPECT_FALSE(MWClass::needsFalloutFurnitureIdleRefresh(FalloutFurnitureState::None, "idle"));
        EXPECT_TRUE(MWClass::needsFalloutFurnitureIdleRefresh(FalloutFurnitureState::Seated, "idle"));
        EXPECT_FALSE(MWClass::needsFalloutFurnitureIdleRefresh(FalloutFurnitureState::Seated, "chairsit"));
        EXPECT_TRUE(MWClass::needsFalloutFurnitureIdleRefresh(FalloutFurnitureState::Approaching, "chairsit"));
        EXPECT_TRUE(MWClass::needsFalloutFurnitureIdleRefresh(FalloutFurnitureState::Exiting, "chairsit"));
    }

    TEST(FnvFurnitureLifecycleTest, SceneReloadRetainsOnlyTheMatchingActiveFurnitureClaim)
    {
        EXPECT_TRUE(MWClass::shouldRetainFalloutFurnitureClaim(FalloutFurnitureState::Seated, true, true));
        EXPECT_TRUE(MWClass::shouldRetainFalloutFurnitureClaim(FalloutFurnitureState::Entering, true, true));
        EXPECT_FALSE(MWClass::shouldRetainFalloutFurnitureClaim(FalloutFurnitureState::None, true, true));
        EXPECT_FALSE(MWClass::shouldRetainFalloutFurnitureClaim(FalloutFurnitureState::Seated, false, true));
        EXPECT_FALSE(MWClass::shouldRetainFalloutFurnitureClaim(FalloutFurnitureState::Seated, true, false));
    }
}
