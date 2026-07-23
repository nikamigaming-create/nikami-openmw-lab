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
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loaddial.hpp>
#include <components/esm4/loadfact.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/loadmesg.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadrefr.hpp>
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

TEST(ESM4QuestRuntimeTest, ExecutesExactGoodspringsShootingGalleryFactionSetup)
{
    MWWorld::ESMStore store;

    const ESM::FormId vcg02Id{ .mIndex = 0x10a214, .mContentFile = 0 };
    const ESM::FormId vcg02ScriptId{ .mIndex = 0x10a1f0, .mContentFile = 0 };
    const ESM::FormId sunnyId{ .mIndex = 0x104e85, .mContentFile = 0 };
    const ESM::FormId cheyenneId{ .mIndex = 0x10588e, .mContentFile = 0 };
    const ESM::FormId tutorialQuestId{ .mIndex = 0x059c85, .mContentFile = 0 };
    const ESM::FormId tutorialScriptId{ .mIndex = 0x104c01, .mContentFile = 0 };
    const ESM::FormId sunnyFactionId{ .mIndex = 0x1691ea, .mContentFile = 0 };
    const ESM::FormId playerFactionId{ .mIndex = 0x01b2a4, .mContentFile = 0 };
    const ESM::FormId victorFactionId{ .mIndex = 0x11cd0d, .mContentFile = 0 };
    const ESM::FormId goodspringsFactionId{ .mIndex = 0x104c6e, .mContentFile = 0 };

    ESM4::Quest vcg02 = makeQuest(vcg02Id, "VCG02");
    vcg02.mQuestScript = vcg02ScriptId;
    ESM4::QuestStageEntry entry;
    // FalloutNV.esm 0010A214 VCG02 stage 10 entry 0, byte-for-byte. Its
    // three 0x1079 frames are the authored SetAlly calls that prevent the
    // shooting tutorial from turning Sunny, Victor, or Goodsprings hostile.
    const std::array<std::uint8_t, 89> retailScda{
        0x1c, 0x00, 0x01, 0x00, 0x5e, 0x10, 0x00, 0x00, 0xdd, 0x11, 0x05, 0x00, 0x01, 0x00, 0x72,
        0x03, 0x00, 0x39, 0x10, 0x0a, 0x00, 0x02, 0x00, 0x72, 0x04, 0x00, 0x6e, 0x36, 0x00, 0x00,
        0x00, 0x79, 0x10, 0x08, 0x00, 0x02, 0x00, 0x72, 0x05, 0x00, 0x72, 0x06, 0x00, 0x79, 0x10,
        0x08, 0x00, 0x02, 0x00, 0x72, 0x05, 0x00, 0x72, 0x07, 0x00, 0x79, 0x10, 0x08, 0x00, 0x02,
        0x00, 0x72, 0x08, 0x00, 0x72, 0x07, 0x00, 0x15, 0x00, 0x0a, 0x00, 0x72, 0x03, 0x00, 0x73,
        0x04, 0x00, 0x02, 0x00, 0x20, 0x31, 0x1c, 0x00, 0x02, 0x00, 0x5e, 0x10, 0x00, 0x00
    };
    entry.mScript.compiledData.assign(retailScda.begin(), retailScda.end());
    entry.mScript.scriptSource = "; Sunny goes outside\r\n"
                                 "SunnyREF.evp\r\n"
                                 "ForceActiveQuest VCG02\r\n"
                                 "SetStage CGTutorial 54\r\n"
                                 "SetAlly SunnySmilesFaction PlayerFaction\r\n"
                                 "SetAlly SunnySmilesFaction VVictorFaction\r\n"
                                 "SetAlly GoodspringsFaction VVictorFaction\r\n"
                                 "set VCG02.bShootingTutorialActive to 1\r\n"
                                 "CheyenneRef.evp";
    entry.mScript.references = { sunnyId, cheyenneId, vcg02Id, tutorialQuestId, sunnyFactionId, playerFactionId,
        victorFactionId, goodspringsFactionId };
    vcg02.mStages.push_back({ .mIndex = 10, .mEntries = { std::move(entry) } });
    store.overrideRecord(vcg02);

    ESM4::Script vcg02Script;
    vcg02Script.mId = vcg02ScriptId;
    vcg02Script.mEditorId = "VCG02SCRIPT";
    vcg02Script.mScript.localVarData
        = { ESM4::ScriptLocalVariableData{ .index = 4, .type = 1, .variableName = "bShootingTutorialActive" } };
    store.overrideRecord(vcg02Script);

    ESM4::Quest tutorial = makeQuest(tutorialQuestId, "CGTutorial");
    tutorial.mQuestScript = tutorialScriptId;
    ESM4::QuestStageEntry tutorialEntry;
    // FalloutNV.esm 00059C85 CGTutorial stage 54 entry 0, byte-for-byte:
    // run the quest-target tutorial timer for five seconds.
    const std::array<std::uint8_t, 28> retailTutorialScda{
        0x15, 0x00, 0x0a, 0x00, 0x72, 0x01, 0x00, 0x73, 0x01, 0x00, 0x02, 0x00, 0x20, 0x31, 0x15, 0x00,
        0x0a, 0x00, 0x72, 0x01, 0x00, 0x66, 0x02, 0x00, 0x02, 0x00, 0x20, 0x35
    };
    tutorialEntry.mScript.compiledData.assign(retailTutorialScda.begin(), retailTutorialScda.end());
    tutorialEntry.mScript.references = { tutorialQuestId };
    tutorial.mStages.push_back({ .mIndex = 54, .mEntries = { std::move(tutorialEntry) } });
    store.overrideRecord(tutorial);

    ESM4::Script tutorialScript;
    tutorialScript.mId = tutorialScriptId;
    tutorialScript.mEditorId = "CGTutorialSCRIPT";
    tutorialScript.mScript.localVarData = {
        ESM4::ScriptLocalVariableData{ .index = 1, .type = 1, .variableName = "runTimer" },
        ESM4::ScriptLocalVariableData{ .index = 2, .type = 0, .variableName = "timer" },
    };
    store.overrideRecord(tutorialScript);

    ESM4::ActorCharacter sunny;
    sunny.mId = sunnyId;
    sunny.mEditorId = "SunnyREF";
    store.overrideRecord(sunny);
    ESM4::ActorCreature cheyenne;
    cheyenne.mId = cheyenneId;
    cheyenne.mEditorId = "CheyenneREF";
    store.overrideRecord(cheyenne);
    for (const auto& [id, editorId] : std::array{
             std::pair{ sunnyFactionId, "SunnySmilesFaction" },
             std::pair{ playerFactionId, "PlayerFaction" },
             std::pair{ victorFactionId, "VVictorFaction" },
             std::pair{ goodspringsFactionId, "GoodspringsFaction" },
         })
    {
        ESM4::Faction faction;
        faction.mId = id;
        faction.mEditorId = editorId;
        store.overrideRecord(faction);
    }

    std::vector<ESM::FormId> evaluatedPackages;
    std::vector<std::pair<ESM::FormId, ESM::FormId>> allies;
    MWWorld::ESM4QuestRuntime runtime;
    runtime.setReferenceCommandHandler(
        [&evaluatedPackages](MWWorld::ESM4QuestReferenceCommand command, ESM::FormId target) {
            if (command != MWWorld::ESM4QuestReferenceCommand::EvaluatePackage)
                return false;
            evaluatedPackages.push_back(target);
            return true;
        });
    runtime.setSetAllyHandler([&allies](ESM::FormId first, ESM::FormId second) {
        allies.emplace_back(first, second);
        return true;
    });
    runtime.initialize(store);

    ASSERT_TRUE(runtime.setStage(vcg02Id, 10));
    const MWWorld::ESM4QuestState* vcg02State = runtime.search(vcg02Id);
    const MWWorld::ESM4QuestState* tutorialState = runtime.search(tutorialQuestId);
    ASSERT_NE(vcg02State, nullptr);
    ASSERT_NE(tutorialState, nullptr);
    EXPECT_EQ(vcg02State->mCurrentStage, 10);
    EXPECT_EQ(runtime.getActiveQuest(), vcg02Id);
    EXPECT_EQ(runtime.getQuestVariable("VCG02", "bShootingTutorialActive"), 1.f);
    EXPECT_EQ(tutorialState->mCurrentStage, 54);
    EXPECT_EQ(runtime.getQuestVariable("CGTutorial", "runTimer"), 1.f);
    EXPECT_EQ(runtime.getQuestVariable("CGTutorial", "timer"), 5.f);
    EXPECT_EQ(evaluatedPackages, (std::vector<ESM::FormId>{ sunnyId, cheyenneId }));
    EXPECT_EQ(allies, (std::vector<std::pair<ESM::FormId, ESM::FormId>>{
                           { sunnyFactionId, playerFactionId },
                           { sunnyFactionId, victorFactionId },
                           { goodspringsFactionId, victorFactionId },
                       }));
    EXPECT_EQ(vcg02State->mAllies.size(), 3);
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
    EXPECT_TRUE(runtime.getUnsupportedCompiledOpcodes().empty());
}

