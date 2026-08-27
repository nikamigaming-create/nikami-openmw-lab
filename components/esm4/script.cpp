#include "script.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

#include "falloutformat.hpp"
#include "reader.hpp"

namespace
{
    constexpr std::size_t sFnvTargetConditionSize = sizeof(ESM4::TargetCondition);
    static_assert(sFnvTargetConditionSize == ESM4::Fallout::kTargetConditionNativeBytes);
}

bool ESM4::loadTargetCondition(Reader& reader, TargetCondition& condition, ESM::FormId* parameter3)
{
    condition = {};
    if (parameter3 != nullptr)
        *parameter3 = {};

    bool loaded = false;
    switch (reader.subRecordHeader().dataSize)
    {
        case Fallout::kTargetConditionTes4Bytes:
            loaded = reader.get(&condition, Fallout::kTargetConditionTes4Bytes);
            break;
        case Fallout::kTargetConditionFalloutBytes:
            loaded = reader.get(&condition, Fallout::kTargetConditionFalloutBytes);
            break;
        case sFnvTargetConditionSize:
            loaded = reader.getExact(condition);
            break;
        case Fallout::kTargetConditionTes5Bytes:
        {
            loaded = reader.get(&condition, Fallout::kTargetConditionTes5PrefixBytes);
            ESM::FormId32 rawParameter3 = 0;
            if (loaded)
                loaded = reader.getExact(rawParameter3);
            if (loaded && parameter3 != nullptr)
            {
                *parameter3 = ESM::FormId::fromUint32(rawParameter3);
                if (!parameter3->isZeroOrUnset())
                    reader.adjustFormId(*parameter3);
            }
            if (loaded)
                loaded = reader.getExact(condition.runOn);
            if (loaded)
                loaded = reader.getExact(condition.reference);
            if (loaded)
                reader.skipSubRecordData(Fallout::kTargetConditionTes5TailBytes);
            break;
        }
        default:
            reader.skipSubRecordData();
            return false;
    }

    if (!loaded)
        return false;

    if (condition.reference != 0)
        reader.adjustFormId(condition.reference);

    if ((condition.condition & CTF_UseGlobal) != 0)
    {
        std::uint32_t comparisonGlobal = std::bit_cast<std::uint32_t>(condition.comparison);
        if (comparisonGlobal != 0)
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
            if (condition.param1 != 0)
                reader.adjustFormId(condition.param1);
            break;
        default:
            break;
    }

    return true;
}
