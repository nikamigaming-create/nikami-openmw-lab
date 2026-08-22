#include <components/esm4/loadipct.hpp>

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

    std::string makeImpactData()
    {
        std::string result;
        ESM4Test::appendPod(result, 1.25f);
        ESM4Test::appendPod(result, std::uint32_t{ 2 });
        ESM4Test::appendPod(result, 45.f);
        ESM4Test::appendPod(result, 0.5f);
        ESM4Test::appendPod(result, std::uint32_t{ 1 });
        ESM4Test::appendPod(result, std::uint32_t{ 1 });
        return result;
    }

    ESM4::ImpactData loadImpactData(std::string data, float version = 1.34f)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "EDID", zString("SyntheticImpact"));
        ESM4Test::appendSubRecord(recordData, "MODL", zString("effects/impact.nif"));
        std::string textureSet;
        ESM4Test::appendPod(textureSet, std::uint32_t{ 0x101 });
        ESM4Test::appendSubRecord(recordData, "DNAM", textureSet);
        std::string sound1;
        ESM4Test::appendPod(sound1, std::uint32_t{ 0x202 });
        ESM4Test::appendSubRecord(recordData, "SNAM", sound1);
        ESM4Test::appendSubRecord(recordData, "DATA", data);
        std::string sound2;
        ESM4Test::appendPod(sound2, std::uint32_t{ 0x303 });
        ESM4Test::appendSubRecord(recordData, "NAM1", sound2);
        ESM4Test::appendSubRecord(recordData, "DODT", std::string(36, '\0'));

        auto reader = ESM4Test::makeReader("IPCT", std::move(recordData), 2, version);
        ESM4::ImpactData impactData;
        impactData.load(*reader);
        return impactData;
    }

    TEST(Esm4ImpactDataTest, loadsTwentyFourByteFalloutCoreAndAllReferences)
    {
        const ESM4::ImpactData impactData = loadImpactData(makeImpactData());

        EXPECT_EQ(impactData.mId, ESM::FormId::fromUint32(0x02123456));
        EXPECT_EQ(impactData.mEditorId, "SyntheticImpact");
        EXPECT_EQ(impactData.mModel.getOriginal(), "effects/impact.nif");
        EXPECT_EQ(impactData.mTextureSet, ESM::FormId::fromUint32(0x02000101));
        EXPECT_EQ(impactData.mSound1, ESM::FormId::fromUint32(0x02000202));
        EXPECT_EQ(impactData.mSound2, ESM::FormId::fromUint32(0x02000303));
        ASSERT_TRUE(impactData.mData.mPresent);
        EXPECT_FLOAT_EQ(impactData.mData.mEffectDuration, 1.25f);
        EXPECT_EQ(impactData.mData.mOrientation, 2u);
        EXPECT_FLOAT_EQ(impactData.mData.mAngleThreshold, 45.f);
        EXPECT_FLOAT_EQ(impactData.mData.mPlacementRadius, 0.5f);
        EXPECT_EQ(impactData.mData.mSoundLevel, 1u);
        EXPECT_EQ(impactData.mData.mFlags, ESM4::ImpactData::NoDecalData);
    }

    TEST(Esm4ImpactDataTest, skipsUnknownDataSizeWithoutLosingReferenceAlignment)
    {
        const ESM4::ImpactData impactData = loadImpactData(std::string(20, '\0'));

        EXPECT_FALSE(impactData.mData.mPresent);
        EXPECT_EQ(impactData.mSound2, ESM::FormId::fromUint32(0x02000303));
    }

    TEST(Esm4ImpactDataTest, leavesFalloutCoreAbsentForOtherPluginVersions)
    {
        const ESM4::ImpactData impactData = loadImpactData(makeImpactData(), 1.f);

        EXPECT_FALSE(impactData.mData.mPresent);
        EXPECT_TRUE(impactData.mTextureSet.isZeroOrUnset());
        EXPECT_TRUE(impactData.mSound1.isZeroOrUnset());
        EXPECT_TRUE(impactData.mSound2.isZeroOrUnset());
    }
}