TEST(ESM4QuestRuntimeTest, MissingSetAllyFactionFailsBeforeQuestMutation)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x100, .mContentFile = 0 };
    const ESM::FormId existingFactionId{ .mIndex = 0x200, .mContentFile = 0 };
    const ESM::FormId missingFactionId{ .mIndex = 0x300, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "SetAllyFailure");
    ESM4::QuestStageEntry entry;
    entry.mScript.compiledData
        = { 0x79, 0x10, 0x08, 0x00, 0x02, 0x00, 0x72, 0x01, 0x00, 0x72, 0x02, 0x00 };
    entry.mScript.references = { existingFactionId, missingFactionId };
    quest.mStages.push_back({ .mIndex = 10, .mEntries = { std::move(entry) } });
    store.overrideRecord(quest);
    ESM4::Faction existingFaction;
    existingFaction.mId = existingFactionId;
    store.overrideRecord(existingFaction);

    int calls = 0;
    MWWorld::ESM4QuestRuntime runtime;
    runtime.setSetAllyHandler([&calls](ESM::FormId, ESM::FormId) {
        ++calls;
        return true;
    });
    runtime.initialize(store);

    EXPECT_FALSE(runtime.setStage(questId, 10));
    const MWWorld::ESM4QuestState* state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mCurrentStage, 0);
    EXPECT_FALSE(state->mStageDone.at(10));
    EXPECT_EQ(calls, 0);
}

TEST(ESM4QuestRuntimeTest, ExecutesRetailVms16SetEnemyFramesWithDirectionalFlags)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x104eae, .mContentFile = 0 };
    const ESM::FormId goodspringsId{ .mIndex = 0x104c6e, .mContentFile = 0 };
    const ESM::FormId playerId{ .mIndex = 0x01b2a4, .mContentFile = 0 };
    const ESM::FormId ringoAlliesId{ .mIndex = 0x15f8db, .mContentFile = 0 };
    const ESM::FormId powderGangId{ .mIndex = 0x104c7e, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "VMS16");
    ESM4::QuestStageEntry allyEntry;
    allyEntry.mScript.compiledData
        = { 0x79, 0x10, 0x08, 0x00, 0x02, 0x00, 0x72, 0x01, 0x00, 0x72, 0x02, 0x00 };
    allyEntry.mScript.references = { goodspringsId, playerId };
    quest.mStages.push_back({ .mIndex = 60, .mEntries = { std::move(allyEntry) } });
    ESM4::QuestStageEntry fightEntry;
    // The two exact SetEnemy frames from retail VMS16 stage 70. Omitted flags default both directions to Enemy.
    fightEntry.mScript.compiledData = {
        0x78, 0x10, 0x08, 0x00, 0x02, 0x00, 0x72, 0x0c, 0x00, 0x72, 0x0b, 0x00,
        0x78, 0x10, 0x08, 0x00, 0x02, 0x00, 0x72, 0x0c, 0x00, 0x72, 0x0a, 0x00,
    };
    fightEntry.mScript.references.resize(12);
    fightEntry.mScript.references[9] = playerId;
    fightEntry.mScript.references[10] = ringoAlliesId;
    fightEntry.mScript.references[11] = powderGangId;
    quest.mStages.push_back({ .mIndex = 70, .mEntries = { std::move(fightEntry) } });

    ESM4::QuestStageEntry outcomeEntry;
    // The two exact SetEnemy frames from retail VMS16 stage 100. Its authored 1,1 flags make both directions Neutral.
    outcomeEntry.mScript.compiledData = {
        0x78, 0x10, 0x12, 0x00, 0x04, 0x00, 0x72, 0x08, 0x00, 0x72, 0x09, 0x00, 0x6e, 0x01, 0x00, 0x00,
        0x00, 0x6e, 0x01, 0x00, 0x00, 0x00,
        0x78, 0x10, 0x12, 0x00, 0x04, 0x00, 0x72, 0x0a, 0x00, 0x72, 0x09, 0x00, 0x6e, 0x01, 0x00, 0x00,
        0x00, 0x6e, 0x01, 0x00, 0x00, 0x00,
    };
    outcomeEntry.mScript.references.resize(10);
    outcomeEntry.mScript.references[7] = goodspringsId;
    outcomeEntry.mScript.references[8] = playerId;
    outcomeEntry.mScript.references[9] = ringoAlliesId;
    quest.mStages.push_back({ .mIndex = 100, .mEntries = { std::move(outcomeEntry) } });
    store.overrideRecord(quest);

    for (const auto& [id, editorId] : std::array{
             std::pair{ goodspringsId, "GoodspringsFaction" },
             std::pair{ playerId, "PlayerFaction" },
             std::pair{ ringoAlliesId, "GoodspringsRingoAlliesFaction" },
             std::pair{ powderGangId, "GoodspringsPowderGangFaction" },
         })
    {
        ESM4::Faction faction;
        faction.mId = id;
        faction.mEditorId = editorId;
        store.overrideRecord(faction);
    }

    std::vector<MWWorld::ESM4QuestState::EnemyRelation> calls;
    MWWorld::ESM4QuestRuntime runtime;
    runtime.setSetAllyHandler([](ESM::FormId, ESM::FormId) { return true; });
    runtime.setSetEnemyHandler(
        [&calls](ESM::FormId first, ESM::FormId second, bool firstNeutral, bool secondNeutral) {
            calls.push_back({ first, second, firstNeutral, secondNeutral });
            return true;
        });
    runtime.initialize(store);

    ASSERT_TRUE(runtime.setStage(questId, 60));
    ASSERT_TRUE(runtime.setStage(questId, 70));
    ASSERT_TRUE(runtime.setStage(questId, 100));
    EXPECT_EQ(calls,
        (std::vector<MWWorld::ESM4QuestState::EnemyRelation>{
            { powderGangId, ringoAlliesId, false, false },
            { powderGangId, playerId, false, false },
            { goodspringsId, playerId, true, true },
            { ringoAlliesId, playerId, true, true },
        }));
    const MWWorld::ESM4QuestState* state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mEnemies.size(), 4);
    EXPECT_TRUE(state->mAllies.empty());
    EXPECT_TRUE(runtime.getUnsupportedCompiledOpcodes().empty());
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
}

