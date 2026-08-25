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
#include <components/esm4/falloutformat.hpp>
#include <components/esm4/loadrcpe.hpp>
#include <components/esm4/reader.hpp>

#include "apps/openmw/mwworld/esmstore.hpp"

namespace
{
    constexpr std::uint32_t kRecipeFormId = 0x000003e8;
    constexpr std::uint32_t kExpectedRecipeFormId = 0x020003e8;
    constexpr std::uint32_t kCategoryRaw = 0x01100010;
    constexpr std::uint32_t kExpectedCategory = 0x02100010;
    constexpr std::uint32_t kSubCategoryRaw = 0x01100020;
    constexpr std::uint32_t kExpectedSubCategory = 0x02100020;
    constexpr std::uint32_t kIngredientRaw = 0x01100030;
    constexpr std::uint32_t kExpectedIngredient = 0x02100030;
    constexpr std::uint32_t kOutputRaw = 0x01100040;
    constexpr std::uint32_t kExpectedOutput = 0x02100040;
    constexpr std::uint32_t kIngredientQuantity = 2;
    constexpr std::uint32_t kOutputQuantity = 3;
    constexpr std::uint32_t kRecordFlags = ESM4::Rec_Constant;
    constexpr std::uint32_t kHeaderFlags = 0x800;
    constexpr std::int32_t kHeaderRecordCount = 2;
    constexpr std::uint32_t kUnusedRecordWord = 0;
    constexpr std::uint16_t kUnusedRecordVersion = 0;
    constexpr std::uint32_t kSyntheticModIndex = 2;
    constexpr float kFalloutPluginVersion
        = std::bit_cast<float>(static_cast<std::uint32_t>(ESM::VER_134));

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

    std::string condition()
    {
        std::string result;
        appendPod(result, std::uint32_t{});
        appendPod(result, 1.f);
        appendPod(result, ESM4::FUN_GetHasNote);
        appendPod(result, std::uint32_t{});
        appendPod(result, std::uint32_t{});
        appendPod(result, std::uint32_t{});
        appendPod(result, std::uint32_t{});
        return result;
    }

    std::string recipePayload()
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("StoredRecipe"));
        appendSubRecord(payload, "FULL", zString("Stored Recipe"));
        appendSubRecord(payload, "CTDA", condition());

        std::string data;
        appendPod(data, std::int32_t{ -1 });
        appendPod(data, std::uint32_t{ 50 });
        appendPod(data, kCategoryRaw);
        appendPod(data, kSubCategoryRaw);
        if (data.size() != ESM4::Fallout::kRecipeDataBytes)
            throw std::logic_error("synthetic recipe DATA shape drifted");
        appendSubRecord(payload, "DATA", data);
        appendSubRecord(payload, "RCIL", pod(kIngredientRaw));
        appendSubRecord(payload, "RCQY", pod(kIngredientQuantity));
        appendSubRecord(payload, "RCOD", pod(kOutputRaw));
        appendSubRecord(payload, "RCQY", pod(kOutputQuantity));
        return payload;
    }

    std::string makePlugin(std::string_view recipe)
    {
        std::string hedr;
        appendPod(hedr, kFalloutPluginVersion);
        appendPod(hedr, kHeaderRecordCount);
        appendPod(hedr, kHeaderFlags);

        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload, ESM4::Rec_ESM);
        appendRecord(plugin, "RCPE", kRecipeFormId, recipe, kRecordFlags);
        return plugin;
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& plugin, std::string_view filename)
    {
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), filename, nullptr, nullptr, true);
        reader->setModIndex(kSyntheticModIndex);
        return reader;
    }

    TEST(Esm4RecipeStoreTest, LoadsFalloutRecipeIntoTypedStore)
    {
        auto reader = makeReader(makePlugin(recipePayload()), "FalloutNV.esm");
        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);
        store.setUp();

        const auto& recipes = store.get<ESM4::Recipe>();
        ASSERT_EQ(recipes.getSize(), 1);
        const ESM4::Recipe* recipe = recipes.search(ESM::RefId(ESM::FormId::fromUint32(kExpectedRecipeFormId)));
        ASSERT_NE(recipe, nullptr);
        EXPECT_EQ(recipe->mFlags, kRecordFlags);
        EXPECT_EQ(recipe->mEditorId, "StoredRecipe");
        EXPECT_EQ(recipe->mData.mCategory, ESM::FormId::fromUint32(kExpectedCategory));
        EXPECT_EQ(recipe->mData.mSubCategory, ESM::FormId::fromUint32(kExpectedSubCategory));
        ASSERT_EQ(recipe->mIngredients.size(), 1);
        EXPECT_EQ(recipe->mIngredients.front().mItem, ESM::FormId::fromUint32(kExpectedIngredient));
        EXPECT_EQ(recipe->mIngredients.front().mQuantity, kIngredientQuantity);
        ASSERT_EQ(recipe->mOutputs.size(), 1);
        EXPECT_EQ(recipe->mOutputs.front().mItem, ESM::FormId::fromUint32(kExpectedOutput));
        EXPECT_EQ(recipe->mOutputs.front().mQuantity, kOutputQuantity);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::FalloutNewVegas);
    }

    TEST(Esm4RecipeStoreTest, DoesNotApplyTheFalloutRecipeSchemaToOtherGames)
    {
        auto reader = makeReader(makePlugin(recipePayload()), "Skyrim.esm");
        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        EXPECT_EQ(store.get<ESM4::Recipe>().getSize(), 0);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::Skyrim);
    }

    TEST(Esm4RecipeStoreTest, RejectsMalformedRecipeBeforeTypedStoreInsertion)
    {
        std::string malformed = recipePayload();
        appendSubRecord(malformed, "ICON", zString("not-authored.dds"));

        auto reader = makeReader(makePlugin(malformed), "FalloutNV.esm");
        MWWorld::ESMStore store;
        EXPECT_THROW(store.loadESM4(*reader, nullptr), std::runtime_error);
        EXPECT_EQ(store.get<ESM4::Recipe>().getSize(), 0);
    }
}
