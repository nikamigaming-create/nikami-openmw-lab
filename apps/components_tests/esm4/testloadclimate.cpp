#include <components/esm4/loadclmt.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "testutil.hpp"

namespace
{
    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    ESM4::Climate loadClimate(std::string payload)
    {
        auto reader = ESM4Test::makeReader("CLMT", std::move(payload));
        ESM4::Climate climate;
        climate.load(*reader);
        return climate;
    }

    TEST(Esm4ClimateTest, loadsFalloutWeatherListTexturesAndTiming)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString("NVDefaultClimate"));

        std::string weatherList;
        ESM4Test::appendPod(weatherList, std::uint32_t{ 0x00176574 });
        ESM4Test::appendPod(weatherList, std::int32_t{ 100 });
        ESM4Test::appendPod(weatherList, std::uint32_t{ 0x00176537 });
        ESM4Test::appendPod(weatherList, std::uint32_t{ 0x000ffc88 });
        ESM4Test::appendPod(weatherList, std::int32_t{ 100 });
        ESM4Test::appendPod(weatherList, std::uint32_t{});
        ESM4Test::appendSubRecord(payload, "WLST", weatherList);

        ESM4Test::appendSubRecord(payload, "FNAM", zString("Sky\\Sun.dds"));
        ESM4Test::appendSubRecord(payload, "GNAM", zString("sky\\NV_sunglare.dds"));
        ESM4Test::appendSubRecord(payload, "MODL", zString("Sky\\Stars.nif"));
        const std::array<std::uint8_t, ESM4::Climate::sTimingSerializedSize> timing{ 36, 48, 108, 120, 0, 0x83 };
        ESM4Test::appendSubRecord(payload, "TNAM", std::string(reinterpret_cast<const char*>(timing.data()), timing.size()));

        const ESM4::Climate climate = loadClimate(std::move(payload));

        EXPECT_EQ(climate.mId, ESM::FormId::fromUint32(0x02123456));
        EXPECT_EQ(climate.mEditorId, "NVDefaultClimate");
        ASSERT_EQ(climate.mWeatherTypes.size(), 2u);
        EXPECT_EQ(climate.mWeatherTypes[0].mWeather, ESM::FormId::fromUint32(0x02176574));
        EXPECT_EQ(climate.mWeatherTypes[0].mChance, 100);
        EXPECT_EQ(climate.mWeatherTypes[0].mGlobal, ESM::FormId::fromUint32(0x02176537));
        EXPECT_EQ(climate.mWeatherTypes[1].mWeather, ESM::FormId::fromUint32(0x020ffc88));
        EXPECT_TRUE(climate.mWeatherTypes[1].mGlobal.isZeroOrUnset());
        EXPECT_EQ(climate.mSunTexture, "Sky\\Sun.dds");
        EXPECT_EQ(climate.mSunGlareTexture, "sky\\NV_sunglare.dds");
        EXPECT_EQ(climate.mNightSkyModel, "Sky\\Stars.nif");
        ASSERT_TRUE(climate.mHasTiming);
        EXPECT_EQ(climate.mTiming.mSunriseBegin, 36);
        EXPECT_EQ(climate.mTiming.mSunsetEnd, 120);
        EXPECT_EQ(climate.mTiming.getMoonPhaseLength(), 3);
        EXPECT_TRUE(climate.mTiming.hasMasser());
        EXPECT_FALSE(climate.mTiming.hasSecunda());
    }

    TEST(Esm4ClimateTest, rejectsMalformedFixedSizeSubrecords)
    {
        std::string weatherList;
        ESM4Test::appendSubRecord(weatherList, "WLST", std::string(ESM4::Climate::sWeatherTypeSerializedSize - 1, '\0'));
        EXPECT_THROW(loadClimate(std::move(weatherList)), std::runtime_error);

        std::string timing;
        ESM4Test::appendSubRecord(timing, "TNAM", std::string(ESM4::Climate::sTimingSerializedSize - 1, '\0'));
        EXPECT_THROW(loadClimate(std::move(timing)), std::runtime_error);
    }
}