TEST(ESM4QuestRuntimeTest, ExecutesVms16FactionSetupFromWholeSourceFallback)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x104eae, .mContentFile = 0 };
    const ESM::FormId goodspringsId{ .mIndex = 0x104c6e, .mContentFile = 0 };
    const ESM::FormId playerId{ .mIndex = 0x01b2a4, .mContentFile = 0 };
    const ESM::FormId ringoAlliesId{ .mIndex = 0x15f8db, .mContentFile = 0 };
    const ESM::FormId powderGangId{ .mIndex = 0x104c7e, .mContentFile = 0 };

    ESM4::Quest quest = makeQuest(questId, "VMS16");
    ESM4::QuestStageEntry entry;
    // Exact retail VMS16 stage 70 SetUnconscious frame keeps this entry on the whole-SCTX path while its
    // independently supported faction commands execute from the authored source.
    entry.mScript.compiledData
        = { 0x1c, 0x00, 0x01, 0x00, 0xf1, 0x10, 0x07, 0x00, 0x01, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00 };
    entry.mScript.references = { ESM::FormId{ .mIndex = 0x1073e8, .mContentFile = 0 } };
    entry.mScript.scriptSource = "SetAlly GoodspringsFaction PlayerFaction\r\n"
                                 "SetAlly GoodspringsRingoAlliesFaction PlayerFaction\r\n"
                                 "SetEnemy GoodspringsPowderGangFaction GoodspringsRingoAlliesFaction\r\n"
                                 "SetEnemy GoodspringsPowderGangFaction PlayerFaction";
    quest.mStages.push_back({ .mIndex = 70, .mEntries = { std::move(entry) } });
    store.overrideRecord(quest);
    for (const auto& [id, editorId] : std::array{
             std::pair{ goodspringsId, "GoodspringsFaction" },
             std::pair{ playerId, "PlayerFaction" },
             std::pair{ ringoAlliesId, "GoodspringsRingoAlliesFaction" },
             std::pair{ powderGangId, "GoodspringsPowderGangFaction" },
         })
    {
        ESM4::Faction faction;
        faction.mId = id;
        faction.mEditorId = editorId;
        store.overrideRecord(faction);
    }

    std::vector<std::pair<ESM::FormId, ESM::FormId>> allies;
    std::vector<MWWorld::ESM4QuestState::EnemyRelation> enemies;
    MWWorld::ESM4QuestRuntime runtime;
    runtime.setSetAllyHandler([&allies](ESM::FormId first, ESM::FormId second) {
        allies.emplace_back(first, second);
        return true;
    });
    runtime.setSetEnemyHandler(
        [&enemies](ESM::FormId first, ESM::FormId second, bool firstNeutral, bool secondNeutral) {
            enemies.push_back({ first, second, firstNeutral, secondNeutral });
            return true;
        });
    runtime.initialize(store);

    ASSERT_TRUE(runtime.setStage(questId, 70));
    EXPECT_EQ(allies,
        (std::vector<std::pair<ESM::FormId, ESM::FormId>>{
            { goodspringsId, playerId }, { ringoAlliesId, playerId } }));
    EXPECT_EQ(enemies,
        (std::vector<MWWorld::ESM4QuestState::EnemyRelation>{
            { powderGangId, ringoAlliesId, false, false },
            { powderGangId, playerId, false, false },
        }));
    const MWWorld::ESM4QuestState* state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mAllies.size(), 2);
    EXPECT_EQ(state->mEnemies.size(), 2);
    EXPECT_EQ(runtime.getUnsupportedCompiledOpcodes(), (std::vector<std::uint16_t>{ 0x10f1 }));
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
}

