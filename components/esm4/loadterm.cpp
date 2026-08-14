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
#include "loadterm.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "reader.hpp"
// #include "writer.hpp"

namespace
{
    enum class TopPhase
    {
        Start,
        EditorId,
        Bounds,
        FullName,
        Model,
        ModelData,
        ModelTextureSwaps,
        Script,
        Description,
        Sound,
        Password,
        Data,
    };

    enum class MenuPhase
    {
        ResultText,
        Flags,
        LinkOrScript,
        Script,
        AfterHeader,
        NeedSource,
        ScriptBody,
        NeedLocalName,
        LocalReferences,
        FormReferences,
        Conditions,
    };

    bool isFalloutNewVegas(const ESM4::Reader& reader)
    {
        const std::uint32_t version = reader.esmVersion();
        return version == ESM::VER_132 || version == ESM::VER_133 || version == ESM::VER_134;
    }

    [[noreturn]] void fail(std::string_view message)
    {
        throw std::runtime_error("ESM4::Terminal::load - " + std::string(message));
    }

    void requireSize(const ESM4::SubRecordHeader& header, std::uint32_t expected)
    {
        if (header.dataSize != expected)
        {
            fail("unsupported " + ESM::printName(header.typeId) + " size " + std::to_string(header.dataSize)
                + ", expected " + std::to_string(expected));
        }
    }

