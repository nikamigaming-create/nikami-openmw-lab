#include "fnvterminalruntime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <components/esm/defs.hpp>
#include <components/esm/refid.hpp>
#include <components/esm4/loadnote.hpp>
#include <components/esm4/loadterm.hpp>

#include "esmstore.hpp"

namespace
{
    bool hasRenderableText(std::string_view value)
    {
        return !value.empty() && value.find('\0') == std::string_view::npos;
    }

    bool hasRequiredBounds(const ESM4::Terminal& terminal)
    {
        return std::any_of(terminal.mObjectBounds.begin(), terminal.mObjectBounds.end(),
            [](std::uint8_t value) { return value != 0; });
    }

    bool hasSupportedDataShape(const ESM4::Terminal& terminal)
    {
        return terminal.mData.mSerializedSize == 3 || terminal.mData.mSerializedSize == 4;
    }

    std::optional<MWWorld::PreparedTerminalSession> fail(
        MWWorld::FnvTerminalPreparationError value, MWWorld::FnvTerminalPreparationError* output)
    {
        if (output != nullptr)
            *output = value;
        return std::nullopt;
    }
}

namespace MWWorld
{
    FnvTerminalAccessDecision resolveFnvTerminalAccess(const FnvTerminalAccessSource& source)
    {
        FnvTerminalAccessDecision result;
        if ((source.mData.mSerializedSize != 3 && source.mData.mSerializedSize != 4)
            || source.mData.mBytes[0] > static_cast<std::uint8_t>(FnvTerminalDifficulty::RequiresKey)
            || !std::isfinite(source.mScience) || !std::isfinite(source.mRequiredScience)
            || source.mRequiredScience < 0.f || source.mRequiredScience > 255.f)
            return result;

        result.mDifficulty = static_cast<FnvTerminalDifficulty>(source.mData.mBytes[0]);
        result.mRequiredScience = result.mDifficulty == FnvTerminalDifficulty::RequiresKey
            ? 255
            : static_cast<std::uint8_t>(source.mRequiredScience);

        constexpr std::uint8_t unlockedFlag = 1u << 1;
        if (source.mReferenceUnlocked || source.mPreviouslyHacked || (source.mData.mBytes[1] & unlockedFlag) != 0)
            result.mResult = FnvTerminalAccessResult::Open;
        else if (source.mHasPassword)
            result.mResult = FnvTerminalAccessResult::PasswordAccepted;
        else if (source.mLockedOut)
            result.mResult = source.mHasComputerWhiz && !source.mComputerWhizRetryConsumed
                ? FnvTerminalAccessResult::ComputerWhizRetry
                : FnvTerminalAccessResult::LockedOut;
        else if (result.mDifficulty == FnvTerminalDifficulty::RequiresKey)
            result.mResult = FnvTerminalAccessResult::RequiresKey;
        else if (source.mScience < result.mRequiredScience)
            result.mResult = FnvTerminalAccessResult::InsufficientScience;
        else
            result.mResult = FnvTerminalAccessResult::NeedsHacking;
        return result;
    }

    std::string_view getFnvTerminalMinimumScienceGameSetting(FnvTerminalDifficulty difficulty)
    {
        constexpr std::array<std::string_view, 5> settings{
            "fHackingMinSkillVeryEasy",
            "fHackingMinSkillEasy",
            "fHackingMinSkillAverage",
            "fHackingMinSkillHard",
            "fHackingMinSkillVeryHard",
        };
        const std::size_t index = static_cast<std::size_t>(difficulty);
        return index < settings.size() ? settings[index] : std::string_view{};
    }

    class FnvTerminalSessionBuilder
    {
    public:
        static PreparedTerminalMenuItem makeMenuItem(std::string text, std::string resultText, std::uint8_t flags,
            std::optional<ESM::FormId> displayNote, std::optional<ESM::FormId> submenu,
            std::optional<PreparedTerminalDisplayNote> displayNotePayload, ESM4::ScriptDefinition script,
            std::vector<ESM4::TargetCondition> conditions)
        {
            return PreparedTerminalMenuItem(std::move(text), std::move(resultText), flags, std::move(displayNote),
                std::move(submenu), std::move(displayNotePayload), std::move(script), std::move(conditions));
        }