TEST(ESM4QuestRuntimeTest, RoundTripsSetAllyAcrossChangedLoadOrder)
{
    const ESM::FormId originalQuestId{ .mIndex = 0x100, .mContentFile = 2 };
    const ESM::FormId originalFirstId{ .mIndex = 0x200, .mContentFile = 2 };
    const ESM::FormId originalSecondId{ .mIndex = 0x300, .mContentFile = 2 };
    ESM4::Quest originalQuest = makeQuest(originalQuestId, "SetAllySave");
    ESM4::QuestStageEntry entry;
    entry.mScript.compiledData
        = { 0x79, 0x10, 0x08, 0x00, 0x02, 0x00, 0x72, 0x01, 0x00, 0x72, 0x02, 0x00 };
    entry.mScript.references = { originalFirstId, originalSecondId };
    originalQuest.mStages.push_back({ .mIndex = 10, .mEntries = { std::move(entry) } });

    MWWorld::ESMStore originalStore;
    originalStore.overrideRecord(originalQuest);
    ESM4::Faction originalFirst;
    originalFirst.mId = originalFirstId;
    originalStore.overrideRecord(originalFirst);
    ESM4::Faction originalSecond;
    originalSecond.mId = originalSecondId;
    originalStore.overrideRecord(originalSecond);
    MWWorld::ESM4QuestRuntime originalRuntime;
    originalRuntime.setSetAllyHandler([](ESM::FormId, ESM::FormId) { return true; });
    originalRuntime.initialize(originalStore);
    ASSERT_TRUE(originalRuntime.setStage(originalQuestId, 10));

    auto stream = std::make_unique<std::stringstream>();
    {
        ESM::ESMWriter writer;
        writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
        writer.save(*stream);
        originalRuntime.write(writer);
    }

    ESM::ESMReader reader;
    reader.open(std::move(stream), "fallout-setally-save");
    const std::map<int, int> contentMapping{ { 2, 7 } };
    reader.setContentFileMapping(&contentMapping);
    ASSERT_TRUE(reader.hasMoreRecs());
    ASSERT_EQ(reader.getRecName().toInt(), ESM::REC_FQST);
    reader.getRecHeader();

    const ESM::FormId remappedQuestId{ .mIndex = 0x100, .mContentFile = 7 };
    const ESM::FormId remappedFirstId{ .mIndex = 0x200, .mContentFile = 7 };
    const ESM::FormId remappedSecondId{ .mIndex = 0x300, .mContentFile = 7 };
    ESM4::Quest remappedQuest = originalQuest;
    remappedQuest.mId = remappedQuestId;
    remappedQuest.mStages[0].mEntries[0].mScript.references = { remappedFirstId, remappedSecondId };
    MWWorld::ESMStore remappedStore;
    remappedStore.overrideRecord(remappedQuest);
    ESM4::Faction remappedFirst;
    remappedFirst.mId = remappedFirstId;
    remappedStore.overrideRecord(remappedFirst);
    ESM4::Faction remappedSecond;
    remappedSecond.mId = remappedSecondId;
    remappedStore.overrideRecord(remappedSecond);

    std::vector<std::pair<ESM::FormId, ESM::FormId>> restoredAllies;
    MWWorld::ESM4QuestRuntime restoredRuntime;
    restoredRuntime.setSetAllyHandler([&restoredAllies](ESM::FormId first, ESM::FormId second) {
        restoredAllies.emplace_back(first, second);
        return true;
    });
    restoredRuntime.initialize(remappedStore);
    restoredRuntime.readRecord(reader);

    const MWWorld::ESM4QuestState* restored = restoredRuntime.search(remappedQuestId);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->mCurrentStage, 10);
    EXPECT_TRUE(restored->mStageDone.at(10));
    EXPECT_EQ(restored->mAllies,
        (std::vector<std::pair<ESM::FormId, ESM::FormId>>{ { remappedFirstId, remappedSecondId } }));
    EXPECT_EQ(restoredAllies,
        (std::vector<std::pair<ESM::FormId, ESM::FormId>>{ { remappedFirstId, remappedSecondId } }));
}

TEST(ESM4QuestRuntimeTest, RoundTripsSetEnemyAcrossChangedLoadOrder)
{
    const ESM::FormId originalQuestId{ .mIndex = 0x100, .mContentFile = 2 };
    const ESM::FormId originalFirstId{ .mIndex = 0x200, .mContentFile = 2 };
    const ESM::FormId originalSecondId{ .mIndex = 0x300, .mContentFile = 2 };
    ESM4::Quest originalQuest = makeQuest(originalQuestId, "SetEnemySave");
    ESM4::QuestStageEntry entry;
    entry.mScript.compiledData = { 0x78, 0x10, 0x12, 0x00, 0x04, 0x00, 0x72, 0x01, 0x00, 0x72, 0x02,
        0x00, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00 };
    entry.mScript.references = { originalFirstId, originalSecondId };
    originalQuest.mStages.push_back({ .mIndex = 70, .mEntries = { std::move(entry) } });

    MWWorld::ESMStore originalStore;
    originalStore.overrideRecord(originalQuest);
    ESM4::Faction originalFirst;
    originalFirst.mId = originalFirstId;
    originalStore.overrideRecord(originalFirst);
    ESM4::Faction originalSecond;
    originalSecond.mId = originalSecondId;
    originalStore.overrideRecord(originalSecond);
    MWWorld::ESM4QuestRuntime originalRuntime;
    originalRuntime.setSetEnemyHandler([](ESM::FormId, ESM::FormId, bool, bool) { return true; });
    originalRuntime.initialize(originalStore);
    ASSERT_TRUE(originalRuntime.setStage(originalQuestId, 70));

    auto stream = std::make_unique<std::stringstream>();
    {
        ESM::ESMWriter writer;
        writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
        writer.save(*stream);
        originalRuntime.write(writer);
    }

    ESM::ESMReader reader;
    reader.open(std::move(stream), "fallout-setenemy-save");
    const std::map<int, int> contentMapping{ { 2, 7 } };
    reader.setContentFileMapping(&contentMapping);
    ASSERT_TRUE(reader.hasMoreRecs());
    ASSERT_EQ(reader.getRecName().toInt(), ESM::REC_FQST);
    reader.getRecHeader();

    const ESM::FormId remappedQuestId{ .mIndex = 0x100, .mContentFile = 7 };
    const ESM::FormId remappedFirstId{ .mIndex = 0x200, .mContentFile = 7 };
    const ESM::FormId remappedSecondId{ .mIndex = 0x300, .mContentFile = 7 };
    ESM4::Quest remappedQuest = originalQuest;
    remappedQuest.mId = remappedQuestId;
    remappedQuest.mStages[0].mEntries[0].mScript.references = { remappedFirstId, remappedSecondId };
    MWWorld::ESMStore remappedStore;
    remappedStore.overrideRecord(remappedQuest);
    ESM4::Faction remappedFirst;
    remappedFirst.mId = remappedFirstId;
    remappedStore.overrideRecord(remappedFirst);
    ESM4::Faction remappedSecond;
    remappedSecond.mId = remappedSecondId;
    remappedStore.overrideRecord(remappedSecond);

    std::vector<MWWorld::ESM4QuestState::EnemyRelation> restoredEnemies;
    MWWorld::ESM4QuestRuntime restoredRuntime;
    restoredRuntime.setSetEnemyHandler(
        [&restoredEnemies](ESM::FormId first, ESM::FormId second, bool firstNeutral, bool secondNeutral) {
            restoredEnemies.push_back({ first, second, firstNeutral, secondNeutral });
            return true;
        });
    restoredRuntime.initialize(remappedStore);
    restoredRuntime.readRecord(reader);

    const MWWorld::ESM4QuestState* restored = restoredRuntime.search(remappedQuestId);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->mCurrentStage, 70);
    EXPECT_TRUE(restored->mStageDone.at(70));
    EXPECT_EQ(restored->mEnemies,
        (std::vector<MWWorld::ESM4QuestState::EnemyRelation>{
            { remappedFirstId, remappedSecondId, false, true } }));
    EXPECT_EQ(restoredEnemies, restored->mEnemies);
}

