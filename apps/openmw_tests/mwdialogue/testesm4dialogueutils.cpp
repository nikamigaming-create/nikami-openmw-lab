#include <apps/openmw/mwdialogue/esm4dialogueutils.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

#include <components/esm/formid.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/script.hpp>

#include <apps/openmw/mwbase/environment.hpp>

#include <apps/openmw/mwclass/classes.hpp>

#include <apps/openmw/mwworld/esm4questruntime.hpp>
#include <apps/openmw/mwworld/esmstore.hpp>
#include <apps/openmw/mwworld/fnvplayerruntimestate.hpp>
#include <apps/openmw/mwworld/livecellref.hpp>
#include <apps/openmw/mwworld/ptr.hpp>

namespace
{
    ESM::FormId form(std::uint32_t value)
    {
        return ESM::FormId::fromUint32(value);
    }

    ESM4::TargetCondition condition(
        std::uint32_t function, ESM::FormId parameter, float comparison = 1.f)
    {
        ESM4::TargetCondition result;
        result.condition = ESM4::CTF_EqualTo;
        result.comparison = comparison;
        result.functionIndex = function;
        result.param1 = parameter.toUint32();
        return result;
    }

    ESM4::Npc actorBase(std::uint32_t id, std::string_view editorId, std::string_view name,
        std::uint32_t race, std::uint32_t voiceType, std::initializer_list<std::uint32_t> factions)
    {
        ESM4::Npc result;
        result.mId = form(id);
        result.mEditorId = editorId;
        result.mFullName = name;
        result.mIsFONV = true;
        result.mHasFNVData = true;
        result.mFNVData.health = 100;
        result.mBaseConfig.fo3.levelOrMult = 1;
        result.mRace = form(race);
        result.mVoiceType = form(voiceType);
        for (const std::uint32_t faction : factions)
            result.mFactions.push_back(ESM4::ActorFaction{ faction, 0, 0, 0, 0 });
        return result;
    }

    ESM4::ActorCharacter actorReference(std::uint32_t id, ESM::FormId base)
    {
        ESM4::ActorCharacter result{};
        result.mId = form(id);
        result.mBaseObj = base;
        return result;
    }

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

    TEST(Esm4DialogueUtilsTest, RightSideTopicRetainsTheExactInfoUsedForItsVisiblePrompt)
    {
        // Exact normalized FalloutNV.esm Easy Pete records observed in the Save330 runtime:
        // VFreeformGoodspringsGSEasyPeteTopic000 DIAL 01105159 selects INFO 0110515C.
        const MWDialogue::Esm4DialogueSelection displayed{ form(0x01105159), form(0x0110515c) };
        MWDialogue::Esm4DialoguePicker picker;
        ASSERT_TRUE(picker.bindTopic("Why are you called Easy Pete?", displayed));

        const std::optional<MWDialogue::Esm4DialogueSelection> activated
            = picker.selectTopic("why are you called easy pete?");
        ASSERT_TRUE(activated.has_value());
        EXPECT_EQ(*activated, displayed);

        ESM4::DialogInfo displayedInfo;
        displayedInfo.mId = form(0x0110515c);
        displayedInfo.mTopic = form(0x01105159);
        EXPECT_TRUE(MWDialogue::matchesEsm4DialogueSelection(*activated, displayedInfo));

        ESM4::DialogInfo laterFallback = displayedInfo;
        laterFallback.mId = form(0x0110515d);
        EXPECT_FALSE(MWDialogue::matchesEsm4DialogueSelection(*activated, laterFallback));
    }

