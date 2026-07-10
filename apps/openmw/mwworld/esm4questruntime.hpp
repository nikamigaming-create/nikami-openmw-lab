#ifndef OPENMW_MWWORLD_ESM4QUESTRUNTIME_H
#define OPENMW_MWWORLD_ESM4QUESTRUNTIME_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <components/esm/formid.hpp>

namespace ESM4
{
    struct Quest;
    struct TargetCondition;
}

namespace ESM
{
    class ESMReader;
    class ESMWriter;
}

namespace MWWorld
{
    class ESMStore;
    class Globals;

    struct ESM4QuestState
    {
        enum Flag : std::uint8_t
        {
            Flag_Running = 0x01,
            Flag_Completed = 0x02,
            Flag_AllowRepeatedTopics = 0x04,
            Flag_AllowRepeatedStages = 0x08,
            Flag_RunAfterReset = 0x10,
            Flag_ShownInPipBoy = 0x20,
            Flag_Failed = 0x40,
        };

        enum ObjectiveFlag : std::uint8_t
        {
            Objective_Displayed = 0x01,
            Objective_Completed = 0x02,
        };

        std::uint8_t mFlags = 0;
        std::uint8_t mCurrentStage = 0;
        std::map<std::int16_t, bool> mStageDone;
        std::map<std::int32_t, std::uint8_t> mObjectiveStatus;
    };

    class ESM4QuestRuntime
    {
        const ESMStore* mStore = nullptr;
        const Globals* mGlobals = nullptr;
        std::unordered_map<ESM::FormId, ESM4QuestState> mStates;
        std::optional<ESM::FormId> mActiveQuest;
        std::vector<std::string> mUnsupportedStageCommands;
        std::vector<std::uint32_t> mUnsupportedConditionFunctions;

        const ESM4::Quest* resolveQuest(std::string_view id) const;
        ESM4QuestState* findState(const ESM4::Quest& quest);
        const ESM4QuestState* findState(const ESM4::Quest& quest) const;
        std::optional<float> evaluateConditionValue(const ESM4::TargetCondition& condition);
        bool isStateDirty(ESM::FormId id, const ESM4QuestState& state) const;
        void executeStageSource(std::string_view source);

    public:
        void initialize(const ESMStore& store, const Globals* globals = nullptr);
        void clear();

        bool startQuest(std::string_view id);
        bool setStage(std::string_view id, std::uint8_t stage);
        bool setStage(ESM::FormId id, std::uint8_t stage);
        bool setObjectiveDisplayed(std::string_view id, std::int32_t objective, bool displayed);
        bool setObjectiveCompleted(std::string_view id, std::int32_t objective, bool completed);
        bool forceActiveQuest(std::string_view id);
        bool evaluateConditions(const std::vector<ESM4::TargetCondition>& conditions);

        int countSavedGameRecords() const;
        void write(ESM::ESMWriter& writer) const;
        void readRecord(ESM::ESMReader& reader);

        const ESM4QuestState* search(std::string_view id) const;
        const ESM4QuestState* search(ESM::FormId id) const;
        std::optional<ESM::FormId> getActiveQuest() const { return mActiveQuest; }
        const std::vector<std::string>& getUnsupportedStageCommands() const { return mUnsupportedStageCommands; }
        const std::vector<std::uint32_t>& getUnsupportedConditionFunctions() const
        {
            return mUnsupportedConditionFunctions;
        }
    };
}

#endif