TEST(ESM4QuestRuntimeTest, ReadsLegacyQuestStateWithoutAllianceExtension)
{
    const ESM::FormId questId{ .mIndex = 0x100, .mContentFile = 2 };
    const ESM::FormId scriptId{ .mIndex = 0x101, .mContentFile = 2 };
    auto stream = std::make_unique<std::stringstream>();
    {
        ESM::ESMWriter writer;
        writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
        writer.save(*stream);
        writer.startRecord(ESM::REC_FQST);
        writer.writeFormId(questId, true, "FORM");
        writer.writeHNT("FLAG", static_cast<std::uint8_t>(1));
        writer.writeHNT("STAG", static_cast<std::uint8_t>(0));
        writer.writeHNT("ACTV", static_cast<std::uint8_t>(0));
        writer.writeHNT("DNCT", std::uint32_t{ 0 });
        writer.writeHNT("OBCT", std::uint32_t{ 0 });
        writer.writeHNString("VNAM", "legacyVariable");
        writer.writeHNT("VVAL", 12.5f);
        writer.endRecord(ESM::REC_FQST);
    }

    ESM::ESMReader reader;
    reader.open(std::move(stream), "legacy-fallout-quest-save");
    const std::map<int, int> contentMapping{ { 2, 2 } };
    reader.setContentFileMapping(&contentMapping);
    ASSERT_TRUE(reader.hasMoreRecs());
    ASSERT_EQ(reader.getRecName().toInt(), ESM::REC_FQST);
    reader.getRecHeader();

    ESM4::Quest quest = makeQuest(questId, "LegacyQuestState");
    quest.mQuestScript = scriptId;
    ESM4::Script script;
    script.mId = scriptId;
    script.mScript.localVarData
        = { ESM4::ScriptLocalVariableData{ .index = 1, .variableName = "legacyVariable" } };
    MWWorld::ESMStore store;
    store.overrideRecord(quest);
    store.overrideRecord(script);
    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    runtime.readRecord(reader);

    EXPECT_EQ(runtime.getQuestVariable("LegacyQuestState", "legacyVariable"), 12.5f);
    const MWWorld::ESM4QuestState* state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->mAllies.empty());
}

TEST(ESM4QuestRuntimeTest, ExecutesExactGoodspringsGeckoTutorialReferenceEffects)
{
    MWWorld::ESMStore store;

    const ESM::FormId vcg02Id{ .mIndex = 0x10a214, .mContentFile = 0 };
    const ESM::FormId cheyenneId{ .mIndex = 0x10588e, .mContentFile = 0 };
    const ESM::FormId sunnyId{ .mIndex = 0x104e85, .mContentFile = 0 };
    const ESM::FormId gecko1Id{ .mIndex = 0x10a1fe, .mContentFile = 0 };
    const ESM::FormId gecko2Id{ .mIndex = 0x10a1fd, .mContentFile = 0 };
    const ESM::FormId questScriptId{ .mIndex = 0x10a1f0, .mContentFile = 0 };

    ESM4::Quest vcg02 = makeQuest(vcg02Id, "VCG02");
    vcg02.mQuestScript = questScriptId;
    ESM4::QuestStageEntry entry;
    // FalloutNV.esm 0010A214 VCG02 stage 25 entry 0, byte-for-byte. The five SCROs are
    // VCG02, CheyenneREF, SunnyREF, VCG02Gecko1REF, and VCG02Gecko2REF.
    const std::array<std::uint8_t, 50> retailScda{
        0x15, 0x00, 0x0a, 0x00, 0x72, 0x05, 0x00, 0x73, 0x04, 0x00, 0x02, 0x00, 0x20, 0x30, 0x1c, 0x00, 0x01,
        0x00, 0x5e, 0x10, 0x00, 0x00, 0x1c, 0x00, 0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x1c, 0x00, 0x03, 0x00,
        0x21, 0x10, 0x02, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x04, 0x00, 0x21, 0x10, 0x02, 0x00, 0x00, 0x00
    };
    entry.mScript.compiledData.assign(retailScda.begin(), retailScda.end());
    entry.mScript.references = { cheyenneId, sunnyId, gecko1Id, gecko2Id, vcg02Id };
    entry.mScript.scriptSource = "; sunny moves towards the first well\r\n"
                                 "set VCG02.bShootingTutorialActive to 0\r\n"
                                 "CheyenneRef.evp\r\n"
                                 "sunnyref.evp\r\n"
                                 "VCG02Gecko1REF.Enable\r\n"
                                 "VCG02Gecko2REF.Enable";
    vcg02.mStages.push_back({ .mIndex = 25, .mEntries = { std::move(entry) } });
    store.overrideRecord(vcg02);

    ESM4::Script questScript;
    questScript.mId = questScriptId;
    questScript.mScript.localVarData
        = { ESM4::ScriptLocalVariableData{ .index = 4, .variableName = "bShootingTutorialActive" } };
    store.overrideRecord(questScript);

    ESM4::ActorCreature cheyenne;
    cheyenne.mId = cheyenneId;
    cheyenne.mEditorId = "CheyenneREF";
    store.overrideRecord(cheyenne);
    ESM4::ActorCharacter sunny;
    sunny.mId = sunnyId;
    sunny.mEditorId = "SunnyREF";
    store.overrideRecord(sunny);
    ESM4::ActorCreature gecko1;
    gecko1.mId = gecko1Id;
    gecko1.mEditorId = "VCG02Gecko1REF";
    store.overrideRecord(gecko1);
    ESM4::ActorCreature gecko2;
    gecko2.mId = gecko2Id;
    gecko2.mEditorId = "VCG02Gecko2REF";
    store.overrideRecord(gecko2);

    std::vector<std::pair<MWWorld::ESM4QuestReferenceCommand, ESM::FormId>> commands;
    MWWorld::ESM4QuestRuntime runtime;
    runtime.setReferenceCommandHandler(
        [&commands](MWWorld::ESM4QuestReferenceCommand command, ESM::FormId reference) {
            commands.emplace_back(command, reference);
            return true;
        });
    runtime.initialize(store);

    ASSERT_TRUE(runtime.setStage(vcg02Id, 25));
    EXPECT_EQ(runtime.getQuestVariable("VCG02", "bShootingTutorialActive"), 0.f);
    EXPECT_EQ(commands,
        (std::vector<std::pair<MWWorld::ESM4QuestReferenceCommand, ESM::FormId>>{
            { MWWorld::ESM4QuestReferenceCommand::EvaluatePackage, cheyenneId },
            { MWWorld::ESM4QuestReferenceCommand::EvaluatePackage, sunnyId },
            { MWWorld::ESM4QuestReferenceCommand::Enable, gecko1Id },
            { MWWorld::ESM4QuestReferenceCommand::Enable, gecko2Id },
        }));
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
    EXPECT_EQ(runtime.getUnsupportedCompiledOpcodes(),
        (std::vector<std::uint16_t>{ 0x1021, 0x1021 }));
}

