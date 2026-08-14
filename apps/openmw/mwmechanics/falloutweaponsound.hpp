#ifndef OPENMW_MWMECHANICS_FALLOUTWEAPONSOUND_H
#define OPENMW_MWMECHANICS_FALLOUTWEAPONSOUND_H

#include <cstdint>
#include <optional>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>
#include <components/esm4/loadweap.hpp>

namespace MWMechanics
{
    enum class FalloutWeaponSoundEvent : std::uint8_t
    {
        Fire,
        DryFire,
        Equip,
        Unequip,
    };

    /// Select the exact authored WEAP sound FormID for one semantic event. For Fire, localPlayer selects the XNAM
    /// 2D field and a non-player selects the first SNAM 3D field; modded selects WMS2/WMS1 respectively. Other
    /// events map to TNAM/NAM9/NAM8. Missing or zero fields return no sound and never fall back to a Morrowind ID,
    /// another Fallout event, or the second SNAM distant layer.
    [[nodiscard]] inline std::optional<ESM::FormId> selectAuthoredFalloutWeaponSound(
        const ESM4::Weapon& weapon, FalloutWeaponSoundEvent event, bool localPlayer, bool modded = false) noexcept
    {
        std::uint32_t type = 0;
        switch (event)
        {
            case FalloutWeaponSoundEvent::Fire:
                type = modded ? (localPlayer ? ESM::fourCC("WMS2") : ESM::fourCC("WMS1"))
                              : (localPlayer ? ESM::fourCC("XNAM") : ESM::fourCC("SNAM"));
                break;
            case FalloutWeaponSoundEvent::DryFire:
                type = ESM::fourCC("TNAM");
                break;
            case FalloutWeaponSoundEvent::Equip:
                type = ESM::fourCC("NAM9");
                break;
            case FalloutWeaponSoundEvent::Unequip:
                type = ESM::fourCC("NAM8");
                break;
        }

        for (const ESM4::Weapon::SoundRef& sound : weapon.mSoundRefs)
        {
            if (sound.mType != type)
                continue;
            if (sound.mSound.isZeroOrUnset())
                return std::nullopt;
            return sound.mSound;
        }
        return std::nullopt;
    }
}

#endif
