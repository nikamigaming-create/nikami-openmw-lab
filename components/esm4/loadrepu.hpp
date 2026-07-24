#ifndef OPENMW_COMPONENTS_ESM4_LOADREPU_H
#define OPENMW_COMPONENTS_ESM4_LOADREPU_H

#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    // Fallout: New Vegas REPU data. DATA is the maximum fame/infamy scale used
    // independently by both reputation axes.
    struct Reputation
    {
        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        std::string mIcon;
        float mMaximum = 0.f;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::REC_REPU4;
    };
}

#endif
