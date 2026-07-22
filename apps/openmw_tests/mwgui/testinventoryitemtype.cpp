#include <gtest/gtest.h>

#include <components/esm4/loadammo.hpp>

#include "apps/openmw/mwgui/inventoryitemtype.hpp"

namespace MWGui
{
    TEST(InventoryItemTypeTest, NativeFalloutWeaponsUseTheInventoryWeaponPolicy)
    {
        EXPECT_TRUE(isInventoryWeaponType(ESM::Weapon::sRecordId));
        EXPECT_TRUE(isInventoryWeaponType(ESM4::Weapon::sRecordId));
        EXPECT_FALSE(isInventoryWeaponType(ESM::Armor::sRecordId));
        EXPECT_FALSE(isInventoryWeaponType(ESM4::Armor::sRecordId));
    }

    TEST(InventoryItemTypeTest, NativeFalloutArmorUsesTheDurabilityEquipPolicy)
    {
        EXPECT_TRUE(isInventoryWeaponOrArmorType(ESM::Weapon::sRecordId));
        EXPECT_TRUE(isInventoryWeaponOrArmorType(ESM4::Weapon::sRecordId));
        EXPECT_TRUE(isInventoryWeaponOrArmorType(ESM::Armor::sRecordId));
        EXPECT_TRUE(isInventoryWeaponOrArmorType(ESM4::Armor::sRecordId));
        EXPECT_FALSE(isInventoryWeaponOrArmorType(ESM4::Ammunition::sRecordId));
    }
}
