#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "apps/openmw/mwgui/falloutquestlistpolicy.hpp"

namespace
{
    constexpr ESM::FormId quest(std::uint32_t value)
    {
        return ESM::FormId::fromUint32(value);
    }

    TEST(FalloutQuestListPolicyTest, PreservesQuestIdentityAfterActiveFirstReordering)
    {
        const ESM::FormId first = quest(0x01000100);
        const ESM::FormId selected = quest(0x01000200);
        const std::vector<MWGui::FalloutQuestListRow> reordered{
            { selected, MWGui::FalloutQuestRowKind::Header },
            { first, MWGui::FalloutQuestRowKind::Header },
            { first, MWGui::FalloutQuestRowKind::Objective },
        };

        EXPECT_EQ(MWGui::selectFalloutQuestHeader(reordered, selected), 0u);
    }

    TEST(FalloutQuestListPolicyTest, ControllerNavigationSkipsObjectiveRows)
    {
        const ESM::FormId first = quest(0x01000100);
        const ESM::FormId second = quest(0x01000200);
        const std::vector<MWGui::FalloutQuestListRow> rows{
            { first, MWGui::FalloutQuestRowKind::Header },
            { first, MWGui::FalloutQuestRowKind::Objective },
            { second, MWGui::FalloutQuestRowKind::Header },
            { second, MWGui::FalloutQuestRowKind::Objective },
        };

        EXPECT_EQ(MWGui::advanceFalloutQuestHeader(rows, 0, true), 2u);
        EXPECT_EQ(MWGui::advanceFalloutQuestHeader(rows, 2, true), 0u);
        EXPECT_EQ(MWGui::advanceFalloutQuestHeader(rows, 0, false), 2u);
        EXPECT_EQ(MWGui::selectFalloutQuestHeader(rows, rows[3].mQuest), 2u);
    }

    TEST(FalloutQuestListPolicyTest, MissingSelectionFallsBackToFirstVisibleHeader)
    {
        const std::vector<MWGui::FalloutQuestListRow> rows{
            { quest(0x01000100), MWGui::FalloutQuestRowKind::Header },
            { quest(0x01000100), MWGui::FalloutQuestRowKind::Objective },
        };

        EXPECT_EQ(MWGui::selectFalloutQuestHeader(rows, quest(0x01000900)), 0u);
    }

    TEST(FalloutQuestListPolicyTest, HandlesEmptyAndSingleQuestLists)
    {
        const std::vector<MWGui::FalloutQuestListRow> empty;
        EXPECT_FALSE(MWGui::selectFalloutQuestHeader(empty, std::nullopt));
        EXPECT_FALSE(MWGui::advanceFalloutQuestHeader(empty, 0, true));

        const std::vector<MWGui::FalloutQuestListRow> single{
            { quest(0x01000100), MWGui::FalloutQuestRowKind::Header },
            { quest(0x01000100), MWGui::FalloutQuestRowKind::Objective },
        };
        EXPECT_EQ(MWGui::advanceFalloutQuestHeader(single, 0, true), 0u);
        EXPECT_EQ(MWGui::advanceFalloutQuestHeader(single, 1, false), 0u);
    }
}
