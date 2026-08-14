#ifndef OPENMW_MWDIALOGUE_ESM4DIALOGUEUTILS_H
#define OPENMW_MWDIALOGUE_ESM4DIALOGUEUTILS_H

#include <functional>
#include <optional>
#include <string_view>

#include <components/esm4/loaddial.hpp>
#include <components/esm4/loadinfo.hpp>

namespace ESM4
{
    struct Quest;
    struct TargetCondition;
}

namespace MWWorld
{
    struct ESM4QuestState;
    class Ptr;
}

namespace MWDialogue
{
    using Esm4DialogueConditionEvaluator
        = std::function<std::optional<bool>(const ESM4::TargetCondition&)>;

    inline std::string_view getEsm4DialoguePrompt(
        const ESM4::Dialogue& dialogue, const ESM4::DialogInfo& info)
    {
        if (!info.mPrompt.empty())
            return info.mPrompt;
        if (!dialogue.mTopicName.empty())
            return dialogue.mTopicName;
        return dialogue.mEditorId;
    }

    std::optional<bool> evaluateEsm4ActorDialogueCondition(
        const ESM4::TargetCondition& condition, const MWWorld::Ptr& actor, bool isPlayer);

    bool matchesEsm4DialogueConditions(
        const std::vector<ESM4::TargetCondition>& conditions, const Esm4DialogueConditionEvaluator& evaluate);

    bool matchesEsm4DialogueInfoConditions(const ESM4::DialogInfo& info, const ESM4::Quest* ownerQuest,
        const MWWorld::ESM4QuestState* ownerState, const Esm4DialogueConditionEvaluator& evaluate);
}

#endif
