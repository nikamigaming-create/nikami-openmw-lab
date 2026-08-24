#include <components/esm4/loadwthr.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "testutil.hpp"

namespace
{
    using namespace std::literals;

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    template <class T>
    std::string podData(const T& value)
    {
        std::string result;
        ESM4Test::appendPod(result, value);
        return result;
    }

    ESM4::Weather loadWeather(std::string payload)
    {
        auto reader = ESM4Test::makeReader("WTHR", std::move(payload));
        ESM4::Weather weather;
        weather.load(*reader);
        return weather;
    }

    TEST(Esm4WeatherTest, loadsFalloutColorsImageSpacesFogAndData)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString("NVWastelandGS"));
        ESM4Test::appendSubRecord(payload, "DNAM", zString("textures/sky/layer0.dds"));
        ESM4Test::appendSubRecord(payload, "CNAM", zString("textures/sky/layer1.dds"));
        ESM4Test::appendSubRecord(payload, "ANAM", zString("textures/sky/layer2.dds"));
        ESM4Test::appendSubRecord(payload, "BNAM", zString("textures/sky/layer3.dds"));
        ESM4Test::appendSubRecord(payload, "MODL", zString("meshes/weather.nif"));
        ESM4Test::appendSubRecord(payload, "LNAM", podData(std::uint32_t{ 4 }));
        ESM4Test::appendSubRecord(payload, std::string_view("\0IAD", 4), podData(std::uint32_t{ 0x0cee1a }));
        ESM4Test::appendSubRecord(payload, std::string_view("\1IAD", 4), podData(std::uint32_t{ 0x0cee18 }));
        ESM4Test::appendSubRecord(payload, std::string_view("\4IAD", 4), podData(std::uint32_t{ 0x0cee18 }));

        const std::string unusedImageSpace(ESM4::Weather::sUnusedImageSpaceDataSize, '\x7f');
        ESM4Test::appendSubRecord(payload, "INAM", unusedImageSpace);

        const std::array<std::uint8_t, ESM4::Weather::sCloudLayerCount> cloudSpeeds{ 25, 50, 75, 100 };
        ESM4Test::appendSubRecord(payload, "ONAM", podData(cloudSpeeds));
        std::array<std::array<ESM4::Weather::Color, ESM4::Weather::sTimeCount>,
            ESM4::Weather::sCloudLayerCount>
            cloudColors{};
        cloudColors[ESM4::Weather::CloudLayer_Upper][ESM4::Weather::Time_HighNoon] = { 10, 20, 30, 40 };
        ESM4Test::appendSubRecord(payload, "PNAM", podData(cloudColors));

        std::array<std::array<ESM4::Weather::Color, ESM4::Weather::sTimeCount>, ESM4::Weather::sColorTypeCount>
            colors{};
        colors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Day] = { 87, 105, 138, 0 };
        colors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_HighNoon] = { 99, 120, 154, 0 };
        colors[ESM4::Weather::Color_Sunlight][ESM4::Weather::Time_Day] = { 255, 227, 170, 0 };
        ESM4Test::appendSubRecord(payload, "NAM0", podData(colors));

        std::array<float, ESM4::Weather::sFogDistanceCount> fog{ 10.f, 120000.f, 0.f, 150000.f, 0.5f, 0.5f };
        ESM4Test::appendSubRecord(payload, "FNAM", podData(fog));
        std::array<std::uint8_t, ESM4::Weather::sDataSerializedSize>
            data{ 50, 45, 45, 255, 54, 0, 0, 0, 0, 0, 255, 1, 0, 0, 0 };
        ESM4Test::appendSubRecord(payload, "DATA", podData(data));
        std::string sound;
        ESM4Test::appendPod(sound, std::uint32_t{ 0x0cee20 });
        ESM4Test::appendPod(sound, std::uint32_t{ 3 });
        ESM4Test::appendSubRecord(payload, "SNAM", sound);

        const ESM4::Weather weather = loadWeather(std::move(payload));

        EXPECT_EQ(weather.mId, ESM::FormId::fromUint32(0x02123456));
        EXPECT_EQ(weather.mEditorId, "NVWastelandGS");
        EXPECT_EQ(weather.mCloudTextures[ESM4::Weather::CloudLayer_Lower], "textures/sky/layer0.dds");
        EXPECT_EQ(weather.mCloudTextures[ESM4::Weather::CloudLayer_Sky], "textures/sky/layer3.dds");
        EXPECT_EQ(weather.mModel, "meshes/weather.nif");
        EXPECT_TRUE(weather.mHasMaxCloudLayers);
        EXPECT_EQ(weather.mMaxCloudLayers, 4u);
        EXPECT_EQ(weather.mImageSpaceModifiers[ESM4::Weather::Time_Sunrise],
            ESM::FormId::fromUint32(0x020cee1a));
        EXPECT_TRUE(weather.mHasUnusedImageSpaceData);
        EXPECT_EQ(weather.mUnusedImageSpaceData.front(), 0x7f);
        EXPECT_TRUE(weather.mHasCloudSpeeds);
        EXPECT_TRUE(weather.mHasCloudColors);
        EXPECT_EQ(weather.mCloudColorSampleCount, ESM4::Weather::sTimeCount);
        EXPECT_EQ(weather.mCloudSpeeds[ESM4::Weather::CloudLayer_Upper], 75);
        EXPECT_EQ(weather.mCloudColors[ESM4::Weather::CloudLayer_Upper][ESM4::Weather::Time_HighNoon].unused, 40);
        EXPECT_TRUE(weather.mHasColors);
        EXPECT_EQ(weather.mColorSampleCount, ESM4::Weather::sTimeCount);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Day].r, 87);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_HighNoon].b, 154);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Sunlight][ESM4::Weather::Time_Day].g, 227);
        EXPECT_FLOAT_EQ(weather.mFogDistance[1], 120000.f);
        EXPECT_EQ(weather.mData.transitionDelta, 255);
        EXPECT_EQ(weather.mData.classification, 1);
        EXPECT_TRUE(weather.mHasData);
        ASSERT_EQ(weather.mSounds.size(), 1);
        EXPECT_EQ(weather.mSounds.front().sound, ESM::FormId::fromUint32(0x020cee20));
        EXPECT_EQ(weather.mSounds.front().type, 3u);
    }

    TEST(Esm4WeatherTest, expandsLegacyFourTimeColorRowsWithoutInventingHighNoon)
    {
        std::string payload;
        std::array<std::array<ESM4::Weather::Color, ESM4::Weather::sLegacyTimeCount>,
            ESM4::Weather::sColorTypeCount>
            colors{};
        colors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Night] = { 12, 34, 56, 0 };
        ESM4Test::appendSubRecord(payload, "NAM0", podData(colors));

        const ESM4::Weather weather = loadWeather(std::move(payload));

        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Night].b, 56);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_HighNoon].r, 0);
    }

    TEST(Esm4WeatherTest, expandsLegacyFourTimeCloudRowsWithoutInventingHighNoon)
    {
        std::string payload;
        std::array<std::array<ESM4::Weather::Color, ESM4::Weather::sLegacyTimeCount>,
            ESM4::Weather::sCloudLayerCount>
            colors{};
        colors[ESM4::Weather::CloudLayer_Upper][ESM4::Weather::Time_Night] = { 12, 34, 56, 0 };
        ESM4Test::appendSubRecord(payload, "PNAM", podData(colors));

        const ESM4::Weather weather = loadWeather(std::move(payload));

        EXPECT_TRUE(weather.mHasCloudColors);
        EXPECT_EQ(weather.mCloudColorSampleCount, ESM4::Weather::sLegacyTimeCount);
        EXPECT_EQ(weather.mCloudColors[ESM4::Weather::CloudLayer_Upper][ESM4::Weather::Time_Night].b, 56);
        EXPECT_EQ(weather.mCloudColors[ESM4::Weather::CloudLayer_Upper][ESM4::Weather::Time_HighNoon].r, 0);
    }

    TEST(Esm4WeatherTest, rejectsMalformedFixedSizeSubrecords)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "SNAM", std::string(ESM4::Weather::sDataSerializedSize, '\0'));
        EXPECT_THROW(loadWeather(std::move(payload)), std::runtime_error);
    }
}
