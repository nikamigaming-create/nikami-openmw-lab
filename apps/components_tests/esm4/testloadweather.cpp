#include <components/esm4/loadwthr.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace
{
    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void appendFourCC(std::string& output, std::string_view value)
    {
        ASSERT_EQ(value.size(), 4);
        output.append(value);
    }

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        ASSERT_EQ(type.size(), 4);
        ASSERT_LE(data.size(), std::numeric_limits<std::uint16_t>::max());
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    template <class T>
    void appendSubRecord(std::string& output, std::string_view type, const T& data)
    {
        appendSubRecord(output, type, std::string_view(reinterpret_cast<const char*>(&data), sizeof(data)));
    }

    void appendSubRecord(std::string& output, std::string_view type, const std::string& data)
    {
        appendSubRecord(output, type, std::string_view(data));
    }

    std::unique_ptr<ESM4::Reader> makeReader(
        std::string payload, std::uint32_t modIndex = 0, std::uint16_t formVersion = 0)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);

        const auto appendRecord = [](std::string& output, std::string_view type, std::uint32_t formId,
                                      std::string_view body, std::uint16_t version) {
            output.append(type);
            appendPod(output, static_cast<std::uint32_t>(body.size()));
            appendPod(output, std::uint32_t{ 0 });
            appendPod(output, formId);
            appendPod(output, std::uint32_t{ 0 });
            appendPod(output, version);
            appendPod(output, std::uint16_t{ 0 });
            output.append(body);
        };

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload, 0);
        appendRecord(plugin, "WTHR", 0x1237d7, payload, formVersion);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "weather.esm", nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    TEST(Esm4WeatherTest, shouldParseRetailFNVWeatherColorsAndImageSpaces)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", std::string_view("NVWastelandGS\0", 14));
        appendSubRecord(payload, std::string_view("\0IAD", 4), std::uint32_t{ 0x0cee1a });
        appendSubRecord(payload, std::string_view("\1IAD", 4), std::uint32_t{ 0x0cee18 });
        appendSubRecord(payload, std::string_view("\4IAD", 4), std::uint32_t{ 0x0cee18 });
        appendSubRecord(payload, "DNAM", std::string_view("textures/sky/layer0.dds\0", 24));
        appendSubRecord(payload, "CNAM", std::string_view("textures/sky/layer1.dds\0", 24));
        const std::array<std::uint8_t, 4> cloudSpeeds{ 25, 50, 75, 100 };
        appendSubRecord(payload, "ONAM", cloudSpeeds);
        std::array<std::array<ESM4::Weather::Color, ESM4::Weather::sTimeCount>,
            ESM4::Weather::sCloudLayerCount>
            cloudColors{};
        cloudColors[2][ESM4::Weather::Time_HighNoon] = { 10, 20, 30, 40 };
        appendSubRecord(payload, "PNAM", cloudColors);

        std::array<std::array<ESM4::Weather::Color, ESM4::Weather::sTimeCount>,
            ESM4::Weather::sColorTypeCount>
            colors{};
        colors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Day] = { 87, 105, 138, 0 };
        colors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_HighNoon] = { 99, 120, 154, 0 };
        colors[ESM4::Weather::Color_Sunlight][ESM4::Weather::Time_Day] = { 255, 227, 170, 0 };
        appendSubRecord(payload, "NAM0", colors);

        std::array<float, 6> fog{ 10.f, 120000.f, 0.f, 150000.f, 0.5f, 0.5f };
        appendSubRecord(payload, "FNAM", fog);
        std::array<std::uint8_t, 15> data{ 50, 45, 45, 255, 54, 0, 0, 0, 0, 0, 255, 1, 0, 0, 0 };
        appendSubRecord(payload, "DATA", data);

        auto reader = makeReader(payload, 2);
        ESM4::Weather weather;
        weather.load(*reader);

        EXPECT_EQ(weather.mId, ESM::FormId::fromUint32(0x021237d7));
        EXPECT_EQ(weather.mEditorId, "NVWastelandGS");
        EXPECT_EQ(weather.mImageSpaceModifiers[ESM4::Weather::Time_Sunrise],
            ESM::FormId::fromUint32(0x020cee1a));
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Day].r, 87);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_HighNoon].b, 154);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Sunlight][ESM4::Weather::Time_Day].g, 227);
        EXPECT_EQ(weather.mCloudTextures[0], "textures/sky/layer0.dds");
        EXPECT_EQ(weather.mCloudSpeeds[2], 75);
        EXPECT_EQ(weather.mCloudColors[2][ESM4::Weather::Time_HighNoon].unused, 40);
        EXPECT_FLOAT_EQ(weather.mFogDistance[1], 120000.f);
        EXPECT_EQ(weather.mData.transitionDelta, 255);
        EXPECT_EQ(weather.mData.classification, 1);
    }

    TEST(Esm4WeatherTest, shouldExpandFO3FourTimeColorRowsWithoutInventingHighNoon)
    {
        std::string payload;
        std::array<std::array<ESM4::Weather::Color, 4>, ESM4::Weather::sColorTypeCount> colors{};
        colors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Night] = { 12, 34, 56, 0 };
        appendSubRecord(payload, "NAM0", colors);

        auto reader = makeReader(payload);
        ESM4::Weather weather;
        weather.load(*reader);

        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Night].b, 56);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_HighNoon].r, 0);
    }

    TEST(Esm4WeatherTest, shouldConsumeTes5AndLaterCloudLayersAndKnownFields)
    {
        std::string payload;
        appendSubRecord(payload, "00TX", std::string_view("textures/sky/layer0.dds\0", 24));
        appendSubRecord(payload, "30TX", std::string_view("textures/sky/layer3.dds\0", 24));
        appendSubRecord(payload, "40TX", std::string_view("textures/sky/layer4.dds\0", 24));
        appendSubRecord(payload, "O0TX", std::string_view("textures/sky/layer31.dds\0", 25));
        appendSubRecord(payload, "MNAM", std::uint32_t{ 0x1234 });
        appendSubRecord(payload, "NNAM", std::uint32_t{ 0x5678 });
        appendSubRecord(payload, "RNAM", std::array<std::uint8_t, 32>{});
        appendSubRecord(payload, "QNAM", std::array<std::uint8_t, 32>{});
        appendSubRecord(payload, "JNAM", std::array<float, 128>{});
        appendSubRecord(payload, "NAM1", std::uint32_t{ 0 });
        appendSubRecord(payload, "TNAM", std::uint32_t{ 0x9abc });
        appendSubRecord(payload, "IMSP", std::array<std::uint32_t, 4>{});
        appendSubRecord(payload, "DALC", std::array<std::uint8_t, 32>{});
        appendSubRecord(payload, "NAM2", std::array<std::uint8_t, 16>{});
        appendSubRecord(payload, "NAM3", std::array<std::uint8_t, 16>{});
        appendSubRecord(payload, "NAM4", std::array<float, 32>{});
        appendSubRecord(payload, "GNAM", std::uint32_t{ 0xdef0 });

        auto reader = makeReader(payload);
        ESM4::Weather weather;
        ASSERT_NO_THROW(weather.load(*reader));

        EXPECT_EQ(weather.mCloudTextures[0], "textures/sky/layer0.dds");
        EXPECT_EQ(weather.mCloudTextures[3], "textures/sky/layer3.dds");
    }

    TEST(Esm4WeatherTest, shouldParseFallout4ExtendedWeatherPayloads)
    {
        std::string payload;
        std::array<std::uint8_t, 32> ySpeeds{};
        ySpeeds.fill(127);
        ySpeeds[2] = 254;
        appendSubRecord(payload, "RNAM", ySpeeds);

        std::array<std::array<ESM4::Weather::Color, 8>, 32> cloudColors{};
        cloudColors[2][ESM4::Weather::Time_Day] = { 30, 60, 90, 0 };
        appendSubRecord(payload, "PNAM", cloudColors);

        std::array<std::array<float, 8>, 32> cloudAlphas{};
        for (auto& layer : cloudAlphas)
            layer.fill(1.f);
        cloudAlphas[2][ESM4::Weather::Time_Day] = 0.25f;
        appendSubRecord(payload, "JNAM", cloudAlphas);

        std::array<std::array<ESM4::Weather::Color, 8>, 19> colors{};
        colors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Day] = { 101, 102, 103, 0 };
        colors[ESM4::Weather::Color_SkyUpper][ESM4::Weather::Time_Night] = { 20, 30, 40, 0 };
        appendSubRecord(payload, "NAM0", colors);

        std::array<float, 32> unknownLayerValues{};
        unknownLayerValues[4] = 0.75f;
        appendSubRecord(payload, "NAM4", unknownLayerValues);

        std::array<float, 18> fog{};
        fog[0] = 100.f;
        fog[1] = 250000.f;
        fog[5] = 0.8f;
        appendSubRecord(payload, "FNAM", fog);

        std::array<std::uint8_t, 20> data{};
        data[0] = 42;
        data[3] = 77;
        data[11] = 0x41;
        data[12] = 10;
        data[13] = 20;
        data[14] = 30;
        appendSubRecord(payload, "DATA", data);

        appendSubRecord(payload, "IMSP", std::array<std::uint32_t, 8>{ 1, 2, 3, 4, 5, 6, 7, 8 });
        appendSubRecord(payload, "WGDR", std::array<std::uint32_t, 8>{});
        appendSubRecord(payload, "UNAM", std::array<std::uint8_t, 24>{});
        appendSubRecord(payload, "VNAM", 0.5f);
        appendSubRecord(payload, "WNAM", 0.75f);

        auto reader = makeReader(payload, 1, 131);
        ESM4::Weather weather;
        ASSERT_NO_THROW(weather.load(*reader));

        EXPECT_TRUE(weather.mUsesExtendedCloudSpeeds);
        EXPECT_EQ(weather.mCloudSpeeds[2], 254);
        EXPECT_EQ(weather.mCloudColors[2][ESM4::Weather::Time_Day].g, 60);
        EXPECT_TRUE(weather.mHasCloudAlphas);
        EXPECT_FLOAT_EQ(weather.mCloudAlphas[2][ESM4::Weather::Time_Day], 0.25f);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Day].b, 103);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_SkyUpper][ESM4::Weather::Time_Night].r, 20);
        EXPECT_FLOAT_EQ(weather.mUnknownCloudLayerValues[4], 0.75f);
        EXPECT_FLOAT_EQ(weather.mFogDistance[1], 250000.f);
        EXPECT_EQ(weather.mData.windSpeed, 42);
        EXPECT_EQ(weather.mData.transitionDelta, 77);
        EXPECT_EQ(weather.mData.classification, 0x41);
        EXPECT_EQ(weather.mData.lightningColor.b, 30);
        EXPECT_EQ(weather.mImageSpaceModifiers[ESM4::Weather::Time_Sunset], ESM::FormId::fromUint32(0x01000003));
    }
}
