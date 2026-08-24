#include <components/esm4/loadammo.hpp>

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <string>

#include "testutil.hpp"

namespace
{
    using namespace std::literals;

    constexpr std::size_t kUnsupportedData2Bytes = ESM4::Fallout::kAmmunitionDat2BaseBytes + sizeof(std::uint32_t);

    std::string makeData2(std::uint32_t projectilesPerShot, std::uint32_t projectile, float weight,
        std::uint32_t consumedAmmo = 0, float consumedPercentage = 0.f, bool includeConsumedAmmo = false)
    {
        std::string result;
        ESM4Test::appendPod(result, projectilesPerShot);
        ESM4Test::appendPod(result, projectile);
        ESM4Test::appendPod(result, weight);
        if (includeConsumedAmmo)
        {
            ESM4Test::appendPod(result, consumedAmmo);
            ESM4Test::appendPod(result, consumedPercentage);
        }
        return result;
    }

    ESM4::Ammunition loadAmmunition(std::string data2)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "DAT2", data2);
        ESM4Test::appendSubRecord(recordData, "ICON", "icons/ammo.dds\0"sv);
        auto reader = ESM4Test::makeReader("AMMO", std::move(recordData));
        ESM4::Ammunition ammunition;
        ammunition.load(*reader);
        return ammunition;
    }

    TEST(Esm4AmmunitionTest, loadsTwelveByteData2WithoutConsumedAmmo)
    {
        const ESM4::Ammunition ammunition = loadAmmunition(makeData2(5, 0x5678, 0.25f));

        EXPECT_EQ(ammunition.mData.mProjPerShot, 5u);
        EXPECT_EQ(ammunition.mData.mProjectile, ESM::FormId::fromUint32(0x02005678));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(ammunition.mData.mWeight), 0x3e800000u);
        EXPECT_TRUE(ammunition.mData.mConsumedAmmo.isZeroOrUnset());
        EXPECT_FLOAT_EQ(ammunition.mData.mConsumedPercentage, 0.f);
        EXPECT_EQ(ammunition.mIcon, "icons/ammo.dds");
    }

    TEST(Esm4AmmunitionTest, loadsTwentyByteData2WithConsumedAmmo)
    {
        const ESM4::Ammunition ammunition = loadAmmunition(makeData2(1, 0x5678, 0.5f, 0xabcd, 35.f, true));

        EXPECT_EQ(ammunition.mData.mProjPerShot, 1u);
        EXPECT_EQ(ammunition.mData.mProjectile, ESM::FormId::fromUint32(0x02005678));
        EXPECT_FLOAT_EQ(ammunition.mData.mWeight, 0.5f);
        EXPECT_EQ(ammunition.mData.mConsumedAmmo, ESM::FormId::fromUint32(0x0200abcd));
        EXPECT_FLOAT_EQ(ammunition.mData.mConsumedPercentage, 35.f);
        EXPECT_EQ(ammunition.mIcon, "icons/ammo.dds");
    }

    TEST(Esm4AmmunitionTest, skipsUnknownData2SizeWithoutLosingSubrecordAlignment)
    {
        const ESM4::Ammunition ammunition = loadAmmunition(std::string(kUnsupportedData2Bytes, '\0'));

        EXPECT_EQ(ammunition.mData.mProjPerShot, 0u);
        EXPECT_TRUE(ammunition.mData.mProjectile.isZeroOrUnset());
        EXPECT_EQ(ammunition.mIcon, "icons/ammo.dds");
    }
}
