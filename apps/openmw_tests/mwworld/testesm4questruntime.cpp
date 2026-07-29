#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadfact.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/loadimad.hpp>
#include <components/esm4/loadidle.hpp>
#include <components/esm4/loadmesg.hpp>
#include <components/esm4/loadnote.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadperk.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadrepu.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/esm4/loadspel.hpp>
#include <components/esm4/loadweap.hpp>

#include "apps/openmw/mwworld/esm4questruntime.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/globals.hpp"

namespace
{
    ESM4::Quest makeQuest(ESM::FormId id, std::string_view editorId)
    {
        ESM4::Quest quest;
        quest.mId = id;
        quest.mEditorId = editorId;
        return quest;
    }

    ESM4::GlobalVariable makeGlobal(ESM::FormId id, std::string_view editorId, float value)
    {
        ESM4::GlobalVariable global;
        global.mId = id;
        global.mEditorId = editorId;
        global.mValue = value;
        return global;
    }

    ESM4::Cell makeOpeningCell(ESM::FormId id)
    {
        ESM4::Cell cell{};
        cell.mId = ESM::RefId(id);
        cell.mEditorId = "OpenNVOpeningCell";
        cell.mFullName = "OpenNV Opening Cell";
        cell.mCellFlags = ESM4::CELL_Interior;
        return cell;
    }

    ESM4::Reference makeOpeningMarker(ESM::FormId id, ESM::FormId cellId, std::string_view editorId,
        float x = 0.f, float y = 0.f, float z = 0.f)
    {
        ESM4::Reference marker{};
        marker.mId = id;
        marker.mParent = ESM::RefId(cellId);
        marker.mEditorId = editorId;
        marker.mPos.pos[0] = x;
        marker.mPos.pos[1] = y;
        marker.mPos.pos[2] = z;
        return marker;
    }

    ESM4::TargetCondition makeCondition(std::uint32_t function, ESM::FormId parameter, float comparison,
        std::uint32_t type = ESM4::CTF_EqualTo, std::uint32_t parameter2 = 0)
    {
        ESM4::TargetCondition condition;
        condition.condition = type;
        condition.comparison = comparison;
        condition.functionIndex = function;
        condition.param1 = parameter.toUint32();
        condition.param2 = parameter2;
        return condition;
    }

    ESM4::QuestStageEntry makeCompiledSetStageEntry(
        std::vector<ESM::FormId> references, std::uint16_t referenceIndex, std::int32_t stage)
    {
        const std::uint32_t stageBits = std::bit_cast<std::uint32_t>(stage);
        ESM4::QuestStageEntry entry;
        entry.mScript.compiledData = { 0x39, 0x10, 0x0a, 0x00, 0x02, 0x00, 0x72,
            static_cast<std::uint8_t>(referenceIndex), static_cast<std::uint8_t>(referenceIndex >> 8), 0x6e,
            static_cast<std::uint8_t>(stageBits), static_cast<std::uint8_t>(stageBits >> 8),
            static_cast<std::uint8_t>(stageBits >> 16), static_cast<std::uint8_t>(stageBits >> 24) };
        entry.mScript.references = std::move(references);
        return entry;
    }
}

TEST(ESM4QuestRuntimeTest, MatchesRetailVcg02StageFiveTransition)
{
    MWWorld::ESMStore store;

    const ESM::FormId vcg02Id{ .mIndex = 0x10a214, .mContentFile = 0 };
    ESM4::Quest vcg02 = makeQuest(vcg02Id, "VCG02");
    vcg02.mObjectives.push_back(ESM4::QuestObjective{ .mIndex = 3, .mDescription = "Choose your skills" });
    ESM4::QuestStageEntry entry;
    // FalloutNV.esm 0010A214 VCG02 stage 5 entry 0, byte-for-byte. The sole SCRO is VCG02.
    const std::array<std::uint8_t, 28> retailScda{ 0xa3, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01, 0x00, 0x6e, 0x03,
        0x00, 0x00, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00, 0xdd, 0x11, 0x05, 0x00, 0x01, 0x00, 0x72, 0x01, 0x00 };
    entry.mScript.compiledData.assign(retailScda.begin(), retailScda.end());
    entry.mScript.references.push_back(vcg02Id);
    vcg02.mStages.push_back(ESM4::QuestStage{ .mIndex = 5, .mEntries = { std::move(entry) } });
    store.overrideRecord(vcg02);

    const std::array<std::pair<ESM::FormId, std::string_view>, 3> unchangedQuests = {
        std::pair{ ESM::FormId{ .mIndex = 0x10a212, .mContentFile = 0 }, std::string_view{ "VCG00" } },
        std::pair{ ESM::FormId{ .mIndex = 0x10a213, .mContentFile = 0 }, std::string_view{ "VCG01" } },
        std::pair{ ESM::FormId{ .mIndex = 0x10a215, .mContentFile = 0 }, std::string_view{ "VCG03" } },
    };
    for (const auto& [id, editorId] : unchangedQuests)
        store.overrideRecord(makeQuest(id, editorId));

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);

    const MWWorld::ESM4QuestState* before = runtime.search("vcg02");
    ASSERT_NE(before, nullptr);
    EXPECT_EQ(before->mFlags, 0);
    EXPECT_EQ(before->mCurrentStage, 0);
    ASSERT_TRUE(before->mStageDone.contains(5));
    EXPECT_FALSE(before->mStageDone.at(5));

    ASSERT_TRUE(runtime.setStage("VCG02", 5));

    const MWWorld::ESM4QuestState* after = runtime.search(vcg02Id);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->mFlags, 0x21);
    EXPECT_EQ(after->mCurrentStage, 5);
    EXPECT_TRUE(after->mStageDone.at(5));
    EXPECT_EQ(after->mObjectiveStatus.at(3), MWWorld::ESM4QuestState::Objective_Displayed);
    EXPECT_EQ(runtime.getActiveQuest(), vcg02Id);
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());

    for (const auto& [id, editorId] : unchangedQuests)
    {
        const MWWorld::ESM4QuestState* state = runtime.search(id);
        ASSERT_NE(state, nullptr) << editorId;
        EXPECT_EQ(state->mFlags, 0) << editorId;
        EXPECT_EQ(state->mCurrentStage, 0) << editorId;
        EXPECT_TRUE(state->mStageDone.empty()) << editorId;
    }
}

TEST(ESM4QuestRuntimeTest, ExecutesFalloutShortFormImageSpaceCommands)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x1000f0, .mContentFile = 0 };
    const ESM::FormId modifierId{ .mIndex = 0x1000f1, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "ImageSpaceQuest");
    ESM4::QuestStageEntry applyEntry;
    applyEntry.mScript.scriptSource = "imod IntroFade";
    quest.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(applyEntry) } });
    ESM4::QuestStageEntry reapplyEntry;
    reapplyEntry.mScript.scriptSource = "imod IntroFade";
    quest.mStages.push_back({ .mIndex = 10, .mEntries = { std::move(reapplyEntry) } });
    ESM4::QuestStageEntry removeEntry;
    removeEntry.mScript.scriptSource = "rimod IntroFade";
    quest.mStages.push_back({ .mIndex = 15, .mEntries = { std::move(removeEntry) } });
    store.overrideRecord(quest);

    ESM4::ImageSpaceModifier modifier;
    modifier.mId = modifierId;
    modifier.mEditorId = "IntroFade";
    modifier.mAdapterFlags = 1;
    modifier.mDuration = 1.f;
    store.overrideRecord(modifier);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);

    ASSERT_TRUE(runtime.setStage("ImageSpaceQuest", 5));
    const std::vector<ESM4::ImageSpaceModifierRuntimeState> active = runtime.getActiveImageSpaceModifiers();
    ASSERT_EQ(active.size(), 1);
    EXPECT_EQ(active.front().mId, modifierId);

    runtime.update(1.f, false);
    EXPECT_TRUE(runtime.getActiveImageSpaceModifiers().empty());

    ASSERT_TRUE(runtime.setStage("ImageSpaceQuest", 10));
    ASSERT_EQ(runtime.getActiveImageSpaceModifiers().size(), 1);

    ASSERT_TRUE(runtime.setStage("ImageSpaceQuest", 15));
    EXPECT_TRUE(runtime.getActiveImageSpaceModifiers().empty());
}

TEST(ESM4QuestRuntimeTest, FindsUnambiguousAuthoredOpeningPlacementFromStageZeroSource)
{
    MWWorld::ESMStore store;
    const ESM::FormId openingQuestId{ .mIndex = 0x100100, .mContentFile = 0 };
    const ESM::FormId cellId{ .mIndex = 0x100101, .mContentFile = 0 };
    const ESM::FormId markerId{ .mIndex = 0x100102, .mContentFile = 0 };

    ESM4::Quest openingQuest = makeQuest(openingQuestId, "OpeningQuest");
    ESM4::QuestStageEntry openingEntry;
    openingEntry.mScript.scriptSource = "; player.moveto CommentedOutMarker\n"
                                       "Player.MoveTo CourierOpeningMarker\n"
                                       "PlayBink \"FNV Intro.bik\" 1 1 0 1\n"
                                       "SetStage OpeningQuest 5";
    openingQuest.mStages.push_back({ .mIndex = 0, .mEntries = { openingEntry } });
    store.overrideRecord(openingQuest);
    store.overrideRecord(makeOpeningCell(cellId));
    auto& references = const_cast<MWWorld::Store<ESM4::Reference>&>(store.get<ESM4::Reference>());
    references.insertStatic(makeOpeningMarker(markerId, cellId, "CourierOpeningMarker", 2257.04f, 2318.21f, 7360.f));

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);

    const std::optional<MWWorld::ESM4AuthoredStartPlacement> placement = runtime.findAuthoredStartPlacement();
    ASSERT_TRUE(placement.has_value());
    EXPECT_EQ(placement->mQuest, openingQuestId);
    EXPECT_EQ(placement->mActivationStage, 5);
    EXPECT_EQ(placement->mMarker, markerId);
    EXPECT_EQ(placement->mCell, ESM::RefId(cellId));
    EXPECT_EQ(placement->mQuestEditorId, "OpeningQuest");
    EXPECT_EQ(placement->mMarkerEditorId, "CourierOpeningMarker");
    EXPECT_EQ(placement->mCinematicAsset, "FNV Intro.bik");
    EXPECT_FLOAT_EQ(placement->mPosition.pos[0], 2257.04f);
    EXPECT_FLOAT_EQ(placement->mPosition.pos[1], 2318.21f);
    EXPECT_FLOAT_EQ(placement->mPosition.pos[2], 7360.f);
}

TEST(ESM4QuestRuntimeTest, SelectsSelfActivatingOpeningOverLaterCinematicTransition)
{
    MWWorld::ESMStore store;
    const ESM::FormId openingQuestId{ .mIndex = 0x100105, .mContentFile = 0 };
    const ESM::FormId laterQuestId{ .mIndex = 0x100106, .mContentFile = 0 };
    const ESM::FormId openingCellId{ .mIndex = 0x100107, .mContentFile = 0 };
    const ESM::FormId laterCellId{ .mIndex = 0x100108, .mContentFile = 0 };

    ESM4::Quest openingQuest = makeQuest(openingQuestId, "BootstrapQuest");
    ESM4::QuestStageEntry openingEntry;
    openingEntry.mScript.scriptSource = "Player.MoveTo BirthMarker\n"
                                        "PlayBink \"Birth Intro.bik\" 1\n"
                                        "SetStage BootstrapQuest 5";
    openingQuest.mStages.push_back({ .mIndex = 0, .mEntries = { openingEntry } });
    store.overrideRecord(openingQuest);

    // This looks similar enough to be dangerous: it has a marker and a movie,
    // but it begins another quest rather than handing its own stage zero to an
    // authored continuation. It must not become a new-game spawn rule.
    ESM4::Quest laterQuest = makeQuest(laterQuestId, "LaterCinematic");
    ESM4::QuestStageEntry laterEntry;
    laterEntry.mScript.scriptSource = "Player.MoveTo LaterMarker\n"
                                      "PlayBink \"Years Later.bik\" 1\n"
                                      "StartQuest LaterCinematic";
    laterQuest.mStages.push_back({ .mIndex = 0, .mEntries = { laterEntry } });
    store.overrideRecord(laterQuest);

    store.overrideRecord(makeOpeningCell(openingCellId));
    store.overrideRecord(makeOpeningCell(laterCellId));
    auto& references = const_cast<MWWorld::Store<ESM4::Reference>&>(store.get<ESM4::Reference>());
    references.insertStatic(makeOpeningMarker({ .mIndex = 0x100109, .mContentFile = 0 }, openingCellId, "BirthMarker"));
    references.insertStatic(makeOpeningMarker({ .mIndex = 0x10010a, .mContentFile = 0 }, laterCellId, "LaterMarker"));

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);

    const std::optional<MWWorld::ESM4AuthoredStartPlacement> placement = runtime.findAuthoredStartPlacement();
    ASSERT_TRUE(placement.has_value());
    EXPECT_EQ(placement->mQuest, openingQuestId);
    EXPECT_EQ(placement->mActivationStage, 5);
    EXPECT_EQ(placement->mMarkerEditorId, "BirthMarker");
    EXPECT_EQ(placement->mCinematicAsset, "Birth Intro.bik");
}

