#include <apps/openmw/mwmechanics/dialoguefacing.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(DialogueFacingTest, EnablesHeadTrackingMovementStopAndBodyTurnForStandingSpeaker)
    {
        const MWMechanics::DialogueFacingPolicy policy
            = MWMechanics::makeDialogueFacingPolicy(true, true, true, false);

        EXPECT_TRUE(policy.mTrackHead);
        EXPECT_TRUE(policy.mStopMovement);
        EXPECT_TRUE(policy.mTurnBody);
    }

    TEST(DialogueFacingTest, PreservesFurnitureConstrainedBodyYaw)
    {
        const MWMechanics::DialogueFacingPolicy policy
            = MWMechanics::makeDialogueFacingPolicy(true, true, true, true);

        EXPECT_TRUE(policy.mTrackHead);
        EXPECT_TRUE(policy.mStopMovement);
        EXPECT_FALSE(policy.mTurnBody);
    }

    TEST(DialogueFacingTest, ClearsFacingWhenDialogueClosesReferenceIsLostOrCellChanges)
    {
        const MWMechanics::DialogueFacingPolicy dialogueClosed
            = MWMechanics::makeDialogueFacingPolicy(false, true, true, false);
        const MWMechanics::DialogueFacingPolicy referenceLost
            = MWMechanics::makeDialogueFacingPolicy(true, false, true, false);
        const MWMechanics::DialogueFacingPolicy cellChanged
            = MWMechanics::makeDialogueFacingPolicy(true, true, false, false);

        for (const MWMechanics::DialogueFacingPolicy policy : { dialogueClosed, referenceLost, cellChanged })
        {
            EXPECT_FALSE(policy.mTrackHead);
            EXPECT_FALSE(policy.mStopMovement);
            EXPECT_FALSE(policy.mTurnBody);
        }

        EXPECT_TRUE(MWMechanics::shouldClearDialogueFacing(7, -1, 7));
        EXPECT_FALSE(MWMechanics::shouldClearDialogueFacing(8, -1, 7));
    }
}
