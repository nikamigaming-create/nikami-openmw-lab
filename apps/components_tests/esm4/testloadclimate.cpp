#include <components/esm4/loadclmt.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace
{
    constexpr std::string_view sDefaultClimateHex
        = "4544494411004e5644656661756c74436c696d61746500574c5354180074651700640000003765170088fc0f006400000000000000464e414d0c0053"
          "6b795c53756e2e64647300474e414d1400736b795c4e565f73756e676c6172652e646473004d4f444c0e00536b795c53746172732e6e696600544e41"
          "4d060024306c780083";

    std::uint8_t decodeNibble(char value)
    {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<std::uint8_t>(value - 'a' + 10);
        throw std::runtime_error("Invalid hexadecimal fixture");
    }

    std::string decodeHex(std::string_view value)
    {
        if (value.size() % 2 != 0)
            throw std::runtime_error("Odd hexadecimal fixture length");
        std::string result(value.size() / 2, '\0');
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = static_cast<char>((decodeNibble(value[i * 2]) << 4) | decodeNibble(value[i * 2 + 1]));
        return result;
    }

    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        if (type.size() != 4 || data.size() > std::numeric_limits<std::uint16_t>::max())
            throw std::runtime_error("Invalid test subrecord");
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    std::unique_ptr<ESM4::Reader> makeReader(std::string body, std::uint32_t modIndex = 0)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerBody;
        appendSubRecord(headerBody, "HEDR", hedr);

        const auto appendRecord = [](std::string& output, std::string_view type, std::uint32_t id,
                                      std::string_view payload, std::uint16_t version) {
            output.append(type);
            appendPod(output, static_cast<std::uint32_t>(payload.size()));
            appendPod(output, std::uint32_t{ 0 });
            appendPod(output, id);
            appendPod(output, std::uint32_t{ 0 });
            appendPod(output, version);
            appendPod(output, std::uint16_t{ 0 });
            output.append(payload);
        };

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerBody, 0);
        appendRecord(plugin, "CLMT", 0x0008809b, body, 15);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "climate.esm", nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        if (!reader->getRecordHeader())
            throw std::runtime_error("Missing CLMT test record");
        reader->getRecordData();
        return reader;
    }

    ESM4::Climate loadClimate(std::string body, std::uint32_t modIndex = 0)
    {
        auto reader = makeReader(std::move(body), modIndex);
        ESM4::Climate climate;
        climate.load(*reader);
        return climate;
    }

    TEST(Esm4ClimateTest, LoadsExactNVDefaultClimateRecord)
    {
        const std::string body = decodeHex(sDefaultClimateHex);
        ASSERT_EQ(body.size(), 129);
        const ESM4::Climate climate = loadClimate(body, 3);

        EXPECT_EQ(climate.mId, ESM::FormId::fromUint32(0x0308809b));
        EXPECT_EQ(climate.mEditorId, "NVDefaultClimate");
        ASSERT_EQ(climate.mWeatherTypes.size(), 2u);
        EXPECT_EQ(climate.mWeatherTypes[0].mWeather, ESM::FormId::fromUint32(0x03176574));
        EXPECT_EQ(climate.mWeatherTypes[0].mChance, 100);
        EXPECT_EQ(climate.mWeatherTypes[0].mGlobal, ESM::FormId::fromUint32(0x03176537));
        EXPECT_EQ(climate.mWeatherTypes[1].mWeather, ESM::FormId::fromUint32(0x030ffc88));
        EXPECT_EQ(climate.mWeatherTypes[1].mChance, 100);
        EXPECT_TRUE(climate.mWeatherTypes[1].mGlobal.isZeroOrUnset());
        EXPECT_EQ(climate.mSunTexture, "Sky\\Sun.dds");
        EXPECT_EQ(climate.mSunGlareTexture, "sky\\NV_sunglare.dds");
        EXPECT_EQ(climate.mNightSkyModel, "Sky\\Stars.nif");
        EXPECT_TRUE(climate.mHasTiming);
        EXPECT_EQ(climate.mTiming.mSunriseBegin, 36u);
        EXPECT_EQ(climate.mTiming.mSunriseEnd, 48u);
        EXPECT_EQ(climate.mTiming.mSunsetBegin, 108u);
        EXPECT_EQ(climate.mTiming.mSunsetEnd, 120u);
        EXPECT_EQ(climate.mTiming.mVolatility, 0u);
        EXPECT_EQ(climate.mTiming.mMoonInfo, 0x83u);
        EXPECT_EQ(climate.mTiming.getMoonPhaseLength(), 3u);
        EXPECT_TRUE(climate.mTiming.hasMasser());
        EXPECT_FALSE(climate.mTiming.hasSecunda());
    }

    TEST(Esm4ClimateTest, LeavesOptionalTimingAndWeatherListAbsent)
    {
        const ESM4::Climate climate = loadClimate({});
        EXPECT_TRUE(climate.mWeatherTypes.empty());
        EXPECT_FALSE(climate.mHasTiming);
    }

    TEST(Esm4ClimateTest, RejectsMalformedWeatherListAndTiming)
    {
        std::string weatherList;
        appendSubRecord(weatherList, "WLST", std::string(13, '\0'));
        EXPECT_THROW(loadClimate(std::move(weatherList)), std::runtime_error);

        std::string timing;
        appendSubRecord(timing, "TNAM", std::string(5, '\0'));
        EXPECT_THROW(loadClimate(std::move(timing)), std::runtime_error);
    }
}