        static PreparedTerminalSession makeSession(ESM::FormId terminal, std::string name, std::string description,
            ESM::FormId topLevelScript, ESM::FormId passwordNote, ESM::FormId sound,
            ESM4::Terminal::Data data, std::vector<PreparedTerminalMenuItem> menuItems)
        {
            return PreparedTerminalSession(terminal, std::move(name), std::move(description), topLevelScript,
                passwordNote, sound, data, std::move(menuItems));
        }
    };

    PreparedTerminalMenuItem::PreparedTerminalMenuItem(std::string text, std::string resultText,
        std::uint8_t flags, std::optional<ESM::FormId> displayNote, std::optional<ESM::FormId> submenu,
        std::optional<PreparedTerminalDisplayNote> displayNotePayload, ESM4::ScriptDefinition script,
        std::vector<ESM4::TargetCondition> conditions)
        : mText(std::move(text))
        , mResultText(std::move(resultText))
        , mFlags(flags)
        , mDisplayNote(std::move(displayNote))
        , mDisplayNotePayload(std::move(displayNotePayload))
        , mSubmenu(std::move(submenu))
        , mScript(std::move(script))
        , mConditions(std::move(conditions))
    {
    }

    PreparedTerminalSession::PreparedTerminalSession(ESM::FormId terminal, std::string name,
        std::string description, ESM::FormId topLevelScript, ESM::FormId passwordNote, ESM::FormId sound,
        ESM4::Terminal::Data data, std::vector<PreparedTerminalMenuItem> menuItems)
        : mTerminal(terminal)
        , mName(std::move(name))
        , mDescription(std::move(description))
        , mTopLevelScript(topLevelScript)
        , mPasswordNote(passwordNote)
        , mSound(sound)
        , mData(data)
        , mMenuItems(std::move(menuItems))
    {
    }

