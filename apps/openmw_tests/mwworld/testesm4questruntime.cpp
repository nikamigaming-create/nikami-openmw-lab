#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string_view>
#include <variant>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadscpt.hpp>

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
