#ifndef OPENMW_MWWORLD_FNVTERMINALRUNTIME_H
#define OPENMW_MWWORLD_FNVTERMINALRUNTIME_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <components/esm/formid.hpp>
#include <components/esm4/loadterm.hpp>

namespace MWWorld
{
    inline constexpr std::uint32_t FnvTerminalScienceActorValue = 40;

    enum class FnvTerminalDifficulty : std::uint8_t
    {
        VeryEasy = 0,
        Easy = 1,
        Average = 2,
        Hard = 3,
        VeryHard = 4,
        RequiresKey = 5,
    };

    enum class FnvTerminalAccessResult
    {
        Open,
        PasswordAccepted,
        NeedsHacking,
        ComputerWhizRetry,
        InsufficientScience,
        RequiresKey,
        LockedOut,
        InvalidData,
    };

    struct FnvTerminalAccessSource
    {
        ESM4::Terminal::Data mData;
        float mScience = 0.f;
        float mRequiredScience = 0.f;
        bool mHasPassword = false;
        bool mReferenceUnlocked = false;
        bool mPreviouslyHacked = false;
        bool mLockedOut = false;
        bool mHasComputerWhiz = false;
        bool mComputerWhizRetryConsumed = false;
    };

    struct FnvTerminalAccessDecision
    {
        FnvTerminalAccessResult mResult = FnvTerminalAccessResult::InvalidData;
        FnvTerminalDifficulty mDifficulty = FnvTerminalDifficulty::RequiresKey;
        std::uint8_t mRequiredScience = 0;
    };

    [[nodiscard]] FnvTerminalAccessDecision resolveFnvTerminalAccess(const FnvTerminalAccessSource& source);
    [[nodiscard]] std::string_view getFnvTerminalMinimumScienceGameSetting(FnvTerminalDifficulty difficulty);

    class ESMStore;
    enum class ESM4Game;

    struct FnvTerminalSessionSource
    {
        ESM4Game mGame;
        unsigned int mRecordType;
        bool mDeleted;
        const ESM4::Terminal* mTerminal;
        const ESMStore* mStore = nullptr;
    };

    enum class FnvTerminalPreparationError
    {
        None,
        NotFalloutNewVegas,
        MissingTarget,
        WrongTargetType,
        DeletedTarget,
        MissingRequiredField,
        UnsupportedDataShape,
        UnsupportedMenuItem,
        MissingDisplayNote,
        UnsupportedDisplayNote,
    };

    class FnvTerminalSessionBuilder;

    struct PreparedTerminalDisplayNote
    {
        ESM::FormId mId;
        std::string mName;
        std::uint8_t mData = 0;
        std::string mText;
        std::string mImage;
        ESM::FormId mVoiceTopic;
        ESM::FormId mVoiceSpeaker;
        std::vector<ESM::FormId> mQuests;
    };

    class PreparedTerminalMenuItem final
    {
        const std::string mText;
        const std::string mResultText;
        const std::uint8_t mFlags;
        const std::optional<ESM::FormId> mDisplayNote;
        const std::optional<PreparedTerminalDisplayNote> mDisplayNotePayload;
        const std::optional<ESM::FormId> mSubmenu;
        const ESM4::ScriptDefinition mScript;
        const std::vector<ESM4::TargetCondition> mConditions;

        PreparedTerminalMenuItem(std::string text, std::string resultText, std::uint8_t flags,
            std::optional<ESM::FormId> displayNote, std::optional<ESM::FormId> submenu,
            std::optional<PreparedTerminalDisplayNote> displayNotePayload, ESM4::ScriptDefinition script,
            std::vector<ESM4::TargetCondition> conditions);

        friend class FnvTerminalSessionBuilder;

    public:
        std::string_view getText() const { return mText; }
        std::string_view getResultText() const { return mResultText; }
        std::uint8_t getFlags() const { return mFlags; }
        bool redrawsMenu() const { return (mFlags & 2u) != 0; }
        std::optional<ESM::FormId> getDisplayNote() const { return mDisplayNote; }
        const std::optional<PreparedTerminalDisplayNote>& getDisplayNotePayload() const
        {
            return mDisplayNotePayload;
        }
        std::optional<ESM::FormId> getSubmenu() const { return mSubmenu; }
        const ESM4::ScriptDefinition& getScript() const { return mScript; }
        const std::vector<ESM4::TargetCondition>& getConditions() const { return mConditions; }
    };

    /// A fully preflighted, read-only terminal interaction. There are no
    /// mutators so an action cannot turn presentation into game-state change.
    class PreparedTerminalSession final
    {
        const ESM::FormId mTerminal;
        const std::string mName;
        const std::string mDescription;
        const ESM::FormId mTopLevelScript;
        const ESM::FormId mPasswordNote;
        const ESM::FormId mSound;
        const ESM4::Terminal::Data mData;
        const std::vector<PreparedTerminalMenuItem> mMenuItems;

        PreparedTerminalSession(ESM::FormId terminal, std::string name, std::string description,
            ESM::FormId topLevelScript, ESM::FormId passwordNote, ESM::FormId sound,
            ESM4::Terminal::Data data, std::vector<PreparedTerminalMenuItem> menuItems);

        friend class FnvTerminalSessionBuilder;

    public:
        ESM::FormId getTerminal() const { return mTerminal; }
        std::string_view getName() const { return mName; }
        std::string_view getDescription() const { return mDescription; }
        ESM::FormId getTopLevelScript() const { return mTopLevelScript; }
        ESM::FormId getPasswordNote() const { return mPasswordNote; }
        ESM::FormId getSound() const { return mSound; }
        const ESM4::Terminal::Data& getData() const { return mData; }
        const std::vector<PreparedTerminalMenuItem>& getMenuItems() const { return mMenuItems; }
    };

    /// Copy a parser-validated FNV TERM into an immutable runtime session before
    /// UI or sound is allowed. Authored fields remain intact for the access,
    /// condition, submenu, result-script, and presentation layers.
    [[nodiscard]] std::optional<PreparedTerminalSession> prepareFnvTerminalSession(
        const FnvTerminalSessionSource& source, FnvTerminalPreparationError* error = nullptr);

    std::string_view getFnvTerminalPreparationErrorName(FnvTerminalPreparationError error);
}

#endif