TEST(ESM4QuestRuntimeTest, RejectsAmbiguousAuthoredOpeningPlacements)
{
    MWWorld::ESMStore store;
    const ESM::FormId cellId{ .mIndex = 0x100110, .mContentFile = 0 };
    store.overrideRecord(makeOpeningCell(cellId));
    auto& references = const_cast<MWWorld::Store<ESM4::Reference>&>(store.get<ESM4::Reference>());
    references.insertStatic(makeOpeningMarker({ .mIndex = 0x100111, .mContentFile = 0 }, cellId, "OpeningMarkerOne"));
    references.insertStatic(makeOpeningMarker({ .mIndex = 0x100112, .mContentFile = 0 }, cellId, "OpeningMarkerTwo"));

    for (const auto& [id, editorId, marker] : std::array{
             std::tuple{ ESM::FormId{ .mIndex = 0x100113, .mContentFile = 0 }, "OpeningOne", "OpeningMarkerOne" },
             std::tuple{ ESM::FormId{ .mIndex = 0x100114, .mContentFile = 0 }, "OpeningTwo", "OpeningMarkerTwo" },
         })
    {
        ESM4::Quest quest = makeQuest(id, editorId);
        ESM4::QuestStageEntry entry;
        entry.mScript.scriptSource = std::string("player.moveto ") + marker + "\nPlayBink \"intro.bik\" 1\nSetStage "
            + editorId + " 5";
        quest.mStages.push_back({ .mIndex = 0, .mEntries = { entry } });
        store.overrideRecord(quest);
    }

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    EXPECT_FALSE(runtime.findAuthoredStartPlacement().has_value());
}

TEST(ESM4QuestRuntimeTest, ExecutesFourNativeQuestStateCommandsFromFrozenRetailFrames)
{
    MWWorld::ESMStore store;

    // HonestHearts.esm 02008891 NVDLC02MQ00 stage 20 entry 0, the complete 19-byte SCDA.
    // SHA256 649404efe12622c281e09c318d64127d7d51441dd82c17c24a4897891986318b.
    const ESM::FormId objectiveQuestId{ .mIndex = 0x008891, .mContentFile = 2 };
    ESM4::Quest objectiveQuest = makeQuest(objectiveQuestId, "NVDLC02MQ00");
    objectiveQuest.mObjectives.push_back({ .mIndex = 10, .mDescription = "Travel to Zion" });
    ESM4::QuestStageEntry objectiveEntry;
    objectiveEntry.mScript.compiledData = { 0xa2, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01, 0x00, 0x6e, 0x0a,
        0x00, 0x00, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00 };
    objectiveEntry.mScript.references = { objectiveQuestId };
    objectiveQuest.mStages.push_back({ .mIndex = 20, .mEntries = { std::move(objectiveEntry) } });
    ESM4::QuestStageEntry clearObjectiveEntry;
    clearObjectiveEntry.mScript.compiledData = { 0xa2, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01, 0x00, 0x6e,
        0x0a, 0x00, 0x00, 0x00, 0x6e, 0x00, 0x00, 0x00, 0x00 };
    clearObjectiveEntry.mScript.references = { objectiveQuestId };
    objectiveQuest.mStages.push_back({ .mIndex = 21, .mEntries = { std::move(clearObjectiveEntry) } });
    store.overrideRecord(objectiveQuest);

    // FalloutNV.esm 0015D912 VCG03 stage 255 entry 0, the complete nine-byte SCDA.
    // SHA256 98c19cf8f292bee54870f488fb17fa17a9f7ae747f876954ad8ab61463813277.
    const ESM::FormId stoppedQuestId{ .mIndex = 0x15d912, .mContentFile = 0 };
    ESM4::Quest stoppedQuest = makeQuest(stoppedQuestId, "VCG03");
    ESM4::QuestStageEntry stopEntry;
    stopEntry.mScript.compiledData = { 0x37, 0x10, 0x05, 0x00, 0x01, 0x00, 0x72, 0x01, 0x00 };
    stopEntry.mScript.references = { stoppedQuestId };
    stoppedQuest.mStages.push_back({ .mIndex = 255, .mEntries = { std::move(stopEntry) } });
    store.overrideRecord(stoppedQuest);

    // FalloutNV.esm 001348DB VMS38 stage 10 entry 0 StartQuest frame at SCDA offset 19.
    // The frame is byte-exact; its argument is the third SCRO, 0013AE5A VMS38a.
    const ESM::FormId startOwnerId{ .mIndex = 0x1348db, .mContentFile = 0 };
    const ESM::FormId startedQuestId{ .mIndex = 0x13ae5a, .mContentFile = 0 };
    ESM4::Quest startOwner = makeQuest(startOwnerId, "VMS38");
    ESM4::QuestStageEntry startEntry;
    startEntry.mScript.compiledData = { 0x36, 0x10, 0x05, 0x00, 0x01, 0x00, 0x72, 0x03, 0x00 };
    startEntry.mScript.references
        = { ESM::FormId{ .mIndex = 0x134950, .mContentFile = 0 }, startOwnerId, startedQuestId };
    startOwner.mStages.push_back({ .mIndex = 10, .mEntries = { std::move(startEntry) } });
    store.overrideRecord(startOwner);
    store.overrideRecord(makeQuest(startedQuestId, "VMS38a"));

    // FalloutNV.esm 000F0629 VMS11 stage 100 entry 0 CompleteQuest frame at SCDA offset 9.
    const ESM::FormId completedQuestId{ .mIndex = 0x0f0629, .mContentFile = 0 };
    ESM4::Quest completedQuest = makeQuest(completedQuestId, "VMS11");
    ESM4::QuestStageEntry completeEntry;
    completeEntry.mScript.compiledData = { 0x71, 0x10, 0x05, 0x00, 0x01, 0x00, 0x72, 0x01, 0x00 };
    completeEntry.mScript.references = { completedQuestId };
    completedQuest.mStages.push_back({ .mIndex = 100, .mEntries = { std::move(completeEntry) } });
    store.overrideRecord(completedQuest);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    ASSERT_TRUE(runtime.setStage(objectiveQuestId, 20));
    ASSERT_TRUE(runtime.startQuest(stoppedQuestId));
    ASSERT_TRUE(runtime.setStage(stoppedQuestId, 255));
    ASSERT_TRUE(runtime.setStage(startOwnerId, 10));
    ASSERT_TRUE(runtime.setStage(completedQuestId, 100));

    ASSERT_NE(runtime.search(objectiveQuestId), nullptr);
    EXPECT_EQ(runtime.search(objectiveQuestId)->mObjectiveStatus.at(10),
        MWWorld::ESM4QuestState::Objective_Completed);
    ASSERT_TRUE(runtime.setStage(objectiveQuestId, 21));
    EXPECT_EQ(runtime.search(objectiveQuestId)->mObjectiveStatus.at(10), 0);
    ASSERT_NE(runtime.search(stoppedQuestId), nullptr);
    EXPECT_EQ(runtime.search(stoppedQuestId)->mFlags & MWWorld::ESM4QuestState::Flag_Running, 0);
    ASSERT_NE(runtime.search(startedQuestId), nullptr);
    EXPECT_NE(runtime.search(startedQuestId)->mFlags & MWWorld::ESM4QuestState::Flag_Running, 0);
    ASSERT_NE(runtime.search(completedQuestId), nullptr);
    EXPECT_NE(runtime.search(completedQuestId)->mFlags & MWWorld::ESM4QuestState::Flag_Completed, 0);
    EXPECT_EQ(runtime.search(completedQuestId)->mFlags & MWWorld::ESM4QuestState::Flag_Running, 0);
    EXPECT_TRUE(runtime.getUnsupportedCompiledOpcodes().empty());
}

TEST(ESM4QuestRuntimeTest, RejectsMalformedSignaturesForEveryNewNativeQuestOpcode)
{
    constexpr std::array<std::uint16_t, 4> opcodes{ 0x11a2, 0x1037, 0x1036, 0x1071 };
    for (std::size_t i = 0; i < opcodes.size(); ++i)
    {
        MWWorld::ESMStore store;
        const ESM::FormId questId{ .mIndex = static_cast<std::uint32_t>(0x120200 + i), .mContentFile = 0 };
        ESM4::Quest quest = makeQuest(questId, "MalformedNativeQuestCommand");
        quest.mObjectives.push_back({ .mIndex = 10, .mDescription = "Must remain unchanged" });
        ESM4::QuestStageEntry entry;
        entry.mScript.compiledData = { static_cast<std::uint8_t>(opcodes[i]),
            static_cast<std::uint8_t>(opcodes[i] >> 8), 0x02, 0x00, 0x00, 0x00 }; // zero args
        quest.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(entry) } });
        ESM4::QuestStageEntry wrongTypeEntry;
        if (opcodes[i] == 0x11a2)
        {
            wrongTypeEntry.mScript.compiledData = { 0xa2, 0x11, 0x11, 0x00, 0x03, 0x00, 0x6e, 0x01, 0x00, 0x00,
                0x00, 0x6e, 0x0a, 0x00, 0x00, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00 };
        }
        else
        {
            wrongTypeEntry.mScript.compiledData = { static_cast<std::uint8_t>(opcodes[i]),
                static_cast<std::uint8_t>(opcodes[i] >> 8), 0x07, 0x00, 0x01, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00 };
        }
        quest.mStages.push_back({ .mIndex = 6, .mEntries = { std::move(wrongTypeEntry) } });
        store.overrideRecord(quest);

        MWWorld::ESM4QuestRuntime runtime;
        runtime.initialize(store);
        EXPECT_FALSE(runtime.setStage(questId, 5)) << opcodes[i];
        const MWWorld::ESM4QuestState* state = runtime.search(questId);
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->mFlags, 0);
        EXPECT_EQ(state->mCurrentStage, 0);
        EXPECT_FALSE(state->mStageDone.at(5));
        EXPECT_EQ(state->mObjectiveStatus.at(10), 0);
        EXPECT_FALSE(runtime.setStage(questId, 6)) << opcodes[i];
        EXPECT_EQ(state->mFlags, 0);
        EXPECT_EQ(state->mCurrentStage, 0);
        EXPECT_FALSE(state->mStageDone.at(6));
        EXPECT_EQ(state->mObjectiveStatus.at(10), 0);
    }
}

TEST(ESM4QuestRuntimeTest, RejectsNonexistentCompiledQuestAndObjectiveBeforeStageMutation)
{
    MWWorld::ESMStore store;
    const ESM::FormId driverId{ .mIndex = 0x120208, .mContentFile = 0 };
    const ESM::FormId objectiveId{ .mIndex = 0x120209, .mContentFile = 0 };
    const ESM::FormId missingId{ .mIndex = 0x12020a, .mContentFile = 0 };
    ESM4::Quest driver = makeQuest(driverId, "MissingNativeTargets");
    ESM4::QuestStageEntry missingQuestEntry;
    missingQuestEntry.mScript.compiledData = { 0x36, 0x10, 0x05, 0x00, 0x01, 0x00, 0x72, 0x01, 0x00 };
    missingQuestEntry.mScript.references = { missingId };
    driver.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(missingQuestEntry) } });
    ESM4::QuestStageEntry missingObjectiveEntry;
    missingObjectiveEntry.mScript.compiledData = { 0xa2, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01, 0x00, 0x6e,
        0x63, 0x00, 0x00, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00 };
    missingObjectiveEntry.mScript.references = { objectiveId };
    driver.mStages.push_back({ .mIndex = 6, .mEntries = { std::move(missingObjectiveEntry) } });
    store.overrideRecord(driver);
    ESM4::Quest objectiveQuest = makeQuest(objectiveId, "MissingObjectiveTarget");
    objectiveQuest.mObjectives.push_back({ .mIndex = 10, .mDescription = "The only objective" });
    store.overrideRecord(objectiveQuest);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    EXPECT_FALSE(runtime.setStage(driverId, 5));
    EXPECT_FALSE(runtime.setStage(driverId, 6));
    const MWWorld::ESM4QuestState* driverState = runtime.search(driverId);
    ASSERT_NE(driverState, nullptr);
    EXPECT_EQ(driverState->mFlags, 0);
    EXPECT_EQ(driverState->mCurrentStage, 0);
    EXPECT_FALSE(driverState->mStageDone.at(5));
    EXPECT_FALSE(driverState->mStageDone.at(6));
    const MWWorld::ESM4QuestState* objectiveState = runtime.search(objectiveId);
    ASSERT_NE(objectiveState, nullptr);
    EXPECT_EQ(objectiveState->mObjectiveStatus.at(10), 0);
}

