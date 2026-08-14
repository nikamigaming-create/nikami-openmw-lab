#include "globals.hpp"

#include <stdexcept>

#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm4/loadglob.hpp>

#include "esmstore.hpp"

namespace MWWorld
{
    Globals::Collection::const_iterator Globals::find(std::string_view name) const
    {
        Collection::const_iterator iter = mVariables.find(name);

        if (iter == mVariables.end())
            throw std::runtime_error("unknown global variable: " + std::string{ name });

        return iter;
    }

    Globals::Collection::iterator Globals::find(std::string_view name)
    {
        Collection::iterator iter = mVariables.find(name);

        if (iter == mVariables.end())
            throw std::runtime_error("unknown global variable: " + std::string{ name });

        return iter;
    }

    void Globals::fill(const MWWorld::ESMStore& store)
    {
        mVariables.clear();

        const MWWorld::Store<ESM::Global>& globals = store.get<ESM::Global>();

        for (const ESM::Global& esmGlobal : globals)
        {
            mVariables.emplace(esmGlobal.mId, esmGlobal);
        }

        const auto insertFalloutGlobal = [this](std::string_view id, float value, std::uint32_t flags) {
            ESM::Global runtimeGlobal;
            runtimeGlobal.mRecordFlags = flags;
            runtimeGlobal.mId = ESM::RefId::stringRefId(id);
            runtimeGlobal.mValue.setType(ESM::VT_Float);
            runtimeGlobal.mValue.setFloat(value);
            mVariables.insert_or_assign(runtimeGlobal.mId, std::move(runtimeGlobal));
        };

        for (const ESM4::GlobalVariable& esm4Global : store.get<ESM4::GlobalVariable>())
        {
            if (esm4Global.mEditorId.empty())
                continue;
            insertFalloutGlobal(esm4Global.mEditorId, esm4Global.mValue, esm4Global.mFlags);

            // OpenMW's calendar names predate the TES4-family Game* names. Keep both IDs
            // backed by the same initial retail value so DateTimeManager uses the master data.
            if (Misc::StringUtils::ciEqual(esm4Global.mEditorId, "GameYear"))
                insertFalloutGlobal(sYear.getValue(), esm4Global.mValue, esm4Global.mFlags);
            else if (Misc::StringUtils::ciEqual(esm4Global.mEditorId, "GameMonth"))
                insertFalloutGlobal(sMonth.getValue(), esm4Global.mValue, esm4Global.mFlags);
            else if (Misc::StringUtils::ciEqual(esm4Global.mEditorId, "GameDay"))
                insertFalloutGlobal(sDay.getValue(), esm4Global.mValue, esm4Global.mFlags);
            else if (Misc::StringUtils::ciEqual(esm4Global.mEditorId, "GameDaysPassed"))
                insertFalloutGlobal(sDaysPassed.getValue(), esm4Global.mValue, esm4Global.mFlags);
        }
    }

    const ESM::Variant& Globals::operator[](GlobalVariableName name) const
    {
        return find(name.getValue())->second.mValue;
    }

    ESM::Variant& Globals::operator[](GlobalVariableName name)
    {
        return find(name.getValue())->second.mValue;
    }

    char Globals::getType(GlobalVariableName name) const
    {
        Collection::const_iterator iter = mVariables.find(name.getValue());

        if (iter == mVariables.end())
            return ' ';

        switch (iter->second.mValue.getType())
        {
            case ESM::VT_Short:
                return 's';
            case ESM::VT_Long:
                return 'l';
            case ESM::VT_Float:
                return 'f';

            default:
                return ' ';
        }
    }

    int Globals::countSavedGameRecords() const
    {
        return mVariables.size();
    }

    void Globals::write(ESM::ESMWriter& writer, Loading::Listener& progress) const
    {
        for (const auto& variable : mVariables)
        {
            writer.startRecord(ESM::REC_GLOB);
            variable.second.save(writer);
            writer.endRecord(ESM::REC_GLOB);
        }
    }

    bool Globals::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type == ESM::REC_GLOB)
        {
            ESM::Global global;
            bool isDeleted = false;

            // This readRecord() method is used when reading a saved game.
            // Deleted globals can't appear there, so isDeleted will be ignored here.
            global.load(reader, isDeleted);

            if (const auto iter = mVariables.find(global.mId); iter != mVariables.end())
                iter->second = std::move(global);

            return true;
        }

        return false;
    }
}
