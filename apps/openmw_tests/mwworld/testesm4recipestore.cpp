#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <components/esm4/common.hpp>
#include <components/esm4/loadrcct.hpp>
#include <components/esm4/reader.hpp>

#include "apps/openmw/mwworld/esmstore.hpp"

namespace
{
    constexpr std::uint32_t kCategoryFormId = 0x000003e8;
    constexpr std::uint32_t kExpectedCategoryFormId = 0x020003e8;
    constexpr std::uint32_t kHeaderFlags = 0x800;
    constexpr std::uint32_t kRecordFlags = ESM4::Rec_Constant;
    constexpr std::uint32_t kSyntheticModIndex = 2;
    constexpr std::uint8_t kAuthoredData = 0x6b;
    constexpr std::size_t kExpectedCategoryCount = 1;
    constexpr std::int32_t kHeaderRecordCount = 2;
    constexpr std::uint32_t kUnusedRecordWord = 0;
    constexpr std::uint16_t kUnusedRecordVersion = 0;
    constexpr float kFalloutPluginVersion
        = std::bit_cast<float>(static_cast<std::uint32_t>(ESM::VER_134));

    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        ASSERT_EQ(type.size(), ESM4::Fallout::kOnDiskFormIdBytes);
        ASSERT_LE(data.size(), std::numeric_limits<std::uint16_t>::max());
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view data,
        std::uint32_t flags)
    {
        ASSERT_EQ(type.size(), ESM4::Fallout::kOnDiskFormIdBytes);
        output.append(type);
        appendPod(output, static_cast<std::uint32_t>(data.size()));
        appendPod(output, flags);
        appendPod(output, formId);
        appendPod(output, kUnusedRecordWord);
        appendPod(output, kUnusedRecordVersion);
        appendPod(output, kUnusedRecordVersion);
        output.append(data);
    }

    std::string categoryPayload(std::uint8_t data)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("StoredCategory"));
        appendSubRecord(payload, "FULL", zString("Stored Category"));
        appendSubRecord(
            payload, "DATA", std::string(ESM4::Fallout::kRecipeCategoryDataBytes, static_cast<char>(data)));
        return payload;
    }

    std::string makePlugin(std::string_view categoryPayload, std::uint32_t categoryFlags = kRecordFlags)
    {
        std::string hedr;
        appendPod(hedr, kFalloutPluginVersion);
        appendPod(hedr, kHeaderRecordCount);
        appendPod(hedr, kHeaderFlags);

        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload, ESM4::Rec_ESM);
        appendRecord(plugin, "RCCT", kCategoryFormId, categoryPayload, categoryFlags);
        return plugin;
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& plugin, std::string_view filename)
    {
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), filename, nullptr, nullptr, true);
        reader->setModIndex(kSyntheticModIndex);
        return reader;
    }

    TEST(Esm4RecipeCategoryStoreTest, LoadsFalloutCategoryIntoTypedStore)
    {
        auto reader = makeReader(makePlugin(categoryPayload(kAuthoredData)), "FalloutNV.esm");
        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);
        store.setUp();

        const auto& categories = store.get<ESM4::RecipeCategory>();
        ASSERT_EQ(categories.getSize(), kExpectedCategoryCount);
        const ESM4::RecipeCategory* category
            = categories.search(ESM::RefId(ESM::FormId::fromUint32(kExpectedCategoryFormId)));
        ASSERT_NE(category, nullptr);
        EXPECT_EQ(category->mFlags, kRecordFlags);
        EXPECT_EQ(category->mEditorId, "StoredCategory");
        EXPECT_EQ(category->mFullName, "Stored Category");
        EXPECT_EQ(category->mData, kAuthoredData);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::FalloutNewVegas);
    }

    TEST(Esm4RecipeCategoryStoreTest, DoesNotApplyTheFalloutSchemaToOtherGames)
    {
        auto reader = makeReader(makePlugin(categoryPayload(kAuthoredData)), "Skyrim.esm");
        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        EXPECT_EQ(store.get<ESM4::RecipeCategory>().getSize(), 0);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::Skyrim);
    }

    TEST(Esm4RecipeCategoryStoreTest, RejectsMalformedRecordsBeforeInsertion)
    {
        std::string malformed = categoryPayload(kAuthoredData);
        appendSubRecord(malformed, "ICON", zString("not-authored.dds"));

        auto reader = makeReader(makePlugin(malformed), "FalloutNV.esm");
        MWWorld::ESMStore store;
        EXPECT_THROW(store.loadESM4(*reader, nullptr), std::runtime_error);
        EXPECT_EQ(store.get<ESM4::RecipeCategory>().getSize(), 0);
    }
}
