#include <gtest/gtest.h>

#include <cstdint>
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

#include <components/esm/refid.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadrcct.hpp>
#include <components/esm4/loadrcpe.hpp>
#include <components/esm4/reader.hpp>

#include "apps/openmw/mwworld/esmstore.hpp"

namespace
{
    struct Record
    {
        std::string mType;
        std::uint32_t mFormId = 0;
        std::string mPayload;
        std::uint32_t mFlags = 0;
    };

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

    void appendRecord(std::string& output, const Record& record)
    {
        output.append(record.mType);
        appendPod(output, static_cast<std::uint32_t>(record.mPayload.size()));
        appendPod(output, record.mFlags);
        appendPod(output, record.mFormId);
        appendPod(output, std::uint32_t{ 0 });
        output.append(record.mPayload);
    }

    std::string categoryPayload(std::string_view editorId, std::string_view fullName, std::uint8_t data)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString(editorId));
        appendSubRecord(payload, "FULL", zString(fullName));
        appendSubRecord(payload, "DATA", pod(data));
        return payload;
    }

    std::string recipePayload(std::string_view editorId, std::string_view fullName, std::uint32_t category,
        std::uint32_t subCategory, std::uint32_t ingredient = 0x14, std::uint32_t output = 0x15,
        std::uint32_t outputQuantity = 1)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString(editorId));
        appendSubRecord(payload, "FULL", zString(fullName));
        std::string data;
        appendPod(data, std::int32_t{ -1 });
        appendPod(data, std::uint32_t{ 50 });
        appendPod(data, category);
        appendPod(data, subCategory);
        appendSubRecord(payload, "DATA", data);
        appendSubRecord(payload, "RCIL", pod(ingredient));
        appendSubRecord(payload, "RCQY", pod(std::uint32_t{ 2 }));
        appendSubRecord(payload, "RCOD", pod(output));
        appendSubRecord(payload, "RCQY", pod(outputQuantity));
        return payload;
    }

    std::string makePlugin(const std::vector<Record>& records, std::string_view master = {})
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, static_cast<std::int32_t>(records.size() + 1));
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);
        if (!master.empty())
        {
            appendSubRecord(headerPayload, "MAST", zString(master));
            appendSubRecord(headerPayload, "DATA", pod(std::uint64_t{ 0 }));
        }

        std::string plugin;
        appendRecord(plugin, { "TES4", 0, std::move(headerPayload), ESM4::Rec_ESM });
        for (const Record& record : records)
            appendRecord(plugin, record);
        return plugin;
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& plugin, std::string_view filename,
        std::uint32_t modIndex, const std::map<std::string, int>& masters = {})
    {
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), filename, nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        if (!masters.empty())
            reader->updateModIndices(masters);
        return reader;
    }

    TEST(Esm4RecipeStoreTest, loadsBothFnvRecordTypesAndAdjustsEveryStoredFormId)
    {
        const std::string plugin = makePlugin(
            {
                { "RCCT", 0x00000500, categoryPayload("StoredCategory", "Stored Category", 0x6b),
                    ESM4::Rec_Constant },
                { "RCPE", 0x01000600,
                    recipePayload("StoredRecipe", "Stored Recipe", 0x00000500, 0x01000501, 0x00000014,
                        0x01000015, 3),
                    ESM4::Rec_Constant },
            },
            "Master.esm");
        auto reader = makeReader(plugin, "FalloutNV.esm", 5, { { "master.esm", 2 } });

        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        const auto& categories = store.get<ESM4::RecipeCategory>();
        ASSERT_EQ(categories.getSize(), 1);
        const ESM4::RecipeCategory* category
            = categories.search(ESM::RefId(ESM::FormId::fromUint32(0x02000500)));
        ASSERT_NE(category, nullptr);
        EXPECT_EQ(category->mEditorId, "StoredCategory");
        EXPECT_EQ(category->mData, 0x6b);

        const auto& recipes = store.get<ESM4::Recipe>();
        ASSERT_EQ(recipes.getSize(), 1);
        const ESM4::Recipe* recipe = recipes.search(ESM::RefId(ESM::FormId::fromUint32(0x05000600)));
        ASSERT_NE(recipe, nullptr);
        EXPECT_EQ(recipe->mFlags, ESM4::Rec_Constant);
        EXPECT_EQ(recipe->mData.mCategory, ESM::FormId::fromUint32(0x02000500));
        EXPECT_EQ(recipe->mData.mSubCategory, ESM::FormId::fromUint32(0x05000501));
        ASSERT_EQ(recipe->mIngredients.size(), 1);
        EXPECT_EQ(recipe->mIngredients[0].mItem, ESM::FormId::fromUint32(0x02000014));
        ASSERT_EQ(recipe->mOutputs.size(), 1);
        EXPECT_EQ(recipe->mOutputs[0].mItem, ESM::FormId::fromUint32(0x05000015));
        EXPECT_EQ(recipe->mOutputs[0].mQuantity, 3);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::FalloutNewVegas);
    }

    TEST(Esm4RecipeStoreTest, reproducesTheFrozenOfficialWinningLiveCensus)
    {
        constexpr std::size_t frozenRecipeCategories = 11;
        constexpr std::size_t frozenRecipes = 291;
        constexpr std::size_t frozenIngredientEntries = 651;
        constexpr std::size_t frozenOutputEntries = 336;
        constexpr std::size_t frozenQuantities = frozenIngredientEntries + frozenOutputEntries;
        static_assert(frozenQuantities == 987);

        std::vector<Record> records;
        records.reserve(frozenRecipeCategories + frozenRecipes);
        for (std::size_t index = 0; index < frozenRecipeCategories; ++index)
        {
            records.push_back({ "RCCT", 0x00010000u + static_cast<std::uint32_t>(index),
                categoryPayload("Category" + std::to_string(index), "Category", static_cast<std::uint8_t>(index)) });
        }
        for (std::size_t index = 0; index < frozenRecipes; ++index)
        {
            records.push_back({ "RCPE", 0x00020000u + static_cast<std::uint32_t>(index),
                recipePayload("Recipe" + std::to_string(index), "Recipe", 0x00010000, 0x00010000) });
        }
        ASSERT_EQ(records.size(), 302);

        auto reader = makeReader(makePlugin(records), "FalloutNV.esm", 0);
        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        EXPECT_EQ(store.get<ESM4::RecipeCategory>().getSize(), frozenRecipeCategories);
        EXPECT_EQ(store.get<ESM4::Recipe>().getSize(), frozenRecipes);
    }

    TEST(Esm4RecipeStoreTest, laterPluginsReplaceEarlierRecipeAndCategoryRecords)
    {
        const std::vector<Record> baseRecords = {
            { "RCCT", 0x00000500, categoryPayload("Category", "Base Category", 0x01) },
            { "RCPE", 0x00000600, recipePayload("Recipe", "Base Recipe", 0x00000500, 0x00000500) },
        };
        const std::vector<Record> overrideRecords = {
            { "RCCT", 0x00000500, categoryPayload("Category", "Winning Category", 0x6b) },
            { "RCPE", 0x00000600,
                recipePayload("Recipe", "Winning Recipe", 0x00000500, 0x00000500, 0x14, 0x15, 7) },
        };

        MWWorld::ESMStore store;
        auto base = makeReader(makePlugin(baseRecords), "FalloutNV.esm", 0);
        store.loadESM4(*base, nullptr);
        auto overrideReader = makeReader(
            makePlugin(overrideRecords, "FalloutNV.esm"), "DeadMoney.esm", 1, { { "falloutnv.esm", 0 } });
        store.loadESM4(*overrideReader, nullptr);

        const auto& categories = store.get<ESM4::RecipeCategory>();
        ASSERT_EQ(categories.getSize(), 1);
        const auto* category = categories.search(ESM::RefId(ESM::FormId::fromUint32(0x00000500)));
        ASSERT_NE(category, nullptr);
        EXPECT_EQ(category->mFullName, "Winning Category");
        EXPECT_EQ(category->mData, 0x6b);

        const auto& recipes = store.get<ESM4::Recipe>();
        ASSERT_EQ(recipes.getSize(), 1);
        const auto* recipe = recipes.search(ESM::RefId(ESM::FormId::fromUint32(0x00000600)));
        ASSERT_NE(recipe, nullptr);
        EXPECT_EQ(recipe->mFullName, "Winning Recipe");
        ASSERT_EQ(recipe->mOutputs.size(), 1);
        EXPECT_EQ(recipe->mOutputs[0].mQuantity, 7);
    }

    TEST(Esm4RecipeStoreTest, doesNotApplyTheFnvSchemasToAnotherGame)
    {
        const std::string plugin = makePlugin({
            { "RCCT", 0x00000500, categoryPayload("WrongCategory", "Skipped", 1) },
            { "RCPE", 0x00000600, recipePayload("WrongRecipe", "Skipped", 0x500, 0x500) },
        });
        auto reader = makeReader(plugin, "Skyrim.esm", 0);

        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        EXPECT_EQ(store.get<ESM4::RecipeCategory>().getSize(), 0);
        EXPECT_EQ(store.get<ESM4::Recipe>().getSize(), 0);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::Skyrim);
    }

    TEST(Esm4RecipeStoreTest, rejectsMalformedFnvRecordsBeforeTypedStoreInsertion)
    {
        std::string invalidCategory = categoryPayload("InvalidCategory", "Invalid", 1);
        appendSubRecord(invalidCategory, "ICON", zString("not-authored.dds"));
        auto categoryReader = makeReader(
            makePlugin({ { "RCCT", 0x00000500, std::move(invalidCategory) } }), "FalloutNV.esm", 0);
        MWWorld::ESMStore categoryStore;
        EXPECT_THROW(categoryStore.loadESM4(*categoryReader, nullptr), std::runtime_error);
        EXPECT_EQ(categoryStore.get<ESM4::RecipeCategory>().getSize(), 0);

        std::string invalidRecipe = recipePayload("InvalidRecipe", "Invalid", 0x500, 0x500);
        appendSubRecord(invalidRecipe, "ICON", zString("not-authored.dds"));
        auto recipeReader = makeReader(
            makePlugin({ { "RCPE", 0x00000600, std::move(invalidRecipe) } }), "FalloutNV.esm", 0);
        MWWorld::ESMStore recipeStore;
        EXPECT_THROW(recipeStore.loadESM4(*recipeReader, nullptr), std::runtime_error);
        EXPECT_EQ(recipeStore.get<ESM4::Recipe>().getSize(), 0);
    }
}