TEST(ESM4QuestRuntimeTest, LaterInvalidNewCommandCannotPartiallyMutateAnyQuest)
{
    MWWorld::ESMStore store;
    const ESM::FormId driverId{ .mIndex = 0x120210, .mContentFile = 0 };
    const ESM::FormId objectiveId{ .mIndex = 0x120211, .mContentFile = 0 };
    const ESM::FormId startId{ .mIndex = 0x120212, .mContentFile = 0 };
    const ESM::FormId completeId{ .mIndex = 0x120213, .mContentFile = 0 };
    ESM4::Quest driver = makeQuest(driverId, "AtomicNativeDriver");
    ESM4::QuestStageEntry entry;
    entry.mScript.references = { objectiveId, startId, completeId };
    entry.mScript.compiledData = {
        0xa2, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01, 0x00, 0x6e, 0x0a, 0x00, 0x00, 0x00, 0x6e, 0x01, 0x00,
        0x00, 0x00, // valid SetObjectiveCompleted
        0x36, 0x10, 0x05, 0x00, 0x01, 0x00, 0x72, 0x02, 0x00, // valid StartQuest
        0x71, 0x10, 0x05, 0x00, 0x01, 0x00, 0x72, 0x03, 0x00, // valid CompleteQuest
        0x37, 0x10, 0x05, 0x00, 0x01, 0x00, 0x72, 0x04, 0x00 // invalid later StopQuest SCRO index
    };
    driver.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(entry) } });
    store.overrideRecord(driver);
    ESM4::Quest objectiveQuest = makeQuest(objectiveId, "AtomicObjectiveTarget");
    objectiveQuest.mObjectives.push_back({ .mIndex = 10, .mDescription = "Must remain incomplete" });
    store.overrideRecord(objectiveQuest);
    store.overrideRecord(makeQuest(startId, "AtomicStartTarget"));
    store.overrideRecord(makeQuest(completeId, "AtomicCompleteTarget"));

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    EXPECT_FALSE(runtime.setStage(driverId, 5));
    ASSERT_NE(runtime.search(driverId), nullptr);
    EXPECT_EQ(runtime.search(driverId)->mFlags, 0);
    EXPECT_EQ(runtime.search(driverId)->mCurrentStage, 0);
    EXPECT_FALSE(runtime.search(driverId)->mStageDone.at(5));
    ASSERT_NE(runtime.search(objectiveId), nullptr);
    EXPECT_EQ(runtime.search(objectiveId)->mObjectiveStatus.at(10), 0);
    ASSERT_NE(runtime.search(startId), nullptr);
    EXPECT_EQ(runtime.search(startId)->mFlags, 0);
    ASSERT_NE(runtime.search(completeId), nullptr);
    EXPECT_EQ(runtime.search(completeId)->mFlags, 0);
}

TEST(ESM4QuestRuntimeTest, ExecutesRetailVms38NestedSetStageAsOneTransaction)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x1348db, .mContentFile = 0 };
    ESM4::Quest quest = makeQuest(questId, "VMS38");
    quest.mObjectives.push_back({ .mIndex = 140, .mDescription = "Return to Red Lucy" });

    ESM4::QuestStageEntry stage140;
    // FalloutNV.esm VMS38 stage 140 entry 0, complete SCDA, SHA256
    // 210dadb71f7e8539f23fbfef10808e0df47310b1845d9297773c2a7eb04bcd33.
    stage140.mScript.compiledData = { 0xa3, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01, 0x00, 0x6e, 0x8c,
        0x00, 0x00, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00, 0x39, 0x10, 0x0a, 0x00, 0x02, 0x00, 0x72, 0x01, 0x00,
        0x6e, 0x96, 0x00, 0x00, 0x00 };
    stage140.mScript.references = { questId };
    quest.mStages.push_back({ .mIndex = 140, .mEntries = { std::move(stage140) } });

    ESM4::QuestStageEntry stage150;
    // FalloutNV.esm VMS38 stage 150 entry 0, complete SCDA, SHA256
    // d606b82e0020674e61f6f80b3f96bc40774a4fd367629c67424a5e5baafb4cbc.
    stage150.mScript.compiledData = { 0xa2, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01, 0x00, 0x6e, 0x8c,
        0x00, 0x00, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00 };
    stage150.mScript.references = { questId };
    quest.mStages.push_back({ .mIndex = 150, .mEntries = { std::move(stage150) } });
    store.overrideRecord(quest);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    ASSERT_TRUE(runtime.setStage(questId, 140));
    const MWWorld::ESM4QuestState* state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mCurrentStage, 150);
    EXPECT_TRUE(state->mStageDone.at(140));
    EXPECT_TRUE(state->mStageDone.at(150));
    EXPECT_EQ(state->mObjectiveStatus.at(140),
        MWWorld::ESM4QuestState::Objective_Displayed | MWWorld::ESM4QuestState::Objective_Completed);
    EXPECT_TRUE(runtime.getUnsupportedCompiledOpcodes().empty());
    EXPECT_TRUE(runtime.getUnsupportedConditionFunctions().empty());
}

TEST(ESM4QuestRuntimeTest, RejectsSelfAndMutualCompiledSetStageCyclesWithoutMutation)
{
    MWWorld::ESMStore store;
    const ESM::FormId selfId{ .mIndex = 0x120220, .mContentFile = 0 };
    ESM4::Quest self = makeQuest(selfId, "SelfStageCycle");
    self.mStages.push_back({ .mIndex = 5, .mEntries = { makeCompiledSetStageEntry({ selfId }, 1, 5) } });
    store.overrideRecord(self);

    const ESM::FormId firstId{ .mIndex = 0x120221, .mContentFile = 0 };
    const ESM::FormId secondId{ .mIndex = 0x120222, .mContentFile = 0 };
    ESM4::Quest first = makeQuest(firstId, "FirstStageCycle");
    first.mStages.push_back({ .mIndex = 5, .mEntries = { makeCompiledSetStageEntry({ secondId }, 1, 5) } });
    store.overrideRecord(first);
    ESM4::Quest second = makeQuest(secondId, "SecondStageCycle");
    second.mStages.push_back({ .mIndex = 5, .mEntries = { makeCompiledSetStageEntry({ firstId }, 1, 5) } });
    store.overrideRecord(second);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    EXPECT_FALSE(runtime.setStage(selfId, 5));
    EXPECT_FALSE(runtime.setStage(firstId, 5));
    for (const ESM::FormId id : { selfId, firstId, secondId })
    {
        const MWWorld::ESM4QuestState* state = runtime.search(id);
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->mFlags, 0);
        EXPECT_EQ(state->mCurrentStage, 0);
        EXPECT_FALSE(state->mStageDone.at(5));
    }
}

TEST(ESM4QuestRuntimeTest, RejectsInvalidCompiledSetStageQuestStageAndRange)
{
    MWWorld::ESMStore store;
    const ESM::FormId driverId{ .mIndex = 0x120223, .mContentFile = 0 };
    const ESM::FormId targetId{ .mIndex = 0x120224, .mContentFile = 0 };
    const ESM::FormId missingId{ .mIndex = 0x120225, .mContentFile = 0 };
    ESM4::Quest driver = makeQuest(driverId, "InvalidStageDriver");
    driver.mStages.push_back({ .mIndex = 5, .mEntries = { makeCompiledSetStageEntry({ missingId }, 1, 5) } });
    driver.mStages.push_back({ .mIndex = 6, .mEntries = { makeCompiledSetStageEntry({ targetId }, 1, 99) } });
    driver.mStages.push_back({ .mIndex = 7, .mEntries = { makeCompiledSetStageEntry({ targetId }, 1, -1) } });
    driver.mStages.push_back({ .mIndex = 8, .mEntries = { makeCompiledSetStageEntry({ targetId }, 1, 256) } });
    ESM4::QuestStageEntry wrongQuestType;
    wrongQuestType.mScript.compiledData = { 0x39, 0x10, 0x0c, 0x00, 0x02, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00,
        0x6e, 0x05, 0x00, 0x00, 0x00 };
    driver.mStages.push_back({ .mIndex = 9, .mEntries = { std::move(wrongQuestType) } });
    ESM4::QuestStageEntry wrongStageType;
    wrongStageType.mScript.compiledData
        = { 0x39, 0x10, 0x08, 0x00, 0x02, 0x00, 0x72, 0x01, 0x00, 0x72, 0x01, 0x00 };
    wrongStageType.mScript.references = { targetId };
    driver.mStages.push_back({ .mIndex = 10, .mEntries = { std::move(wrongStageType) } });
    store.overrideRecord(driver);
    ESM4::Quest target = makeQuest(targetId, "InvalidStageTarget");
    ESM4::QuestStageEntry targetEntry;
    targetEntry.mScript.compiledData = { 0x37, 0x10, 0x05, 0x00, 0x01, 0x00, 0x72, 0x01, 0x00 };
    targetEntry.mScript.references = { targetId };
    target.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(targetEntry) } });
    store.overrideRecord(target);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    for (const std::uint8_t stage : { 5, 6, 7, 8, 9, 10 })
        EXPECT_FALSE(runtime.setStage(driverId, stage)) << static_cast<unsigned int>(stage);
    const MWWorld::ESM4QuestState* driverState = runtime.search(driverId);
    ASSERT_NE(driverState, nullptr);
    EXPECT_EQ(driverState->mFlags, 0);
    EXPECT_EQ(driverState->mCurrentStage, 0);
    for (const std::uint8_t stage : { 5, 6, 7, 8, 9, 10 })
        EXPECT_FALSE(driverState->mStageDone.at(stage));
    EXPECT_TRUE(runtime.getUnsupportedCompiledOpcodes().empty());
}

TEST(ESM4QuestRuntimeTest, RollsBackActiveQuestAndStateWhenNestedStageHasLaterInvalidCommand)
{
    MWWorld::ESMStore store;
    const ESM::FormId baselineId{ .mIndex = 0x120226, .mContentFile = 0 };
    const ESM::FormId activeCandidateId{ .mIndex = 0x120227, .mContentFile = 0 };
    const ESM::FormId rootId{ .mIndex = 0x120228, .mContentFile = 0 };
    const ESM::FormId nestedId{ .mIndex = 0x120229, .mContentFile = 0 };
    store.overrideRecord(makeQuest(baselineId, "BaselineActiveQuest"));
    store.overrideRecord(makeQuest(activeCandidateId, "RejectedActiveQuest"));

    ESM4::Quest root = makeQuest(rootId, "TransactionalRoot");
    root.mObjectives.push_back({ .mIndex = 10, .mDescription = "Must remain hidden" });
    ESM4::QuestStageEntry rootEntry;
    rootEntry.mScript.references = { rootId, activeCandidateId, nestedId };
    rootEntry.mScript.compiledData = {
        0xa3, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01, 0x00, 0x6e, 0x0a, 0x00, 0x00, 0x00, 0x6e, 0x01, 0x00,
        0x00, 0x00, // SetObjectiveDisplayed root 10 1
        0xdd, 0x11, 0x05, 0x00, 0x01, 0x00, 0x72, 0x02, 0x00, // ForceActiveQuest candidate
        0x39, 0x10, 0x0a, 0x00, 0x02, 0x00, 0x72, 0x03, 0x00, 0x6e, 0x05, 0x00, 0x00, 0x00 // SetStage nested 5
    };
    root.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(rootEntry) } });
    store.overrideRecord(root);

    ESM4::Quest nested = makeQuest(nestedId, "TransactionalNested");
    nested.mObjectives.push_back({ .mIndex = 20, .mDescription = "Must remain hidden" });
    ESM4::QuestStageEntry nestedEntry;
    nestedEntry.mScript.references = { nestedId };
    nestedEntry.mScript.compiledData = {
        0xa3, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01, 0x00, 0x6e, 0x14, 0x00, 0x00, 0x00, 0x6e, 0x01, 0x00,
        0x00, 0x00, // valid objective prefix
        0x36, 0x10, 0x05, 0x00, 0x01, 0x00, 0x72, 0x02, 0x00 // invalid later StartQuest SCRO
    };
    nested.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(nestedEntry) } });
    store.overrideRecord(nested);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    ASSERT_TRUE(runtime.forceActiveQuest(baselineId));
    ASSERT_EQ(runtime.getActiveQuest(), baselineId);
    EXPECT_FALSE(runtime.setStage(rootId, 5));
    EXPECT_EQ(runtime.getActiveQuest(), baselineId);
    ASSERT_NE(runtime.search(rootId), nullptr);
    EXPECT_EQ(runtime.search(rootId)->mFlags, 0);
    EXPECT_FALSE(runtime.search(rootId)->mStageDone.at(5));
    EXPECT_EQ(runtime.search(rootId)->mObjectiveStatus.at(10), 0);
    ASSERT_NE(runtime.search(nestedId), nullptr);
    EXPECT_EQ(runtime.search(nestedId)->mFlags, 0);
    EXPECT_FALSE(runtime.search(nestedId)->mStageDone.at(5));
    EXPECT_EQ(runtime.search(nestedId)->mObjectiveStatus.at(20), 0);
    ASSERT_NE(runtime.search(activeCandidateId), nullptr);
    EXPECT_EQ(runtime.search(activeCandidateId)->mFlags, 0);
}

