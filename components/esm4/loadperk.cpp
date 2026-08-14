#include "loadperk.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "reader.hpp"

namespace
{
    enum class EntryPhase
    {
        None,
        QuestData,
        AbilityDataOrEnd,
        EntryPointData,
        EntryPointConditionsOrFunction,
        EntryPointCondition,
        FunctionData,
        ScriptLabel,
        ScriptFlags,
        ScriptHeader,
        ScriptBytecode,
        ScriptSource,
        ScriptReferences,
        End,
    };

    [[noreturn]] void fail(std::string_view message)
    {
        throw std::runtime_error("ESM4::Perk::load - " + std::string(message));
    }

    void requireSize(const ESM4::SubRecordHeader& header, std::uint32_t expected)
    {
        if (header.dataSize != expected)
        {
            fail("unsupported " + ESM::printName(header.typeId) + " size " + std::to_string(header.dataSize)
                + ", expected " + std::to_string(expected));
        }
    }

    void requireZString(const ESM4::SubRecordHeader& header)
    {
        if (header.dataSize == 0)
            fail("zero-sized " + ESM::printName(header.typeId));
    }

    template <class T>
    void readExact(ESM4::Reader& reader, T& value, std::string_view field)
    {
        if (!reader.getExact(value))
            fail("could not read " + std::string(field));
    }

    void readZString(ESM4::Reader& reader, std::string& value, std::string_view field)
    {
        requireZString(reader.subRecordHeader());
        if (!reader.getZString(value))
            fail("could not read " + std::string(field));
    }

    bool isAdditionalProvenFormIdCondition(std::uint32_t function)
    {
        // loadTargetCondition already adjusts the common TES4-family FormID
        // functions. These four additional functions are FormID-bearing in the
        // frozen FNV PERK corpus and are not covered by that shared switch.
        switch (function)
        {
            case ESM4::FUN_GetDetected:
            case ESM4::FUN_HasMagicEffect:
            case ESM4::FUN_IsSpellTarget:
            case ESM4::FUN_IsWeaponInList:
                return true;
            default:
                return false;
        }
    }

    ESM4::TargetCondition readCondition(ESM4::Reader& reader)
    {
        requireSize(reader.subRecordHeader(), 28);
        ESM4::TargetCondition condition;
        if (!ESM4::loadTargetCondition(reader, condition))
            fail("could not read CTDA");
        if (isAdditionalProvenFormIdCondition(condition.functionIndex))
            reader.adjustFormId(condition.param1);
        return condition;
    }

    void validateScript(const ESM4::Perk::EntryPointEntry& entry)
    {
        const ESM4::ScriptDefinition& script = entry.mScript;
        if (entry.mScriptFlags != 0)
            fail("unsupported nonzero EPF3 flags");
        if (script.scriptHeader.unused != 0)
            fail("unsupported nonzero SCHR unused field");
        if (script.scriptHeader.variableCount != 0)
            fail("unsupported embedded-script local variables");
        if (script.scriptHeader.type != 0)
            fail("unsupported embedded-script SCHR type");
        if (script.scriptHeader.flag != 1)
            fail("unsupported embedded-script SCHR flags");
        if (script.scriptHeader.compiledSize != script.compiledData.size())
            fail("SCHR compiled size does not match SCDA");
        if (script.scriptHeader.refCount != script.references.size())
            fail("SCHR reference count does not match SCRO records");
        if (script.references.empty())
            fail("embedded script has no SCRO references");
    }
}

