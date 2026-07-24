#include <gtest/gtest.h>

#include "apps/openmw/mwrender/playervisualpolicy.hpp"

namespace MWRender
{
    TEST(MWRenderPlayerVisualPolicyTest, normalSessionDoesNotInjectProofEquipment)
    {
        const ESM4PlayerVisualEquipmentPolicy policy
            = resolveESM4PlayerVisualEquipmentPolicy(nullptr, nullptr, nullptr, nullptr, false);
        EXPECT_TRUE(policy.mOutfit.empty());
        EXPECT_TRUE(policy.mHeadgear.empty());
    }

    TEST(MWRenderPlayerVisualPolicyTest, explicitEquipmentUsesGenericNamesBeforeLegacyNames)
    {
        const ESM4PlayerVisualEquipmentPolicy policy = resolveESM4PlayerVisualEquipmentPolicy(
            "GenericOutfit", "LegacyOutfit", "GenericHeadgear", "LegacyHeadgear", true);
        EXPECT_EQ(policy.mOutfit, "GenericOutfit");
        EXPECT_EQ(policy.mHeadgear, "GenericHeadgear");
    }

    TEST(MWRenderPlayerVisualPolicyTest, explicitProofBootstrapSelectsCourierDefault)
    {
        const ESM4PlayerVisualEquipmentPolicy policy
            = resolveESM4PlayerVisualEquipmentPolicy("", "", nullptr, "LegacyHeadgear", true);
        EXPECT_EQ(policy.mOutfit, "OutfitRepublican02");
        EXPECT_EQ(policy.mHeadgear, "LegacyHeadgear");
    }
}
