#include <components/esm4/loadclmt.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
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

    std::unique_ptr<ESM4::Reader> makeReader(std::string payload)
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
        appendRecord(plugin, "CLMT", 0x08809b, payload, 15);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "climate.esm", nullptr, nullptr, true);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    ESM4::Climate loadClimate(std::string payload)
    {
        auto reader = makeReader(std::move(payload));
        ESM4::Climate climate;
        climate.load(*reader);
        return climate;
    }

    TEST(Esm4ClimateTest, shouldPreserveAuthoredFNVTimingProvenance)
    {
        std::string payload;
        const std::array<std::uint8_t, 6> timing{ 36, 48, 108, 120, 0, 0x83 };
        appendSubRecord(payload, "TNAM", timing);

        const ESM4::Climate climate = loadClimate(std::move(payload));
        EXPECT_TRUE(climate.mHasTiming);
        EXPECT_EQ(climate.mTiming.mSunriseBegin, 36u);
        EXPECT_EQ(climate.mTiming.mSunriseEnd, 48u);
        EXPECT_EQ(climate.mTiming.mSunsetBegin, 108u);
        EXPECT_EQ(climate.mTiming.mSunsetEnd, 120u);
        EXPECT_EQ(climate.mTiming.mMoonInfo, 0x83u);
        EXPECT_EQ(climate.mTiming.getMoonPhaseLength(), 3u);
        EXPECT_TRUE(climate.mTiming.hasMasser());
        EXPECT_FALSE(climate.mTiming.hasSecunda());
    }

    TEST(Esm4ClimateTest, shouldNotClaimMissingOrMalformedTiming)
    {
        EXPECT_FALSE(loadClimate({}).mHasTiming);

        std::string payload;
        appendSubRecord(payload, "TNAM", std::array<std::uint8_t, 5>{});
        EXPECT_FALSE(loadClimate(std::move(payload)).mHasTiming);
    }
}
