#include <gtest/gtest.h>

#include "apps/openmw/mwworld/actorfacing.hpp"
#include "apps/openmw/mwworld/fnvmovement.hpp"

#include <osg/Math>

namespace MWWorld
{
    TEST(ActorFacingTest, convertsFalloutModelForwardToGameplayForward)
    {
        constexpr float gameplayYaw = 1.25f;
        EXPECT_FLOAT_EQ(getActorModelYaw(gameplayYaw, false, true), gameplayYaw - osg::PI_2f);
    }

    TEST(ActorFacingTest, preservesTes4NpcQuarterTurn)
    {
        constexpr float gameplayYaw = 1.25f;
        EXPECT_FLOAT_EQ(getActorModelYaw(gameplayYaw, true, false), gameplayYaw + osg::PI_2f);
    }

    TEST(ActorFacingTest, keepsOtherActorModelsAlignedWithGameplayYaw)
    {
        constexpr float gameplayYaw = 1.25f;
        EXPECT_FLOAT_EQ(getActorModelYaw(gameplayYaw, false, false), gameplayYaw);
    }

    TEST(FalloutMovementTest, usesRetailBaseAndRunMultiplier)
    {
        EXPECT_FLOAT_EQ(getFalloutWalkSpeed(1.f), 77.f);
        EXPECT_FLOAT_EQ(getFalloutWalkSpeed(1.2f), 92.4f);
        EXPECT_FLOAT_EQ(getFalloutRunSpeed(77.f), 308.f);
    }

    TEST(FalloutMovementTest, parsesBoundedOptionalPlayerOverride)
    {
        EXPECT_FLOAT_EQ(parseFalloutPlayerSpeedScale(nullptr), 1.f);
        EXPECT_FLOAT_EQ(parseFalloutPlayerSpeedScale("3"), 3.f);
        EXPECT_FLOAT_EQ(parseFalloutPlayerSpeedScale("bogus"), 1.f);
        EXPECT_FLOAT_EQ(parseFalloutPlayerSpeedScale("100"), 10.f);
        EXPECT_FLOAT_EQ(parseFalloutPlayerSpeedScale("0"), 0.1f);
    }
}