    void requireOneOfSizes(
        const ESM4::SubRecordHeader& header, std::initializer_list<std::uint32_t> expected)
    {
        if (std::find(expected.begin(), expected.end(), header.dataSize) == expected.end())
            fail("unsupported " + ESM::printName(header.typeId) + " size " + std::to_string(header.dataSize));
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

    void readBytes(ESM4::Reader& reader, std::vector<std::uint8_t>& value, std::string_view field)
    {
        value.resize(reader.subRecordHeader().dataSize);
        if (!value.empty() && !reader.get(value.data(), value.size()))
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

    ESM4::TargetCondition readCondition(ESM4::Reader& reader)
    {
        requireSize(reader.subRecordHeader(), 28);
        ESM4::TargetCondition condition;
        if (!ESM4::loadTargetCondition(reader, condition))
            fail("could not read CTDA");

        // loadTargetCondition handles the shared TES4-family FormID-bearing
        // functions. These two additional functions carry FormIDs in every
        // occurrence in the frozen FNV TERM corpus.
        if (condition.functionIndex == ESM4::FUN_GetScriptVariable
            || condition.functionIndex == ESM4::FUN_GetFactionRank)
            reader.adjustFormId(condition.param1);
        return condition;
    }

    bool canCloseMenu(const ESM4::Terminal::MenuItem& item, MenuPhase phase)
    {
        if (phase == MenuPhase::AfterHeader)
            return item.mScript.scriptHeader.compiledSize == 0;
        return phase == MenuPhase::ScriptBody || phase == MenuPhase::LocalReferences
            || phase == MenuPhase::FormReferences || phase == MenuPhase::Conditions;
    }

    void loadLegacy(ESM4::Terminal& terminal, ESM4::Reader& reader)
    {
        terminal.mId = reader.getFormIdFromHeader();
        terminal.mFlags = reader.hdr().record.flags;

        while (reader.getSubRecordHeader())
        {
            const ESM4::SubRecordHeader& subHdr = reader.subRecordHeader();
            switch (subHdr.typeId)
            {
                case ESM::fourCC("EDID"):
                    reader.getZString(terminal.mEditorId);
                    break;
                case ESM::fourCC("FULL"):
                    reader.getLocalizedString(terminal.mFullName);
                    break;
                case ESM::fourCC("DESC"):
                    reader.getLocalizedString(terminal.mText);
                    break;
                case ESM::fourCC("SCRI"):
                    reader.getFormId(terminal.mScriptId);
                    break;
                case ESM::fourCC("PNAM"):
                    reader.getFormId(terminal.mPasswordNote);
                    break;
                case ESM::fourCC("SNAM"):
                    if (subHdr.dataSize == 4)
                        reader.getFormId(terminal.mSound);
                    // FIXME: FO4 sound marker params
                    else
                        reader.skipSubRecordData();
                    break;
                case ESM::fourCC("MODL"):
                    reader.getZString(terminal.mModel);
                    break;
                case ESM::fourCC("RNAM"):
                    reader.getZString(terminal.mResultText);
                    break;
                case ESM::fourCC("DNAM"): // difficulty
                case ESM::fourCC("ANAM"): // flags
                case ESM::fourCC("CTDA"):
                case ESM::fourCC("CIS1"):
                case ESM::fourCC("CIS2"):
                case ESM::fourCC("INAM"):
                case ESM::fourCC("ITXT"): // Menu Item
                case ESM::fourCC("MODT"): // Model data
                case ESM::fourCC("MODC"):
                case ESM::fourCC("MODS"):
                case ESM::fourCC("MODF"): // Model data end
                case ESM::fourCC("SCDA"):
                case ESM::fourCC("SCHR"):
                case ESM::fourCC("SCRO"):
                case ESM::fourCC("SCRV"):
                case ESM::fourCC("SCTX"):
                case ESM::fourCC("SCVR"):
                case ESM::fourCC("SLSD"):
                case ESM::fourCC("TNAM"):
                case ESM::fourCC("OBND"):
                case ESM::fourCC("VMAD"):
                case ESM::fourCC("KSIZ"):
                case ESM::fourCC("KWDA"):
                case ESM::fourCC("BSIZ"): // FO4
                case ESM::fourCC("BTXT"): // FO4
                case ESM::fourCC("COCT"): // FO4
                case ESM::fourCC("CNTO"): // FO4
                case ESM::fourCC("FNAM"): // FO4
                case ESM::fourCC("ISIZ"): // FO4
                case ESM::fourCC("ITID"): // FO4
                case ESM::fourCC("MNAM"): // FO4
                case ESM::fourCC("NAM0"): // FO4
                case ESM::fourCC("PRPS"): // FO4
                case ESM::fourCC("PTRN"): // FO4
                case ESM::fourCC("UNAM"): // FO4
                case ESM::fourCC("VNAM"): // FO4
                case ESM::fourCC("WBDT"): // FO4
                case ESM::fourCC("WNAM"): // FO4
                case ESM::fourCC("XMRK"): // FO4
                    reader.skipSubRecordData();
                    break;
                default:
                    if (reader.skipUnknownStarfieldSubRecordData("loadterm"))
                        break;
                    throw std::runtime_error(
                        "ESM4::TERM::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
            }
        }
    }
}

void ESM4::Terminal::load(ESM4::Reader& reader)
{
    if (!isFalloutNewVegas(reader))
    {
        loadLegacy(*this, reader);
        return;
    }

    // Frozen English Ultimate Edition corpus (10 official masters): 515
    // physical TERM records, all winning/live (FNV 344, DM 95, HH 9, OWB 47,
    // LR 20); 1,350 menu items. Every record has EDID/OBND/DESC/DNAM;
    // DNAM is 513x4 + 2x3. Every menu item has ITXT/RNAM/ANAM/SCHR;
    // INAM 772x4; TNAM 108x4; CTDA 802x28; SCHR 1,350x20;
    // SCDA 384; SCTX 407; SLSD/SCVR 89 each; SCRV 87; SCRO 1,477x4.
    // No other TERM subrecord or fixed-size variant occurs. This is a
    // lossless parser slice only; it does not render menus, hack terminals,
    // evaluate conditions, execute scripts, or persist terminal state.
    Terminal value;
    value.mId = reader.getFormIdFromHeader();
    value.mFlags = reader.hdr().record.flags;

    TopPhase topPhase = TopPhase::Start;
    MenuPhase menuPhase = MenuPhase::ResultText;
    MenuItem* menu = nullptr;

    const auto closeMenu = [&]() {
        if (menu == nullptr || !canCloseMenu(*menu, menuPhase))
            fail("menu item is incomplete");
        menu = nullptr;
    };

    const auto startMenu = [&]() {
        if (topPhase != TopPhase::Data)
            fail("ITXT appears before DNAM");
        value.mMenuItems.emplace_back();
        menu = &value.mMenuItems.back();
        readZString(reader, menu->mText, "ITXT");
        menuPhase = MenuPhase::ResultText;
    };

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();

        if (menu != nullptr && header.typeId == ESM::fourCC("ITXT"))
        {
            closeMenu();
            startMenu();
            continue;
        }

        if (menu == nullptr)
        {
            switch (header.typeId)
            {
                case ESM::fourCC("EDID"):
                    if (topPhase != TopPhase::Start)
                        fail("EDID is missing, duplicated, or out of order");
                    readZString(reader, value.mEditorId, "EDID");
                    topPhase = TopPhase::EditorId;
                    break;
                case ESM::fourCC("OBND"):
                    if (topPhase != TopPhase::EditorId)
                        fail("OBND is missing, duplicated, or out of order");
                    requireSize(header, value.mObjectBounds.size());
                    readArray(reader, value.mObjectBounds, "OBND");
                    topPhase = TopPhase::Bounds;
                    break;
                case ESM::fourCC("FULL"):
                    if (topPhase != TopPhase::Bounds)
                        fail("FULL is duplicated or out of order");
                    readZString(reader, value.mFullName, "FULL");
                    topPhase = TopPhase::FullName;
                    break;
                case ESM::fourCC("MODL"):
                    if (topPhase != TopPhase::Bounds && topPhase != TopPhase::FullName)
                        fail("MODL is duplicated or out of order");
                    readZString(reader, value.mModel, "MODL");
                    topPhase = TopPhase::Model;
                    break;
                case ESM::fourCC("MODT"):
                    if (topPhase != TopPhase::Model)
                        fail("MODT appears without MODL or is out of order");
                    requireOneOfSizes(header, { 96, 120, 144, 168 });
                    readBytes(reader, value.mModelData, "MODT");
                    topPhase = TopPhase::ModelData;
                    break;
                case ESM::fourCC("MODS"):
                    if (topPhase != TopPhase::Model)
                        fail("MODS appears without MODL or is out of order");
                    requireOneOfSizes(header, { 36, 37, 137 });
                    readBytes(reader, value.mModelTextureSwaps, "MODS");
                    topPhase = TopPhase::ModelTextureSwaps;
                    break;
                case ESM::fourCC("SCRI"):
                    if (topPhase != TopPhase::Model && topPhase != TopPhase::ModelData)
                        fail("SCRI is duplicated or out of order");
                    requireSize(header, 4);
                    value.mScriptId = readFormId(reader, "SCRI");
                    topPhase = TopPhase::Script;
                    break;
                case ESM::fourCC("DESC"):
                    if (topPhase != TopPhase::Bounds && topPhase != TopPhase::FullName
                        && topPhase != TopPhase::Model && topPhase != TopPhase::ModelData
                        && topPhase != TopPhase::ModelTextureSwaps && topPhase != TopPhase::Script)
                        fail("DESC is missing, duplicated, or out of order");
                    readZString(reader, value.mText, "DESC");
                    topPhase = TopPhase::Description;
                    break;
                case ESM::fourCC("SNAM"):
                    if (topPhase != TopPhase::Description)
                        fail("SNAM is duplicated or out of order");
                    requireSize(header, 4);
                    value.mSound = readFormId(reader, "SNAM");
                    topPhase = TopPhase::Sound;
                    break;
                case ESM::fourCC("PNAM"):
                    if (topPhase != TopPhase::Description && topPhase != TopPhase::Sound)
                        fail("PNAM is duplicated or out of order");
                    requireSize(header, 4);
                    value.mPasswordNote = readFormId(reader, "PNAM");
                    topPhase = TopPhase::Password;
                    break;
                case ESM::fourCC("DNAM"):
                    if (topPhase != TopPhase::Description && topPhase != TopPhase::Sound
                        && topPhase != TopPhase::Password)
                        fail("DNAM is missing, duplicated, or out of order");
                    requireOneOfSizes(header, { 3, 4 });
                    value.mData.mSerializedSize = static_cast<std::uint8_t>(header.dataSize);
                    if (!reader.get(value.mData.mBytes.data(), header.dataSize))
                        fail("could not read DNAM");
                    topPhase = TopPhase::Data;
                    break;
                case ESM::fourCC("ITXT"):
                    startMenu();
                    break;
                default:
                    fail("unknown or out-of-order Fallout New Vegas top-level subrecord "
                        + ESM::printName(header.typeId));
            }
            continue;
        }

        switch (header.typeId)
        {
            case ESM::fourCC("RNAM"):
                if (menuPhase != MenuPhase::ResultText)
                    fail("RNAM is missing, duplicated, or out of order");
                readZString(reader, menu->mResultText, "RNAM");
                value.mResultText = menu->mResultText;
                menuPhase = MenuPhase::Flags;
                break;
            case ESM::fourCC("ANAM"):
                if (menuPhase != MenuPhase::Flags)
                    fail("ANAM is missing, duplicated, or out of order");
                requireSize(header, 1);
                readExact(reader, menu->mFlags, "ANAM");
                if (menu->mFlags > 3)
                    fail("unsupported ANAM flags " + std::to_string(menu->mFlags));
                menuPhase = MenuPhase::LinkOrScript;
                break;
            case ESM::fourCC("INAM"):
                if (menuPhase != MenuPhase::LinkOrScript)
                    fail("INAM is duplicated, conflicts with TNAM, or is out of order");
                requireSize(header, 4);
                menu->mDisplayNote = readFormId(reader, "INAM");
                menuPhase = MenuPhase::Script;
                break;
            case ESM::fourCC("TNAM"):
                if (menuPhase != MenuPhase::LinkOrScript)
                    fail("TNAM is duplicated, conflicts with INAM, or is out of order");
                requireSize(header, 4);
                menu->mSubmenu = readFormId(reader, "TNAM");
                menuPhase = MenuPhase::Script;
                break;
            case ESM::fourCC("SCHR"):
                if (menuPhase != MenuPhase::LinkOrScript && menuPhase != MenuPhase::Script)
                    fail("SCHR is missing, duplicated, or out of order");
                requireSize(header, sizeof(ScriptHeader));
                readExact(reader, menu->mScript.scriptHeader, "SCHR");
                if (menu->mScript.scriptHeader.unused != 0)
                    fail("unsupported nonzero SCHR unused field");
                if (menu->mScript.scriptHeader.type > 1)
                    fail("unsupported SCHR type " + std::to_string(menu->mScript.scriptHeader.type));
                if (menu->mScript.scriptHeader.flag > 1)
                    fail("unsupported SCHR flags " + std::to_string(menu->mScript.scriptHeader.flag));
                menuPhase = MenuPhase::AfterHeader;
                break;
            case ESM::fourCC("SCDA"):
                if (menuPhase != MenuPhase::AfterHeader || menu->mScript.scriptHeader.compiledSize == 0)
                    fail("SCDA is out of order or contradicts SCHR");
                if (header.dataSize != menu->mScript.scriptHeader.compiledSize)
                    fail("SCDA size does not match SCHR compiled size");
                readBytes(reader, menu->mScript.compiledData, "SCDA");
                menuPhase = MenuPhase::NeedSource;
                break;
            case ESM::fourCC("SCTX"):
                if (menuPhase != MenuPhase::AfterHeader && menuPhase != MenuPhase::NeedSource)
                    fail("SCTX is duplicated or out of order");
                if (menuPhase == MenuPhase::AfterHeader && menu->mScript.scriptHeader.compiledSize != 0)
                    fail("SCTX appears before required SCDA");
                if (header.dataSize == 0 || !reader.getString(menu->mScript.scriptSource))
                    fail("could not read nonempty SCTX");
                menuPhase = MenuPhase::ScriptBody;
                break;
            case ESM::fourCC("SLSD"):
            {
                if (menuPhase != MenuPhase::ScriptBody)
                    fail("SLSD is out of order");
                requireSize(header, 24);
                ScriptLocalVariableData local;
                readExact(reader, local.index, "SLSD index");
                readExact(reader, local.unknown1, "SLSD unknown 1");
                readExact(reader, local.unknown2, "SLSD unknown 2");
                readExact(reader, local.unknown3, "SLSD unknown 3");
                readExact(reader, local.type, "SLSD type");
                readExact(reader, local.unknown4, "SLSD unknown 4");
                menu->mScript.localVarData.push_back(std::move(local));
                menuPhase = MenuPhase::NeedLocalName;
                break;
            }
            case ESM::fourCC("SCVR"):
                if (menuPhase != MenuPhase::NeedLocalName || menu->mScript.localVarData.empty())
                    fail("SCVR appears without its SLSD");
                readZString(reader, menu->mScript.localVarData.back().variableName, "SCVR");
                menuPhase = MenuPhase::ScriptBody;
                break;
            case ESM::fourCC("SCRV"):
            {
                if (menuPhase != MenuPhase::ScriptBody && menuPhase != MenuPhase::LocalReferences)
                    fail("SCRV appears outside the local-reference section");
                if (menu->mScript.localVarData.empty())
                    fail("SCRV appears without local-variable metadata");
                requireSize(header, 4);
                std::uint32_t index = 0;
                readExact(reader, index, "SCRV");
                menu->mScript.localRefVarIndex.push_back(index);
                menuPhase = MenuPhase::LocalReferences;
                break;
            }
            case ESM::fourCC("SCRO"):
            {
                if (menuPhase != MenuPhase::ScriptBody && menuPhase != MenuPhase::LocalReferences
                    && menuPhase != MenuPhase::FormReferences)
                    fail("SCRO appears outside the FormID-reference section");
                requireSize(header, 4);
                const ESM::FormId reference = readFormId(reader, "SCRO");
                menu->mScript.globReference = reference;
                menu->mScript.references.push_back(reference);
                menuPhase = MenuPhase::FormReferences;
                break;
            }
            case ESM::fourCC("CTDA"):
                if (!canCloseMenu(*menu, menuPhase))
                    fail("CTDA appears before a complete menu script");
                menu->mConditions.push_back(readCondition(reader));
                menuPhase = MenuPhase::Conditions;
                break;
            default:
                fail("unknown or out-of-order Fallout New Vegas menu subrecord " + ESM::printName(header.typeId));
        }
    }

    if (menu != nullptr)
        closeMenu();
    if (topPhase != TopPhase::Data)
        fail("record is missing required EDID, OBND, DESC, or DNAM");

    *this = std::move(value);
}

// void ESM4::Terminal::save(ESM4::Writer& writer) const
//{
// }

// void ESM4::Terminal::blank()
//{
// }
