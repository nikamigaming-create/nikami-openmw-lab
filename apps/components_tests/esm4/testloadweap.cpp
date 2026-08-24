#include <components/esm4/loadweap.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "testutil.hpp"

namespace
{
    using namespace std::literals;

    constexpr std::size_t kUnsupportedTruncatedDnamBytes = ESM4::Fallout::kWeaponDnamAnimationBytes - sizeof(std::uint32_t);
    constexpr std::size_t kExtendedDnamFixturePaddingBytes = sizeof(std::uint32_t) * 3;
    constexpr std::size_t kExtendedDnamFixtureBytes
        = ESM4::Fallout::kWeaponDnamBallisticsBytes + kExtendedDnamFixturePaddingBytes;

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    std::string makeFalloutDnam(std::size_t size)
    {
        std::string result;
        ESM4Test::appendPod(result, std::uint32_t{ 6 });
        ESM4Test::appendPod(result, 1.25f);
        ESM4Test::appendPod(result, 1.5f);
        ESM4Test::appendPod(result, std::uint8_t{ 0x43 });
        ESM4Test::appendPod(result, std::uint8_t{ 0xe6 });
        ESM4Test::appendPod(result, std::uint8_t{ 2 });
        ESM4Test::appendPod(result, std::uint8_t{ 7 });
        if (size >= ESM4::Fallout::kWeaponDnamBallisticsBytes)
        {
            ESM4Test::appendPod(result, 0.1f);
            ESM4Test::appendPod(result, 0.2f);
            ESM4Test::appendPod(result, std::uint32_t{ 0 });
            ESM4Test::appendPod(result, 55.f);
            ESM4Test::appendPod(result, std::uint32_t{ 0 });
            ESM4Test::appendPod(result, std::uint32_t{ 0x888 });
            ESM4Test::appendPod(result, std::uint8_t{ 42 });
            ESM4Test::appendPod(result, std::uint8_t{ 32 });
            ESM4Test::appendPod(result, std::uint8_t{ 3 });
            ESM4Test::appendPod(result, std::uint8_t{ 5 });
            ESM4Test::appendPod(result, 512.f);
            ESM4Test::appendPod(result, 4096.f);
            ESM4Test::appendPod(result, std::uint32_t{ 2 });
            ESM4Test::appendPod(result, std::uint32_t{ 0x1234 });
            ESM4Test::appendPod(result, 0.75f);
            ESM4Test::appendPod(result, 2.5f);
        }
        if (result.size() > size)
            throw std::logic_error("synthetic WEAP.DNAM size is too small");
        result.resize(size, '\x7f');
        return result;
    }

