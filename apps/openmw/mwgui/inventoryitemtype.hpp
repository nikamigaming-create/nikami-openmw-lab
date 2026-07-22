#ifndef OPENMW_MWGUI_INVENTORYITEMTYPE_H
#define OPENMW_MWGUI_INVENTORYITEMTYPE_H

#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadweap.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadweap.hpp>

namespace MWGui
{
    [[nodiscard]] constexpr bool isInventoryWeaponType(unsigned int type) noexcept
    {
        return type == ESM::Weapon::sRecordId || type == ESM4::Weapon::sRecordId;
    }

    [[nodiscard]] constexpr bool isInventoryWeaponOrArmorType(unsigned int type) noexcept
    {
        return isInventoryWeaponType(type) || type == ESM::Armor::sRecordId || type == ESM4::Armor::sRecordId;
    }
}

#endif
