#include <gtest/gtest.h>

#include "apps/openmw/mwrender/playervisualpolicy.hpp"

namespace MWRender
{
    TEST(MWRenderPlayerVisualPolicyTest, normalSessionDoesNotInjectProofEquipment)
    {
        const ESM4PlayerVisualEquipmentPolicy policy
            = resolveESM4PlayerVisualEquipmentPolicy(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false);
        EXPECT_TRUE(policy.mOutfit.empty());
        EXPECT_TRUE(policy.mHeadgear.empty());
        EXPECT_TRUE(policy.mWeapon.empty());
    }

    TEST(MWRenderPlayerVisualPolicyTest, explicitEquipmentUsesGenericNamesBeforeLegacyNames)
    {
        const ESM4PlayerVisualEquipmentPolicy policy = resolveESM4PlayerVisualEquipmentPolicy(
            "GenericOutfit", "LegacyOutfit", "GenericHeadgear", "LegacyHeadgear", "GenericWeapon",
            "LegacyWeapon", true);
        EXPECT_EQ(policy.mOutfit, "GenericOutfit");
        EXPECT_EQ(policy.mHeadgear, "GenericHeadgear");
        EXPECT_EQ(policy.mWeapon, "GenericWeapon");
    }

    TEST(MWRenderPlayerVisualPolicyTest, explicitProofBootstrapSelectsCourierDefault)
    {
        const ESM4PlayerVisualEquipmentPolicy policy
            = resolveESM4PlayerVisualEquipmentPolicy("", "", nullptr, nullptr, "", "", true);
        EXPECT_EQ(policy.mOutfit, "VaultSuit21");
        EXPECT_EQ(policy.mHeadgear, "CowboyHat02");
        EXPECT_EQ(policy.mWeapon, "WeapNVVarmintRifle");
    }

    TEST(MWRenderPlayerVisualPolicyTest, legacyOverridesRemainAvailableForCourierProofs)
    {
        const ESM4PlayerVisualEquipmentPolicy policy = resolveESM4PlayerVisualEquipmentPolicy(
            "", "LegacyOutfit", "", "LegacyHeadgear", "", "LegacyWeapon", true);
        EXPECT_EQ(policy.mOutfit, "LegacyOutfit");
        EXPECT_EQ(policy.mHeadgear, "LegacyHeadgear");
        EXPECT_EQ(policy.mWeapon, "LegacyWeapon");
    }
}
