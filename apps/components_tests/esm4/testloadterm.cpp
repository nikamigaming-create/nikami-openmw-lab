#include <components/esm4/loadterm.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
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

    template <class T>
    std::string pod(const T& value)
    {
        std::string result;
        appendPod(result, value);
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

    std::uint8_t hexDigit(char value)
    {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<std::uint8_t>(value - 'a' + 10);
        if (value >= 'A' && value <= 'F')
            return static_cast<std::uint8_t>(value - 'A' + 10);
        throw std::runtime_error("invalid hexadecimal fixture");
    }

    std::string fromHex(std::string_view value)
    {
        if (value.size() % 2 != 0)
            throw std::runtime_error("odd hexadecimal fixture size");
        std::string result;
        result.reserve(value.size() / 2);
        for (std::size_t i = 0; i < value.size(); i += 2)
        {
            result.push_back(static_cast<char>(
                static_cast<std::uint8_t>((hexDigit(value[i]) << 4) | hexDigit(value[i + 1]))));
        }
        return result;
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
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

    void appendDeclaredSubRecord(
        std::string& output, std::string_view type, std::uint16_t declaredSize, std::string_view data)
    {
        ASSERT_EQ(type.size(), 4);
        output.append(type);
        appendPod(output, declaredSize);
        output.append(data);
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view payload,
        std::uint32_t flags = 0)
    {
        output.append(type);
        appendPod(output, static_cast<std::uint32_t>(payload.size()));
        appendPod(output, flags);
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 });
        appendPod(output, std::uint16_t{ 0 });
        appendPod(output, std::uint16_t{ 0 });
        output.append(payload);
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& payload, float version = 1.34f,
        bool withMaster = true, std::uint32_t recordId = 0x010003e8)
    {
        std::string hedr;
        appendPod(hedr, version);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });

        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);
        if (withMaster)
        {
            appendSubRecord(headerPayload, "MAST", zString("Master.esm"));
            appendSubRecord(headerPayload, "DATA", pod(std::uint64_t{ 0 }));
        }

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload, ESM4::Rec_ESM);
        appendRecord(plugin, "TERM", recordId, payload, ESM4::Rec_Constant);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "FalloutNV.esm", nullptr, nullptr, true);
        reader->setModIndex(5);
        if (withMaster)
            reader->updateModIndices({ { "master.esm", 2 } });
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    ESM4::ScriptHeader scriptHeader(
        std::uint32_t references = 0, std::uint32_t compiledSize = 0, std::uint32_t variables = 0,
        std::uint16_t type = 0, std::uint16_t flags = 1)
    {
        return ESM4::ScriptHeader{ 0, references, compiledSize, variables, type, flags };
    }

    std::string minimalPrefix(std::string_view data = std::string_view("\0\2\5\0", 4))
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("TestTerminal"));
        appendSubRecord(payload, "OBND", std::string(12, '\0'));
        appendSubRecord(payload, "DESC", zString("Terminal text"));
        appendSubRecord(payload, "DNAM", data);
        return payload;
    }

    void appendEmptyMenu(std::string& payload, std::string_view text = "Menu Item")
    {
        appendSubRecord(payload, "ITXT", zString(text));
        appendSubRecord(payload, "RNAM", zString("Result"));
        appendSubRecord(payload, "ANAM", bytes({ 0 }));
        appendSubRecord(payload, "SCHR", pod(scriptHeader()));
    }

    void expectRejected(const std::string& payload)
    {
        auto reader = makeReader(payload);
        ESM4::Terminal terminal;
        EXPECT_THROW(terminal.load(*reader), std::runtime_error);
    }

    TEST(Esm4TerminalTest, preservesExactThreeByteRetailDataAndEveryRepeatedResult)
    {
        // Byte-exact FalloutNV.esm 0x0004d848 payload. It is one of the two
        // official records with the three-byte DNAM shape and has two RNAMs.
        std::string payload;
        appendSubRecord(payload, "EDID", zString("CGDadsTerminalExperiments"));
        appendSubRecord(payload, "OBND", std::string(12, '\0'));
        appendSubRecord(payload, "FULL", zString("Experiments"));
        appendSubRecord(payload, "DESC", zString(""));
        appendSubRecord(payload, "DNAM", bytes({ 0, 2, 2 }));

        appendSubRecord(payload, "ITXT", zString("Experiment 27CE"));
        appendSubRecord(payload, "RNAM", std::string(4, '\0'));
        appendSubRecord(payload, "ANAM", bytes({ 0 }));
        appendSubRecord(payload, "INAM", pod(std::uint32_t{ 0x0004d845 }));
        appendSubRecord(payload, "SCHR", pod(scriptHeader(0, 0, 0, 1)));

        appendSubRecord(payload, "ITXT", zString("Experiment PP216"));
        appendSubRecord(payload, "RNAM", std::string(4, '\0'));
        appendSubRecord(payload, "ANAM", bytes({ 0 }));
        appendSubRecord(payload, "INAM", pod(std::uint32_t{ 0x0004d846 }));
        appendSubRecord(payload, "SCHR", pod(scriptHeader(0, 0, 0, 1)));

        auto reader = makeReader(payload, 1.34f, false, 0x0004d848);
        ESM4::Terminal terminal;
        terminal.load(*reader);

        EXPECT_EQ(terminal.mId, ESM::FormId::fromUint32(0x0504d848));
        EXPECT_EQ(terminal.mFlags, ESM4::Rec_Constant);
        EXPECT_EQ(terminal.mEditorId, "CGDadsTerminalExperiments");
        EXPECT_EQ(terminal.mFullName, "Experiments");
        EXPECT_TRUE(terminal.mText.empty());
        EXPECT_EQ(terminal.mData.mSerializedSize, 3);
        EXPECT_EQ(terminal.mData.mBytes, (std::array<std::uint8_t, 4>{ 0, 2, 2, 0 }));

        ASSERT_EQ(terminal.mMenuItems.size(), 2);
        EXPECT_EQ(terminal.mMenuItems[0].mText, "Experiment 27CE");
        EXPECT_EQ(terminal.mMenuItems[0].mResultText, std::string(3, '\0'));
        ASSERT_TRUE(terminal.mMenuItems[0].mDisplayNote.has_value());
        EXPECT_EQ(*terminal.mMenuItems[0].mDisplayNote, ESM::FormId::fromUint32(0x0504d845));
        EXPECT_EQ(terminal.mMenuItems[0].mScript.scriptHeader.type, 1);
        EXPECT_EQ(terminal.mMenuItems[1].mText, "Experiment PP216");
        EXPECT_EQ(terminal.mMenuItems[1].mResultText, std::string(3, '\0'));
        ASSERT_TRUE(terminal.mMenuItems[1].mDisplayNote.has_value());
        EXPECT_EQ(*terminal.mMenuItems[1].mDisplayNote, ESM::FormId::fromUint32(0x0504d846));
        EXPECT_EQ(terminal.mResultText, terminal.mMenuItems[1].mResultText);
    }

    TEST(Esm4TerminalTest, preservesExactRetailBytecodeSourceLocalsReferencesAndConditions)
    {
        // Top-level fields from DeadMoney.esm 0x01003514, followed by three
        // exact retail menu fragments that cover source/bytecode, SCRO, and
        // the two TERM-specific FormID-bearing CTDA functions.
        std::string payload;
        appendSubRecord(payload, "EDID", zString("NVDLC01HoloVaultTerminalA"));
        appendSubRecord(payload, "OBND", fromHex("f4ffeeffe9ff340016001300"));
        appendSubRecord(payload, "FULL", zString("Hologram Control"));
        appendSubRecord(payload, "MODL", zString("terminals\\terminal01.nif"));
        appendSubRecord(payload, "SCRI", pod(std::uint32_t{ 0x010000d5 }));
        appendSubRecord(payload, "DESC", zString("Sierra Madre Security Network\r\n"));
        appendSubRecord(payload, "DNAM", fromHex("00020800"));

        appendSubRecord(payload, "ITXT", zString("Check Security Hologram Status"));
        appendSubRecord(payload, "RNAM", zString("Currently patrolling default route."));
        appendSubRecord(payload, "ANAM", bytes({ 2 }));
        appendSubRecord(payload, "SCHR", fromHex("0000000000000000000000000000000000000100"));
        appendSubRecord(payload, "CTDA", fromHex("000000000000000035000000f1290001180000000000000000000000"));

        appendSubRecord(payload, "ITXT", zString("Set Behavior: Alternate Route Patrol"));
        appendSubRecord(payload, "RNAM", zString("Uploading new behavior pattern..."));
        appendSubRecord(payload, "ANAM", bytes({ 2 }));
        appendSubRecord(payload, "SCHR", fromHex("00000000030000001f0000000000000000000100"));
        const std::string bytecode
            = fromHex("15000a00720200731800020020321c000100fa110000261005000100720300");
        appendSubRecord(payload, "SCDA", bytecode);
        const std::string source
            = "Set HoloTestCodeBox.bAPatrol to 2\r\nHoloGalREF.ResetAI\r\nPlaySound OBJNVDLC01HologramSwitch";
        appendSubRecord(payload, "SCTX", source);
        appendSubRecord(payload, "SCRO", pod(std::uint32_t{ 0x010029f0 }));
        appendSubRecord(payload, "SCRO", pod(std::uint32_t{ 0x010029f1 }));
        appendSubRecord(payload, "SCRO", pod(std::uint32_t{ 0x01012f50 }));
        appendSubRecord(payload, "CTDA", fromHex("200000000000004035000000f1290001180000000000000000000000"));

        appendSubRecord(payload, "ITXT", zString("System Information"));
        appendSubRecord(payload, "RNAM", zString("Loading LOG file..."));
        appendSubRecord(payload, "ANAM", bytes({ 0 }));
        appendSubRecord(payload, "INAM", pod(std::uint32_t{ 0x000af8ce }));
        appendSubRecord(payload, "SCHR", fromHex("0000000000000000000000000000000000000100"));
        appendSubRecord(payload, "CTDA", fromHex("600000000000803f4900000076140200000000000400000000000000"));

        auto reader = makeReader(payload, 1.32f, true, 0x01003514);
        ESM4::Terminal terminal;
        terminal.load(*reader);

        EXPECT_EQ(terminal.mId, ESM::FormId::fromUint32(0x05003514));
        EXPECT_EQ(terminal.mScriptId, ESM::FormId::fromUint32(0x050000d5));
        ASSERT_EQ(terminal.mMenuItems.size(), 3);

        const ESM4::Terminal::MenuItem& conditionItem = terminal.mMenuItems[0];
        ASSERT_EQ(conditionItem.mConditions.size(), 1);
        EXPECT_EQ(conditionItem.mConditions[0].functionIndex, ESM4::FUN_GetScriptVariable);
        EXPECT_EQ(conditionItem.mConditions[0].param1, 0x050029f1u);

        const ESM4::Terminal::MenuItem& scriptItem = terminal.mMenuItems[1];
        EXPECT_EQ(scriptItem.mScript.compiledData,
            (std::vector<std::uint8_t>(bytecode.begin(), bytecode.end())));
        EXPECT_EQ(scriptItem.mScript.scriptSource, source);
        ASSERT_EQ(scriptItem.mScript.references.size(), 3);
        EXPECT_EQ(scriptItem.mScript.references[0], ESM::FormId::fromUint32(0x050029f0));
        EXPECT_EQ(scriptItem.mScript.references[1], ESM::FormId::fromUint32(0x050029f1));
        EXPECT_EQ(scriptItem.mScript.references[2], ESM::FormId::fromUint32(0x05012f50));

        const ESM4::Terminal::MenuItem& factionItem = terminal.mMenuItems[2];
        ASSERT_TRUE(factionItem.mDisplayNote.has_value());
        EXPECT_EQ(*factionItem.mDisplayNote, ESM::FormId::fromUint32(0x020af8ce));
        ASSERT_EQ(factionItem.mConditions.size(), 1);
        EXPECT_EQ(factionItem.mConditions[0].functionIndex, ESM4::FUN_GetFactionRank);
        EXPECT_EQ(factionItem.mConditions[0].param1, 0x02021476u);
    }

    TEST(Esm4TerminalTest, preservesRawModelVariantsTopLinksAndLocalVariableMetadata)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("RawTopLevelTerminal"));
        appendSubRecord(payload, "OBND", fromHex("0102030405060708090a0b0c"));
        appendSubRecord(payload, "FULL", zString("Raw terminal"));
        appendSubRecord(payload, "MODL", zString("terminals\\terminal01.nif"));
        const std::string modelData(96, static_cast<char>(0xa5));
        appendSubRecord(payload, "MODT", modelData);
        appendSubRecord(payload, "SCRI", pod(std::uint32_t{ 0x00001234 }));
        appendSubRecord(payload, "DESC", zString("Text"));
        appendSubRecord(payload, "SNAM", pod(std::uint32_t{ 0x01002345 }));
        appendSubRecord(payload, "PNAM", pod(std::uint32_t{ 0x00003456 }));
        appendSubRecord(payload, "DNAM", fromHex("03010500"));
        appendSubRecord(payload, "ITXT", zString("Run"));
        appendSubRecord(payload, "RNAM", zString("Running"));
        appendSubRecord(payload, "ANAM", bytes({ 3 }));
        appendSubRecord(payload, "TNAM", pod(std::uint32_t{ 0x01004567 }));
        appendSubRecord(payload, "SCHR", pod(scriptHeader(2, 4, 1)));
        appendSubRecord(payload, "SCDA", fromHex("23120000"));
        appendSubRecord(payload, "SCTX", std::string_view("\r\n", 2));
        appendSubRecord(payload, "SLSD", fromHex("010000000200000003000000040000000500000006000000"));
        appendSubRecord(payload, "SCVR", zString("local"));
        appendSubRecord(payload, "SCRV", pod(std::uint32_t{ 1 }));
        appendSubRecord(payload, "SCRO", pod(std::uint32_t{ 0x01005678 }));

        auto reader = makeReader(payload);
        ESM4::Terminal terminal;
        terminal.load(*reader);

        EXPECT_EQ(terminal.mObjectBounds,
            (std::array<std::uint8_t, 12>{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }));
        EXPECT_EQ(terminal.mModelData, (std::vector<std::uint8_t>(96, 0xa5)));
        EXPECT_EQ(terminal.mScriptId, ESM::FormId::fromUint32(0x02001234));
        EXPECT_EQ(terminal.mSound, ESM::FormId::fromUint32(0x05002345));
        EXPECT_EQ(terminal.mPasswordNote, ESM::FormId::fromUint32(0x02003456));
        ASSERT_EQ(terminal.mMenuItems.size(), 1);
        const auto& item = terminal.mMenuItems[0];
        ASSERT_TRUE(item.mSubmenu.has_value());
        EXPECT_EQ(*item.mSubmenu, ESM::FormId::fromUint32(0x05004567));
        ASSERT_EQ(item.mScript.localVarData.size(), 1);
        EXPECT_EQ(item.mScript.localVarData[0].index, 1u);
        EXPECT_EQ(item.mScript.localVarData[0].unknown1, 2u);
        EXPECT_EQ(item.mScript.localVarData[0].unknown2, 3u);
        EXPECT_EQ(item.mScript.localVarData[0].unknown3, 4u);
        EXPECT_EQ(item.mScript.localVarData[0].type, 5u);
        EXPECT_EQ(item.mScript.localVarData[0].unknown4, 6u);
        EXPECT_EQ(item.mScript.localVarData[0].variableName, "local");
        EXPECT_EQ(item.mScript.localRefVarIndex, (std::vector<std::uint32_t>{ 1 }));
        EXPECT_EQ(item.mScript.references,
            (std::vector<ESM::FormId>{ ESM::FormId::fromUint32(0x05005678) }));

        std::string swapsPayload;
        appendSubRecord(swapsPayload, "EDID", zString("TextureSwapTerminal"));
        appendSubRecord(swapsPayload, "OBND", std::string(12, '\0'));
        appendSubRecord(swapsPayload, "MODL", zString("terminals\\terminal01.nif"));
        const std::string swaps(37, static_cast<char>(0x5a));
        appendSubRecord(swapsPayload, "MODS", swaps);
        appendSubRecord(swapsPayload, "DESC", zString("Text"));
        appendSubRecord(swapsPayload, "DNAM", fromHex("00020500"));
        auto swapsReader = makeReader(swapsPayload);
        ESM4::Terminal swapsTerminal;
        swapsTerminal.load(*swapsReader);
        EXPECT_EQ(swapsTerminal.mModelTextureSwaps, (std::vector<std::uint8_t>(37, 0x5a)));
    }

    TEST(Esm4TerminalTest, rejectsMalformedFixedSizesAndUnprovenValues)
    {
        std::vector<std::pair<std::string, std::string>> malformed;

        for (const std::size_t size : { std::size_t{ 11 }, std::size_t{ 13 } })
        {
            std::string payload;
            appendSubRecord(payload, "EDID", zString("BadBounds"));
            appendSubRecord(payload, "OBND", std::string(size, '\0'));
            malformed.emplace_back("OBND", std::move(payload));
        }
        for (const std::size_t size : { std::size_t{ 2 }, std::size_t{ 5 } })
        {
            std::string payload = minimalPrefix();
            payload.clear();
            appendSubRecord(payload, "EDID", zString("BadData"));
            appendSubRecord(payload, "OBND", std::string(12, '\0'));
            appendSubRecord(payload, "DESC", zString("Text"));
            appendSubRecord(payload, "DNAM", std::string(size, '\0'));
            malformed.emplace_back("DNAM", std::move(payload));
        }

        std::string badFlags = minimalPrefix();
        appendSubRecord(badFlags, "ITXT", zString("Item"));
        appendSubRecord(badFlags, "RNAM", zString("Result"));
        appendSubRecord(badFlags, "ANAM", bytes({ 4 }));
        malformed.emplace_back("ANAM value", std::move(badFlags));

        for (const std::size_t size : { std::size_t{ 19 }, std::size_t{ 21 } })
        {
            std::string payload = minimalPrefix();
            appendSubRecord(payload, "ITXT", zString("Item"));
            appendSubRecord(payload, "RNAM", zString("Result"));
            appendSubRecord(payload, "ANAM", bytes({ 0 }));
            appendSubRecord(payload, "SCHR", std::string(size, '\0'));
            malformed.emplace_back("SCHR", std::move(payload));
        }

        for (const auto& [type, size] : { std::pair{ "INAM", 3 }, std::pair{ "TNAM", 5 } })
        {
            std::string payload = minimalPrefix();
            appendSubRecord(payload, "ITXT", zString("Item"));
            appendSubRecord(payload, "RNAM", zString("Result"));
            appendSubRecord(payload, "ANAM", bytes({ 0 }));
            appendSubRecord(payload, type, std::string(size, '\0'));
            malformed.emplace_back(type, std::move(payload));
        }

        std::string badCondition = minimalPrefix();
        appendEmptyMenu(badCondition);
        appendSubRecord(badCondition, "CTDA", std::string(27, '\0'));
        malformed.emplace_back("CTDA", std::move(badCondition));

        std::string badLocal = minimalPrefix();
        appendSubRecord(badLocal, "ITXT", zString("Item"));
        appendSubRecord(badLocal, "RNAM", zString("Result"));
        appendSubRecord(badLocal, "ANAM", bytes({ 0 }));
        appendSubRecord(badLocal, "SCHR", pod(scriptHeader(0, 0, 1)));
        appendSubRecord(badLocal, "SCTX", std::string_view("x", 1));
        appendSubRecord(badLocal, "SLSD", std::string(23, '\0'));
        malformed.emplace_back("SLSD", std::move(badLocal));

        for (const auto& [name, payload] : malformed)
        {
            SCOPED_TRACE(name);
            expectRejected(payload);
        }
    }

    TEST(Esm4TerminalTest, rejectsIncompleteDuplicateAndOutOfOrderFnvGrammar)
    {
        std::vector<std::pair<std::string, std::string>> malformed;
        malformed.emplace_back("empty", std::string{});

        std::string missingBounds;
        appendSubRecord(missingBounds, "EDID", zString("MissingBounds"));
        appendSubRecord(missingBounds, "DESC", zString("Text"));
        appendSubRecord(missingBounds, "DNAM", fromHex("00020500"));
        malformed.emplace_back("missing OBND", std::move(missingBounds));

        std::string duplicateEditor = minimalPrefix();
        appendSubRecord(duplicateEditor, "EDID", zString("Duplicate"));
        malformed.emplace_back("duplicate EDID", std::move(duplicateEditor));

        std::string beforeData;
        appendSubRecord(beforeData, "EDID", zString("BeforeData"));
        appendSubRecord(beforeData, "OBND", std::string(12, '\0'));
        appendSubRecord(beforeData, "DESC", zString("Text"));
        appendSubRecord(beforeData, "ITXT", zString("Item"));
        malformed.emplace_back("ITXT before DNAM", std::move(beforeData));

        std::string conflictingLinks = minimalPrefix();
        appendSubRecord(conflictingLinks, "ITXT", zString("Item"));
        appendSubRecord(conflictingLinks, "RNAM", zString("Result"));
        appendSubRecord(conflictingLinks, "ANAM", bytes({ 0 }));
        appendSubRecord(conflictingLinks, "INAM", pod(std::uint32_t{ 0x01001234 }));
        appendSubRecord(conflictingLinks, "TNAM", pod(std::uint32_t{ 0x01005678 }));
        malformed.emplace_back("INAM and TNAM", std::move(conflictingLinks));

        std::string missingHeader = minimalPrefix();
        appendSubRecord(missingHeader, "ITXT", zString("Item"));
        appendSubRecord(missingHeader, "RNAM", zString("Result"));
        appendSubRecord(missingHeader, "ANAM", bytes({ 0 }));
        malformed.emplace_back("missing SCHR", std::move(missingHeader));

        std::string bytecodeWithoutSource = minimalPrefix();
        appendSubRecord(bytecodeWithoutSource, "ITXT", zString("Item"));
        appendSubRecord(bytecodeWithoutSource, "RNAM", zString("Result"));
        appendSubRecord(bytecodeWithoutSource, "ANAM", bytes({ 0 }));
        appendSubRecord(bytecodeWithoutSource, "SCHR", pod(scriptHeader(0, 4)));
        appendSubRecord(bytecodeWithoutSource, "SCDA", fromHex("23120000"));
        malformed.emplace_back("SCDA without SCTX", std::move(bytecodeWithoutSource));

        std::string mismatchedBytecode = minimalPrefix();
        appendSubRecord(mismatchedBytecode, "ITXT", zString("Item"));
        appendSubRecord(mismatchedBytecode, "RNAM", zString("Result"));
        appendSubRecord(mismatchedBytecode, "ANAM", bytes({ 0 }));
        appendSubRecord(mismatchedBytecode, "SCHR", pod(scriptHeader(0, 5)));
        appendSubRecord(mismatchedBytecode, "SCDA", fromHex("23120000"));
        malformed.emplace_back("SCHR SCDA mismatch", std::move(mismatchedBytecode));

        std::string orphanLocalName = minimalPrefix();
        appendSubRecord(orphanLocalName, "ITXT", zString("Item"));
        appendSubRecord(orphanLocalName, "RNAM", zString("Result"));
        appendSubRecord(orphanLocalName, "ANAM", bytes({ 0 }));
        appendSubRecord(orphanLocalName, "SCHR", pod(scriptHeader()));
        appendSubRecord(orphanLocalName, "SCTX", std::string_view("x", 1));
        appendSubRecord(orphanLocalName, "SCVR", zString("orphan"));
        malformed.emplace_back("orphan SCVR", std::move(orphanLocalName));

        std::string duplicateHeader = minimalPrefix();
        appendEmptyMenu(duplicateHeader);
        appendSubRecord(duplicateHeader, "SCHR", pod(scriptHeader()));
        malformed.emplace_back("duplicate SCHR", std::move(duplicateHeader));

        std::string unknown = minimalPrefix();
        appendSubRecord(unknown, "JUNK", {});
        malformed.emplace_back("unknown", std::move(unknown));

        for (const auto& [name, payload] : malformed)
        {
            SCOPED_TRACE(name);
            expectRejected(payload);
        }
    }

    TEST(Esm4TerminalTest, rejectsTruncatedStringsAndLeavesExistingValueAtomic)
    {
        std::string valid = minimalPrefix();
        appendEmptyMenu(valid, "Stable item");
        auto validReader = makeReader(valid);
        ESM4::Terminal terminal;
        terminal.load(*validReader);
        ASSERT_EQ(terminal.mMenuItems.size(), 1);
        ASSERT_EQ(terminal.mMenuItems[0].mText, "Stable item");

        std::string malformed;
        appendSubRecord(malformed, "EDID", zString("Replacement"));
        appendSubRecord(malformed, "OBND", std::string(12, '\0'));
        appendSubRecord(malformed, "DESC", zString("Text"));
        appendSubRecord(malformed, "DNAM", fromHex("00020500"));
        appendDeclaredSubRecord(malformed, "ITXT", 8, std::string_view("short", 5));
        auto malformedReader = makeReader(malformed);
        EXPECT_THROW(terminal.load(*malformedReader), std::runtime_error);

        EXPECT_EQ(terminal.mEditorId, "TestTerminal");
        ASSERT_EQ(terminal.mMenuItems.size(), 1);
        EXPECT_EQ(terminal.mMenuItems[0].mText, "Stable item");

        std::string unterminated = minimalPrefix();
        appendSubRecord(unterminated, "ITXT", std::string_view("not-null", 8));
        expectRejected(unterminated);
    }

    TEST(Esm4TerminalTest, retainsLegacyPermissiveBehaviorOutsideFnvVersions)
    {
        std::string payload;
        appendSubRecord(payload, "RNAM", zString("Legacy result"));
        appendSubRecord(payload, "DNAM", std::string(2, '\0'));
        appendSubRecord(payload, "ITXT", zString("Ignored legacy item"));
        appendSubRecord(payload, "EDID", zString("LegacyTerminal"));

        auto reader = makeReader(payload, 0.94f);
        ESM4::Terminal terminal;
        EXPECT_NO_THROW(terminal.load(*reader));
        EXPECT_EQ(terminal.mEditorId, "LegacyTerminal");
        EXPECT_EQ(terminal.mResultText, "Legacy result");
        EXPECT_TRUE(terminal.mMenuItems.empty());
    }
}
