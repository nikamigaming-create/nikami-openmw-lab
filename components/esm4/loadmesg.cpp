/*
  Copyright (C) 2026

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.
*/
#include "loadmesg.hpp"

#include <stdexcept>

#include "reader.hpp"

void ESM4::Message::load(ESM4::Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    while (reader.getSubRecordHeader())
    {
        const ESM4::SubRecordHeader& subHdr = reader.subRecordHeader();
        switch (subHdr.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("DESC"):
                reader.getLocalizedString(mDescription);
                break;
            case ESM::fourCC("FULL"):
                reader.getLocalizedString(mFullName);
                break;
            case ESM::fourCC("DNAM"):
                if (subHdr.dataSize == sizeof(mData))
                    reader.get(mData);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("ITXT"):
            {
                std::string button;
                reader.getLocalizedString(button);
                mButtons.push_back(std::move(button));
                break;
            }
            // Fallout 3/New Vegas' INAM is a four-byte display/icon field.
            // It is preserved as raw source data by the loader boundary but
            // has no effect on the native OpenMW message presentation.
            case ESM::fourCC("INAM"):
                reader.skipSubRecordData();
                break;
            default:
                // MESG grew additional presentation fields in later games.
                // Keep the cross-game reader forward-compatible while the
                // actual message body and authored choices remain usable.
                if (reader.skipUnknownStarfieldSubRecordData("loadmesg"))
                    break;
                reader.skipSubRecordData();
                break;
        }
    }
}
