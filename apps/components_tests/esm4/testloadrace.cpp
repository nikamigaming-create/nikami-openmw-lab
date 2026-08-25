#include <components/esm4/loadrace.hpp>

#include "testutil.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace
{
    constexpr std::uint32_t kBodyPartDataSourceFormId = 0x0002d1e0;
    constexpr std::uint32_t kBodyPartDataAdjustedFormId
        = (ESM4Test::kSyntheticModIndex << 24) | kBodyPartDataSourceFormId;
    constexpr std::size_t kMalformedBodyPartDataSize = sizeof(ESM::FormId32) - 1;
    constexpr std::string_view kEditorId = "FalloutRace";

    std::string formIdPayload(std::uint32_t value)
    {
        std::string payload;
        ESM4Test::appendPod(payload, value);
        return payload;
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    TEST(Esm4RaceTest, LoadsFalloutBodyPartDataWithFormIdAdjustment)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "GNAM", formIdPayload(kBodyPartDataSourceFormId));

        auto reader = ESM4Test::makeReader("RACE", payload);
        ESM4::Race race;
        race.load(*reader);

        EXPECT_EQ(race.mBodyPartData, ESM::FormId::fromUint32(kBodyPartDataAdjustedFormId));
    }

    TEST(Esm4RaceTest, SkipsMalformedBodyPartDataAndPreservesFollowingSubrecord)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "GNAM", std::string(kMalformedBodyPartDataSize, '\0'));
        ESM4Test::appendSubRecord(payload, "EDID", zString(kEditorId));

        auto reader = ESM4Test::makeReader("RACE", payload);
        ESM4::Race race;
        race.load(*reader);

        EXPECT_TRUE(race.mBodyPartData.isZeroOrUnset());
        EXPECT_EQ(race.mEditorId, kEditorId);
    }
}
