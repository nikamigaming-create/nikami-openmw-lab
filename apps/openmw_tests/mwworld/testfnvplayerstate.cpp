#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <components/esm/attr.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm4/fonvsavegame.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadclas.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadwrld.hpp>

#include "apps/openmw/mwworld/fnvplayerstate.hpp"
#include "apps/openmw/mwworld/store.hpp"

namespace
{
    using testing::Contains;
    using testing::ElementsAreArray;
    using testing::HasSubstr;
    using testing::Not;

    constexpr ESM::FormId form(std::uint32_t index, std::int32_t contentFile = 0)
    {
        return ESM::FormId{ index, contentFile };
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
        result.mFactions = { ESM4::ActorFaction{ form(0x1b2a4).toUint32(), 0, 0, 0, 0 } };

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
        result.mInventory = {
            ESM4::InventoryItem{ form(0x25b83, id.mContentFile).toUint32(), 1 },
            ESM4::InventoryItem{ form(0x15038, id.mContentFile).toUint32(), 1 },
        };
        return result;
    }

    MWWorld::FalloutPlayerStateResolution resolvePlayer(
        const MWWorld::Store<ESM4::Npc>& npcs, std::int32_t contentFile = 0)
    {
        return MWWorld::resolveFalloutPlayerState(npcs, ESM::FormId{ 7, contentFile });
    }

