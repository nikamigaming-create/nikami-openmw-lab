#ifndef OPENMW_ESM4_IMPACTDATA_H
#define OPENMW_ESM4_IMPACTDATA_H

#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

#include "decaldata.hpp"

namespace ESM4
{
    class Reader;

    struct ImpactData
    {
        struct Data
        {
            float mEffectDuration = 0.f;
            std::uint32_t mOrientation = 0;
            float mAngleThreshold = 0.f;
            float mPlacementRadius = 0.f;
            std::uint32_t mSoundLevel = 0;
            std::uint32_t mFlags = 0;
            bool mPresent = false;

            static constexpr std::uint32_t NoDecalData = 1u << 0;
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mModel;
        ESM::FormId mTextureSet;
        ESM::FormId mSound;
        Data mData;
        DecalData mDecal;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_IPCT4;
    };
}

#endif