TEST(ESM4QuestRuntimeTest, RejectsImpureNestedStageWithoutTouchingFallbackOrUnsupportedLedgers)
{
    MWWorld::ESMStore store;
    const ESM::FormId driverId{ .mIndex = 0x12022a, .mContentFile = 0 };
    const ESM::FormId sourceId{ .mIndex = 0x12022b, .mContentFile = 0 };
    const ESM::FormId unsupportedId{ .mIndex = 0x12022c, .mContentFile = 0 };
    const ESM::FormId conditionedId{ .mIndex = 0x12022d, .mContentFile = 0 };
    ESM4::Quest driver = makeQuest(driverId, "ImpureNestedDriver");
    driver.mStages.push_back({ .mIndex = 5, .mEntries = { makeCompiledSetStageEntry({ sourceId }, 1, 5) } });
    driver.mStages.push_back({ .mIndex = 6, .mEntries = { makeCompiledSetStageEntry({ unsupportedId }, 1, 5) } });
    driver.mStages.push_back({ .mIndex = 7, .mEntries = { makeCompiledSetStageEntry({ conditionedId }, 1, 5) } });
    store.overrideRecord(driver);

    ESM4::Quest source = makeQuest(sourceId, "SourceFallbackTarget");
    source.mObjectives.push_back({ .mIndex = 10, .mDescription = "Must remain hidden" });
    ESM4::QuestStageEntry sourceEntry;
    sourceEntry.mScript.scriptSource = "SetObjectiveDisplayed SourceFallbackTarget 10 1";
    source.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(sourceEntry) } });
    store.overrideRecord(source);

    ESM4::Quest unsupported = makeQuest(unsupportedId, "UnsupportedTarget");
    unsupported.mObjectives.push_back({ .mIndex = 10, .mDescription = "Must remain hidden" });
    ESM4::QuestStageEntry unsupportedEntry;
    unsupportedEntry.mScript.compiledData = { 0xef, 0xbe, 0x03, 0x00, 0xaa, 0xbb, 0xcc };
    unsupportedEntry.mScript.scriptSource = "SetObjectiveDisplayed UnsupportedTarget 10 1";
    unsupported.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(unsupportedEntry) } });
    store.overrideRecord(unsupported);

    ESM4::Quest conditioned = makeQuest(conditionedId, "UnsupportedConditionTarget");
    conditioned.mObjectives.push_back({ .mIndex = 10, .mDescription = "Must remain hidden" });
    ESM4::QuestStageEntry conditionedEntry;
    conditionedEntry.mConditions = { makeCondition(9999, conditionedId, 1.f) };
    conditionedEntry.mScript.compiledData = { 0xa3, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01, 0x00, 0x6e,
        0x0a, 0x00, 0x00, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00 };
    conditionedEntry.mScript.references = { conditionedId };
    conditioned.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(conditionedEntry) } });
    store.overrideRecord(conditioned);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    EXPECT_FALSE(runtime.setStage(driverId, 5));
    EXPECT_FALSE(runtime.setStage(driverId, 6));
    EXPECT_FALSE(runtime.setStage(driverId, 7));
    const MWWorld::ESM4QuestState* driverState = runtime.search(driverId);
    ASSERT_NE(driverState, nullptr);
    EXPECT_EQ(driverState->mFlags, 0);
    EXPECT_EQ(driverState->mCurrentStage, 0);
    for (const std::uint8_t stage : { 5, 6, 7 })
        EXPECT_FALSE(driverState->mStageDone.at(stage));
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
    EXPECT_TRUE(runtime.getUnsupportedCompiledOpcodes().empty());
    EXPECT_TRUE(runtime.getUnsupportedConditionFunctions().empty());
    for (const ESM::FormId id : { sourceId, unsupportedId, conditionedId })
    {
        const MWWorld::ESM4QuestState* state = runtime.search(id);
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->mFlags, 0);
        EXPECT_FALSE(state->mStageDone.at(5));
        EXPECT_EQ(state->mObjectiveStatus.at(10), 0);
    }
}

TEST(ESM4QuestRuntimeTest, TreatsAlreadyDoneNonRepeatableNestedTargetAsTerminalBeforeCycleCheck)
{
    MWWorld::ESMStore store;
    const ESM::FormId driverId{ .mIndex = 0x12022e, .mContentFile = 0 };
    const ESM::FormId targetId{ .mIndex = 0x12022f, .mContentFile = 0 };
    ESM4::Quest driver = makeQuest(driverId, "DoneTargetDriver");
    driver.mStages.push_back({ .mIndex = 5, .mEntries = { makeCompiledSetStageEntry({ targetId }, 1, 5) } });
    store.overrideRecord(driver);
    ESM4::Quest target = makeQuest(targetId, "DoneTarget");
    ESM4::QuestStageEntry stopEntry;
    stopEntry.mScript.compiledData = { 0x37, 0x10, 0x05, 0x00, 0x01, 0x00, 0x72, 0x01, 0x00 };
    stopEntry.mScript.references = { targetId };
    target.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(stopEntry) } });
    store.overrideRecord(target);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    ASSERT_TRUE(runtime.setStage(targetId, 5));
    ASSERT_TRUE(runtime.search(targetId)->mStageDone.at(5));

    ESM4::Quest cyclicTarget = makeQuest(targetId, "DoneTarget");
    cyclicTarget.mStages.push_back(
        { .mIndex = 5, .mEntries = { makeCompiledSetStageEntry({ targetId }, 1, 5) } });
    store.overrideRecord(cyclicTarget);
    ASSERT_TRUE(runtime.setStage(driverId, 5));
    ASSERT_NE(runtime.search(driverId), nullptr);
    EXPECT_TRUE(runtime.search(driverId)->mStageDone.at(5));
    EXPECT_TRUE(runtime.search(targetId)->mStageDone.at(5));
}

TEST(ESM4QuestRuntimeTest, RejectsCompiledSetStageGraphBeyondExplicitDepthLimit)
{
    MWWorld::ESMStore store;
    std::array<ESM::FormId, 33> ids;
    for (std::size_t i = 0; i < ids.size(); ++i)
        ids[i] = ESM::FormId{ .mIndex = static_cast<std::uint32_t>(0x121000 + i), .mContentFile = 0 };
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        ESM4::Quest quest = makeQuest(ids[i], "SetStageDepth" + std::to_string(i));
        if (i + 1 < ids.size())
            quest.mStages.push_back(
                { .mIndex = 5, .mEntries = { makeCompiledSetStageEntry({ ids[i + 1] }, 1, 5) } });
        else
        {
            ESM4::QuestStageEntry terminal;
            terminal.mScript.compiledData = { 0x37, 0x10, 0x05, 0x00, 0x01, 0x00, 0x72, 0x01, 0x00 };
            terminal.mScript.references = { ids[i] };
            quest.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(terminal) } });
        }
        store.overrideRecord(quest);
    }

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    EXPECT_FALSE(runtime.setStage(ids.front(), 5));
    for (const ESM::FormId id : ids)
    {
        const MWWorld::ESM4QuestState* state = runtime.search(id);
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->mFlags, 0);
        EXPECT_EQ(state->mCurrentStage, 0);
        EXPECT_FALSE(state->mStageDone.at(5));
    }
}

TEST(ESM4QuestRuntimeTest, DecodesOneBasedScroReferencesAndRejectsEveryBoundedArgumentMismatch)
{
    const ESM::FormId first{ .mIndex = 0x100, .mContentFile = 0 };
    const ESM::FormId second{ .mIndex = 0x200, .mContentFile = 0 };
    const std::array<ESM::FormId, 2> references{ first, second };
    std::vector<ESM4::ScriptBytecodeArgument> arguments;

    const std::array<std::uint8_t, 10> exact{ 0x02, 0x00, 0x72, 0x02, 0x00, 0x6e, 0xf9, 0xff, 0xff, 0xff };
    const ESM4::ScriptBytecodeArgumentDecodeResult exactResult
        = ESM4::decodeFalloutScriptArguments(exact, references, arguments);
    ASSERT_TRUE(exactResult.succeeded());
    ASSERT_EQ(arguments.size(), 2);
    EXPECT_EQ(std::get<ESM::FormId>(arguments[0]), second);
    EXPECT_EQ(std::get<std::int32_t>(arguments[1]), -7);

    using Error = ESM4::ScriptBytecodeArgumentDecodeError;
    const auto expectFailure = [&](std::span<const std::uint8_t> payload, Error expected) {
        arguments = { first };
        const auto result = ESM4::decodeFalloutScriptArguments(payload, references, arguments);
        EXPECT_EQ(result.error, expected);
        EXPECT_TRUE(arguments.empty());
    };
    const std::array<std::uint8_t, 1> noCount{ 0x01 };
    const std::array<std::uint8_t, 2> missingArgument{ 0x01, 0x00 };
    const std::array<std::uint8_t, 3> unknownToken{ 0x01, 0x00, 0xff };
    const std::array<std::uint8_t, 4> truncatedReference{ 0x01, 0x00, 0x72, 0x01 };
    const std::array<std::uint8_t, 5> zeroReference{ 0x01, 0x00, 0x72, 0x00, 0x00 };
    const std::array<std::uint8_t, 5> outOfRangeReference{ 0x01, 0x00, 0x72, 0x03, 0x00 };
    const std::array<std::uint8_t, 6> truncatedInteger{ 0x01, 0x00, 0x6e, 0x01, 0x00, 0x00 };
    const std::array<std::uint8_t, 3> trailingData{ 0x00, 0x00, 0xff };
    expectFailure(noCount, Error::TruncatedArgumentCount);
    expectFailure(missingArgument, Error::ArgumentCountMismatch);
    expectFailure(unknownToken, Error::UnknownArgumentToken);
    expectFailure(truncatedReference, Error::TruncatedReference);
    expectFailure(zeroReference, Error::InvalidReferenceIndex);
    expectFailure(outOfRangeReference, Error::InvalidReferenceIndex);
    expectFailure(truncatedInteger, Error::TruncatedInteger);
    expectFailure(trailingData, Error::TrailingArgumentData);
}

TEST(ESM4QuestRuntimeTest, SurfacesUnsupportedCompiledOpcodeAndUsesWholeSourceFallback)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x120100, .mContentFile = 0 };
    ESM4::Quest quest = makeQuest(questId, "FallbackQuest");
    quest.mObjectives.push_back({ .mIndex = 3, .mDescription = "Fallback objective" });
    ESM4::QuestStageEntry entry;
    // Unsupported commands have their own parameter grammars. Only their frame and optional
    // calling-reference bounds are preflighted before the whole source script is used.
    entry.mScript.compiledData = { 0xef, 0xbe, 0x03, 0x00, 0xaa, 0xbb, 0xcc };
    entry.mScript.scriptSource = "SetObjectiveDisplayed FallbackQuest 3 1";
    quest.mStages.push_back({ .mIndex = 5, .mEntries = { entry } });
    store.overrideRecord(quest);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    ASSERT_TRUE(runtime.setStage(questId, 5));
    ASSERT_EQ(runtime.getUnsupportedCompiledOpcodes(), std::vector<std::uint16_t>{ 0xbeef });
    ASSERT_NE(runtime.search(questId), nullptr);
    EXPECT_EQ(runtime.search(questId)->mObjectiveStatus.at(3), MWWorld::ESM4QuestState::Objective_Displayed);
}

