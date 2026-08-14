/*
  Copyright (C) 2019, 2020 cc9cii

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  cc9cii cc9c@iinet.net.au

  Much of the information on the data structures are based on the information
  from Tes4Mod:Mod_File_Format and Tes5Mod:File_Formats but also refined by
  trial & error.  See http://en.uesp.net/wiki for details.

*/
#ifndef ESM4_TERM_H
#define ESM4_TERM_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

#include "script.hpp"

namespace ESM4
{
    class Reader;
    class Writer;

    struct Terminal
    {
        struct Data
        {
            std::array<std::uint8_t, 4> mBytes{};
            std::uint8_t mSerializedSize = 0;
        };

        struct MenuItem
        {
            std::string mText;
            std::string mResultText;
            std::uint8_t mFlags = 0;
            std::optional<ESM::FormId> mDisplayNote;
            std::optional<ESM::FormId> mSubmenu;
            ScriptDefinition mScript;
            std::vector<TargetCondition> mConditions;
        };

        ESM::FormId mId{}; // from the header
        std::uint32_t mFlags = 0; // from the header, see enum type RecordFlag for details

        std::string mEditorId;
        std::string mFullName;
        std::string mText;

        std::array<std::uint8_t, 12> mObjectBounds{};
        std::string mModel;
        std::vector<std::uint8_t> mModelData;
        std::vector<std::uint8_t> mModelTextureSwaps;

        // Retained for source compatibility with the old loader. Fallout:
        // New Vegas authors one RNAM per menu item; mMenuItems preserves every
        // occurrence while this member mirrors the final item.
        std::string mResultText;

        ESM::FormId mScriptId{};
        ESM::FormId mPasswordNote{};
        ESM::FormId mSound{};

        Data mData;
        std::vector<MenuItem> mMenuItems;

        void load(ESM4::Reader& reader);
        // void save(ESM4::Writer& writer) const;

        // void blank();
        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_TERM4;
    };
}

#endif // ESM4_TERM_H
