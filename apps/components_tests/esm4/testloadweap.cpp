#include <components/esm4/loadweap.hpp>

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>

namespace
{
    TEST(Esm4WeaponTest, shouldParseFalloutAnimationSelectorsFromDnamPrefix)
    {
        // Retail FNV WeapNVAssaultCarbine (0008F21E): TwoHandAutomatic, default grip, one ammo/use,
        // reload animation 5. Bytes between the selectors deliberately contain non-zero payload.
        const std::array<std::uint8_t, 16> dnam{
            6, 0x11, 0x22, 0x33, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x20, 0x41, 0x7f, 0xff, 1, 5
        };
        ESM4::Weapon::Data data;

        ASSERT_TRUE(ESM4::loadFalloutWeaponDnam(dnam, data));
        EXPECT_EQ(data.animationType, 6);
        EXPECT_EQ(data.handGrip, 0xff);
        EXPECT_EQ(data.ammoUse, 1);
        EXPECT_EQ(data.reloadAnim, 5);
    }

    TEST(Esm4WeaponTest, shouldRejectTruncatedFalloutDnamWithoutChangingDefaults)
    {
        const std::array<std::uint8_t, 15> dnam{};
        ESM4::Weapon::Data data;

        EXPECT_FALSE(ESM4::loadFalloutWeaponDnam(dnam, data));
        EXPECT_EQ(data.animationType, 0xff);
        EXPECT_EQ(data.handGrip, 0xff);
        EXPECT_EQ(data.ammoUse, 0);
        EXPECT_EQ(data.reloadAnim, 0);
    }

    TEST(Esm4WeaponTest, shouldParseRetailServiceRifleBallisticContractByteExactly)
    {
        // FalloutNV.esm 000E9C3B WEAP.DNAM, through fireRate at byte 67.
        const std::array<std::uint8_t, 68> dnam{ 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
            0x00, 0x00, 0x80, 0x3f, 0x00, 0xff, 0x01, 0x05, 0xcd, 0xcc, 0x0c, 0x3f, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x42, 0x00, 0x00, 0x00, 0x00, 0x6d, 0x42,
            0x00, 0x00, 0x2a, 0x26, 0x01, 0x00, 0x00, 0x00, 0x40, 0x44, 0x00, 0xc0, 0x5d, 0x45, 0x00,
            0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x33, 0x33, 0x33, 0x3f, 0x00, 0x00, 0x80, 0x3f };
        ESM4::Weapon::Data data;

        ASSERT_TRUE(ESM4::loadFalloutWeaponDnam(dnam, data));
        ASSERT_TRUE(data.hasBallistics);
        EXPECT_EQ(data.projectile.toUint32(), 0x426d);
        EXPECT_EQ(data.numProjectiles, 1);
        EXPECT_EQ(data.ammoUse, 1);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.minSpread), std::uint32_t{ 0x3f0ccccd });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.minRange), std::uint32_t{ 0x44400000 });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.maxRange), std::uint32_t{ 0x455dc000 });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.animAttackMult), std::uint32_t{ 0x3f333333 });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.fireRate), std::uint32_t{ 0x3f800000 });
    }
}
