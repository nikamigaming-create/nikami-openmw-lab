#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <components/esm/refid.hpp>
#include <components/esm4/dialogue.hpp>
#include <components/esm4/loaddial.hpp>
#include <components/esm4/loadinfo.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadsoun.hpp>
#include <components/esm4/loadtact.hpp>

#include "apps/openmw/mwworld/esm4questruntime.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/fnvradioprogram.hpp"
#include "apps/openmw/mwworld/actionesm4radio.hpp"

namespace
{
    constexpr std::uint32_t sStationId = 0x000cd126;
    constexpr std::uint32_t sQuestId = 0x000cd18a;
    constexpr std::uint32_t sTopicId = 0x00170001;
    constexpr std::uint32_t sInfoId = 0x000cd185;
    constexpr std::uint32_t sSoundId = 0x00169bed;
    // FalloutNV.esm 000CD185 has this exact blank SCHR for both begin and end result scripts.
    constexpr std::array<std::uint8_t, sizeof(ESM4::ScriptHeader)> sRetailEmptyResultScriptHeader{
        0x00, 0x00, 0x00, 0x00, // unused
        0x00, 0x00, 0x00, 0x00, // reference count
        0x00, 0x00, 0x00, 0x00, // compiled size
        0x00, 0x00, 0x00, 0x00, // variable count
        0x00, 0x00, // type
        0x01, 0x00, // enabled flag
    };

    ESM::FormId formId(std::uint32_t value)
    {
        return ESM::FormId::fromUint32(value);
    }

    ESM4::TargetCondition makeStationCondition()
    {
        ESM4::TargetCondition condition{};
        condition.condition = ESM4::CTF_EqualTo;
        condition.comparison = 1.f;
        condition.functionIndex = ESM4::FUN_GetIsID;
        condition.param1 = sStationId;
        return condition;
    }

    struct RadioProgramHarness
    {
        ESM4::TalkingActivator mStation{};
        ESM4::Quest mQuest{};
        ESM4::Dialogue mTopic{};
        ESM4::DialogInfo mInfo{};
        ESM4::Sound mSound{};
        std::vector<ESM4::Quest> mExtraQuests;
        std::vector<ESM4::Dialogue> mExtraTopics;
        std::vector<ESM4::DialogInfo> mExtraInfos;
        MWWorld::ESMStore mStore;
        MWWorld::ESM4QuestRuntime mRuntime;

        RadioProgramHarness()
        {
            mStation.mId = formId(sStationId);
            mStation.mFlags = ESM4::TACT_RadioStation;
            mStation.mEditorId = "RadioRepcon";

            mQuest.mId = formId(sQuestId);
            mQuest.mEditorId = "RadioRepconQuest";
            mQuest.mData.flags = 0x15;
            mQuest.mTargetConditions = { makeStationCondition() };

            mTopic.mId = formId(sTopicId);
            mTopic.mEditorId = "RadioHello";
            mTopic.mDialType = ESM4::DTYP_Radio;
            mTopic.mQuests = { mQuest.mId };

            mInfo.mId = formId(sInfoId);
            mInfo.mQuest = mQuest.mId;
            mInfo.mTopic = mTopic.mId;
            mInfo.mDialType = ESM4::DTYP_Radio;
            mInfo.mInfoFlags = ESM4::INFO_RunImmediately;
            mInfo.mScript.scriptHeader = std::bit_cast<ESM4::ScriptHeader>(sRetailEmptyResultScriptHeader);
            mInfo.mEndScript.scriptHeader = std::bit_cast<ESM4::ScriptHeader>(sRetailEmptyResultScriptHeader);
            ESM4::DialogResponse response{};
            response.mData.sound = sSoundId;
            mInfo.mResponses = { response };

            mSound.mId = formId(sSoundId);
            mSound.mEditorId = "MUSRideOfTheValkyries";
        }

