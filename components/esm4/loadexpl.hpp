#ifndef OPENMW_COMPONENTS_ESM4_LOADEXPL_H
#define OPENMW_COMPONENTS_ESM4_LOADEXPL_H

#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>
#include <components/esm/path.hpp>

namespace ESM4
{
    class Reader;

    struct Explosion
    {
        enum Flags : std::uint32_t
        {
            RadiusInBsUnits = 1u << 0,
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
            float mForce = 0.f;
            float mDamage = 0.f;
            float mRadius = 0.f;
            ESM::FormId mLight;
            ESM::FormId mSound1;
            std::uint32_t mFlags = 0;
            float mImageSpaceRadius = 0.f;
            ESM::FormId mImpactDataSet;
            ESM::FormId mSound2;
            float mRadiationLevel = 0.f;
            float mRadiationDissipationTime = 0.f;
            float mRadiationRadius = 0.f;
            std::uint32_t mSoundLevel = Loud;
            bool mPresent = false;
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        ESM::Path mModel;
        ESM::FormId mObjectEffect;
        ESM::FormId mImageSpaceModifier;
        ESM::FormId mPlacedImpactObject;
        Data mData;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_EXPL4;
    };
}

#endif
