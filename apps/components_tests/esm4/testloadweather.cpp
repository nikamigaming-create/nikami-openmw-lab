#include <components/esm4/loadwthr.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace
{
    constexpr std::string_view sCurrentWeatherHex
        = "454449440e004e5657617374656c616e644753000049414404001aee0c0001494144040018ee0c0002494144040019ee0c000349414404001bee0c00"
          "04494144040018ee0c00444e414d0e00736b795c616c7068612e64647300434e414d0e00736b795c616c7068612e64647300414e414d0e00736b795c"
          "616c7068612e64647300424e414d1500736b795c4e56436c6f75646c696768742e646473004c4e414d0400040000004f4e414d040034000041504e41"
          "4d6000fcead60057c45a0006ff96001e1e1e00ff00800000000000ffffff00ffffff00f5c29c00000000000000000000000000ffffff00ffffff00d5"
          "d2ca00000000000000000000000000e6beb600e8ebee00e9cbbd00181c2300e8ebee00252c38004e414d30f000323489003247870049497a00020105"
          "00485b9f00000000008977960096a8be00afa3b8000d0b110096a8be0000000000ccc7d60097a8b700e6c5ac000000000000ff000000000000adadd1"
          "0057698a00b7b7f200acaad70063789a0000000000ff9d6f00ffe3aa00ffad5300a6bfd200ffe3aa0000000000f4680000fef1d300ff751a00000000"
          "00ffffff000000000000000000ffffff00ffffff00f5fbfe00ffffff00000000009cb4eb005c7394005e70ae0002040d005c73940000000000b89c9d"
          "00dde9ec00dbc0ae0047475800dde9ec002bc8d500ffffff007f7f7f00000000000000000000ff000000000000464e414d1800000020410060ea4700"
          "000000007c12480000003f0000003f494e414d30010000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f000080"
          "3f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f000080"
          "3f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000000000000000000000000000000000000000000000000000803f000000"
          "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000803f00000000000000"
          "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
          "00000000000000000000000000000000000000000000000000444154410f00322d2dff360000000000ff01000000";

    constexpr std::string_view sDefaultWeatherHex
        = "4544494411004e5657617374656c616e64436c656172000049414404001aee0c0001494144040018ee0c0002494144040019ee0c000349414404001b"
          "ee0c0004494144040018ee0c00444e414d0e00736b795c616c7068612e64647300434e414d0e00736b795c616c7068612e64647300414e414d0e0073"
          "6b795c616c7068612e64647300424e414d1500736b795c4e56436c6f75646c696768742e646473004c4e414d0400040000004f4e414d040034000041"
          "504e414d6000fcead60057c45a0006ff96001e1e1e00ff00800000000000ffffff00ffffff00f5c29c00000000000000000000000000ffffff00ffff"
          "ff00d5d2ca00000000000000000000000000e6beb600e8ebee00e9cbbd00181c2300e8ebee00252c38004e414d30f000323489003247870049497a00"
          "02010500485b9f00000000008977960096a8be00afa3b8000d0b110096a8be0000000000ccc7d60097a8b700e6c5ac000000000000ff000000000000"
          "adadd10057698a00b7b7f200acaad70063789a0000000000ff9d6f00ffe3aa00ffad5300a6bfd200ffe3aa0000000000f4680000fef1d300ff751a00"
          "ffffff00ffffff000000000000000000ffffff00ffffff00ffffff00ffffff00000000009cb4eb005c7394005e70ae0002040d005c73940000000000"
          "b89c9d00dde9ec00dbc0ae0047475800dde9ec002bc8d500ffffff007f7f7f00000000000000000000ff000000000000464e414d1800000020c10050"
          "4348000020c1005043489a99193f0000003f494e414d30010000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f"
          "0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f"
          "0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000000000000000000000000000000000000000000000000000803f"
          "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000803f00000000"
          "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
          "00000000000000000000000000000000000000000000000000000000444154410f00322d2dff360000000000ff01000000";

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

    std::unique_ptr<ESM4::Reader> makeReader(
        std::string body, std::uint32_t formId, std::uint32_t modIndex = 0, std::uint16_t formVersion = 15)
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
        appendRecord(plugin, "WTHR", formId, body, formVersion);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "weather.esm", nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        if (!reader->getRecordHeader())
            throw std::runtime_error("Missing WTHR test record");
        reader->getRecordData();
        return reader;
    }

    ESM4::Weather loadWeather(std::string body, std::uint32_t formId = 0x001237d7, std::uint32_t modIndex = 0)
    {
        auto reader = makeReader(std::move(body), formId, modIndex);
        ESM4::Weather weather;
        weather.load(*reader);
        return weather;
    }

    std::string_view findSubRecord(std::string_view body, std::string_view type)
    {
        for (std::size_t offset = 0; offset + 6 <= body.size();)
        {
            const std::uint16_t size = static_cast<std::uint8_t>(body[offset + 4])
                | (static_cast<std::uint16_t>(static_cast<std::uint8_t>(body[offset + 5])) << 8);
            const std::size_t dataOffset = offset + 6;
            if (dataOffset + size > body.size())
                throw std::runtime_error("Truncated fixture subrecord");
            if (body.substr(offset, 4) == type)
                return body.substr(dataOffset, size);
            offset = dataOffset + size;
        }
        throw std::runtime_error("Fixture subrecord not found");
    }

    TEST(Esm4WeatherTest, LoadsExactNVWastelandGSRecord)
    {
        const std::string body = decodeHex(sCurrentWeatherHex);
        ASSERT_EQ(body.size(), 886);
        const ESM4::Weather weather = loadWeather(body, 0x001237d7, 2);

        EXPECT_EQ(weather.mId, ESM::FormId::fromUint32(0x021237d7));
        EXPECT_EQ(weather.mEditorId, "NVWastelandGS");
        EXPECT_EQ(weather.mImageSpaceModifiers[ESM4::Weather::Time_Sunrise],
            ESM::FormId::fromUint32(0x020cee1a));
        EXPECT_EQ(weather.mImageSpaceModifiers[ESM4::Weather::Time_Night],
            ESM::FormId::fromUint32(0x020cee1b));
        EXPECT_TRUE(weather.mImageSpaceModifiers[ESM4::Weather::Time_Midnight].isZeroOrUnset());
        EXPECT_EQ(weather.mCloudTextures,
            (std::array<std::string, 4>{ "sky\\alpha.dds", "sky\\alpha.dds", "sky\\alpha.dds",
                "sky\\NVCloudlight.dds" }));
        EXPECT_TRUE(weather.mHasMaxCloudLayers);
        EXPECT_EQ(weather.mMaxCloudLayers, 4u);
        EXPECT_TRUE(weather.mHasCloudSpeeds);
        EXPECT_EQ(weather.mCloudSpeeds, (std::array<std::uint8_t, 4>{ 52, 0, 0, 65 }));
        EXPECT_EQ(weather.mCloudColorSampleCount, 6u);
        EXPECT_EQ(weather.mCloudColors[0][ESM4::Weather::Time_HighNoon].r, 255u);
        EXPECT_EQ(weather.mCloudColors[3][ESM4::Weather::Time_Midnight].b, 56u);
        EXPECT_EQ(weather.mColorSampleCount, 6u);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Day].g, 105u);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Horizon][ESM4::Weather::Time_Midnight].g, 200u);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_EffectLighting][ESM4::Weather::Time_HighNoon].g, 255u);
        EXPECT_FLOAT_EQ(weather.mFogDistance[0], 10.f);
        EXPECT_FLOAT_EQ(weather.mFogDistance[1], 120000.f);
        EXPECT_FLOAT_EQ(weather.mFogDistance[3], 150000.f);
        EXPECT_FLOAT_EQ(weather.mFogDistance[4], 0.5f);
        EXPECT_TRUE(weather.mHasUnusedImageSpaceData);
        const std::string_view inam = findSubRecord(body, "INAM");
        ASSERT_EQ(inam.size(), weather.mUnusedImageSpaceData.size());
        EXPECT_EQ(std::memcmp(inam.data(), weather.mUnusedImageSpaceData.data(), inam.size()), 0);
        EXPECT_EQ(weather.mData.windSpeed, 50u);
        EXPECT_EQ(weather.mData.transitionDelta, 255u);
        EXPECT_EQ(weather.mData.sunGlare, 54u);
        EXPECT_EQ(weather.mData.lightningFrequency, 255u);
        EXPECT_EQ(weather.mData.classification, 1u);
        EXPECT_EQ(weather.mData.lightningColor.unused, 0u);
        EXPECT_TRUE(weather.mSounds.empty());
    }

    TEST(Esm4WeatherTest, LoadsExactNVWastelandClearRecord)
    {
        const std::string body = decodeHex(sDefaultWeatherHex);
        ASSERT_EQ(body.size(), 889);
        const ESM4::Weather weather = loadWeather(body, 0x000ffc88);

        EXPECT_EQ(weather.mId, ESM::FormId::fromUint32(0x000ffc88));
        EXPECT_EQ(weather.mEditorId, "NVWastelandClear");
        EXPECT_EQ(weather.mColorSampleCount, 6u);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Sun][ESM4::Weather::Time_Night].r, 255u);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Stars][ESM4::Weather::Time_Night].b, 255u);
        EXPECT_FLOAT_EQ(weather.mFogDistance[0], -10.f);
        EXPECT_FLOAT_EQ(weather.mFogDistance[1], 200000.f);
        EXPECT_FLOAT_EQ(weather.mFogDistance[2], -10.f);
        EXPECT_FLOAT_EQ(weather.mFogDistance[3], 200000.f);
        EXPECT_FLOAT_EQ(weather.mFogDistance[4], 0.6f);
        EXPECT_FLOAT_EQ(weather.mFogDistance[5], 0.5f);
        const std::string_view inam = findSubRecord(body, "INAM");
        EXPECT_EQ(std::memcmp(inam.data(), weather.mUnusedImageSpaceData.data(), inam.size()), 0);
    }

    TEST(Esm4WeatherTest, SelectsLegacySamplesByExactSizeAndPreservesUnusedColorBytes)
    {
        std::array<std::uint8_t, 64> cloudColors{};
        cloudColors[47] = 0xa5;
        std::array<std::uint8_t, 160> colors{};
        colors[60] = 12;
        colors[61] = 34;
        colors[62] = 56;
        colors[63] = 0xbc;
        std::string body;
        appendSubRecord(body, "PNAM",
            std::string_view(reinterpret_cast<const char*>(cloudColors.data()), cloudColors.size()));
        appendSubRecord(
            body, "NAM0", std::string_view(reinterpret_cast<const char*>(colors.data()), colors.size()));

        const ESM4::Weather weather = loadWeather(std::move(body));
        EXPECT_EQ(weather.mCloudColorSampleCount, 4u);
        EXPECT_EQ(weather.mCloudColors[2][ESM4::Weather::Time_Night].unused, 0xa5u);
        EXPECT_EQ(weather.mColorSampleCount, 4u);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Night].r, 12u);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_Night].unused, 0xbcu);
        EXPECT_EQ(weather.mColors[ESM4::Weather::Color_Ambient][ESM4::Weather::Time_HighNoon].r, 0u);
    }

    TEST(Esm4WeatherTest, RejectsMalformedFixedSizeSubrecords)
    {
        const auto expectRejected = [](std::string_view type, std::size_t size) {
            SCOPED_TRACE(std::string(type.data(), type.size()));
            std::string body;
            appendSubRecord(body, type, std::string(size, '\0'));
            EXPECT_THROW(loadWeather(std::move(body)), std::runtime_error);
        };

        expectRejected(std::string_view("\0IAD", 4), 3);
        expectRejected("LNAM", 3);
        expectRejected("ONAM", 5);
        expectRejected("PNAM", 65);
        expectRejected("NAM0", 159);
        expectRejected("FNAM", 20);
        expectRejected("INAM", 303);
        expectRejected("DATA", 16);
        expectRejected("SNAM", 7);
    }
}
