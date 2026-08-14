#include <components/esm4/loadproj.hpp>

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>

namespace
{
    TEST(Esm4ProjectileTest, shouldParseRetailServiceRifleProjectileByteExactly)
    {
        // FalloutNV.esm 0000426D PROJ.DATA (556mmRifleBulletProjectile).
        const std::array<std::uint8_t, 84> bytes{
            0x8d, 0x00, 0x01, 0x00, 0xcd, 0xcc, 0xcc, 0x3e, 0x00, 0xc0, 0x1e, 0x47,
            0x00, 0x40, 0x1c, 0x46, 0x00, 0x00, 0x00, 0x00, 0xe9, 0x1d, 0x03, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x31, 0x87, 0x01, 0x00, 0x0a, 0xd7, 0x23, 0x3d,
            0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        ESM4::Projectile::Data data;

        ASSERT_TRUE(ESM4::loadFalloutProjectileData(bytes, data));
        ASSERT_TRUE(data.present);
        EXPECT_EQ(data.flags, 0x008d);
        EXPECT_TRUE(data.flags & ESM4::Projectile::Hitscan);
        EXPECT_EQ(data.type, 1);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.gravity), std::uint32_t{ 0x3ecccccd });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.speed), std::uint32_t{ 0x471ec000 });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.range), std::uint32_t{ 0x461c4000 });
        EXPECT_EQ(data.muzzleFlashLight.toUint32(), 0x31de9);
        EXPECT_EQ(data.sound.toUint32(), 0x18731);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.muzzleFlashDuration), std::uint32_t{ 0x3d23d70a });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.impactForce), std::uint32_t{ 0x40400000 });
    }

    TEST(Esm4ProjectileTest, shouldRejectUnknownProjectileDataLayout)
    {
        const std::array<std::uint8_t, 80> bytes{};
        ESM4::Projectile::Data data;
        EXPECT_FALSE(ESM4::loadFalloutProjectileData(bytes, data));
        EXPECT_FALSE(data.present);
    }

    TEST(Esm4ProjectileTest, shouldAcceptTheExplicitRetailSixtyEightByteLayout)
    {
        std::array<std::uint8_t, 68> bytes{};
        bytes[0] = ESM4::Projectile::Hitscan;
        ESM4::Projectile::Data data;

        ASSERT_TRUE(ESM4::loadFalloutProjectileData(bytes, data));
        EXPECT_TRUE(data.present);
        EXPECT_EQ(data.flags, ESM4::Projectile::Hitscan);
        EXPECT_FLOAT_EQ(data.rotationX, 0.f);
        EXPECT_FLOAT_EQ(data.bounciness, 0.f);
    }

    TEST(Esm4ProjectileTest, shouldParseRetailSkyrimProjectileDataByteExactly)
    {
        // Skyrim.esm VoiceDismayProjectile03 PROJ.DATA. The record uses the
        // complete 92-byte Skyrim layout including both trailing FormIDs.
        const std::vector<std::uint8_t> bytes = {
            0x00, 0x02, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x44, 0x00, 0x00, 0x16, 0x45,
            0xa1, 0xfd, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xc8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x41, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x80, 0x3e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        ESM4::Projectile::Data data;

        ASSERT_TRUE(ESM4::loadSkyrimProjectileData(bytes, data));
        EXPECT_TRUE(data.present);
        EXPECT_EQ(data.flags, 0x0200);
        EXPECT_EQ(data.type, 0x0010);
        EXPECT_FLOAT_EQ(data.speed, 1600.f);
        EXPECT_FLOAT_EQ(data.range, 2400.f);
        EXPECT_EQ(data.projectileLight.toUint32(), 0x10fda1);
        EXPECT_FLOAT_EQ(data.impactForce, 10.f);
        EXPECT_FLOAT_EQ(data.coneSpread, 20.f);
        EXPECT_FLOAT_EQ(data.collisionRadius, 32.f);
        EXPECT_FLOAT_EQ(data.lifetime, 0.f);
        EXPECT_FLOAT_EQ(data.relaunchInterval, 0.25f);
        EXPECT_EQ(data.decalData.toUint32(), 0);
        EXPECT_EQ(data.collisionLayer.toUint32(), 0);
    }

    TEST(Esm4ProjectileTest, shouldAcceptOnlyAuthoredSkyrimDataSizes)
    {
        ESM4::Projectile::Data data;
        EXPECT_TRUE(ESM4::loadSkyrimProjectileData(std::vector<std::uint8_t>(84), data));
        EXPECT_TRUE(ESM4::loadSkyrimProjectileData(std::vector<std::uint8_t>(88), data));
        EXPECT_TRUE(ESM4::loadSkyrimProjectileData(std::vector<std::uint8_t>(92), data));
        EXPECT_FALSE(ESM4::loadSkyrimProjectileData(std::vector<std::uint8_t>(90), data));
    }

    TEST(Esm4ProjectileTest, shouldParseRetailFallout4DnamLayoutByteExactly)
    {
        // Fallout4.esm 00084A28 PROJ.DNAM (VATSBullet45cal_Explosive).
        const std::array<std::uint8_t, 93> bytes{
            0x0a, 0x0a, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x1c, 0x46,
            0x00, 0x40, 0x1c, 0x46, 0x00, 0x00, 0x00, 0x00, 0x29, 0x4b, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xbb, 0x73, 0x1e, 0x00,
            0x00, 0x00, 0x00, 0x00, 0xcd, 0xcc, 0x4c, 0x3d, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x80, 0x3e, 0x00, 0x00, 0x00, 0x00, 0x67, 0x87, 0x08, 0x00, 0x01, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        ESM4::Projectile::Data data;

        ASSERT_TRUE(ESM4::loadFallout4ProjectileData(bytes, data));
        ASSERT_TRUE(data.present);
        EXPECT_EQ(data.flags, 0x0a0a);
        EXPECT_EQ(data.type, 1);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.speed), std::uint32_t{ 0x461c4000 });
        EXPECT_EQ(data.muzzleFlashLight.toUint32(), 0x4b29);
        EXPECT_EQ(data.explosion.toUint32(), 0x1e73bb);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.muzzleFlashDuration), std::uint32_t{ 0x3d4ccccd });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.coneSpread), std::uint32_t{ 0x00400000 });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.lifetime), std::uint32_t{ 0x3e800000 });
        EXPECT_EQ(data.decalData.toUint32(), 0x88767);
        EXPECT_EQ(data.collisionLayer.toUint32(), 1);
        EXPECT_EQ(data.tracerFrequency, 0);
        EXPECT_EQ(data.vatsProjectile.toUint32(), 0);
    }
}
