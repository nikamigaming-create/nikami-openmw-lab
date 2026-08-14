#include <gtest/gtest.h>

#include "apps/openmw/mwworld/actorfacing.hpp"
#include "apps/openmw/mwworld/fnvmovement.hpp"

#include <osg/Math>

namespace MWWorld
{
    TEST(ActorFacingTest, convertsFalloutHumanModelForwardToGameplayForward)
    {
        constexpr float gameplayYaw = 1.25f;
        EXPECT_FLOAT_EQ(getActorModelYaw(gameplayYaw, false, true), gameplayYaw + osg::PI_2f);
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

    TEST(ActorFacingTest, keepsFalloutAccumulationRootInBindBasis)
    {
        const osg::Quat sampled(0.37f, osg::Vec3f(0.f, 0.f, 1.f));
        const osg::Quat bind(osg::PI_2f, osg::Vec3f(0.f, 0.f, 1.f));

        const osg::Quat actual = getActorAnimationRootRotation(sampled, bind, true);
        EXPECT_FLOAT_EQ(actual.x(), bind.x());
        EXPECT_FLOAT_EQ(actual.y(), bind.y());
        EXPECT_FLOAT_EQ(actual.z(), bind.z());
        EXPECT_FLOAT_EQ(actual.w(), bind.w());
    }

    TEST(ActorFacingTest, preservesNonFalloutAnimationRootRotation)
    {
        const osg::Quat sampled(0.37f, osg::Vec3f(0.f, 0.f, 1.f));
        const osg::Quat bind(osg::PI_2f, osg::Vec3f(0.f, 0.f, 1.f));

        const osg::Quat actual = getActorAnimationRootRotation(sampled, bind, false);
        EXPECT_FLOAT_EQ(actual.x(), sampled.x());
        EXPECT_FLOAT_EQ(actual.y(), sampled.y());
        EXPECT_FLOAT_EQ(actual.z(), sampled.z());
        EXPECT_FLOAT_EQ(actual.w(), sampled.w());
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
