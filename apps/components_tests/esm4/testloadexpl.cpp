#include <components/esm4/loadexpl.hpp>

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

    std::string makeExplosionData()
    {
        std::string result;
        ESM4Test::appendPod(result, 375.f);
        ESM4Test::appendPod(result, 125.f);
        ESM4Test::appendPod(result, 900.f);
        ESM4Test::appendPod(result, std::uint32_t{ 0x101 });
        ESM4Test::appendPod(result, std::uint32_t{ 0x202 });
        ESM4Test::appendPod(result, std::uint32_t{ 0x43 });
        ESM4Test::appendPod(result, 1800.f);
        ESM4Test::appendPod(result, std::uint32_t{ 0x303 });
        ESM4Test::appendPod(result, std::uint32_t{ 0x404 });
        ESM4Test::appendPod(result, 2.f);
        ESM4Test::appendPod(result, 3.f);
        ESM4Test::appendPod(result, 4.f);
        ESM4Test::appendPod(result, std::uint32_t{ 1 });
        return result;
    }

    ESM4::Explosion loadExplosion(std::string data, float version = 1.34f)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "EDID", zString("SyntheticExplosion"));
        ESM4Test::appendSubRecord(recordData, "MODL", zString("effects/explosion.nif"));
        std::string objectEffect;
        ESM4Test::appendPod(objectEffect, std::uint32_t{ 0x505 });
        ESM4Test::appendSubRecord(recordData, "EITM", objectEffect);
        std::string imageSpaceModifier;
        ESM4Test::appendPod(imageSpaceModifier, std::uint32_t{ 0x606 });
        ESM4Test::appendSubRecord(recordData, "MNAM", imageSpaceModifier);
        ESM4Test::appendSubRecord(recordData, "DATA", data);
        std::string placedImpactObject;
        ESM4Test::appendPod(placedImpactObject, std::uint32_t{ 0x707 });
        ESM4Test::appendSubRecord(recordData, "INAM", placedImpactObject);

        auto reader = ESM4Test::makeReader("EXPL", std::move(recordData), 2, version);
        ESM4::Explosion explosion;
        explosion.load(*reader);
        return explosion;
    }

    TEST(Esm4ExplosionTest, loadsFiftyTwoByteFalloutDataAndAdjustsEveryFormId)
    {
        const ESM4::Explosion explosion = loadExplosion(makeExplosionData());

        EXPECT_EQ(explosion.mId, ESM::FormId::fromUint32(0x02123456));
        EXPECT_EQ(explosion.mEditorId, "SyntheticExplosion");
        EXPECT_EQ(explosion.mModel.getOriginal(), "effects/explosion.nif");
        EXPECT_EQ(explosion.mObjectEffect, ESM::FormId::fromUint32(0x02000505));
        EXPECT_EQ(explosion.mImageSpaceModifier, ESM::FormId::fromUint32(0x02000606));
        EXPECT_EQ(explosion.mPlacedImpactObject, ESM::FormId::fromUint32(0x02000707));
        ASSERT_TRUE(explosion.mData.mPresent);
        EXPECT_FLOAT_EQ(explosion.mData.mForce, 375.f);
        EXPECT_FLOAT_EQ(explosion.mData.mDamage, 125.f);
        EXPECT_FLOAT_EQ(explosion.mData.mRadius, 900.f);
        EXPECT_EQ(explosion.mData.mLight, ESM::FormId::fromUint32(0x02000101));
        EXPECT_EQ(explosion.mData.mSound1, ESM::FormId::fromUint32(0x02000202));
        EXPECT_EQ(explosion.mData.mFlags, 0x43u);
        EXPECT_FLOAT_EQ(explosion.mData.mImageSpaceRadius, 1800.f);
        EXPECT_EQ(explosion.mData.mImpactDataSet, ESM::FormId::fromUint32(0x02000303));
        EXPECT_EQ(explosion.mData.mSound2, ESM::FormId::fromUint32(0x02000404));
        EXPECT_FLOAT_EQ(explosion.mData.mRadiationLevel, 2.f);
        EXPECT_FLOAT_EQ(explosion.mData.mRadiationDissipationTime, 3.f);
        EXPECT_FLOAT_EQ(explosion.mData.mRadiationRadius, 4.f);
        EXPECT_EQ(explosion.mData.mSoundLevel, 1u);
    }

    TEST(Esm4ExplosionTest, skipsUnknownDataSizeWithoutLosingSubrecordAlignment)
    {
        const ESM4::Explosion explosion = loadExplosion(std::string(48, '\0'));

        EXPECT_FALSE(explosion.mData.mPresent);
        EXPECT_EQ(explosion.mPlacedImpactObject, ESM::FormId::fromUint32(0x02000707));
    }

    TEST(Esm4ExplosionTest, leavesFalloutDataAbsentForOtherPluginVersions)
    {
        const ESM4::Explosion explosion = loadExplosion(makeExplosionData(), 1.f);

        EXPECT_FALSE(explosion.mData.mPresent);
        EXPECT_EQ(explosion.mModel.getOriginal(), "effects/explosion.nif");
        EXPECT_EQ(explosion.mPlacedImpactObject, ESM::FormId::fromUint32(0x02000707));
    }
}
