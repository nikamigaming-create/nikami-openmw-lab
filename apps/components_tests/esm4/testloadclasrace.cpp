#include <components/esm4/loadclas.hpp>
#include <components/esm4/loadrace.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "testutil.hpp"

namespace
{
    constexpr std::array<std::int32_t, ESM4::Fallout::kClassFalloutTagActorValueCount> kClassTagActorValues{
        32, 41, 43, -1
    };
    constexpr std::uint32_t kClassFlags = 0x1d;
    constexpr std::uint32_t kClassServices = 0x00024001;
    constexpr std::uint8_t kClassTeaches = 0xff;
    constexpr std::uint8_t kClassTrainingLevel = 50;
    constexpr std::array<std::uint8_t, ESM4::Fallout::kClassFalloutReservedBytes> kClassReserved{ 0xaa, 0x55 };
    constexpr std::array<std::uint8_t, ESM4::Fallout::kClassFalloutAttributesBytes> kClassAttributes{
        1, 3, 5, 7, 9, 11, 13
    };
    constexpr std::size_t kMalformedClassDataBytes = ESM4::Fallout::kClassFalloutDataBytes - 1;

    constexpr std::array<std::uint8_t, ESM4::Fallout::kRaceFalloutSkillBoostCount * 2> kRaceSkillBoosts{
        32, 5, 41, 10, 43, 0xf9, 45, 3, 0xff, 0, 36, 2, 44, 4
    };
    constexpr std::array<std::uint8_t, ESM4::Fallout::kRaceFalloutReservedBytes> kRaceReserved{ 0xaa, 0x55 };
    constexpr float kRaceHeightMale = 1.0f;
    constexpr float kRaceHeightFemale = 1.25f;
    constexpr float kRaceWeightMale = 0.75f;
    constexpr float kRaceWeightFemale = 2.0f;
    constexpr std::uint32_t kRaceFlags = 5;
    constexpr std::size_t kMalformedRaceDataBytes = ESM4::Fallout::kRaceFalloutDataBytes - 1;
    constexpr std::string_view kEditorId = "FalloutClassRace";

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    std::span<const std::uint8_t> byteSpan(const std::string& value)
    {
        return { reinterpret_cast<const std::uint8_t*>(value.data()), value.size() };
    }

    std::string makeClassData()
    {
        std::string result;
        for (const std::int32_t value : kClassTagActorValues)
            ESM4Test::appendPod(result, value);
        ESM4Test::appendPod(result, kClassFlags);
        ESM4Test::appendPod(result, kClassServices);
        ESM4Test::appendPod(result, kClassTeaches);
        ESM4Test::appendPod(result, kClassTrainingLevel);
        for (const std::uint8_t value : kClassReserved)
            ESM4Test::appendPod(result, value);
        return result;
    }

    std::string makeRaceData()
    {
        std::string result;
        for (const std::uint8_t value : kRaceSkillBoosts)
            ESM4Test::appendPod(result, value);
        for (const std::uint8_t value : kRaceReserved)
            ESM4Test::appendPod(result, value);
        ESM4Test::appendPod(result, kRaceHeightMale);
        ESM4Test::appendPod(result, kRaceHeightFemale);
        ESM4Test::appendPod(result, kRaceWeightMale);
        ESM4Test::appendPod(result, kRaceWeightFemale);
        ESM4Test::appendPod(result, kRaceFlags);
        return result;
    }

