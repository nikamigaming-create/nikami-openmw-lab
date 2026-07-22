#include <gtest/gtest.h>

#include "apps/openmw/mwworld/actorfacing.hpp"

#include <osg/Math>

namespace MWWorld
{
    TEST(ActorFacingTest, keepsFalloutNpcModelAlignedWithGameplayYaw)
    {
        constexpr float gameplayYaw = 1.25f;
        EXPECT_FLOAT_EQ(getActorModelYaw(gameplayYaw, false), gameplayYaw);
    }

    TEST(ActorFacingTest, preservesTes4NpcQuarterTurn)
    {
        constexpr float gameplayYaw = 1.25f;
        EXPECT_FLOAT_EQ(getActorModelYaw(gameplayYaw, true), gameplayYaw + osg::PI_2f);
    }
}
