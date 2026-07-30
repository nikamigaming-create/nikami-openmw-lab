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
