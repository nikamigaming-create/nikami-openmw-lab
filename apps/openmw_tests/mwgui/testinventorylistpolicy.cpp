#include <gtest/gtest.h>

#include "apps/openmw/mwgui/inventorylistpolicy.hpp"

namespace MWGui
{
    TEST(InventoryListPolicyTest, EmptyFalloutListHasNoControllerFocus)
    {
        EXPECT_EQ(normalizeInventoryControllerFocus(0, 0), -1);
        EXPECT_EQ(normalizeInventoryControllerFocus(4, -1), -1);
    }

    TEST(InventoryListPolicyTest, KeepsValidFocusAcrossInventoryUpdates)
    {
        EXPECT_EQ(normalizeInventoryControllerFocus(0, 5), 0);
        EXPECT_EQ(normalizeInventoryControllerFocus(3, 5), 3);
    }

    TEST(InventoryListPolicyTest, ClampsFocusAfterFilteringOrTradingShrinksTheList)
    {
        EXPECT_EQ(normalizeInventoryControllerFocus(7, 3), 2);
        EXPECT_EQ(normalizeInventoryControllerFocus(-1, 3), 0);
    }
}
