#include <components/esm4/loadexpl.hpp>

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>

namespace
{
    TEST(Esm4ExplosionTest, shouldParseRetailFragGrenadeExplosionByteExactly)
    {
        // FalloutNV.esm 000179DA GrenadeFragExplosion EXPL.DATA.
        const std::array<std::uint8_t, 52> bytes{
            0x00, 0x80, 0xbb, 0x43, 0x00, 0x00, 0xfa, 0x42, 0x00, 0x00, 0x61, 0x44, 0xff,
            0x4b, 0x01, 0x00, 0xe8, 0x61, 0x03, 0x00, 0x49, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xe1, 0x44, 0xa4, 0x3b, 0x09, 0x00, 0xe9, 0x61, 0x03, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        ESM4::Explosion::Data data;

        ASSERT_TRUE(ESM4::loadFalloutExplosionData(bytes, data));
        ASSERT_TRUE(data.present);
        EXPECT_FLOAT_EQ(data.force, 375.f);
        EXPECT_FLOAT_EQ(data.damage, 125.f);
        EXPECT_FLOAT_EQ(data.radius, 900.f);
        EXPECT_EQ(data.light.toUint32(), 0x14bff);
        EXPECT_EQ(data.sound1.toUint32(), 0x361e8);
        EXPECT_EQ(data.flags, 0x49u);
        EXPECT_TRUE(data.flags & ESM4::Explosion::IgnoreImageSpaceSwap);
        EXPECT_FALSE(data.flags & ESM4::Explosion::IgnoreLineOfSight);
        EXPECT_FLOAT_EQ(data.imageSpaceRadius, 1800.f);
        EXPECT_EQ(data.impactDataSet.toUint32(), 0x93ba4);
        EXPECT_EQ(data.sound2.toUint32(), 0x361e9);
        EXPECT_FLOAT_EQ(data.radiationLevel, 0.f);
        EXPECT_FLOAT_EQ(data.radiationDissipationTime, 0.f);
        EXPECT_FLOAT_EQ(data.radiationRadius, 0.f);
        EXPECT_EQ(data.soundLevel, ESM4::Explosion::Loud);
    }

    TEST(Esm4ExplosionTest, shouldPreserveRetailMissileExplosionFlagsAndValues)
    {
        // FalloutNV.esm 0005F9CD MissileExplosion EXPL.DATA.
        const std::array<std::uint8_t, 52> bytes{
            0x00, 0x00, 0xe1, 0x43, 0x00, 0x00, 0x48, 0x43, 0x00, 0x00, 0x7a, 0x44, 0xff,
            0x4b, 0x01, 0x00, 0x56, 0x2a, 0x17, 0x00, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xfa, 0x44, 0xa4, 0x3b, 0x09, 0x00, 0x57, 0x2a, 0x17, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        ESM4::Explosion::Data data;

        ASSERT_TRUE(ESM4::loadFalloutExplosionData(bytes, data));
        EXPECT_FLOAT_EQ(data.force, 450.f);
        EXPECT_FLOAT_EQ(data.damage, 200.f);
        EXPECT_FLOAT_EQ(data.radius, 1000.f);
        EXPECT_EQ(data.flags, 0x43u);
        EXPECT_FLOAT_EQ(data.imageSpaceRadius, 2000.f);
        EXPECT_EQ(data.sound1.toUint32(), 0x172a56);
        EXPECT_EQ(data.sound2.toUint32(), 0x172a57);
    }

    TEST(Esm4ExplosionTest, shouldRejectEveryNonRetailDataSize)
    {
        const std::array<std::uint8_t, 48> shortBytes{};
        const std::array<std::uint8_t, 56> longBytes{};
        ESM4::Explosion::Data data;
        EXPECT_FALSE(ESM4::loadFalloutExplosionData(shortBytes, data));
        EXPECT_FALSE(data.present);
        EXPECT_FALSE(ESM4::loadFalloutExplosionData(longBytes, data));
        EXPECT_FALSE(data.present);
    }
}