TEST(ESM4QuestRuntimeTest, ExecutesExactGoodspringsSneakTutorialStageTransaction)
{
    MWWorld::ESMStore store;

    const ESM::FormId vcg02Id{ .mIndex = 0x10a214, .mContentFile = 0 };
    const ESM::FormId sunnyId{ .mIndex = 0x104e85, .mContentFile = 0 };
    const ESM::FormId tutorialQuestId{ .mIndex = 0x059c85, .mContentFile = 0 };
    const ESM::FormId tutorialScriptId{ .mIndex = 0x104c01, .mContentFile = 0 };
    const ESM::FormId tutorialMessageId{ .mIndex = 0x0abc58, .mContentFile = 0 };

    ESM4::Quest vcg02 = makeQuest(vcg02Id, "VCG02");
    vcg02.mObjectives.push_back({ .mIndex = 20, .mDescription = "Sneak closer to the geckos" });
    ESM4::QuestStageEntry entry;
    // FalloutNV.esm 0010A214 VCG02 stage 35 entry 0, byte-for-byte. The SCROs are
    // SunnyREF, VCG02, CGTutorial, and CGTutorialSneak.
    const std::array<std::uint8_t, 56> retailScda{
        0xa2, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x02, 0x00, 0x6e, 0x14, 0x00, 0x00, 0x00, 0x6e, 0x01, 0x00,
        0x00, 0x00, 0x1c, 0x00, 0x01, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x39, 0x10, 0x0a, 0x00, 0x02, 0x00, 0x72,
        0x03, 0x00, 0x6e, 0x5a, 0x00, 0x00, 0x00, 0x59, 0x10, 0x0b, 0x00, 0x01, 0x00, 0x72, 0x04, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    entry.mScript.compiledData.assign(retailScda.begin(), retailScda.end());
    entry.mScript.references = { sunnyId, vcg02Id, tutorialQuestId, tutorialMessageId };
    vcg02.mStages.push_back({ .mIndex = 35, .mEntries = { std::move(entry) } });
    store.overrideRecord(vcg02);

    ESM4::Quest tutorial = makeQuest(tutorialQuestId, "CGTutorial");
    tutorial.mQuestScript = tutorialScriptId;
    ESM4::QuestStageEntry tutorialEntry;
    // FalloutNV.esm 00059C85 CGTutorial stage 90 entry 0, byte-for-byte:
    // set CGTutorial.runTimer to 1; set CGTutorial.timer to 0.
    const std::array<std::uint8_t, 28> retailTutorialScda{
        0x15, 0x00, 0x0a, 0x00, 0x72, 0x01, 0x00, 0x73, 0x01, 0x00, 0x02, 0x00, 0x20, 0x31, 0x15, 0x00,
        0x0a, 0x00, 0x72, 0x01, 0x00, 0x66, 0x02, 0x00, 0x02, 0x00, 0x20, 0x30
    };
    tutorialEntry.mScript.compiledData.assign(retailTutorialScda.begin(), retailTutorialScda.end());
    tutorialEntry.mScript.references = { tutorialQuestId };
    tutorial.mStages.push_back({ .mIndex = 90, .mEntries = { std::move(tutorialEntry) } });
    store.overrideRecord(tutorial);
    ESM4::Script tutorialScript;
    tutorialScript.mId = tutorialScriptId;
    tutorialScript.mEditorId = "CGTutorialSCRIPT";
    tutorialScript.mScript.localVarData = {
        ESM4::ScriptLocalVariableData{ .index = 1, .type = 1, .variableName = "runTimer" },
        ESM4::ScriptLocalVariableData{ .index = 2, .type = 0, .variableName = "timer" },
    };
    store.overrideRecord(tutorialScript);
    ESM4::ActorCharacter sunny;
    sunny.mId = sunnyId;
    sunny.mEditorId = "SunnyREF";
    store.overrideRecord(sunny);
    ESM4::Message message;
    message.mId = tutorialMessageId;
    message.mEditorId = "CGTutorialSneak";
    message.mDescription = "To sneak or crouch, &sUActnCrouch;.";
    store.overrideRecord(message);
    ASSERT_NE(store.get<ESM4::Quest>().search(ESM::RefId(vcg02Id)), nullptr);
    ASSERT_NE(store.get<ESM4::Quest>().search(ESM::RefId(tutorialQuestId)), nullptr);
    ASSERT_NE(store.get<ESM4::ActorCharacter>().search(sunnyId), nullptr);
    ASSERT_NE(store.get<ESM4::Message>().search(tutorialMessageId), nullptr);

    std::vector<ESM::FormId> evaluatedPackages;
    std::vector<ESM::FormId> shownMessages;
    MWWorld::ESM4QuestRuntime runtime;
    runtime.setReferenceCommandHandler(
        [&evaluatedPackages](MWWorld::ESM4QuestReferenceCommand command, ESM::FormId reference) {
            if (command != MWWorld::ESM4QuestReferenceCommand::EvaluatePackage)
                return false;
            evaluatedPackages.push_back(reference);
            return true;
        });
    runtime.setMessageHandler([&shownMessages](ESM::FormId messageId) {
        shownMessages.push_back(messageId);
        return true;
    });
    runtime.initialize(store);

    ASSERT_TRUE(runtime.setStage(vcg02Id, 35));
    const MWWorld::ESM4QuestState* vcg02State = runtime.search(vcg02Id);
    const MWWorld::ESM4QuestState* tutorialState = runtime.search(tutorialQuestId);
    ASSERT_NE(vcg02State, nullptr);
    ASSERT_NE(tutorialState, nullptr);
    EXPECT_EQ(vcg02State->mCurrentStage, 35);
    EXPECT_EQ(vcg02State->mObjectiveStatus.at(20), MWWorld::ESM4QuestState::Objective_Completed);
    EXPECT_EQ(tutorialState->mCurrentStage, 90);
    EXPECT_TRUE(tutorialState->mStageDone.at(90));
    EXPECT_EQ(runtime.getQuestVariable("CGTutorial", "runTimer"), 1.f);
    EXPECT_EQ(runtime.getQuestVariable("CGTutorial", "timer"), 0.f);
    EXPECT_EQ(evaluatedPackages, std::vector<ESM::FormId>{ sunnyId });
    EXPECT_EQ(shownMessages, std::vector<ESM::FormId>{ tutorialMessageId });
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
    EXPECT_TRUE(runtime.getUnsupportedCompiledOpcodes().empty());
}

TEST(ESM4QuestRuntimeTest, ExecutesExactGoodspringsSunnySayToBark)
{
    MWWorld::ESMStore store;

    const ESM::FormId vcg02Id{ .mIndex = 0x10a214, .mContentFile = 0 };
    const ESM::FormId sunnyId{ .mIndex = 0x104e85, .mContentFile = 0 };
    const ESM::FormId playerId{ .mIndex = 0x14, .mContentFile = 0 };
    const ESM::FormId barkId{ .mIndex = 0x10a1e0, .mContentFile = 0 };

    ESM4::Quest vcg02 = makeQuest(vcg02Id, "VCG02");
    ESM4::QuestStageEntry entry;
    // FalloutNV.esm 0010A214 VCG02 stage 40 entry 0, byte-for-byte. The SCROs
    // are SunnyREF, PlayerRef, and VCG02SunnyBark.
    const std::array<std::uint8_t, 16> retailScda{
        0x1c, 0x00, 0x01, 0x00, 0x34, 0x10, 0x08, 0x00, 0x02, 0x00, 0x72, 0x02, 0x00, 0x72, 0x03, 0x00
    };
    entry.mScript.compiledData.assign(retailScda.begin(), retailScda.end());
    entry.mScript.references = { sunnyId, playerId, barkId };
    vcg02.mStages.push_back({ .mIndex = 40, .mEntries = { std::move(entry) } });
    store.overrideRecord(vcg02);

    ESM4::ActorCharacter sunny;
    sunny.mId = sunnyId;
    sunny.mEditorId = "SunnyREF";
    store.overrideRecord(sunny);
    ESM4::Dialogue bark;
    bark.mId = barkId;
    bark.mEditorId = "VCG02SunnyBark";
    store.overrideRecord(bark);

    struct SayToCall
    {
        ESM::FormId mSpeaker;
        ESM::FormId mListener;
        ESM::FormId mTopic;
    };
    std::vector<SayToCall> calls;
    MWWorld::ESM4QuestRuntime runtime;
    runtime.setSayToHandler([&calls](ESM::FormId speaker, ESM::FormId listener, ESM::FormId topic) {
        calls.push_back({ speaker, listener, topic });
        return true;
    });
    runtime.initialize(store);

    ASSERT_TRUE(runtime.setStage(vcg02Id, 40));
    const MWWorld::ESM4QuestState* state = runtime.search(vcg02Id);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mCurrentStage, 40);
    EXPECT_TRUE(state->mStageDone.at(40));
    ASSERT_EQ(calls.size(), 1);
    EXPECT_EQ(calls[0].mSpeaker, sunnyId);
    EXPECT_EQ(calls[0].mListener, playerId);
    EXPECT_EQ(calls[0].mTopic, barkId);
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
    EXPECT_TRUE(runtime.getUnsupportedCompiledOpcodes().empty());
}

