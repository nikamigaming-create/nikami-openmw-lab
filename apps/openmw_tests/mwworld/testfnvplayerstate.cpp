#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include <components/esm/attr.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm4/loadnpc.hpp>

#include "apps/openmw/mwworld/fnvplayerstate.hpp"
#include "apps/openmw/mwworld/store.hpp"

namespace
{
    using testing::ElementsAreArray;
    using testing::HasSubstr;

    constexpr ESM::FormId form(std::uint32_t index)
    {
        return ESM::FormId{ index, 0 };
    }

    ESM4::Npc makeCompletePlayer(ESM::FormId id = form(7))
    {
        ESM4::Npc result;
        result.mId = id;
        result.mIsFONV = true;
        result.mEditorId = "Player";
        result.mFullName.clear();
        result.mModel = "Characters\\_Male\\Skeleton.NIF";
        result.mRace = form(0x19);
        result.mClass = form(0x57e6a);
        result.mHair = form(0x2ddee);
        result.mEyes = form(0x4253);
        result.mVoiceType = form(0x2853b);

        result.mHasFNVBaseConfig = true;
        result.mBaseConfig.fo3.flags = 0;
        result.mBaseConfig.fo3.fatigue = 200;
        result.mBaseConfig.fo3.barterGold = 0;
        result.mBaseConfig.fo3.levelOrMult = 1;
        result.mBaseConfig.fo3.calcMinlevel = 1;
        result.mBaseConfig.fo3.calcMaxlevel = 0;
        result.mBaseConfig.fo3.speedMultiplier = 100;
        result.mBaseConfig.fo3.karma = 0.f;
        result.mBaseConfig.fo3.dispositionBase = 0;
        result.mBaseConfig.fo3.templateFlags = 0;

        result.mHasFNVAIData = true;
        result.mFNVAIData.aggression = 0;
        result.mFNVAIData.confidence = 0;
        result.mFNVAIData.energyLevel = 50;
        result.mFNVAIData.responsibility = 50;
        result.mFNVAIData.mood = 0;

        result.mHasFNVData = true;
        result.mFNVData = { 100, 5, 5, 5, 5, 5, 5, 5 };

        result.mHasFNVSkills = true;
        result.mFNVSkills.values = { 13, 14, 12, 15, 15, 14, 30, 13, 14, 25, 22, 14, 22, 29 };
        result.mFNVSkills.offsets = {};
        return result;
    }

    MWWorld::FalloutPlayerStateResolution resolvePlayer(
        const MWWorld::Store<ESM4::Npc>& npcs, std::int32_t contentFile = 0)
    {
        return MWWorld::resolveFalloutPlayerState(npcs, ESM::FormId{ 7, contentFile });
    }

    TEST(FalloutPlayerStateTest, resolvesExactOfficialPlayerIdentityStateAndActorValueComponents)
    {
        MWWorld::Store<ESM4::Npc> npcs;
        npcs.insertStatic(makeCompletePlayer());

        const MWWorld::FalloutPlayerStateResolution resolution = resolvePlayer(npcs);
        ASSERT_TRUE(resolution) << resolution.mError;
        const MWWorld::FalloutPlayerState& state = *resolution.mState;

        EXPECT_EQ(state.mBaseRecord, form(7));
        EXPECT_EQ(state.mTraitsRecord, form(7));
        EXPECT_EQ(state.mStatsRecord, form(7));
        EXPECT_EQ(state.mAIDataRecord, form(7));
        EXPECT_EQ(state.mModelRecord, form(7));
        EXPECT_EQ(state.mBaseDataRecord, form(7));
        EXPECT_EQ(state.mEditorId, "Player");
        EXPECT_TRUE(state.mFullName.empty());
        EXPECT_EQ(state.mModel, "Characters\\_Male\\Skeleton.NIF");
        EXPECT_EQ(state.mRace, form(0x19));
        EXPECT_EQ(state.mClass, form(0x57e6a));
        EXPECT_EQ(state.mHair, form(0x2ddee));
        EXPECT_EQ(state.mEyes, form(0x4253));
        EXPECT_EQ(state.mVoiceType, form(0x2853b));
        EXPECT_EQ(state.mHealth, 100);
        EXPECT_EQ(state.mStatsConfig.fatigue, 200);
        EXPECT_EQ(state.mStatsConfig.levelOrMult, 1);
        EXPECT_THAT(state.mSpecial, ElementsAreArray(std::array<std::uint8_t, 7>{ 5, 5, 5, 5, 5, 5, 5 }));
        EXPECT_THAT(state.mSkillValues,
            ElementsAreArray(std::array<std::uint8_t, 14>{ 13, 14, 12, 15, 15, 14, 30, 13, 14, 25, 22, 14, 22, 29 }));
        EXPECT_THAT(state.mSkillOffsets, ElementsAreArray(std::array<std::uint8_t, 14>{}));

        ASSERT_TRUE(state.getActorValueComponents(2).has_value());
        EXPECT_DOUBLE_EQ(state.getActorValueComponents(2)->mValue, 50.0);
        ASSERT_TRUE(state.getActorValueComponents(9).has_value());
        EXPECT_DOUBLE_EQ(state.getActorValueComponents(9)->mValue, 5.0);
        ASSERT_TRUE(state.getActorValueComponents(16).has_value());
        EXPECT_DOUBLE_EQ(state.getActorValueComponents(16)->mValue, 100.0);
        ASSERT_TRUE(state.getActorValueComponents(43).has_value());
        EXPECT_DOUBLE_EQ(state.getActorValueComponents(43)->mValue, 14.0);
        ASSERT_TRUE(state.getActorValueComponents(43)->mRawSkillOffset.has_value());
        EXPECT_EQ(*state.getActorValueComponents(43)->mRawSkillOffset, 0);
        EXPECT_FALSE(state.getActorValueComponents(12).has_value());
        EXPECT_FALSE(state.getActorValueComponents(31).has_value());
        EXPECT_FALSE(state.getActorValueComponents(46).has_value());
    }

