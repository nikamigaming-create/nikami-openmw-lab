#ifndef OPENMW_COMPONENTS_ESM4_FALLOUTFORMAT_H
#define OPENMW_COMPONENTS_ESM4_FALLOUTFORMAT_H

#include <cstdint>

#include <components/esm/common.hpp>

namespace ESM4::Fallout
{
    // These values describe the on-disk Fallout ESM4 schemas. They are not
    // user-tunable settings: changing them would change record alignment and
    // make the reader unsafe. Keep them named and centralized so every loader
    // uses the same reviewed contract.
    inline constexpr std::uint32_t kOnDiskFormIdBytes = sizeof(std::uint32_t);

    inline constexpr std::uint32_t kAmmunitionTes4DataBytes = 18;
    inline constexpr std::uint32_t kAmmunitionFalloutDataBytes = 13;
    inline constexpr std::uint32_t kAmmunitionTes5DataBytes = 16;
    inline constexpr std::uint32_t kAmmunitionSseDataBytes = 20;
    inline constexpr std::uint32_t kAmmunitionShortDataBytes = 8;
    inline constexpr std::uint32_t kAmmunitionDat2BaseBytes = 12;
    inline constexpr std::uint32_t kAmmunitionDat2FullBytes = 20;
    inline constexpr std::uint32_t kAmmunitionFlagsByteMask = 0xff;

    inline constexpr std::uint32_t kWeaponTes4DataBytes = 10;
    inline constexpr std::uint32_t kWeaponFalloutDataBytes = 15;
    inline constexpr std::uint32_t kWeaponTes5DataBytes = 30;
    inline constexpr std::uint32_t kExplosionDataBytes = 52;
    inline constexpr std::uint32_t kImpactDataBytes = 24;
    inline constexpr std::uint32_t kProjectileDataBytes = 68;
    inline constexpr std::uint32_t kProjectileDataWithRotationBytes = 84;

    inline constexpr std::uint32_t kImpactDataSetMinMaterials = 9;
    inline constexpr std::uint32_t kImpactDataSetMaxMaterials = 12;
    inline constexpr std::uint32_t kImpactDataSetMinBytes
        = kImpactDataSetMinMaterials * kOnDiskFormIdBytes;
    inline constexpr std::uint32_t kImpactDataSetMaxBytes
        = kImpactDataSetMaxMaterials * kOnDiskFormIdBytes;

    inline constexpr std::uint32_t kWeaponDnamAnimationBytes = 16;
    inline constexpr std::uint32_t kWeaponDnamBallisticsBytes = 68;
    inline constexpr std::uint32_t kWeaponDnamPaddingBytes = kOnDiskFormIdBytes;

    inline constexpr std::uint32_t kClassFalloutDataBytes = 28;
    inline constexpr std::uint32_t kClassFalloutAttributesBytes = 7;
    inline constexpr std::uint32_t kClassFalloutTagActorValueCount = 4;
    inline constexpr std::uint32_t kClassFalloutReservedBytes = 2;
    inline constexpr std::uint32_t kRaceFalloutDataBytes = 36;
    inline constexpr std::uint32_t kRaceFalloutSkillBoostCount = 7;
    inline constexpr std::uint32_t kRaceFalloutReservedBytes = 2;

    inline constexpr bool isNewVegasVersion(std::uint32_t version)
    {
        return version == ESM::VER_132 || version == ESM::VER_133 || version == ESM::VER_134;
    }
}

#endif
