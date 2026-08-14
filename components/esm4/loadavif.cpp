#include "loadavif.hpp"

#include <stdexcept>

#include "reader.hpp"

void ESM4::ActorValueInformation::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& subHeader = reader.subRecordHeader();
        switch (subHeader.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("FULL"):
                reader.getLocalizedString(mFullName);
                break;
            case ESM::fourCC("DESC"):
                reader.getLocalizedString(mDescription);
                break;
            case ESM::fourCC("ICON"):
                reader.getZString(mLargeIcon);
                break;
            case ESM::fourCC("MICO"):
                reader.getZString(mSmallIcon);
                break;
            case ESM::fourCC("ANAM"):
                reader.getZString(mShortName);
                break;
            default:
                throw std::runtime_error(
                    "ESM4::AVIF::load - Unknown Fallout New Vegas subrecord " + ESM::printName(subHeader.typeId));
        }
    }
}
