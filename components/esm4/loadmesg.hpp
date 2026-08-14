#ifndef OPENMW_COMPONENTS_ESM4_LOADMESG_H
#define OPENMW_COMPONENTS_ESM4_LOADMESG_H

#include <cstdint>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

#include "script.hpp"

namespace ESM4
{
    class Reader;

    struct MessageButton
    {
        std::string mText;
        std::vector<TargetCondition> mConditions;
    };

    struct Message
    {
        ESM::FormId mId{};
        std::uint32_t mFlags = 0;

        std::string mEditorId;
        std::string mDescription;
        std::string mFullName;
        ESM::FormId mIcon{};
        std::uint32_t mMessageFlags = 0;
        std::uint32_t mDisplayTime = 0;
        std::vector<MessageButton> mButtons;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_MESG4;
    };
}

#endif
