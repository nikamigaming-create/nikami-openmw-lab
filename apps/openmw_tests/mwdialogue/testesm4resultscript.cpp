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

    TEST(Esm4ResultScriptTest, RetainsConditionalQuestSourceAndTopLevelWorldCommands)
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
            "SunnyREF.evp;\r\n"
            "VictorREF.stopcombat player\r\n");

        ASSERT_EQ(script.mCommands.size(), 6);
        EXPECT_EQ(script.mCommands[0].mType, Esm4ResultCommandType::Quest);
        EXPECT_EQ(script.mCommands[0].mSource,
            "if(GetQuestRunning VCG03)\n"
            "SetObjectiveDisplayed VCG03 40 1\n"
            "else\n"
            "SetObjectiveDisplayed VCG02 70 1\n"
            "endif");
        EXPECT_EQ(script.mCommands[1].mType, Esm4ResultCommandType::Unlock);
        EXPECT_EQ(script.mCommands[1].mTarget, "GSGasStationDoorRef");
        EXPECT_EQ(script.mCommands[2].mType, Esm4ResultCommandType::Enable);
        EXPECT_EQ(script.mCommands[2].mTarget, "TrudyRef");
        EXPECT_EQ(script.mCommands[3].mType, Esm4ResultCommandType::Quest);
        EXPECT_EQ(script.mCommands[3].mSource, "set VFreeformGoodsprings.bEnableTrudyDone to 1");
        EXPECT_EQ(script.mCommands[4].mType, Esm4ResultCommandType::EvaluatePackage);
        EXPECT_EQ(script.mCommands[4].mTarget, "SunnyREF");
        EXPECT_EQ(script.mCommands[5].mType, Esm4ResultCommandType::StopCombat);
        EXPECT_EQ(script.mCommands[5].mTarget, "VictorREF");
        EXPECT_EQ(script.mSkippedConditionalCommands, 0);
        EXPECT_FALSE(script.mMalformedControlFlow);
    }

    TEST(Esm4ResultScriptTest, ExecutesExactStripTollInventoryCommandsWithoutGuessingVariableCounts)
    {
        const Esm4ResultScript script = parseEsm4ResultScript(
            "Player.RemoveItem Caps001 400\r\n"
            "Set VFreeformTheStreet02.bPaid to 1\r\n"
            "Set VFreeformTheStreet02.bHarassed to 1\r\n"
            "Set VFreeformTheStreet02.bToll to 0\r\n");

        ASSERT_EQ(script.mCommands.size(), 4);
        EXPECT_EQ(script.mCommands[0].mType, Esm4ResultCommandType::RemoveItem);
        EXPECT_EQ(script.mCommands[0].mTarget, "Player");
        EXPECT_EQ(script.mCommands[0].mItem, "Caps001");
        EXPECT_EQ(script.mCommands[0].mCount, 400);
        EXPECT_TRUE(script.mCommands[0].mSource.empty());
        EXPECT_EQ(script.mCommands[1].mType, Esm4ResultCommandType::Quest);
        EXPECT_EQ(script.mCommands[2].mType, Esm4ResultCommandType::Quest);
        EXPECT_EQ(script.mCommands[3].mType, Esm4ResultCommandType::Quest);

        const Esm4ResultScript variableCount
            = parseEsm4ResultScript("Player.RemoveItem Caps001 VES05.iAllPlayersCaps");
        ASSERT_EQ(variableCount.mCommands.size(), 1);
        EXPECT_EQ(variableCount.mCommands[0].mType, Esm4ResultCommandType::Quest);
        EXPECT_EQ(variableCount.mCommands[0].mSource, "Player.RemoveItem Caps001 VES05.iAllPlayersCaps");
    }
}
