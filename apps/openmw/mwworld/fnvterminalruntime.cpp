#include "fnvterminalruntime.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include <components/esm/defs.hpp>
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
        static PreparedTerminalMenuItem makeMenuItem(std::string text, std::string resultText, bool redraw)
        {
            return PreparedTerminalMenuItem(std::move(text), std::move(resultText), redraw);
        }

        static PreparedTerminalSession makeSession(
            ESM::FormId terminal, std::string description, std::vector<PreparedTerminalMenuItem> menuItems)
        {
            return PreparedTerminalSession(terminal, std::move(description), std::move(menuItems));
        }
    };

    PreparedTerminalMenuItem::PreparedTerminalMenuItem(std::string text, std::string resultText, bool redraw)
        : mText(std::move(text))
        , mResultText(std::move(resultText))
        , mRedraw(redraw)
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
        // Frozen English Ultimate Edition numerator: 3/1,350 menu items,
        // 3/515 winning live TERM records, and 3/357 placed terminal REFRs.
        // This is runtime coverage only: no retail visual parity or certified
        // subsystem parity is claimed.
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
            if (!hasRenderableText(item.mText) || !hasRenderableText(item.mResultText)
                || (item.mFlags != 0 && item.mFlags != 2) || item.mDisplayNote.has_value() || item.mSubmenu.has_value()
                || !item.mConditions.empty() || !isStrictEmptyScript(item.mScript))
                return fail(FnvTerminalPreparationError::UnsupportedMenuItem, error);

            preparedItems.push_back(
                FnvTerminalSessionBuilder::makeMenuItem(item.mText, item.mResultText, item.mFlags == 2));
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
        }
        return "unknown";
    }
}
