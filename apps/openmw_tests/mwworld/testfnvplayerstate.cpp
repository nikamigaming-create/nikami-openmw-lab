#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <components/esm/refid.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm4/fonvsavegame.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadnpc.hpp>
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

    ESM4::Npc makeCompletePlayer(ESM::FormId id)
    {
        ESM4::Npc result;
        result.mId = id;
        result.mFlags = 0x10203040;
        result.mIsFONV = true;
        result.mEditorId = "Player";
        result.mFullName = "The Courier";
        result.mModel = "Characters\\_Male\\Skeleton.NIF";
        result.mRace = form(0x19, id.mContentFile);
        result.mClass = form(0x57e6a, id.mContentFile);
        result.mHair = form(0x2ddee, id.mContentFile);
        result.mEyes = form(0x4253, id.mContentFile);
        result.mVoiceType = form(0x2853b, id.mContentFile);

        result.mHasFNVBaseConfig = true;
        result.mBaseConfig.fo3.flags = 0x89abcdef;
        result.mBaseConfig.fo3.fatigue = 201;
        result.mBaseConfig.fo3.barterGold = 987;
        result.mBaseConfig.fo3.levelOrMult = 7;
        result.mBaseConfig.fo3.calcMinlevel = 2;
        result.mBaseConfig.fo3.calcMaxlevel = 42;
        result.mBaseConfig.fo3.speedMultiplier = 115;
        result.mBaseConfig.fo3.karma = -2.5f;
        result.mBaseConfig.fo3.dispositionBase = 19;
        result.mBaseConfig.fo3.templateFlags = 0;

        result.mHasFNVAIData = true;
        result.mFNVAIData.aggression = 2;
        result.mFNVAIData.confidence = 3;
        result.mFNVAIData.energyLevel = 61;
        result.mFNVAIData.responsibility = 72;
        result.mFNVAIData.mood = 4;
        result.mFNVAIData.services = 0x12345678;
        result.mFNVAIData.trainSkill = -3;
        result.mFNVAIData.trainLevel = 17;
        result.mFNVAIData.assistance = 1;
        result.mFNVAIData.aggroRadiusBehavior = 6;
        result.mFNVAIData.aggroRadius = 2048;

        result.mHasFNVData = true;
        result.mFNVData = { 345, 1, 2, 3, 4, 5, 6, 7 };

        result.mHasFNVSkills = true;
        result.mFNVSkills.values = { 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24 };
        result.mFNVSkills.offsets = { 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44 };
        return result;
    }

    ESM4::Npc makeTemplate(ESM::FormId id, const std::string& editorId)
    {
        ESM4::Npc result = makeCompletePlayer(id);
        result.mEditorId = editorId;
        return result;
    }

    ESM4::ActorCharacter makePlayerReference(ESM::FormId id, ESM::FormId base)
    {
        ESM4::ActorCharacter result;
        result.mId = id;
        result.mBaseObj = base;
        return result;
    }

    ESM4::FONVSaveGamePrefix makeSavePrefix(std::initializer_list<std::string> masters)
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

    TEST(FalloutPlayerStateTest, resolvesExactNormalizedMasterPlayerAndNativePayloads)
    {
        constexpr std::int32_t masterIndex = 37;
        const ESM::FormId playerId = form(7, masterIndex);
        MWWorld::Store<ESM4::Npc> npcs;
        npcs.insertStatic(makeCompletePlayer(playerId));

        const MWWorld::FalloutPlayerStateResolution resolution = MWWorld::resolveFalloutPlayerState(npcs, playerId);
        ASSERT_TRUE(resolution) << resolution.mError;
        const MWWorld::FalloutPlayerState& state = *resolution.mState;

        EXPECT_EQ(state.mBaseRecord, playerId);
        EXPECT_EQ(state.mTraitsRecord, playerId);
        EXPECT_EQ(state.mStatsRecord, playerId);
        EXPECT_EQ(state.mAIDataRecord, playerId);
        EXPECT_EQ(state.mModelRecord, playerId);
        EXPECT_EQ(state.mBaseDataRecord, playerId);

        // ACBS
        EXPECT_EQ(state.mStatsConfig.flags, 0x89abcdefu);
        EXPECT_EQ(state.mStatsConfig.fatigue, 201);
        EXPECT_EQ(state.mStatsConfig.barterGold, 987);
        EXPECT_EQ(state.mStatsConfig.levelOrMult, 7);
        EXPECT_EQ(state.mStatsConfig.calcMinlevel, 2);
        EXPECT_EQ(state.mStatsConfig.calcMaxlevel, 42);
        EXPECT_EQ(state.mStatsConfig.speedMultiplier, 115);
        EXPECT_FLOAT_EQ(state.mStatsConfig.karma, -2.5f);
        EXPECT_EQ(state.mStatsConfig.dispositionBase, 19);

        // AIDT
        EXPECT_EQ(state.mAIData.aggression, 2);
        EXPECT_EQ(state.mAIData.confidence, 3);
        EXPECT_EQ(state.mAIData.energyLevel, 61);
        EXPECT_EQ(state.mAIData.responsibility, 72);
        EXPECT_EQ(state.mAIData.mood, 4);
        EXPECT_EQ(state.mAIData.services, 0x12345678u);
        EXPECT_EQ(state.mAIData.trainSkill, -3);
        EXPECT_EQ(state.mAIData.trainLevel, 17);
        EXPECT_EQ(state.mAIData.assistance, 1);
        EXPECT_EQ(state.mAIData.aggroRadiusBehavior, 6);
        EXPECT_EQ(state.mAIData.aggroRadius, 2048);

        // DATA, DNAM, and VTCK
        EXPECT_EQ(state.mHealth, 345);
        EXPECT_THAT(state.mSpecial, ElementsAreArray(std::array<std::uint8_t, 7>{ 1, 2, 3, 4, 5, 6, 7 }));
        EXPECT_THAT(state.mSkillValues,
            ElementsAreArray(std::array<std::uint8_t, 14>{ 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24 }));
        EXPECT_THAT(state.mSkillOffsets,
            ElementsAreArray(std::array<std::uint8_t, 14>{ 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44 }));
        EXPECT_EQ(state.mVoiceType, form(0x2853b, masterIndex));
    }

    TEST(FalloutPlayerStateTest, resolvesExactPlayerReferenceToBaseRelation)
    {
        constexpr std::int32_t masterIndex = 37;
        const ESM::FormId playerId = form(7, masterIndex);
        const ESM::FormId playerReferenceId = form(0x14, masterIndex);
        MWWorld::Store<ESM4::Npc> npcs;
        npcs.insertStatic(makeCompletePlayer(playerId));
        MWWorld::Store<ESM4::ActorCharacter> actorReferences;
        actorReferences.insertStatic(makePlayerReference(playerReferenceId, playerId));

        MWWorld::FalloutPlayerStateResolution resolution
            = MWWorld::resolveFalloutPlayerIdentity(npcs, actorReferences, playerId, playerReferenceId);
        ASSERT_TRUE(resolution) << resolution.mError;
        EXPECT_EQ(resolution.mState->mReferenceRecord, playerReferenceId);
        EXPECT_EQ(resolution.mState->mReferenceBaseRecord, playerId);

        MWWorld::Store<ESM4::ActorCharacter> missingReference;
        resolution = MWWorld::resolveFalloutPlayerIdentity(npcs, missingReference, playerId, playerReferenceId);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("missing winning FalloutNV.esm Player ACHR"));

        MWWorld::Store<ESM4::ActorCharacter> wrongBase;
        wrongBase.insertStatic(makePlayerReference(playerReferenceId, form(8, masterIndex)));
        resolution = MWWorld::resolveFalloutPlayerIdentity(npcs, wrongBase, playerId, playerReferenceId);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("does not target NPC_ FormID"));
    }

    TEST(FalloutPlayerStateTest, resolvesEachTemplateCategoryIndependently)
    {
        constexpr std::int32_t masterIndex = 19;
        const ESM::FormId playerId = form(7, masterIndex);
        const ESM::FormId traitsId = form(0x100, masterIndex);
        const ESM::FormId statsId = form(0x101, masterIndex);
        const ESM::FormId aiId = form(0x102, masterIndex);
        const ESM::FormId modelId = form(0x103, masterIndex);
        const ESM::FormId baseDataId = form(0x104, masterIndex);

        ESM4::Npc player = makeCompletePlayer(playerId);
        player.mBaseTemplate = traitsId;
        player.mBaseConfig.fo3.templateFlags = ESM4::Npc::Template_UseTraits | ESM4::Npc::Template_UseStats
            | ESM4::Npc::Template_UseAIData | ESM4::Npc::Template_UseModel | ESM4::Npc::Template_UseBaseData;

        ESM4::Npc traits = makeTemplate(traitsId, "TraitsTemplate");
        traits.mBaseTemplate = statsId;
        traits.mBaseConfig.fo3.templateFlags = ESM4::Npc::Template_UseStats | ESM4::Npc::Template_UseAIData
            | ESM4::Npc::Template_UseModel | ESM4::Npc::Template_UseBaseData;
        traits.mRace = form(0x201, masterIndex);
        traits.mHair = form(0x202, masterIndex);
        traits.mEyes = form(0x203, masterIndex);
        traits.mVoiceType = form(0x204, masterIndex);

        ESM4::Npc stats = makeTemplate(statsId, "StatsTemplate");
        stats.mBaseTemplate = aiId;
        stats.mBaseConfig.fo3.templateFlags
            = ESM4::Npc::Template_UseAIData | ESM4::Npc::Template_UseModel | ESM4::Npc::Template_UseBaseData;
        stats.mClass = form(0x301, masterIndex);
        stats.mBaseConfig.fo3.fatigue = 333;
        stats.mFNVData.health = 456;
        stats.mFNVData.strength = 9;
        stats.mFNVSkills.values.speech = 88;
        stats.mFNVSkills.offsets.speech = 77;

        ESM4::Npc ai = makeTemplate(aiId, "AITemplate");
        ai.mBaseTemplate = modelId;
        ai.mBaseConfig.fo3.templateFlags = ESM4::Npc::Template_UseModel | ESM4::Npc::Template_UseBaseData;
        ai.mFNVAIData.aggression = 4;
        ai.mFNVAIData.services = 0xaabbccdd;

        ESM4::Npc model = makeTemplate(modelId, "ModelTemplate");
        model.mBaseTemplate = baseDataId;
        model.mBaseConfig.fo3.templateFlags = ESM4::Npc::Template_UseBaseData;
        model.mModel = "Characters\\_Male\\TemplateSkeleton.NIF";

        ESM4::Npc baseData = makeTemplate(baseDataId, "BaseDataTemplate");
        baseData.mFullName = "Delegated Courier";
        baseData.mFlags = 0x55667788;

        MWWorld::Store<ESM4::Npc> npcs;
        npcs.insertStatic(player);
        npcs.insertStatic(traits);
        npcs.insertStatic(stats);
        npcs.insertStatic(ai);
        npcs.insertStatic(model);
        npcs.insertStatic(baseData);

        const MWWorld::FalloutPlayerStateResolution resolution = MWWorld::resolveFalloutPlayerState(npcs, playerId);
        ASSERT_TRUE(resolution) << resolution.mError;
        const MWWorld::FalloutPlayerState& state = *resolution.mState;

        EXPECT_EQ(state.mBaseRecord, playerId);
        EXPECT_EQ(state.mTraitsRecord, traitsId);
        EXPECT_EQ(state.mStatsRecord, statsId);
        EXPECT_EQ(state.mAIDataRecord, aiId);
        EXPECT_EQ(state.mModelRecord, modelId);
        EXPECT_EQ(state.mBaseDataRecord, baseDataId);
        EXPECT_EQ(state.mRace, form(0x201, masterIndex));
        EXPECT_EQ(state.mHair, form(0x202, masterIndex));
        EXPECT_EQ(state.mEyes, form(0x203, masterIndex));
        EXPECT_EQ(state.mVoiceType, form(0x204, masterIndex));
        EXPECT_EQ(state.mClass, form(0x301, masterIndex));
        EXPECT_EQ(state.mStatsConfig.fatigue, 333);
        EXPECT_EQ(state.mHealth, 456);
        EXPECT_EQ(state.getSpecial(MWWorld::FalloutSpecial::Strength), 9);
        EXPECT_EQ(state.getSkillValue(MWWorld::FalloutSkill::Speech), 88);
        EXPECT_EQ(state.getSkillOffset(MWWorld::FalloutSkill::Speech), 77);
        EXPECT_EQ(state.mAIData.aggression, 4);
        EXPECT_EQ(state.mAIData.services, 0xaabbccddu);
        EXPECT_EQ(state.mModel, "Characters\\_Male\\TemplateSkeleton.NIF");
        EXPECT_EQ(state.mFullName, "Delegated Courier");
        EXPECT_EQ(state.mRecordFlags, 0x55667788u);
    }

    TEST(FalloutPlayerStateTest, rejectsTemplateCycles)
    {
        constexpr std::int32_t masterIndex = 3;
        const ESM::FormId playerId = form(7, masterIndex);
        const ESM::FormId templateId = form(0x1000, masterIndex);
        ESM4::Npc player = makeCompletePlayer(playerId);
        player.mBaseTemplate = templateId;
        player.mBaseConfig.fo3.templateFlags = ESM4::Npc::Template_UseStats;
        ESM4::Npc templated = makeTemplate(templateId, "CycleTemplate");
        templated.mBaseTemplate = playerId;
        templated.mBaseConfig.fo3.templateFlags = ESM4::Npc::Template_UseStats;

        MWWorld::Store<ESM4::Npc> npcs;
        npcs.insertStatic(player);
        npcs.insertStatic(templated);

        const MWWorld::FalloutPlayerStateResolution resolution = MWWorld::resolveFalloutPlayerState(npcs, playerId);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("template cycle while resolving stats"));
    }

    TEST(FalloutPlayerStateTest, rejectsMissingAndUnresolvedTemplateReferences)
    {
        constexpr std::int32_t masterIndex = 5;
        const ESM::FormId playerId = form(7, masterIndex);

        ESM4::Npc player = makeCompletePlayer(playerId);
        player.mBaseConfig.fo3.templateFlags = ESM4::Npc::Template_UseAIData;
        MWWorld::Store<ESM4::Npc> missingTemplateId;
        missingTemplateId.insertStatic(player);
        MWWorld::FalloutPlayerStateResolution resolution
            = MWWorld::resolveFalloutPlayerState(missingTemplateId, playerId);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("missing TPLT while resolving delegated AI data"));

        player.mBaseTemplate = form(0xdead, masterIndex);
        player.mBaseConfig.fo3.templateFlags = ESM4::Npc::Template_UseModel;
        MWWorld::Store<ESM4::Npc> unresolvedTemplate;
        unresolvedTemplate.insertStatic(player);
        resolution = MWWorld::resolveFalloutPlayerState(unresolvedTemplate, playerId);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("unresolved TPLT while resolving model"));
    }

    TEST(FalloutPlayerStateTest, usesOnlySuppliedNormalizedMasterIdAndNeverScansContentIndices)
    {
        const ESM::FormId requestedPlayerId = form(7, 64);
        MWWorld::Store<ESM4::Npc> npcs;

        ESM4::Npc lowDecoy = makeCompletePlayer(form(7, 0));
        lowDecoy.mFNVData.health = 100;
        npcs.insertStatic(lowDecoy);
        ESM4::Npc adjacentDecoy = makeCompletePlayer(form(7, 63));
        adjacentDecoy.mFNVData.health = 200;
        npcs.insertStatic(adjacentDecoy);
        ESM4::Npc highDecoy = makeCompletePlayer(form(7, 255));
        highDecoy.mFNVData.health = 300;
        npcs.insertStatic(highDecoy);

        MWWorld::FalloutPlayerStateResolution resolution = MWWorld::resolveFalloutPlayerState(npcs, requestedPlayerId);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr(requestedPlayerId.toString()));

        ESM4::Npc exactPlayer = makeCompletePlayer(requestedPlayerId);
        exactPlayer.mFNVData.health = 640;
        npcs.insertStatic(exactPlayer);
        resolution = MWWorld::resolveFalloutPlayerState(npcs, requestedPlayerId);
        ASSERT_TRUE(resolution) << resolution.mError;
        EXPECT_EQ(resolution.mState->mBaseRecord, requestedPlayerId);
        EXPECT_EQ(resolution.mState->mHealth, 640);
    }

    TEST(FalloutPlayerStateTest, rejectsMissingRequiredNativePayloads)
    {
        struct MissingPayloadCase
        {
            const char* mName;
            void (*mRemove)(ESM4::Npc&);
            const char* mExpectedError;
        };
        const MissingPayloadCase cases[] = {
            { "ACBS", [](ESM4::Npc& npc) { npc.mHasFNVBaseConfig = false; }, "missing exact 24-byte ACBS" },
            { "AIDT", [](ESM4::Npc& npc) { npc.mHasFNVAIData = false; }, "lacks exact 20-byte AIDT" },
            { "DATA", [](ESM4::Npc& npc) { npc.mHasFNVData = false; }, "lacks exact 11-byte DATA" },
            { "DNAM", [](ESM4::Npc& npc) { npc.mHasFNVSkills = false; }, "lacks exact 28-byte DNAM" },
        };

        const ESM::FormId playerId = form(7, 8);
        for (const MissingPayloadCase& testCase : cases)
        {
            SCOPED_TRACE(testCase.mName);
            ESM4::Npc player = makeCompletePlayer(playerId);
            testCase.mRemove(player);
            MWWorld::Store<ESM4::Npc> npcs;
            npcs.insertStatic(player);

            const MWWorld::FalloutPlayerStateResolution resolution = MWWorld::resolveFalloutPlayerState(npcs, playerId);
            EXPECT_FALSE(resolution);
            EXPECT_THAT(resolution.mError, HasSubstr(testCase.mExpectedError));
        }
    }

    TEST(FalloutPlayerStateTest, resolvesSaveLoadPlanAndAppliesOnlySemanticHeaderState)
    {
        const std::vector<std::string> currentContentFiles{ "builtin.omwscripts", "FalloutNV.esm", "DeadMoney.esm" };
        const ESM::FormId playerId = form(7, 1);
        const ESM::FormId playerReferenceId = form(0x14, 1);
        MWWorld::Store<ESM4::Npc> npcs;
        npcs.insertStatic(makeCompletePlayer(playerId));
        MWWorld::Store<ESM4::ActorCharacter> actorReferences;
        actorReferences.insertStatic(makePlayerReference(playerReferenceId, playerId));
        const MWWorld::FalloutPlayerStateResolution playerResolution
            = MWWorld::resolveFalloutPlayerIdentity(npcs, actorReferences, playerId, playerReferenceId);
        ASSERT_TRUE(playerResolution) << playerResolution.mError;

        const ESM4::FONVSaveGamePrefix save = makeSavePrefix({ "FalloutNV.esm", "DeadMoney.esm" });
        const MWWorld::FalloutSaveLoadPlanResolution resolution
            = MWWorld::resolveFalloutSaveLoadPlan(save, &*playerResolution.mState, currentContentFiles);
        ASSERT_TRUE(resolution) << resolution.mError;
        const MWWorld::FalloutSaveLoadPlan& plan = *resolution.mPlan;

        EXPECT_EQ(plan.mPlayer.mBaseRecord, playerId);
        EXPECT_EQ(plan.mPlayer.mReferenceRecord, playerReferenceId);
        EXPECT_EQ(plan.mPlayer.mSaveFalloutNewVegasMasterIndex, 0);
        EXPECT_EQ(plan.mPlayer.mCurrentFalloutNewVegasMasterIndex, 1);
        EXPECT_EQ(plan.mPlayer.mReferenceChangeFlags, 0xb0000022u);
        EXPECT_EQ(plan.mPlayer.mReferencePayloadOffset, 1024u);
        EXPECT_EQ(plan.mPlayer.mReferencePayloadBytes, 5095u);
        EXPECT_EQ(plan.mPlayer.mSaveNumber, 330);
        EXPECT_EQ(plan.mPlayer.mName, "Courier Six");
        EXPECT_EQ(plan.mPlayer.mKarmaTitle, "Desert Survivor");
        EXPECT_EQ(plan.mPlayer.mLevel, 12);
        EXPECT_EQ(plan.mPlayer.mLocationLabel, "Goodsprings");
        EXPECT_EQ(plan.mPlayer.mPlayTimeLabel, "00.16.45");
        EXPECT_EQ(plan.mTransform.mCellOrWorldspaceRecord, form(0x000da726, 1));
        EXPECT_FLOAT_EQ(plan.mTransform.mPosition[0], -72392.84375f);
        EXPECT_FLOAT_EQ(plan.mTransform.mPosition[1], -1240.19275f);
        EXPECT_FLOAT_EQ(plan.mTransform.mPosition[2], 8137.58643f);
        EXPECT_FLOAT_EQ(plan.mTransform.mRotationRadians[2], 2.93332028f);
        EXPECT_EQ(plan.mScene.mCurrentWeather, form(0x001237d7, 1));
        EXPECT_EQ(plan.mScene.mDefaultWeather, form(0x000ffc88, 1));
        EXPECT_FALSE(plan.mScene.mTransitionWeather.has_value());
        EXPECT_FALSE(plan.mScene.mOverrideWeather.has_value());
        EXPECT_FLOAT_EQ(plan.mScene.mGameHour, 14.215002059936523f);
        EXPECT_FLOAT_EQ(plan.mScene.mLastUpdateHour, 17.606250762939453f);
        EXPECT_FLOAT_EQ(plan.mScene.mWeatherPercent, 1.f);
        EXPECT_FLOAT_EQ(plan.mScene.mFogPower, 0.5f);
        EXPECT_EQ(plan.mScene.mFlags, 0x20u);
        EXPECT_EQ(plan.mScene.mSkyMode, 3u);
        EXPECT_EQ(plan.mScene.mPayloadOffset, 494355u);
        EXPECT_EQ(plan.mScene.mPayloadBytes, 71u);
        EXPECT_THAT(plan.mUncoveredState, Not(Contains("player-reference-cell-worldspace-transform")));
        EXPECT_THAT(plan.mUncoveredState, Not(Contains("global-variables-time-weather")));
        EXPECT_THAT(plan.mUncoveredState, Contains("global-variables"));
        EXPECT_THAT(plan.mUncoveredState, Contains("player-inventory-equipment-ammo"));
        EXPECT_THAT(plan.mUncoveredState, Contains("quest-stages-objectives-variables"));
        EXPECT_THAT(plan.mUncoveredState, Contains("world-change-forms-doors-containers-actors-unloaded-references"));

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
        EXPECT_EQ(placement.mPlacement->mWorldspaceRecord, worldspace.mId);
        EXPECT_EQ(placement.mPlacement->mCellRecord, form(0x000e1aa7, 1));
        EXPECT_EQ(placement.mPlacement->mCellX, -18);
        EXPECT_EQ(placement.mPlacement->mCellY, -1);

        ESM::NPC proxy;
        proxy.blank();
        proxy.mId = ESM::RefId::stringRefId("Player");
        proxy.mName = "Base Player";
        proxy.mModel = "base-model.nif";
        proxy.mRace = ESM::RefId::stringRefId("base-race");
        proxy.mClass = ESM::RefId::stringRefId("base-class");
        proxy.mFaction = ESM::RefId::stringRefId("base-faction");
        proxy.mNpdt.mLevel = 1;
        proxy.mNpdt.mHealth = 777;
        proxy.mNpdt.mMana = 66;
        proxy.mNpdt.mFatigue = 55;
        proxy.mNpdt.mAttributes.fill(44);
        proxy.mNpdt.mSkills.fill(33);

        MWWorld::applyFalloutSavePlayerHeader(proxy, plan.mPlayer);

        EXPECT_EQ(proxy.mName, "Courier Six");
        EXPECT_EQ(proxy.mNpdt.mLevel, 12);
        EXPECT_EQ(proxy.mModel, "base-model.nif");
        EXPECT_EQ(proxy.mRace, ESM::RefId::stringRefId("base-race"));
        EXPECT_EQ(proxy.mClass, ESM::RefId::stringRefId("base-class"));
        EXPECT_EQ(proxy.mFaction, ESM::RefId::stringRefId("base-faction"));
        EXPECT_EQ(proxy.mNpdt.mHealth, 777);
        EXPECT_EQ(proxy.mNpdt.mMana, 66);
        EXPECT_EQ(proxy.mNpdt.mFatigue, 55);
        EXPECT_THAT(
            proxy.mNpdt.mAttributes, ElementsAreArray(std::array<std::uint8_t, 8>{ 44, 44, 44, 44, 44, 44, 44, 44 }));
        EXPECT_THAT(proxy.mNpdt.mSkills,
            ElementsAreArray(std::array<std::uint8_t, 27>{ 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33,
                33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33 }));
    }

    TEST(FalloutPlayerStateTest, saveLoadPlanFailsClosedOnMissingOrAmbiguousPlayerProvenance)
    {
        const std::vector<std::string> currentContentFiles{ "builtin.omwscripts", "FalloutNV.esm", "DeadMoney.esm" };
        MWWorld::FalloutPlayerState nativePlayer;
        nativePlayer.mBaseRecord = form(7, 1);
        nativePlayer.mReferenceRecord = form(0x14, 1);
        nativePlayer.mReferenceBaseRecord = nativePlayer.mBaseRecord;
        nativePlayer.mEditorId = "Player";
        ESM4::FONVSaveGamePrefix save = makeSavePrefix({ "FalloutNV.esm", "DeadMoney.esm" });

        MWWorld::FalloutSaveLoadPlanResolution resolution
            = MWWorld::resolveFalloutSaveLoadPlan(save, nullptr, currentContentFiles);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("missing resolved native FNV Player state"));

        save = makeSavePrefix({ "DeadMoney.esm" });
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &nativePlayer, currentContentFiles);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("no FalloutNV.esm Player provenance"));

        save = makeSavePrefix({ "FalloutNV.esm", "falloutnv.ESM" });
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &nativePlayer, currentContentFiles);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("ambiguous duplicate FalloutNV.esm"));

        save = makeSavePrefix({ "FalloutNV.esm", "DeadMoney.esm" });
        const std::vector<std::string> duplicateCurrentContent{ "FalloutNV.esm", "FALLOUTNV.ESM", "DeadMoney.esm" };
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &nativePlayer, duplicateCurrentContent);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("current content has ambiguous duplicate FalloutNV.esm"));

        save = makeSavePrefix({ "FalloutNV.esm", "DeadMoney.esm" });
        save.mPlayerReferenceMovement.reset();
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &nativePlayer, currentContentFiles);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("does not expose a proven canonical Player reference-movement"));
        save = makeSavePrefix({ "FalloutNV.esm", "DeadMoney.esm" });

        save.mSky.reset();
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &nativePlayer, currentContentFiles);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("does not expose a proven Sky global-data payload"));
        save = makeSavePrefix({ "FalloutNV.esm", "DeadMoney.esm" });

        const std::vector<std::string> extraCurrentContent{
            "builtin.omwscripts", "FalloutNV.esm", "DeadMoney.esm", "GunRunnersArsenal.esm"
        };
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &nativePlayer, extraCurrentContent);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("content count does not exactly match"));

        MWWorld::FalloutPlayerState reorderedPlayer = nativePlayer;
        reorderedPlayer.mBaseRecord = form(7, 2);
        reorderedPlayer.mReferenceRecord = form(0x14, 2);
        reorderedPlayer.mReferenceBaseRecord = reorderedPlayer.mBaseRecord;
        const std::vector<std::string> reorderedCurrentContent{
            "builtin.omwscripts", "DeadMoney.esm", "FalloutNV.esm"
        };
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &reorderedPlayer, reorderedCurrentContent);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("content order does not exactly match"));

        nativePlayer.mBaseRecord = form(8, 1);
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &nativePlayer, currentContentFiles);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("not exact winning FalloutNV.esm FormID"));

        nativePlayer.mBaseRecord = form(7, 1);
        nativePlayer.mReferenceBaseRecord = nativePlayer.mBaseRecord;
        save.mHeader.mPlayerLevel.mValue = std::numeric_limits<std::uint32_t>::max();
        resolution = MWWorld::resolveFalloutSaveLoadPlan(save, &nativePlayer, currentContentFiles);
        EXPECT_FALSE(resolution);
        EXPECT_THAT(resolution.mError, HasSubstr("player level cannot be represented"));
    }

    TEST(FalloutPlayerStateTest, semanticHeaderApplicationRejectsNonPlayerCarrierWithoutMutation)
    {
        ESM::NPC proxy;
        proxy.blank();
        proxy.mId = ESM::RefId::stringRefId("NotPlayer");
        proxy.mName = "Before";
        proxy.mNpdt.mLevel = 3;
        MWWorld::FalloutSavePlayerHeaderState state;
        state.mBaseRecord = form(7, 0);
        state.mName = "After";
        state.mLevel = 12;

        EXPECT_THROW(MWWorld::applyFalloutSavePlayerHeader(proxy, state), std::runtime_error);
        EXPECT_EQ(proxy.mName, "Before");
        EXPECT_EQ(proxy.mNpdt.mLevel, 3);
    }

    TEST(FalloutPlayerStateTest, emptySaveHeaderNamePreservesPlayerCarrierName)
    {
        ESM::NPC proxy;
        proxy.blank();
        proxy.mId = ESM::RefId::stringRefId("Player");
        proxy.mName = "Content Player";
        proxy.mNpdt.mLevel = 1;
        MWWorld::FalloutSavePlayerHeaderState state;
        state.mBaseRecord = form(7, 0);
        state.mReferenceRecord = form(0x14, 0);
        state.mName.clear();
        state.mLevel = 12;

        MWWorld::applyFalloutSavePlayerHeader(proxy, state);
        EXPECT_EQ(proxy.mName, "Content Player");
        EXPECT_EQ(proxy.mNpdt.mLevel, 12);
    }
}
