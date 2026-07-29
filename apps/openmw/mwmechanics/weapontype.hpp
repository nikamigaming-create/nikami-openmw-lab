#ifndef GAME_MWMECHANICS_WEAPONTYPE_H
#define GAME_MWMECHANICS_WEAPONTYPE_H

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace ESM
{
    struct WeaponType;
}

namespace MWWorld
{
    class Ptr;

    template <class PtrType>
    class ContainerStoreIteratorBase;

    using ContainerStoreIterator = ContainerStoreIteratorBase<Ptr>;
}

namespace MWMechanics
{
    // FNV WEAP.DNAM animation types are deliberately kept outside the ESM3
    // weapon enum.  Treating them as a Morrowind weapon family selects the
    // wrong locomotion and attack groups.
    bool isFalloutWeaponType(int weaponType);
    std::optional<int> getFalloutWeaponType(std::uint8_t animationType);
    std::optional<std::uint8_t> getFalloutWeaponAnimationType(int weaponType);
    bool shouldUseFalloutWeaponState(int requestedWeaponType, int currentWeaponType);
    bool shouldTransitionFalloutWeaponState(
        int requestedWeaponType, int currentWeaponType, bool weaponChanged);

    MWWorld::ContainerStoreIterator getActiveWeapon(const MWWorld::Ptr& actor, int* weaptype);

    const ESM::WeaponType* getWeaponType(const int weaponType);

    std::vector<std::string_view> getAllWeaponTypeShortGroups();
}

#endif
