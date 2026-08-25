#include <components/esm4/falloutformat.hpp>
#include <components/esm4/loadrcpe.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testutil.hpp"

namespace
{
    constexpr std::uint32_t kExpectedFormId = 0x02123456;
    constexpr std::uint32_t kCategoryRaw = 0x01100010;
    constexpr std::uint32_t kExpectedCategory = 0x02100010;
    constexpr std::uint32_t kSubCategoryRaw = 0x01100020;
    constexpr std::uint32_t kExpectedSubCategory = 0x02100020;
    constexpr std::uint32_t kIngredientOneRaw = 0x01100030;
    constexpr std::uint32_t kIngredientTwoRaw = 0x01100040;
    constexpr std::uint32_t kExpectedIngredientOne = 0x02100030;
    constexpr std::uint32_t kExpectedIngredientTwo = 0x02100040;
    constexpr std::uint32_t kOutputOneRaw = 0x01100050;
    constexpr std::uint32_t kOutputTwoRaw = 0x01100060;
    constexpr std::uint32_t kExpectedOutputOne = 0x02100050;
    constexpr std::uint32_t kExpectedOutputTwo = 0x02100060;
    constexpr std::uint32_t kFirstIngredientQuantity = 2;
    constexpr std::uint32_t kSecondIngredientQuantity = 1000;
    constexpr std::uint32_t kFirstOutputQuantity = 3;
    constexpr std::uint32_t kSecondOutputQuantity = 1;
    constexpr std::int32_t kRequiredSkill = ESM4::Fallout::kRecipeDefaultRequiredSkill;
    constexpr std::int32_t kSentinelRequiredSkill = 42;
    constexpr std::uint32_t kRequiredSkillLevel = 50;
    constexpr std::uint32_t kConditionFlags = 0;
    constexpr float kConditionComparison = 1.f;
    constexpr std::uint32_t kConditionParameter = 0x14;
    constexpr std::uint32_t kConditionRunOn = 2;
    constexpr std::uint32_t kConditionReferenceRaw = 0x01101234;
    constexpr std::uint32_t kExpectedConditionReference = 0x02101234;
    constexpr std::uint32_t kSecondConditionReferenceRaw = 0x01104321;
    constexpr std::uint32_t kExpectedSecondConditionReference = 0x02104321;
    constexpr std::uint32_t kConditionFunction = ESM4::FUN_GetHasNote;
    constexpr std::uint32_t kSecondConditionFunction = ESM4::FUN_GetMapMarkerVisible;
    constexpr std::size_t kMalformedConditionBytes = sizeof(ESM4::TargetCondition) - 1;
    constexpr std::size_t kOversizedConditionBytes = sizeof(ESM4::TargetCondition) + 1;
    constexpr std::size_t kMalformedDataBytes = ESM4::Fallout::kRecipeDataBytes - 1;

    constexpr std::string_view kEditorId = "ByteExactRecipe";
    constexpr std::string_view kFullName = "Byte Exact Recipe";

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    template <class T>
    std::string pod(const T& value)
    {
        std::string result;
        ESM4Test::appendPod(result, value);
        return result;
    }

    std::string condition(std::uint32_t function, std::uint32_t parameter1 = kConditionParameter,
        std::uint32_t reference = kConditionReferenceRaw)
    {
        std::string result;
        ESM4Test::appendPod(result, kConditionFlags);
        ESM4Test::appendPod(result, kConditionComparison);
        ESM4Test::appendPod(result, function);
        ESM4Test::appendPod(result, parameter1);
        ESM4Test::appendPod(result, std::uint32_t{});
        ESM4Test::appendPod(result, kConditionRunOn);
        ESM4Test::appendPod(result, reference);
        return result;
    }

    std::string data(std::uint32_t category = kCategoryRaw, std::uint32_t subCategory = kSubCategoryRaw)
    {
        std::string result;
        ESM4Test::appendPod(result, kRequiredSkill);
        ESM4Test::appendPod(result, kRequiredSkillLevel);
        ESM4Test::appendPod(result, category);
        ESM4Test::appendPod(result, subCategory);
        return result;
    }

    void appendIngredient(std::string& payload, std::uint32_t item, std::uint32_t quantity)
    {
        ESM4Test::appendSubRecord(payload, "RCIL", pod(item));
        ESM4Test::appendSubRecord(payload, "RCQY", pod(quantity));
    }

