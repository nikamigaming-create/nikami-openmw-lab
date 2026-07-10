#include "script.hpp"

#include "reader.hpp"

#include <bit>

#include <components/esm/defs.hpp>

namespace ESM4
{
    bool loadScriptSubRecord(Reader& reader, ScriptDefinition& script)
    {
        const SubRecordHeader& subRecord = reader.subRecordHeader();
        switch (subRecord.typeId)
        {
            case ESM::fourCC("SCHR"):
                if (subRecord.dataSize == sizeof(script.scriptHeader))
                    reader.get(script.scriptHeader);
                else
                    reader.skipSubRecordData();
                return true;
            case ESM::fourCC("SCDA"):
                script.compiledData.resize(subRecord.dataSize);
                if (!script.compiledData.empty())
                    reader.get(script.compiledData.data(), script.compiledData.size());
                return true;
            case ESM::fourCC("SCTX"):
                reader.getString(script.scriptSource);
                return true;
            case ESM::fourCC("SLSD"):
            {
                if (subRecord.dataSize != 24)
                {
                    reader.skipSubRecordData();
                    return true;
                }

                ScriptLocalVariableData localVariable{};
                reader.get(localVariable.index);
                reader.get(localVariable.unknown1);
                reader.get(localVariable.unknown2);
                reader.get(localVariable.unknown3);
                reader.get(localVariable.type);
                reader.get(localVariable.unknown4);
                script.localVarData.push_back(std::move(localVariable));
                return true;
            }
            case ESM::fourCC("SCVR"):
                if (!script.localVarData.empty())
                    reader.getZString(script.localVarData.back().variableName);
                else
                    reader.skipSubRecordData();
                return true;
            case ESM::fourCC("SCRV"):
            {
                if (subRecord.dataSize != sizeof(std::uint32_t))
                {
                    reader.skipSubRecordData();
                    return true;
                }

                std::uint32_t index = 0;
                reader.get(index);
                script.localRefVarIndex.push_back(index);
                return true;
            }
            case ESM::fourCC("SCRO"):
            {
                ESM::FormId reference;
                reader.getFormId(reference);
                script.globReference = reference;
                script.references.push_back(reference);
                return true;
            }
            default:
                return false;
        }
    }

    bool loadTargetCondition(Reader& reader, TargetCondition& condition, ESM::FormId* parameter3)
    {
        const SubRecordHeader& subRecord = reader.subRecordHeader();
        condition = {};
        if (parameter3 != nullptr)
            *parameter3 = {};

        bool loaded = false;
        switch (subRecord.dataSize)
        {
            case 20:
                reader.get(&condition, 20);
                loaded = true;
                break;
            case 24:
                reader.get(&condition, 24);
                loaded = true;
                break;
            case 28:
                reader.get(condition);
                if (condition.reference != 0)
                    reader.adjustFormId(condition.reference);
                loaded = true;
                break;
            case 36:
            {
                reader.get(&condition, 20);
                ESM::FormId ignoredParameter3;
                reader.getFormId(parameter3 != nullptr ? *parameter3 : ignoredParameter3);
                reader.get(condition.runOn);
                reader.get(condition.reference);
                if (condition.reference != 0)
                    reader.adjustFormId(condition.reference);
                reader.skipSubRecordData(4);
                loaded = true;
                break;
            }
            default:
                reader.skipSubRecordData();
                return false;
        }

        if ((condition.condition & CTF_UseGlobal) != 0)
        {
            ESM::FormId32 comparisonGlobal = std::bit_cast<ESM::FormId32>(condition.comparison);
            reader.adjustFormId(comparisonGlobal);
            condition.comparison = std::bit_cast<float>(comparisonGlobal);
        }

        switch (condition.functionIndex)
        {
            case FUN_GetQuestRunning:
            case FUN_GetStage:
            case FUN_GetStageDone:
            case FUN_GetGlobalValue:
            case FUN_GetQuestCompleted:
                reader.adjustFormId(condition.param1);
                break;
            default:
                break;
        }

        return loaded;
    }
}