    ESM4::FONVSaveGamePrefix makeSavePlanFixture(std::initializer_list<std::string> masters)
    {
        ESM4::FONVSaveGamePrefix result;
        result.mFileSize = 4096;
        result.mHeader.mSaveNumber.mValue = 330;
        result.mHeader.mPlayerName.mValue = "Courier Six";
        result.mHeader.mPlayerKarmaTitle.mValue = "Desert Survivor";
        result.mHeader.mPlayerLevel.mValue = 12;
        result.mHeader.mPlayerLocation.mValue = "Goodsprings";
        result.mHeader.mPlayTime.mValue = "00.16.45";
        for (const std::string& name : masters)
        {
            ESM4::FONVSaveMaster master;
            master.mFileName.mValue = name;
            result.mMasters.push_back(std::move(master));
        }

        ESM4::FONVSaveChangedFormEnvelope playerChange;
        playerChange.mResolvedFormId = ESM4::sFONVPlayerReferenceFormId;
        playerChange.mChangeType = ESM4::sFONVActorReferenceChangeType;
        playerChange.mChangeFlags.mValue = 0xb0000022;
        playerChange.mUnparsedPayload.mRange = { 1024, 5095 };
        result.mChangedForms.mEntries.push_back(std::move(playerChange));

        ESM4::FONVSavePlayerReferenceMovement movement;
        movement.mCellOrWorldspace.mResolvedFormId = 0x000da726u;
        movement.mPosition[0].mValue = -72392.84375f;
        movement.mPosition[1].mValue = -1240.19275f;
        movement.mPosition[2].mValue = 8137.58643f;
        movement.mRotationRadians[0].mValue = -0.06439045f;
        movement.mRotationRadians[1].mValue = -0.0f;
        movement.mRotationRadians[2].mValue = 2.93332028f;
        result.mPlayerReferenceMovement = std::move(movement);

        ESM4::FONVSavePlayerProcessInventoryData processInventory;
        processInventory.mProcessLevel.mValue = 0;
        const auto addInventory
            = [&](std::uint32_t rawFormId, std::int32_t delta, std::optional<std::uint64_t> wornOffset) {
            ESM4::FONVSavePlayerInventoryEntry entry;
            entry.mType.mResolvedFormId = rawFormId;
            entry.mDelta.mValue = delta;
            if (wornOffset)
            {
                ESM4::FONVSavePlayerInventoryExtendData extend;
                ESM4::FONVSavePlayerInventoryExtraData worn;
                worn.mType.mValue = ESM4::sFONVExtraWornType;
                worn.mType.mRange = { *wornOffset, 1 };
                extend.mExtraData.push_back(std::move(worn));
                entry.mExtendData.push_back(std::move(extend));
            }
            processInventory.mInventoryEntries.push_back(std::move(entry));
        };
        constexpr std::array<std::uint32_t, 50> save330ItemFormIds = { 0x000340fdu, 0x000425bau,
            0x0001cbdcu, 0x00004345u, 0x0000434fu, 0x00022102u, 0x0013b2b1u, 0x0005b6d0u,
            0x000250a7u, 0x000250a3u, 0x000250a8u, 0x000250a6u, 0x0013b2b2u, 0x0002210eu,
            0x0013b2b3u, 0x000e2c6fu, 0x00032c74u, 0x00050f8fu, 0x00015165u, 0x00004241u,
            0x000151a3u, 0x000cb05cu, 0x00015169u, 0x0000000fu, 0x0000000au, 0x0000421cu,
            0x00004323u, 0x00028ff9u, 0x00025b83u, 0x00015038u, 0x0000431eu, 0x0002042eu,
            0x0002935bu, 0x00034040u, 0x001735d1u, 0x001735d2u, 0x001735d4u, 0x001735e0u,
            0x001735e3u, 0x000e86f2u, 0x00140a68u, 0x000e6346u, 0x001735e1u, 0x001735e4u,
            0x0007ea26u, 0x000ccef2u, 0x001735e2u, 0x0014d2acu, 0x001735e5u, 0x001613d0u };
        constexpr std::array<std::int32_t, 50> save330Deltas = { 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1,
            2, 1, 1, 1, 1, 3, 1, 2, 250, 4, 5, 21, 300, 21, 1, 1, 1, 0, 0, 14, 1, 50, 1, 1, 1, 1,
            1, 1, 40, 4, 1, 1, 1, 20, 3, 1, 10, 1, 5 };
        for (std::size_t index = 0; index < save330ItemFormIds.size(); ++index)
        {
            std::optional<std::uint64_t> wornOffset;
            if (index == 28)
                wornOffset = 2010;
            else if (index == 29)
                wornOffset = 2020;
            else if (index == 30)
                wornOffset = 2030;
            addInventory(save330ItemFormIds[index], save330Deltas[index], wornOffset);
        }
        ESM4::FONVSavePlayerInventoryEntry& jumpsuit = processInventory.mInventoryEntries[30];
        jumpsuit.mExtendData.clear();
        constexpr std::array<std::int16_t, 5> jumpsuitCounts{ 3, 2, 3, 3, 2 };
        constexpr std::array<float, 6> jumpsuitHealth{ 30.0000019f, 35.f, 50.f, 65.f, 75.f, 99.9f };
        for (std::size_t index = 0; index < jumpsuitHealth.size(); ++index)
        {
            ESM4::FONVSavePlayerInventoryExtendData extend;
            extend.mRange.mOffset = 2100 + index * 10;
            if (index < jumpsuitCounts.size())
            {
                ESM4::FONVSavePlayerInventoryExtraData count;
                count.mType.mValue = ESM4::sFONVExtraCountType;
                count.mCount.emplace().mValue = jumpsuitCounts[index];
                extend.mExtraData.push_back(std::move(count));
            }
            ESM4::FONVSavePlayerInventoryExtraData health;
            health.mType.mValue = ESM4::sFONVExtraHealthType;
            health.mHealth.emplace().mValue = jumpsuitHealth[index];
            extend.mExtraData.push_back(std::move(health));
            if (index + 1 == jumpsuitHealth.size())
            {
                ESM4::FONVSavePlayerInventoryExtraData worn;
                worn.mType.mValue = ESM4::sFONVExtraWornType;
                worn.mType.mRange = { 2030, 1 };
                extend.mExtraData.push_back(std::move(worn));
            }
            jumpsuit.mExtendData.push_back(std::move(extend));
        }
        processInventory.mInventoryEntryCount.mValue
            = static_cast<std::uint32_t>(processInventory.mInventoryEntries.size());
        result.mPlayerProcessInventoryData = std::move(processInventory);

        ESM4::FONVSavePlayerCharacterScalarReferenceState camera;
        camera.mFirstPersonMode.mValue = 0;
        camera.mFirstPersonMode.mRange = { 501845, 1 };
        camera.mFirstPersonModelFov.mValue = 55.f;
        camera.mFirstPersonModelFov.mRange = { 501942, 4 };
        camera.mWorldFov.mValue = 75.f;
        camera.mWorldFov.mRange = { 501947, 4 };
        camera.mQuest.mResolvedFormId = 0x01000042u;
        result.mPlayerCharacterScalarReferenceState = std::move(camera);

        ESM4::FONVSavePlayerCharacterListsState questLists;
        ESM4::FONVSavePlayerCharacterStageEntry stage;
        stage.mQuest.mResolvedFormId = 0x00000043u;
        stage.mStage.mValue = 5;
        stage.mLogEntry.mValue = 1;
        questLists.mStages.push_back(std::move(stage));
        ESM4::FONVSavePlayerCharacterObjectiveEntry objective;
        objective.mQuest.mResolvedFormId = 0x01000042u;
        objective.mObjective.mValue = 10;
        questLists.mObjectives.push_back(std::move(objective));
        result.mPlayerCharacterListsState = std::move(questLists);

        ESM4::FONVSaveSkyState sky;
        sky.mCurrentWeather.mResolvedFormId = 0x001237d7u;
        sky.mDefaultWeather.mResolvedFormId = 0x000ffc88u;
        sky.mGameHour.mValue = 14.215002059936523f;
        sky.mLastUpdateHour.mValue = 17.606250762939453f;
        sky.mWeatherPercent.mValue = 1.f;
        sky.mFlags.mValue = 0x20u;
        sky.mFogPower.mValue = 0.5f;
        sky.mSkyMode.mValue = 3u;
        sky.mRange = { 494355, 71 };
        result.mSky = std::move(sky);
        return result;
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
        EXPECT_EQ(state.mFactionsRecord, form(7));
        EXPECT_EQ(state.mAIDataRecord, form(7));
        EXPECT_EQ(state.mModelRecord, form(7));
        EXPECT_EQ(state.mBaseDataRecord, form(7));
        EXPECT_EQ(state.mInventoryRecord, form(7));
        EXPECT_EQ(state.mEditorId, "Player");
        EXPECT_TRUE(state.mFullName.empty());
        EXPECT_EQ(state.mModel, "Characters\\_Male\\Skeleton.NIF");
        EXPECT_EQ(state.mRace, form(0x19));
        EXPECT_EQ(state.mClass, form(0x57e6a));
        EXPECT_EQ(state.mHair, form(0x2ddee));
        EXPECT_EQ(state.mEyes, form(0x4253));
        EXPECT_EQ(state.mVoiceType, form(0x2853b));
        EXPECT_THAT(state.mInventoryItems,
            ElementsAreArray(std::array<MWWorld::FalloutInventoryItem, 2>{ {
                { form(0x15038), 1 },
                { form(0x25b83), 1 },
            } }));
        ASSERT_EQ(state.mFactions.size(), 1);
        EXPECT_EQ(ESM::FormId::fromUint32(state.mFactions.front().faction), form(0x1b2a4));
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
            | ESM4::Npc::Template_UseFactions | ESM4::Npc::Template_UseAIData | ESM4::Npc::Template_UseModel
            | ESM4::Npc::Template_UseBaseData | ESM4::Npc::Template_UseInventory;
        player.mHasFNVData = false;
        player.mHasFNVSkills = false;
        player.mHasFNVAIData = false;
        player.mRace = {};
        player.mClass = {};
        player.mFactions.clear();
        player.mModel.clear();
        player.mInventory.clear();

        ESM4::Npc templated = makeCompletePlayer(templateId);
        templated.mEditorId = "PlayerTemplate";
        templated.mFullName = "Authored Template Name";
        templated.mFNVData.health = 321;
        templated.mFNVData.strength = 8;
        templated.mFNVAIData.aggression = 2;
        templated.mFactions.front().faction = form(0x2000).toUint32();
        npcs.insertStatic(player);
        npcs.insertStatic(templated);

        const MWWorld::FalloutPlayerStateResolution resolution = resolvePlayer(npcs);
        ASSERT_TRUE(resolution) << resolution.mError;
        EXPECT_EQ(resolution.mState->mBaseRecord, form(7));
        EXPECT_EQ(resolution.mState->mTraitsRecord, templateId);
        EXPECT_EQ(resolution.mState->mStatsRecord, templateId);
        EXPECT_EQ(resolution.mState->mFactionsRecord, templateId);
        EXPECT_EQ(resolution.mState->mAIDataRecord, templateId);
        EXPECT_EQ(resolution.mState->mModelRecord, templateId);
        EXPECT_EQ(resolution.mState->mBaseDataRecord, templateId);
        EXPECT_EQ(resolution.mState->mInventoryRecord, templateId);
        EXPECT_EQ(resolution.mState->mFullName, "Authored Template Name");
        EXPECT_EQ(resolution.mState->mHealth, 321);
        EXPECT_EQ(resolution.mState->getSpecial(MWWorld::FalloutSpecial::Strength), 8);
        EXPECT_EQ(resolution.mState->mAIData.aggression, 2);
        ASSERT_EQ(resolution.mState->mFactions.size(), 1);
        EXPECT_EQ(ESM::FormId::fromUint32(resolution.mState->mFactions.front().faction), form(0x2000));
        ASSERT_EQ(resolution.mState->mInventoryItems.size(), 2u);
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
        ASSERT_EQ(proxy.mInventory.mList.size(), 2u);
        EXPECT_EQ(proxy.mInventory.mList[0].mItem, ESM::RefId(form(0x15038)));
        EXPECT_EQ(proxy.mInventory.mList[0].mCount, 1);
        EXPECT_EQ(proxy.mInventory.mList[1].mItem, ESM::RefId(form(0x25b83)));
        EXPECT_EQ(proxy.mInventory.mList[1].mCount, 1);
    }

