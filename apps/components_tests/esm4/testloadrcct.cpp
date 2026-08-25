#include <components/esm4/falloutformat.hpp>
#include <components/esm4/loadrcct.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testutil.hpp"

namespace
{
    constexpr std::uint8_t kAuthoredData = 0x6b;
    constexpr std::uint8_t kSentinelData = 0xa5;
    constexpr std::array<std::uint8_t, 6> kAuthoredDataValues{ 0x00, 0x01, 0x3c, 0x6b, 0xfe, 0xff };
    constexpr std::size_t kMalformedDataBytes = ESM4::Fallout::kRecipeCategoryDataBytes + 1;
    constexpr std::uint32_t kExpectedFormId = 0x02123456;
    constexpr std::string_view kEditorId = "ChemsSubRecipes";
    constexpr std::string_view kFullName = "Chems";

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    std::string completePayload(std::uint8_t data = kAuthoredData)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString(kEditorId));
        ESM4Test::appendSubRecord(payload, "FULL", zString(kFullName));
        ESM4Test::appendSubRecord(
            payload, "DATA", std::string(ESM4::Fallout::kRecipeCategoryDataBytes, static_cast<char>(data)));
        return payload;
    }

    ESM4::RecipeCategory loadCategory(std::string payload, float version = ESM4Test::kFalloutPluginVersion)
    {
        auto reader = ESM4Test::makeReader("RCCT", std::move(payload), ESM4Test::kSyntheticModIndex, version);
        ESM4::RecipeCategory category;
        category.load(*reader);
        return category;
    }

    TEST(Esm4RecipeCategoryTest, LoadsExactFalloutShapeAndPreservesAuthoredData)
    {
        const ESM4::RecipeCategory category = loadCategory(completePayload());

        EXPECT_EQ(category.mId, ESM::FormId::fromUint32(kExpectedFormId));
        EXPECT_EQ(category.mEditorId, kEditorId);
        EXPECT_EQ(category.mFullName, kFullName);
        EXPECT_EQ(category.mData, kAuthoredData);
    }

    TEST(Esm4RecipeCategoryTest, PreservesEveryAuthoredDataByte)
    {
        for (const std::uint8_t data : kAuthoredDataValues)
        {
            SCOPED_TRACE(static_cast<unsigned int>(data));
            EXPECT_EQ(loadCategory(completePayload(data)).mData, data);
        }
    }

    TEST(Esm4RecipeCategoryTest, RejectsMalformedRecordsWithoutMutatingTheDestination)
    {
        std::vector<std::string> malformed;
        malformed.emplace_back();

        std::string missingTerminator;
        ESM4Test::appendSubRecord(missingTerminator, "EDID", kEditorId);
        ESM4Test::appendSubRecord(missingTerminator, "FULL", zString(kFullName));
        ESM4Test::appendSubRecord(missingTerminator, "DATA",
            std::string(ESM4::Fallout::kRecipeCategoryDataBytes, static_cast<char>(kAuthoredData)));
        malformed.push_back(std::move(missingTerminator));

        std::string zeroEditor;
        ESM4Test::appendSubRecord(zeroEditor, "EDID", {});
        ESM4Test::appendSubRecord(zeroEditor, "FULL", zString(kFullName));
        ESM4Test::appendSubRecord(zeroEditor, "DATA",
            std::string(ESM4::Fallout::kRecipeCategoryDataBytes, static_cast<char>(kAuthoredData)));
        malformed.push_back(std::move(zeroEditor));

        std::string fullFirst;
        ESM4Test::appendSubRecord(fullFirst, "FULL", zString(kFullName));
        ESM4Test::appendSubRecord(fullFirst, "EDID", zString(kEditorId));
        ESM4Test::appendSubRecord(fullFirst, "DATA",
            std::string(ESM4::Fallout::kRecipeCategoryDataBytes, static_cast<char>(kAuthoredData)));
        malformed.push_back(std::move(fullFirst));

        std::string duplicateEditor = completePayload();
        ESM4Test::appendSubRecord(duplicateEditor, "EDID", zString("Again"));
        malformed.push_back(std::move(duplicateEditor));

        std::string missingFull;
        ESM4Test::appendSubRecord(missingFull, "EDID", zString(kEditorId));
        ESM4Test::appendSubRecord(missingFull, "DATA",
            std::string(ESM4::Fallout::kRecipeCategoryDataBytes, static_cast<char>(kAuthoredData)));
        malformed.push_back(std::move(missingFull));

        std::string zeroFull;
        ESM4Test::appendSubRecord(zeroFull, "EDID", zString(kEditorId));
        ESM4Test::appendSubRecord(zeroFull, "FULL", {});
        ESM4Test::appendSubRecord(zeroFull, "DATA",
            std::string(ESM4::Fallout::kRecipeCategoryDataBytes, static_cast<char>(kAuthoredData)));
        malformed.push_back(std::move(zeroFull));

        std::string shortData;
        ESM4Test::appendSubRecord(shortData, "EDID", zString(kEditorId));
        ESM4Test::appendSubRecord(shortData, "FULL", zString(kFullName));
        ESM4Test::appendSubRecord(shortData, "DATA", {});
        malformed.push_back(std::move(shortData));

        std::string longData;
        ESM4Test::appendSubRecord(longData, "EDID", zString(kEditorId));
        ESM4Test::appendSubRecord(longData, "FULL", zString(kFullName));
        ESM4Test::appendSubRecord(longData, "DATA", std::string(kMalformedDataBytes, '\0'));
        malformed.push_back(std::move(longData));

        std::string duplicateData = completePayload();
        ESM4Test::appendSubRecord(duplicateData, "DATA",
            std::string(ESM4::Fallout::kRecipeCategoryDataBytes, static_cast<char>(kAuthoredData)));
        malformed.push_back(std::move(duplicateData));

        std::string unknown;
        ESM4Test::appendSubRecord(unknown, "EDID", zString(kEditorId));
        ESM4Test::appendSubRecord(unknown, "FULL", zString(kFullName));
        ESM4Test::appendSubRecord(unknown, "ICON", zString("not-authored.dds"));
        ESM4Test::appendSubRecord(unknown, "DATA",
            std::string(ESM4::Fallout::kRecipeCategoryDataBytes, static_cast<char>(kAuthoredData)));
        malformed.push_back(std::move(unknown));

        for (std::size_t index = 0; index < malformed.size(); ++index)
        {
            SCOPED_TRACE(index);
            auto reader = ESM4Test::makeReader("RCCT", std::move(malformed[index]));
            ESM4::RecipeCategory category;
            category.mEditorId = "sentinel";
            category.mFullName = "unchanged";
            category.mData = kSentinelData;

            EXPECT_THROW(category.load(*reader), std::runtime_error);
            EXPECT_EQ(category.mEditorId, "sentinel");
            EXPECT_EQ(category.mFullName, "unchanged");
            EXPECT_EQ(category.mData, kSentinelData);
        }
    }

    TEST(Esm4RecipeCategoryTest, RejectsNonFalloutVersions)
    {
        EXPECT_THROW(loadCategory(completePayload(), ESM4Test::kOtherPluginVersion), std::runtime_error);
    }
}