    TEST(FalloutPlayerStateTest, resolvesRetailPlayerAfterBuiltinContentShiftsTheMasterIndex)
    {
        MWWorld::Store<ESM4::Npc> npcs;
        ESM4::Npc wrongIndex = makeCompletePlayer();
        wrongIndex.mEditorId = "NotTheRetailPlayer";
        npcs.insertStatic(wrongIndex);

        ESM4::Npc player = makeCompletePlayer(ESM::FormId{ 7, 1 });
        player.mFNVData.health = 177;
        npcs.insertStatic(player);

        const MWWorld::FalloutPlayerStateResolution wrongResolution = resolvePlayer(npcs);
        EXPECT_FALSE(wrongResolution);
        EXPECT_THAT(wrongResolution.mError, HasSubstr("does not have EDID Player"));

        const MWWorld::FalloutPlayerStateResolution resolution = resolvePlayer(npcs, 1);
        ASSERT_TRUE(resolution) << resolution.mError;
        EXPECT_EQ(resolution.mState->mBaseRecord, (ESM::FormId{ 7, 1 }));
        EXPECT_EQ(resolution.mState->mHealth, 177);
    }

    TEST(FalloutPlayerStateTest, winningOverrideIsAuthoritativeAndMalformedOverrideDoesNotFallBack)
    {
        MWWorld::Store<ESM4::Npc> npcs;
        npcs.insertStatic(makeCompletePlayer());

        ESM4::Npc winning = makeCompletePlayer();
        winning.mFNVData.health = 222;
        winning.mFNVData.luck = 9;
        winning.mFNVSkills.values.speech = 77;
        winning.mFNVSkills.offsets.speech = 0xfe;
        npcs.insert(winning);

        MWWorld::FalloutPlayerStateResolution resolution = resolvePlayer(npcs);
        ASSERT_TRUE(resolution) << resolution.mError;
        EXPECT_EQ(resolution.mState->mHealth, 222);
        EXPECT_EQ(resolution.mState->getSpecial(MWWorld::FalloutSpecial::Luck), 9);
        EXPECT_EQ(resolution.mState->getSkillValue(MWWorld::FalloutSkill::Speech), 77);
        EXPECT_EQ(resolution.mState->getSkillOffset(MWWorld::FalloutSkill::Speech), 0xfe);

        winning.mHasFNVSkills = false;
        npcs.insert(winning);
        resolution = resolvePlayer(npcs);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("lacks exact 28-byte DNAM"));
    }

    TEST(FalloutPlayerStateTest, resolvesEveryDelegatedCategoryThroughTheTemplateChain)
    {
        MWWorld::Store<ESM4::Npc> npcs;
        ESM4::Npc player = makeCompletePlayer();
        const ESM::FormId templateId = form(0x1000);
        player.mBaseTemplate = templateId;
        player.mBaseConfig.fo3.templateFlags = ESM4::Npc::Template_UseTraits | ESM4::Npc::Template_UseStats
            | ESM4::Npc::Template_UseAIData | ESM4::Npc::Template_UseModel | ESM4::Npc::Template_UseBaseData;
        player.mHasFNVData = false;
        player.mHasFNVSkills = false;
        player.mHasFNVAIData = false;
        player.mRace = {};
        player.mClass = {};
        player.mModel.clear();

        ESM4::Npc templated = makeCompletePlayer(templateId);
        templated.mEditorId = "PlayerTemplate";
        templated.mFullName = "Authored Template Name";
        templated.mFNVData.health = 321;
        templated.mFNVData.strength = 8;
        templated.mFNVAIData.aggression = 2;
        npcs.insertStatic(player);
        npcs.insertStatic(templated);

        const MWWorld::FalloutPlayerStateResolution resolution = resolvePlayer(npcs);
        ASSERT_TRUE(resolution) << resolution.mError;
        EXPECT_EQ(resolution.mState->mBaseRecord, form(7));
        EXPECT_EQ(resolution.mState->mTraitsRecord, templateId);
        EXPECT_EQ(resolution.mState->mStatsRecord, templateId);
        EXPECT_EQ(resolution.mState->mAIDataRecord, templateId);
        EXPECT_EQ(resolution.mState->mModelRecord, templateId);
        EXPECT_EQ(resolution.mState->mBaseDataRecord, templateId);
        EXPECT_EQ(resolution.mState->mFullName, "Authored Template Name");
        EXPECT_EQ(resolution.mState->mHealth, 321);
        EXPECT_EQ(resolution.mState->getSpecial(MWWorld::FalloutSpecial::Strength), 8);
        EXPECT_EQ(resolution.mState->mAIData.aggression, 2);
    }

    TEST(FalloutPlayerStateTest, templateCyclesAndMissingTemplatesFailClosed)
    {
        MWWorld::Store<ESM4::Npc> cycleNpcs;
        ESM4::Npc player = makeCompletePlayer();
        ESM4::Npc templated = makeCompletePlayer(form(0x1000));
        templated.mEditorId = "CycleTemplate";
        player.mBaseConfig.fo3.templateFlags = ESM4::Npc::Template_UseStats;
        player.mBaseTemplate = templated.mId;
        templated.mBaseConfig.fo3.templateFlags = ESM4::Npc::Template_UseStats;
        templated.mBaseTemplate = player.mId;
        cycleNpcs.insertStatic(player);
        cycleNpcs.insertStatic(templated);

        MWWorld::FalloutPlayerStateResolution resolution = resolvePlayer(cycleNpcs);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("template cycle while resolving stats"));

        MWWorld::Store<ESM4::Npc> missingNpcs;
        player = makeCompletePlayer();
        player.mBaseConfig.fo3.templateFlags = ESM4::Npc::Template_UseAIData;
        player.mBaseTemplate = form(0xdead);
        missingNpcs.insertStatic(player);
        resolution = resolvePlayer(missingNpcs);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("unresolved TPLT while resolving AI data"));
    }

    TEST(FalloutPlayerStateTest, missingOfficialRecordAndRequiredPayloadsFailClosed)
    {
        MWWorld::Store<ESM4::Npc> emptyNpcs;
        MWWorld::FalloutPlayerStateResolution resolution = resolvePlayer(emptyNpcs);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("FormID 0x00000007"));

        MWWorld::Store<ESM4::Npc> wrongIdNpcs;
        wrongIdNpcs.insertStatic(makeCompletePlayer(form(8)));
        resolution = resolvePlayer(wrongIdNpcs);
        EXPECT_FALSE(resolution);

        MWWorld::Store<ESM4::Npc> missingAcbsNpcs;
        ESM4::Npc player = makeCompletePlayer();
        player.mHasFNVBaseConfig = false;
        missingAcbsNpcs.insertStatic(player);
        resolution = resolvePlayer(missingAcbsNpcs);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("missing exact 24-byte ACBS"));

        MWWorld::Store<ESM4::Npc> missingDataNpcs;
        player = makeCompletePlayer();
        player.mHasFNVData = false;
        missingDataNpcs.insertStatic(player);
        resolution = resolvePlayer(missingDataNpcs);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("lacks exact 11-byte DATA"));

        MWWorld::Store<ESM4::Npc> missingAiNpcs;
        player = makeCompletePlayer();
        player.mHasFNVAIData = false;
        missingAiNpcs.insertStatic(player);
        resolution = resolvePlayer(missingAiNpcs);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("lacks exact 20-byte AIDT"));
    }

    TEST(FalloutPlayerStateTest, compatibilityProxyUsesExactSharedFieldsWithoutProjectingSpecialOrSkills)
    {
        MWWorld::Store<ESM4::Npc> npcs;
        npcs.insertStatic(makeCompletePlayer());
        const MWWorld::FalloutPlayerStateResolution resolution = resolvePlayer(npcs);
        ASSERT_TRUE(resolution) << resolution.mError;

        ESM::NPC proxy;
        proxy.blank();
        proxy.mName = "Courier";
        proxy.mNpdt.mAttributes.fill(50);
        proxy.mNpdt.mSkills.fill(35);
        proxy.mNpdt.mHealth = 125;
        proxy.mNpdt.mMana = 60;
        proxy.mNpdt.mFatigue = 220;
        proxy.mNpdt.mDisposition = 50;

        MWWorld::seedFalloutPlayerProxy(proxy, *resolution.mState);

        EXPECT_EQ(proxy.mName, "Courier");
        EXPECT_EQ(proxy.mModel, "Characters\\_Male\\Skeleton.NIF");
        EXPECT_EQ(proxy.mNpdt.mLevel, 1);
        EXPECT_EQ(proxy.mNpdt.mHealth, 100);
        EXPECT_EQ(proxy.mNpdt.mFatigue, 200);
        EXPECT_EQ(proxy.mNpdt.mMana, 60);
        EXPECT_EQ(proxy.mNpdt.mDisposition, 50);
        EXPECT_TRUE(std::all_of(proxy.mNpdt.mAttributes.begin(), proxy.mNpdt.mAttributes.end(),
            [](std::uint8_t value) { return value == 50; }));
        EXPECT_TRUE(std::all_of(
            proxy.mNpdt.mSkills.begin(), proxy.mNpdt.mSkills.end(), [](std::uint8_t value) { return value == 35; }));
    }
}
