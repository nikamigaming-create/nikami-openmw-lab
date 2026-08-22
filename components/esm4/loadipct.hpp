#ifndef OPENMW_COMPONENTS_ESM4_LOADIPCT_H
#define OPENMW_COMPONENTS_ESM4_LOADIPCT_H

#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>
#include <components/esm/path.hpp>

namespace ESM4
{
    class Reader;

    struct ImpactData
    {
        enum Flags : std::uint32_t
        {
            NoDecalData = 1u << 0,
        };

        struct Data
        {
            float mEffectDuration = 0.f;
            std::uint32_t mOrientation = 0;
            float mAngleThreshold = 0.f;
            float mPlacementRadius = 0.f;
            std::uint32_t mSoundLevel = 0;
            std::uint32_t mFlags = 0;
            bool mPresent = false;
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        ESM::Path mModel;
        ESM::FormId mTextureSet;
        ESM::FormId mSound1;
        ESM::FormId mSound2;
        Data mData;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_IPCT4;
    };
}

#endif
