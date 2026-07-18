#include "fnvterminalruntime.hpp"

#include <algorithm>
#include <array>
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

    bool isStrictEmptyScript(const ESM4::ScriptDefinition& script)
    {
        const ESM4::ScriptHeader& header = script.scriptHeader;
        return header.unused == 0 && header.refCount == 0 && header.compiledSize == 0 && header.variableCount == 0
            && header.type == 0 && header.flag == 1 && script.compiledData.empty() && script.scriptSource.empty()
            && script.localVarData.empty() && script.localRefVarIndex.empty() && script.references.empty()
            && script.globReference.isZeroOrUnset();
    }

    bool hasRequiredBounds(const ESM4::Terminal& terminal)
    {
        return std::any_of(terminal.mObjectBounds.begin(), terminal.mObjectBounds.end(),
            [](std::uint8_t value) { return value != 0; });
    }

    bool hasSupportedDataShape(const ESM4::Terminal& terminal)
    {
        // These are the only two byte-exact DNAM shapes in the frozen strict
        // read-only subset. Their individual field meanings are intentionally
        // not guessed by this runtime slice.
        constexpr std::array<std::uint8_t, 4> baseShape{ 0x00, 0x02, 0x04, 0x00 };
        constexpr std::array<std::uint8_t, 4> deadMoneyShape{ 0x00, 0x02, 0x08, 0x00 };
        return terminal.mData.mSerializedSize == 4
            && (terminal.mData.mBytes == baseShape || terminal.mData.mBytes == deadMoneyShape);
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
    class FnvTerminalSessionBuilder
    {
    public:
        static PreparedTerminalMenuItem makeMenuItem(std::string text, std::string resultText, bool redraw,
            std::optional<ESM::FormId> displayNote = std::nullopt)
        {
            return PreparedTerminalMenuItem(std::move(text), std::move(resultText), redraw, std::move(displayNote));
        }

        static PreparedTerminalSession makeSession(
            ESM::FormId terminal, std::string description, std::vector<PreparedTerminalMenuItem> menuItems)
        {
            return PreparedTerminalSession(terminal, std::move(description), std::move(menuItems));
        }
    };

    PreparedTerminalMenuItem::PreparedTerminalMenuItem(
        std::string text, std::string resultText, bool redraw, std::optional<ESM::FormId> displayNote)
        : mText(std::move(text))
        , mResultText(std::move(resultText))
        , mRedraw(redraw)
        , mDisplayNote(std::move(displayNote))
    {
    }

    PreparedTerminalSession::PreparedTerminalSession(
        ESM::FormId terminal, std::string description, std::vector<PreparedTerminalMenuItem> menuItems)
        : mTerminal(terminal)
        , mDescription(std::move(description))
        , mMenuItems(std::move(menuItems))
    {
    }

    std::optional<PreparedTerminalSession> prepareFnvTerminalSession(
        const FnvTerminalSessionSource& source, FnvTerminalPreparationError* error)
    {
        // Frozen English Ultimate Edition numerator: 14/1,350 menu items,
        // 7/515 winning live TERM records, and 7/357 placed terminal REFRs.
        // Ten of those items are DATA=1 NOTE links on exactly four TERM records
        // and four placements; the other four items retain the pre-existing
        // RNAM path. This is runtime coverage only: no retail visual parity or
        // certified subsystem parity is claimed.
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
        if (terminal.mFlags != 0)
            return fail(FnvTerminalPreparationError::UnsupportedRecordFlags, error);
        if (terminal.mId.isZeroOrUnset() || !hasRenderableText(terminal.mEditorId)
            || !hasRenderableText(terminal.mFullName) || !hasRenderableText(terminal.mModel)
            || !hasRequiredBounds(terminal) || !hasRenderableText(terminal.mText) || terminal.mMenuItems.empty())
            return fail(FnvTerminalPreparationError::MissingRequiredField, error);
        if (!terminal.mScriptId.isZeroOrUnset() || !terminal.mSound.isZeroOrUnset()
            || !terminal.mPasswordNote.isZeroOrUnset() || !terminal.mModelData.empty()
            || !terminal.mModelTextureSwaps.empty())
            return fail(FnvTerminalPreparationError::UnsupportedTopLevelField, error);
        if (!hasSupportedDataShape(terminal))
            return fail(FnvTerminalPreparationError::UnsupportedDataShape, error);

        std::vector<PreparedTerminalMenuItem> preparedItems;
        preparedItems.reserve(terminal.mMenuItems.size());
        for (const ESM4::Terminal::MenuItem& item : terminal.mMenuItems)
        {
            if (!hasRenderableText(item.mText) || (item.mFlags != 0 && item.mFlags != 2) || item.mSubmenu.has_value()
                || !item.mConditions.empty() || !isStrictEmptyScript(item.mScript))
                return fail(FnvTerminalPreparationError::UnsupportedMenuItem, error);

            std::string resultText;
            std::optional<ESM::FormId> displayNote;
            if (item.mDisplayNote.has_value())
            {
                if (source.mStore == nullptr)
                    return fail(FnvTerminalPreparationError::MissingDisplayNote, error);
                const ESM4::Note* note = source.mStore->get<ESM4::Note>().search(ESM::RefId(*item.mDisplayNote));
                if (note == nullptr)
                    return fail(FnvTerminalPreparationError::MissingDisplayNote, error);
                if (note->mId != *item.mDisplayNote || note->mFlags != 0 || note->mData != 1
                    || !hasRenderableText(note->mText) || !note->mImage.empty() || !note->mVoiceTopic.isZeroOrUnset()
                    || !note->mVoiceSpeaker.isZeroOrUnset() || !note->mQuests.empty())
                {
                    return fail(FnvTerminalPreparationError::UnsupportedDisplayNote, error);
                }
                resultText = note->mText;
                displayNote = *item.mDisplayNote;
            }
            else
            {
                if (!hasRenderableText(item.mResultText))
                    return fail(FnvTerminalPreparationError::UnsupportedMenuItem, error);
                resultText = item.mResultText;
            }

            preparedItems.push_back(FnvTerminalSessionBuilder::makeMenuItem(
                item.mText, std::move(resultText), item.mFlags == 2, std::move(displayNote)));
        }

        // loadterm keeps this compatibility mirror synchronized with the final
        // authored RNAM. A mismatch means the source was not a complete strict
        // FNV parse and must fail before presentation.
        if (terminal.mResultText != terminal.mMenuItems.back().mResultText)
            return fail(FnvTerminalPreparationError::MissingRequiredField, error);

        return FnvTerminalSessionBuilder::makeSession(terminal.mId, terminal.mText, std::move(preparedItems));
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
            case FnvTerminalPreparationError::UnsupportedRecordFlags:
                return "unsupported-record-flags";
            case FnvTerminalPreparationError::MissingRequiredField:
                return "missing-required-field";
            case FnvTerminalPreparationError::UnsupportedTopLevelField:
                return "unsupported-top-level-field";
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
