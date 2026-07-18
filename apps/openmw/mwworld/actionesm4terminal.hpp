#ifndef GAME_MWWORLD_ACTIONESM4TERMINAL_H
#define GAME_MWWORLD_ACTIONESM4TERMINAL_H

#include <string_view>
#include <vector>

#include "action.hpp"
#include "fnvterminalruntime.hpp"

namespace MWWorld
{
    class TerminalSessionPresenter
    {
    public:
        virtual ~TerminalSessionPresenter() = default;

        /// Present one blocking message and return its selected button index.
        virtual int show(std::string_view message, const std::vector<std::string>& buttons) = 0;
    };

    enum class TerminalSessionRunResult
    {
        Completed,
        Cancelled,
        InvalidSelection,
        RedrawLimitExceeded,
    };

    /// Run the immutable DESC -> ITXT -> RNAM interaction. ANAM 2 redraws
    /// the root menu, with a defensive limit of sixteen completed redraws.
    [[nodiscard]] TerminalSessionRunResult runPreparedTerminalSession(
        const PreparedTerminalSession& session, TerminalSessionPresenter& presenter);

    class ActionEsm4Terminal final : public Action
    {
        const PreparedTerminalSession mSession;

        void executeImp(const Ptr& actor) override;

    public:
        ActionEsm4Terminal(const Ptr& target, PreparedTerminalSession session);
    };
}

#endif
