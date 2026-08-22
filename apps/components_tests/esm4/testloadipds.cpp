#include <components/esm4/loadipds.hpp>

#include <gtest/gtest.h>

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

    std::string makeImpactDataSetData(std::size_t count)
    {
        std::string result;
        for (std::size_t index = 0; index < count; ++index)
            ESM4Test::appendPod(result, static_cast<std::uint32_t>(0x100 + index));
        return result;
    }

    ESM4::ImpactDataSet loadImpactDataSet(std::string data, float version = 1.34f)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "EDID", zString("SyntheticImpactDataSet"));
        ESM4Test::appendSubRecord(recordData, "DATA", data);
        auto reader = ESM4Test::makeReader("IPDS", std::move(recordData), 2, version);
        ESM4::ImpactDataSet impactDataSet;
        impactDataSet.load(*reader);
        return impactDataSet;
    }

    TEST(Esm4ImpactDataSetTest, loadsNineMaterialEntriesAndAdjustsFormIds)
    {
        const ESM4::ImpactDataSet impactDataSet = loadImpactDataSet(makeImpactDataSetData(9));

        EXPECT_EQ(impactDataSet.mId, ESM::FormId::fromUint32(0x02123456));
        EXPECT_EQ(impactDataSet.mEditorId, "SyntheticImpactDataSet");
        ASSERT_TRUE(impactDataSet.mPresent);
        for (std::size_t index = 0; index < 9; ++index)
        {
            const ESM::FormId expected
                = ESM::FormId::fromUint32(0x02000100u + static_cast<std::uint32_t>(index));
            EXPECT_EQ(impactDataSet.mImpacts[index], expected);
        }
        for (std::size_t index = 9; index < ESM4::ImpactDataSet::MaterialCount; ++index)
            EXPECT_TRUE(impactDataSet.mImpacts[index].isZeroOrUnset());
    }

    TEST(Esm4ImpactDataSetTest, loadsTwelveMaterialEntries)
    {
        const ESM4::ImpactDataSet impactDataSet = loadImpactDataSet(makeImpactDataSetData(12));

        ASSERT_TRUE(impactDataSet.mPresent);
        for (std::size_t index = 0; index < ESM4::ImpactDataSet::MaterialCount; ++index)
        {
            const ESM::FormId expected
                = ESM::FormId::fromUint32(0x02000100u + static_cast<std::uint32_t>(index));
            EXPECT_EQ(impactDataSet.mImpacts[index], expected);
        }
    }

    TEST(Esm4ImpactDataSetTest, skipsUnsupportedSizesAndOtherPluginVersions)
    {
        const ESM4::ImpactDataSet shortData = loadImpactDataSet(std::string(32, '\0'));
        EXPECT_FALSE(shortData.mPresent);
        EXPECT_EQ(shortData.mEditorId, "SyntheticImpactDataSet");

        const ESM4::ImpactDataSet longData = loadImpactDataSet(std::string(52, '\0'));
        EXPECT_FALSE(longData.mPresent);

        const ESM4::ImpactDataSet otherGame = loadImpactDataSet(makeImpactDataSetData(12), 1.f);
        EXPECT_FALSE(otherGame.mPresent);
        EXPECT_EQ(otherGame.mEditorId, "SyntheticImpactDataSet");
    }
}
