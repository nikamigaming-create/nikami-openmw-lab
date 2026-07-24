#include <components/esm4/loadregn.hpp>
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

    std::unique_ptr<ESM4::Reader> makeReader(std::string payload, std::uint32_t modIndex)
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
        appendRecord(plugin, "REGN", 0x123ce3, payload);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "region.esm", nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    TEST(Esm4RegionTest, shouldPreserveEveryWeatherBlockPriorityAndAdjustedReferences)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", std::string_view("GSWeatherRegion\0", 16));

        ESM4::Region::RegionData first{ ESM4::Region::RDAT_Weather, 0, 20, 0 };
        appendSubRecord(payload, "RDAT", first);
        appendSubRecord(payload, "RDWT", std::array<std::uint32_t, 3>{ 0x123456, 25, 0 });

        ESM4::Region::RegionData second{ ESM4::Region::RDAT_Weather, 0, 100, 0 };
        appendSubRecord(payload, "RDAT", second);
        appendSubRecord(payload, "RDWT", std::array<std::uint32_t, 6>{ 0x1237d7, 100, 0, 0x1237d8, 50, 0x77 });

        auto reader = makeReader(std::move(payload), 2);
        ESM4::Region region;
        region.load(*reader);

        EXPECT_EQ(region.mId, ESM::FormId::fromUint32(0x02123ce3));
        EXPECT_EQ(region.mEditorId, "GSWeatherRegion");
        ASSERT_EQ(region.mWeather.size(), 2);
        EXPECT_EQ(region.mWeather[0].mData.priority, 20);
        ASSERT_EQ(region.mWeather[0].mEntries.size(), 1);
        EXPECT_EQ(region.mWeather[0].mEntries[0].mWeather, ESM::FormId::fromUint32(0x02123456));
        EXPECT_TRUE(region.mWeather[0].mEntries[0].mGlobal.isZeroOrUnset());

        EXPECT_EQ(region.mWeather[1].mData.priority, 100);
        ASSERT_EQ(region.mWeather[1].mEntries.size(), 2);
        EXPECT_EQ(region.mWeather[1].mEntries[0].mWeather, ESM::FormId::fromUint32(0x021237d7));
        EXPECT_EQ(region.mWeather[1].mEntries[0].mChance, 100u);
        EXPECT_TRUE(region.mWeather[1].mEntries[0].mGlobal.isZeroOrUnset());
        EXPECT_EQ(region.mWeather[1].mEntries[1].mGlobal, ESM::FormId::fromUint32(0x02000077));
    }

    TEST(Esm4RegionTest, shouldPreserveSoundBlockPriorityChanceAndAdjustedReferences)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", std::string_view("GSSoundRegion\0", 14));

        ESM4::Region::RegionData soundData{ ESM4::Region::RDAT_Sound, 0, 75, 0 };
        appendSubRecord(payload, "RDAT", soundData);
        appendSubRecord(payload, "RDSD",
            std::array<std::uint32_t, 6>{ 0x123456, 1, 1'000'000, 0x123457, 2, 125'000 });

        ESM4::Region::RegionData mapData{ ESM4::Region::RDAT_Map, 0, 5, 0 };
        appendSubRecord(payload, "RDAT", mapData);
        appendSubRecord(payload, "RDMP", std::string_view("Goodsprings\0", 12));

        auto reader = makeReader(std::move(payload), 2);
        ESM4::Region region;
        region.load(*reader);

        ASSERT_EQ(region.mSoundBlocks.size(), 1);
        EXPECT_EQ(region.mSoundBlocks[0].mData.priority, 75);
        ASSERT_EQ(region.mSoundBlocks[0].mEntries.size(), 2);
        EXPECT_EQ(region.mSoundBlocks[0].mEntries[0].mSound, ESM::FormId::fromUint32(0x02123456));
        EXPECT_EQ(region.mSoundBlocks[0].mEntries[0].mFlags, 1u);
        EXPECT_EQ(region.mSoundBlocks[0].mEntries[0].mChance, 1'000'000u);
        EXPECT_EQ(region.mSoundBlocks[0].mEntries[1].mSound, ESM::FormId::fromUint32(0x02123457));
        EXPECT_EQ(region.mSoundBlocks[0].mEntries[1].mFlags, 2u);
        EXPECT_EQ(region.mSoundBlocks[0].mEntries[1].mChance, 125'000u);
        ASSERT_EQ(region.mSounds.size(), 2);
    }
}