TEST(ESM4QuestRuntimeTest, PreservesNativeQuestEffectsWhenMixedOpcodeStageUsesSourceFallback)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x120130, .mContentFile = 0 };
    const ESM::FormId scriptId{ .mIndex = 0x120131, .mContentFile = 0 };
    const ESM::FormId rewardBookId{ .mIndex = 0x120132, .mContentFile = 0 };
    const ESM::FormId removedBookId{ .mIndex = 0x120133, .mContentFile = 0 };
    const ESM::FormId doorId{ .mIndex = 0x120134, .mContentFile = 0 };
    const ESM::FormId targetId{ .mIndex = 0x120135, .mContentFile = 0 };
    const ESM::FormId reputationId{ .mIndex = 0x120136, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "MixedFallbackQuest");
    quest.mQuestScript = scriptId;
    quest.mObjectives = {
        { .mIndex = 10, .mDescription = "First objective" },
        { .mIndex = 20, .mDescription = "Second objective" },
    };
    ESM4::QuestStageEntry entry;
    entry.mScript.compiledData = { 0xef, 0xbe, 0x03, 0x00, 0xaa, 0xbb, 0xcc };
    entry.mScript.scriptSource
        = "set MixedFallbackQuest.RewardCount to QuestRewardCount + 1\n"
          "Player.AddItem RewardBook MixedFallbackQuest.RewardCount 1\n"
          "Player.RemoveItem RemovedBook 1 0\n"
          "RewardXP QuestXpGlobal\n"
          "Player.RewardXP 25\n"
          "AddReputation NCRReputation 1 2\n"
          "Player.AddReputation NCRReputation 0 1\n"
          "SetReputation NCRReputation 0 40\n"
          "Player.AddReputationExact NCRReputation 1 QuestRewardCount + 1\n"
          "QuestDoor.Unlock\n"
          "QuestTarget.Kill Player\n"
          "QuestTarget.ResetAI\n"
          "CompleteAllObjectives MixedFallbackQuest";
    quest.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(entry) } });
    store.overrideRecord(quest);

    ESM4::Script script;
    script.mId = scriptId;
    script.mEditorId = "MixedFallbackQuestScript";
    script.mScript.localVarData = { { .index = 1, .variableName = "RewardCount" } };
    store.overrideRecord(script);
    store.overrideRecord(makeGlobal({ .mIndex = 0x120137, .mContentFile = 0 }, "QuestRewardCount", 1.f));
    store.overrideRecord(makeGlobal({ .mIndex = 0x120138, .mContentFile = 0 }, "QuestXpGlobal", 100.f));

    ESM4::Book rewardBook{};
    rewardBook.mId = rewardBookId;
    rewardBook.mEditorId = "RewardBook";
    store.overrideRecord(rewardBook);
    ESM4::Book removedBook{};
    removedBook.mId = removedBookId;
    removedBook.mEditorId = "RemovedBook";
    store.overrideRecord(removedBook);

    ESM4::Reference door{};
    door.mId = doorId;
    door.mEditorId = "QuestDoor";
    store.overrideRecord(door);
    ESM4::ActorCharacter target{};
    target.mId = targetId;
    target.mEditorId = "QuestTarget";
    store.overrideRecord(target);

    ESM4::Reputation reputation;
    reputation.mId = reputationId;
    reputation.mEditorId = "NCRReputation";
    reputation.mMaximum = 100.f;
    store.overrideRecord(reputation);

    MWWorld::Globals globals;
    globals.fill(store);
    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store, &globals);

    std::vector<std::tuple<ESM::FormId, ESM::FormId, int>> addedItems;
    std::vector<std::tuple<ESM::FormId, ESM::FormId, int>> removedItems;
    std::vector<int> rewardedXp;
    std::vector<std::tuple<ESM::FormId, bool, int>> reputationChanges;
    std::vector<std::tuple<ESM::FormId, MWWorld::ESM4QuestReputationValueCommand, bool, float>>
        reputationValueChanges;
    std::vector<std::pair<MWWorld::ESM4QuestReferenceCommand, ESM::FormId>> referenceCommands;
    runtime.setAddItemHandler([&](ESM::FormId owner, ESM::FormId item, int count) {
        addedItems.emplace_back(owner, item, count);
        return true;
    });
    runtime.setRemoveItemHandler([&](ESM::FormId owner, ESM::FormId item, int count) {
        removedItems.emplace_back(owner, item, count);
        return true;
    });
    runtime.setRewardXpHandler([&](int amount) {
        rewardedXp.push_back(amount);
        return true;
    });
    runtime.setAddReputationHandler([&](ESM::FormId id, bool fame, int bump) {
        reputationChanges.emplace_back(id, fame, bump);
        return true;
    });
    runtime.setReputationValueCommandHandler(
        [&](ESM::FormId id, MWWorld::ESM4QuestReputationValueCommand command, bool fame, float amount) {
            reputationValueChanges.emplace_back(id, command, fame, amount);
            return true;
        });
    runtime.setReferenceCommandHandler([&](MWWorld::ESM4QuestReferenceCommand command, ESM::FormId target) {
        referenceCommands.emplace_back(command, target);
        return true;
    });

    ASSERT_TRUE(runtime.setStage(questId, 5));
    const std::optional<float> rewardCount = runtime.getQuestVariable("MixedFallbackQuest", "RewardCount");
    ASSERT_TRUE(rewardCount.has_value());
    EXPECT_FLOAT_EQ(*rewardCount, 2.f);

    ASSERT_EQ(addedItems.size(), 1);
    EXPECT_EQ(addedItems[0], std::tuple(ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 }, rewardBookId, 2));
    ASSERT_EQ(removedItems.size(), 1);
    EXPECT_EQ(removedItems[0], std::tuple(ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 }, removedBookId, 1));
    EXPECT_EQ(rewardedXp, (std::vector<int>{ 100, 25 }));
    ASSERT_EQ(reputationChanges.size(), 2);
    EXPECT_EQ(reputationChanges[0], std::tuple(reputationId, true, 2));
    EXPECT_EQ(reputationChanges[1], std::tuple(reputationId, false, 1));
    ASSERT_EQ(reputationValueChanges.size(), 2);
    EXPECT_EQ(reputationValueChanges[0],
        std::tuple(reputationId, MWWorld::ESM4QuestReputationValueCommand::Set, false, 40.f));
    EXPECT_EQ(reputationValueChanges[1],
        std::tuple(reputationId, MWWorld::ESM4QuestReputationValueCommand::AddExact, true, 2.f));
    ASSERT_EQ(referenceCommands.size(), 3);
    EXPECT_EQ(referenceCommands[0], std::pair(MWWorld::ESM4QuestReferenceCommand::Unlock, doorId));
    EXPECT_EQ(referenceCommands[1], std::pair(MWWorld::ESM4QuestReferenceCommand::Kill, targetId));
    EXPECT_EQ(referenceCommands[2], std::pair(MWWorld::ESM4QuestReferenceCommand::ResetAi, targetId));

    const MWWorld::ESM4QuestState* const state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_NE(state->mObjectiveStatus.at(10) & MWWorld::ESM4QuestState::Objective_Completed, 0);
    EXPECT_NE(state->mObjectiveStatus.at(20) & MWWorld::ESM4QuestState::Objective_Completed, 0);
    EXPECT_EQ(runtime.getUnsupportedCompiledOpcodes(), (std::vector<std::uint16_t>{ 0xbeef }));
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
}

TEST(ESM4QuestRuntimeTest, ExecutesConditionedItemRewardsAndScriptedLocksFromSourceFallback)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x120139, .mContentFile = 0 };
    const ESM::FormId weaponId{ .mIndex = 0x12013a, .mContentFile = 0 };
    const ESM::FormId doorId{ .mIndex = 0x12013b, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "ConditionRewardQuest");
    ESM4::QuestStageEntry entry;
    entry.mScript.scriptSource
        = "Player.AddItemHealthPercent ConditionRewardWeapon 2 .85\n"
          "ConditionRewardDoor.Lock 255\n"
          "ConditionRewardDoor.Lock";
    quest.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(entry) } });
    store.overrideRecord(quest);

    ESM4::Weapon weapon;
    weapon.mId = weaponId;
    weapon.mEditorId = "ConditionRewardWeapon";
    weapon.mData.health = 400;
    store.overrideRecord(weapon);

    ESM4::Reference door;
    door.mId = doorId;
    door.mEditorId = "ConditionRewardDoor";
    store.overrideRecord(door);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);

    std::vector<std::tuple<ESM::FormId, ESM::FormId, int, float>> conditionedItems;
    std::vector<std::pair<ESM::FormId, std::optional<int>>> locks;
    runtime.setAddItemHealthPercentHandler(
        [&](ESM::FormId owner, ESM::FormId item, int count, float healthPercent) {
            conditionedItems.emplace_back(owner, item, count, healthPercent);
            return true;
        });
    runtime.setLockHandler([&](ESM::FormId reference, std::optional<int> lockLevel) {
        locks.emplace_back(reference, lockLevel);
        return true;
    });

    ASSERT_TRUE(runtime.setStage(questId, 5));
    ASSERT_EQ(conditionedItems.size(), 1);
    EXPECT_EQ(std::get<0>(conditionedItems.front()), (ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 }));
    EXPECT_EQ(std::get<1>(conditionedItems.front()), weaponId);
    EXPECT_EQ(std::get<2>(conditionedItems.front()), 2);
    EXPECT_FLOAT_EQ(std::get<3>(conditionedItems.front()), .85f);
    EXPECT_EQ(locks,
        (std::vector<std::pair<ESM::FormId, std::optional<int>>>{
            { doorId, 255 },
            { doorId, std::nullopt },
        }));
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
}

TEST(ESM4QuestRuntimeTest, ExecutesActorValuesAndPerReferenceFactionsFromSourceFallback)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x12013c, .mContentFile = 0 };
    const ESM::FormId actorId{ .mIndex = 0x12013d, .mContentFile = 0 };
    const ESM::FormId factionId{ .mIndex = 0x12013e, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "ActorStateQuest");
    quest.mObjectives.push_back({ .mIndex = 10, .mDescription = "Actor values applied" });
    quest.mObjectives.push_back({ .mIndex = 20, .mDescription = "Faction removed" });
    ESM4::QuestStageEntry mutateEntry;
    mutateEntry.mScript.scriptSource
        = "ActorStateRef.SetAV Aggression 1\n"
          "ActorStateRef.ModAV Repair 30\n"
          "ActorStateRef.RestoreAV LeftMobilityCondition 100\n"
          "ActorStateRef.AddToFaction ActorStateFaction 2";
    ESM4::QuestStageEntry presentConditionEntry;
    presentConditionEntry.mScript.scriptSource
        =
          "if ActorStateRef.GetAV Aggression == 1 && ActorStateRef.GetFactionRank ActorStateFaction == 2\n"
          "  SetObjectiveDisplayed ActorStateQuest 10 1\n"
          "endif";
    ESM4::QuestStageEntry removeEntry;
    removeEntry.mScript.scriptSource = "ActorStateRef.RemoveFromFaction ActorStateFaction";
    ESM4::QuestStageEntry absentConditionEntry;
    absentConditionEntry.mScript.scriptSource
        =
          "if ActorStateRef.GetInFaction ActorStateFaction == 0\n"
          "  SetObjectiveDisplayed ActorStateQuest 20 1\n"
          "endif";
    quest.mStages.push_back({ .mIndex = 5,
        .mEntries = { std::move(mutateEntry), std::move(presentConditionEntry), std::move(removeEntry),
            std::move(absentConditionEntry) } });
    store.overrideRecord(quest);

    ESM4::Reference actor;
    actor.mId = actorId;
    actor.mEditorId = "ActorStateRef";
    store.overrideRecord(actor);

    ESM4::Faction faction;
    faction.mId = factionId;
    faction.mEditorId = "ActorStateFaction";
    store.overrideRecord(faction);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);

    std::vector<std::tuple<ESM::FormId, MWWorld::ESM4QuestActorValueCommand, std::string, float>>
        actorValueCommands;
    std::map<std::string, float, std::less<>> actorValues;
    actorValues["aggression"] = 0.f;
    actorValues["repair"] = 50.f;
    actorValues["leftmobilitycondition"] = 0.f;
    runtime.setActorValueCommandHandler(
        [&](ESM::FormId actorRef, MWWorld::ESM4QuestActorValueCommand command,
            std::string_view actorValue, float value) {
            actorValueCommands.emplace_back(actorRef, command, std::string(actorValue), value);
            std::string key(actorValue);
            std::ranges::transform(key, key.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            float& current = actorValues[key];
            if (command == MWWorld::ESM4QuestActorValueCommand::Set)
                current = value;
            else
                current += value;
            return true;
        });
    runtime.setActorValueHandler([&](ESM::FormId actorRef, std::string_view actorValue) -> std::optional<float> {
        if (actorRef != actorId)
            return std::nullopt;
        std::string key(actorValue);
        std::ranges::transform(key, key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        const auto found = actorValues.find(key);
        return found != actorValues.end() ? std::optional<float>(found->second) : std::nullopt;
    });

    std::optional<int> factionRank;
    runtime.setActorFactionCommandHandler(
        [&](ESM::FormId actorRef, ESM::FormId factionRef, std::optional<int> rank) {
            if (actorRef != actorId || factionRef != factionId)
                return false;
            factionRank = rank;
            return true;
        });
    runtime.setActorFactionMembershipHandler(
        [&](ESM::FormId actorRef, ESM::FormId factionRef)
            -> std::optional<MWWorld::ESM4QuestFactionMembership> {
            if (actorRef != actorId || factionRef != factionId)
                return std::nullopt;
            return MWWorld::ESM4QuestFactionMembership{
                factionRank.has_value(), static_cast<std::int8_t>(factionRank.value_or(-1)) };
        });

    ASSERT_TRUE(runtime.setStage(questId, 5));
    ASSERT_EQ(actorValueCommands.size(), 3);
    EXPECT_EQ(std::get<1>(actorValueCommands[0]), MWWorld::ESM4QuestActorValueCommand::Set);
    EXPECT_EQ(std::get<1>(actorValueCommands[1]), MWWorld::ESM4QuestActorValueCommand::Mod);
    EXPECT_EQ(std::get<1>(actorValueCommands[2]), MWWorld::ESM4QuestActorValueCommand::Restore);
    EXPECT_FALSE(factionRank.has_value());

    const MWWorld::ESM4QuestState* const state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_NE(state->mObjectiveStatus.at(10) & MWWorld::ESM4QuestState::Objective_Displayed, 0);
    EXPECT_NE(state->mObjectiveStatus.at(20) & MWWorld::ESM4QuestState::Objective_Displayed, 0);
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
}

TEST(ESM4QuestRuntimeTest, ExecutesSetFactionRankAndTreatsMinusOneAsRemoval)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x12015d, .mContentFile = 0 };
    const ESM::FormId factionId{ .mIndex = 0x12015e, .mContentFile = 0 };
    const ESM::FormId playerId{ .mIndex = 0x14, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "SetFactionRankQuest");
    ESM4::QuestStageEntry entry;
    entry.mScript.scriptSource
        = "Player.SetFactionRank SetFactionRankFaction 0\n"
          "Player.SetFactionRank SetFactionRankFaction -1";
    quest.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(entry) } });
    store.overrideRecord(quest);

    ESM4::Faction faction;
    faction.mId = factionId;
    faction.mEditorId = "SetFactionRankFaction";
    store.overrideRecord(faction);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    std::vector<std::optional<int>> ranks;
    runtime.setActorFactionCommandHandler(
        [&](ESM::FormId actor, ESM::FormId factionRef, std::optional<int> rank) {
            if (actor != playerId || factionRef != factionId)
                return false;
            ranks.push_back(rank);
            return true;
        });

    ASSERT_TRUE(runtime.setStage(questId, 5));
    ASSERT_EQ(ranks.size(), 2);
    EXPECT_EQ(ranks[0], 0);
    EXPECT_FALSE(ranks[1].has_value());
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
}

TEST(ESM4QuestRuntimeTest, RoutesScriptedActivateThroughTheReferenceActivationHandler)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x12015f, .mContentFile = 0 };
    const ESM::FormId targetId{ .mIndex = 0x120168, .mContentFile = 0 };
    const ESM::FormId activatorId{ .mIndex = 0x120169, .mContentFile = 0 };
    const ESM::FormId playerId{ .mIndex = 0x14, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "ReferenceActivationQuest");
    ESM4::QuestStageEntry entry;
    entry.mScript.scriptSource
        = "ActivationTarget.Activate\n"
          "ActivationTarget.Activate Player\n"
          "ActivationTarget.Activate ActivationActor 1";
    quest.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(entry) } });
    store.overrideRecord(quest);

    ESM4::Reference target;
    target.mId = targetId;
    target.mEditorId = "ActivationTarget";
    store.overrideRecord(target);
    ESM4::Reference activator;
    activator.mId = activatorId;
    activator.mEditorId = "ActivationActor";
    store.overrideRecord(activator);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    std::vector<std::tuple<ESM::FormId, ESM::FormId, bool>> activations;
    runtime.setReferenceActivationHandler(
        [&](ESM::FormId targetRef, ESM::FormId activatorRef, bool runOnActivateBlock) {
            activations.emplace_back(targetRef, activatorRef, runOnActivateBlock);
            return true;
        });

    ASSERT_TRUE(runtime.setStage(questId, 5));
    EXPECT_EQ(activations,
        (std::vector<std::tuple<ESM::FormId, ESM::FormId, bool>>{
            { targetId, playerId, false },
            { targetId, playerId, false },
            { targetId, activatorId, true },
        }));
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
}