void ESM4::Perk::load(Reader& reader)
{
    // Frozen English Ultimate Edition winning-live corpus (10 official masters):
    // 257 PERK records; EDID 257; FULL 252; DESC 257; ICON 246;
    // record CTDA 148x28; record DATA 11x4 + 246x5; 369 entries;
    // PRKE 369x3; entry DATA 261x3 + 72x4 + 34x8 (2 absent);
    // PRKC 196x1; entry CTDA 321x28; EPFT 261x1; EPFD 254x4;
    // EPF2/EPF3/SCHR/SCDA/SCTX 7 each; SCRO 10x4; PRKF 369x0.
    // No other subrecord or fixed-size variant occurs. This loader preserves
    // records only; it does not evaluate conditions or execute perk effects.
    Perk value;
    value.mId = reader.getFormIdFromHeader();
    value.mFlags = reader.hdr().record.flags;

    bool hasEditorId = false;
    bool hasFullName = false;
    bool hasDescription = false;
    bool hasIcon = false;
    bool hasData = false;
    Entry* currentEntry = nullptr;
    ConditionGroup* currentConditionGroup = nullptr;
    EntryPhase phase = EntryPhase::None;

    const auto closeEntry = [&]() {
        if (currentEntry == nullptr)
            fail("PRKF appears without PRKE");
        if (currentConditionGroup != nullptr && currentConditionGroup->mConditions.empty())
            fail("PRKC condition group is empty");
        currentEntry = nullptr;
        currentConditionGroup = nullptr;
        phase = EntryPhase::None;
    };

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();

        if (currentEntry == nullptr)
        {
            switch (header.typeId)
            {
                case ESM::fourCC("EDID"):
                    if (hasEditorId || hasFullName || hasDescription || hasData)
                        fail("EDID is duplicated or out of order");
                    readZString(reader, value.mEditorId, "EDID");
                    hasEditorId = true;
                    break;
                case ESM::fourCC("FULL"):
                    if (!hasEditorId || hasFullName || hasDescription || hasData)
                        fail("FULL is duplicated or out of order");
                    readZString(reader, value.mFullName, "FULL");
                    hasFullName = true;
                    break;
                case ESM::fourCC("DESC"):
                    if (!hasEditorId || hasDescription || hasData)
                        fail("DESC is missing, duplicated, or out of order");
                    readZString(reader, value.mDescription, "DESC");
                    hasDescription = true;
                    break;
                case ESM::fourCC("ICON"):
                    if (!hasDescription || hasIcon || hasData || !value.mConditions.empty())
                        fail("ICON is duplicated or out of order");
                    readZString(reader, value.mIcon, "ICON");
                    hasIcon = true;
                    break;
                case ESM::fourCC("CTDA"):
                    if (!hasDescription || hasData)
                        fail("record CTDA is out of order");
                    value.mConditions.push_back(readCondition(reader));
                    break;
                case ESM::fourCC("DATA"):
                    if (!hasDescription || hasData)
                        fail("record DATA is missing, duplicated, or out of order");
                    if (header.dataSize != 4 && header.dataSize != 5)
                        fail("unsupported record DATA size " + std::to_string(header.dataSize) + ", expected 4 or 5");
                    readExact(reader, value.mData.mTrait, "DATA trait");
                    readExact(reader, value.mData.mMinimumLevel, "DATA minimum level");
                    readExact(reader, value.mData.mRankCount, "DATA rank count");
                    readExact(reader, value.mData.mPlayable, "DATA playable flag");
                    if (header.dataSize == 5)
                    {
                        std::uint8_t hidden = 0;
                        readExact(reader, hidden, "DATA hidden flag");
                        value.mData.mHidden = hidden;
                    }
                    value.mData.mSerializedSize = static_cast<std::uint8_t>(header.dataSize);
                    hasData = true;
                    break;
                case ESM::fourCC("PRKE"):
                {
                    if (!hasData)
                        fail("PRKE appears before record DATA");
                    requireSize(header, 3);
                    std::uint8_t rawType = 0;
                    Entry entry;
                    readExact(reader, rawType, "PRKE type");
                    readExact(reader, entry.mRank, "PRKE rank");
                    readExact(reader, entry.mPriority, "PRKE priority");
                    if (rawType > static_cast<std::uint8_t>(EntryType::EntryPoint))
                        fail("unsupported PRKE entry type " + std::to_string(rawType));
                    entry.mType = static_cast<EntryType>(rawType);
                    switch (entry.mType)
                    {
                        case EntryType::Quest:
                            entry.mData = QuestEntry{};
                            phase = EntryPhase::QuestData;
                            break;
                        case EntryType::Ability:
                            entry.mData = AbilityEntry{};
                            phase = EntryPhase::AbilityDataOrEnd;
                            break;
                        case EntryType::EntryPoint:
                            entry.mData = EntryPointEntry{};
                            phase = EntryPhase::EntryPointData;
                            break;
                    }
                    value.mEntries.push_back(std::move(entry));
                    currentEntry = &value.mEntries.back();
                    break;
                }
                default:
                    fail("unknown or out-of-order Fallout New Vegas subrecord " + ESM::printName(header.typeId));
            }
            continue;
        }

        switch (header.typeId)
        {
            case ESM::fourCC("DATA"):
                if (currentEntry->mType == EntryType::Quest && phase == EntryPhase::QuestData)
                {
                    requireSize(header, 8);
                    QuestEntry& entry = std::get<QuestEntry>(currentEntry->mData);
                    if (!reader.getFormId(entry.mQuest) || entry.mQuest.isZeroOrUnset())
                        fail("could not read quest-entry FormID");
                    readExact(reader, entry.mStage, "quest-entry stage");
                    if (!reader.get(entry.mUnused.data(), entry.mUnused.size()))
                        fail("could not read quest-entry unused bytes");
                    phase = EntryPhase::End;
                    break;
                }
                if (currentEntry->mType == EntryType::Ability && phase == EntryPhase::AbilityDataOrEnd)
                {
                    requireSize(header, 4);
                    ESM::FormId ability;
                    if (!reader.getFormId(ability) || ability.isZeroOrUnset())
                        fail("could not read ability-entry FormID");
                    std::get<AbilityEntry>(currentEntry->mData).mAbility = ability;
                    phase = EntryPhase::End;
                    break;
                }
                if (currentEntry->mType == EntryType::EntryPoint && phase == EntryPhase::EntryPointData)
                {
                    requireSize(header, 3);
                    EntryPointData& data = std::get<EntryPointEntry>(currentEntry->mData).mData;
                    readExact(reader, data.mEntryPoint, "entry-point index");
                    readExact(reader, data.mFunction, "entry-point function");
                    readExact(reader, data.mConditionTabCount, "entry-point condition-tab count");
                    if (data.mConditionTabCount == 0)
                        fail("entry-point condition-tab count is zero");
                    phase = EntryPhase::EntryPointConditionsOrFunction;
                    break;
                }
                fail("entry DATA does not match its PRKE type or order");
            case ESM::fourCC("PRKC"):
            {
                if (currentEntry->mType != EntryType::EntryPoint
                    || (phase != EntryPhase::EntryPointConditionsOrFunction && phase != EntryPhase::EntryPointCondition))
                    fail("PRKC is out of order");
                if (currentConditionGroup != nullptr && currentConditionGroup->mConditions.empty())
                    fail("PRKC condition group is empty");
                requireSize(header, 1);
                std::uint8_t tab = 0;
                readExact(reader, tab, "PRKC tab");
                EntryPointEntry& entry = std::get<EntryPointEntry>(currentEntry->mData);
                if (tab >= entry.mData.mConditionTabCount)
                    fail("PRKC tab is outside the declared condition-tab count");
                if (!entry.mConditionGroups.empty() && tab <= entry.mConditionGroups.back().mTab)
                    fail("PRKC tabs are duplicated or out of order");
                entry.mConditionGroups.push_back({ .mTab = tab });
                currentConditionGroup = &entry.mConditionGroups.back();
                phase = EntryPhase::EntryPointCondition;
                break;
            }
            case ESM::fourCC("CTDA"):
                if (currentEntry->mType != EntryType::EntryPoint || phase != EntryPhase::EntryPointCondition
                    || currentConditionGroup == nullptr)
                    fail("entry CTDA appears without PRKC");
                currentConditionGroup->mConditions.push_back(readCondition(reader));
                break;
            case ESM::fourCC("EPFT"):
            {
                if (currentEntry->mType != EntryType::EntryPoint
                    || (phase != EntryPhase::EntryPointConditionsOrFunction && phase != EntryPhase::EntryPointCondition))
                    fail("EPFT is out of order");
                if (currentConditionGroup != nullptr && currentConditionGroup->mConditions.empty())
                    fail("PRKC condition group is empty");
                requireSize(header, 1);
                std::uint8_t rawType = 0;
                readExact(reader, rawType, "EPFT type");
                EntryPointEntry& entry = std::get<EntryPointEntry>(currentEntry->mData);
                switch (rawType)
                {
                    case static_cast<std::uint8_t>(EntryPointFunctionType::Float):
                        entry.mFunctionType = EntryPointFunctionType::Float;
                        phase = EntryPhase::FunctionData;
                        break;
                    case static_cast<std::uint8_t>(EntryPointFunctionType::FormId):
                        entry.mFunctionType = EntryPointFunctionType::FormId;
                        phase = EntryPhase::FunctionData;
                        break;
                    case static_cast<std::uint8_t>(EntryPointFunctionType::EmbeddedScript):
                        entry.mFunctionType = EntryPointFunctionType::EmbeddedScript;
                        phase = EntryPhase::ScriptLabel;
                        break;
                    default:
                        fail("unsupported EPFT type " + std::to_string(rawType));
                }
                currentConditionGroup = nullptr;
                break;
            }
            case ESM::fourCC("EPFD"):
            {
                if (currentEntry->mType != EntryType::EntryPoint || phase != EntryPhase::FunctionData)
                    fail("EPFD is out of order or belongs to an embedded script");
                requireSize(header, 4);
                EntryPointEntry& entry = std::get<EntryPointEntry>(currentEntry->mData);
                if (entry.mFunctionType == EntryPointFunctionType::Float)
                {
                    float number = 0.f;
                    readExact(reader, number, "EPFD float");
                    if (!std::isfinite(number))
                        fail("EPFD float is not finite");
                    entry.mFloat = number;
                }
                else if (entry.mFunctionType == EntryPointFunctionType::FormId)
                {
                    if (!reader.getFormId(entry.mFormId) || entry.mFormId.isZeroOrUnset())
                        fail("could not read EPFD FormID");
                }
                else
                    fail("EPFD does not match EPFT");
                phase = EntryPhase::End;
                break;
            }
            case ESM::fourCC("EPF2"):
            {
                if (currentEntry->mType != EntryType::EntryPoint || phase != EntryPhase::ScriptLabel)
                    fail("EPF2 is out of order or does not follow script EPFT");
                readZString(reader, std::get<EntryPointEntry>(currentEntry->mData).mButtonLabel, "EPF2");
                phase = EntryPhase::ScriptFlags;
                break;
            }
            case ESM::fourCC("EPF3"):
                if (currentEntry->mType != EntryType::EntryPoint || phase != EntryPhase::ScriptFlags)
                    fail("EPF3 is out of order");
                requireSize(header, 2);
                readExact(reader, std::get<EntryPointEntry>(currentEntry->mData).mScriptFlags, "EPF3 flags");
                phase = EntryPhase::ScriptHeader;
                break;
            case ESM::fourCC("SCHR"):
                if (currentEntry->mType != EntryType::EntryPoint || phase != EntryPhase::ScriptHeader)
                    fail("SCHR is out of order");
                requireSize(header, 20);
                if (!loadScriptSubRecord(reader, std::get<EntryPointEntry>(currentEntry->mData).mScript))
                    fail("could not read SCHR");
                phase = EntryPhase::ScriptBytecode;
                break;
            case ESM::fourCC("SCDA"):
                if (currentEntry->mType != EntryType::EntryPoint || phase != EntryPhase::ScriptBytecode)
                    fail("SCDA is out of order");
                if (header.dataSize == 0)
                    fail("zero-sized SCDA");
                if (!loadScriptSubRecord(reader, std::get<EntryPointEntry>(currentEntry->mData).mScript))
                    fail("could not read SCDA");
                phase = EntryPhase::ScriptSource;
                break;
            case ESM::fourCC("SCTX"):
                if (currentEntry->mType != EntryType::EntryPoint || phase != EntryPhase::ScriptSource)
                    fail("SCTX is out of order");
                if (header.dataSize == 0)
                    fail("zero-sized SCTX");
                if (!loadScriptSubRecord(reader, std::get<EntryPointEntry>(currentEntry->mData).mScript))
                    fail("could not read SCTX");
                phase = EntryPhase::ScriptReferences;
                break;
            case ESM::fourCC("SCRO"):
            {
                if (currentEntry->mType != EntryType::EntryPoint || phase != EntryPhase::ScriptReferences)
                    fail("SCRO is out of order");
                requireSize(header, 4);
                EntryPointEntry& entry = std::get<EntryPointEntry>(currentEntry->mData);
                if (!loadScriptSubRecord(reader, entry.mScript) || entry.mScript.references.back().isZeroOrUnset())
                    fail("could not read SCRO FormID");
                break;
            }
            case ESM::fourCC("PRKF"):
                requireSize(header, 0);
                if (currentEntry->mType == EntryType::Ability && phase == EntryPhase::AbilityDataOrEnd)
                {
                    closeEntry();
                    break;
                }
                if (phase == EntryPhase::ScriptReferences)
                    validateScript(std::get<EntryPointEntry>(currentEntry->mData));
                else if (phase != EntryPhase::End)
                    fail("PRKF closes an incomplete or mismatched entry");
                closeEntry();
                break;
            default:
                fail("unknown or out-of-order Fallout New Vegas entry subrecord " + ESM::printName(header.typeId));
        }
    }

    if (currentEntry != nullptr)
        fail("record ends before PRKF");
    if (!hasEditorId || !hasDescription || !hasData)
        fail("record is missing required EDID, DESC, or DATA");

    *this = std::move(value);
}
