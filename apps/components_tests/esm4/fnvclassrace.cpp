#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>

#include <components/esm4/loadclas.hpp>
#include <components/esm4/loadrace.hpp>

namespace
{
    TEST(FalloutClassRaceDataTest, ClassDataUsesExactNativeLayout)
    {
        constexpr std::array<std::uint8_t, 28> payload{ 0x20, 0, 0, 0, 0x29, 0, 0, 0, 0x2b, 0, 0, 0,
            0xff, 0xff, 0xff, 0xff, 0x1d, 0, 0, 0, 0x01, 0x40, 0x02, 0, 0xff, 50, 0xaa, 0x55 };

        const ESM4::Class::Data data = ESM4::Class::decodeFalloutData(payload);
        EXPECT_EQ(data.mTagActorValues, (std::array<std::int32_t, 4>{ 32, 41, 43, -1 }));
        EXPECT_EQ(data.mRawFlags, 0x1du);
        EXPECT_EQ(data.mRawServices, 0x00024001u);
        EXPECT_EQ(data.mRawTeaches, 0xffu);
        EXPECT_EQ(data.mTrainingLevel, 50u);
        EXPECT_EQ(data.mReserved, (std::array<std::uint8_t, 2>{ 0xaa, 0x55 }));
        EXPECT_THROW(ESM4::Class::decodeFalloutData(std::span(payload).first<27>()), std::runtime_error);
    }

    TEST(FalloutClassRaceDataTest, RaceDataPreservesSevenSignedBoostsAndReservedBytes)
    {
        constexpr std::array<std::uint8_t, 36> payload{ 32, 5, 41, 10, 43, 0xf9, 45, 3, 0xff, 0, 36, 2, 44,
            4, 0xaa, 0x55, 0, 0, 0x80, 0x3f, 0, 0, 0xa0, 0x3f, 0, 0, 0x40, 0x3f, 0, 0, 0, 0x40, 5, 0, 0, 0 };

        const ESM4::Race::Data data = ESM4::Race::decodeFalloutData(payload);
        EXPECT_EQ(data.mSkillBoosts[0].mRawActorValue, 32u);
        EXPECT_EQ(data.mSkillBoosts[2].mBoost, -7);
        EXPECT_EQ(data.mSkillBoosts[6].mRawActorValue, 44u);
        EXPECT_EQ(data.mReserved, (std::array<std::uint8_t, 2>{ 0xaa, 0x55 }));
        EXPECT_FLOAT_EQ(data.mHeightMale, 1.0f);
        EXPECT_FLOAT_EQ(data.mHeightFemale, 1.25f);
        EXPECT_FLOAT_EQ(data.mWeightMale, 0.75f);
        EXPECT_FLOAT_EQ(data.mWeightFemale, 2.0f);
        EXPECT_EQ(data.mRawFlags, 5u);
        EXPECT_THROW(ESM4::Race::decodeFalloutData(std::span(payload).first<35>()), std::runtime_error);
    }

    TEST(FalloutClassRaceDataTest, AbsentPayloadsHaveDefinedFailClosedState)
    {
        const ESM4::Class nativeClass;
        EXPECT_FALSE(nativeClass.mHasFalloutData);
        EXPECT_FALSE(nativeClass.mHasFalloutAttributes);
        EXPECT_EQ(nativeClass.mFlags, 0u);

        const ESM4::Race nativeRace;
        EXPECT_FALSE(nativeRace.mHasFalloutData);
        EXPECT_EQ(nativeRace.mRaceFlags, 0u);
        EXPECT_FLOAT_EQ(nativeRace.mHeightMale, 1.0f);
        EXPECT_FLOAT_EQ(nativeRace.mFaceGenMainClamp, 0.0f);
        EXPECT_EQ(nativeRace.mNumKeywords, 0u);
    }
}