TEST(ESM4QuestRuntimeTest, RoutesPlayIdleThroughTheNativeActorIdleHandler)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x12016a, .mContentFile = 0 };
    const ESM::FormId actorId{ .mIndex = 0x12016b, .mContentFile = 0 };
    const ESM::FormId idleId{ .mIndex = 0x12016c, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "PlayIdleQuest");
    ESM4::QuestStageEntry entry;
    entry.mScript.scriptSource = "IdleActor.PlayIdle LooseTestIdle";
    quest.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(entry) } });
    store.overrideRecord(quest);

    ESM4::ActorCharacter actor;
    actor.mId = actorId;
    actor.mEditorId = "IdleActor";
    store.overrideRecord(actor);
    ESM4::IdleAnimation idle;
    idle.mId = idleId;
    idle.mEditorId = "LooseTestIdle";
    idle.mModel = "meshes\\characters\\_male\\idleanims\\loosetestidle.kf";
    store.overrideRecord(idle);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    std::vector<std::pair<ESM::FormId, ESM::FormId>> playedIdles;
    runtime.setActorIdleHandler([&](ESM::FormId actorRef, ESM::FormId idleRef) {
        playedIdles.emplace_back(actorRef, idleRef);
        return true;
    });

    ASSERT_TRUE(runtime.setStage(questId, 5));
    EXPECT_EQ(playedIdles, (std::vector<std::pair<ESM::FormId, ESM::FormId>>{ { actorId, idleId } }));
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
}

TEST(ESM4QuestRuntimeTest, RoutesPlayGroupThroughTheNativeReferenceAnimationHandler)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x12016d, .mContentFile = 0 };
    const ESM::FormId referenceId{ .mIndex = 0x12016e, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "PlayGroupQuest");
    ESM4::QuestStageEntry entry;
    entry.mScript.scriptSource
        = "AnimatedReference.PlayGroup Forward 1\n"
          "AnimatedReference.PlayGroup Backward";
    quest.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(entry) } });
    store.overrideRecord(quest);

    ESM4::Reference reference;
    reference.mId = referenceId;
    reference.mEditorId = "AnimatedReference";
    store.overrideRecord(reference);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    std::vector<std::tuple<ESM::FormId, std::string, int>> groups;
    runtime.setReferenceAnimationGroupHandler(
        [&](ESM::FormId referenceRef, std::string_view group, int mode) {
            groups.emplace_back(referenceRef, group, mode);
            return true;
        });

    ASSERT_TRUE(runtime.setStage(questId, 5));
    EXPECT_EQ(groups,
        (std::vector<std::tuple<ESM::FormId, std::string, int>>{
            { referenceId, "Forward", 1 },
            { referenceId, "Backward", 0 },
        }));
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
}

TEST(ESM4QuestRuntimeTest, ExecutesPersistentActorControlEquipmentAndInventoryCommandsFromSourceFallback)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x120160, .mContentFile = 0 };
    const ESM::FormId actorId{ .mIndex = 0x120161, .mContentFile = 0 };
    const ESM::FormId targetId{ .mIndex = 0x120162, .mContentFile = 0 };
    const ESM::FormId destinationId{ .mIndex = 0x120163, .mContentFile = 0 };
    const ESM::FormId weaponId{ .mIndex = 0x120164, .mContentFile = 0 };
    const ESM::FormId exceptionListId{ .mIndex = 0x120165, .mContentFile = 0 };
    const ESM::FormId spellId{ .mIndex = 0x120166, .mContentFile = 0 };
    const ESM::FormId nameMessageId{ .mIndex = 0x120167, .mContentFile = 0 };
    const ESM::FormId playerId{ .mIndex = 0x14, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "ActorControlQuest");
    quest.mObjectives.push_back({ .mIndex = 10, .mDescription = "Actor flags applied" });
    ESM4::QuestStageEntry commandEntry;
    commandEntry.mScript.scriptSource
        = "ActorControlRef.SetUnconscious 1\n"
          "ActorControlRef.SetRestrained 1\n"
          "ActorControlRef.SetPlayerTeammate 1\n"
          "ActorControlRef.IgnoreCrime 1\n"
          "ActorControlRef.SetGhost 1\n"
          "ActorControlRef.SetIgnoreFriendlyHits 1\n"
          "ActorControlRef.SetAlert 1\n"
          "ActorControlRef.EquipItem ActorControlWeapon 1 1\n"
          "ActorControlRef.UnequipItem ActorControlWeapon 1\n"
          "ActorControlRef.StartCombat ActorControlTarget\n"
          "ActorControlRef.StopCombat\n"
          "ActorControlRef.Look ActorControlTarget 1\n"
          "ActorControlRef.StopLook ActorControlTarget\n"
          "ActorControlRef.ResetHealth\n"
          "ActorControlRef.AddSpell ActorControlAbility\n"
          "ActorControlRef.RemoveSpell ActorControlAbility\n"
          "RemoveSpell ActorControlAbility\n"
          "ActorControlRef.SetActorFullName ActorControlName\n"
          "ActorControlRef.ResetInventory\n"
          "ActorControlRef.RemoveAllItems ActorControlDestination 0 1\n"
          "ActorControlRef.RemoveAllTypedItems player 0 1 108 ActorControlExceptions\n"
          "player.EquipItem ActorControlWeapon 1 1\n"
          "player.UnequipItem ActorControlWeapon 1\n"
          "player.RemoveAllItems";
    ESM4::QuestStageEntry conditionEntry;
    conditionEntry.mScript.scriptSource
        = "if ActorControlRef.GetUnconscious == 1 && ActorControlRef.GetRestrained == 1\n"
          "  if ActorControlRef.GetPlayerTeammate == 1 && ActorControlRef.GetIgnoreCrime == 1\n"
          "    if ActorControlRef.GetIsGhost == 1 && ActorControlRef.GetIgnoreFriendlyHits == 1\n"
          "      if ActorControlRef.GetIsAlerted == 1\n"
          "        SetObjectiveDisplayed ActorControlQuest 10 1\n"
          "      endif\n"
          "    endif\n"
          "  endif\n"
          "endif";
    quest.mStages.push_back(
        { .mIndex = 5, .mEntries = { std::move(commandEntry), std::move(conditionEntry) } });
    store.overrideRecord(quest);

    for (const auto& [id, editor] : std::array{
             std::pair{ actorId, std::string_view("ActorControlRef") },
             std::pair{ targetId, std::string_view("ActorControlTarget") },
             std::pair{ destinationId, std::string_view("ActorControlDestination") },
         })
    {
        ESM4::Reference reference;
        reference.mId = id;
        reference.mEditorId = editor;
        store.overrideRecord(reference);
    }
    ESM4::Weapon weapon;
    weapon.mId = weaponId;
    weapon.mEditorId = "ActorControlWeapon";
    store.overrideRecord(weapon);
    ESM4::FormIdList exceptionList;
    exceptionList.mId = exceptionListId;
    exceptionList.mEditorId = "ActorControlExceptions";
    exceptionList.mObjects.push_back(weaponId);
    store.overrideRecord(exceptionList);
    ESM4::Spell spell;
    spell.mId = spellId;
    spell.mEditorId = "ActorControlAbility";
    spell.mData.present = true;
    spell.mData.type = ESM4::Spell::Type::Ability;
    store.overrideRecord(spell);
    ESM4::Message nameMessage;
    nameMessage.mId = nameMessageId;
    nameMessage.mEditorId = "ActorControlName";
    nameMessage.mFullName = "Dog";
    store.overrideRecord(nameMessage);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);

    std::map<MWWorld::ESM4QuestActorFlag, bool> flags;
    runtime.setActorFlagCommandHandler(
        [&](ESM::FormId actor, MWWorld::ESM4QuestActorFlag flag, bool enabled) {
            if (actor != actorId)
                return false;
            flags[flag] = enabled;
            return true;
        });
    runtime.setActorFlagHandler(
        [&](ESM::FormId actor, MWWorld::ESM4QuestActorFlag flag) -> std::optional<bool> {
            if (actor != actorId)
                return std::nullopt;
            const auto found = flags.find(flag);
            return found != flags.end() ? std::optional<bool>(found->second) : std::nullopt;
        });

    std::vector<std::tuple<ESM::FormId, ESM::FormId, bool>> equipment;
    runtime.setEquipmentCommandHandler([&](ESM::FormId actor, ESM::FormId item, bool equip) {
        equipment.emplace_back(actor, item, equip);
        return (actor == actorId || actor == playerId) && item == weaponId;
    });
    std::vector<std::tuple<MWWorld::ESM4QuestActorCommand, ESM::FormId, ESM::FormId, bool>> actorCommands;
    runtime.setActorCommandHandler(
        [&](MWWorld::ESM4QuestActorCommand command, ESM::FormId actor, ESM::FormId target,
            bool commandFlag) {
            actorCommands.emplace_back(command, actor, target, commandFlag);
            return actor == actorId;
        });
    std::vector<std::tuple<ESM::FormId, ESM::FormId, bool>> actorEffects;
    runtime.setActorEffectCommandHandler([&](ESM::FormId actor, ESM::FormId spell, bool add) {
        actorEffects.emplace_back(actor, spell, add);
        return (actor == actorId || actor == playerId) && spell == spellId;
    });
    std::vector<std::pair<ESM::FormId, std::string>> actorNames;
    runtime.setActorNameCommandHandler([&](ESM::FormId actor, std::string_view name) {
        actorNames.emplace_back(actor, name);
        return actor == actorId && name == "Dog";
    });
    std::vector<std::tuple<ESM::FormId, std::optional<ESM::FormId>, bool>> removals;
    runtime.setRemoveAllItemsHandler(
        [&](ESM::FormId owner, std::optional<ESM::FormId> destination, bool retainOwnership) {
            removals.emplace_back(owner, destination, retainOwnership);
            return (owner == actorId && destination == destinationId && !retainOwnership)
                || (owner == playerId && !destination && !retainOwnership);
        });
    std::vector<std::tuple<ESM::FormId, std::optional<ESM::FormId>, bool, std::int32_t,
        std::optional<ESM::FormId>>>
        typedRemovals;
    runtime.setRemoveAllTypedItemsHandler([&](ESM::FormId owner,
                                              std::optional<ESM::FormId> destination,
                                              bool retainOwnership, std::int32_t type,
                                              std::optional<ESM::FormId> exception) {
        typedRemovals.emplace_back(owner, destination, retainOwnership, type, exception);
        return owner == actorId && destination == playerId && !retainOwnership && type == 108
            && exception == exceptionListId;
    });

    ASSERT_TRUE(runtime.setStage(questId, 5));
    EXPECT_EQ(flags.size(), 7);
    EXPECT_TRUE(flags[MWWorld::ESM4QuestActorFlag::Unconscious]);
    EXPECT_TRUE(flags[MWWorld::ESM4QuestActorFlag::Restrained]);
    EXPECT_TRUE(flags[MWWorld::ESM4QuestActorFlag::PlayerTeammate]);
    EXPECT_TRUE(flags[MWWorld::ESM4QuestActorFlag::IgnoreCrime]);
    EXPECT_TRUE(flags[MWWorld::ESM4QuestActorFlag::Ghost]);
    EXPECT_TRUE(flags[MWWorld::ESM4QuestActorFlag::IgnoreFriendlyHits]);
    EXPECT_TRUE(flags[MWWorld::ESM4QuestActorFlag::Alert]);
    ASSERT_EQ(equipment.size(), 4);
    EXPECT_TRUE(std::get<2>(equipment[0]));
    EXPECT_FALSE(std::get<2>(equipment[1]));
    EXPECT_EQ(std::get<0>(equipment[2]), playerId);
    EXPECT_TRUE(std::get<2>(equipment[2]));
    EXPECT_EQ(std::get<0>(equipment[3]), playerId);
    EXPECT_FALSE(std::get<2>(equipment[3]));
    ASSERT_EQ(actorCommands.size(), 6);
    EXPECT_EQ(std::get<0>(actorCommands[0]), MWWorld::ESM4QuestActorCommand::StartCombat);
    EXPECT_EQ(std::get<2>(actorCommands[0]), targetId);
    EXPECT_EQ(std::get<0>(actorCommands[1]), MWWorld::ESM4QuestActorCommand::StopCombat);
    EXPECT_EQ(std::get<0>(actorCommands[2]), MWWorld::ESM4QuestActorCommand::Look);
    EXPECT_EQ(std::get<2>(actorCommands[2]), targetId);
    EXPECT_TRUE(std::get<3>(actorCommands[2]));
    EXPECT_EQ(std::get<0>(actorCommands[3]), MWWorld::ESM4QuestActorCommand::StopLook);
    EXPECT_EQ(std::get<2>(actorCommands[3]), targetId);
    EXPECT_EQ(std::get<0>(actorCommands[4]), MWWorld::ESM4QuestActorCommand::ResetHealth);
    EXPECT_TRUE(std::get<2>(actorCommands[4]).isZeroOrUnset());
    EXPECT_EQ(std::get<0>(actorCommands[5]), MWWorld::ESM4QuestActorCommand::ResetInventory);
    EXPECT_EQ(std::get<1>(actorCommands[5]), actorId);
    ASSERT_EQ(actorEffects.size(), 3);
    EXPECT_EQ(actorEffects[0], std::tuple(actorId, spellId, true));
    EXPECT_EQ(actorEffects[1], std::tuple(actorId, spellId, false));
    EXPECT_EQ(actorEffects[2], std::tuple(playerId, spellId, false));
    ASSERT_EQ(actorNames.size(), 1);
    EXPECT_EQ(actorNames.front(), std::pair(actorId, std::string("Dog")));
    ASSERT_EQ(removals.size(), 2);
    EXPECT_EQ(std::get<1>(removals[0]), destinationId);
    EXPECT_FALSE(std::get<2>(removals[0]));
    EXPECT_EQ(std::get<0>(removals[1]), playerId);
    EXPECT_FALSE(std::get<1>(removals[1]));
    EXPECT_FALSE(std::get<2>(removals[1]));
    ASSERT_EQ(typedRemovals.size(), 1);
    EXPECT_EQ(std::get<0>(typedRemovals[0]), actorId);
    EXPECT_EQ(std::get<1>(typedRemovals[0]), playerId);
    EXPECT_FALSE(std::get<2>(typedRemovals[0]));
    EXPECT_EQ(std::get<3>(typedRemovals[0]), 108);
    EXPECT_EQ(std::get<4>(typedRemovals[0]), exceptionListId);

    const MWWorld::ESM4QuestState* const state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_NE(state->mObjectiveStatus.at(10) & MWWorld::ESM4QuestState::Objective_Displayed, 0);
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
}

