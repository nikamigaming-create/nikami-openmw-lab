/*
  Copyright (C) 2019-2021 cc9cii

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
#include "loadnote.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "reader.hpp"
// #include "writer.hpp"

namespace
{
    enum class Phase
    {
        Start,
        EditorId,
        Bounds,
        FullName,
        Model,
        BoundRadius,
        Icon,
        Data,
        Quests,
        Content,
        Speaker,
    };

    bool isFalloutNewVegas(const ESM4::Reader& reader)
    {
        const std::uint32_t version = reader.esmVersion();
        return version == ESM::VER_132 || version == ESM::VER_133 || version == ESM::VER_134;
    }

    [[noreturn]] void fail(std::string_view message)
    {
        throw std::runtime_error("ESM4::Note::load - " + std::string(message));
    }

    void requireSize(const ESM4::SubRecordHeader& header, std::uint32_t expected)
    {
        if (header.dataSize != expected)
        {
            fail("unsupported " + ESM::printName(header.typeId) + " size " + std::to_string(header.dataSize)
                + ", expected " + std::to_string(expected));
        }
    }

    template <class T>
    void readExact(ESM4::Reader& reader, T& value, std::string_view field)
    {
        if (!reader.getExact(value))
            fail("could not read " + std::string(field));
    }

    template <std::size_t Size>
    void readArray(ESM4::Reader& reader, std::array<std::uint8_t, Size>& value, std::string_view field)
    {
        if (!reader.get(value.data(), value.size()))
            fail("could not read " + std::string(field));
    }

    void readZString(ESM4::Reader& reader, std::string& value, std::string_view field)
    {
        if (reader.subRecordHeader().dataSize == 0)
            fail("zero-sized " + std::string(field));
        if (!reader.getZString(value))
            fail("could not read " + std::string(field));
    }

    ESM::FormId readFormId(ESM4::Reader& reader, std::string_view field)
    {
        ESM::FormId value;
        if (!reader.getFormId(value) || value.isZeroOrUnset())
            fail("could not read nonzero " + std::string(field) + " FormID");
        return value;
    }

    void loadLegacy(ESM4::Note& note, ESM4::Reader& reader)
    {
        note.mId = reader.getFormIdFromHeader();
        note.mFlags = reader.hdr().record.flags;

        while (reader.getSubRecordHeader())
        {
            const ESM4::SubRecordHeader& subHdr = reader.subRecordHeader();
            switch (subHdr.typeId)
            {
                case ESM::fourCC("EDID"):
                    reader.getZString(note.mEditorId);
                    break;
                case ESM::fourCC("FULL"):
                    reader.getLocalizedString(note.mFullName);
                    break;
                case ESM::fourCC("MODL"):
                    reader.getZString(note.mModel);
                    break;
                case ESM::fourCC("ICON"):
                    reader.getZString(note.mIcon);
                    break;
                case ESM::fourCC("MODB"):
                    reader.get(note.mBoundRadius);
                    break;
                case ESM::fourCC("YNAM"):
                    reader.getFormId(note.mPickUpSound);
                    break;
                case ESM::fourCC("ZNAM"):
                    reader.getFormId(note.mDropSound);
                    break;
                case ESM::fourCC("DATA"):
                case ESM::fourCC("MODT"): // Model data
                case ESM::fourCC("MODC"):
                case ESM::fourCC("MODS"):
                case ESM::fourCC("MODF"): // Model data end
                case ESM::fourCC("ONAM"):
                case ESM::fourCC("SNAM"):
                case ESM::fourCC("TNAM"):
                case ESM::fourCC("XNAM"):
                case ESM::fourCC("OBND"):
                case ESM::fourCC("VMAD"):
                case ESM::fourCC("DNAM"): // FO4
                case ESM::fourCC("PNAM"): // FO4
                case ESM::fourCC("PTRN"): // FO4
                    reader.skipSubRecordData();
                    break;
                default:
                    if (reader.skipUnknownStarfieldSubRecordData("loadnote"))
                        break;
                    throw std::runtime_error("ESM4::NOTE::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
            }
        }
    }
}

void ESM4::Note::load(ESM4::Reader& reader)
{
    if (!isFalloutNewVegas(reader))
    {
        loadLegacy(*this, reader);
        return;
    }

    // Frozen English Ultimate Edition corpus (10 official masters): 1,225
    // physical NOTE records and 1,225 winning/live records, with no deletes,
    // compression, or overrides (FNV 894, Dead Money 148, Honest Hearts 21,
    // Old World Blues 125, Lonesome Road 37). EDID/DATA occur once per
    // record; DATA is one byte with raw counts 0:2, 1:1166, 2:7, 3:50.
    // DATA=1 owns one Z-string TNAM, DATA=2 one Z-string XNAM, and DATA=3
    // one nonzero DIAL TNAM plus optional nonzero NPC_/CREA SNAM. ONAM is a
    // repeatable nonzero QUST FormID (115 occurrences in 106 records, max
    // four). OBND 1,219x12; FULL 1,053; MODL 440; ICON 529; MODB 1x4.
    // No other NOTE subrecord or size variant occurs. This slice preserves
    // bytes and resolved identities only; it does not display or play notes.
    Note value;
    value.mId = reader.getFormIdFromHeader();
    value.mFlags = reader.hdr().record.flags;
    if (value.mFlags != 0)
        fail("unsupported nonzero record flags " + std::to_string(value.mFlags));

    Phase phase = Phase::Start;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();
        switch (header.typeId)
        {
            case ESM::fourCC("EDID"):
                if (phase != Phase::Start)
                    fail("EDID is duplicated or out of order");
                readZString(reader, value.mEditorId, "EDID");
                phase = Phase::EditorId;
                break;
            case ESM::fourCC("OBND"):
                if (phase != Phase::EditorId)
                    fail("OBND is duplicated or out of order");
                requireSize(header, value.mObjectBounds.size());
                readArray(reader, value.mObjectBounds, "OBND");
                phase = Phase::Bounds;
                break;
            case ESM::fourCC("FULL"):
                if (phase != Phase::EditorId && phase != Phase::Bounds)
                    fail("FULL is duplicated or out of order");
                readZString(reader, value.mFullName, "FULL");
                phase = Phase::FullName;
                break;
            case ESM::fourCC("MODL"):
                if (phase != Phase::EditorId && phase != Phase::Bounds && phase != Phase::FullName)
                    fail("MODL is duplicated or out of order");
                readZString(reader, value.mModel, "MODL");
                phase = Phase::Model;
                break;
            case ESM::fourCC("MODB"):
                if (phase != Phase::Model)
                    fail("MODB appears without MODL or is out of order");
                requireSize(header, sizeof(value.mBoundRadius));
                readExact(reader, value.mBoundRadius, "MODB");
                phase = Phase::BoundRadius;
                break;
            case ESM::fourCC("ICON"):
                if (phase != Phase::EditorId && phase != Phase::Bounds && phase != Phase::FullName
                    && phase != Phase::Model)
                    fail("ICON is duplicated or out of order");
                readZString(reader, value.mIcon, "ICON");
                phase = Phase::Icon;
                break;
            case ESM::fourCC("DATA"):
                if (phase != Phase::EditorId && phase != Phase::Bounds && phase != Phase::FullName
                    && phase != Phase::Model && phase != Phase::BoundRadius && phase != Phase::Icon)
                    fail("DATA is missing, duplicated, or out of order");
                requireSize(header, 1);
                readExact(reader, value.mData, "DATA");
                if (value.mData > 3)
                    fail("unsupported DATA value " + std::to_string(value.mData));
                phase = Phase::Data;
                break;
            case ESM::fourCC("ONAM"):
                if (phase != Phase::Data && phase != Phase::Quests)
                    fail("ONAM appears before DATA, after content, or out of order");
                requireSize(header, 4);
                value.mQuests.push_back(readFormId(reader, "ONAM"));
                if (value.mQuests.size() > 4)
                    fail("more than four ONAM occurrences");
                phase = Phase::Quests;
                break;
            case ESM::fourCC("TNAM"):
                if (phase != Phase::Data && phase != Phase::Quests)
                    fail("TNAM is duplicated or out of order");
                if (value.mData == 1)
                    readZString(reader, value.mText, "TNAM");
                else if (value.mData == 3)
                {
                    requireSize(header, 4);
                    value.mVoiceTopic = readFormId(reader, "TNAM");
                }
                else
                    fail("TNAM is not authored for this DATA value");
                phase = Phase::Content;
                break;
            case ESM::fourCC("SNAM"):
                if (phase != Phase::Content || value.mData != 3 || value.mVoiceTopic.isZeroOrUnset())
                    fail("SNAM appears without a DATA=3 TNAM or is out of order");
                requireSize(header, 4);
                value.mVoiceSpeaker = readFormId(reader, "SNAM");
                phase = Phase::Speaker;
                break;
            case ESM::fourCC("XNAM"):
                if ((phase != Phase::Data && phase != Phase::Quests) || value.mData != 2)
                    fail("XNAM is not a DATA=2 content field or is out of order");
                readZString(reader, value.mImage, "XNAM");
                phase = Phase::Content;
                break;
            default:
                fail("unknown or out-of-order Fallout New Vegas subrecord " + ESM::printName(header.typeId));
        }
    }

    const bool complete = (value.mData == 0 && phase == Phase::Data) || (value.mData == 1 && phase == Phase::Content)
        || (value.mData == 2 && phase == Phase::Content)
        || (value.mData == 3 && (phase == Phase::Content || phase == Phase::Speaker)
            && !value.mVoiceTopic.isZeroOrUnset());
    if (!complete)
        fail("record is incomplete or its DATA/content shape is unsupported");

    *this = std::move(value);
}

// void ESM4::Note::save(ESM4::Writer& writer) const
//{
// }

// void ESM4::Note::blank()
//{
// }
