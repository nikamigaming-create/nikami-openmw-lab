#include <components/esm4/loadlvli.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

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

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& payload, float version = 1.34f)
    {
        std::string hedr;
        appendPod(hedr, version);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });

        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);
        appendSubRecord(headerPayload, "MAST", zString("Master.esm"));
        appendSubRecord(headerPayload, "DATA", pod(std::uint64_t{ 0 }));

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload, ESM4::Rec_ESM);
        appendRecord(plugin, "LVLI", 0x010003e8, payload, ESM4::Rec_Constant);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "FalloutNV.esm", nullptr, nullptr, true);
        reader->setModIndex(5);
        reader->updateModIndices({ { "master.esm", 2 } });
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    ESM4::LVLO levelledEntry(
        std::int16_t level, ESM::FormId32 item, std::int16_t count, std::uint16_t first, std::uint16_t second)
    {
        return ESM4::LVLO{ level, first, item, count, second };
    }

    std::string editorPrefix()
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("TestLevelledItem"));
        return payload;
    }

    std::string chancePrefix()
    {
        std::string payload = editorPrefix();
        appendSubRecord(payload, "LVLD", bytes({ 0 }));
        return payload;
    }

    std::string flagsPrefix()
    {
        std::string payload = chancePrefix();
        appendSubRecord(payload, "LVLF", bytes({ 0 }));
        return payload;
    }

    std::string entryPayload(const ESM4::LVLO& entry)
    {
        std::string payload = flagsPrefix();
        appendSubRecord(payload, "LVLO", pod(entry));
        return payload;
    }

    void expectRejected(const std::string& payload)
    {
        auto reader = makeReader(payload);
        ESM4::LevelledItem item;
        EXPECT_THROW(item.load(*reader), std::runtime_error);
    }

    TEST(Esm4LevelledItemTest, preservesExactFnvEntryMetadataAndAdjustsOnlyFormIds)
    {
        const ESM4::LVLO first = levelledEntry(3, 0x0000abcd, 2, 0x1111, 0x2222);
        const ESM4::LVLO second = levelledEntry(12, 0x01004567, 5, 0x3333, 0x4444);
        const ESM4::LevelledItemExtraData extra{ 0x0000789a, 0x0100beef, 0.625f };

        std::string payload;
        appendSubRecord(payload, "EDID", zString("ByteExactLevelledItem"));
        appendSubRecord(payload, "OBND", std::string(12, '\0'));
        appendSubRecord(payload, "LVLD", bytes({ 25 }));
        appendSubRecord(payload, "LVLF", bytes({ 0x84 }));
        appendSubRecord(payload, "LVLG", pod(std::uint32_t{ 0x01001234 }));
        appendSubRecord(payload, "LVLO", pod(first));
        appendSubRecord(payload, "LVLO", pod(second));
        appendSubRecord(payload, "COED", pod(extra));

        auto reader = makeReader(payload);
        ESM4::LevelledItem item;
        item.load(*reader);

        EXPECT_EQ(item.mId, ESM::FormId::fromUint32(0x050003e8));
        EXPECT_EQ(item.mFlags, ESM4::Rec_Constant);
        EXPECT_EQ(item.mEditorId, "ByteExactLevelledItem");
        EXPECT_TRUE(item.mHasChanceNone);
        EXPECT_EQ(item.mChanceNone, 25);
        EXPECT_TRUE(item.mHasLvlItemFlags);
        EXPECT_EQ(item.mLvlItemFlags, 0x84);
        EXPECT_EQ(item.mChanceGlobal, ESM::FormId::fromUint32(0x05001234));

        ASSERT_EQ(item.mLvlObject.size(), 2);
        EXPECT_EQ(item.mLvlObject[0].level, 3);
        EXPECT_EQ(item.mLvlObject[0].unknown, 0x1111);
        EXPECT_EQ(item.mLvlObject[0].item, 0x0200abcdu);
        EXPECT_EQ(item.mLvlObject[0].count, 2);
        EXPECT_EQ(item.mLvlObject[0].unknown2, 0x2222);
        EXPECT_EQ(item.mLvlObject[1].item, 0x05004567u);

        ASSERT_EQ(item.mLvlObjectExtra.size(), item.mLvlObject.size());
        EXPECT_FALSE(item.mLvlObjectExtra[0].has_value());
        ASSERT_TRUE(item.mLvlObjectExtra[1].has_value());
        EXPECT_EQ(item.mLvlObjectExtra[1]->mOwner, 0x0200789au);
        EXPECT_EQ(item.mLvlObjectExtra[1]->mGlobalOrRequiredRank, 0x0100beefu);
        EXPECT_FLOAT_EQ(item.mLvlObjectExtra[1]->mItemCondition, 0.625f);
    }

    TEST(Esm4LevelledItemTest, resetsAllPreservedStateBeforeReload)
    {
        std::string firstPayload;
        appendSubRecord(firstPayload, "EDID", zString("FirstLoad"));
        appendSubRecord(firstPayload, "LVLD", bytes({ 50 }));
        appendSubRecord(firstPayload, "LVLF", bytes({ 1 }));
        appendSubRecord(firstPayload, "LVLG", pod(std::uint32_t{ 0x01001234 }));
        appendSubRecord(firstPayload, "LVLO", pod(levelledEntry(1, 0x01004567, 1, 0, 0)));
        appendSubRecord(firstPayload, "COED", pod(ESM4::LevelledItemExtraData{ 0x0000789a, 7, 0.5f }));

        ESM4::LevelledItem item;
        auto firstReader = makeReader(firstPayload);
        item.load(*firstReader);

        auto secondReader = makeReader({}, 0.94f);
        item.load(*secondReader);
        EXPECT_TRUE(item.mEditorId.empty());
        EXPECT_FALSE(item.mHasChanceNone);
        EXPECT_EQ(item.mChanceNone, 0);
        EXPECT_TRUE(item.mChanceGlobal.isZeroOrUnset());
        EXPECT_FALSE(item.mHasLvlItemFlags);
        EXPECT_EQ(item.mLvlItemFlags, 0);
        EXPECT_EQ(item.mData, 0);
        EXPECT_TRUE(item.mLvlObject.empty());
        EXPECT_TRUE(item.mLvlObjectExtra.empty());
    }

    TEST(Esm4LevelledItemTest, acceptsFrozenZeroEntryShape)
    {
        auto reader = makeReader(flagsPrefix());
        ESM4::LevelledItem item;
        EXPECT_NO_THROW(item.load(*reader));
        EXPECT_TRUE(item.mHasChanceNone);
        EXPECT_TRUE(item.mHasLvlItemFlags);
        EXPECT_TRUE(item.mLvlObject.empty());
        EXPECT_TRUE(item.mLvlObjectExtra.empty());
    }

    TEST(Esm4LevelledItemTest, rejectsMalformedFnvSubrecordSizes)
    {
        const ESM4::LVLO entry = levelledEntry(1, 0x01004567, 1, 0, 0);
        const ESM4::LevelledItemExtraData extra{ 0x0000789a, 7, 0.5f };
        std::vector<std::pair<std::string, std::string>> malformed;

        for (const std::string_view value : { std::string_view{}, std::string_view{ "\0\0", 2 } })
        {
            std::string payload = editorPrefix();
            appendSubRecord(payload, "LVLD", value);
            malformed.emplace_back("LVLD", std::move(payload));
        }
        for (const std::string_view value : { std::string_view{}, std::string_view{ "\0\0", 2 } })
        {
            std::string payload = chancePrefix();
            appendSubRecord(payload, "LVLF", value);
            malformed.emplace_back("LVLF", std::move(payload));
        }
        for (const std::size_t size : { std::size_t{ 8 }, std::size_t{ 13 } })
        {
            std::string payload = flagsPrefix();
            appendSubRecord(payload, "LVLO", std::string(size, '\0'));
            malformed.emplace_back("LVLO", std::move(payload));
        }
        for (const std::size_t size : { std::size_t{ 3 }, std::size_t{ 5 } })
        {
            std::string payload = flagsPrefix();
            appendSubRecord(payload, "LVLG", std::string(size, '\0'));
            malformed.emplace_back("LVLG", std::move(payload));
        }
        for (const std::size_t size : { std::size_t{ 11 }, std::size_t{ 13 } })
        {
            std::string payload = editorPrefix();
            appendSubRecord(payload, "OBND", std::string(size, '\0'));
            malformed.emplace_back("OBND", std::move(payload));
        }
        for (const std::size_t size : { sizeof(extra) - 1, sizeof(extra) + 1 })
        {
            std::string payload = entryPayload(entry);
            appendSubRecord(payload, "COED", std::string(size, '\0'));
            malformed.emplace_back("COED", std::move(payload));
        }

        for (const auto& [name, payload] : malformed)
        {
            SCOPED_TRACE(name);
            expectRejected(payload);
        }
    }

    TEST(Esm4LevelledItemTest, rejectsOrphanSeparatedAndDuplicateFnvCoed)
    {
        const ESM4::LVLO entry = levelledEntry(1, 0x01004567, 1, 0, 0);
        const ESM4::LevelledItemExtraData extra{ 0x0000789a, 7, 0.5f };
        std::vector<std::pair<std::string, std::string>> malformed;

        std::string orphan;
        appendSubRecord(orphan, "COED", pod(extra));
        malformed.emplace_back("orphan", std::move(orphan));

        std::string separated = entryPayload(entry);
        appendSubRecord(separated, "LVLF", bytes({ 0 }));
        appendSubRecord(separated, "COED", pod(extra));
        malformed.emplace_back("separated", std::move(separated));

        std::string duplicate = entryPayload(entry);
        appendSubRecord(duplicate, "COED", pod(extra));
        appendSubRecord(duplicate, "COED", pod(extra));
        malformed.emplace_back("duplicate", std::move(duplicate));

        for (const auto& [name, payload] : malformed)
        {
            SCOPED_TRACE(name);
            expectRejected(payload);
        }
    }

    TEST(Esm4LevelledItemTest, rejectsIncompleteDuplicateOutOfOrderAndUnprovenFnvGrammar)
    {
        std::vector<std::pair<std::string, std::string>> malformed;

        malformed.emplace_back("empty", std::string{});
        malformed.emplace_back("missing LVLD and LVLF", editorPrefix());
        malformed.emplace_back("missing LVLF", chancePrefix());

        std::string emptyEditorId;
        appendSubRecord(emptyEditorId, "EDID", {});
        appendSubRecord(emptyEditorId, "LVLD", bytes({ 0 }));
        appendSubRecord(emptyEditorId, "LVLF", bytes({ 0 }));
        malformed.emplace_back("empty EDID payload", std::move(emptyEditorId));

        std::string duplicateEditorId = editorPrefix();
        appendSubRecord(duplicateEditorId, "EDID", zString("Duplicate"));
        malformed.emplace_back("duplicate EDID", std::move(duplicateEditorId));

        std::string duplicateBounds = editorPrefix();
        appendSubRecord(duplicateBounds, "OBND", std::string(12, '\0'));
        appendSubRecord(duplicateBounds, "OBND", std::string(12, '\0'));
        malformed.emplace_back("duplicate OBND", std::move(duplicateBounds));

        std::string duplicateChance = chancePrefix();
        appendSubRecord(duplicateChance, "LVLD", bytes({ 0 }));
        malformed.emplace_back("duplicate LVLD", std::move(duplicateChance));

        std::string duplicateFlags = flagsPrefix();
        appendSubRecord(duplicateFlags, "LVLF", bytes({ 0 }));
        malformed.emplace_back("duplicate LVLF", std::move(duplicateFlags));

        std::string duplicateGlobal = flagsPrefix();
        appendSubRecord(duplicateGlobal, "LVLG", pod(std::uint32_t{ 0x01001234 }));
        appendSubRecord(duplicateGlobal, "LVLG", pod(std::uint32_t{ 0x01005678 }));
        malformed.emplace_back("duplicate LVLG", std::move(duplicateGlobal));

        std::string boundsBeforeEditorId;
        appendSubRecord(boundsBeforeEditorId, "OBND", std::string(12, '\0'));
        malformed.emplace_back("OBND before EDID", std::move(boundsBeforeEditorId));

        std::string flagsBeforeChance = editorPrefix();
        appendSubRecord(flagsBeforeChance, "LVLF", bytes({ 0 }));
        malformed.emplace_back("LVLF before LVLD", std::move(flagsBeforeChance));

        std::string globalBeforeFlags = chancePrefix();
        appendSubRecord(globalBeforeFlags, "LVLG", pod(std::uint32_t{ 0x01001234 }));
        malformed.emplace_back("LVLG before LVLF", std::move(globalBeforeFlags));

        for (const std::string_view type : { std::string_view{ "DATA" }, std::string_view{ "LLCT" },
                 std::string_view{ "JUNK" } })
        {
            std::string payload = flagsPrefix();
            appendSubRecord(payload, type, {});
            malformed.emplace_back(std::string(type), std::move(payload));
        }

        for (const auto& [name, payload] : malformed)
        {
            SCOPED_TRACE(name);
            expectRejected(payload);
        }
    }

    TEST(Esm4LevelledItemTest, retainsLegacyPermissiveSizesOutsideFnv)
    {
        std::string payload;
        appendSubRecord(payload, "LVLD", bytes({ 75, 0xaa }));
        appendSubRecord(payload, "LVLF", bytes({ 3, 0xbb }));
        const ESM4::LVLO entry = levelledEntry(9, 0x01004567, 4, 0, 0);
        std::string legacyEntry;
        appendPod(legacyEntry, entry.level);
        appendPod(legacyEntry, entry.item);
        appendPod(legacyEntry, entry.count);
        appendSubRecord(payload, "LVLO", legacyEntry);

        auto reader = makeReader(payload, 0.94f);
        ESM4::LevelledItem item;
        EXPECT_NO_THROW(item.load(*reader));
        EXPECT_TRUE(item.mHasChanceNone);
        EXPECT_EQ(item.mChanceNone, 75);
        EXPECT_TRUE(item.mHasLvlItemFlags);
        EXPECT_EQ(item.mLvlItemFlags, 3);
        ASSERT_EQ(item.mLvlObject.size(), 1);
        EXPECT_EQ(item.mLvlObject[0].level, 9);
        EXPECT_EQ(item.mLvlObject[0].item, 0x05004567u);
        EXPECT_EQ(item.mLvlObject[0].count, 4);
        ASSERT_EQ(item.mLvlObjectExtra.size(), 1);
        EXPECT_FALSE(item.mLvlObjectExtra[0].has_value());
    }
}
