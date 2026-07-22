#ifndef OPENMW_ESM4_EXPLOSION_H
#define OPENMW_ESM4_EXPLOSION_H

#include <cstdint>
#include <span>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    /// Fallout 3/New Vegas EXPL record. Later games reuse EXPL with different layouts, so this loader is dispatched
    /// only for a detected Fallout: New Vegas session.
    struct Explosion
    {
        enum Flags : std::uint32_t
        {
            AlwaysUseWorldOrientation = 1u << 1,
            KnockDownAlways = 1u << 2,
            KnockDownByFormula = 1u << 3,
            IgnoreLineOfSight = 1u << 4,
            PushSourceReferenceOnly = 1u << 5,
            IgnoreImageSpaceSwap = 1u << 6,
        };

        enum SoundLevel : std::uint32_t
        {
            Loud = 0,
            Normal = 1,
            Silent = 2,
        };

        struct Data
        {
            float force = 0.f;
            float damage = 0.f;
            float radius = 0.f;
            ESM::FormId light;
            ESM::FormId sound1;
            std::uint32_t flags = 0;
            float imageSpaceRadius = 0.f;
            ESM::FormId impactDataSet;
            ESM::FormId sound2;
            float radiationLevel = 0.f;
            float radiationDissipationTime = 0.f;
            float radiationRadius = 0.f;
            std::uint32_t soundLevel = Loud;
            bool present = false;
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        std::string mModel;
        ESM::FormId mObjectEffect;
        ESM::FormId mImageSpaceModifier;
        ESM::FormId mPlacedImpactObject;
        Data mData;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_EXPL4;
    };

    [[nodiscard]] bool loadFalloutExplosionData(std::span<const std::uint8_t> bytes, Explosion::Data& data);
}

#endif
