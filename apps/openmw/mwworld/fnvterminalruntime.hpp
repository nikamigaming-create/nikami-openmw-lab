#ifndef OPENMW_MWWORLD_FNVTERMINALRUNTIME_H
#define OPENMW_MWWORLD_FNVTERMINALRUNTIME_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <components/esm/formid.hpp>

namespace ESM4
{
    struct Terminal;
}

namespace MWWorld
{
    enum class ESM4Game;

    struct FnvTerminalSessionSource
    {
        ESM4Game mGame;
        unsigned int mRecordType;
        bool mDeleted;
        const ESM4::Terminal* mTerminal;
    };

    enum class FnvTerminalPreparationError
    {
        None,
        NotFalloutNewVegas,
        MissingTarget,
        WrongTargetType,
        DeletedTarget,
        UnsupportedRecordFlags,
        MissingRequiredField,
        UnsupportedTopLevelField,
        UnsupportedDataShape,
        UnsupportedMenuItem,
    };

    class FnvTerminalSessionBuilder;

    class PreparedTerminalMenuItem final
    {
        const std::string mText;
        const std::string mResultText;
        const bool mRedraw;

        PreparedTerminalMenuItem(std::string text, std::string resultText, bool redraw);

        friend class FnvTerminalSessionBuilder;

    public:
        std::string_view getText() const { return mText; }
        std::string_view getResultText() const { return mResultText; }
        bool redrawsMenu() const { return mRedraw; }
    };

    /// A fully preflighted, read-only terminal interaction. There are no
    /// mutators so an action cannot turn presentation into game-state change.
    class PreparedTerminalSession final
    {
        const ESM::FormId mTerminal;
        const std::string mDescription;
        const std::vector<PreparedTerminalMenuItem> mMenuItems;

        PreparedTerminalSession(
            ESM::FormId terminal, std::string description, std::vector<PreparedTerminalMenuItem> menuItems);

        friend class FnvTerminalSessionBuilder;

    public:
        ESM::FormId getTerminal() const { return mTerminal; }
        std::string_view getDescription() const { return mDescription; }
        const std::vector<PreparedTerminalMenuItem>& getMenuItems() const { return mMenuItems; }
    };

    /// Preflight the complete TERM before any UI or sound is allowed. This
    /// deliberately narrow first slice accepts exactly the frozen three-item
    /// official corpus subset and treats the two supported DNAM payloads as
    /// opaque byte shapes; it does not infer lock or difficulty semantics.
    [[nodiscard]] std::optional<PreparedTerminalSession> prepareFnvTerminalSession(
        const FnvTerminalSessionSource& source, FnvTerminalPreparationError* error = nullptr);

    std::string_view getFnvTerminalPreparationErrorName(FnvTerminalPreparationError error);
}

#endif
