#include <components/esm4/common.hpp>
#include <components/esm4/loadrcct.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

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

    std::string completePayload(std::uint8_t data = 0x6b)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("ChemsSubRecipes"));
        appendSubRecord(payload, "FULL", zString("Chems"));
        appendSubRecord(payload, "DATA", pod(data));
        return payload;
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
        appendRecord(plugin, "RCCT", 0x010003e8, payload, ESM4::Rec_Constant);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "FalloutNV.esm", nullptr, nullptr, true);
        reader->setModIndex(5);
        reader->updateModIndices({ { "master.esm", 2 } });
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    void expectRejectedWithoutMutation(const std::string& payload)
    {
        auto reader = makeReader(payload);
        ESM4::RecipeCategory category;
        category.mEditorId = "sentinel";
        category.mFullName = "unchanged";
        category.mData = 0xa5;
        EXPECT_THROW(category.load(*reader), std::runtime_error);
        EXPECT_EQ(category.mEditorId, "sentinel");
        EXPECT_EQ(category.mFullName, "unchanged");
        EXPECT_EQ(category.mData, 0xa5);
    }

    TEST(Esm4RecipeCategoryTest, loadsTheExactFrozenFnvShapeAndPreservesTheOpaqueDataByte)
    {
        auto reader = makeReader(completePayload());
        ESM4::RecipeCategory category;
        category.load(*reader);

        EXPECT_EQ(category.mId, ESM::FormId::fromUint32(0x050003e8));
        EXPECT_EQ(category.mFlags, ESM4::Rec_Constant);
        EXPECT_EQ(category.mEditorId, "ChemsSubRecipes");
        EXPECT_EQ(category.mFullName, "Chems");
        EXPECT_EQ(category.mData, 0x6b);

        for (const std::uint8_t authoredValue : { 0x00, 0x01, 0x3c, 0x6b, 0xfe, 0xff })
        {
            auto valueReader = makeReader(completePayload(authoredValue));
            ESM4::RecipeCategory value;
            value.load(*valueReader);
            EXPECT_EQ(value.mData, authoredValue);
        }
    }

    TEST(Esm4RecipeCategoryTest, rejectsMalformedSizesUnknownFieldsDuplicatesOrderingAndTruncationAtomically)
    {
        std::vector<std::string> malformed;
        malformed.emplace_back();

        std::string missingTerminator;
        appendSubRecord(missingTerminator, "EDID", "ChemsSubRecipes");
        appendSubRecord(missingTerminator, "FULL", zString("Chems"));
        appendSubRecord(missingTerminator, "DATA", pod(std::uint8_t{ 1 }));
        malformed.push_back(std::move(missingTerminator));

        std::string zeroEditor;
        appendSubRecord(zeroEditor, "EDID", {});
        appendSubRecord(zeroEditor, "FULL", zString("Chems"));
        appendSubRecord(zeroEditor, "DATA", pod(std::uint8_t{ 1 }));
        malformed.push_back(std::move(zeroEditor));

        std::string fullFirst;
        appendSubRecord(fullFirst, "FULL", zString("Chems"));
        appendSubRecord(fullFirst, "EDID", zString("ChemsSubRecipes"));
        appendSubRecord(fullFirst, "DATA", pod(std::uint8_t{ 1 }));
        malformed.push_back(std::move(fullFirst));

        std::string duplicateEditor = completePayload();
        appendSubRecord(duplicateEditor, "EDID", zString("Again"));
        malformed.push_back(std::move(duplicateEditor));

        std::string missingFull;
        appendSubRecord(missingFull, "EDID", zString("ChemsSubRecipes"));
        appendSubRecord(missingFull, "DATA", pod(std::uint8_t{ 1 }));
        malformed.push_back(std::move(missingFull));

        std::string zeroFull;
        appendSubRecord(zeroFull, "EDID", zString("ChemsSubRecipes"));
        appendSubRecord(zeroFull, "FULL", {});
        appendSubRecord(zeroFull, "DATA", pod(std::uint8_t{ 1 }));
        malformed.push_back(std::move(zeroFull));

        std::string duplicateFull;
        appendSubRecord(duplicateFull, "EDID", zString("ChemsSubRecipes"));
        appendSubRecord(duplicateFull, "FULL", zString("Chems"));
        appendSubRecord(duplicateFull, "FULL", zString("Again"));
        appendSubRecord(duplicateFull, "DATA", pod(std::uint8_t{ 1 }));
        malformed.push_back(std::move(duplicateFull));

        std::string shortData;
        appendSubRecord(shortData, "EDID", zString("ChemsSubRecipes"));
        appendSubRecord(shortData, "FULL", zString("Chems"));
        appendSubRecord(shortData, "DATA", {});
        malformed.push_back(std::move(shortData));

        std::string truncatedData;
        appendSubRecord(truncatedData, "EDID", zString("ChemsSubRecipes"));
        appendSubRecord(truncatedData, "FULL", zString("Chems"));
        truncatedData.append("DATA");
        appendPod(truncatedData, std::uint16_t{ 1 });
        malformed.push_back(std::move(truncatedData));

        std::string longData;
        appendSubRecord(longData, "EDID", zString("ChemsSubRecipes"));
        appendSubRecord(longData, "FULL", zString("Chems"));
        appendSubRecord(longData, "DATA", std::string("\x01\x02", 2));
        malformed.push_back(std::move(longData));

        std::string duplicateData = completePayload();
        appendSubRecord(duplicateData, "DATA", pod(std::uint8_t{ 1 }));
        malformed.push_back(std::move(duplicateData));

        std::string unknown;
        appendSubRecord(unknown, "EDID", zString("ChemsSubRecipes"));
        appendSubRecord(unknown, "FULL", zString("Chems"));
        appendSubRecord(unknown, "ICON", zString("not-authored.dds"));
        appendSubRecord(unknown, "DATA", pod(std::uint8_t{ 1 }));
        malformed.push_back(std::move(unknown));

        for (const std::string& payload : malformed)
            expectRejectedWithoutMutation(payload);
    }
}
