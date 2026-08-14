#include <apps/openmw/mwmechanics/falloutweaponsound.hpp>

#include <gtest/gtest.h>

namespace
{
    ESM::FormId id(std::uint32_t value)
    {
        return ESM::FormId::fromUint32(value);
    }

    void add(ESM4::Weapon& weapon, std::uint32_t type, std::uint32_t value)
    {
        weapon.mSoundRefs.push_back({ type, id(value) });
    }

    TEST(FalloutWeaponSoundTest, SelectsLocal2DAndFirstActor3DFireWithoutDistantSubstitution)
    {
        ESM4::Weapon weapon;
        add(weapon, ESM::fourCC("SNAM"), 0x01000100);
        add(weapon, ESM::fourCC("SNAM"), 0x01000101);
        add(weapon, ESM::fourCC("XNAM"), 0x01000102);

        EXPECT_EQ(
            MWMechanics::selectAuthoredFalloutWeaponSound(weapon, MWMechanics::FalloutWeaponSoundEvent::Fire, true),
            id(0x01000102));
        EXPECT_EQ(
            MWMechanics::selectAuthoredFalloutWeaponSound(weapon, MWMechanics::FalloutWeaponSoundEvent::Fire, false),
            id(0x01000100));
    }

    TEST(FalloutWeaponSoundTest, SelectsOnlyExactDryEquipAndUnequipFields)
    {
        ESM4::Weapon weapon;
        add(weapon, ESM::fourCC("TNAM"), 0x01000200);
        add(weapon, ESM::fourCC("NAM9"), 0x01000201);
        add(weapon, ESM::fourCC("NAM8"), 0x01000202);

        EXPECT_EQ(
            MWMechanics::selectAuthoredFalloutWeaponSound(weapon, MWMechanics::FalloutWeaponSoundEvent::DryFire, true),
            id(0x01000200));
        EXPECT_EQ(
            MWMechanics::selectAuthoredFalloutWeaponSound(weapon, MWMechanics::FalloutWeaponSoundEvent::Equip, false),
            id(0x01000201));
        EXPECT_EQ(
            MWMechanics::selectAuthoredFalloutWeaponSound(weapon, MWMechanics::FalloutWeaponSoundEvent::Unequip, true),
            id(0x01000202));
    }

    TEST(FalloutWeaponSoundTest, SelectsModdedPerspectiveWithoutBaseFallback)
    {
        ESM4::Weapon weapon;
        add(weapon, ESM::fourCC("SNAM"), 0x01000300);
        add(weapon, ESM::fourCC("XNAM"), 0x01000301);
        add(weapon, ESM::fourCC("WMS1"), 0x01000302);
        add(weapon, ESM::fourCC("WMS2"), 0x01000303);

        EXPECT_EQ(MWMechanics::selectAuthoredFalloutWeaponSound(
                      weapon, MWMechanics::FalloutWeaponSoundEvent::Fire, false, true),
            id(0x01000302));
        EXPECT_EQ(MWMechanics::selectAuthoredFalloutWeaponSound(
                      weapon, MWMechanics::FalloutWeaponSoundEvent::Fire, true, true),
            id(0x01000303));

        weapon.mSoundRefs.erase(weapon.mSoundRefs.begin() + 2, weapon.mSoundRefs.end());
        EXPECT_FALSE(MWMechanics::selectAuthoredFalloutWeaponSound(
            weapon, MWMechanics::FalloutWeaponSoundEvent::Fire, false, true));
        EXPECT_FALSE(MWMechanics::selectAuthoredFalloutWeaponSound(
            weapon, MWMechanics::FalloutWeaponSoundEvent::Fire, true, true));
    }

    TEST(FalloutWeaponSoundTest, MissingOrZeroAuthoredFieldNeverInventsDefault)
    {
        ESM4::Weapon weapon;
        add(weapon, ESM::fourCC("SNAM"), 0);
        add(weapon, ESM::fourCC("SNAM"), 0x01000401);
        add(weapon, ESM::fourCC("NAM9"), 0x01000402);

        EXPECT_FALSE(
            MWMechanics::selectAuthoredFalloutWeaponSound(weapon, MWMechanics::FalloutWeaponSoundEvent::Fire, false));
        EXPECT_FALSE(
            MWMechanics::selectAuthoredFalloutWeaponSound(weapon, MWMechanics::FalloutWeaponSoundEvent::Fire, true));
        EXPECT_FALSE(
            MWMechanics::selectAuthoredFalloutWeaponSound(weapon, MWMechanics::FalloutWeaponSoundEvent::DryFire, true));
    }
}
