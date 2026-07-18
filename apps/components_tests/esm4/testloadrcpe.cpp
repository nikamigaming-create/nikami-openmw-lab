#include <components/esm4/common.hpp>
#include <components/esm4/loadrcpe.hpp>
#include <components/esm4/reader.hpp>

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

    std::string data(
        std::int32_t skill, std::uint32_t level, std::uint32_t category, std::uint32_t subCategory)
    {
        std::string result;
        appendPod(result, skill);
        appendPod(result, level);
        appendPod(result, category);
        appendPod(result, subCategory);
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

    std::string prefix()
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("ByteExactRecipe"));
        appendSubRecord(payload, "FULL", zString("Byte Exact Recipe"));
        return payload;
    }

    void appendIngredient(std::string& payload, std::uint32_t item, std::uint32_t quantity)
    {
        appendSubRecord(payload, "RCIL", pod(item));
        appendSubRecord(payload, "RCQY", pod(quantity));
    }

    void appendOutput(std::string& payload, std::uint32_t item, std::uint32_t quantity)
    {
        appendSubRecord(payload, "RCOD", pod(item));
        appendSubRecord(payload, "RCQY", pod(quantity));
    }

    std::string completePayload(std::uint32_t category = 0x00001000, std::uint32_t subCategory = 0x01002000)
    {
        std::string payload = prefix();
        appendSubRecord(payload, "CTDA", condition(ESM4::FUN_GetHasNote, 0x00000014, 0, 2, 0x01001234));
        appendSubRecord(payload, "CTDA", condition(ESM4::FUN_GetMapMarkerVisible, 0, 0, 2, 0x01004321));
        appendSubRecord(payload, "DATA", data(-1, 50, category, subCategory));
        appendIngredient(payload, 0x00003000, 2);
        appendIngredient(payload, 0x01004000, 1000);
        appendOutput(payload, 0x01005000, 3);
        appendOutput(payload, 0x00006000, 1);
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
        appendRecord(plugin, "RCPE", 0x010003e8, payload, ESM4::Rec_Constant);
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
        ESM4::Recipe recipe;
        recipe.mEditorId = "sentinel";
        recipe.mFullName = "unchanged";
        recipe.mData.mRequiredSkill = 42;
        recipe.mIngredients.push_back({ ESM::FormId::fromUint32(0x123), 7 });
        EXPECT_THROW(recipe.load(*reader), std::runtime_error);
        EXPECT_EQ(recipe.mEditorId, "sentinel");
        EXPECT_EQ(recipe.mFullName, "unchanged");
        EXPECT_EQ(recipe.mData.mRequiredSkill, 42);
        ASSERT_EQ(recipe.mIngredients.size(), 1);
        EXPECT_EQ(recipe.mIngredients[0].mQuantity, 7);
    }

    TEST(Esm4RecipeTest, loadsEveryFrozenFieldAndAdjustsAllAuthoredFormIds)
    {
        auto reader = makeReader(completePayload());
        ESM4::Recipe recipe;
        recipe.load(*reader);

        EXPECT_EQ(recipe.mId, ESM::FormId::fromUint32(0x050003e8));
        EXPECT_EQ(recipe.mFlags, ESM4::Rec_Constant);
        EXPECT_EQ(recipe.mEditorId, "ByteExactRecipe");
        EXPECT_EQ(recipe.mFullName, "Byte Exact Recipe");
        EXPECT_EQ(recipe.mData.mRequiredSkill, -1);
        EXPECT_EQ(recipe.mData.mRequiredSkillLevel, 50);
        EXPECT_EQ(recipe.mData.mCategory, ESM::FormId::fromUint32(0x02001000));
        EXPECT_EQ(recipe.mData.mSubCategory, ESM::FormId::fromUint32(0x05002000));

        ASSERT_EQ(recipe.mConditions.size(), 2);
        EXPECT_EQ(recipe.mConditions[0].functionIndex, ESM4::FUN_GetHasNote);
        EXPECT_EQ(recipe.mConditions[0].param1, 0x02000014u);
        EXPECT_EQ(recipe.mConditions[0].reference, 0x05001234u);
        EXPECT_EQ(recipe.mConditions[1].functionIndex, ESM4::FUN_GetMapMarkerVisible);
        EXPECT_EQ(recipe.mConditions[1].reference, 0x05004321u);

        ASSERT_EQ(recipe.mIngredients.size(), 2);
        EXPECT_EQ(recipe.mIngredients[0].mItem, ESM::FormId::fromUint32(0x02003000));
        EXPECT_EQ(recipe.mIngredients[0].mQuantity, 2);
        EXPECT_EQ(recipe.mIngredients[1].mItem, ESM::FormId::fromUint32(0x05004000));
        EXPECT_EQ(recipe.mIngredients[1].mQuantity, 1000);
        ASSERT_EQ(recipe.mOutputs.size(), 2);
        EXPECT_EQ(recipe.mOutputs[0].mItem, ESM::FormId::fromUint32(0x05005000));
        EXPECT_EQ(recipe.mOutputs[0].mQuantity, 3);
        EXPECT_EQ(recipe.mOutputs[1].mItem, ESM::FormId::fromUint32(0x02006000));
        EXPECT_EQ(recipe.mOutputs[1].mQuantity, 1);
    }

    TEST(Esm4RecipeTest, acceptsTheSixOfficialNullableCategoryRecords)
    {
        auto reader = makeReader(completePayload(0));
        ESM4::Recipe recipe;
        recipe.load(*reader);

        EXPECT_TRUE(recipe.mData.mCategory.isZeroOrUnset());
        EXPECT_EQ(recipe.mData.mSubCategory, ESM::FormId::fromUint32(0x05002000));
    }

    TEST(Esm4RecipeTest, rejectsMalformedSizesUnknownFieldsDuplicatesOrderingAndTruncationAtomically)
    {
        std::vector<std::string> malformed;
        malformed.emplace_back();

        std::string missingTerminator;
        appendSubRecord(missingTerminator, "EDID", "ByteExactRecipe");
        appendSubRecord(missingTerminator, "FULL", zString("Byte Exact Recipe"));
        appendSubRecord(missingTerminator, "DATA", data(-1, 50, 0, 0x01002000));
        appendIngredient(missingTerminator, 0x10, 1);
        appendOutput(missingTerminator, 0x20, 1);
        malformed.push_back(std::move(missingTerminator));

        std::string fullFirst;
        appendSubRecord(fullFirst, "FULL", zString("Byte Exact Recipe"));
        appendSubRecord(fullFirst, "EDID", zString("ByteExactRecipe"));
        malformed.push_back(std::move(fullFirst));

        std::string zeroFull;
        appendSubRecord(zeroFull, "EDID", zString("ByteExactRecipe"));
        appendSubRecord(zeroFull, "FULL", {});
        malformed.push_back(std::move(zeroFull));

        std::string duplicateEditor = prefix();
        appendSubRecord(duplicateEditor, "EDID", zString("Again"));
        malformed.push_back(std::move(duplicateEditor));

        std::string duplicateFull = prefix();
        appendSubRecord(duplicateFull, "FULL", zString("Again"));
        malformed.push_back(std::move(duplicateFull));

        std::string conditionBeforeFull;
        appendSubRecord(conditionBeforeFull, "EDID", zString("ByteExactRecipe"));
        appendSubRecord(conditionBeforeFull, "CTDA", condition(ESM4::FUN_GetHasNote));
        malformed.push_back(std::move(conditionBeforeFull));

        for (const std::size_t size : { 27u, 29u })
        {
            std::string badCondition = prefix();
            appendSubRecord(badCondition, "CTDA", std::string(size, '\0'));
            malformed.push_back(std::move(badCondition));
        }

        malformed.push_back(prefix());
        for (const std::size_t size : { 15u, 17u })
        {
            std::string badData = prefix();
            appendSubRecord(badData, "DATA", std::string(size, '\0'));
            malformed.push_back(std::move(badData));
        }

        std::string truncatedData = prefix();
        truncatedData.append("DATA");
        appendPod(truncatedData, std::uint16_t{ 16 });
        truncatedData.append(8, '\0');
        malformed.push_back(std::move(truncatedData));

        std::string duplicateData = prefix();
        appendSubRecord(duplicateData, "DATA", data(-1, 50, 0, 0x20));
        appendSubRecord(duplicateData, "DATA", data(-1, 50, 0, 0x20));
        malformed.push_back(std::move(duplicateData));

        std::string zeroSubCategory = prefix();
        appendSubRecord(zeroSubCategory, "DATA", data(-1, 50, 0, 0));
        malformed.push_back(std::move(zeroSubCategory));

        std::string ingredientBeforeData = prefix();
        appendIngredient(ingredientBeforeData, 0x10, 1);
        malformed.push_back(std::move(ingredientBeforeData));

        std::string outputBeforeIngredient = prefix();
        appendSubRecord(outputBeforeIngredient, "DATA", data(-1, 50, 0, 0x20));
        appendOutput(outputBeforeIngredient, 0x30, 1);
        malformed.push_back(std::move(outputBeforeIngredient));

        std::string shortIngredient = prefix();
        appendSubRecord(shortIngredient, "DATA", data(-1, 50, 0, 0x20));
        appendSubRecord(shortIngredient, "RCIL", std::string(3, '\0'));
        malformed.push_back(std::move(shortIngredient));

        std::string zeroIngredient = prefix();
        appendSubRecord(zeroIngredient, "DATA", data(-1, 50, 0, 0x20));
        appendIngredient(zeroIngredient, 0, 1);
        malformed.push_back(std::move(zeroIngredient));

        std::string missingIngredientQuantity = prefix();
        appendSubRecord(missingIngredientQuantity, "DATA", data(-1, 50, 0, 0x20));
        appendSubRecord(missingIngredientQuantity, "RCIL", pod(std::uint32_t{ 0x10 }));
        malformed.push_back(std::move(missingIngredientQuantity));

        std::string consecutiveIngredients = missingIngredientQuantity;
        appendSubRecord(consecutiveIngredients, "RCIL", pod(std::uint32_t{ 0x11 }));
        malformed.push_back(std::move(consecutiveIngredients));

        std::string shortIngredientQuantity = prefix();
        appendSubRecord(shortIngredientQuantity, "DATA", data(-1, 50, 0, 0x20));
        appendSubRecord(shortIngredientQuantity, "RCIL", pod(std::uint32_t{ 0x10 }));
        appendSubRecord(shortIngredientQuantity, "RCQY", std::string(3, '\0'));
        malformed.push_back(std::move(shortIngredientQuantity));

        std::string zeroIngredientQuantity = prefix();
        appendSubRecord(zeroIngredientQuantity, "DATA", data(-1, 50, 0, 0x20));
        appendIngredient(zeroIngredientQuantity, 0x10, 0);
        malformed.push_back(std::move(zeroIngredientQuantity));

        std::string noOutputs = prefix();
        appendSubRecord(noOutputs, "DATA", data(-1, 50, 0, 0x20));
        appendIngredient(noOutputs, 0x10, 1);
        malformed.push_back(std::move(noOutputs));

        std::string shortOutput = noOutputs;
        appendSubRecord(shortOutput, "RCOD", std::string(3, '\0'));
        malformed.push_back(std::move(shortOutput));

        std::string zeroOutput = noOutputs;
        appendOutput(zeroOutput, 0, 1);
        malformed.push_back(std::move(zeroOutput));

        std::string missingOutputQuantity = noOutputs;
        appendSubRecord(missingOutputQuantity, "RCOD", pod(std::uint32_t{ 0x30 }));
        malformed.push_back(std::move(missingOutputQuantity));

        std::string zeroOutputQuantity = noOutputs;
        appendOutput(zeroOutputQuantity, 0x30, 0);
        malformed.push_back(std::move(zeroOutputQuantity));

        std::string ingredientAfterOutput = noOutputs;
        appendOutput(ingredientAfterOutput, 0x30, 1);
        appendIngredient(ingredientAfterOutput, 0x40, 1);
        malformed.push_back(std::move(ingredientAfterOutput));

        std::string unknown = prefix();
        appendSubRecord(unknown, "ICON", zString("not-authored.dds"));
        malformed.push_back(std::move(unknown));

        std::string trailingCondition = completePayload();
        appendSubRecord(trailingCondition, "CTDA", condition(ESM4::FUN_GetHasNote));
        malformed.push_back(std::move(trailingCondition));

        std::string trailingQuantity = completePayload();
        appendSubRecord(trailingQuantity, "RCQY", pod(std::uint32_t{ 1 }));
        malformed.push_back(std::move(trailingQuantity));

        for (const std::string& payload : malformed)
            expectRejectedWithoutMutation(payload);
    }
}
