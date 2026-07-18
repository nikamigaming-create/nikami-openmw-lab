#include <components/esm4/loaddial.hpp>
#include <components/esm4/loadinfo.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void appendFourCC(std::string& output, std::string_view value)
    {
        ASSERT_EQ(value.size(), 4);
        output.append(value);
    }

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        ASSERT_LE(data.size(), std::numeric_limits<std::uint16_t>::max());
        appendFourCC(output, type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    void appendSubRecord(std::string& output, std::string_view type, const std::string& data)
    {
        appendSubRecord(output, type, std::string_view(data));
    }

    template <class T>
    void appendSubRecord(std::string& output, std::string_view type, const T& data)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        appendSubRecord(output, type, std::string_view(reinterpret_cast<const char*>(&data), sizeof(data)));
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view payload,
        std::uint32_t flags = 0)
    {
        appendFourCC(output, type);
        appendPod(output, static_cast<std::uint32_t>(payload.size()));
        appendPod(output, flags);
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 }); // revision
        appendPod(output, std::uint16_t{ 0 }); // form version
        appendPod(output, std::uint16_t{ 0 }); // unknown
        output.append(payload);
    }

    std::string makePlugin(std::string_view recordType, std::uint32_t formId, std::string_view recordPayload)
    {
        std::string headerPayload;
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 1 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        appendSubRecord(headerPayload, "HEDR", hedr);

        std::string result;
        appendRecord(result, "TES4", 0, headerPayload);
        appendRecord(result, recordType, formId, recordPayload);
        return result;
    }

    std::unique_ptr<ESM4::Reader> makeReader(
        std::string_view recordType, std::uint32_t formId, const std::string& payload, std::uint32_t modIndex = 0)
    {
        const std::string plugin = makePlugin(recordType, formId, payload);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "synthetic.esm", nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    ESM4::TargetCondition condition(std::uint32_t function, std::uint32_t parameter, float comparison = 1.f)
    {
        ESM4::TargetCondition result{};
        result.comparison = comparison;
        result.functionIndex = function;
        result.param1 = parameter;
        return result;
    }

    TEST(Esm4BehaviorRecordTest, shouldPreserveNpcVoiceTypeAndAllFactions)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("DialogueActor"));
        appendSubRecord(payload, "VTCK", std::uint32_t{ 0x1234 });
        ESM4::ActorFaction first{ .faction = 0x2001, .rank = 2 };
        ESM4::ActorFaction second{ .faction = 0x2002, .rank = -1 };
        appendSubRecord(payload, "SNAM", first);
        appendSubRecord(payload, "SNAM", second);

        auto reader = makeReader("NPC_", 0x1000, payload, 3);
        ESM4::Npc npc;
        npc.load(*reader);

        EXPECT_EQ(npc.mVoiceType, ESM::FormId::fromUint32(0x03001234));
        ASSERT_EQ(npc.mFactions.size(), 2);
        EXPECT_EQ(ESM::FormId::fromUint32(npc.mFactions[0].faction), ESM::FormId::fromUint32(0x03002001));
        EXPECT_EQ(npc.mFactions[0].rank, 2);
        EXPECT_EQ(ESM::FormId::fromUint32(npc.mFactions[1].faction), ESM::FormId::fromUint32(0x03002002));
        EXPECT_EQ(npc.mFactions[1].rank, -1);
    }

    TEST(Esm4BehaviorRecordTest, shouldPreserveExactFalloutNpcRuntimeStatePayloads)
    {
        const ESM4::ACBS_FO3 baseConfig{ .fatigue = 200, .levelOrMult = 1, .speedMultiplier = 100 };
        const ESM4::AIDataFO3 aiData{ .aggression = 1,
            .confidence = 2,
            .energyLevel = 73,
            .responsibility = 3,
            .mood = 4,
            .unused = { 0xa1, 0xb2, 0xc3 },
            .services = 0x12345678,
            .trainSkill = -7,
            .trainLevel = 42,
            .assistance = -2,
            .aggroRadiusBehavior = 0x81,
            .aggroRadius = -1234567 };
        const ESM4::Npc::FNVData npcData{ .health = 275,
            .strength = 1,
            .perception = 2,
            .endurance = 3,
            .charisma = 4,
            .intelligence = 5,
            .agility = 6,
            .luck = 7 };
        const ESM4::Npc::FNVSkills skills{
            .values = { .barter = 11,
                .bigGuns = 12,
                .energyWeapons = 13,
                .explosives = 14,
                .lockpick = 15,
                .medicine = 16,
                .meleeWeapons = 17,
                .repair = 18,
                .science = 19,
                .smallGuns = 20,
                .sneak = 21,
                .speech = 22,
                .survivalOrThrowing = 23,
                .unarmed = 24 },
            .offsets = { .barter = 31,
                .bigGuns = 32,
                .energyWeapons = 33,
                .explosives = 34,
                .lockpick = 35,
                .medicine = 36,
                .meleeWeapons = 37,
                .repair = 38,
                .science = 39,
                .smallGuns = 40,
                .sneak = 41,
                .speech = 42,
                .survivalOrThrowing = 43,
                .unarmed = 44 }
        };

        std::string payload;
        appendSubRecord(payload, "ACBS", baseConfig);
        appendSubRecord(payload, "AIDT", aiData);
        appendSubRecord(payload, "DATA", npcData);
        appendSubRecord(payload, "DNAM", skills);

        auto reader = makeReader("NPC_", 0x1011, payload);
        ESM4::Npc npc;
        npc.load(*reader);

        EXPECT_TRUE(npc.mIsFONV);
        EXPECT_TRUE(npc.mHasFNVBaseConfig);
        EXPECT_TRUE(npc.mHasFNVAIData);
        EXPECT_TRUE(npc.mHasFNVData);
        EXPECT_TRUE(npc.mHasFNVSkills);
        EXPECT_EQ(std::memcmp(&npc.mBaseConfig.fo3, &baseConfig, sizeof(baseConfig)), 0);
        EXPECT_EQ(std::memcmp(&npc.mFNVAIData, &aiData, sizeof(aiData)), 0);
        EXPECT_EQ(std::memcmp(&npc.mFNVData, &npcData, sizeof(npcData)), 0);
        EXPECT_EQ(std::memcmp(&npc.mFNVSkills, &skills, sizeof(skills)), 0);
        EXPECT_EQ(npc.mFNVAIData.aggression, 1);
        EXPECT_EQ(npc.mFNVAIData.confidence, 2);
        EXPECT_EQ(npc.mFNVAIData.energyLevel, 73);
        EXPECT_EQ(npc.mFNVAIData.responsibility, 3);
        EXPECT_EQ(npc.mFNVAIData.mood, 4);
        EXPECT_EQ(npc.mFNVAIData.unused[0], 0xa1);
        EXPECT_EQ(npc.mFNVAIData.unused[1], 0xb2);
        EXPECT_EQ(npc.mFNVAIData.unused[2], 0xc3);
        EXPECT_EQ(npc.mFNVAIData.services, 0x12345678u);
        EXPECT_EQ(npc.mFNVAIData.trainSkill, -7);
        EXPECT_EQ(npc.mFNVAIData.trainLevel, 42);
        EXPECT_EQ(npc.mFNVAIData.assistance, -2);
        EXPECT_EQ(npc.mFNVAIData.aggroRadiusBehavior, 0x81);
        EXPECT_EQ(npc.mFNVAIData.aggroRadius, -1234567);
        EXPECT_EQ(npc.mFNVData.health, 275);
        EXPECT_EQ(npc.mFNVData.strength, 1);
        EXPECT_EQ(npc.mFNVData.perception, 2);
        EXPECT_EQ(npc.mFNVData.endurance, 3);
        EXPECT_EQ(npc.mFNVData.charisma, 4);
        EXPECT_EQ(npc.mFNVData.intelligence, 5);
        EXPECT_EQ(npc.mFNVData.agility, 6);
        EXPECT_EQ(npc.mFNVData.luck, 7);
        EXPECT_EQ(npc.mFNVSkills.values.barter, 11);
        EXPECT_EQ(npc.mFNVSkills.values.speech, 22);
        EXPECT_EQ(npc.mFNVSkills.values.survivalOrThrowing, 23);
        EXPECT_EQ(npc.mFNVSkills.values.unarmed, 24);
        EXPECT_EQ(npc.mFNVSkills.offsets.barter, 31);
        EXPECT_EQ(npc.mFNVSkills.offsets.speech, 42);
        EXPECT_EQ(npc.mFNVSkills.offsets.survivalOrThrowing, 43);
        EXPECT_EQ(npc.mFNVSkills.offsets.unarmed, 44);
    }

    TEST(Esm4BehaviorRecordTest, shouldRejectMalformedFalloutNpcRuntimeStatePayloads)
    {
        constexpr std::array<std::pair<std::string_view, std::size_t>, 3> malformed{
            std::pair{ std::string_view{ "AIDT" }, std::size_t{ 19 } },
            std::pair{ std::string_view{ "DATA" }, std::size_t{ 10 } },
            std::pair{ std::string_view{ "DNAM" }, std::size_t{ 27 } },
        };

        for (const auto& [type, size] : malformed)
        {
            SCOPED_TRACE(type);
            std::string payload;
            appendSubRecord(payload, type, std::string(size, '\0'));

            auto reader = makeReader("NPC_", 0x1012, payload);
            ESM4::Npc npc;
            EXPECT_THROW(npc.load(*reader), std::runtime_error);
        }
    }

    TEST(Esm4BehaviorRecordTest, shouldPreserveFalloutQuestStagesObjectivesConditionsAndBytecode)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("TestQuest"));
        appendSubRecord(payload, "FULL", zString("Synthetic Quest"));

        ESM4::QuestData questData{ ESM4::Quest::Flag_StartGameEnabled, 50, 0, 1.5f };
        appendSubRecord(payload, "DATA", questData);
        appendSubRecord(payload, "CTDA", condition(56, 0x100));

        appendSubRecord(payload, "INDX", std::int16_t{ 10 });
        appendSubRecord(payload, "QSDT", std::uint8_t{ ESM4::QuestStageEntry::Flag_CompleteQuest });
        appendSubRecord(payload, "CTDA", condition(58, 0x200, 10.f));
        appendSubRecord(payload, "CNAM", zString("First log entry"));

        ESM4::ScriptHeader scriptHeader{};
        scriptHeader.refCount = 1;
        scriptHeader.compiledSize = 3;
        appendSubRecord(payload, "SCHR", scriptHeader);
        const std::array<std::uint8_t, 3> bytecode{ 0x10, 0x20, 0x30 };
        appendSubRecord(payload, "SCDA", bytecode);
        appendSubRecord(payload, "SCTX", std::string_view("SetStage TestQuest 20"));

        ESM4::ScriptLocalVariableData local{};
        local.index = 7;
        local.type = 1;
        std::array<std::uint8_t, 24> localBytes{};
        std::memcpy(localBytes.data(), &local.index, sizeof(local.index));
        std::memcpy(localBytes.data() + 16, &local.type, sizeof(local.type));
        appendSubRecord(payload, "SLSD", localBytes);
        appendSubRecord(payload, "SCVR", zString("counter"));
        appendSubRecord(payload, "SCRV", std::uint32_t{ 7 });
        appendSubRecord(payload, "SCRO", std::uint32_t{ 0x1234 });

        appendSubRecord(payload, "QSDT", std::uint8_t{ ESM4::QuestStageEntry::Flag_FailQuest });
        appendSubRecord(payload, "CNAM", zString("Second log entry"));
        appendSubRecord(payload, "NAM0", std::uint32_t{ 0x5678 });

        appendSubRecord(payload, "QOBJ", std::int32_t{ 42 });
        appendSubRecord(payload, "NNAM", zString("Reach the marker"));
        std::array<std::uint8_t, 8> target{};
        const std::uint32_t targetForm = 0xbeef;
        std::memcpy(target.data(), &targetForm, sizeof(targetForm));
        target[4] = ESM4::QuestObjectiveTarget::Flag_CompassMarkerIgnoresLocks;
        appendSubRecord(payload, "QSTA", target);
        appendSubRecord(payload, "CTDA", condition(72, targetForm));

        auto reader = makeReader("QUST", 0x1000, payload);
        ESM4::Quest quest;
        quest.load(*reader);

        EXPECT_EQ(quest.mEditorId, "TestQuest");
        EXPECT_EQ(quest.mQuestName, "Synthetic Quest");
        ASSERT_EQ(quest.mTargetConditions.size(), 1);
        EXPECT_EQ(quest.mTargetConditions[0].functionIndex, 56);
        ASSERT_EQ(quest.mStages.size(), 1);
        EXPECT_EQ(quest.mStages[0].mIndex, 10);
        ASSERT_EQ(quest.mStages[0].mEntries.size(), 2);
        const ESM4::QuestStageEntry& firstEntry = quest.mStages[0].mEntries[0];
        EXPECT_EQ(firstEntry.mLogEntry, "First log entry");
        ASSERT_EQ(firstEntry.mConditions.size(), 1);
        EXPECT_EQ(firstEntry.mConditions[0].functionIndex, 58);
        EXPECT_EQ(firstEntry.mScript.compiledData, std::vector<std::uint8_t>(bytecode.begin(), bytecode.end()));
        EXPECT_EQ(firstEntry.mScript.scriptSource, "SetStage TestQuest 20");
        ASSERT_EQ(firstEntry.mScript.localVarData.size(), 1);
        EXPECT_EQ(firstEntry.mScript.localVarData[0].variableName, "counter");
        EXPECT_EQ(firstEntry.mScript.localRefVarIndex, std::vector<std::uint32_t>{ 7 });
        ASSERT_EQ(firstEntry.mScript.references.size(), 1);
        EXPECT_EQ(firstEntry.mScript.references[0], ESM::FormId::fromUint32(0x1234));
        EXPECT_EQ(quest.mStages[0].mEntries[1].mLogEntry, "Second log entry");
        EXPECT_EQ(quest.mStages[0].mEntries[1].mNextQuest, ESM::FormId::fromUint32(0x5678));
        ASSERT_EQ(quest.mObjectives.size(), 1);
        EXPECT_EQ(quest.mObjectives[0].mIndex, 42);
        EXPECT_EQ(quest.mObjectives[0].mDescription, "Reach the marker");
        ASSERT_EQ(quest.mObjectives[0].mTargets.size(), 1);
        EXPECT_EQ(quest.mObjectives[0].mTargets[0].mTarget, ESM::FormId::fromUint32(targetForm));
        ASSERT_EQ(quest.mObjectives[0].mTargets[0].mConditions.size(), 1);
        EXPECT_EQ(quest.mObjectives[0].mTargets[0].mConditions[0].functionIndex, 72);
    }

    TEST(Esm4BehaviorRecordTest, shouldPreserveAllInfoResponsesConditionsLinksAndResultScripts)
    {
        std::string payload;
        const std::array<std::uint8_t, 4> data{ 0, 1, 0x04, 0x01 };
        appendSubRecord(payload, "DATA", data);
        appendSubRecord(payload, "QSTI", std::uint32_t{ 0x1000 });

        ESM4::TargetResponseData firstResponse{};
        firstResponse.emoType = ESM4::EMO_Happy;
        firstResponse.responseNo = 1;
        firstResponse.responsePadding[0] = 0x0f;
        firstResponse.responsePadding[1] = 0x04;
        firstResponse.responsePadding[2] = 0x01;
        appendSubRecord(payload, "TRDT", firstResponse);
        appendSubRecord(payload, "NAM1", zString("First response"));
        appendSubRecord(payload, "SNAM", std::uint32_t{ 0x2001 });

        ESM4::TargetResponseData secondResponse{};
        secondResponse.emoType = ESM4::EMO_Anger;
        secondResponse.responseNo = 2;
        appendSubRecord(payload, "TRDT", secondResponse);
        appendSubRecord(payload, "NAM1", zString("Second response"));
        appendSubRecord(payload, "LNAM", std::uint32_t{ 0x2002 });

        appendSubRecord(payload, "CTDA", condition(56, 0x1000));
        appendSubRecord(payload, "CTDA", condition(47, 0x3000, 2.f));
        appendSubRecord(payload, "NAME", std::uint32_t{ 0x4000 });
        appendSubRecord(payload, "TCLT", std::uint32_t{ 0x4001 });
        appendSubRecord(payload, "TCLT", std::uint32_t{ 0x4002 });
        appendSubRecord(payload, "TCLF", std::uint32_t{ 0x4003 });
        appendSubRecord(payload, "TCFU", std::uint32_t{ 0x5000 });

        ESM4::ScriptHeader beginHeader{};
        beginHeader.compiledSize = 2;
        appendSubRecord(payload, "SCHR", beginHeader);
        const std::array<std::uint8_t, 2> beginBytecode{ 0xaa, 0xbb };
        appendSubRecord(payload, "SCDA", beginBytecode);
        appendSubRecord(payload, "SCRO", std::uint32_t{ 0x6000 });
        appendSubRecord(payload, "NEXT", std::string_view{});
        ESM4::ScriptHeader endHeader{};
        endHeader.compiledSize = 1;
        appendSubRecord(payload, "SCHR", endHeader);
        const std::array<std::uint8_t, 1> endBytecode{ 0xcc };
        appendSubRecord(payload, "SCDA", endBytecode);

        appendSubRecord(payload, "RNAM", zString("Ask about the test"));
        appendSubRecord(payload, "ANAM", std::uint32_t{ 0x7000 });
        appendSubRecord(payload, "KNAM", std::uint32_t{ 0x7001 });
        appendSubRecord(payload, "DNAM", std::uint32_t{ 3 });

        auto reader = makeReader("INFO", 0x1010, payload);
        ESM4::DialogInfo info;
        info.load(*reader);

        EXPECT_EQ(info.mQuest, ESM::FormId::fromUint32(0x1000));
        EXPECT_EQ(info.mDialType, 0);
        EXPECT_EQ(info.mNextSpeaker, 1);
        EXPECT_EQ(info.mInfoFlags, 0x0104);
        ASSERT_EQ(info.mResponses.size(), 2);
        EXPECT_EQ(info.mResponses[0].mData.responseNo, 1);
        EXPECT_EQ(info.mResponses[0].mResponse, "First response");
        EXPECT_EQ(info.mResponses[0].mSpeakerAnimation, ESM::FormId::fromUint32(0x2001));
        EXPECT_EQ(info.mResponses[1].mResponse, "Second response");
        EXPECT_EQ(info.mResponses[1].mListenerAnimation, ESM::FormId::fromUint32(0x2002));
        ASSERT_EQ(info.mTargetConditions.size(), 2);
        EXPECT_EQ(info.mTargetConditions[0].functionIndex, 56);
        EXPECT_EQ(info.mTargetConditions[1].functionIndex, 47);
        EXPECT_EQ(info.mAddTopics, std::vector<ESM::FormId>{ ESM::FormId::fromUint32(0x4000) });
        EXPECT_EQ(info.mChoices,
            (std::vector<ESM::FormId>{ ESM::FormId::fromUint32(0x4001), ESM::FormId::fromUint32(0x4002) }));
        EXPECT_EQ(info.mLinkFrom, std::vector<ESM::FormId>{ ESM::FormId::fromUint32(0x4003) });
        EXPECT_EQ(info.mFollowUps, std::vector<ESM::FormId>{ ESM::FormId::fromUint32(0x5000) });
        EXPECT_EQ(info.mScript.compiledData, std::vector<std::uint8_t>(beginBytecode.begin(), beginBytecode.end()));
        EXPECT_EQ(info.mScript.references, std::vector<ESM::FormId>{ ESM::FormId::fromUint32(0x6000) });
        EXPECT_EQ(info.mEndScript.compiledData, std::vector<std::uint8_t>(endBytecode.begin(), endBytecode.end()));
        EXPECT_EQ(info.mPrompt, "Ask about the test");
        EXPECT_EQ(info.mSpeaker, ESM::FormId::fromUint32(0x7000));
        EXPECT_EQ(info.mActorValueOrPerk, ESM::FormId::fromUint32(0x7001));
        EXPECT_EQ(info.mSpeechChallenge, 3);
    }

    TEST(Esm4BehaviorRecordTest, shouldAssociateDialogueInfoConnectionsWithTheirQuest)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("TestTopic"));
        appendSubRecord(payload, "QSTI", std::uint32_t{ 0x1000 });
        appendSubRecord(payload, "INFC", std::uint32_t{ 0x2000 });
        appendSubRecord(payload, "INFX", std::int32_t{ 3 });
        appendSubRecord(payload, "INFC", std::uint32_t{ 0x2001 });
        appendSubRecord(payload, "INFX", std::int32_t{ 4 });
        appendSubRecord(payload, "QSTI", std::uint32_t{ 0x1001 });
        appendSubRecord(payload, "FULL", zString("Synthetic topic"));
        appendSubRecord(payload, "PNAM", 42.f);
        const std::array<std::uint8_t, 2> data{ 0, 2 };
        appendSubRecord(payload, "DATA", data);

        auto reader = makeReader("DIAL", 0x1020, payload);
        ESM4::Dialogue dialogue;
        dialogue.load(*reader);

        EXPECT_EQ(dialogue.mEditorId, "TestTopic");
        EXPECT_EQ(dialogue.mTopicName, "Synthetic topic");
        EXPECT_FLOAT_EQ(dialogue.mPriority, 42.f);
        ASSERT_EQ(dialogue.mQuests.size(), 2);
        ASSERT_EQ(dialogue.mSharedInfos.size(), 2);
        EXPECT_EQ(dialogue.mSharedInfos[0].mQuest, ESM::FormId::fromUint32(0x1000));
        EXPECT_EQ(dialogue.mSharedInfos[0].mInfo, ESM::FormId::fromUint32(0x2000));
        EXPECT_EQ(dialogue.mSharedInfos[0].mIndex, 3);
        EXPECT_EQ(dialogue.mSharedInfos[1].mInfo, ESM::FormId::fromUint32(0x2001));
        EXPECT_EQ(dialogue.mSharedInfos[1].mIndex, 4);
    }

    TEST(Esm4BehaviorRecordTest, shouldRemapQuestAndGlobalConditionFormIds)
    {
        const std::uint32_t questParameter = 0x00000200;
        const std::uint32_t comparisonGlobal = 0x00000300;
        ESM4::TargetCondition stageCondition = condition(ESM4::FUN_GetStage, questParameter);
        stageCondition.condition = ESM4::CTF_GrThOrEqTo | ESM4::CTF_UseGlobal;
        stageCondition.comparison = std::bit_cast<float>(comparisonGlobal);

        std::string payload;
        appendSubRecord(payload, "EDID", zString("RemappedConditionQuest"));
        appendSubRecord(payload, "DATA", ESM4::QuestData{});
        appendSubRecord(payload, "INDX", std::int16_t{ 10 });
        appendSubRecord(payload, "QSDT", std::uint8_t{ 0 });
        appendSubRecord(payload, "CTDA", stageCondition);

        constexpr std::uint32_t modIndex = 7;
        auto reader = makeReader("QUST", 0x1000, payload, modIndex);
        ESM4::Quest quest;
        quest.load(*reader);

        ASSERT_EQ(quest.mStages.size(), 1);
        ASSERT_EQ(quest.mStages[0].mEntries.size(), 1);
        ASSERT_EQ(quest.mStages[0].mEntries[0].mConditions.size(), 1);
        const ESM4::TargetCondition& loaded = quest.mStages[0].mEntries[0].mConditions[0];
        EXPECT_EQ(ESM::FormId::fromUint32(loaded.param1),
            (ESM::FormId{ .mIndex = questParameter, .mContentFile = static_cast<std::int32_t>(modIndex) }));
        EXPECT_EQ(ESM::FormId::fromUint32(std::bit_cast<std::uint32_t>(loaded.comparison)),
            (ESM::FormId{ .mIndex = comparisonGlobal, .mContentFile = static_cast<std::int32_t>(modIndex) }));
    }
}
