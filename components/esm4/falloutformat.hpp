#ifndef OPENMW_COMPONENTS_ESM4_FALLOUTFORMAT_H
#define OPENMW_COMPONENTS_ESM4_FALLOUTFORMAT_H

#include <cstddef>
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

    inline constexpr std::uint32_t kFactionRelationBytes = 12;
    inline constexpr std::uint32_t kFactionDataFlagBytes = 2;
    inline constexpr std::uint32_t kFactionDataShortBytes = 1;
    inline constexpr std::uint32_t kFactionDataLongBytes = 4;
    inline constexpr std::uint32_t kFactionDataUnusedBytes = 2;
    inline constexpr std::uint32_t kFactionDataSerializedSizeBytes = 1;
    inline constexpr std::uint32_t kFactionDataUnusedFirstIndex = 0;
    inline constexpr std::uint32_t kFactionDataUnusedSecondIndex = 1;
    inline constexpr std::uint32_t kFactionRankBytes = 4;
    inline constexpr std::uint32_t kFactionCrimeGoldMultiplierBytes = sizeof(float);
    inline constexpr std::uint32_t kFactionReputationBytes = kOnDiskFormIdBytes;
    inline constexpr std::uint32_t kEmptySubrecordBytes = 0;

    inline constexpr std::uint32_t kRecipeCategoryDataBytes = sizeof(std::uint8_t);
    inline constexpr std::uint32_t kRecipeDataFormIdCount = 2;
    inline constexpr std::uint32_t kRecipeDataBytes = sizeof(std::int32_t) + sizeof(std::uint32_t)
        + kRecipeDataFormIdCount * kOnDiskFormIdBytes;
    inline constexpr std::uint32_t kRecipeItemBytes = kOnDiskFormIdBytes;
    inline constexpr std::uint32_t kRecipeQuantityBytes = sizeof(std::uint32_t);
    inline constexpr std::uint32_t kRecipeNullFormId = 0;
    inline constexpr std::int32_t kRecipeDefaultRequiredSkill = -1;
    inline constexpr std::uint32_t kRecipeDefaultRequiredSkillLevel = 0;
    inline constexpr std::uint32_t kRecipeDefaultQuantity = 0;
    inline constexpr std::uint32_t kRecipeMinimumQuantity = 1;
    // FNV actor-value IDs used by the native skill gate. These are authored
    // protocol values, not tunable gameplay thresholds.
    inline constexpr std::uint32_t kRecipeSkillActorValueBegin = 32;
    inline constexpr std::uint32_t kRecipeSkillActorValueEnd = 45;

    // Fallout 3/New Vegas LIP container facts. These values describe the
    // authored binary format and are shared by the decoder and its API.
    inline constexpr std::uint32_t kLipVersion = 1;
    inline constexpr std::uint32_t kLipCompressedFlag = 0x1;
    inline constexpr std::uint32_t kLipBigEndianFlag = 0x2;
    inline constexpr std::uint32_t kLipSupportedFlags = kLipCompressedFlag | kLipBigEndianFlag;
    inline constexpr std::uint32_t kLipUncompressedPayloadMarker = 2;
    inline constexpr std::size_t kLipFileHeaderBytes = 3 * sizeof(std::uint32_t);
    inline constexpr std::size_t kLipDecodedHeaderBytes = 3 * sizeof(std::uint32_t);
    inline constexpr std::size_t kLipStoredSizeOverheadBytes = 16;
    inline constexpr std::size_t kLipImplicitTailBytes = 4;
    inline constexpr std::size_t kLipTargetCount = 33;
    inline constexpr float kLipFramesPerSecond = 30.f;

    // CTDA target-condition payload variants used by the Fallout/TES loaders.
    // The widths and tail are authored container facts, not runtime policy.
    inline constexpr std::uint32_t kTargetConditionTes4Bytes = 24;
    inline constexpr std::uint32_t kTargetConditionFalloutBytes = 20;
    inline constexpr std::uint32_t kTargetConditionNativeBytes = 7 * sizeof(std::uint32_t);
    inline constexpr std::uint32_t kTargetConditionTes5PrefixBytes = 20;
    inline constexpr std::uint32_t kTargetConditionTes5Bytes = 36;
    inline constexpr std::uint32_t kTargetConditionTes5TailBytes = 4;

    // Fallout WTHR DATA is a byte-packed fifteen-channel payload. Keep the
    // field offsets here with the other authored record-layout facts so the
    // loader cannot drift from the reviewed on-disk schema.
    inline constexpr std::size_t kWeatherDataSerializedSize = 15;
    inline constexpr std::size_t kWeatherDataWindSpeedOffset = 0;
    inline constexpr std::size_t kWeatherDataLowerCloudSpeedOffset = 1;
    inline constexpr std::size_t kWeatherDataUpperCloudSpeedOffset = 2;
    inline constexpr std::size_t kWeatherDataTransitionDeltaOffset = 3;
    inline constexpr std::size_t kWeatherDataSunGlareOffset = 4;
    inline constexpr std::size_t kWeatherDataSunDamageOffset = 5;
    inline constexpr std::size_t kWeatherDataPrecipitationBeginFadeInOffset = 6;
    inline constexpr std::size_t kWeatherDataPrecipitationEndFadeOutOffset = 7;
    inline constexpr std::size_t kWeatherDataLightningBeginFadeInOffset = 8;
    inline constexpr std::size_t kWeatherDataLightningEndFadeOutOffset = 9;
    inline constexpr std::size_t kWeatherDataLightningFrequencyOffset = 10;
    inline constexpr std::size_t kWeatherDataClassificationOffset = 11;
    inline constexpr std::size_t kWeatherDataLightningColorRedOffset = 12;
    inline constexpr std::size_t kWeatherDataLightningColorGreenOffset = 13;
    inline constexpr std::size_t kWeatherDataLightningColorBlueOffset = 14;

    // Fallout CLMT weather-list and TNAM timing layouts. The masks are the
    // authored bit assignments in the compact moon-information byte.
    inline constexpr std::size_t kClimateWeatherTypeSerializedSize
        = 2 * sizeof(std::uint32_t) + sizeof(std::int32_t);
    inline constexpr std::size_t kClimateTimingSerializedSize = 6;
    inline constexpr std::size_t kClimateTimingSunriseBeginOffset = 0;
    inline constexpr std::size_t kClimateTimingSunriseEndOffset = 1;
    inline constexpr std::size_t kClimateTimingSunsetBeginOffset = 2;
    inline constexpr std::size_t kClimateTimingSunsetEndOffset = 3;
    inline constexpr std::size_t kClimateTimingVolatilityOffset = 4;
    inline constexpr std::size_t kClimateTimingMoonInfoOffset = 5;
    inline constexpr std::uint8_t kClimateMoonPhaseLengthMask = 0x3f;
    inline constexpr std::uint8_t kClimateSecundaFlag = 0x40;
    inline constexpr std::uint8_t kClimateMasserFlag = 0x80;

    inline constexpr std::uint32_t kNoteObjectBoundsBytes = 12;
    inline constexpr std::uint32_t kNoteDataBytes = 1;
    inline constexpr std::uint8_t kNoteDataEmpty = 0;
    inline constexpr std::uint8_t kNoteDataText = 1;
    inline constexpr std::uint8_t kNoteDataImage = 2;
    inline constexpr std::uint8_t kNoteDataVoice = 3;
    inline constexpr std::uint8_t kNoteDataMaximum = kNoteDataVoice;
    inline constexpr std::uint32_t kNoteFormIdBytes = kOnDiskFormIdBytes;
    inline constexpr std::uint32_t kNoteNullFormIdIndex = 0;
    inline constexpr std::size_t kNoteMaximumQuestReferences = 4;
    inline constexpr std::uint32_t kNoteBoundRadiusBytes = sizeof(float);
    inline constexpr std::uint32_t kNoteAllowedRecordFlags = 0;

    inline constexpr bool isNewVegasVersion(std::uint32_t version)
    {
        return version == ESM::VER_132 || version == ESM::VER_133 || version == ESM::VER_134;
    }
}

#endif