TEST(ESM4QuestRuntimeTest, EvaluatesQuestActorAndInventoryFunctionsInSourceFallbackConditions)
{
    MWWorld::ESMStore store;
    const ESM::FormId driverId{ .mIndex = 0x120140, .mContentFile = 0 };
    const ESM::FormId runningId{ .mIndex = 0x120141, .mContentFile = 0 };
    const ESM::FormId completedId{ .mIndex = 0x120142, .mContentFile = 0 };
    const ESM::FormId deadActorId{ .mIndex = 0x120143, .mContentFile = 0 };
    const ESM::FormId liveActorId{ .mIndex = 0x120144, .mContentFile = 0 };
    const ESM::FormId inventoryActorId{ .mIndex = 0x120145, .mContentFile = 0 };
    const ESM::FormId bookId{ .mIndex = 0x120146, .mContentFile = 0 };

    ESM4::Quest driver = makeQuest(driverId, "ConditionFallbackDriver");
    for (const std::int32_t objective : { 10, 20, 30, 40 })
        driver.mObjectives.push_back({ .mIndex = objective, .mDescription = "Condition objective" });
    ESM4::QuestStageEntry entry;
    entry.mScript.scriptSource
        = "if GetQuestRunning RunningQuest\n"
          "  SetObjectiveDisplayed ConditionFallbackDriver 10 1\n"
          "endif\n"
          "if GetQuestCompleted CompletedQuest\n"
          "  SetObjectiveDisplayed ConditionFallbackDriver 20 1\n"
          "endif\n"
          "if DeadActor.GetDead == 1 && GetDead LiveActor == 0\n"
          "  SetObjectiveDisplayed ConditionFallbackDriver 30 1\n"
          "endif\n"
          "if Player.GetItemCount ConditionBook == 2 && InventoryActor.GetItemCount ConditionBook >= 3\n"
          "  SetObjectiveDisplayed ConditionFallbackDriver 40 1\n"
          "endif";
    driver.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(entry) } });
    store.overrideRecord(driver);
    store.overrideRecord(makeQuest(runningId, "RunningQuest"));
    store.overrideRecord(makeQuest(completedId, "CompletedQuest"));

    for (const auto& [id, editorId] : std::array{
             std::pair{ deadActorId, std::string_view{ "DeadActor" } },
             std::pair{ liveActorId, std::string_view{ "LiveActor" } },
             std::pair{ inventoryActorId, std::string_view{ "InventoryActor" } },
         })
    {
        ESM4::ActorCharacter actor{};
        actor.mId = id;
        actor.mEditorId = editorId;
        store.overrideRecord(actor);
    }
    ESM4::Book book{};
    book.mId = bookId;
    book.mEditorId = "ConditionBook";
    store.overrideRecord(book);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    runtime.setActorDeadHandler([&](ESM::FormId actor) -> std::optional<bool> {
        if (actor == deadActorId)
            return true;
        if (actor == liveActorId)
            return false;
        return std::nullopt;
    });
    runtime.setItemCountHandler([&](ESM::FormId owner, ESM::FormId item) -> std::optional<int> {
        if (item != bookId)
            return std::nullopt;
        if (owner.mIndex == 0x14)
            return 2;
        if (owner == inventoryActorId)
            return 3;
        return std::nullopt;
    });

    ASSERT_TRUE(runtime.startQuest(runningId));
    ASSERT_TRUE(runtime.startQuest(completedId));
    ASSERT_TRUE(runtime.completeQuest(completedId));
    ASSERT_TRUE(runtime.setStage(driverId, 5));

    const MWWorld::ESM4QuestState* const state = runtime.search(driverId);
    ASSERT_NE(state, nullptr);
    for (const std::int32_t objective : { 10, 20, 30, 40 })
        EXPECT_NE(state->mObjectiveStatus.at(objective) & MWWorld::ESM4QuestState::Objective_Displayed, 0);
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
}

TEST(ESM4QuestRuntimeTest, PreservesPersistentPlayerRewardsAndEssentialFlagsInSourceFallback)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x120150, .mContentFile = 0 };
    const ESM::FormId firstNoteId{ .mIndex = 0x120151, .mContentFile = 0 };
    const ESM::FormId secondNoteId{ .mIndex = 0x120152, .mContentFile = 0 };
    const ESM::FormId perkId{ .mIndex = 0x120153, .mContentFile = 0 };
    const ESM::FormId itemId{ .mIndex = 0x120154, .mContentFile = 0 };
    const ESM::FormId actorBaseId{ .mIndex = 0x120155, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "PersistentRewardQuest");
    quest.mObjectives = {
        { .mIndex = 10, .mDescription = "Notes retained" },
        { .mIndex = 20, .mDescription = "Perk retained" },
    };
    ESM4::QuestStageEntry entry;
    entry.mScript.compiledData = { 0xef, 0xbe, 0x03, 0x00, 0xaa, 0xbb, 0xcc };
    entry.mScript.scriptSource
        = "AddNote FirstNote\n"
          "Player.AddNote SecondNote\n"
          "if GetHasNote FirstNote == 1 && Player.GetHasNote SecondNote == 1\n"
          "  SetObjectiveDisplayed PersistentRewardQuest 10 1\n"
          "endif\n"
          "Player.AddPerk QuestPerk\n"
          "if PlayerRef.HasPerk QuestPerk == 1\n"
          "  SetObjectiveDisplayed PersistentRewardQuest 20 1\n"
          "endif\n"
          "Player.RemovePerk QuestPerk\n"
          "Player.RemoveNote SecondNote\n"
          "SetQuestObject QuestBook 1\n"
          "SetQuestObject QuestBook 0\n"
          "AddAchievement 16\n"
          "RewardKarma KarmaReward\n"
          "Player.RewardKarma 25\n"
          "AddSpecialPoints 1\n"
          "Player.AddSpecialPoints 2\n"
          "SetEssential QuestActorBase 0";
    quest.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(entry) } });
    store.overrideRecord(quest);

    for (const auto& [id, editorId] : std::array{
             std::pair{ firstNoteId, std::string_view{ "FirstNote" } },
             std::pair{ secondNoteId, std::string_view{ "SecondNote" } },
         })
    {
        ESM4::Note note;
        note.mId = id;
        note.mEditorId = editorId;
        store.overrideRecord(note);
    }
    ESM4::Perk perk;
    perk.mId = perkId;
    perk.mEditorId = "QuestPerk";
    perk.mData.mRankCount = 1;
    store.overrideRecord(perk);
    ESM4::Book book;
    book.mId = itemId;
    book.mEditorId = "QuestBook";
    store.overrideRecord(book);
    ESM4::Npc actorBase;
    actorBase.mId = actorBaseId;
    actorBase.mEditorId = "QuestActorBase";
    store.overrideRecord(actorBase);
    store.overrideRecord(makeGlobal(
        { .mIndex = 0x120156, .mContentFile = 0 }, "KarmaReward", -50.f));

    MWWorld::Globals globals;
    globals.fill(store);
    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store, &globals);

    std::set<ESM::FormId> knownNotes;
    std::set<ESM::FormId> perks;
    std::vector<std::pair<ESM::FormId, bool>> questObjectChanges;
    std::vector<std::uint32_t> achievements;
    std::vector<int> karmaRewards;
    std::vector<int> specialPointRewards;
    std::vector<std::pair<ESM::FormId, bool>> essentialChanges;
    runtime.setNoteHandler([&](ESM::FormId note, bool known) {
        if (known)
            knownNotes.insert(note);
        else
            knownNotes.erase(note);
        return true;
    });
    runtime.setKnownNoteHandler([&](ESM::FormId note) -> std::optional<bool> {
        return knownNotes.contains(note);
    });
    runtime.setPlayerPerkHandler([&](ESM::FormId perkValue, bool add) {
        if (add)
            perks.insert(perkValue);
        else
            perks.erase(perkValue);
        return true;
    });
    runtime.setPlayerHasPerkHandler([&](ESM::FormId perkValue) -> std::optional<bool> {
        return perks.contains(perkValue);
    });
    runtime.setQuestObjectHandler([&](ESM::FormId item, bool questObject) {
        questObjectChanges.emplace_back(item, questObject);
        return true;
    });
    runtime.setAchievementHandler([&](std::uint32_t achievement) {
        achievements.push_back(achievement);
        return true;
    });
    runtime.setRewardKarmaHandler([&](int amount) {
        karmaRewards.push_back(amount);
        return true;
    });
    runtime.setAddSpecialPointsHandler([&](int amount) {
        specialPointRewards.push_back(amount);
        return true;
    });
    runtime.setSetEssentialHandler([&](ESM::FormId actorBaseValue, bool essential) {
        essentialChanges.emplace_back(actorBaseValue, essential);
        return true;
    });

    // Conditions are selected before their executable lines are dispatched, so seed the pre-stage
    // persistent state that these GetHasNote/HasPerk predicates observe.
    knownNotes.insert(firstNoteId);
    knownNotes.insert(secondNoteId);
    perks.insert(perkId);
    ASSERT_TRUE(runtime.setStage(questId, 5));
    const MWWorld::ESM4QuestState* const state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_NE(state->mObjectiveStatus.at(10) & MWWorld::ESM4QuestState::Objective_Displayed, 0);
    EXPECT_NE(state->mObjectiveStatus.at(20) & MWWorld::ESM4QuestState::Objective_Displayed, 0);
    EXPECT_TRUE(knownNotes.contains(firstNoteId));
    EXPECT_FALSE(knownNotes.contains(secondNoteId));
    EXPECT_FALSE(perks.contains(perkId));
    EXPECT_EQ(questObjectChanges,
        (std::vector<std::pair<ESM::FormId, bool>>{ { itemId, true }, { itemId, false } }));
    EXPECT_EQ(achievements, (std::vector<std::uint32_t>{ 16 }));
    EXPECT_EQ(karmaRewards, (std::vector<int>{ -50, 25 }));
    EXPECT_EQ(specialPointRewards, (std::vector<int>{ 1, 2 }));
    EXPECT_EQ(essentialChanges,
        (std::vector<std::pair<ESM::FormId, bool>>{ { actorBaseId, false } }));
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
}

TEST(ESM4QuestRuntimeTest, MalformedCompiledStageFailsClosedWithoutSourceFallback)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x120101, .mContentFile = 0 };
    ESM4::Quest quest = makeQuest(questId, "MalformedQuest");
    quest.mObjectives.push_back({ .mIndex = 3, .mDescription = "Must remain hidden" });
    ESM4::QuestStageEntry entry;
    entry.mScript.compiledData = { 0xa3, 0x11, 0x0f, 0x00, 0x03, 0x00 }; // framed payload overrun
    entry.mScript.scriptSource = "SetObjectiveDisplayed MalformedQuest 3 1";
    quest.mStages.push_back({ .mIndex = 5, .mEntries = { entry } });
    store.overrideRecord(quest);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    EXPECT_FALSE(runtime.setStage(questId, 5));
    const MWWorld::ESM4QuestState* state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mFlags, 0);
    EXPECT_EQ(state->mCurrentStage, 0);
    EXPECT_FALSE(state->mStageDone.at(5));
    EXPECT_EQ(state->mObjectiveStatus.at(3), 0);
    EXPECT_FALSE(runtime.getActiveQuest().has_value());
}