    TEST(Esm4DialogueUtilsTest, NestedChoiceOwnsThePickerUntilItReturnsToRightSideTopics)
    {
        // Exact FalloutNV.esm Victor Science branch: INFO 0115FF4B offers TCLT 0110B192,
        // whose authored response is INFO 0110B19F.
        const MWDialogue::Esm4DialogueSelection rightSide{ form(0x01105159), form(0x0110515c) };
        const MWDialogue::Esm4DialogueSelection nested{ form(0x0110b192), form(0x0110b19f) };
        MWDialogue::Esm4DialoguePicker picker;
        ASSERT_TRUE(picker.bindTopic("Why are you called Easy Pete?", rightSide));
        EXPECT_EQ(picker.bindChoice(nested), 0);

        EXPECT_FALSE(picker.selectTopic("Why are you called Easy Pete?").has_value());
        ASSERT_TRUE(picker.selectChoice(0).has_value());
        EXPECT_EQ(*picker.selectChoice(0), nested);
        EXPECT_FALSE(picker.selectChoice(-1).has_value());
        EXPECT_FALSE(picker.selectChoice(1).has_value());

        picker.clearChoices();
        ASSERT_TRUE(picker.selectTopic("Why are you called Easy Pete?").has_value());
        EXPECT_EQ(*picker.selectTopic("Why are you called Easy Pete?"), rightSide);

        // Starting a new actor/dialogue calls clear(), so no prior actor's label or choice can remain selectable.
        picker.bindChoice(nested);
        picker.clear();
        EXPECT_TRUE(picker.getTopics().empty());
        EXPECT_FALSE(picker.selectTopic("Why are you called Easy Pete?").has_value());
        EXPECT_FALSE(picker.selectChoice(0).has_value());
    }

    TEST(Esm4DialogueUtilsTest, DuplicateVisibleTopicCannotReplaceItsDisplayedSelection)
    {
        // These two exact Goodsprings DIALs share a visible label but belong to Easy Pete and Chet.
        const MWDialogue::Esm4DialogueSelection easyPete{ form(0x01106632), form(0x01106635) };
        const MWDialogue::Esm4DialogueSelection chet{ form(0x011084cc), form(0x011084cd) };
        MWDialogue::Esm4DialoguePicker picker;
        ASSERT_TRUE(picker.bindTopic("Do you know anything about the people who attacked me?", easyPete));
        EXPECT_FALSE(picker.bindTopic("do you know anything about the people who attacked me?", chet));

        const std::optional<MWDialogue::Esm4DialogueSelection> activated
            = picker.selectTopic("Do you know anything about the people who attacked me?");
        ASSERT_TRUE(activated.has_value());
        EXPECT_EQ(*activated, easyPete);
    }

    TEST(Esm4DialogueUtilsTest, NativePlayerValuesResolveExactGoodspringsSkillAndSpecialConditions)
    {
        MWWorld::FalloutPlayerState base;
        base.mBaseRecord = form(7);
        base.mHealth = 100;
        base.mSpecial = { 5, 5, 5, 5, 6, 5, 5 };
        base.mSkillValues = { 24, 14, 12, 25, 15, 30, 30, 13, 25, 25, 25, 30, 22, 29 };

        MWWorld::FalloutPlayerRuntimeState runtime;
        runtime.initialize(base);

        // FalloutNV.esm INFO 0015FF4B (Victor Science), 00104C4B (Easy Pete Explosives), and 0015EC53
        // (Joe Cobb Intelligence) are representative of the 31 GetActorValue CTDAs owned by VFreeformGoodsprings.
        const std::array<ESM4::TargetCondition, 4> conditions{
            condition(ESM4::FUN_GetActorValue, form(0), 25.f),
            condition(ESM4::FUN_GetActorValue, form(0), 25.f),
            condition(ESM4::FUN_GetActorValue, form(0), 6.f),
            condition(ESM4::FUN_GetActorValue, form(0), 25.f),
        };
        std::array<ESM4::TargetCondition, 4> exact = conditions;
        exact[0].param1 = 40; // Science 25 succeeds.
        exact[1].param1 = 35; // Explosives 25 succeeds.
        exact[2].param1 = 9; // Intelligence 6 succeeds.
        exact[3].param1 = 32; // Barter 24 fails a 25 check.

        const auto currentValue = [&](const ESM4::TargetCondition& target) -> std::optional<bool> {
            const std::optional<MWWorld::FalloutRuntimeActorValue> value
                = runtime.getCurrentActorValue(target.param1);
            if (!value)
                return std::nullopt;
            return value->mValue >= target.comparison;
        };
        EXPECT_TRUE(currentValue(exact[0]).value_or(false));
        EXPECT_TRUE(currentValue(exact[1]).value_or(false));
        EXPECT_TRUE(currentValue(exact[2]).value_or(false));
        EXPECT_FALSE(currentValue(exact[3]).value_or(true));

        exact[0].param1 = 31;
        EXPECT_FALSE(currentValue(exact[0]).has_value()) << "unsupported actor values must remain fail-closed";
    }