        void initialize()
        {
            mStore.overrideRecord(mStation);
            mStore.overrideRecord(mQuest);
            mStore.overrideRecord(mTopic);
            mStore.overrideRecord(mInfo);
            mStore.overrideRecord(mSound);
            for (const ESM4::Quest& quest : mExtraQuests)
                mStore.overrideRecord(quest);
            for (const ESM4::Dialogue& topic : mExtraTopics)
                mStore.overrideRecord(topic);
            for (const ESM4::DialogInfo& info : mExtraInfos)
                mStore.overrideRecord(info);
            mRuntime.initialize(mStore);
        }

        std::optional<MWWorld::PreparedFnvRadioOneShot> prepare(
            MWWorld::FnvRadioProgramPreparationError* error = nullptr,
            MWWorld::ESM4Game game = MWWorld::ESM4Game::FalloutNewVegas,
            const ESM4::TalkingActivator* station = nullptr) const
        {
            if (station == nullptr)
                station = mStore.get<ESM4::TalkingActivator>().search(ESM::RefId(formId(sStationId)));
            return MWWorld::prepareFnvRadioOneShot({ game, &mStore, &mRuntime, station }, error);
        }
    };

    template <class Configure>
    void expectPreparationError(Configure&& configure, MWWorld::FnvRadioProgramPreparationError expected)
    {
        RadioProgramHarness harness;
        std::forward<Configure>(configure)(harness);
        harness.initialize();
        MWWorld::FnvRadioProgramPreparationError error = MWWorld::FnvRadioProgramPreparationError::None;
        EXPECT_FALSE(harness.prepare(&error));
        EXPECT_EQ(error, expected);
    }
}

TEST(FnvRadioProgramTest, PreparesFrozenRepconOneShotWithoutFormIdSpecialCases)
{
    RadioProgramHarness harness;
    harness.initialize();

    MWWorld::FnvRadioProgramPreparationError error = MWWorld::FnvRadioProgramPreparationError::MissingStore;
    const std::optional<MWWorld::PreparedFnvRadioOneShot> prepared = harness.prepare(&error);

    ASSERT_TRUE(prepared.has_value());
    const auto beginScriptHeader
        = std::bit_cast<std::array<std::uint8_t, sizeof(ESM4::ScriptHeader)>>(harness.mInfo.mScript.scriptHeader);
    const auto endScriptHeader
        = std::bit_cast<std::array<std::uint8_t, sizeof(ESM4::ScriptHeader)>>(harness.mInfo.mEndScript.scriptHeader);
    EXPECT_EQ(beginScriptHeader, sRetailEmptyResultScriptHeader);
    EXPECT_EQ(endScriptHeader, sRetailEmptyResultScriptHeader);
    EXPECT_EQ(error, MWWorld::FnvRadioProgramPreparationError::None);
    EXPECT_EQ(prepared->mStation, formId(sStationId));
    EXPECT_EQ(prepared->mQuest, formId(sQuestId));
    EXPECT_EQ(prepared->mTopic, formId(sTopicId));
    EXPECT_EQ(prepared->mInfo, formId(sInfoId));
    EXPECT_EQ(prepared->mSound, formId(sSoundId));
}

TEST(FnvRadioProgramTest, RoutesValidProgramAsOneShotForDirectTactAndLinkedActi)
{
    RadioProgramHarness harness;
    harness.initialize();
    const std::optional<MWWorld::PreparedFnvRadioOneShot> prepared = harness.prepare();
    ASSERT_TRUE(prepared.has_value());

    const MWWorld::Esm4RadioPlaybackSelection directTact = MWWorld::selectEsm4RadioBroadcast(
        harness.mStation.mLoopSound, harness.mStation.mRadioTemplate, {}, {}, prepared->mSound);
    EXPECT_EQ(directTact.mSound, formId(sSoundId));
    EXPECT_EQ(directTact.mMode, MWWorld::Esm4RadioPlaybackMode::OneShot);

    const MWWorld::Esm4RadioPlaybackSelection linkedActi = MWWorld::selectEsm4RadioBroadcast(
        {}, {}, harness.mStation.mLoopSound, harness.mStation.mRadioTemplate, prepared->mSound);
    EXPECT_EQ(linkedActi.mSound, formId(sSoundId));
    EXPECT_EQ(linkedActi.mMode, MWWorld::Esm4RadioPlaybackMode::OneShot);
}

