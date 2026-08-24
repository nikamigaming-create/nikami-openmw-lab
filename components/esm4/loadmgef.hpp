#ifndef OPENMW_COMPONENTS_ESM4_LOADMGEF_H
#define OPENMW_COMPONENTS_ESM4_LOADMGEF_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    /// Fallout 3/New Vegas MGEF record. The 72-byte DATA layout is shared by the
    /// actor effects referenced from native FNV SPEL records.
    struct MagicEffect
    {
        // Fallout 3/New Vegas MGEF.DATA is a fixed native layout. Keep the
        // offsets named so binary parsing cannot silently drift into a field
        // at an unexplained byte position.
        static constexpr std::size_t sDataSerializedSize = 72;
        static constexpr std::size_t sDataFlagsOffset = 0;
        static constexpr std::size_t sDataBaseCostOffset = 4;
        static constexpr std::size_t sDataAssociatedItemOffset = 8;
        static constexpr std::size_t sDataSchoolOffset = 12;
        static constexpr std::size_t sDataResistanceActorValueOffset = 16;
        static constexpr std::size_t sDataCounterEffectCountOffset = 20;
        static constexpr std::size_t sDataLightOffset = 24;
        static constexpr std::size_t sDataProjectileSpeedOffset = 28;
        static constexpr std::size_t sDataEffectShaderOffset = 32;
        static constexpr std::size_t sDataObjectDisplayShaderOffset = 36;
        static constexpr std::size_t sDataEffectSoundOffset = 40;
        static constexpr std::size_t sDataBoltSoundOffset = 44;
        static constexpr std::size_t sDataHitSoundOffset = 48;
        static constexpr std::size_t sDataAreaSoundOffset = 52;
        static constexpr std::size_t sDataEnchantmentFactorOffset = 56;
        static constexpr std::size_t sDataBarterFactorOffset = 60;
        static constexpr std::size_t sDataArchetypeOffset = 64;
        static constexpr std::size_t sDataActorValueOffset = 68;

        enum Flags : std::uint32_t
        {
            Hostile = 1u << 0,
            Recover = 1u << 1,
            Detrimental = 1u << 2,
            Self = 1u << 4,
            Touch = 1u << 5,
            Target = 1u << 6,
            NoDuration = 1u << 7,
            NoMagnitude = 1u << 8,
            NoArea = 1u << 9,
            EffectPersists = 1u << 10,
            GoryVisuals = 1u << 12,
            DisplayNameOnly = 1u << 13,
            UseSkill = 1u << 19,
            UseAttribute = 1u << 20,
            Painless = 1u << 24,
            SprayProjectile = 1u << 25,
            BoltProjectile = 1u << 26,
            NoHitEffect = 1u << 27,
            NoDeathDispel = 1u << 28,
        };

        enum class Archetype : std::uint32_t
        {
            ValueModifier = 0,
            Script = 1,
            Dispel = 2,
            CureDisease = 3,
            Invisibility = 11,
            Chameleon = 12,
            Light = 13,
            Lock = 16,
            Open = 17,
            BoundItem = 18,
            SummonCreature = 19,
            Paralysis = 24,
            CureParalysis = 30,
            CureAddiction = 31,
            CurePoison = 32,
            Concussion = 33,
            ValueAndParts = 34,
            LimbCondition = 35,
            Turbo = 36,
        };

        struct Data
        {
            std::uint32_t flags = 0;
            float baseCost = 0.f;
            ESM::FormId associatedItem;
            std::int32_t school = -1;
            std::int32_t resistanceActorValue = -1;
            std::uint16_t counterEffectCount = 0;
            ESM::FormId light;
            float projectileSpeed = 0.f;
            ESM::FormId effectShader;
            ESM::FormId objectDisplayShader;
            ESM::FormId effectSound;
            ESM::FormId boltSound;
            ESM::FormId hitSound;
            ESM::FormId areaSound;
            float enchantmentFactor = 0.f;
            float barterFactor = 0.f;
            Archetype archetype = Archetype::ValueModifier;
            std::int32_t actorValue = -1;
            bool present = false;
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        std::string mDescription;
        std::string mIcon;
        std::string mModel;
        Data mData;
        std::vector<ESM::FormId> mCounterEffects;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::REC_MGEF4;
    };

    [[nodiscard]] bool loadFalloutMagicEffectData(std::span<const std::uint8_t> bytes, MagicEffect::Data& data);
}

#endif
