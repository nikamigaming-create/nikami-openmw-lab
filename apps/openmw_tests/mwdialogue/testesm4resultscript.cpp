#include <gtest/gtest.h>

#include <apps/openmw/mwdialogue/esm4resultscript.hpp>

namespace MWDialogue
{
    TEST(Esm4ResultScriptTest, OpensBarterWhenEveryRetailChetBranchOpensBarter)
    {
        const Esm4ResultScript script = parseEsm4ResultScript(
            "if (GetReputationThreshold RepNVGoodsprings 0 > 2)\r\n"
            "  ShowBarterMenu 75\r\n"
            "elseif (GetStageDone VMS16b 100)\r\n"
            "  ShowBarterMenu 50\r\n"
            "else\r\n"
            "  ShowBarterMenu\r\n"
            "endif");

        ASSERT_EQ(script.mCommands.size(), 1);
        EXPECT_EQ(script.mCommands.front().mType, Esm4ResultCommandType::ShowBarterMenu);
        EXPECT_FALSE(script.mMalformedControlFlow);
    }

    TEST(Esm4ResultScriptTest, DoesNotGuessAtConditionalOnlyBarter)
    {
        const Esm4ResultScript script
            = parseEsm4ResultScript("if GetStageDone VMS16b 100\nShowBarterMenu\nendif");

        EXPECT_TRUE(script.mCommands.empty());
        EXPECT_FALSE(script.mMalformedControlFlow);
    }

    TEST(Esm4ResultScriptTest, RetainsTopLevelWorldCommandsAndSkipsUnknownBranches)
    {
        const Esm4ResultScript script = parseEsm4ResultScript(
            "if(GetQuestRunning VCG03)\r\n"
            "  SetObjectiveDisplayed VCG03 40 1\r\n"
            "else\r\n"
            "  SetObjectiveDisplayed VCG02 70 1\r\n"
            "endif\r\n"
            "GSGasStationDoorRef.Unlock\r\n"
            "TrudyRef.Enable\r\n"
            "set VFreeformGoodsprings.bEnableTrudyDone to 1\r\n"
            "SunnyREF.evp;\r\n");

        ASSERT_EQ(script.mCommands.size(), 4);
        EXPECT_EQ(script.mCommands[0].mType, Esm4ResultCommandType::Unlock);
        EXPECT_EQ(script.mCommands[0].mTarget, "GSGasStationDoorRef");
        EXPECT_EQ(script.mCommands[1].mType, Esm4ResultCommandType::Enable);
        EXPECT_EQ(script.mCommands[1].mTarget, "TrudyRef");
        EXPECT_EQ(script.mCommands[2].mType, Esm4ResultCommandType::Quest);
        EXPECT_EQ(script.mCommands[2].mSource, "set VFreeformGoodsprings.bEnableTrudyDone to 1");
        EXPECT_EQ(script.mCommands[3].mType, Esm4ResultCommandType::EvaluatePackage);
        EXPECT_EQ(script.mCommands[3].mTarget, "SunnyREF");
        EXPECT_EQ(script.mSkippedConditionalCommands, 2);
        EXPECT_FALSE(script.mMalformedControlFlow);
    }
}