    TEST(Esm4DialogueUtilsTest, RetailLegionInfoRequiresRunningOwnerAndMatchingFactionBeforeLocalConditions)
    {
        MWBase::Environment environment;
        MWWorld::ESMStore store;
        environment.setESMStore(store);
        store.setUp();
        MWClass::registerClasses();

        // Frozen FalloutNV.esm records:
        // - 00125EA2 VDialogueReactivityLegion owns INFO 00175650 and requires Caesar's Legion faction 000EE68A.
        // - 00104F02/00104F03 are an ordinary Goodsprings Settler base/reference.
        // - 0012953B/00179134 are a Veteran Legionary base/reference with that faction.
        const ESM::FormId ownerId = form(0x00125ea2);
        ESM4::Quest owner;
        owner.mId = ownerId;
        owner.mEditorId = "VDialogueReactivityLegion";
        owner.mQuestName = "Caesar's Legion Story Reactivity Dialogue";
        owner.mData.flags = 0;
        owner.mTargetConditions.push_back(condition(ESM4::FUN_GetInFaction, form(0x000ee68a)));
        store.overrideRecord(owner);

        ESM4::Race africanAmerican{};
        africanAmerican.mId = form(0x0000424a);
        africanAmerican.mEditorId = "AfricanAmerican";
        africanAmerican.mFullName = "African American";
        store.overrideRecord(africanAmerican);
        ESM4::Race caucasian{};
        caucasian.mId = form(0x00000019);
        caucasian.mEditorId = "Caucasian";
        caucasian.mFullName = "Caucasian";
        store.overrideRecord(caucasian);

        ESM4::DialogInfo info;
        info.mId = form(0x00175650);
        info.mQuest = ownerId;
        constexpr std::array<std::uint32_t, 11> excludedVoiceTypes{ 0x000717e1, 0x0014f3e5, 0x00094eed,
            0x000717e3, 0x0017355e, 0x00173561, 0x0017355f, 0x0013c8ce, 0x0013c8d1, 0x00176578,
            0x00176579 };
        for (const std::uint32_t voiceType : excludedVoiceTypes)
            info.mTargetConditions.push_back(condition(ESM4::FUN_GetIsVoiceType, form(voiceType), 0.f));

        ESM4::Npc goodsprings = actorBase(0x00104f02, "GSSettlerAAM", "Goodsprings Settler", 0x0000424a,
            0x0002ab62,
            { 0x0013f89e, 0x0013f89b, 0x00104c6e, 0x0016311a });
        ESM4::ActorCharacter goodspringsRef = actorReference(0x00104f03, goodsprings.mId);
        MWWorld::LiveCellRef<ESM4::Npc> liveGoodsprings(goodspringsRef, &goodsprings);
        const MWWorld::Ptr goodspringsPtr(&liveGoodsprings);

        ESM4::Npc legion = actorBase(0x0012953b, "FortLegionaryVeteranRangedCM", "Veteran Legionary",
            0x00000019, 0x0013c8d9, { 0x00134b6e, 0x000ee68a, 0x00137bb7, 0x00140d4e, 0x00154418 });
        ESM4::ActorCharacter legionRef = actorReference(0x00179134, legion.mId);
        MWWorld::LiveCellRef<ESM4::Npc> liveLegion(legionRef, &legion);
        const MWWorld::Ptr legionPtr(&liveLegion);

        MWWorld::ESM4QuestRuntime runtime;
        runtime.initialize(store);
        const MWWorld::ESM4QuestState* ownerState = runtime.search(ownerId);
        ASSERT_NE(ownerState, nullptr);
        ASSERT_EQ(ownerState->mFlags & MWWorld::ESM4QuestState::Flag_Running, 0);

        std::size_t stoppedEvaluations = 0;
        const MWDialogue::Esm4DialogueConditionEvaluator stoppedEvaluator
            = [&](const ESM4::TargetCondition& target) {
                  ++stoppedEvaluations;
                  return MWDialogue::evaluateEsm4ActorDialogueCondition(target, legionPtr, false);
              };
        EXPECT_FALSE(MWDialogue::matchesEsm4DialogueInfoConditions(info, &owner, ownerState, stoppedEvaluator));
        EXPECT_EQ(stoppedEvaluations, 0) << "a stopped owning quest must reject before evaluating any CTDA";

        ASSERT_TRUE(runtime.startQuest(ownerId));
        ownerState = runtime.search(ownerId);
        ASSERT_NE(ownerState, nullptr);
        ASSERT_NE(ownerState->mFlags & MWWorld::ESM4QuestState::Flag_Running, 0);

        std::size_t goodspringsFactionChecks = 0;
        std::size_t goodspringsInfoChecks = 0;
        const MWDialogue::Esm4DialogueConditionEvaluator goodspringsEvaluator
            = [&](const ESM4::TargetCondition& target) {
                  goodspringsFactionChecks += target.functionIndex == ESM4::FUN_GetInFaction;
                  goodspringsInfoChecks += target.functionIndex == ESM4::FUN_GetIsVoiceType;
                  return MWDialogue::evaluateEsm4ActorDialogueCondition(target, goodspringsPtr, false);
              };
        EXPECT_FALSE(MWDialogue::matchesEsm4DialogueInfoConditions(info, &owner, ownerState, goodspringsEvaluator));
        EXPECT_EQ(goodspringsFactionChecks, 1);
        EXPECT_EQ(goodspringsInfoChecks, 0) << "owner CTDA failure must reject before INFO-local CTDAs";

        std::size_t legionFactionChecks = 0;
        std::size_t legionInfoChecks = 0;
        const MWDialogue::Esm4DialogueConditionEvaluator legionEvaluator
            = [&](const ESM4::TargetCondition& target) {
                  legionFactionChecks += target.functionIndex == ESM4::FUN_GetInFaction;
                  legionInfoChecks += target.functionIndex == ESM4::FUN_GetIsVoiceType;
                  return MWDialogue::evaluateEsm4ActorDialogueCondition(target, legionPtr, false);
              };
        EXPECT_TRUE(MWDialogue::matchesEsm4DialogueInfoConditions(info, &owner, ownerState, legionEvaluator));
        EXPECT_EQ(legionFactionChecks, 1);
        EXPECT_EQ(legionInfoChecks, excludedVoiceTypes.size());
    }

    TEST(Esm4DialogueExpressionTest, StoresRetailEmotionAndClampsAuthoredWeight)
    {
        int actorToken = 0;
        MWDialogue::setEsm4DialogueExpression(&actorToken, ESM4::EMO_Happy, 150);
        const auto expression = MWDialogue::getEsm4DialogueExpression(&actorToken);
        ASSERT_TRUE(expression.has_value());
        EXPECT_EQ(expression->mType, ESM4::EMO_Happy);
        EXPECT_FLOAT_EQ(expression->mWeight, 1.f);
    }
}