    TEST(FalloutPlayerStateTest, convertsRetailReferenceFovToExactOpenMwVerticalProjection)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(MWWorld::convertFalloutReferenceFovToOpenMwVertical(75.f)), 0x426f5c9du);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(MWWorld::convertFalloutReferenceFovToOpenMwVertical(55.f)), 0x422a9d8eu);
        EXPECT_THROW(MWWorld::convertFalloutReferenceFovToOpenMwVertical(0.f), std::invalid_argument);
        EXPECT_THROW(MWWorld::convertFalloutReferenceFovToOpenMwVertical(180.f), std::invalid_argument);
    }

    TEST(FalloutPlayerStateTest, resolvesEngineReservedReferenceAndExactTypedNativeRecords)
    {
        constexpr std::int32_t masterIndex = 37;
        const ESM::FormId playerId = form(7, masterIndex);
        const ESM::FormId referenceId = form(0x14, masterIndex);
        ESM4::Npc player = makeCompletePlayer(playerId);
        player.mClass = form(0x57e6a, masterIndex);
        player.mRace = form(0x19, masterIndex);
        MWWorld::Store<ESM4::Npc> npcs;
        npcs.insertStatic(player);

        const MWWorld::FalloutPlayerStateResolution identity
            = MWWorld::resolveFalloutPlayerIdentity(npcs, playerId, referenceId);
        ASSERT_TRUE(identity) << identity.mError;
        EXPECT_EQ(identity.mState->mReferenceRecord, referenceId);
        EXPECT_EQ(identity.mState->mReferenceBaseRecord, playerId);

        MWWorld::Store<ESM4::Class> classes;
        ESM4::Class playerClass;
        playerClass.mId = player.mClass;
        playerClass.mHasFalloutData = true;
        playerClass.mHasFalloutAttributes = true;
        classes.insertStatic(playerClass);
        MWWorld::Store<ESM4::Race> races;
        ESM4::Race playerRace;
        playerRace.mId = player.mRace;
        playerRace.mHasFalloutData = true;
        races.insertStatic(playerRace);

        const MWWorld::FalloutNativePlayerRecordsResolution records
            = MWWorld::resolveFalloutNativePlayerRecords(npcs, classes, races, *identity.mState);
        ASSERT_TRUE(records) << records.mError;
        EXPECT_EQ(records.mRecords->mBaseNpc, npcs.search(ESM::RefId(playerId)));
        EXPECT_EQ(records.mRecords->mClass, classes.search(ESM::RefId(player.mClass)));
        EXPECT_EQ(records.mRecords->mRace, races.search(ESM::RefId(player.mRace)));

        EXPECT_FALSE(MWWorld::resolveFalloutPlayerIdentity(npcs, playerId, form(0x15, masterIndex)));
        playerClass.mHasFalloutAttributes = false;
        classes.insert(playerClass);
        const MWWorld::FalloutNativePlayerRecordsResolution malformed
            = MWWorld::resolveFalloutNativePlayerRecords(npcs, classes, races, *identity.mState);
        EXPECT_FALSE(malformed);
        EXPECT_THAT(malformed.mError, HasSubstr("lacks exact 7-byte ATTR"));
    }

    TEST(FalloutPlayerStateTest, resolvesSave330TransformCameraSkyAndExteriorPlacement)
    {
        const std::vector<std::string> content{ "builtin.omwscripts", "FalloutNV.esm", "DeadMoney.esm" };
        const ESM::FormId playerId = form(7, 1);
        const ESM::FormId referenceId = form(0x14, 1);
        MWWorld::Store<ESM4::Npc> npcs;
        npcs.insertStatic(makeCompletePlayer(playerId));
        const MWWorld::FalloutPlayerStateResolution player
            = MWWorld::resolveFalloutPlayerIdentity(npcs, playerId, referenceId);
        ASSERT_TRUE(player) << player.mError;

        const ESM4::FONVSaveGamePrefix save = makeSavePlanFixture({ "FalloutNV.esm", "DeadMoney.esm" });
        const MWWorld::FalloutSaveLoadPlanResolution resolution
            = MWWorld::resolveFalloutSaveLoadPlan(save, &*player.mState, content);
        ASSERT_TRUE(resolution) << resolution.mError;
        const MWWorld::FalloutSaveLoadPlan& plan = *resolution.mPlan;
        EXPECT_EQ(plan.mPlayer.mReferenceRecord, referenceId);
        EXPECT_EQ(plan.mPlayer.mLevel, 12u);
        EXPECT_EQ(plan.mPlayer.mProcessLevel, 0);
        ASSERT_EQ(plan.mPlayer.mWornVisualItems.size(), 3u);
        EXPECT_EQ(plan.mPlayer.mWornVisualItems[0].mRecord, form(0x00025b83, 1));
        EXPECT_EQ(plan.mPlayer.mWornVisualItems[1].mRecord, form(0x00015038, 1));
        EXPECT_EQ(plan.mPlayer.mWornVisualItems[2].mRecord, form(0x0000431e, 1));
        ASSERT_TRUE(plan.mPlayer.mWornVisualItems[2].mHealth.has_value());
        EXPECT_FLOAT_EQ(*plan.mPlayer.mWornVisualItems[2].mHealth, 99.9f);
        EXPECT_EQ(plan.mPlayer.mWornVisualItems[0].mSourceOffset, 2010u);
        ASSERT_EQ(plan.mPlayer.mConditionedStacks.size(), 6u);
        constexpr std::array<std::int32_t, 6> expectedConditionCounts{ 3, 2, 3, 3, 2, 1 };
        constexpr std::array<float, 6> expectedConditionHealth{ 30.0000019f, 35.f, 50.f, 65.f, 75.f, 99.9f };
        for (std::size_t index = 0; index < plan.mPlayer.mConditionedStacks.size(); ++index)
        {
            const auto& stack = plan.mPlayer.mConditionedStacks[index];
            EXPECT_EQ(stack.mRecord, form(0x0000431e, 1));
            EXPECT_EQ(stack.mCount, expectedConditionCounts[index]);
            EXPECT_FLOAT_EQ(stack.mHealth, expectedConditionHealth[index]);
            EXPECT_EQ(stack.mSourceOffset, 2100u + index * 10);
        }
        ASSERT_EQ(plan.mPlayer.mInventoryItems.size(), 50u);
        const auto findInventoryCount = [&](ESM::FormId record) -> std::optional<std::int32_t> {
            const auto found = std::ranges::find(plan.mPlayer.mInventoryItems, record,
                &MWWorld::FalloutInventoryItem::mRecord);
            if (found == plan.mPlayer.mInventoryItems.end())
                return std::nullopt;
            return found->mCount;
        };
        EXPECT_EQ(findInventoryCount(form(0x0000000f, 1)), 300);
        EXPECT_EQ(findInventoryCount(form(0x00004241, 1)), 250);
        EXPECT_EQ(findInventoryCount(form(0x0000431e, 1)), 14);
        EXPECT_EQ(findInventoryCount(form(0x00015038, 1)), 1);
        EXPECT_EQ(findInventoryCount(form(0x00025b83, 1)), 1);
        EXPECT_EQ(findInventoryCount(form(0x0002935b, 1)), 50);
        EXPECT_EQ(plan.mTransform.mCellOrWorldspaceRecord, form(0x000da726, 1));
        EXPECT_FLOAT_EQ(plan.mTransform.mPosition[0], -72392.84375f);
        EXPECT_FLOAT_EQ(plan.mTransform.mPosition[1], -1240.19275f);
        EXPECT_FLOAT_EQ(plan.mTransform.mPosition[2], 8137.58643f);
        EXPECT_FLOAT_EQ(plan.mTransform.mRotationRadians[2], 2.93332028f);
        EXPECT_TRUE(plan.mCamera.mFirstPerson);
        EXPECT_FLOAT_EQ(plan.mCamera.mFirstPersonModelFov, 55.f);
        EXPECT_FLOAT_EQ(plan.mCamera.mWorldFov, 75.f);
        EXPECT_EQ(plan.mCamera.mModeOffset, 501845u);
        ASSERT_TRUE(plan.mQuestProgress.has_value());
        ASSERT_EQ(plan.mQuestProgress->mStages.size(), 1u);
        EXPECT_EQ(plan.mQuestProgress->mStages[0].mQuest, form(0x43, 1));
        EXPECT_EQ(plan.mQuestProgress->mStages[0].mStage, 5u);
        EXPECT_EQ(plan.mQuestProgress->mStages[0].mLogEntry, 1u);
        ASSERT_EQ(plan.mQuestProgress->mObjectives.size(), 1u);
        EXPECT_EQ(plan.mQuestProgress->mObjectives[0].mQuest, form(0x42, 2));
        EXPECT_EQ(plan.mQuestProgress->mObjectives[0].mObjective, 10);
        EXPECT_EQ(plan.mQuestProgress->mActiveQuest, form(0x42, 2));
        EXPECT_EQ(plan.mScene.mCurrentWeather, form(0x001237d7, 1));
        EXPECT_EQ(plan.mScene.mDefaultWeather, form(0x000ffc88, 1));
        EXPECT_FLOAT_EQ(plan.mScene.mGameHour, 14.215002059936523f);
        EXPECT_EQ(plan.mScene.mSkyMode, 3u);
        EXPECT_THAT(plan.mUncoveredState, Contains("global-variables"));
        EXPECT_THAT(plan.mUncoveredState, Not(Contains("global-variables-time-weather")));
        EXPECT_THAT(plan.mUncoveredState, Not(Contains("player-inventory-equipment-ammo")));
        EXPECT_THAT(plan.mUncoveredState,
            Not(Contains("player-inventory-instance-condition-hotkeys-equipment-actions-ammo-selection")));
        EXPECT_THAT(plan.mUncoveredState, Contains("player-inventory-hotkeys-equipment-actions-ammo-selection"));
        EXPECT_THAT(plan.mUncoveredState, Contains("quest-variables"));
        EXPECT_THAT(plan.mUncoveredState, Not(Contains("quest-stages-objectives-variables")));

        MWWorld::Store<ESM4::World> worlds;
        ESM4::World worldspace;
        worldspace.mId = form(0x000da726, 1);
        worlds.insertStatic(worldspace);
        MWWorld::Store<ESM4::Cell> cells;
        ESM4::Cell exterior;
        exterior.mId = ESM::RefId(form(0x000e1aa7, 1));
        exterior.mParent = ESM::RefId(worldspace.mId);
        exterior.mCellFlags = ESM4::CELL_HasWater;
        exterior.mX = -18;
        exterior.mY = -1;
        cells.insertStatic(exterior);
        const MWWorld::FalloutExteriorPlayerPlacementResolution placement
            = MWWorld::resolveFalloutExteriorPlayerPlacement(worlds, cells, plan.mTransform);
        ASSERT_TRUE(placement) << placement.mError;
        EXPECT_EQ(placement.mPlacement->mCellRecord, form(0x000e1aa7, 1));
        EXPECT_EQ(placement.mPlacement->mCellX, -18);
        EXPECT_EQ(placement.mPlacement->mCellY, -1);

        ESM::NPC proxy;
        proxy.blank();
        proxy.mId = ESM::RefId::stringRefId("Player");
        proxy.mName = "Content Player";
        proxy.mNpdt.mHealth = 777;
        MWWorld::applyFalloutSavePlayerHeader(proxy, plan.mPlayer);
        EXPECT_EQ(proxy.mName, "Courier Six");
        EXPECT_EQ(proxy.mNpdt.mLevel, 12);
        EXPECT_EQ(proxy.mNpdt.mHealth, 777);
        ASSERT_EQ(proxy.mInventory.mList.size(), 50u);
        const auto findProxyCount = [&](ESM::FormId record) -> std::optional<std::int32_t> {
            const auto found = std::ranges::find(proxy.mInventory.mList, ESM::RefId(record), &ESM::ContItem::mItem);
            if (found == proxy.mInventory.mList.end())
                return std::nullopt;
            return found->mCount;
        };
        EXPECT_EQ(findProxyCount(form(0x0000000f, 1)), 300);
        EXPECT_EQ(findProxyCount(form(0x0000431e, 1)), 14);
        EXPECT_EQ(findProxyCount(form(0x00015038, 1)), 1);
        EXPECT_EQ(findProxyCount(form(0x00025b83, 1)), 1);
    }

    TEST(FalloutPlayerStateTest, mergesAuthoredInventoryWithSignedSaveDeltasWithoutDuplicatingBaseCounts)
    {
        const std::vector<std::string> content{ "builtin.omwscripts", "FalloutNV.esm", "DeadMoney.esm" };
        const ESM::FormId playerId = form(7, 1);
        ESM4::Npc nativePlayer = makeCompletePlayer(playerId);
        nativePlayer.mInventory = {
            ESM4::InventoryItem{ form(0x100, 1).toUint32(), 2 },
            ESM4::InventoryItem{ form(0x200, 1).toUint32(), 1 },
        };
        MWWorld::Store<ESM4::Npc> npcs;
        npcs.insertStatic(nativePlayer);
        const MWWorld::FalloutPlayerStateResolution player
            = MWWorld::resolveFalloutPlayerIdentity(npcs, playerId, form(0x14, 1));
        ASSERT_TRUE(player) << player.mError;

        ESM4::FONVSaveGamePrefix save = makeSavePlanFixture({ "FalloutNV.esm", "DeadMoney.esm" });
        save.mPlayerProcessInventoryData->mInventoryEntries.clear();
        const auto addDelta = [&](std::uint32_t rawFormId, std::int32_t delta) {
            ESM4::FONVSavePlayerInventoryEntry entry;
            entry.mType.mResolvedFormId = rawFormId;
            entry.mDelta.mValue = delta;
            save.mPlayerProcessInventoryData->mInventoryEntries.push_back(std::move(entry));
        };
        addDelta(0x00000100u, -2);
        addDelta(0x00000200u, 4);
        addDelta(0x00000300u, 3);

        const MWWorld::FalloutSaveLoadPlanResolution resolution
            = MWWorld::resolveFalloutSaveLoadPlan(save, &*player.mState, content);
        ASSERT_TRUE(resolution) << resolution.mError;
        EXPECT_THAT(resolution.mPlan->mPlayer.mInventoryItems,
            ElementsAreArray(std::array<MWWorld::FalloutInventoryItem, 2>{ {
                { form(0x200, 1), 5 },
                { form(0x300, 1), 3 },
            } }));
        EXPECT_TRUE(resolution.mPlan->mPlayer.mWornVisualItems.empty());

        ESM::NPC proxy;
        proxy.blank();
        proxy.mId = ESM::RefId::stringRefId("Player");
        MWWorld::applyFalloutSavePlayerHeader(proxy, resolution.mPlan->mPlayer);
        ASSERT_EQ(proxy.mInventory.mList.size(), 2u);
        EXPECT_EQ(proxy.mInventory.mList[0].mCount, 5);
        EXPECT_EQ(proxy.mInventory.mList[1].mCount, 3);
    }

    TEST(FalloutPlayerStateTest, savePlanFailsClosedWhenCameraOrSkyIsUnproven)
    {
        const std::vector<std::string> content{ "builtin.omwscripts", "FalloutNV.esm", "DeadMoney.esm" };
        MWWorld::FalloutPlayerState player;
        player.mBaseRecord = form(7, 1);
        player.mReferenceRecord = form(0x14, 1);
        player.mReferenceBaseRecord = player.mBaseRecord;
        player.mEditorId = "Player";

        ESM4::FONVSaveGamePrefix save = makeSavePlanFixture({ "FalloutNV.esm", "DeadMoney.esm" });
        save.mPlayerCharacterScalarReferenceState.reset();
        MWWorld::FalloutSaveLoadPlanResolution resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &player, content);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("does not expose the canonical Player camera/FOV state"));

        save = makeSavePlanFixture({ "FalloutNV.esm", "DeadMoney.esm" });
        save.mPlayerCharacterScalarReferenceState->mWorldFov.mValue = std::numeric_limits<float>::infinity();
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &player, content);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("camera FOV values must be finite and in (0, 180)"));

        save = makeSavePlanFixture({ "FalloutNV.esm", "DeadMoney.esm" });
        save.mSky.reset();
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &player, content);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("does not expose a proven Sky global-data payload"));

        save = makeSavePlanFixture({ "FalloutNV.esm", "DeadMoney.esm" });
        save.mPlayerProcessInventoryData.reset();
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &player, content);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("does not expose the canonical Player process/inventory state"));

        save = makeSavePlanFixture({ "FalloutNV.esm", "DeadMoney.esm" });
        save.mPlayerProcessInventoryData->mProcessLevel.mValue = 1;
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &player, content);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("process level is not canonical high process"));

        save = makeSavePlanFixture({ "FalloutNV.esm", "DeadMoney.esm" });
        save.mPlayerProcessInventoryData->mInventoryEntries.front().mType.mResolvedFormId.reset();
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &player, content);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("Player inventory RefID did not resolve"));

        save = makeSavePlanFixture({ "FalloutNV.esm", "DeadMoney.esm" });
        save.mPlayerProcessInventoryData->mInventoryEntries[30].mExtendData[0].mExtraData[0].mCount->mValue = 15;
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &player, content);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("conditioned stacks exceed the final inventory total"));

        save = makeSavePlanFixture({ "FalloutNV.esm", "DeadMoney.esm" });
        player.mInventoryItems = { { form(0x000340fd, 1), std::numeric_limits<std::int32_t>::max() } };
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &player, content);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("inventory total exceeds the compatibility carrier range"));
    }
}
