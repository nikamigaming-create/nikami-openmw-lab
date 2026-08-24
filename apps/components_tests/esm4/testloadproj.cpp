#include <components/esm4/loadproj.hpp>
#include <components/esm4/falloutformat.hpp>

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

    std::string makeProjectileData(bool includeRotation)
    {
        std::string result;
        ESM4Test::appendPod(result, std::uint16_t{ 0x0089 });
        ESM4Test::appendPod(result, std::uint16_t{ 1 });
        ESM4Test::appendPod(result, 0.25f);
        ESM4Test::appendPod(result, 32000.f);
        ESM4Test::appendPod(result, 8192.f);
        ESM4Test::appendPod(result, std::uint32_t{ 0x101 });
        ESM4Test::appendPod(result, std::uint32_t{ 0x202 });
        ESM4Test::appendPod(result, 0.5f);
        ESM4Test::appendPod(result, 128.f);
        ESM4Test::appendPod(result, 1.5f);
        ESM4Test::appendPod(result, std::uint32_t{ 0x303 });
        ESM4Test::appendPod(result, std::uint32_t{ 0x404 });
        ESM4Test::appendPod(result, 0.125f);
        ESM4Test::appendPod(result, 0.75f);
        ESM4Test::appendPod(result, 3.5f);
        ESM4Test::appendPod(result, std::uint32_t{ 0x505 });
        ESM4Test::appendPod(result, std::uint32_t{ 0x606 });
        ESM4Test::appendPod(result, std::uint32_t{ 0x707 });
        if (includeRotation)
        {
            ESM4Test::appendPod(result, 10.f);
            ESM4Test::appendPod(result, 20.f);
            ESM4Test::appendPod(result, 30.f);
            ESM4Test::appendPod(result, 0.625f);
        }
        return result;
    }

    ESM4::Projectile loadProjectile(std::string data, float version = ESM4Test::kFalloutPluginVersion)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "EDID", zString("SyntheticProjectile"));
        ESM4Test::appendSubRecord(recordData, "MODL", zString("projectiles/main.nif"));
        ESM4Test::appendSubRecord(recordData, "DATA", data);
        ESM4Test::appendSubRecord(recordData, "NAM1", zString("projectiles/muzzle.nif"));
        std::string soundLevel;
        ESM4Test::appendPod(soundLevel, std::uint32_t{ 2 });
        ESM4Test::appendSubRecord(recordData, "VNAM", soundLevel);

        auto reader = ESM4Test::makeReader("PROJ", std::move(recordData), ESM4Test::kSyntheticModIndex, version);
        ESM4::Projectile projectile;
        projectile.load(*reader);
        return projectile;
    }

    TEST(Esm4ProjectileTest, loadsSixtyEightByteFalloutDataAndAdjustsFormIds)
    {
        const ESM4::Projectile projectile = loadProjectile(makeProjectileData(false));

        EXPECT_EQ(projectile.mId, ESM::FormId::fromUint32(0x02123456));
        EXPECT_EQ(projectile.mEditorId, "SyntheticProjectile");
        EXPECT_EQ(projectile.mModel.getOriginal(), "projectiles/main.nif");
        EXPECT_EQ(projectile.mMuzzleFlashModel.getOriginal(), "projectiles/muzzle.nif");
        EXPECT_EQ(projectile.mSoundLevel, 2u);
        ASSERT_TRUE(projectile.mData.mPresent);
        EXPECT_EQ(projectile.mData.mFlags, 0x0089);
        EXPECT_EQ(projectile.mData.mType, 1);
        EXPECT_FLOAT_EQ(projectile.mData.mGravity, 0.25f);
        EXPECT_FLOAT_EQ(projectile.mData.mSpeed, 32000.f);
        EXPECT_FLOAT_EQ(projectile.mData.mRange, 8192.f);
        EXPECT_EQ(projectile.mData.mProjectileLight, ESM::FormId::fromUint32(0x02000101));
        EXPECT_EQ(projectile.mData.mMuzzleFlashLight, ESM::FormId::fromUint32(0x02000202));
        EXPECT_FLOAT_EQ(projectile.mData.mTracerChance, 0.5f);
        EXPECT_FLOAT_EQ(projectile.mData.mAlternateProximity, 128.f);
        EXPECT_FLOAT_EQ(projectile.mData.mAlternateTimer, 1.5f);
        EXPECT_EQ(projectile.mData.mExplosion, ESM::FormId::fromUint32(0x02000303));
        EXPECT_EQ(projectile.mData.mSound, ESM::FormId::fromUint32(0x02000404));
        EXPECT_FLOAT_EQ(projectile.mData.mMuzzleFlashDuration, 0.125f);
        EXPECT_FLOAT_EQ(projectile.mData.mFadeDuration, 0.75f);
        EXPECT_FLOAT_EQ(projectile.mData.mImpactForce, 3.5f);
        EXPECT_EQ(projectile.mData.mCountdownSound, ESM::FormId::fromUint32(0x02000505));
        EXPECT_EQ(projectile.mData.mDisableSound, ESM::FormId::fromUint32(0x02000606));
        EXPECT_EQ(projectile.mData.mDefaultWeapon, ESM::FormId::fromUint32(0x02000707));
        EXPECT_FLOAT_EQ(projectile.mData.mRotation[0], 0.f);
        EXPECT_FLOAT_EQ(projectile.mData.mRotation[1], 0.f);
        EXPECT_FLOAT_EQ(projectile.mData.mRotation[2], 0.f);
        EXPECT_FLOAT_EQ(projectile.mData.mBounciness, 0.f);
    }

    TEST(Esm4ProjectileTest, loadsEightyFourByteFalloutRotationAndBounciness)
    {
        const ESM4::Projectile projectile = loadProjectile(makeProjectileData(true));

        ASSERT_TRUE(projectile.mData.mPresent);
        EXPECT_FLOAT_EQ(projectile.mData.mRotation[0], 10.f);
        EXPECT_FLOAT_EQ(projectile.mData.mRotation[1], 20.f);
        EXPECT_FLOAT_EQ(projectile.mData.mRotation[2], 30.f);
        EXPECT_FLOAT_EQ(projectile.mData.mBounciness, 0.625f);
    }

    TEST(Esm4ProjectileTest, skipsUnknownDataSizeWithoutLosingSubrecordAlignment)
    {
        const ESM4::Projectile projectile
            = loadProjectile(std::string(ESM4::Fallout::kProjectileDataBytes + sizeof(std::uint32_t) * 3, '\0'));

        EXPECT_FALSE(projectile.mData.mPresent);
        EXPECT_EQ(projectile.mMuzzleFlashModel.getOriginal(), "projectiles/muzzle.nif");
        EXPECT_EQ(projectile.mSoundLevel, 2u);
    }

    TEST(Esm4ProjectileTest, leavesFalloutDataAbsentForOtherPluginVersions)
    {
        const ESM4::Projectile projectile = loadProjectile(makeProjectileData(false), ESM4Test::kOtherPluginVersion);

        EXPECT_FALSE(projectile.mData.mPresent);
        EXPECT_EQ(projectile.mMuzzleFlashModel.getOriginal(), "projectiles/muzzle.nif");
        EXPECT_EQ(projectile.mSoundLevel, 2u);
    }
}