TEST(FnvRadioProgramTest, KeepsActiAndTactDirectSoundsLoopingAheadOfPreparedProgram)
{
    const ESM::FormId actiSnam = formId(0x00170020);
    const ESM::FormId actiInam = formId(0x00170021);
    const ESM::FormId tactSnam = formId(0x00170022);
    const ESM::FormId tactInam = formId(0x00170023);
    const ESM::FormId prepared = formId(sSoundId);

    MWWorld::Esm4RadioPlaybackSelection selected
        = MWWorld::selectEsm4RadioBroadcast(actiSnam, actiInam, tactSnam, tactInam, prepared);
    EXPECT_EQ(selected.mSound, actiSnam);
    EXPECT_EQ(selected.mMode, MWWorld::Esm4RadioPlaybackMode::Loop);

    selected = MWWorld::selectEsm4RadioBroadcast({}, actiInam, tactSnam, tactInam, prepared);
    EXPECT_EQ(selected.mSound, actiInam);
    EXPECT_EQ(selected.mMode, MWWorld::Esm4RadioPlaybackMode::Loop);

    selected = MWWorld::selectEsm4RadioBroadcast({}, {}, tactSnam, tactInam, prepared);
    EXPECT_EQ(selected.mSound, tactSnam);
    EXPECT_EQ(selected.mMode, MWWorld::Esm4RadioPlaybackMode::Loop);

    selected = MWWorld::selectEsm4RadioBroadcast({}, {}, {}, tactInam, prepared);
    EXPECT_EQ(selected.mSound, tactInam);
    EXPECT_EQ(selected.mMode, MWWorld::Esm4RadioPlaybackMode::Loop);
}

TEST(FnvRadioProgramTest, StopsActiveBroadcastAndStartsInactiveOneShotAsNormalPlayback)
{
    using Mode = MWWorld::Esm4RadioPlaybackMode;
    using Operation = MWWorld::Esm4RadioPlaybackOperation;
    EXPECT_EQ(MWWorld::selectEsm4RadioPlaybackOperation(true, Mode::OneShot), Operation::Stop);
    EXPECT_EQ(MWWorld::selectEsm4RadioPlaybackOperation(true, Mode::Loop), Operation::Stop);
    EXPECT_EQ(MWWorld::selectEsm4RadioPlaybackOperation(false, Mode::OneShot), Operation::PlayOneShot);
    EXPECT_EQ(MWWorld::selectEsm4RadioPlaybackOperation(false, Mode::Loop), Operation::PlayLoop);
}

TEST(FnvRadioProgramTest, RejectsNonRadioDirectAndContinuousStations)
{
    using Error = MWWorld::FnvRadioProgramPreparationError;
    expectPreparationError([](RadioProgramHarness& harness) { harness.mStation.mFlags = 0; }, Error::NotRadioStation);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mStation.mLoopSound = formId(0x00170010); },
        Error::HasDirectBroadcast);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mStation.mFlags |= ESM4::TACT_ContBroadcast; },
        Error::ContinuousBroadcast);

    RadioProgramHarness harness;
    harness.initialize();
    Error error = Error::None;
    EXPECT_FALSE(harness.prepare(&error, MWWorld::ESM4Game::Fallout3));
    EXPECT_EQ(error, Error::NotFalloutNewVegas);
}

