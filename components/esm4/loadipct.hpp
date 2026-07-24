#ifndef OPENMW_ESM4_IMPACTDATA_H
#define OPENMW_ESM4_IMPACTDATA_H

#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    struct ImpactData
    {
        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mModel;
        ESM::FormId mTextureSet;
        ESM::FormId mSound;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_IPCT4;
    };
}

#endif
