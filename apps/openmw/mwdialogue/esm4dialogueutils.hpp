#ifndef OPENMW_MWDIALOGUE_ESM4DIALOGUEUTILS_H
#define OPENMW_MWDIALOGUE_ESM4DIALOGUEUTILS_H

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <components/esm/formid.hpp>
#include <components/esm4/loaddial.hpp>
#include <components/esm4/loadinfo.hpp>
#include <components/misc/strings/algorithm.hpp>

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

    struct Esm4DialogueSelection
    {
        ESM::FormId mTopic{};
        ESM::FormId mInfo{};

        friend bool operator==(const Esm4DialogueSelection&, const Esm4DialogueSelection&) = default;
    };

    // Keeps the player-facing label and the exact INFO selected for it together. Fallout can have several
    // conditionally valid INFOs under one DIAL, so resolving the INFO again after the GUI click can execute a
    // different response than the one whose prompt was displayed.
    class Esm4DialoguePicker
    {
    public:
        using TopicMap = std::map<std::string, Esm4DialogueSelection, Misc::StringUtils::CiComp>;

        void clear();
        void clearTopics();
        void clearChoices();

        bool bindTopic(std::string_view title, Esm4DialogueSelection selection);
        int bindChoice(Esm4DialogueSelection selection);

        std::optional<Esm4DialogueSelection> selectTopic(std::string_view title) const;
        std::optional<Esm4DialogueSelection> selectChoice(int index) const;

        const TopicMap& getTopics() const { return mTopics; }
        bool hasChoices() const { return !mChoices.empty(); }

    private:
        TopicMap mTopics;
        std::vector<Esm4DialogueSelection> mChoices;
    };

    bool matchesEsm4DialogueSelection(
        const Esm4DialogueSelection& selection, const ESM4::DialogInfo& info);

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