    TEST(Esm4ClassRaceDataTest, LoadsFalloutClassDataAndAttributes)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "DATA", makeClassData());
        std::string attributes;
        for (const std::uint8_t value : kClassAttributes)
            ESM4Test::appendPod(attributes, value);
        ESM4Test::appendSubRecord(recordData, "ATTR", attributes);
        ESM4Test::appendSubRecord(recordData, "EDID", zString(kEditorId));

        auto reader = ESM4Test::makeReader("CLAS", std::move(recordData));
        ESM4::Class record;
        record.load(*reader);

        ASSERT_TRUE(record.mHasFalloutData);
        EXPECT_EQ(record.mFalloutData.mTagActorValues, kClassTagActorValues);
        EXPECT_EQ(record.mFalloutData.mRawFlags, kClassFlags);
        EXPECT_EQ(record.mFalloutData.mRawServices, kClassServices);
        EXPECT_EQ(record.mFalloutData.mRawTeaches, kClassTeaches);
        EXPECT_EQ(record.mFalloutData.mTrainingLevel, kClassTrainingLevel);
        EXPECT_EQ(record.mFalloutData.mReserved, kClassReserved);
        ASSERT_TRUE(record.mHasFalloutAttributes);
        EXPECT_EQ(record.mFalloutAttributes, kClassAttributes);
        EXPECT_EQ(record.mEditorId, kEditorId);
    }

    TEST(Esm4ClassRaceDataTest, RejectsMalformedClassData)
    {
        std::string malformed(kMalformedClassDataBytes, '\0');
        EXPECT_THROW(ESM4::Class::decodeFalloutData(byteSpan(malformed)), std::runtime_error);
    }

    TEST(Esm4ClassRaceDataTest, RejectsMalformedClassDataInFullReader)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "DATA", std::string(kMalformedClassDataBytes, '\0'));
        ESM4Test::appendSubRecord(recordData, "EDID", zString(kEditorId));
        auto reader = ESM4Test::makeReader("CLAS", std::move(recordData));
        ESM4::Class record;
        EXPECT_THROW(record.load(*reader), std::runtime_error);
    }

    TEST(Esm4ClassRaceDataTest, LoadsFalloutRaceDataAndPublishesExistingScalarFields)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "DATA", makeRaceData());
        ESM4Test::appendSubRecord(recordData, "EDID", zString(kEditorId));

        auto reader = ESM4Test::makeReader("RACE", std::move(recordData));
        ESM4::Race record;
        record.load(*reader);

        ASSERT_TRUE(record.mHasFalloutData);
        EXPECT_EQ(record.mFalloutData.mSkillBoosts[0].mRawActorValue, 32);
        EXPECT_EQ(record.mFalloutData.mSkillBoosts[2].mBoost, -7);
        EXPECT_EQ(record.mFalloutData.mSkillBoosts[6].mRawActorValue, 44);
        EXPECT_EQ(record.mFalloutData.mReserved, kRaceReserved);
        EXPECT_FLOAT_EQ(record.mHeightMale, kRaceHeightMale);
        EXPECT_FLOAT_EQ(record.mHeightFemale, kRaceHeightFemale);
        EXPECT_FLOAT_EQ(record.mWeightMale, kRaceWeightMale);
        EXPECT_FLOAT_EQ(record.mWeightFemale, kRaceWeightFemale);
        EXPECT_EQ(record.mRaceFlags, kRaceFlags);
        EXPECT_EQ(record.mEditorId, kEditorId);
    }

    TEST(Esm4ClassRaceDataTest, RejectsMalformedRaceData)
    {
        std::string malformed(kMalformedRaceDataBytes, '\0');
        EXPECT_THROW(ESM4::Race::decodeFalloutData(byteSpan(malformed)), std::runtime_error);
    }

    TEST(Esm4ClassRaceDataTest, RejectsMalformedRaceDataInFullReader)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "DATA", std::string(kMalformedRaceDataBytes, '\0'));
        ESM4Test::appendSubRecord(recordData, "EDID", zString(kEditorId));
        auto reader = ESM4Test::makeReader("RACE", std::move(recordData));
        ESM4::Race record;
        EXPECT_THROW(record.load(*reader), std::runtime_error);
    }

    TEST(Esm4ClassRaceDataTest, SkipsFalloutPayloadsForOtherEsmVersions)
    {
        std::string classData;
        ESM4Test::appendSubRecord(classData, "DATA", makeClassData());
        ESM4Test::appendSubRecord(classData, "ATTR", std::string(kClassAttributes.begin(), kClassAttributes.end()));
        ESM4Test::appendSubRecord(classData, "EDID", zString(kEditorId));
        auto classReader = ESM4Test::makeReader("CLAS", std::move(classData), ESM4Test::kSyntheticModIndex,
            ESM4Test::kOtherPluginVersion);
        ESM4::Class classRecord;
        classRecord.load(*classReader);
        EXPECT_FALSE(classRecord.mHasFalloutData);
        EXPECT_FALSE(classRecord.mHasFalloutAttributes);
        EXPECT_EQ(classRecord.mEditorId, kEditorId);

        std::string raceData;
        ESM4Test::appendSubRecord(raceData, "DATA", makeRaceData());
        ESM4Test::appendSubRecord(raceData, "EDID", zString(kEditorId));
        auto raceReader = ESM4Test::makeReader("RACE", std::move(raceData), ESM4Test::kSyntheticModIndex,
            ESM4Test::kOtherPluginVersion);
        ESM4::Race raceRecord;
        raceRecord.load(*raceReader);
        EXPECT_FALSE(raceRecord.mHasFalloutData);
        EXPECT_EQ(raceRecord.mEditorId, kEditorId);
    }
}
