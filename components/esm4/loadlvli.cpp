/*
  Copyright (C) 2016, 2018-2020 cc9cii

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
#include "loadlvli.hpp"

#include <stdexcept>

#include "reader.hpp"
//#include "writer.hpp"

namespace
{
    enum class FnvLevelledItemState
    {
        Start,
        EditorId,
        Bounds,
        ChanceNone,
        Flags,
        ChanceGlobal,
        Entry,
        ExtraData,
    };

    bool isFalloutNewVegas(const ESM4::Reader& reader)
    {
        const std::uint32_t version = reader.esmVersion();
        return version == ESM::VER_132 || version == ESM::VER_133 || version == ESM::VER_134;
    }

    void requireSize(const ESM4::SubRecordHeader& subHdr, std::uint32_t expected, const std::string& editorId)
    {
        if (subHdr.dataSize != expected)
        {
            throw std::runtime_error("ESM4::LVLI::load - " + editorId + " " + ESM::printName(subHdr.typeId)
                + " size error");
        }
    }

    void requireFNVSequence(FnvLevelledItemState state, const ESM4::SubRecordHeader& subHdr,
        const std::string& editorId)
    {
        const std::uint32_t type = subHdr.typeId;
        const bool valid = (state == FnvLevelledItemState::Start && type == ESM::fourCC("EDID"))
            || (state == FnvLevelledItemState::EditorId
                && (type == ESM::fourCC("OBND") || type == ESM::fourCC("LVLD")))
            || (state == FnvLevelledItemState::Bounds && type == ESM::fourCC("LVLD"))
            || (state == FnvLevelledItemState::ChanceNone && type == ESM::fourCC("LVLF"))
            || (state == FnvLevelledItemState::Flags
                && (type == ESM::fourCC("LVLG") || type == ESM::fourCC("LVLO")))
            || (state == FnvLevelledItemState::ChanceGlobal && type == ESM::fourCC("LVLO"))
            || ((state == FnvLevelledItemState::Entry || state == FnvLevelledItemState::ExtraData)
                && type == ESM::fourCC("LVLO"))
            || (state == FnvLevelledItemState::Entry && type == ESM::fourCC("COED"));
        if (!valid)
        {
            throw std::runtime_error("ESM4::LVLI::load - " + editorId + " invalid Fallout New Vegas subrecord "
                + ESM::printName(type) + " or ordering");
        }
    }

    bool isCompleteFNVSequence(FnvLevelledItemState state)
    {
        return state == FnvLevelledItemState::Flags || state == FnvLevelledItemState::ChanceGlobal
            || state == FnvLevelledItemState::Entry || state == FnvLevelledItemState::ExtraData;
    }

    template <class T>
    void readExact(ESM4::Reader& reader, const ESM4::SubRecordHeader& subHdr, T& value,
        const std::string& editorId)
    {
        if (!reader.getExact(value))
        {
            throw std::runtime_error("ESM4::LVLI::load - " + editorId + " truncated "
                + ESM::printName(subHdr.typeId));
        }
    }

    template <class T>
    void readByteSubRecord(ESM4::Reader& reader, const ESM4::SubRecordHeader& subHdr, T& value)
    {
        if (subHdr.dataSize == 0)
            return;

        reader.get(value);
        if (subHdr.dataSize > sizeof(value))
            reader.skipSubRecordData(subHdr.dataSize - static_cast<std::uint32_t>(sizeof(value)));
    }
}

void ESM4::LevelledItem::load(ESM4::Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;
    mEditorId.clear();
    mHasChanceNone = false;
    mChanceNone = 0;
    mChanceGlobal = {};
    mHasLvlItemFlags = false;
    mLvlItemFlags = 0;
    mData = 0;
    mLvlObject.clear();
    mLvlObjectExtra.clear();

    const bool isFNV = isFalloutNewVegas(reader);
    FnvLevelledItemState fnvState = FnvLevelledItemState::Start;

    while (reader.getSubRecordHeader())
    {
        const ESM4::SubRecordHeader& subHdr = reader.subRecordHeader();
        if (isFNV)
            requireFNVSequence(fnvState, subHdr, mEditorId);

        switch (subHdr.typeId)
        {
            case ESM::fourCC("EDID"):
                if (isFNV && subHdr.dataSize == 0)
                    throw std::runtime_error("ESM4::LVLI::load - Fallout New Vegas EDID size error");
                if (!reader.getZString(mEditorId) && isFNV)
                    throw std::runtime_error("ESM4::LVLI::load - Fallout New Vegas EDID read error");
                if (isFNV)
                    fnvState = FnvLevelledItemState::EditorId;
                break;
            case ESM::fourCC("OBND"): // FO3/FONV
                if (isFNV)
                {
                    requireSize(subHdr, 12, mEditorId);
                    reader.skipSubRecordData();
                    fnvState = FnvLevelledItemState::Bounds;
                }
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("LVLD"):
                if (isFNV)
                {
                    requireSize(subHdr, 1, mEditorId);
                    readExact(reader, subHdr, mChanceNone, mEditorId);
                    fnvState = FnvLevelledItemState::ChanceNone;
                }
                else
                    readByteSubRecord(reader, subHdr, mChanceNone);
                mHasChanceNone = true;
                break;
            case ESM::fourCC("LVLF"):
                if (isFNV)
                {
                    requireSize(subHdr, 1, mEditorId);
                    readExact(reader, subHdr, mLvlItemFlags, mEditorId);
                    fnvState = FnvLevelledItemState::Flags;
                }
                else
                    readByteSubRecord(reader, subHdr, mLvlItemFlags);
                mHasLvlItemFlags = true;
                break;
            case ESM::fourCC("DATA"):
                readByteSubRecord(reader, subHdr, mData);
                break;
            case ESM::fourCC("LVLO"):
            {
                LVLO lvlo{};
                if (isFNV)
                {
                    requireSize(subHdr, sizeof(lvlo), mEditorId);
                    readExact(reader, subHdr, lvlo, mEditorId);
                }
                else if (subHdr.dataSize != sizeof(lvlo))
                {
                    if (subHdr.dataSize == 8)
                    {
                        reader.get(lvlo.level);
                        reader.get(lvlo.item);
                        reader.get(lvlo.count);
                    }
                    else
                        throw std::runtime_error("ESM4::LVLI::load - " + mEditorId + " LVLO size error");
                }
                else
                    reader.get(lvlo);

                reader.adjustFormId(lvlo.item);
                mLvlObject.push_back(lvlo);
                mLvlObjectExtra.emplace_back(std::nullopt);
                if (isFNV)
                    fnvState = FnvLevelledItemState::Entry;
                break;
            }
            case ESM::fourCC("LVLG"):
                if (isFNV)
                {
                    requireSize(subHdr, sizeof(ESM::FormId32), mEditorId);
                    if (!reader.getFormId(mChanceGlobal))
                        throw std::runtime_error("ESM4::LVLI::load - " + mEditorId + " truncated LVLG");
                    fnvState = FnvLevelledItemState::ChanceGlobal;
                }
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("COED"):
                if (isFNV)
                {
                    requireSize(subHdr, sizeof(LevelledItemExtraData), mEditorId);
                    if (mLvlObjectExtra.empty() || mLvlObjectExtra.back().has_value())
                        throw std::runtime_error("ESM4::LVLI::load - " + mEditorId + " orphan or duplicate COED");

                    LevelledItemExtraData extra{};
                    readExact(reader, subHdr, extra, mEditorId);
                    reader.adjustFormId(extra.mOwner);
                    mLvlObjectExtra.back() = extra;
                    fnvState = FnvLevelledItemState::ExtraData;
                }
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("LLCT"):
            case ESM::fourCC("LLKC"): // FO4
            case ESM::fourCC("LVLM"): // FO4
            case ESM::fourCC("LVSG"): // FO4
            case ESM::fourCC("ONAM"): // FO4
                reader.skipSubRecordData();
                break;
            default:
                if (reader.skipUnknownStarfieldSubRecordData("loadlvli"))
                    break;
                throw std::runtime_error("ESM4::LVLI::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }

    if (isFNV && !isCompleteFNVSequence(fnvState))
        throw std::runtime_error("ESM4::LVLI::load - " + mEditorId + " incomplete Fallout New Vegas record");

    // FIXME: testing
    // if (mHasLvlItemFlags && mChanceNone >= 90)
    // std::cout << "LVLI " << mEditorId << " chance none " << int(mChanceNone) << std::endl;
}

bool ESM4::LevelledItem::calcAllLvlLessThanPlayer() const
{
    if (mHasLvlItemFlags)
        return (mLvlItemFlags & 0x01) != 0;
    else
        return (mChanceNone & 0x80) != 0; // FIXME: 0x80 is just a guess
}

bool ESM4::LevelledItem::calcEachItemInCount() const
{
    if (mHasLvlItemFlags)
        return (mLvlItemFlags & 0x02) != 0;
    else
        return mData != 0;
}

std::int8_t ESM4::LevelledItem::chanceNone() const
{
    if (mHasLvlItemFlags)
        return mChanceNone;
    else
        return (mChanceNone & 0x7f); // FIXME: 0x80 is just a guess
}

bool ESM4::LevelledItem::useAll() const
{
    if (mHasLvlItemFlags)
        return (mLvlItemFlags & 0x04) != 0;
    else
        return false;
}

// void ESM4::LevelledItem::save(ESM4::Writer& writer) const
//{
// }

// void ESM4::LevelledItem::blank()
//{
// }