TEST(ESM4QuestRuntimeTest, LaterInvalidReferenceCannotPartiallyExecuteCompiledStage)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x120102, .mContentFile = 0 };
    ESM4::Quest quest = makeQuest(questId, "AtomicQuest");
    quest.mObjectives.push_back({ .mIndex = 3, .mDescription = "Must remain hidden" });
    ESM4::QuestStageEntry entry;
    entry.mScript.references = { questId };
    entry.mScript.compiledData = {
        0xa3, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01, 0x00, 0x6e, 0x03, 0x00, 0x00, 0x00, 0x6e, 0x01, 0x00, 0x00,
        0x00, // valid SetObjectiveDisplayed prefix
        0xdd, 0x11, 0x05, 0x00, 0x01, 0x00, 0x72, 0x02, 0x00 // invalid later SCRO index
    };
    quest.mStages.push_back({ .mIndex = 5, .mEntries = { entry } });
    store.overrideRecord(quest);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    EXPECT_FALSE(runtime.setStage(questId, 5));
    const MWWorld::ESM4QuestState* state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mFlags, 0);
    EXPECT_EQ(state->mCurrentStage, 0);
    EXPECT_FALSE(state->mStageDone.at(5));
    EXPECT_EQ(state->mObjectiveStatus.at(3), 0);
    EXPECT_FALSE(runtime.getActiveQuest().has_value());
}

TEST(ESM4QuestRuntimeTest, ImportsFalloutGlobalsAndCalendarAliases)
{
    MWWorld::ESMStore store;
    store.overrideRecord(makeGlobal({ .mIndex = 1, .mContentFile = 0 }, "TimeScale", 30.f));
    store.overrideRecord(makeGlobal({ .mIndex = 2, .mContentFile = 0 }, "GameHour", 12.f));
    store.overrideRecord(makeGlobal({ .mIndex = 3, .mContentFile = 0 }, "GameDaysPassed", 5.f));

    MWWorld::Globals globals;
    globals.fill(store);

    EXPECT_FLOAT_EQ(globals[MWWorld::Globals::sTimeScale].getFloat(), 30.f);
    EXPECT_FLOAT_EQ(globals[MWWorld::Globals::sGameHour].getFloat(), 12.f);
    EXPECT_FLOAT_EQ(globals[MWWorld::Globals::sDaysPassed].getFloat(), 5.f);
}

TEST(ESM4QuestRuntimeTest, ExecutesDialogueResultQuestCommands)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x104eae, .mContentFile = 0 };
    ESM4::Quest quest = makeQuest(questId, "GS001");
    const ESM::FormId scriptId{ .mIndex = 0x104eb0, .mContentFile = 0 };
    quest.mQuestScript = scriptId;
    quest.mObjectives.push_back(ESM4::QuestObjective{ .mIndex = 10, .mDescription = "Recruit Goodsprings" });
    quest.mStages.push_back(ESM4::QuestStage{ .mIndex = 20, .mEntries = { ESM4::QuestStageEntry{} } });
    store.overrideRecord(quest);
    ESM4::Script script;
    script.mId = scriptId;
    script.mEditorId = "VFreeformGoodspringsScript";
    script.mScript.localVarData = {
        ESM4::ScriptLocalVariableData{ .index = 1, .variableName = "bMetPete" },
        ESM4::ScriptLocalVariableData{ .index = 2, .variableName = "bEasyPeteNCR" },
    };
    store.overrideRecord(script);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    runtime.executeResultSource("StartQuest GS001\nSetObjectiveDisplayed GS001 10 1\n"
                                "SetObjectiveCompleted GS001 10 1\nSetStage GS001 20\n"
                                "ForceActiveQuest GS001\nset GS001.bMetPete to 1\nset GS001.bEasyPeteNCR to 1");

    const MWWorld::ESM4QuestState* state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mCurrentStage, 20);
    EXPECT_TRUE(state->mStageDone.at(20));
    EXPECT_EQ(state->mObjectiveStatus.at(10),
        MWWorld::ESM4QuestState::Objective_Displayed | MWWorld::ESM4QuestState::Objective_Completed);
    EXPECT_EQ(runtime.getActiveQuest(), questId);
    EXPECT_EQ(runtime.getQuestVariable("GS001", "bMetPete"), 1.f);
    EXPECT_EQ(runtime.getQuestVariable("gs001", "beasypetencr"), 1.f);
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());

    EXPECT_TRUE(runtime.evaluateConditions(
        { makeCondition(ESM4::FUN_GetObjectiveDisplayed, questId, 1.f, ESM4::CTF_EqualTo, 10),
            makeCondition(ESM4::FUN_GetObjectiveCompleted, questId, 1.f, ESM4::CTF_EqualTo, 10) }));
    EXPECT_TRUE(runtime.evaluateConditions(
        { makeCondition(ESM4::FUN_GetQuestVariable, questId, 1.f, ESM4::CTF_EqualTo, 1) }));

    runtime.executeResultSource("CompleteQuest GS001");
    EXPECT_NE(state->mFlags & MWWorld::ESM4QuestState::Flag_Completed, 0);
    EXPECT_EQ(state->mFlags & MWWorld::ESM4QuestState::Flag_Running, 0);
    runtime.executeResultSource("FailQuest GS001");
    EXPECT_NE(state->mFlags & MWWorld::ESM4QuestState::Flag_Failed, 0);
    EXPECT_EQ(state->mFlags & MWWorld::ESM4QuestState::Flag_Completed, 0);
    runtime.executeResultSource("StartQuest GS001\nStopQuest GS001");
    EXPECT_EQ(state->mFlags & MWWorld::ESM4QuestState::Flag_Running, 0);
}

TEST(ESM4QuestRuntimeTest, EvaluatesRetailQuestAndGlobalConditionGroups)
{
    MWWorld::ESMStore store;
    const ESM::FormId vcg00Id{ .mIndex = 0x10a212, .mContentFile = 0 };
    const ESM::FormId vcg02Id{ .mIndex = 0x10a214, .mContentFile = 0 };
    const ESM::FormId doneQuestId{ .mIndex = 0x120000, .mContentFile = 0 };
    const ESM::FormId targetQuestId{ .mIndex = 0x120001, .mContentFile = 0 };
    const ESM::FormId timeScaleId{ .mIndex = 0x38, .mContentFile = 0 };
    const ESM::FormId stageThresholdId{ .mIndex = 0x120002, .mContentFile = 0 };

    store.overrideRecord(makeQuest(vcg00Id, "VCG00"));

    ESM4::Quest vcg02 = makeQuest(vcg02Id, "VCG02");
    vcg02.mStages.push_back(ESM4::QuestStage{ .mIndex = 5, .mEntries = { ESM4::QuestStageEntry{} } });
    store.overrideRecord(vcg02);

    ESM4::Quest doneQuest = makeQuest(doneQuestId, "DoneQuest");
    ESM4::QuestStageEntry completeEntry;
    completeEntry.mFlags = ESM4::QuestStageEntry::Flag_CompleteQuest;
    doneQuest.mStages.push_back(ESM4::QuestStage{ .mIndex = 1, .mEntries = { completeEntry } });
    store.overrideRecord(doneQuest);

    ESM4::Quest targetQuest = makeQuest(targetQuestId, "ConditionTarget");
    targetQuest.mObjectives.push_back(ESM4::QuestObjective{ .mIndex = 7, .mDescription = "Condition passed" });
    ESM4::QuestStageEntry conditionedEntry;
    conditionedEntry.mConditions = {
        makeCondition(ESM4::FUN_GetQuestRunning, vcg00Id, 1.f, ESM4::CTF_EqualTo | ESM4::CTF_Combine),
        makeCondition(ESM4::FUN_GetQuestRunning, vcg02Id, 1.f),
        makeCondition(ESM4::FUN_GetStage, vcg02Id, std::bit_cast<float>(stageThresholdId.toUint32()),
            ESM4::CTF_GrThOrEqTo | ESM4::CTF_UseGlobal),
        makeCondition(ESM4::FUN_GetStageDone, vcg02Id, 1.f, ESM4::CTF_EqualTo, 5),
        makeCondition(ESM4::FUN_GetGlobalValue, timeScaleId, 12.f),
        makeCondition(ESM4::FUN_GetQuestCompleted, doneQuestId, 1.f),
    };
    conditionedEntry.mScript.scriptSource = "SetObjectiveDisplayed ConditionTarget 7 1";
    targetQuest.mStages.push_back(ESM4::QuestStage{ .mIndex = 1, .mEntries = { conditionedEntry } });

    ESM4::QuestStageEntry unsupportedEntry;
    unsupportedEntry.mConditions = { makeCondition(9999, vcg02Id, 1.f) };
    unsupportedEntry.mScript.scriptSource = "SetObjectiveDisplayed ConditionTarget 7 0";
    targetQuest.mStages.push_back(ESM4::QuestStage{ .mIndex = 2, .mEntries = { unsupportedEntry } });
    store.overrideRecord(targetQuest);

    store.overrideRecord(makeGlobal(timeScaleId, "TimeScale", 12.f));
    store.overrideRecord(makeGlobal(stageThresholdId, "StageThreshold", 5.f));
    MWWorld::Globals globals;
    globals.fill(store);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store, &globals);
    ASSERT_TRUE(runtime.setStage("VCG02", 5));
    ASSERT_TRUE(runtime.setStage("DoneQuest", 1));
    ASSERT_TRUE(runtime.setStage("ConditionTarget", 1));

    const MWWorld::ESM4QuestState* state = runtime.search(targetQuestId);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mFlags, 0x21);
    EXPECT_EQ(state->mObjectiveStatus.at(7), MWWorld::ESM4QuestState::Objective_Displayed);
    EXPECT_TRUE(runtime.getUnsupportedConditionFunctions().empty());

    ASSERT_TRUE(runtime.setStage("ConditionTarget", 2));
    EXPECT_EQ(state->mObjectiveStatus.at(7), MWWorld::ESM4QuestState::Objective_Displayed);
    EXPECT_EQ(runtime.getUnsupportedConditionFunctions(), std::vector<std::uint32_t>{ 9999 });
}

TEST(ESM4QuestRuntimeTest, RoundTripsQuestStateAcrossChangedLoadOrder)
{
    const ESM::FormId originalId{ .mIndex = 0x10a214, .mContentFile = 2 };
    const ESM::FormId originalScriptId{ .mIndex = 0x10a216, .mContentFile = 2 };
    ESM4::Quest originalQuest = makeQuest(originalId, "VCG02");
    originalQuest.mQuestScript = originalScriptId;
    originalQuest.mObjectives.push_back(ESM4::QuestObjective{ .mIndex = 3, .mDescription = "Choose your skills" });
    ESM4::QuestStageEntry entry;
    entry.mScript.scriptSource = "SetObjectiveDisplayed VCG02 3 1;\nForceActiveQuest VCG02";
    originalQuest.mStages.push_back(ESM4::QuestStage{ .mIndex = 5, .mEntries = { entry } });

    MWWorld::ESMStore originalStore;
    originalStore.overrideRecord(originalQuest);
    ESM4::Script originalScript;
    originalScript.mId = originalScriptId;
    originalScript.mScript.localVarData
        = { ESM4::ScriptLocalVariableData{ .index = 1, .variableName = "bDialogueComplete" } };
    originalStore.overrideRecord(originalScript);
    originalStore.overrideRecord(makeQuest({ .mIndex = 0x10a212, .mContentFile = 2 }, "VCG00"));
    MWWorld::ESM4QuestRuntime originalRuntime;
    originalRuntime.initialize(originalStore);
    ASSERT_TRUE(originalRuntime.setStage("VCG02", 5));
    ASSERT_TRUE(originalRuntime.setQuestVariable("VCG02", "bDialogueComplete", 1.f));
    ASSERT_EQ(originalRuntime.countSavedGameRecords(), 1);

    auto stream = std::make_unique<std::stringstream>();
    {
        ESM::ESMWriter writer;
        writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
        writer.save(*stream);
        originalRuntime.write(writer);
    }

    ESM::ESMReader reader;
    reader.open(std::move(stream), "fallout-quest-save");
    const std::map<int, int> contentMapping{ { 2, 7 } };
    reader.setContentFileMapping(&contentMapping);
    ASSERT_TRUE(reader.hasMoreRecs());
    ASSERT_EQ(reader.getRecName().toInt(), ESM::REC_FQST);
    reader.getRecHeader();

    const ESM::FormId remappedId{ .mIndex = originalId.mIndex, .mContentFile = 7 };
    ESM4::Quest remappedQuest = originalQuest;
    remappedQuest.mId = remappedId;
    remappedQuest.mQuestScript.mContentFile = 7;
    MWWorld::ESMStore remappedStore;
    remappedStore.overrideRecord(remappedQuest);
    ESM4::Script remappedScript = originalScript;
    remappedScript.mId.mContentFile = 7;
    remappedStore.overrideRecord(remappedScript);
    remappedStore.overrideRecord(makeQuest({ .mIndex = 0x10a212, .mContentFile = 7 }, "VCG00"));
    MWWorld::ESM4QuestRuntime restoredRuntime;
    restoredRuntime.initialize(remappedStore);
    restoredRuntime.readRecord(reader);

    const MWWorld::ESM4QuestState* restored = restoredRuntime.search(remappedId);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->mFlags, 0x21);
    EXPECT_EQ(restored->mCurrentStage, 5);
    EXPECT_TRUE(restored->mStageDone.at(5));
    EXPECT_EQ(restored->mObjectiveStatus.at(3), MWWorld::ESM4QuestState::Objective_Displayed);
    EXPECT_EQ(restoredRuntime.getActiveQuest(), remappedId);
    EXPECT_EQ(restoredRuntime.getQuestVariable("VCG02", "bDialogueComplete"), 1.f);

    const MWWorld::ESM4QuestState* unchanged = restoredRuntime.search("VCG00");
    ASSERT_NE(unchanged, nullptr);
    EXPECT_EQ(unchanged->mFlags, 0);
    EXPECT_EQ(unchanged->mCurrentStage, 0);
}
