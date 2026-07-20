#include <gtest/gtest.h>

#include "apps/openmw/mwrender/playervisualpolicy.hpp"

namespace MWRender
{
    TEST(MWRenderPlayerVisualPolicyTest, keepsFlatFalloutPlayerAlignedWithGameplayYaw)
    {
        EXPECT_FLOAT_EQ(getFalloutFlatPlayerVisualYawOffset(), 0.f);
    }

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

    TEST(MWRenderPlayerVisualPolicyTest, derivesAuthoredFirstPersonModelFromWornBipedModel)
    {
        EXPECT_EQ(normalizeFalloutFirstPersonBipedModel(
                      "characters\\_male\\lefthandpipboyglove.nif"),
            "meshes/characters/_male/lefthandpipboyglove1st.nif");
        EXPECT_EQ(normalizeFalloutFirstPersonBipedModel(
                      "meshes/characters/_male/femalelefthandpipboyglove1st.nif"),
            "meshes/characters/_male/femalelefthandpipboyglove1st.nif");
        EXPECT_TRUE(normalizeFalloutFirstPersonBipedModel("").empty());
        EXPECT_TRUE(normalizeFalloutFirstPersonBipedModel("not-a-nif.dds").empty());
    }

    TEST(MWRenderPlayerVisualPolicyTest, unrelatedLeftHandArmorIsNotMisclassifiedAsPipBoyGlove)
    {
        EXPECT_TRUE(isFalloutPipBoyGloveFirstPersonModel(
            true, "meshes/characters/_male/lefthandpipboyglove1st.nif"));
        EXPECT_FALSE(isFalloutPipBoyGloveFirstPersonModel(
            false, "meshes/characters/_male/lefthandpipboyglove1st.nif"));
        EXPECT_FALSE(isFalloutPipBoyGloveFirstPersonModel(
            true, "meshes/armor/raider/lefthandglove1st.nif"));
    }

    TEST(MWRenderPlayerVisualPolicyTest, selectsAuthoredFirstPersonWeaponModelOnlyForCameraView)
    {
        constexpr std::string_view world = "weapons/2handrifle/varmintrifle.nif";
        constexpr std::string_view firstPerson = "weapons/2handrifle/1stpersonvarmintrifle.nif";

        EXPECT_EQ(selectFalloutWeaponViewModel(world, firstPerson, false), world);
        EXPECT_EQ(selectFalloutWeaponViewModel(world, firstPerson, true), firstPerson);
        EXPECT_EQ(selectFalloutWeaponViewModel(world, {}, true), world);
    }
}
