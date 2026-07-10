#include <components/esm4/loadweap.hpp>

#include <gtest/gtest.h>

#include <array>
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
}
