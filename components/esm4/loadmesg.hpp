/*
  Copyright (C) 2026

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.
*/
#ifndef ESM4_MESG_H
#define ESM4_MESG_H

#include <cstdint>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    // A TES4-family message box. Fallout 3 and New Vegas use MESG records for
    // authored character-generation and tutorial choices; retaining the text
    // and buttons lets the runtime present those choices without a parallel
    // campaign-specific UI table.
    struct Message
    {
        ESM::FormId mId{};
        std::uint32_t mFlags = 0;

        std::string mEditorId;
        std::string mDescription;
        std::string mFullName;
        std::uint32_t mData = 0;
        std::vector<std::string> mButtons;

        void load(ESM4::Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_MESG4;
    };
}

#endif
