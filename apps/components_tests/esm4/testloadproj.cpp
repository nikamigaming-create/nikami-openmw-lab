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

    TEST(Esm4ProjectileTest, shouldParseRetailThrowingSpearProjectileLifecycleByteExactly)
    {
        // FalloutNV.esm 0014D6C7 PROJ.DATA (NVSpearProjectile). CanBePickedUp is preserved as authored data, but
        // retail ignores that legacy flag for Missile projectiles; a thrown spear is consumed rather than cloned at
        // impact. PinsLimbs describes severed-limb attachment and does not make the projectile persistent.
        const std::array<std::uint8_t, 84> bytes{
            0x44, 0x01, 0x01, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x48, 0x45,
            0x00, 0x80, 0xbb, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x40,
            0x7f, 0x39, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
        };
        ESM4::Projectile::Data data;

        ASSERT_TRUE(ESM4::loadFalloutProjectileData(bytes, data));
        ASSERT_TRUE(data.present);
        EXPECT_EQ(data.flags, 0x0144);
        EXPECT_EQ(data.type, ESM4::Projectile::Missile);
        EXPECT_TRUE(data.flags & ESM4::Projectile::AlternateTrigger);
        EXPECT_TRUE(data.flags & ESM4::Projectile::CanBePickedUp);
        EXPECT_TRUE(data.flags & ESM4::Projectile::PinsLimbs);
        EXPECT_FALSE(data.flags & ESM4::Projectile::Explosion);
        EXPECT_FLOAT_EQ(data.gravity, 1.f);
        EXPECT_FLOAT_EQ(data.speed, 3200.f);
        EXPECT_FLOAT_EQ(data.range, 6000.f);
        EXPECT_FLOAT_EQ(data.alternateTimer, 2.5f);
        EXPECT_EQ(data.explosion.toUint32(), 0xe397f);
        EXPECT_FLOAT_EQ(data.fadeDuration, 0.5f);
        EXPECT_FLOAT_EQ(data.impactForce, 2.f);
        EXPECT_TRUE(data.defaultWeapon.isZeroOrUnset());
        EXPECT_FLOAT_EQ(data.bounciness, 1.f);
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
}
