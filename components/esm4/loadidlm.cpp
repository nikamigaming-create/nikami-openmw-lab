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
#include "loadidlm.hpp"

<<<<<<< HEAD
=======
#include <algorithm>
>>>>>>> origin/main
#include <stdexcept>

#include "reader.hpp"
//#include "writer.hpp"

void ESM4::IdleMarker::load(ESM4::Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    std::uint32_t esmVer = reader.esmVersion();

    while (reader.getSubRecordHeader())
    {
        const ESM4::SubRecordHeader& subHdr = reader.subRecordHeader();
        switch (subHdr.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("IDLF"):
                reader.get(mIdleFlags);
                break;
            case ESM::fourCC("IDLC"):
<<<<<<< HEAD
                if (subHdr.dataSize != 1) // FO3 can have 4?
                {
                    reader.skipSubRecordData();
                    break;
                }

                reader.get(mIdleCount);
=======
                if (subHdr.dataSize == 1)
                    reader.get(mIdleCount);
                else if (subHdr.dataSize == 4)
                {
                    std::uint32_t count = 0;
                    reader.get(count);
                    mIdleCount = static_cast<std::uint8_t>(std::min<std::uint32_t>(count, 0xff));
                }
                else
                    reader.skipSubRecordData();
>>>>>>> origin/main
                break;
            case ESM::fourCC("IDLT"):
                reader.get(mIdleTimer);
                break;
            case ESM::fourCC("IDLA"):
            {
<<<<<<< HEAD
                bool isFONV = esmVer == ESM::VER_132 || esmVer == ESM::VER_133 || esmVer == ESM::VER_134;
                if (esmVer == ESM::VER_094 || isFONV) // FO3? 4 or 8 bytes
=======
                if (subHdr.dataSize % sizeof(ESM::FormId32) != 0)
>>>>>>> origin/main
                {
                    reader.skipSubRecordData();
                    break;
                }

<<<<<<< HEAD
                mIdleAnim.resize(mIdleCount);
=======
                const std::size_t idleCount = subHdr.dataSize / sizeof(ESM::FormId32);
                mIdleAnim.resize(idleCount);
>>>>>>> origin/main
                for (ESM::FormId& value : mIdleAnim)
                    reader.getFormId(value);
                break;
            }
            case ESM::fourCC("MODL"):
                reader.getZString(mModel);
                break;
            case ESM::fourCC("OBND"): // object bounds
            case ESM::fourCC("KSIZ"):
            case ESM::fourCC("KWDA"):
            case ESM::fourCC("MODT"): // Model data
            case ESM::fourCC("MODC"):
            case ESM::fourCC("MODS"):
            case ESM::fourCC("MODF"): // Model data end
            case ESM::fourCC("QNAM"):
                reader.skipSubRecordData();
                break;
            default:
<<<<<<< HEAD
=======
                if (reader.skipUnknownStarfieldSubRecordData("loadidlm"))
                    break;
>>>>>>> origin/main
                throw std::runtime_error("ESM4::IDLM::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}

// void ESM4::IdleMarker::save(ESM4::Writer& writer) const
//{
// }

// void ESM4::IdleMarker::blank()
//{
// }
