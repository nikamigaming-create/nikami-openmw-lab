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

    std::unique_ptr<ESM4::Reader> makeReader(std::string payload, std::uint32_t modIndex = 0)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);

        const auto appendRecord = [](std::string& output, std::string_view type, std::uint32_t formId,
                                      std::string_view body) {
            output.append(type);
            appendPod(output, static_cast<std::uint32_t>(body.size()));
            appendPod(output, std::uint32_t{ 0 });
            appendPod(output, formId);
            appendPod(output, std::uint32_t{ 0 });
            appendPod(output, std::uint16_t{ 0 });
            appendPod(output, std::uint16_t{ 0 });
            output.append(body);
        };

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload);
        appendRecord(plugin, "WTHR", 0x1237d7, payload);
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
}