    std::optional<PreparedTerminalSession> prepareFnvTerminalSession(
        const FnvTerminalSessionSource& source, FnvTerminalPreparationError* error)
    {
        if (error != nullptr)
            *error = FnvTerminalPreparationError::None;

        if (source.mGame != ESM4Game::FalloutNewVegas)
            return fail(FnvTerminalPreparationError::NotFalloutNewVegas, error);
        if (source.mRecordType == 0 && source.mTerminal == nullptr)
            return fail(FnvTerminalPreparationError::MissingTarget, error);
        if (source.mRecordType != ESM::REC_TERM4)
            return fail(FnvTerminalPreparationError::WrongTargetType, error);
        if (source.mDeleted)
            return fail(FnvTerminalPreparationError::DeletedTarget, error);
        if (source.mTerminal == nullptr)
            return fail(FnvTerminalPreparationError::MissingTarget, error);

        const ESM4::Terminal& terminal = *source.mTerminal;
        if ((terminal.mFlags & ESM4::Rec_Deleted) != 0)
            return fail(FnvTerminalPreparationError::DeletedTarget, error);
        if (terminal.mId.isZeroOrUnset() || !hasRenderableText(terminal.mEditorId)
            || !hasRequiredBounds(terminal) || terminal.mMenuItems.empty())
            return fail(FnvTerminalPreparationError::MissingRequiredField, error);
        if (!hasSupportedDataShape(terminal))
            return fail(FnvTerminalPreparationError::UnsupportedDataShape, error);

        std::vector<PreparedTerminalMenuItem> preparedItems;
        preparedItems.reserve(terminal.mMenuItems.size());
        for (const ESM4::Terminal::MenuItem& item : terminal.mMenuItems)
        {
            if (!hasRenderableText(item.mText) || item.mFlags > 3)
                return fail(FnvTerminalPreparationError::UnsupportedMenuItem, error);

            std::string resultText;
            std::optional<ESM::FormId> displayNote;
            std::optional<PreparedTerminalDisplayNote> displayNotePayload;
            if (item.mDisplayNote.has_value())
            {
                if (source.mStore == nullptr)
                    return fail(FnvTerminalPreparationError::MissingDisplayNote, error);
                const ESM4::Note* note = source.mStore->get<ESM4::Note>().search(ESM::RefId(*item.mDisplayNote));
                if (note == nullptr)
                    return fail(FnvTerminalPreparationError::MissingDisplayNote, error);
                if (note->mId != *item.mDisplayNote || (note->mFlags & ESM4::Rec_Deleted) != 0)
                {
                    return fail(FnvTerminalPreparationError::UnsupportedDisplayNote, error);
                }
                switch (note->mData)
                {
                    case 0:
                        if (!note->mText.empty() || !note->mImage.empty() || !note->mVoiceTopic.isZeroOrUnset()
                            || !note->mVoiceSpeaker.isZeroOrUnset())
                            return fail(FnvTerminalPreparationError::UnsupportedDisplayNote, error);
                        break;
                    case 1:
                        if (!hasRenderableText(note->mText) || !note->mImage.empty()
                            || !note->mVoiceTopic.isZeroOrUnset() || !note->mVoiceSpeaker.isZeroOrUnset())
                            return fail(FnvTerminalPreparationError::UnsupportedDisplayNote, error);
                        resultText = note->mText;
                        break;
                    case 2:
                        if (!hasRenderableText(note->mImage) || !note->mText.empty()
                            || !note->mVoiceTopic.isZeroOrUnset() || !note->mVoiceSpeaker.isZeroOrUnset())
                            return fail(FnvTerminalPreparationError::UnsupportedDisplayNote, error);
                        resultText = item.mResultText;
                        break;
                    case 3:
                        if (note->mVoiceTopic.isZeroOrUnset() || !note->mText.empty() || !note->mImage.empty())
                            return fail(FnvTerminalPreparationError::UnsupportedDisplayNote, error);
                        resultText = item.mResultText;
                        break;
                    default:
                        return fail(FnvTerminalPreparationError::UnsupportedDisplayNote, error);
                }
                displayNote = *item.mDisplayNote;
                displayNotePayload = PreparedTerminalDisplayNote{ note->mId, note->mFullName, note->mData,
                    note->mText, note->mImage, note->mVoiceTopic, note->mVoiceSpeaker, note->mQuests };
            }
            else
            {
                if (!item.mSubmenu.has_value() && item.mScript.scriptSource.empty()
                    && item.mScript.compiledData.empty() && !hasRenderableText(item.mResultText))
                    return fail(FnvTerminalPreparationError::UnsupportedMenuItem, error);
                resultText = item.mResultText;
            }

            preparedItems.push_back(FnvTerminalSessionBuilder::makeMenuItem(
                item.mText, std::move(resultText), item.mFlags, std::move(displayNote), item.mSubmenu,
                std::move(displayNotePayload), item.mScript, item.mConditions));
        }

        // loadterm keeps this compatibility mirror synchronized with the final
        // authored RNAM. A mismatch means the source was not a complete strict
        // FNV parse and must fail before presentation.
        if (terminal.mResultText != terminal.mMenuItems.back().mResultText)
            return fail(FnvTerminalPreparationError::MissingRequiredField, error);

        return FnvTerminalSessionBuilder::makeSession(terminal.mId, terminal.mFullName, terminal.mText,
            terminal.mScriptId, terminal.mPasswordNote, terminal.mSound, terminal.mData, std::move(preparedItems));
    }

    std::string_view getFnvTerminalPreparationErrorName(FnvTerminalPreparationError error)
    {
        switch (error)
        {
            case FnvTerminalPreparationError::None:
                return "none";
            case FnvTerminalPreparationError::NotFalloutNewVegas:
                return "not-fallout-new-vegas";
            case FnvTerminalPreparationError::MissingTarget:
                return "missing-target";
            case FnvTerminalPreparationError::WrongTargetType:
                return "wrong-target-type";
            case FnvTerminalPreparationError::DeletedTarget:
                return "deleted-target";
            case FnvTerminalPreparationError::MissingRequiredField:
                return "missing-required-field";
            case FnvTerminalPreparationError::UnsupportedDataShape:
                return "unsupported-data-shape";
            case FnvTerminalPreparationError::UnsupportedMenuItem:
                return "unsupported-menu-item";
            case FnvTerminalPreparationError::MissingDisplayNote:
                return "missing-display-note";
            case FnvTerminalPreparationError::UnsupportedDisplayNote:
                return "unsupported-display-note";
        }
        return "unknown";
    }
}
