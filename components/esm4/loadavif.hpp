#ifndef OPENMW_COMPONENTS_ESM4_LOADAVIF_H
#define OPENMW_COMPONENTS_ESM4_LOADAVIF_H

#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    struct ActorValueInformation
    {
        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        std::string mDescription;
        std::string mLargeIcon;
        std::string mSmallIcon;
        std::string mShortName;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::REC_AVIF4;
    };
}

#endif // OPENMW_COMPONENTS_ESM4_LOADAVIF_H
