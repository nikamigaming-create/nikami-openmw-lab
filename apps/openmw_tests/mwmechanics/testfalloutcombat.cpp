#include <apps/openmw/mwmechanics/falloutcombat.hpp>

#include <components/esm4/loadproj.hpp>
#include <components/esm4/loadweap.hpp>

#include <gtest/gtest.h>

#include <array>
#include <unordered_map>

namespace
{
    ESM::FormId id(std::uint32_t value)
    {
        return ESM::FormId::fromUint32(value);
    }

    TEST(FalloutCombatTest, SelectsFirstAvailableAmmoInAuthoredListOrder)
    {
        const std::array candidates{ id(0x10), id(0x20), id(0x30) };
        const std::unordered_map<std::uint32_t, int> counts{ { 0x10, 0 }, { 0x20, 2 }, { 0x30, 99 } };

        const auto selected = MWMechanics::selectAuthoredFalloutAmmo(candidates, 1,
            [](ESM::FormId candidate) { return candidate.toUint32() != 0x10; },
            [&](ESM::FormId candidate) { return counts.at(candidate.toUint32()); });

        ASSERT_TRUE(selected);
        EXPECT_EQ(selected->toUint32(), 0x20);
    }

    TEST(FalloutCombatTest, DoesNotInventFallbackAmmo)
    {
        const std::array candidates{ id(0x10), id(0x20) };
        const auto selected = MWMechanics::selectAuthoredFalloutAmmo(candidates, 2,
            [](ESM::FormId) { return true; }, [](ESM::FormId) { return 1; });
        EXPECT_FALSE(selected);
    }

    TEST(FalloutCombatTest, PreservesExactRetailServiceRifleShotContract)
    {
        ESM4::Weapon weapon;
        weapon.mData.hasBallistics = true;
        weapon.mData.projectile = id(0x426d);
        weapon.mData.ammoUse = 1;
        weapon.mData.numProjectiles = 1;
        weapon.mData.damage = 18;
        weapon.mData.minRange = 768.f;
        weapon.mData.maxRange = 3548.f;

        ESM4::Projectile projectile;
        projectile.mId = id(0x426d);
        projectile.mData.present = true;
        projectile.mData.flags = ESM4::Projectile::Hitscan;
        projectile.mData.range = 10000.f;

        MWMechanics::FalloutShotFailure failure;
        const auto contract = MWMechanics::buildFalloutHitscanContract(weapon, projectile, id(0x4240), failure);

        ASSERT_TRUE(contract);
        EXPECT_EQ(failure, MWMechanics::FalloutShotFailure::None);
        EXPECT_EQ(contract->mAmmo.toUint32(), 0x4240);
        EXPECT_EQ(contract->mProjectile.toUint32(), 0x426d);
        EXPECT_EQ(contract->mAmmoUse, 1);
        EXPECT_EQ(contract->mProjectileCount, 1);
        EXPECT_FLOAT_EQ(contract->mDamage, 18.f);
        EXPECT_FLOAT_EQ(contract->mMinRange, 768.f);
        EXPECT_FLOAT_EQ(contract->mMaxRange, 3548.f);
        EXPECT_FLOAT_EQ(contract->mProjectileRange, 10000.f);
    }

    TEST(FalloutCombatTest, FailsClosedForAnUnimplementedMultiProjectileWeapon)
    {
        ESM4::Weapon weapon;
        weapon.mData.hasBallistics = true;
        weapon.mData.projectile = id(0x426d);
        weapon.mData.ammoUse = 1;
        weapon.mData.numProjectiles = 7;
        ESM4::Projectile projectile;
        projectile.mId = id(0x426d);
        projectile.mData.present = true;
        projectile.mData.flags = ESM4::Projectile::Hitscan;
        projectile.mData.range = 10000.f;

        MWMechanics::FalloutShotFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutHitscanContract(weapon, projectile, id(0x4240), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutShotFailure::UnsupportedProjectileCount);
    }
}
