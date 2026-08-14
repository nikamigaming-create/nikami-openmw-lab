#include <components/esm4/loadperk.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
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

    template <class T>
    std::string pod(const T& value)
    {
        std::string result;
        appendPod(result, value);
        return result;
    }

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        ASSERT_EQ(type.size(), 4);
        ASSERT_LE(data.size(), std::numeric_limits<std::uint16_t>::max());
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    std::string bytes(std::initializer_list<std::uint8_t> values)
    {
        std::string result;
        result.reserve(values.size());
        for (const std::uint8_t value : values)
            result.push_back(static_cast<char>(value));
        return result;
    }

    std::string bytesFromHex(std::string_view hex)
    {
        EXPECT_EQ(hex.size() % 2, 0);
        const auto nibble = [](char value) -> std::uint8_t {
            if (value >= '0' && value <= '9')
                return static_cast<std::uint8_t>(value - '0');
            if (value >= 'a' && value <= 'f')
                return static_cast<std::uint8_t>(value - 'a' + 10);
            if (value >= 'A' && value <= 'F')
                return static_cast<std::uint8_t>(value - 'A' + 10);
            throw std::runtime_error("invalid hexadecimal fixture");
        };

        std::string result;
        result.reserve(hex.size() / 2);
        for (std::size_t index = 0; index < hex.size(); index += 2)
            result.push_back(static_cast<char>((nibble(hex[index]) << 4) | nibble(hex[index + 1])));
        return result;
    }

    std::string condition(std::uint32_t function, std::uint32_t parameter1 = 0, std::uint32_t parameter2 = 0,
        std::uint32_t runOn = 0, std::uint32_t reference = 0)
    {
        std::string result;
        appendPod(result, std::uint32_t{ 0 });
        appendPod(result, 1.f);
        appendPod(result, function);
        appendPod(result, parameter1);
        appendPod(result, parameter2);
        appendPod(result, runOn);
        appendPod(result, reference);
        return result;
    }

    std::string questData(std::uint32_t quest, std::uint8_t stage, std::array<std::uint8_t, 3> unused)
    {
        std::string result;
        appendPod(result, quest);
        appendPod(result, stage);
        result.append(reinterpret_cast<const char*>(unused.data()), unused.size());
        return result;
    }

    std::string scriptHeader(std::uint32_t references, std::uint32_t compiledSize, std::uint32_t unused = 0,
        std::uint32_t variableCount = 0, std::uint16_t type = 0, std::uint16_t flags = 1)
    {
        std::string result;
        appendPod(result, unused);
        appendPod(result, references);
        appendPod(result, compiledSize);
        appendPod(result, variableCount);
        appendPod(result, type);
        appendPod(result, flags);
        return result;
    }

    std::string recordPrefix(std::string_view editorId = "TestPerk")
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString(editorId));
        appendSubRecord(payload, "DESC", zString("Description"));
        return payload;
    }

    void appendRecordData(std::string& payload)
    {
        appendSubRecord(payload, "DATA", bytes({ 0, 12, 2, 1, 0 }));
    }

    std::string embeddedScriptPayload(std::string header, std::uint16_t epf3 = 0,
        std::string compiled = bytes({ 1, 2, 3, 4 }), std::size_t referenceCount = 1,
        std::uint32_t firstReference = 0x14)
    {
        std::string payload = recordPrefix("MalformedScriptPerk");
        appendRecordData(payload);
        appendSubRecord(payload, "PRKE", bytes({ 2, 0, 0 }));
        appendSubRecord(payload, "DATA", bytes({ 27, 9, 1 }));
        appendSubRecord(payload, "EPFT", bytes({ 4 }));
        appendSubRecord(payload, "EPF2", zString("Use"));
        appendSubRecord(payload, "EPF3", pod(epf3));
        appendSubRecord(payload, "SCHR", header);
        appendSubRecord(payload, "SCDA", compiled);
        appendSubRecord(payload, "SCTX", "source");
        for (std::size_t index = 0; index < referenceCount; ++index)
            appendSubRecord(
                payload, "SCRO", pod(firstReference + static_cast<std::uint32_t>(index)));
        appendSubRecord(payload, "PRKF", {});
        return payload;
    }

    std::string completePayload()
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("ByteExactPerk"));
        appendSubRecord(payload, "FULL", zString("Byte Exact Perk"));
        appendSubRecord(payload, "DESC", zString("Preserves every official entry branch."));
        appendSubRecord(payload, "ICON", zString("interface\\icons\\pipboyimages\\perks\\test.dds"));
        appendSubRecord(payload, "CTDA", condition(ESM4::FUN_GetDetected, 0x00000014));
        appendRecordData(payload);

        appendSubRecord(payload, "PRKE", bytes({ 0, 0, 0 }));
        appendSubRecord(payload, "DATA", questData(0x01001234, 10, { 0x65, 0x5c, 0x00 }));
        appendSubRecord(payload, "PRKF", {});

        appendSubRecord(payload, "PRKE", bytes({ 1, 1, 0 }));
        appendSubRecord(payload, "DATA", pod(std::uint32_t{ 0x00005678 }));
        appendSubRecord(payload, "PRKF", {});

        appendSubRecord(payload, "PRKE", bytes({ 1, 0, 0 }));
        appendSubRecord(payload, "PRKF", {});

        appendSubRecord(payload, "PRKE", bytes({ 2, 0, 0 }));
        appendSubRecord(payload, "DATA", bytes({ 0, 3, 1 }));
        appendSubRecord(payload, "PRKC", bytes({ 0 }));
        appendSubRecord(payload, "CTDA", condition(ESM4::FUN_IsWeaponInList, 0x0100abcd));
        appendSubRecord(payload, "EPFT", bytes({ 1 }));
        appendSubRecord(payload, "EPFD", pod(1.25f));
        appendSubRecord(payload, "PRKF", {});

        appendSubRecord(payload, "PRKE", bytes({ 2, 0, 0 }));
        appendSubRecord(payload, "DATA", bytes({ 21, 8, 2 }));
        appendSubRecord(payload, "EPFT", bytes({ 3 }));
        appendSubRecord(payload, "EPFD", pod(std::uint32_t{ 0x010098c8 }));
        appendSubRecord(payload, "PRKF", {});

        // FalloutNV.esm PERK 0015EAE1 CannibalChallengePerk embedded SCDA,
        // preserved byte-for-byte. The surrounding synthetic record supplies
        // deterministic master/current FormIDs for adjustment assertions.
        const std::string compiled = bytesFromHex(
            "7e10050001007201001c00010002100a0002007202006e010000000412070001006effffffff37110900020018006e01000000");
        const std::string source = "StartCannibal Player\r\nPlayer.AddItem NVCannibalFood 1\r\nRewardKarma -1\r\n"
                                   "ModPCMiscStat \"Corpses Eaten\" 1";
        EXPECT_EQ(compiled.size(), 51);
        EXPECT_EQ(source.size(), 102);
        appendSubRecord(payload, "PRKE", bytes({ 2, 0, 0 }));
        appendSubRecord(payload, "DATA", bytes({ 27, 9, 2 }));
        appendSubRecord(payload, "PRKC", bytes({ 0 }));
        appendSubRecord(payload, "CTDA", condition(ESM4::FUN_GetDead));
        appendSubRecord(payload, "EPFT", bytes({ 4 }));
        appendSubRecord(payload, "EPF2", zString("Dine and Dash"));
        appendSubRecord(payload, "EPF3", pod(std::uint16_t{ 0 }));
        appendSubRecord(payload, "SCHR", scriptHeader(2, static_cast<std::uint32_t>(compiled.size())));
        appendSubRecord(payload, "SCDA", compiled);
        appendSubRecord(payload, "SCTX", source);
        appendSubRecord(payload, "SCRO", pod(std::uint32_t{ 0x00000014 }));
        appendSubRecord(payload, "SCRO", pod(std::uint32_t{ 0x0015eacf }));
        appendSubRecord(payload, "PRKF", {});
        return payload;
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view payload,
        std::uint32_t flags = 0)
    {
        output.append(type);
        appendPod(output, static_cast<std::uint32_t>(payload.size()));
        appendPod(output, flags);
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 });
        output.append(payload);
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& payload)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);
        appendSubRecord(headerPayload, "MAST", zString("Master.esm"));
        appendSubRecord(headerPayload, "DATA", pod(std::uint64_t{ 0 }));

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload, ESM4::Rec_ESM);
        appendRecord(plugin, "PERK", 0x010003e8, payload, ESM4::Rec_Constant);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "FalloutNV.esm", nullptr, nullptr, true);
        reader->setModIndex(5);
        reader->updateModIndices({ { "master.esm", 2 } });
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    void expectRejected(const std::string& payload)
    {
        auto reader = makeReader(payload);
        ESM4::Perk perk;
        EXPECT_THROW(perk.load(*reader), std::runtime_error);
    }

    TEST(Esm4PerkTest, loadsEveryFrozenFnvEntryBranchAndAdjustsOnlyProvenFormIds)
    {
        auto reader = makeReader(completePayload());
        ESM4::Perk perk;
        perk.load(*reader);

        EXPECT_EQ(perk.mId, ESM::FormId::fromUint32(0x050003e8));
        EXPECT_EQ(perk.mFlags, ESM4::Rec_Constant);
        EXPECT_EQ(perk.mEditorId, "ByteExactPerk");
        EXPECT_EQ(perk.mFullName, "Byte Exact Perk");
        EXPECT_EQ(perk.mData.mMinimumLevel, 12);
        EXPECT_EQ(perk.mData.mRankCount, 2);
        EXPECT_EQ(perk.mData.mSerializedSize, 5);
        ASSERT_TRUE(perk.mData.mHidden.has_value());
        EXPECT_EQ(*perk.mData.mHidden, 0);

        ASSERT_EQ(perk.mConditions.size(), 1);
        EXPECT_EQ(perk.mConditions[0].functionIndex, ESM4::FUN_GetDetected);
        EXPECT_EQ(perk.mConditions[0].param1, 0x02000014u);
        ASSERT_EQ(perk.mEntries.size(), 6);

        const auto& quest = std::get<ESM4::Perk::QuestEntry>(perk.mEntries[0].mData);
        EXPECT_EQ(quest.mQuest, ESM::FormId::fromUint32(0x05001234));
        EXPECT_EQ(quest.mStage, 10);
        EXPECT_EQ(quest.mUnused, (std::array<std::uint8_t, 3>{ 0x65, 0x5c, 0x00 }));

        const auto& ability = std::get<ESM4::Perk::AbilityEntry>(perk.mEntries[1].mData);
        ASSERT_TRUE(ability.mAbility.has_value());
        EXPECT_EQ(*ability.mAbility, ESM::FormId::fromUint32(0x02005678));
        EXPECT_FALSE(std::get<ESM4::Perk::AbilityEntry>(perk.mEntries[2].mData).mAbility.has_value());

        const auto& number = std::get<ESM4::Perk::EntryPointEntry>(perk.mEntries[3].mData);
        ASSERT_EQ(number.mConditionGroups.size(), 1);
        ASSERT_EQ(number.mConditionGroups[0].mConditions.size(), 1);
        EXPECT_EQ(number.mConditionGroups[0].mConditions[0].param1, 0x0500abcdu);
        ASSERT_TRUE(number.mFloat.has_value());
        EXPECT_FLOAT_EQ(*number.mFloat, 1.25f);

        const auto& form = std::get<ESM4::Perk::EntryPointEntry>(perk.mEntries[4].mData);
        EXPECT_EQ(form.mFunctionType, ESM4::Perk::EntryPointFunctionType::FormId);
        EXPECT_EQ(form.mFormId, ESM::FormId::fromUint32(0x050098c8));

        const auto& script = std::get<ESM4::Perk::EntryPointEntry>(perk.mEntries[5].mData);
        EXPECT_EQ(script.mFunctionType, ESM4::Perk::EntryPointFunctionType::EmbeddedScript);
        EXPECT_EQ(script.mButtonLabel, "Dine and Dash");
        EXPECT_EQ(script.mScript.compiledData.size(), 51);
        EXPECT_EQ(script.mScript.scriptSource.size(), 102);
        const std::string parsedCompiled(script.mScript.compiledData.begin(), script.mScript.compiledData.end());
        EXPECT_EQ(parsedCompiled,
            bytesFromHex(
                "7e10050001007201001c00010002100a0002007202006e010000000412070001006effffffff37110900020018006e01000000"));
        EXPECT_EQ(script.mScript.scriptSource,
            "StartCannibal Player\r\nPlayer.AddItem NVCannibalFood 1\r\nRewardKarma -1\r\n"
            "ModPCMiscStat \"Corpses Eaten\" 1");
        ASSERT_EQ(script.mScript.references.size(), 2);
        EXPECT_EQ(script.mScript.references[0], ESM::FormId::fromUint32(0x02000014));
        EXPECT_EQ(script.mScript.references[1], ESM::FormId::fromUint32(0x0215eacf));
    }

    TEST(Esm4PerkTest, acceptsBothObservedRecordDataSizes)
    {
        std::string payload = recordPrefix();
        appendSubRecord(payload, "DATA", bytes({ 0, 14, 1, 1 }));
        auto reader = makeReader(payload);
        ESM4::Perk perk;
        perk.load(*reader);

        EXPECT_EQ(perk.mData.mSerializedSize, 4);
        EXPECT_FALSE(perk.mData.mHidden.has_value());
    }

    TEST(Esm4PerkTest, rejectsMalformedSizesUnknownFieldsAndOrdering)
    {
        std::vector<std::pair<std::string, std::string>> malformed;

        const auto entryPointPrefix = [](std::uint8_t conditionTabs = 1) {
            std::string payload = recordPrefix("MalformedEntryPoint");
            appendRecordData(payload);
            appendSubRecord(payload, "PRKE", bytes({ 2, 0, 0 }));
            appendSubRecord(payload, "DATA", bytes({ 0, 1, conditionTabs }));
            return payload;
        };
        const auto finishFloatEntry = [](std::string& payload) {
            appendSubRecord(payload, "EPFT", bytes({ 1 }));
            appendSubRecord(payload, "EPFD", pod(1.f));
            appendSubRecord(payload, "PRKF", {});
        };

        std::string missingDescription;
        appendSubRecord(missingDescription, "EDID", zString("MissingDescription"));
        appendRecordData(missingDescription);
        malformed.emplace_back("missing DESC", std::move(missingDescription));

        std::string duplicateEditorId;
        appendSubRecord(duplicateEditorId, "EDID", zString("FirstEditorId"));
        appendSubRecord(duplicateEditorId, "EDID", zString("SecondEditorId"));
        appendSubRecord(duplicateEditorId, "DESC", zString("Description"));
        appendRecordData(duplicateEditorId);
        malformed.emplace_back("duplicate EDID", std::move(duplicateEditorId));

        std::string editorIdAfterFull;
        appendSubRecord(editorIdAfterFull, "EDID", zString("FirstEditorId"));
        appendSubRecord(editorIdAfterFull, "FULL", zString("Name"));
        appendSubRecord(editorIdAfterFull, "EDID", zString("LateEditorId"));
        appendSubRecord(editorIdAfterFull, "DESC", zString("Description"));
        appendRecordData(editorIdAfterFull);
        malformed.emplace_back("EDID after FULL", std::move(editorIdAfterFull));

        std::string descriptionBeforeEditorId;
        appendSubRecord(descriptionBeforeEditorId, "DESC", zString("EarlyDescription"));
        appendSubRecord(descriptionBeforeEditorId, "EDID", zString("LateEditorId"));
        appendRecordData(descriptionBeforeEditorId);
        malformed.emplace_back("DESC before EDID", std::move(descriptionBeforeEditorId));

        std::string duplicateDescription = recordPrefix();
        appendSubRecord(duplicateDescription, "DESC", zString("SecondDescription"));
        appendRecordData(duplicateDescription);
        malformed.emplace_back("duplicate DESC", std::move(duplicateDescription));

        std::string duplicateRecordData = recordPrefix();
        appendRecordData(duplicateRecordData);
        appendRecordData(duplicateRecordData);
        malformed.emplace_back("duplicate record DATA", std::move(duplicateRecordData));

        std::string unknown = recordPrefix();
        appendRecordData(unknown);
        appendSubRecord(unknown, "MICO", zString("unproven.dds"));
        malformed.emplace_back("unknown MICO", std::move(unknown));

        std::string badRecordData = recordPrefix();
        appendSubRecord(badRecordData, "DATA", bytes({ 0, 1, 1 }));
        malformed.emplace_back("short record DATA", std::move(badRecordData));

        std::string badConditionSize = recordPrefix();
        std::string shortCondition = condition(ESM4::FUN_GetDead);
        shortCondition.pop_back();
        appendSubRecord(badConditionSize, "CTDA", shortCondition);
        appendRecordData(badConditionSize);
        malformed.emplace_back("short CTDA", std::move(badConditionSize));

        std::string conditionAfterData = recordPrefix();
        appendRecordData(conditionAfterData);
        appendSubRecord(conditionAfterData, "CTDA", condition(ESM4::FUN_GetDead));
        malformed.emplace_back("record CTDA after DATA", std::move(conditionAfterData));

        std::string badQuest = recordPrefix();
        appendRecordData(badQuest);
        appendSubRecord(badQuest, "PRKE", bytes({ 0, 0, 0 }));
        appendSubRecord(badQuest, "DATA", pod(std::uint32_t{ 0x1234 }));
        appendSubRecord(badQuest, "PRKF", {});
        malformed.emplace_back("quest DATA4", std::move(badQuest));

        std::string zeroQuest = recordPrefix();
        appendRecordData(zeroQuest);
        appendSubRecord(zeroQuest, "PRKE", bytes({ 0, 0, 0 }));
        appendSubRecord(zeroQuest, "DATA", questData(0, 10, { 0, 0, 0 }));
        appendSubRecord(zeroQuest, "PRKF", {});
        malformed.emplace_back("zero quest FormID", std::move(zeroQuest));

        std::string zeroAbility = recordPrefix();
        appendRecordData(zeroAbility);
        appendSubRecord(zeroAbility, "PRKE", bytes({ 1, 0, 0 }));
        appendSubRecord(zeroAbility, "DATA", pod(std::uint32_t{ 0 }));
        appendSubRecord(zeroAbility, "PRKF", {});
        malformed.emplace_back("zero ability FormID", std::move(zeroAbility));

        std::string emptyConditionGroup = recordPrefix();
        appendRecordData(emptyConditionGroup);
        appendSubRecord(emptyConditionGroup, "PRKE", bytes({ 2, 0, 0 }));
        appendSubRecord(emptyConditionGroup, "DATA", bytes({ 0, 1, 1 }));
        appendSubRecord(emptyConditionGroup, "PRKC", bytes({ 0 }));
        appendSubRecord(emptyConditionGroup, "EPFT", bytes({ 1 }));
        appendSubRecord(emptyConditionGroup, "EPFD", pod(1.f));
        appendSubRecord(emptyConditionGroup, "PRKF", {});
        malformed.emplace_back("empty PRKC group", std::move(emptyConditionGroup));

        std::string outOfRangeTab = recordPrefix();
        appendRecordData(outOfRangeTab);
        appendSubRecord(outOfRangeTab, "PRKE", bytes({ 2, 0, 0 }));
        appendSubRecord(outOfRangeTab, "DATA", bytes({ 0, 1, 1 }));
        appendSubRecord(outOfRangeTab, "PRKC", bytes({ 1 }));
        appendSubRecord(outOfRangeTab, "CTDA", condition(ESM4::FUN_GetDead));
        appendSubRecord(outOfRangeTab, "EPFT", bytes({ 1 }));
        appendSubRecord(outOfRangeTab, "EPFD", pod(1.f));
        appendSubRecord(outOfRangeTab, "PRKF", {});
        malformed.emplace_back("out-of-range PRKC", std::move(outOfRangeTab));

        std::string duplicateConditionTab = entryPointPrefix(2);
        appendSubRecord(duplicateConditionTab, "PRKC", bytes({ 0 }));
        appendSubRecord(duplicateConditionTab, "CTDA", condition(ESM4::FUN_GetDead));
        appendSubRecord(duplicateConditionTab, "PRKC", bytes({ 0 }));
        appendSubRecord(duplicateConditionTab, "CTDA", condition(ESM4::FUN_GetDead));
        finishFloatEntry(duplicateConditionTab);
        malformed.emplace_back("duplicate PRKC", std::move(duplicateConditionTab));

        std::string decreasingConditionTab = entryPointPrefix(3);
        appendSubRecord(decreasingConditionTab, "PRKC", bytes({ 1 }));
        appendSubRecord(decreasingConditionTab, "CTDA", condition(ESM4::FUN_GetDead));
        appendSubRecord(decreasingConditionTab, "PRKC", bytes({ 0 }));
        appendSubRecord(decreasingConditionTab, "CTDA", condition(ESM4::FUN_GetDead));
        finishFloatEntry(decreasingConditionTab);
        malformed.emplace_back("decreasing PRKC", std::move(decreasingConditionTab));

        std::string badFunctionType = recordPrefix();
        appendRecordData(badFunctionType);
        appendSubRecord(badFunctionType, "PRKE", bytes({ 2, 0, 0 }));
        appendSubRecord(badFunctionType, "DATA", bytes({ 0, 1, 1 }));
        appendSubRecord(badFunctionType, "EPFT", bytes({ 2 }));
        appendSubRecord(badFunctionType, "EPFD", pod(1.f));
        appendSubRecord(badFunctionType, "PRKF", {});
        malformed.emplace_back("unknown EPFT", std::move(badFunctionType));

        std::string nonFiniteFloat = entryPointPrefix();
        appendSubRecord(nonFiniteFloat, "EPFT", bytes({ 1 }));
        appendSubRecord(nonFiniteFloat, "EPFD", pod(std::numeric_limits<float>::infinity()));
        appendSubRecord(nonFiniteFloat, "PRKF", {});
        malformed.emplace_back("non-finite EPFD float", std::move(nonFiniteFloat));

        std::string zeroFunctionFormId = entryPointPrefix();
        appendSubRecord(zeroFunctionFormId, "EPFT", bytes({ 3 }));
        appendSubRecord(zeroFunctionFormId, "EPFD", pod(std::uint32_t{ 0 }));
        appendSubRecord(zeroFunctionFormId, "PRKF", {});
        malformed.emplace_back("zero EPFD FormID", std::move(zeroFunctionFormId));

        std::string epfdOnScript = entryPointPrefix();
        appendSubRecord(epfdOnScript, "EPFT", bytes({ 4 }));
        appendSubRecord(epfdOnScript, "EPFD", pod(1.f));
        appendSubRecord(epfdOnScript, "PRKF", {});
        malformed.emplace_back("EPFD on embedded-script branch", std::move(epfdOnScript));

        std::string scriptFieldOnFloat = entryPointPrefix();
        appendSubRecord(scriptFieldOnFloat, "EPFT", bytes({ 1 }));
        appendSubRecord(scriptFieldOnFloat, "EPF2", zString("Wrong branch"));
        appendSubRecord(scriptFieldOnFloat, "PRKF", {});
        malformed.emplace_back("script field on EPFD branch", std::move(scriptFieldOnFloat));

        malformed.emplace_back("nonzero EPF3", embeddedScriptPayload(scriptHeader(1, 4), 1));
        malformed.emplace_back("nonzero SCHR unused", embeddedScriptPayload(scriptHeader(1, 4, 1)));
        malformed.emplace_back(
            "nonzero SCHR variable count", embeddedScriptPayload(scriptHeader(1, 4, 0, 1)));
        malformed.emplace_back("nonzero SCHR type", embeddedScriptPayload(scriptHeader(1, 4, 0, 0, 1)));
        malformed.emplace_back("wrong SCHR flags", embeddedScriptPayload(scriptHeader(1, 4, 0, 0, 0, 0)));
        malformed.emplace_back("SCHR compiled-size mismatch", embeddedScriptPayload(scriptHeader(1, 5)));
        malformed.emplace_back("SCHR reference-count mismatch", embeddedScriptPayload(scriptHeader(2, 4)));
        malformed.emplace_back(
            "zero SCRO FormID", embeddedScriptPayload(scriptHeader(1, 4), 0, bytes({ 1, 2, 3, 4 }), 1, 0));

        std::string nonzeroEntryEnd = recordPrefix();
        appendRecordData(nonzeroEntryEnd);
        appendSubRecord(nonzeroEntryEnd, "PRKE", bytes({ 1, 0, 0 }));
        appendSubRecord(nonzeroEntryEnd, "PRKF", bytes({ 1 }));
        malformed.emplace_back("nonzero PRKF", std::move(nonzeroEntryEnd));

        std::string unterminated = recordPrefix();
        appendRecordData(unterminated);
        appendSubRecord(unterminated, "PRKE", bytes({ 1, 0, 0 }));
        appendSubRecord(unterminated, "DATA", pod(std::uint32_t{ 0x1234 }));
        malformed.emplace_back("missing PRKF", std::move(unterminated));

        std::string declaredOverrun = recordPrefix();
        declaredOverrun.append("DATA");
        appendPod(declaredOverrun, std::uint16_t{ 5 });
        declaredOverrun.append(bytes({ 0, 1, 1, 1 }));
        malformed.emplace_back("declared DATA payload overrun", std::move(declaredOverrun));

        for (const auto& [name, payload] : malformed)
        {
            SCOPED_TRACE(name);
            expectRejected(payload);
        }
    }
}