    ESM4::Weapon loadWeapon(std::string dnam, float version = ESM4Test::kFalloutPluginVersion)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "DNAM", dnam);
        ESM4Test::appendSubRecord(recordData, "ICON", "icons/weapon.dds\0"sv);
        auto reader = ESM4Test::makeReader("WEAP", std::move(recordData), ESM4Test::kSyntheticModIndex, version);
        ESM4::Weapon weapon;
        weapon.load(*reader);
        return weapon;
    }

    TEST(Esm4WeaponTest, loadsHeldWorldAndFirstPersonModelFields)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "MODL", zString("weapons/held.nif"));
        ESM4Test::appendSubRecord(recordData, "MOD4", zString("weapons/world.nif"));
        ESM4Test::appendSubRecord(recordData, "MO4T", std::string(7, '\x11'));
        ESM4Test::appendSubRecord(recordData, "MO4S", std::string(13, '\x22'));
        ESM4Test::appendSubRecord(recordData, "MO4C", std::string(4, '\x33'));
        ESM4Test::appendSubRecord(recordData, "MO4F", std::string(1, '\x44'));
        std::string firstPersonModel;
        ESM4Test::appendPod(firstPersonModel, std::uint32_t{ 0x5678 });
        ESM4Test::appendSubRecord(recordData, "WNAM", firstPersonModel);
        ESM4Test::appendSubRecord(recordData, "ICON", "icons/weapon.dds\0"sv);

        auto reader = ESM4Test::makeReader("WEAP", std::move(recordData));
        ESM4::Weapon weapon;
        weapon.load(*reader);

        EXPECT_EQ(weapon.mModel.getOriginal(), "weapons/held.nif");
        EXPECT_EQ(weapon.mWorldModel.getOriginal(), "weapons/world.nif");
        EXPECT_EQ(weapon.mFirstPersonModel, ESM::FormId::fromUint32(0x02005678));
        EXPECT_EQ(weapon.mIcon, "icons/weapon.dds");
    }

    TEST(Esm4WeaponTest, loadsFalloutDnamBallisticPrefixAndAdjustsProjectile)
    {
        const ESM4::Weapon weapon = loadWeapon(makeFalloutDnam(kExtendedDnamFixtureBytes));

        ASSERT_TRUE(weapon.mFalloutData.mPresent);
        ASSERT_TRUE(weapon.mFalloutData.mBallisticsPresent);
        EXPECT_EQ(weapon.mFalloutData.mAnimationType, 6u);
        EXPECT_FLOAT_EQ(weapon.mFalloutData.mAnimationMultiplier, 1.25f);
        EXPECT_FLOAT_EQ(weapon.mFalloutData.mReach, 1.5f);
        EXPECT_EQ(weapon.mFalloutData.mFlags, 0x43);
        EXPECT_EQ(weapon.mFalloutData.mGripAnimation, 0xe6);
        EXPECT_EQ(weapon.mFalloutData.mAmmoUse, 2);
        EXPECT_EQ(weapon.mFalloutData.mReloadAnimation, 7);
        EXPECT_FLOAT_EQ(weapon.mFalloutData.mMinSpread, 0.1f);
        EXPECT_FLOAT_EQ(weapon.mFalloutData.mSpread, 0.2f);
        EXPECT_FLOAT_EQ(weapon.mFalloutData.mSightFov, 55.f);
        EXPECT_EQ(weapon.mFalloutData.mProjectile, ESM::FormId::fromUint32(0x02000888));
        EXPECT_EQ(weapon.mFalloutData.mBaseVatsChance, 42);
        EXPECT_EQ(weapon.mFalloutData.mAttackAnimation, 32);
        EXPECT_EQ(weapon.mFalloutData.mProjectileCount, 3);
        EXPECT_EQ(weapon.mFalloutData.mEmbeddedWeaponActorValue, 5);
        EXPECT_FLOAT_EQ(weapon.mFalloutData.mMinRange, 512.f);
        EXPECT_FLOAT_EQ(weapon.mFalloutData.mMaxRange, 4096.f);
        EXPECT_EQ(weapon.mFalloutData.mOnHit, 2u);
        EXPECT_EQ(weapon.mFalloutData.mFlags2, 0x1234u);
        EXPECT_FLOAT_EQ(weapon.mFalloutData.mAnimationAttackMultiplier, 0.75f);
        EXPECT_FLOAT_EQ(weapon.mFalloutData.mFireRate, 2.5f);
        EXPECT_EQ(weapon.mIcon, "icons/weapon.dds");
    }

    TEST(Esm4WeaponTest, loadsSixteenByteFalloutPrefixWithoutInventingBallistics)
    {
        const ESM4::Weapon weapon = loadWeapon(makeFalloutDnam(ESM4::Fallout::kWeaponDnamAnimationBytes));

        ASSERT_TRUE(weapon.mFalloutData.mPresent);
        EXPECT_FALSE(weapon.mFalloutData.mBallisticsPresent);
        EXPECT_EQ(weapon.mFalloutData.mAnimationType, 6u);
        EXPECT_EQ(weapon.mFalloutData.mReloadAnimation, 7);
        EXPECT_TRUE(weapon.mFalloutData.mProjectile.isZeroOrUnset());
        EXPECT_EQ(weapon.mIcon, "icons/weapon.dds");
    }

    TEST(Esm4WeaponTest, skipsTruncatedAndNonFalloutDnamWithoutLosingAlignment)
    {
        const ESM4::Weapon truncated = loadWeapon(std::string(kUnsupportedTruncatedDnamBytes, '\0'));
        EXPECT_FALSE(truncated.mFalloutData.mPresent);
        EXPECT_EQ(truncated.mIcon, "icons/weapon.dds");

        const ESM4::Weapon otherGame
            = loadWeapon(makeFalloutDnam(kExtendedDnamFixtureBytes), ESM4Test::kOtherPluginVersion);
        EXPECT_FALSE(otherGame.mFalloutData.mPresent);
        EXPECT_EQ(otherGame.mIcon, "icons/weapon.dds");
    }
}
