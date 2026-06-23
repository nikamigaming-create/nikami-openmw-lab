/*
  Copyright (C) 2026 Nikami

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software in
     a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#ifndef ESM4_PROJ_H
#define ESM4_PROJ_H

#include <cstdint>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;
    class Writer;

    struct Projectile
    {
        ESM::FormId mId;
        std::uint32_t mFlags;

        std::string mEditorId;
        std::string mFullName;
        std::string mModel;

        std::uint32_t mSoundLevel;
        std::vector<std::uint8_t> mData;
        std::vector<std::uint8_t> mObjectBounds;
        std::vector<std::uint8_t> mModelData;
        std::vector<std::uint8_t> mNameData1;
        std::vector<std::uint8_t> mNameData2;
        std::vector<std::vector<std::uint8_t>> mDestructibleData;

        void load(ESM4::Reader& reader);
        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_PROJ4;
    };
}

#endif // ESM4_PROJ_H
