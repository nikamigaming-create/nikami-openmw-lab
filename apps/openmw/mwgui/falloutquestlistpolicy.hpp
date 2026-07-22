#ifndef OPENMW_MWGUI_FALLOUTQUESTLISTPOLICY_H
#define OPENMW_MWGUI_FALLOUTQUESTLISTPOLICY_H

#include <cstddef>
#include <optional>
#include <span>

#include <components/esm/formid.hpp>

namespace MWGui
{
    enum class FalloutQuestRowKind
    {
        Header,
        Objective,
    };

    struct FalloutQuestListRow
    {
        ESM::FormId mQuest;
        FalloutQuestRowKind mKind = FalloutQuestRowKind::Header;
    };

    [[nodiscard]] inline std::optional<std::size_t> selectFalloutQuestHeader(
        std::span<const FalloutQuestListRow> rows, std::optional<ESM::FormId> selectedQuest)
    {
        std::optional<std::size_t> firstHeader;
        for (std::size_t index = 0; index < rows.size(); ++index)
        {
            if (rows[index].mKind != FalloutQuestRowKind::Header)
                continue;
            if (!firstHeader)
                firstHeader = index;
            if (selectedQuest && rows[index].mQuest == *selectedQuest)
                return index;
        }
        return firstHeader;
    }

    [[nodiscard]] inline std::optional<std::size_t> advanceFalloutQuestHeader(
        std::span<const FalloutQuestListRow> rows, std::size_t current, bool next)
    {
        if (rows.empty())
            return std::nullopt;

        std::optional<std::size_t> start;
        if (current < rows.size())
            start = selectFalloutQuestHeader(rows, rows[current].mQuest);
        if (!start)
            start = selectFalloutQuestHeader(rows, std::nullopt);
        if (!start)
            return std::nullopt;

        for (std::size_t distance = 1; distance <= rows.size(); ++distance)
        {
            const std::size_t index = next
                ? (*start + distance) % rows.size()
                : (*start + rows.size() - (distance % rows.size())) % rows.size();
            if (rows[index].mKind == FalloutQuestRowKind::Header)
                return index;
        }
        return start;
    }
}

#endif
