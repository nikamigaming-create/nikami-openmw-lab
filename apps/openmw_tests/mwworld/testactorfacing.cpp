#include <gtest/gtest.h>

#include "apps/openmw/mwworld/actorfacing.hpp"
#include "apps/openmw/mwworld/fnvmovement.hpp"

#include <cmath>

#include <osg/Math>
#include <osg/Quat>

namespace MWWorld
{
    TEST(ActorFacingTest, convertsFalloutNpcModelForwardToGameplayForward)
    {
        constexpr float gameplayYaw = 1.25f;
        EXPECT_FLOAT_EQ(getActorModelYaw(gameplayYaw, false, true, false), gameplayYaw + osg::PI_2f);
        const osg::Vec3f renderedForward
            = osg::Quat(getActorModelYaw(gameplayYaw, false, true, false), osg::Vec3f(0.f, 0.f, -1.f))
            * getActorModelLocalForward(false, true, false);
        EXPECT_NEAR(renderedForward.x(), std::sin(gameplayYaw), 0.00001f);
        EXPECT_NEAR(renderedForward.y(), std::cos(gameplayYaw), 0.00001f);
    }

    TEST(ActorFacingTest, convertsFalloutCreatureModelForwardToGameplayForward)
    {
        constexpr float gameplayYaw = 1.25f;
        EXPECT_FLOAT_EQ(getActorModelYaw(gameplayYaw, false, false, true), gameplayYaw - osg::PI_2f);
        const osg::Vec3f renderedForward
            = osg::Quat(getActorModelYaw(gameplayYaw, false, false, true), osg::Vec3f(0.f, 0.f, -1.f))
            * getActorModelLocalForward(false, false, true);
        EXPECT_NEAR(renderedForward.x(), std::sin(gameplayYaw), 0.00001f);
        EXPECT_NEAR(renderedForward.y(), std::cos(gameplayYaw), 0.00001f);
    }

    TEST(ActorFacingTest, preservesTes4NpcQuarterTurn)
    {
        constexpr float gameplayYaw = 1.25f;
        EXPECT_FLOAT_EQ(getActorModelYaw(gameplayYaw, true, false, false), gameplayYaw + osg::PI_2f);
    }

    TEST(ActorFacingTest, keepsOtherActorModelsAlignedWithGameplayYaw)
    {
        constexpr float gameplayYaw = 1.25f;
        EXPECT_FLOAT_EQ(getActorModelYaw(gameplayYaw, false, false, false), gameplayYaw);
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