TEST(FnvRadioProgramTest, RequiresOneExactRunningStationQuest)
{
    using Error = MWWorld::FnvRadioProgramPreparationError;
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mQuest.mTargetConditions.clear(); },
        Error::MissingStationQuest);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mQuest.mTargetConditions.push_back({}); },
        Error::UnsupportedQuestConditions);
    expectPreparationError(
        [](RadioProgramHarness& harness) {
            ESM4::Quest duplicate = harness.mQuest;
            duplicate.mId = formId(0x00170011);
            duplicate.mEditorId = "DuplicateRadioQuest";
            harness.mExtraQuests.push_back(std::move(duplicate));
        },
        Error::AmbiguousStationQuest);

    RadioProgramHarness harness;
    harness.initialize();
    ASSERT_TRUE(harness.mRuntime.stopQuest(formId(sQuestId)));
    Error error = Error::None;
    EXPECT_FALSE(harness.prepare(&error));
    EXPECT_EQ(error, Error::QuestNotRunning);
}

TEST(FnvRadioProgramTest, RequiresUniqueQuestOwnedRadioHelloTopic)
{
    using Error = MWWorld::FnvRadioProgramPreparationError;
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mTopic.mEditorId = "RadioGoodbye"; },
        Error::MissingHelloTopic);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mTopic.mDialType = ESM4::DTYP_Topic; },
        Error::MissingHelloTopic);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mTopic.mQuests.clear(); }, Error::MissingHelloTopic);
    expectPreparationError(
        [](RadioProgramHarness& harness) {
            ESM4::Dialogue duplicate = harness.mTopic;
            duplicate.mId = formId(0x00170012);
            harness.mExtraTopics.push_back(std::move(duplicate));
        },
        Error::AmbiguousHelloTopic);
}

TEST(FnvRadioProgramTest, RejectsAmbiguousConditionedRandomOrScriptedHelloInfo)
{
    using Error = MWWorld::FnvRadioProgramPreparationError;
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mTargetConditions.push_back({}); },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mInfoFlags |= ESM4::INFO_Random; },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mScript.scriptSource = "set x to 1"; },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mEndScript.scriptSource = "set x to 1"; },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mScript.scriptHeader.flag = 2; },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mScript.scriptHeader.unused = 1; },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mScript.scriptHeader.refCount = 1; },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mScript.scriptHeader.compiledSize = 1; },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mScript.scriptHeader.variableCount = 1; },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mScript.scriptHeader.type = 1; },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mScript.compiledData.push_back(0); },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mScript.references.push_back(formId(0x00170015)); },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mScript.localVarData.push_back({}); },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mScript.localRefVarIndex.push_back(1); },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mScript.globReference = formId(0x00170016); },
        Error::UnsupportedInfo);
    expectPreparationError(
        [](RadioProgramHarness& harness) {
            ESM4::DialogInfo duplicate = harness.mInfo;
            duplicate.mId = formId(0x00170013);
            harness.mExtraInfos.push_back(std::move(duplicate));
        },
        Error::AmbiguousHelloInfo);
}

TEST(FnvRadioProgramTest, RequiresExactlyOneResponseWithAStoredDirectSound)
{
    using Error = MWWorld::FnvRadioProgramPreparationError;
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mResponses.clear(); }, Error::InvalidResponse);
    expectPreparationError(
        [](RadioProgramHarness& harness) {
            harness.mInfo.mResponses.front().mData.sound = 0;
            harness.mInfo.mResponses.front().mResponse = "voice-asset-only response";
        },
        Error::InvalidResponse);
    expectPreparationError(
        [](RadioProgramHarness& harness) {
            harness.mInfo.mResponses.push_back(harness.mInfo.mResponses.front());
        },
        Error::InvalidResponse);
    expectPreparationError(
        [](RadioProgramHarness& harness) { harness.mInfo.mResponses.front().mData.sound = 0x00170014; },
        Error::MissingSound);
}

TEST(FnvRadioProgramTest, ReportsStableFailClosedReasonNames)
{
    EXPECT_EQ(MWWorld::getFnvRadioProgramPreparationErrorName(
                  MWWorld::FnvRadioProgramPreparationError::AmbiguousStationQuest),
        std::string_view("ambiguous-station-quest"));
    EXPECT_EQ(MWWorld::getFnvRadioProgramPreparationErrorName(
                  MWWorld::FnvRadioProgramPreparationError::MissingSound),
        std::string_view("missing-sound"));
}
