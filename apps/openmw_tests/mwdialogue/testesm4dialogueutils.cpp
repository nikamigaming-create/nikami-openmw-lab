#include <apps/openmw/mwdialogue/esm4dialogueutils.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(Esm4DialogueUtilsTest, UsesSelectedInfoPromptForPlayerFacingChoice)
    {
        ESM4::Dialogue dialogue;
        dialogue.mEditorId = "VFreeformGoodSpringsDocMitchellSpeechCheck";
        dialogue.mTopicName = "< Speech 25 >";
        ESM4::DialogInfo info;
        info.mPrompt = "Isn't it customary for a doctor to prescribe follow-up medication?";

        EXPECT_EQ(MWDialogue::getEsm4DialoguePrompt(dialogue, info), info.mPrompt);
    }

    TEST(Esm4DialogueUtilsTest, FallsBackToTopicAndEditorNames)
    {
        ESM4::Dialogue dialogue;
        dialogue.mEditorId = "GoodspringsFallback";
        dialogue.mTopicName = "Ask about Goodsprings.";
        ESM4::DialogInfo info;

        EXPECT_EQ(MWDialogue::getEsm4DialoguePrompt(dialogue, info), dialogue.mTopicName);
        dialogue.mTopicName.clear();
        EXPECT_EQ(MWDialogue::getEsm4DialoguePrompt(dialogue, info), dialogue.mEditorId);
    }
}
