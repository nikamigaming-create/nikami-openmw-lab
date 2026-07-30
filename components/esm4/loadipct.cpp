#include "loadipct.hpp"

#include <stdexcept>

#include "reader.hpp"

void ESM4::ImpactData::load(Reader& reader)
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
            case ESM::fourCC("MODL"):
                reader.getZString(mModel);
                break;
            case ESM::fourCC("DNAM"):
                reader.getFormId(mTextureSet);
                break;
            case ESM::fourCC("SNAM"):
                reader.getFormId(mSound);
                break;
            case ESM::fourCC("MODT"):
            case ESM::fourCC("DATA"):
            case ESM::fourCC("DODT"):
            case ESM::fourCC("NAM1"):
                reader.skipSubRecordData();
                break;
            default:
                throw std::runtime_error(
                    "ESM4::ImpactData::load - unknown Fallout New Vegas subrecord "
                    + ESM::printName(subHeader.typeId));
        }
    }
}
