#include "script.hpp"

#include "reader.hpp"

#include <bit>
#include <cstddef>
#include <optional>
#include <span>

#include <components/esm/defs.hpp>

namespace
{
    struct RawScriptBytecodeInstruction
    {
        std::uint16_t opcode = 0;
        std::optional<std::uint16_t> callingReferenceIndex;
        std::size_t argumentOffset = 0;
        std::size_t argumentSize = 0;
        std::size_t nextOffset = 0;
    };

    std::uint16_t readLittleEndianUint16(std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        return static_cast<std::uint16_t>(bytes[offset])
            | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
    }

    ESM4::ScriptBytecodeDecodeError decodeInstruction(
        std::span<const std::uint8_t> bytecode, std::size_t offset, RawScriptBytecodeInstruction& instruction)
    {
        const std::size_t remaining = bytecode.size() - offset;
        if (remaining < 4)
            return ESM4::ScriptBytecodeDecodeError::TruncatedInstructionHeader;

        const std::uint16_t outerOpcode = readLittleEndianUint16(bytecode, offset);
        std::size_t headerSize = 4;
        std::size_t lengthOffset = offset + 2;
        instruction.callingReferenceIndex.reset();
        instruction.opcode = outerOpcode;

        if (outerOpcode == 0x001c)
        {
            if (remaining < 8)
                return ESM4::ScriptBytecodeDecodeError::TruncatedReferenceInstructionHeader;
            instruction.callingReferenceIndex = readLittleEndianUint16(bytecode, offset + 2);
            instruction.opcode = readLittleEndianUint16(bytecode, offset + 4);
            lengthOffset = offset + 6;
            headerSize = 8;
        }

        const std::size_t argumentSize = readLittleEndianUint16(bytecode, lengthOffset);
        const std::size_t argumentOffset = offset + headerSize;
        if (argumentSize > bytecode.size() - argumentOffset)
            return ESM4::ScriptBytecodeDecodeError::ArgumentPayloadOverrun;

        instruction.argumentOffset = argumentOffset;
        instruction.argumentSize = argumentSize;
        instruction.nextOffset = argumentOffset + argumentSize;
        return ESM4::ScriptBytecodeDecodeError::None;
    }
}

namespace ESM4
{
    ScriptBytecodeDecodeResult decodeFalloutScriptBytecode(
        std::span<const std::uint8_t> bytecode, std::vector<ScriptBytecodeInstruction>& instructions)
    {
        instructions.clear();

        std::size_t offset = 0;
        std::size_t instructionCount = 0;
        while (offset < bytecode.size())
        {
            RawScriptBytecodeInstruction instruction;
            const ScriptBytecodeDecodeError error = decodeInstruction(bytecode, offset, instruction);
            if (error != ScriptBytecodeDecodeError::None)
                return { error, offset, instructionCount };
            offset = instruction.nextOffset;
            ++instructionCount;
        }

        // The first pass proves the entire byte stream is framed and bounds the only allocation
        // to exactly one view per instruction. It also prevents callers from observing a valid
        // prefix when malformed trailing bytes are present.
        instructions.reserve(instructionCount);
        offset = 0;
        while (offset < bytecode.size())
        {
            RawScriptBytecodeInstruction instruction;
            const ScriptBytecodeDecodeError error = decodeInstruction(bytecode, offset, instruction);
            if (error != ScriptBytecodeDecodeError::None)
            {
                const std::size_t decodedInstructions = instructions.size();
                instructions.clear();
                return { error, offset, decodedInstructions };
            }
            instructions.push_back({ offset, instruction.opcode, instruction.callingReferenceIndex,
                bytecode.subspan(instruction.argumentOffset, instruction.argumentSize) });
            offset = instruction.nextOffset;
        }

        return { ScriptBytecodeDecodeError::None, bytecode.size(), instructionCount };
    }

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
            case FUN_GetDistance:
            case FUN_GetItemCount:
            case FUN_GetDeadCount:
            case FUN_GetInCell:
            case FUN_GetIsClass:
            case FUN_GetIsRace:
            case FUN_GetIsVoiceType:
            case FUN_GetInFaction:
            case FUN_GetIsID:
            case FUN_GetEquipped:
            case FUN_GetIsCurrentPackage:
            case FUN_GetIsReference:
            case FUN_GetInWorldspace:
            case FUN_GetHasNote:
            case FUN_HasPerk:
            case FUN_GetGlobalValue:
            case FUN_GetQuestVariable:
            case FUN_GetQuestRunning:
            case FUN_GetStage:
            case FUN_GetStageDone:
            case FUN_GetQuestCompleted:
            case FUN_GetObjectiveCompleted:
            case FUN_GetObjectiveDisplayed:
            case FUN_IsInList:
                reader.adjustFormId(condition.param1);
                break;
            default:
                break;
        }

        return loaded;
    }
}