TEST(ESM4QuestRuntimeTest, ExecutesExactGoodspringsVatsTutorialStageTransaction)
{
    MWWorld::ESMStore store;

    const ESM::FormId vcg02Id{ .mIndex = 0x10a214, .mContentFile = 0 };
    const ESM::FormId tutorialQuestId{ .mIndex = 0x059c85, .mContentFile = 0 };
    const ESM::FormId tutorialScriptId{ .mIndex = 0x104c01, .mContentFile = 0 };

    ESM4::Quest vcg02 = makeQuest(vcg02Id, "VCG02");
    vcg02.mObjectives.push_back({ .mIndex = 30, .mDescription = "Kill the geckos at the well" });
    ESM4::QuestStageEntry entry;
    // FalloutNV.esm 0010A214 VCG02 stage 45 entry 0, byte-for-byte:
    // display objective 30 and advance CGTutorial to stage 70.
    const std::array<std::uint8_t, 33> retailScda{
        0xa3, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01, 0x00, 0x6e, 0x1e, 0x00, 0x00, 0x00, 0x6e, 0x01,
        0x00, 0x00, 0x00, 0x39, 0x10, 0x0a, 0x00, 0x02, 0x00, 0x72, 0x02, 0x00, 0x6e, 0x46, 0x00, 0x00,
        0x00
    };
    entry.mScript.compiledData.assign(retailScda.begin(), retailScda.end());
    entry.mScript.references = { vcg02Id, tutorialQuestId };
    vcg02.mStages.push_back({ .mIndex = 45, .mEntries = { std::move(entry) } });
    store.overrideRecord(vcg02);

    ESM4::Quest tutorial = makeQuest(tutorialQuestId, "CGTutorial");
    tutorial.mQuestScript = tutorialScriptId;
    ESM4::QuestStageEntry tutorialEntry;
    // FalloutNV.esm 00059C85 CGTutorial stage 70 entry 0, byte-for-byte:
    // set CGTutorial.runTimer to 1; set CGTutorial.timer to 3.
    const std::array<std::uint8_t, 28> retailTutorialScda{
        0x15, 0x00, 0x0a, 0x00, 0x72, 0x01, 0x00, 0x73, 0x01, 0x00, 0x02, 0x00, 0x20, 0x31, 0x15, 0x00,
        0x0a, 0x00, 0x72, 0x01, 0x00, 0x66, 0x02, 0x00, 0x02, 0x00, 0x20, 0x33
    };
    tutorialEntry.mScript.compiledData.assign(retailTutorialScda.begin(), retailTutorialScda.end());
    tutorialEntry.mScript.references = { tutorialQuestId };
    tutorial.mStages.push_back({ .mIndex = 70, .mEntries = { std::move(tutorialEntry) } });
    store.overrideRecord(tutorial);

    ESM4::Script tutorialScript;
    tutorialScript.mId = tutorialScriptId;
    tutorialScript.mEditorId = "CGTutorialSCRIPT";
    tutorialScript.mScript.localVarData = {
        ESM4::ScriptLocalVariableData{ .index = 1, .type = 1, .variableName = "runTimer" },
        ESM4::ScriptLocalVariableData{ .index = 2, .type = 0, .variableName = "timer" },
    };
    store.overrideRecord(tutorialScript);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);

    ASSERT_TRUE(runtime.setStage(vcg02Id, 45));
    const MWWorld::ESM4QuestState* vcg02State = runtime.search(vcg02Id);
    const MWWorld::ESM4QuestState* tutorialState = runtime.search(tutorialQuestId);
    ASSERT_NE(vcg02State, nullptr);
    ASSERT_NE(tutorialState, nullptr);
    EXPECT_EQ(vcg02State->mCurrentStage, 45);
    EXPECT_EQ(vcg02State->mObjectiveStatus.at(30), MWWorld::ESM4QuestState::Objective_Displayed);
    EXPECT_EQ(tutorialState->mCurrentStage, 70);
    EXPECT_EQ(runtime.getQuestVariable("CGTutorial", "runTimer"), 1.f);
    EXPECT_EQ(runtime.getQuestVariable("CGTutorial", "timer"), 3.f);
    EXPECT_TRUE(runtime.getUnsupportedStageCommands().empty());
    EXPECT_TRUE(runtime.getUnsupportedCompiledOpcodes().empty());
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