    void appendOutput(std::string& payload, std::uint32_t item, std::uint32_t quantity)
    {
        ESM4Test::appendSubRecord(payload, "RCOD", pod(item));
        ESM4Test::appendSubRecord(payload, "RCQY", pod(quantity));
    }

    std::string prefix()
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString(kEditorId));
        ESM4Test::appendSubRecord(payload, "FULL", zString(kFullName));
        return payload;
    }

    std::string completePayload(std::uint32_t category = kCategoryRaw)
    {
        std::string payload = prefix();
        ESM4Test::appendSubRecord(payload, "CTDA", condition(kConditionFunction));
        ESM4Test::appendSubRecord(
            payload, "CTDA", condition(kSecondConditionFunction, 0, kSecondConditionReferenceRaw));
        ESM4Test::appendSubRecord(payload, "DATA", data(category));
        appendIngredient(payload, kIngredientOneRaw, kFirstIngredientQuantity);
        appendIngredient(payload, kIngredientTwoRaw, kSecondIngredientQuantity);
        appendOutput(payload, kOutputOneRaw, kFirstOutputQuantity);
        appendOutput(payload, kOutputTwoRaw, kSecondOutputQuantity);
        return payload;
    }

    ESM4::Recipe loadRecipe(std::string payload, float version = ESM4Test::kFalloutPluginVersion)
    {
        auto reader = ESM4Test::makeReader("RCPE", std::move(payload), ESM4Test::kSyntheticModIndex, version);
        ESM4::Recipe recipe;
        recipe.load(*reader);
        return recipe;
    }

    TEST(Esm4RecipeTest, LoadsExactFalloutShapeAndAdjustsAuthoredFormIds)
    {
        const ESM4::Recipe recipe = loadRecipe(completePayload());

        EXPECT_EQ(recipe.mId, ESM::FormId::fromUint32(kExpectedFormId));
        EXPECT_EQ(recipe.mEditorId, kEditorId);
        EXPECT_EQ(recipe.mFullName, kFullName);
        EXPECT_EQ(recipe.mData.mRequiredSkill, kRequiredSkill);
        EXPECT_EQ(recipe.mData.mRequiredSkillLevel, kRequiredSkillLevel);
        EXPECT_EQ(recipe.mData.mCategory, ESM::FormId::fromUint32(kExpectedCategory));
        EXPECT_EQ(recipe.mData.mSubCategory, ESM::FormId::fromUint32(kExpectedSubCategory));

        ASSERT_EQ(recipe.mConditions.size(), 2);
        EXPECT_EQ(recipe.mConditions[0].functionIndex, kConditionFunction);
        EXPECT_EQ(recipe.mConditions[0].param1, kConditionParameter);
        EXPECT_EQ(recipe.mConditions[0].reference, kExpectedConditionReference);
        EXPECT_EQ(recipe.mConditions[1].functionIndex, kSecondConditionFunction);
        EXPECT_EQ(recipe.mConditions[1].reference, kExpectedSecondConditionReference);

        ASSERT_EQ(recipe.mIngredients.size(), 2);
        EXPECT_EQ(recipe.mIngredients[0].mItem, ESM::FormId::fromUint32(kExpectedIngredientOne));
        EXPECT_EQ(recipe.mIngredients[0].mQuantity, kFirstIngredientQuantity);
        EXPECT_EQ(recipe.mIngredients[1].mItem, ESM::FormId::fromUint32(kExpectedIngredientTwo));
        EXPECT_EQ(recipe.mIngredients[1].mQuantity, kSecondIngredientQuantity);
        ASSERT_EQ(recipe.mOutputs.size(), 2);
        EXPECT_EQ(recipe.mOutputs[0].mItem, ESM::FormId::fromUint32(kExpectedOutputOne));
        EXPECT_EQ(recipe.mOutputs[0].mQuantity, kFirstOutputQuantity);
        EXPECT_EQ(recipe.mOutputs[1].mItem, ESM::FormId::fromUint32(kExpectedOutputTwo));
        EXPECT_EQ(recipe.mOutputs[1].mQuantity, kSecondOutputQuantity);
    }

    TEST(Esm4RecipeTest, PreservesAuthoredNullCategory)
    {
        const ESM4::Recipe recipe = loadRecipe(completePayload(ESM4::Fallout::kRecipeNullFormId));

        EXPECT_TRUE(recipe.mData.mCategory.isZeroOrUnset());
        EXPECT_EQ(recipe.mData.mSubCategory, ESM::FormId::fromUint32(kExpectedSubCategory));
    }

    TEST(Esm4RecipeTest, RejectsMalformedRecordsWithoutMutatingTheDestination)
    {
        std::vector<std::string> malformed;
        malformed.emplace_back();

        std::string missingTerminator;
        ESM4Test::appendSubRecord(missingTerminator, "EDID", kEditorId);
        ESM4Test::appendSubRecord(missingTerminator, "FULL", zString(kFullName));
        ESM4Test::appendSubRecord(missingTerminator, "DATA", data());
        appendIngredient(missingTerminator, kIngredientOneRaw, kFirstIngredientQuantity);
        appendOutput(missingTerminator, kOutputOneRaw, kFirstOutputQuantity);
        malformed.push_back(std::move(missingTerminator));

        std::string fullFirst;
        ESM4Test::appendSubRecord(fullFirst, "FULL", zString(kFullName));
        ESM4Test::appendSubRecord(fullFirst, "EDID", zString(kEditorId));
        malformed.push_back(std::move(fullFirst));

        std::string zeroFull;
        ESM4Test::appendSubRecord(zeroFull, "EDID", zString(kEditorId));
        ESM4Test::appendSubRecord(zeroFull, "FULL", {});
        malformed.push_back(std::move(zeroFull));

        std::string duplicateEditor = prefix();
        ESM4Test::appendSubRecord(duplicateEditor, "EDID", zString("Again"));
        malformed.push_back(std::move(duplicateEditor));

        std::string conditionBeforeFull;
        ESM4Test::appendSubRecord(conditionBeforeFull, "EDID", zString(kEditorId));
        ESM4Test::appendSubRecord(conditionBeforeFull, "CTDA", condition(kConditionFunction));
        malformed.push_back(std::move(conditionBeforeFull));

        for (const std::size_t size : { kMalformedConditionBytes, kOversizedConditionBytes })
        {
            std::string badCondition = prefix();
            ESM4Test::appendSubRecord(badCondition, "CTDA", std::string(size, '\0'));
            malformed.push_back(std::move(badCondition));
        }

        std::string duplicateData = prefix();
        ESM4Test::appendSubRecord(duplicateData, "DATA", data());
        ESM4Test::appendSubRecord(duplicateData, "DATA", data());
        malformed.push_back(std::move(duplicateData));

        std::string shortData = prefix();
        ESM4Test::appendSubRecord(shortData, "DATA", std::string(kMalformedDataBytes, '\0'));
        malformed.push_back(std::move(shortData));

        std::string zeroSubCategory = prefix();
        ESM4Test::appendSubRecord(zeroSubCategory, "DATA", data(kCategoryRaw, ESM4::Fallout::kRecipeNullFormId));
        malformed.push_back(std::move(zeroSubCategory));

        std::string ingredientBeforeData = prefix();
        ESM4Test::appendSubRecord(ingredientBeforeData, "RCIL", pod(kIngredientOneRaw));
        malformed.push_back(std::move(ingredientBeforeData));

        std::string missingIngredientQuantity = prefix();
        ESM4Test::appendSubRecord(missingIngredientQuantity, "DATA", data());
        ESM4Test::appendSubRecord(missingIngredientQuantity, "RCIL", pod(kIngredientOneRaw));
        malformed.push_back(std::move(missingIngredientQuantity));

        std::string zeroIngredientQuantity = prefix();
        ESM4Test::appendSubRecord(zeroIngredientQuantity, "DATA", data());
        appendIngredient(zeroIngredientQuantity, kIngredientOneRaw, ESM4::Fallout::kRecipeNullFormId);
        malformed.push_back(std::move(zeroIngredientQuantity));

        std::string shortIngredient = prefix();
        ESM4Test::appendSubRecord(shortIngredient, "DATA", data());
        ESM4Test::appendSubRecord(shortIngredient, "RCIL", std::string(ESM4::Fallout::kRecipeItemBytes - 1, '\0'));
        malformed.push_back(std::move(shortIngredient));

        std::string outputBeforeIngredient = prefix();
        ESM4Test::appendSubRecord(outputBeforeIngredient, "DATA", data());
        appendOutput(outputBeforeIngredient, kOutputOneRaw, kFirstOutputQuantity);
        malformed.push_back(std::move(outputBeforeIngredient));

        std::string zeroOutput = prefix();
        ESM4Test::appendSubRecord(zeroOutput, "DATA", data());
        appendIngredient(zeroOutput, kIngredientOneRaw, kFirstIngredientQuantity);
        appendOutput(zeroOutput, ESM4::Fallout::kRecipeNullFormId, kFirstOutputQuantity);
        malformed.push_back(std::move(zeroOutput));

        std::string missingOutputQuantity = prefix();
        ESM4Test::appendSubRecord(missingOutputQuantity, "DATA", data());
        appendIngredient(missingOutputQuantity, kIngredientOneRaw, kFirstIngredientQuantity);
        ESM4Test::appendSubRecord(missingOutputQuantity, "RCOD", pod(kOutputOneRaw));
        malformed.push_back(std::move(missingOutputQuantity));

        std::string zeroOutputQuantity = prefix();
        ESM4Test::appendSubRecord(zeroOutputQuantity, "DATA", data());
        appendIngredient(zeroOutputQuantity, kIngredientOneRaw, kFirstIngredientQuantity);
        appendOutput(zeroOutputQuantity, kOutputOneRaw, ESM4::Fallout::kRecipeNullFormId);
        malformed.push_back(std::move(zeroOutputQuantity));

        std::string ingredientAfterOutput = prefix();
        ESM4Test::appendSubRecord(ingredientAfterOutput, "DATA", data());
        appendIngredient(ingredientAfterOutput, kIngredientOneRaw, kFirstIngredientQuantity);
        appendOutput(ingredientAfterOutput, kOutputOneRaw, kFirstOutputQuantity);
        appendIngredient(ingredientAfterOutput, kIngredientTwoRaw, kSecondIngredientQuantity);
        malformed.push_back(std::move(ingredientAfterOutput));

        std::string unknown = prefix();
        ESM4Test::appendSubRecord(unknown, "ICON", zString("not-authored.dds"));
        malformed.push_back(std::move(unknown));

        std::string trailingCondition = completePayload();
        ESM4Test::appendSubRecord(trailingCondition, "CTDA", condition(kConditionFunction));
        malformed.push_back(std::move(trailingCondition));

        std::string trailingQuantity = completePayload();
        ESM4Test::appendSubRecord(trailingQuantity, "RCQY", pod(kFirstOutputQuantity));
        malformed.push_back(std::move(trailingQuantity));

        for (std::size_t index = 0; index < malformed.size(); ++index)
        {
            SCOPED_TRACE(index);
            auto reader = ESM4Test::makeReader("RCPE", std::move(malformed[index]));
            ESM4::Recipe recipe;
            recipe.mEditorId = "sentinel";
            recipe.mFullName = "unchanged";
            recipe.mData.mRequiredSkill = kSentinelRequiredSkill;
            recipe.mConditions.push_back(ESM4::TargetCondition{});
            recipe.mIngredients.push_back({ ESM::FormId::fromUint32(kExpectedIngredientOne), kFirstIngredientQuantity });
            recipe.mOutputs.push_back({ ESM::FormId::fromUint32(kExpectedOutputOne), kFirstOutputQuantity });

            EXPECT_THROW(recipe.load(*reader), std::runtime_error);
            EXPECT_EQ(recipe.mEditorId, "sentinel");
            EXPECT_EQ(recipe.mFullName, "unchanged");
            EXPECT_EQ(recipe.mData.mRequiredSkill, kSentinelRequiredSkill);
            ASSERT_EQ(recipe.mConditions.size(), 1);
            ASSERT_EQ(recipe.mIngredients.size(), 1);
            ASSERT_EQ(recipe.mOutputs.size(), 1);
            EXPECT_EQ(recipe.mIngredients.front().mQuantity, kFirstIngredientQuantity);
            EXPECT_EQ(recipe.mOutputs.front().mQuantity, kFirstOutputQuantity);
        }
    }

    TEST(Esm4RecipeTest, RejectsNonFalloutVersions)
    {
        EXPECT_THROW(loadRecipe(completePayload(), ESM4Test::kOtherPluginVersion), std::runtime_error);
    }
}