TEST(ESM4QuestRuntimeTest, ExecutesExactGoodspringsConditionalDialogueQuestResults)
{
    MWWorld::ESMStore store;

    const ESM::FormId goodspringsId{ .mIndex = 0x104c66, .mContentFile = 0 };
    ESM4::Quest goodsprings = makeQuest(goodspringsId, "VFreeformGoodsprings");
    const ESM::FormId goodspringsScriptId{ .mIndex = 0x104c67, .mContentFile = 0 };
    goodsprings.mQuestScript = goodspringsScriptId;
    store.overrideRecord(goodsprings);
    ESM4::Script goodspringsScript;
    goodspringsScript.mId = goodspringsScriptId;
    goodspringsScript.mEditorId = "VFreeformGoodspringsScript";
    goodspringsScript.mScript.localVarData = {
        ESM4::ScriptLocalVariableData{ .index = 1, .variableName = "bArgumentOver" },
    };
    store.overrideRecord(goodspringsScript);

    const ESM::FormId vcg02Id{ .mIndex = 0x10a214, .mContentFile = 0 };
    ESM4::Quest vcg02 = makeQuest(vcg02Id, "VCG02");
    vcg02.mObjectives.push_back({ .mIndex = 70, .mDescription = "Complete the exam" });
    store.overrideRecord(vcg02);

    const ESM::FormId vcg03Id{ .mIndex = 0x15d912, .mContentFile = 0 };
    ESM4::Quest vcg03 = makeQuest(vcg03Id, "VCG03");
    vcg03.mObjectives.push_back({ .mIndex = 40, .mDescription = "Finish the tutorial" });
    store.overrideRecord(vcg03);

    const ESM::FormId vms16Id{ .mIndex = 0x104eae, .mContentFile = 0 };
    ESM4::Quest vms16 = makeQuest(vms16Id, "VMS16");
    vms16.mStages = {
        ESM4::QuestStage{ .mIndex = 5, .mEntries = { ESM4::QuestStageEntry{} } },
        ESM4::QuestStage{ .mIndex = 10, .mEntries = { ESM4::QuestStageEntry{} } },
        ESM4::QuestStage{ .mIndex = 110, .mEntries = { ESM4::QuestStageEntry{} } },
    };
    store.overrideRecord(vms16);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);

    runtime.executeResultSource("if VFreeformGoodsprings.bArgumentOver == 0\n"
                                "  set VFreeformGoodsprings.bArgumentOver to 1\n"
                                "endif");
    EXPECT_EQ(runtime.getQuestVariable("VFreeformGoodsprings", "bArgumentOver"), 1.f);

    ASSERT_TRUE(runtime.startQuest("VCG03"));
    runtime.executeResultSource("if(GetQuestRunning VCG03)\n"
                                "  RewardXP 50\n"
                                "  SetObjectiveCompleted VCG03 40 1\n"
                                "  CompleteQuest VCG03\n"
                                "else\n"
                                "  SetObjectiveCompleted VCG02 70 1\n"
                                "  CompleteQuest VCG02\n"
                                "endif\n"
                                "StopQuest VCG02\n"
                                "StopQuest VCG03");
    const MWWorld::ESM4QuestState* vcg03State = runtime.search(vcg03Id);
    const MWWorld::ESM4QuestState* vcg02State = runtime.search(vcg02Id);
    ASSERT_NE(vcg03State, nullptr);
    ASSERT_NE(vcg02State, nullptr);
    EXPECT_NE(vcg03State->mFlags & MWWorld::ESM4QuestState::Flag_Completed, 0);
    EXPECT_EQ(vcg03State->mObjectiveStatus.at(40), MWWorld::ESM4QuestState::Objective_Completed);
    EXPECT_EQ(vcg02State->mFlags & MWWorld::ESM4QuestState::Flag_Completed, 0);
    EXPECT_EQ(vcg02State->mObjectiveStatus.at(70), 0);

    ASSERT_TRUE(runtime.setStage("VMS16", 10));
    runtime.executeResultSource("if GetStage VMS16 > 0\nSetStage VMS16 110\nendif");
    ASSERT_NE(runtime.search(vms16Id), nullptr);
    EXPECT_EQ(runtime.search(vms16Id)->mCurrentStage, 110);

    runtime.executeResultSource(
        "if SunnyRef.GetDead == 0\nSetStage VMS16 5\nelse\nSetStage VMS16 5\nendif");
    EXPECT_EQ(runtime.search(vms16Id)->mCurrentStage, 110)
        << "unsupported actor predicates must make the entire branch tree fail closed";
    ASSERT_FALSE(runtime.getUnsupportedStageCommands().empty());
    EXPECT_EQ(runtime.getUnsupportedStageCommands().back(), "if SunnyRef.GetDead == 0");
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

TEST(ESM4QuestRuntimeTest, ImportsRetailSaveProgressTransactionallyWithoutExecutingStageScripts)
{
    MWWorld::ESMStore store;
    const ESM::FormId questId{ .mIndex = 0x5229, .mContentFile = 1 };
    const ESM::FormId scriptId{ .mIndex = 0x522a, .mContentFile = 1 };
    ESM4::Quest quest = makeQuest(questId, "SaveQuest");
    quest.mQuestScript = scriptId;
    quest.mObjectives.push_back({ .mIndex = 10, .mDescription = "Read the imported objective" });
    ESM4::QuestStageEntry stageEntry;
    stageEntry.mScript.scriptSource = "SetObjectiveCompleted SaveQuest 10 1";
    quest.mStages.push_back({ .mIndex = 5, .mEntries = { std::move(stageEntry) } });
    store.overrideRecord(quest);
    ESM4::Script script;
    script.mId = scriptId;
    script.mScript.localVarData
        = { ESM4::ScriptLocalVariableData{ .index = 7, .variableName = "iDialoguePath" } };
    store.overrideRecord(script);

    MWWorld::ESM4QuestRuntime runtime;
    runtime.initialize(store);
    MWWorld::ESM4SavedQuestProgress progress;
    progress.mStates.push_back({ questId, 0 });
    progress.mStages.push_back({ questId, 5, 1 });
    progress.mObjectives.push_back(
        { questId, 10, MWWorld::ESM4QuestState::Objective_Displayed
                | MWWorld::ESM4QuestState::Objective_Completed });
    progress.mVariables.push_back({ questId, 7, 42.25f });
    progress.mActiveQuest = questId;

    std::string error;
    ASSERT_TRUE(runtime.loadSavedProgress(progress, &error)) << error;
    const MWWorld::ESM4QuestState* state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mFlags, 0x21);
    EXPECT_EQ(state->mCurrentStage, 5);
    EXPECT_TRUE(state->mStageDone.at(5));
    EXPECT_EQ(state->mObjectiveStatus.at(10),
        MWWorld::ESM4QuestState::Objective_Displayed | MWWorld::ESM4QuestState::Objective_Completed);
    EXPECT_EQ(runtime.getQuestVariable("SaveQuest", "iDialoguePath"), 42.25f);
    EXPECT_EQ(runtime.getActiveQuest(), questId);

    MWWorld::ESM4SavedQuestProgress invalid = progress;
    invalid.mVariables.front().mIndex = 99;
    EXPECT_FALSE(runtime.loadSavedProgress(invalid, &error));
    EXPECT_FALSE(error.empty());
    state = runtime.search(questId);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mObjectiveStatus.at(10),
        MWWorld::ESM4QuestState::Objective_Displayed | MWWorld::ESM4QuestState::Objective_Completed);
    EXPECT_EQ(runtime.getQuestVariable("SaveQuest", "iDialoguePath"), 42.25f);
    EXPECT_EQ(runtime.getActiveQuest(), questId);
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
