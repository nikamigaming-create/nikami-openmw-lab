#include <gtest/gtest.h>

#include "apps/openmw/mwrender/falloutanimationtargets.hpp"
#include "apps/openmw/mwrender/fallouthitreaction.hpp"
#include "apps/openmw/mwrender/falloutweaponanimation.hpp"
#include "apps/openmw/mwmechanics/weapontype.hpp"

#include <components/nif/controller.hpp>
#include <components/nif/data.hpp>
#include <components/nifosg/controller.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm3/loadweap.hpp>

#include <bit>
#include <memory>

namespace MWRender
{
    namespace
    {
        osg::ref_ptr<NifOsg::KeyframeController> makeEndpointController(
            const osg::Vec3f& startTranslation, const osg::Vec3f& endTranslation,
            const osg::Quat& startRotation, const osg::Quat& endRotation)
        {
            Nif::NiKeyframeData data;
            data.mTranslations = std::make_shared<Nif::Vector3KeyMap>();
            data.mTranslations->mInterpolationType = Nif::InterpolationType_Linear;
            Nif::KeyT<osg::Vec3f> startTranslationKey;
            startTranslationKey.mValue = startTranslation;
            Nif::KeyT<osg::Vec3f> endTranslationKey;
            endTranslationKey.mValue = endTranslation;
            data.mTranslations->mKeys.emplace_back(0.f, startTranslationKey);
            data.mTranslations->mKeys.emplace_back(1.f, endTranslationKey);

            data.mRotations = std::make_shared<Nif::QuaternionKeyMap>();
            data.mRotations->mInterpolationType = Nif::InterpolationType_Linear;
            Nif::KeyT<osg::Quat> startRotationKey;
            startRotationKey.mValue = startRotation;
            Nif::KeyT<osg::Quat> endRotationKey;
            endRotationKey.mValue = endRotation;
            data.mRotations->mKeys.emplace_back(0.f, startRotationKey);
            data.mRotations->mKeys.emplace_back(1.f, endRotationKey);

            Nif::NiTransformInterpolator interpolator;
            interpolator.recType = Nif::RC_NiTransformInterpolator;
            interpolator.mData = Nif::NiKeyframeDataPtr(&data);
            interpolator.mDefaultValue.mTranslation = startTranslation;
            interpolator.mDefaultValue.mRotation = startRotation;
            interpolator.mDefaultValue.mScale = 1.f;

            osg::ref_ptr<NifOsg::KeyframeController> controller = new NifOsg::KeyframeController(&interpolator);
            controller->setFunction(std::make_shared<NifOsg::ControllerFunction>(1.f, 0.f, 0.f, 1.f,
                Nif::NiTimeController::ExtrapolationMode::Constant));
            return controller;
        }

        void expectEndpoint(const SceneUtil::KeyframeController::KfTransform& actual,
            const osg::Vec3f& expectedTranslation, const osg::Quat& expectedRotation)
        {
            ASSERT_TRUE(actual.mTranslation.has_value());
            ASSERT_TRUE(actual.mRotation.has_value());
            EXPECT_NEAR(actual.mTranslation->x(), expectedTranslation.x(), 1e-6f);
            EXPECT_NEAR(actual.mTranslation->y(), expectedTranslation.y(), 1e-6f);
            EXPECT_NEAR(actual.mTranslation->z(), expectedTranslation.z(), 1e-6f);
            EXPECT_NEAR(actual.mRotation->x(), expectedRotation.x(), 1e-6f);
            EXPECT_NEAR(actual.mRotation->y(), expectedRotation.y(), 1e-6f);
            EXPECT_NEAR(actual.mRotation->z(), expectedRotation.z(), 1e-6f);
            EXPECT_NEAR(actual.mRotation->w(), expectedRotation.w(), 1e-6f);
        }
    }

    TEST(FalloutHitReactionTest, resolvesCanonicalRecoverySourceWithinEachGoodspringsCreatureRig)
    {
        struct ExpectedSource
        {
            std::string_view mDirectory;
            std::string_view mPath;
        };
        static constexpr std::array<ExpectedSource, 6> expectedSources = { {
            { "meshes/creatures/nvsecuritron", "meshes/creatures/nvsecuritron/idleanims/specialidle_hithead.kf" },
            { "meshes/creatures/dog", "meshes/creatures/dog/idleanims/mtspecialidle_hithead.kf" },
            { "meshes/creatures/nvbighorner", "meshes/creatures/nvbighorner/idleanims/hitreaction_torso.kf" },
            { "meshes/creatures/nvmantis", "meshes/creatures/nvmantis/idleanims/specialidle_hittorso.kf" },
            { "meshes/creatures/radscorpion", "meshes/creatures/radscorpion/recoil.kf" },
            { "meshes/creatures/nvgecko", "meshes/creatures/nvgecko/idleanims/hitreaction_torso.kf" },
        } };
        const std::vector<std::string> available = {
            "meshes/creatures/nvsecuritron/idleanims/specialidle_hithead.kf",
            "meshes/creatures/dog/idleanims/mtspecialidle_hithead.kf",
            "meshes/creatures/dog/h2hrecoil.kf",
            "meshes/creatures/nvbighorner/idleanims/hitreaction_torso.kf",
            "meshes/creatures/nvbighorner/idleanims/hitreaction_head.kf",
            "meshes/creatures/nvmantis/idleanims/specialidle_hittorso.kf",
            "meshes/creatures/nvmantis/idleanims/specialidle_hithead.kf",
            "meshes/creatures/radscorpion/recoil.kf",
            "meshes/creatures/nvgecko/idleanims/hitreaction_torso.kf",
            "meshes/creatures/nvgecko/idleanims/hitreaction_head.kf",
        };
        const auto exists = [&available](std::string_view path) {
            return std::find(available.begin(), available.end(), path) != available.end();
        };

        EXPECT_EQ(FonvCreatureHitReactionSemanticGroup, "hit1");
        for (const ExpectedSource& expected : expectedSources)
            EXPECT_EQ(resolveFonvCreatureHitReaction(expected.mDirectory, exists), expected.mPath);
    }

    TEST(FalloutHitReactionTest, preservesDeterministicTorsoFirstCandidatePriority)
    {
        const std::array<std::string, 7> candidates
            = getFonvCreatureHitReactionCandidates("MESHES\\CREATURES\\NVGECKO\\");
        const std::vector<std::string> available(candidates.begin(), candidates.end());
        const auto exists = [&available](std::string_view path) {
            return std::find(available.begin(), available.end(), path) != available.end();
        };

        EXPECT_EQ(candidates.front(), "meshes/creatures/nvgecko/idleanims/hitreaction_torso.kf");
        EXPECT_EQ(resolveFonvCreatureHitReaction("MESHES\\CREATURES\\NVGECKO\\", exists), candidates.front());
    }

    TEST(FalloutHitReactionTest, failsClosedForRavenAndUnauditedRigs)
    {
        const std::vector<std::string> available = {
            "meshes/creatures/nvraven/idleanims/specialidle_flyaway.kf",
            "meshes/creatures/nightstalker/idleanims/hitreaction_torso.kf",
            "meshes/creatures/nightstalker/recoil.kf",
            "meshes/creatures/nvgecko/idleanims/hitreaction_torso.kf",
            "meshes/creatures/radscorpion/recoil.kf",
        };
        const auto exists = [&available](std::string_view path) {
            return std::find(available.begin(), available.end(), path) != available.end();
        };

        EXPECT_TRUE(resolveFonvCreatureHitReaction("meshes\\creatures\\nvraven\\", exists).empty());
        EXPECT_TRUE(resolveFonvCreatureHitReaction("meshes/creatures/nightstalker", exists).empty());
    }

    TEST(FalloutWeaponAnimationTest, resolvesFamilySpecificEquipAndHandGripSelectors)
    {
        EXPECT_EQ(getFonvWeaponAnimationPrefix(3), "1hp");
        EXPECT_EQ(getFonvWeaponAnimationKf(3, "equip"), "meshes/characters/_male/1hpequip.kf");
        EXPECT_EQ(getFonvWeaponAnimationPrefix(5), "2hr");
        EXPECT_EQ(getFonvWeaponAnimationKf(5, "equip"), "meshes/characters/_male/2hrequip.kf");

        const std::optional<unsigned int> defaultGrip = getFonvWeaponHandGripIndex(0xff);
        const std::optional<unsigned int> firstGrip = getFonvWeaponHandGripIndex(0xe6);
        const std::optional<unsigned int> lastGrip = getFonvWeaponHandGripIndex(0xeb);
        ASSERT_TRUE(defaultGrip.has_value());
        ASSERT_TRUE(firstGrip.has_value());
        ASSERT_TRUE(lastGrip.has_value());
        EXPECT_EQ(*defaultGrip, 0u);
        EXPECT_EQ(*firstGrip, 1u);
        EXPECT_EQ(*lastGrip, 6u);
        EXPECT_FALSE(getFonvWeaponHandGripIndex(0xe5).has_value());
        EXPECT_FALSE(getFonvWeaponHandGripIndex(0xec).has_value());
        EXPECT_EQ(getFonvWeaponHandGripKf(3, 0xe6), "meshes/characters/_male/1hphandgrip1.kf");
        EXPECT_EQ(getFonvWeaponHandGripKf(5, 0xeb), "meshes/characters/_male/2hrhandgrip6.kf");
        EXPECT_TRUE(getFonvWeaponHandGripKf(3, 0xff).empty());
    }

    TEST(FalloutWeaponAnimationTest, resolvesExactRetailWeaponActionManifestsFromDnam)
    {
        const std::vector<FonvWeaponActionSource> melee = getFonvWeaponActionManifest(1, 0);
        ASSERT_EQ(melee.size(), 3u);
        EXPECT_EQ(melee[0].mAction, FonvWeaponAction::PrimaryAttack);
        EXPECT_EQ(melee[0].mPath, "meshes/characters/_male/1hmattackright_a.kf");
        EXPECT_EQ(melee[0].mSemanticGroup, "attack2");
        EXPECT_EQ(melee[1].mAction, FonvWeaponAction::Equip);
        EXPECT_EQ(melee[1].mPath, "meshes/characters/_male/1hmequip.kf");
        EXPECT_EQ(melee[1].mSemanticGroup, "equip");
        EXPECT_EQ(melee[2].mAction, FonvWeaponAction::Unequip);
        EXPECT_EQ(melee[2].mPath, "meshes/characters/_male/1hmunequip.kf");
        EXPECT_EQ(melee[2].mSemanticGroup, "unequip");

        // Retail WeapNVAssaultCarbine DNAM is type=6 (2ha), reload=5 (ReloadF/JamF).
        const std::vector<FonvWeaponActionSource> automatic = getFonvWeaponActionManifest(6, 5);
        ASSERT_EQ(automatic.size(), 5u);
        EXPECT_EQ(automatic[0].mAction, FonvWeaponAction::PrimaryAttack);
        EXPECT_EQ(automatic[0].mPath, "meshes/characters/_male/2haattackloop.kf");
        EXPECT_EQ(automatic[0].mSemanticGroup, "attack1");
        EXPECT_EQ(automatic[1].mAction, FonvWeaponAction::Equip);
        EXPECT_EQ(automatic[1].mPath, "meshes/characters/_male/2haequip.kf");
        EXPECT_EQ(automatic[2].mAction, FonvWeaponAction::Reload);
        EXPECT_EQ(automatic[2].mPath, "meshes/characters/_male/2hareloadf.kf");
        EXPECT_EQ(automatic[2].mSemanticGroup, "reload");
        EXPECT_TRUE(automatic[2].mRequired);
        EXPECT_EQ(automatic[3].mAction, FonvWeaponAction::Jam);
        EXPECT_EQ(automatic[3].mPath, "meshes/characters/_male/2hajamf.kf");
        EXPECT_EQ(automatic[3].mSemanticGroup, "jam");
        EXPECT_FALSE(automatic[3].mRequired);
        EXPECT_EQ(automatic[4].mAction, FonvWeaponAction::Unequip);
        EXPECT_EQ(automatic[4].mPath, "meshes/characters/_male/2haunequip.kf");

        const std::vector<FonvWeaponActionSource> mine = getFonvWeaponActionManifest(11, 0);
        ASSERT_EQ(mine.size(), 3u);
        EXPECT_EQ(mine[0].mPath, "meshes/characters/_male/1mdplacemine.kf");
        EXPECT_EQ(mine[0].mSemanticGroup, "attack1");

        EXPECT_EQ(getFonvWeaponReloadAnimationLetter(0), 'a');
        EXPECT_EQ(getFonvWeaponReloadAnimationLetter(18), 's');
        EXPECT_EQ(getFonvWeaponReloadAnimationLetter(19), 'w');
        EXPECT_EQ(getFonvWeaponReloadAnimationLetter(20), 'x');
        EXPECT_EQ(getFonvWeaponReloadAnimationLetter(22), 'z');
        EXPECT_FALSE(getFonvWeaponReloadAnimationLetter(23).has_value());
        EXPECT_TRUE(getFonvWeaponActionManifest(0xff, 0).empty());
    }

    TEST(FalloutWeaponAnimationTest, mapsEveryRetailDnamWeaponTypeWithoutFallback)
    {
        struct ExpectedPrimary
        {
            std::uint8_t mAnimationType;
            std::string_view mPath;
            std::string_view mGroup;
        };
        static constexpr std::array<ExpectedPrimary, 14> expected{ {
            { 0, "meshes/characters/_male/h2hattackright_a.kf", "attack2" },
            { 1, "meshes/characters/_male/1hmattackright_a.kf", "attack2" },
            { 2, "meshes/characters/_male/2hmattackright_a.kf", "attack2" },
            { 3, "meshes/characters/_male/1hpattack3.kf", "attack3" },
            { 4, "meshes/characters/_male/1hpattack3.kf", "attack3" },
            { 5, "meshes/characters/_male/2hrattack3.kf", "attack3" },
            { 6, "meshes/characters/_male/2haattackloop.kf", "attack1" },
            { 7, "meshes/characters/_male/2hrattack3.kf", "attack3" },
            { 8, "meshes/characters/_male/2hhattackloop.kf", "attack1" },
            { 9, "meshes/characters/_male/2hlattack3.kf", "attack3" },
            { 10, "meshes/characters/_male/1gtattackthrow.kf", "attack1" },
            { 11, "meshes/characters/_male/1mdplacemine.kf", "attack1" },
            { 12, "meshes/characters/_male/1lmplacemine.kf", "attack1" },
            { 13, "meshes/characters/_male/1gtattackthrow.kf", "attack1" },
        } };

        for (const ExpectedPrimary& expectedAction : expected)
        {
            const std::optional<FonvWeaponActionSource> primary = getFonvWeaponActionSource(
                expectedAction.mAnimationType, 0, FonvWeaponAction::PrimaryAttack);
            ASSERT_TRUE(primary.has_value()) << static_cast<unsigned int>(expectedAction.mAnimationType);
            EXPECT_EQ(primary->mAction, FonvWeaponAction::PrimaryAttack);
            EXPECT_EQ(primary->mPath, expectedAction.mPath);
            EXPECT_EQ(primary->mSemanticGroup, expectedAction.mGroup);

            const std::optional<FonvWeaponActionSource> equip
                = getFonvWeaponActionSource(expectedAction.mAnimationType, 0, FonvWeaponAction::Equip);
            const std::optional<FonvWeaponActionSource> unequip
                = getFonvWeaponActionSource(expectedAction.mAnimationType, 0, FonvWeaponAction::Unequip);
            ASSERT_TRUE(equip.has_value());
            ASSERT_TRUE(unequip.has_value());
            EXPECT_EQ(equip->mSemanticGroup, "equip");
            EXPECT_EQ(unequip->mSemanticGroup, "unequip");
        }

        EXPECT_FALSE(getFonvWeaponActionSource(0xff, 0, FonvWeaponAction::PrimaryAttack).has_value());
        EXPECT_FALSE(getFonvWeaponActionSource(1, 0, FonvWeaponAction::Reload).has_value());
        EXPECT_FALSE(getFonvWeaponActionSource(3, 23, FonvWeaponAction::Reload).has_value());
    }

    TEST(FalloutWeaponAnimationTest, preservesMeasuredRetailHolsterContractsBitExactlyWithoutFallback)
    {
        struct ExpectedContract
        {
            std::uint8_t mAnimationType;
            std::uint32_t mSourceForm;
            std::string_view mParentName;
            std::array<std::uint32_t, 9> mRotationBits;
            std::array<std::uint32_t, 3> mTranslationBits;
            std::uint32_t mScaleBits;
        };
        static constexpr std::array<ExpectedContract, 3> expected{ {
            { 3, 0x000e3778, "Bip01 Pelvis",
                { 3209060608, 3206663839, 3137485292, 3206659353, 1061569791, 1023527333,
                    3164042662, 1020489499, 3212828405 },
                { 1071143469, 3223221091, 3245862991 }, 1065353218 },
            { 5, 0x0007ea24, "Bip01 Spine2",
                { 3210826934, 3203525720, 1026989424, 3189668151, 1045015346, 3212302068,
                    1055242061, 3210471966, 3195822950 },
                { 1100293858, 3239811472, 3235687431 }, 1065353217 },
            { 6, 0x000e9c3b, "Bip01 Spine2",
                { 1065318487, 1027137190, 3174802633, 3175280776, 1025547984, 3212804936,
                    3174115547, 1065323204, 1026102809 },
                { 1088343032, 1074830907, 1068977381 }, 1065353218 },
        } };

        for (const ExpectedContract& expectedContract : expected)
        {
            const FonvRetailHolsterContract* actual
                = getFonvRetailHolsterContract(expectedContract.mAnimationType);
            ASSERT_NE(actual, nullptr) << static_cast<unsigned int>(expectedContract.mAnimationType);
            EXPECT_EQ(actual->mAnimationType, expectedContract.mAnimationType);
            EXPECT_EQ(actual->mSourceForm, expectedContract.mSourceForm);
            EXPECT_EQ(actual->mEvaluatedSlot, 5u);
            EXPECT_EQ(actual->mEvaluatedState, 0u);
            EXPECT_EQ(actual->mFrameName, "Weapon");
            EXPECT_EQ(actual->mParentName, expectedContract.mParentName);
            EXPECT_EQ(actual->mRotationBits, expectedContract.mRotationBits);
            EXPECT_EQ(actual->mTranslationBits, expectedContract.mTranslationBits);
            EXPECT_EQ(actual->mScaleBits, expectedContract.mScaleBits);
            for (std::uint32_t bits : actual->mRotationBits)
                EXPECT_EQ(std::bit_cast<std::uint32_t>(std::bit_cast<float>(bits)), bits);
            for (std::uint32_t bits : actual->mTranslationBits)
                EXPECT_EQ(std::bit_cast<std::uint32_t>(std::bit_cast<float>(bits)), bits);
            EXPECT_EQ(std::bit_cast<std::uint32_t>(std::bit_cast<float>(actual->mScaleBits)), actual->mScaleBits);
        }

        for (std::uint8_t animationType : { 0, 1, 2, 4, 7, 8, 9, 10, 11, 12, 13, 0xff })
            EXPECT_EQ(getFonvRetailHolsterContract(animationType), nullptr);
    }

    TEST(FalloutWeaponAnimationTest, preservesEveryDnamTypeAcrossTheMechanicsEncoding)
    {
        for (std::uint8_t animationType = 0; animationType < 14; ++animationType)
        {
            const std::optional<int> weaponType = MWMechanics::getFalloutWeaponType(animationType);
            ASSERT_TRUE(weaponType.has_value());
            EXPECT_TRUE(MWMechanics::isFalloutWeaponType(*weaponType));
            const std::optional<std::uint8_t> decoded = MWMechanics::getFalloutWeaponAnimationType(*weaponType);
            ASSERT_TRUE(decoded.has_value());
            EXPECT_EQ(*decoded, animationType);
        }

        EXPECT_FALSE(MWMechanics::getFalloutWeaponType(14).has_value());
        EXPECT_FALSE(MWMechanics::getFalloutWeaponAnimationType(ESM::Weapon::HandToHand).has_value());
        EXPECT_FALSE(MWMechanics::getFalloutWeaponAnimationType(0x10e).has_value());
    }

    TEST(FalloutWeaponAnimationTest, rejectsSameNamedActionsFromAnotherWeaponFamily)
    {
        const std::optional<FonvWeaponActionSource> pistol
            = getFonvWeaponActionSource(3, 0, FonvWeaponAction::PrimaryAttack);
        const std::optional<FonvWeaponActionSource> rifle
            = getFonvWeaponActionSource(5, 0, FonvWeaponAction::PrimaryAttack);
        ASSERT_TRUE(pistol.has_value());
        ASSERT_TRUE(rifle.has_value());
        ASSERT_EQ(pistol->mSemanticGroup, rifle->mSemanticGroup);
        ASSERT_NE(pistol->mPath, rifle->mPath);

        EXPECT_TRUE(matchesFonvWeaponActionSource(*pistol, "attack3", pistol->mPath, pistol->mPath));
        EXPECT_FALSE(matchesFonvWeaponActionSource(*pistol, "attack3", rifle->mPath, pistol->mPath));
        EXPECT_FALSE(matchesFonvWeaponActionSource(*pistol, "attack2", pistol->mPath, pistol->mPath));
    }

    TEST(FalloutWeaponAnimationTest, derivesPowerArmorSiblingsWithoutHeuristics)
    {
        EXPECT_EQ(getFonvPowerArmorAnimationKf("mtidle.kf"), "pamtidle.kf");
        EXPECT_EQ(getFonvPowerArmorAnimationKf("meshes/characters/_male/2hraim.kf"),
            "meshes/characters/_male/pa2hraim.kf");
        EXPECT_EQ(getFonvPowerArmorAnimationKf("meshes\\characters\\_male\\2hrequip.kf"),
            "meshes\\characters\\_male\\pa2hrequip.kf");
        EXPECT_EQ(getFonvPowerArmorAnimationKf("meshes/characters/_male/pamtidle.kf"),
            "meshes/characters/_male/pamtidle.kf");

        static_assert(FonvPowerArmorGeneralFlag == ESM4::Armor::FO3_PowerArmor);
        EXPECT_TRUE(hasFonvPowerArmorGeneralFlag(
            ESM4::Armor::TYPE_FO3 | ESM4::Armor::FO3_HeavyArmor | ESM4::Armor::FO3_PowerArmor));
        EXPECT_FALSE(hasFonvPowerArmorGeneralFlag(ESM4::Armor::TYPE_FO3 | ESM4::Armor::FO3_HeavyArmor));
    }

    TEST(FalloutWeaponAnimationTest, resolvesEveryPowerArmorCandidateBeforeAnyGenericFallback)
    {
        const std::vector<std::string> candidates{ "meshes/characters/_male/locomotion/2hrforward.kf",
            "meshes/characters/_male/locomotion/male/mtforward.kf" };
        const std::vector<std::string> available{ "meshes/characters/_male/locomotion/2hrforward.kf",
            "meshes/characters/_male/locomotion/male/pamtforward.kf" };
        const auto exists = [&available](std::string_view path) {
            return std::find(available.begin(), available.end(), path) != available.end();
        };

        const FonvAnimationFamilyResolution normal = resolveFonvAnimationFamily(candidates, false, exists);
        EXPECT_EQ(normal.mPath, candidates[0]);
        EXPECT_EQ(normal.mSelection, FonvAnimationFamilySelection::Generic);

        const FonvAnimationFamilyResolution armored = resolveFonvAnimationFamily(candidates, true, exists);
        EXPECT_EQ(armored.mPath, available[1]);
        EXPECT_EQ(armored.mSelection, FonvAnimationFamilySelection::PowerArmor);
    }

    TEST(FalloutWeaponAnimationTest, resolvesPowerArmorAimActionsAndExplicitGenericFallbacks)
    {
        const std::vector<std::string> available{ "meshes/characters/_male/pa2hraim.kf",
            "meshes/characters/_male/pa2hrequip.kf", "meshes/characters/_male/pa2hrunequip.kf",
            "meshes/characters/_male/pa2haattackloop.kf", "meshes/characters/_male/2hrattack3.kf" };
        const auto exists = [&available](std::string_view path) {
            return std::find(available.begin(), available.end(), path) != available.end();
        };

        for (const std::string& path : { "meshes/characters/_male/2hraim.kf",
                 "meshes/characters/_male/2hrequip.kf", "meshes/characters/_male/2hrunequip.kf",
                 "meshes/characters/_male/2haattackloop.kf" })
        {
            const FonvAnimationFamilyResolution result = resolveFonvAnimationFamily({ path }, true, exists);
            ASSERT_EQ(result.mSelection, FonvAnimationFamilySelection::PowerArmor) << path;
            EXPECT_EQ(result.mPath, getFonvPowerArmorAnimationKf(path));
        }

        const FonvAnimationFamilyResolution fallback
            = resolveFonvAnimationFamily({ "meshes/characters/_male/2hrattack3.kf" }, true, exists);
        EXPECT_EQ(fallback.mPath, "meshes/characters/_male/2hrattack3.kf");
        EXPECT_EQ(fallback.mSelection, FonvAnimationFamilySelection::GenericFallback);

        const FonvAnimationFamilyResolution missing
            = resolveFonvAnimationFamily({ "meshes/characters/_male/2hrreloadz.kf" }, true, exists);
        EXPECT_TRUE(missing.mPath.empty());
        EXPECT_EQ(missing.mSelection, FonvAnimationFamilySelection::Missing);
    }

    TEST(FalloutWeaponAnimationTest, validatesWeaponActionsAgainstTheResolvedArmorFamily)
    {
        const std::optional<FonvWeaponActionSource> rifle
            = getFonvWeaponActionSource(5, 0, FonvWeaponAction::Equip);
        ASSERT_TRUE(rifle.has_value());
        const std::string powerArmorPath = getFonvPowerArmorAnimationKf(rifle->mPath);

        EXPECT_TRUE(matchesFonvWeaponActionSource(*rifle, "equip", rifle->mPath, rifle->mPath));
        EXPECT_FALSE(matchesFonvWeaponActionSource(*rifle, "equip", powerArmorPath, rifle->mPath));
        EXPECT_TRUE(matchesFonvWeaponActionSource(*rifle, "equip", rifle->mPath, rifle->mPath));
        EXPECT_TRUE(matchesFonvWeaponActionSource(*rifle, "equip", powerArmorPath, powerArmorPath));
        EXPECT_FALSE(matchesFonvWeaponActionSource(
            *rifle, "equip", "meshes/characters/_male/pa2haequip.kf", powerArmorPath));
        EXPECT_FALSE(matchesFonvWeaponActionSource(*rifle, "attack3", powerArmorPath, powerArmorPath));
        EXPECT_FALSE(matchesFonvWeaponActionSource(*rifle, "equip", powerArmorPath, {}));
    }

    TEST(FalloutWeaponAnimationTest, preservesPreFillSemanticsWhenEarlierSourcesExposeMoreGroups)
    {
        std::vector<std::string> liveSemantics{ "walkforward" };
        const FonvAnimationSemanticSnapshot baseline({ "walkforward", "runforward" },
            [&liveSemantics](std::string_view semantic) {
                return std::find(liveSemantics.begin(), liveSemantics.end(), semantic) != liveSemantics.end();
            });

        // Loading mtforward exposes runforward too, but it was not authoritative at the KFFZ/IDLE baseline.
        liveSemantics.emplace_back("runforward");
        EXPECT_TRUE(baseline.wasPresent("walkforward"));
        EXPECT_FALSE(baseline.wasPresent("runforward"));

        const std::vector<std::string> available{ "meshes/characters/_male/locomotion/2hrfastforward.kf",
            "meshes/characters/_male/locomotion/male/mtforward.kf" };
        const FonvAnimationFamilyResolution run = resolveFonvAnimationFamily(available, false,
            [&available](std::string_view path) {
                return std::find(available.begin(), available.end(), path) != available.end();
            });
        EXPECT_EQ(run.mPath, "meshes/characters/_male/locomotion/2hrfastforward.kf");
    }

    TEST(FalloutWeaponAnimationTest, routesEverySyntheticFalloutTypeThroughTheExactStateMachine)
    {
        for (std::uint8_t animationType = 0; animationType < 14; ++animationType)
        {
            const int weaponType = *MWMechanics::getFalloutWeaponType(animationType);
            EXPECT_TRUE(MWMechanics::shouldUseFalloutWeaponState(weaponType, ESM::Weapon::None));
            EXPECT_TRUE(MWMechanics::shouldUseFalloutWeaponState(ESM::Weapon::None, weaponType));
        }
        EXPECT_FALSE(MWMechanics::shouldUseFalloutWeaponState(
            ESM::Weapon::LongBladeOneHand, ESM::Weapon::None));
    }

    TEST(FalloutWeaponAnimationTest, transitionsWhenWeaponIdentityChangesWithinOneDnamFamily)
    {
        const int pistol = *MWMechanics::getFalloutWeaponType(3);
        const int rifle = *MWMechanics::getFalloutWeaponType(5);
        EXPECT_FALSE(MWMechanics::shouldTransitionFalloutWeaponState(pistol, pistol, false));
        EXPECT_TRUE(MWMechanics::shouldTransitionFalloutWeaponState(pistol, pistol, true));
        EXPECT_TRUE(MWMechanics::shouldTransitionFalloutWeaponState(rifle, pistol, false));
    }

    TEST(FalloutWeaponAnimationTest, distinguishesCompletionFromInterruptedActionState)
    {
        EXPECT_EQ(getFonvWeaponActionProgress(true, 0.f), FonvWeaponActionProgress::Running);
        EXPECT_EQ(getFonvWeaponActionProgress(true, 0.999f), FonvWeaponActionProgress::Running);
        EXPECT_EQ(getFonvWeaponActionProgress(true, 1.f), FonvWeaponActionProgress::Completed);
        EXPECT_EQ(getFonvWeaponActionProgress(false, 1.f), FonvWeaponActionProgress::Interrupted);
    }

    TEST(FalloutWeaponAnimationTest, blocksSemanticTransitionsDuringHitStateRecovery)
    {
        EXPECT_TRUE(canAdvanceFonvWeaponState(false, false, false));
        EXPECT_FALSE(canAdvanceFonvWeaponState(true, false, false));
        EXPECT_FALSE(canAdvanceFonvWeaponState(false, true, false));
        EXPECT_FALSE(canAdvanceFonvWeaponState(false, false, true));
    }

    TEST(FalloutWeaponAnimationTest, explicitSemanticAliasIncludesLegacyPlayerRenderer)
    {
        EXPECT_TRUE(shouldSynthesizeFonvSemanticAlias(true, {}));
        EXPECT_TRUE(shouldSynthesizeFonvSemanticAlias(false, "attack3"));
        EXPECT_FALSE(shouldSynthesizeFonvSemanticAlias(false, {}));
    }

    TEST(FalloutWeaponAnimationTest, distinguishesRequiredSkeletonTargetsFromOptionalVisualTargets)
    {
        EXPECT_TRUE(isFonvRequiredSkeletonControllerTarget("bip01"));
        EXPECT_TRUE(isFonvRequiredSkeletonControllerTarget("bip01 l hand"));
        EXPECT_TRUE(isFonvRequiredSkeletonControllerTarget("bip01_laser01"));
        EXPECT_TRUE(isFonvRequiredSkeletonControllerTarget("weapon"));

        EXPECT_FALSE(isFonvRequiredSkeletonControllerTarget("##357trigger"));
        EXPECT_FALSE(isFonvRequiredSkeletonControllerTarget("eyesoneblue"));
        EXPECT_FALSE(isFonvRequiredSkeletonControllerTarget("projectilenode_sonicbark"));
        EXPECT_FALSE(isFonvRequiredSkeletonControllerTarget("screenstatic:0"));
        EXPECT_FALSE(isFonvRequiredSkeletonControllerTarget("voicebox_talk:0"));
        EXPECT_FALSE(isFonvRequiredSkeletonControllerTarget("assualtcarbine"));
        EXPECT_FALSE(isFonvRequiredSkeletonControllerTarget("dome"));
        EXPECT_FALSE(isFonvRequiredSkeletonControllerTarget("buzzsawblad"));

        // Missing targets are fail-closed unless retail evidence placed them in
        // the explicit optional-visual set.
        EXPECT_TRUE(isFonvRequiredSkeletonControllerTarget("unknown_future_rig_target"));
    }

    TEST(FalloutWeaponAnimationTest, classifiesSecuritronSpin1AsAnExactAuthoredDuplicate)
    {
        EXPECT_EQ(getFonvExactDuplicateControllerTarget("bip01 spin1"), "bip01 spine1");
        EXPECT_TRUE(getFonvExactDuplicateControllerTarget("bip01 spin01").empty());
        EXPECT_TRUE(getFonvExactDuplicateControllerTarget("bip01 spine1").empty());
    }

    TEST(FalloutWeaponAnimationTest, handGripOverlayPreservesTwoHandAimWeaponController)
    {
        SceneUtil::KeyframeHolder twoHandAim;
        osg::ref_ptr<NifOsg::KeyframeController> weapon = new NifOsg::KeyframeController;
        osg::ref_ptr<NifOsg::KeyframeController> originalFinger = new NifOsg::KeyframeController;
        twoHandAim.mKeyframeControllers.emplace("Weapon", weapon);
        twoHandAim.mKeyframeControllers.emplace("Bip01 R Finger11", originalFinger);

        SceneUtil::KeyframeHolder handGrip;
        osg::ref_ptr<NifOsg::KeyframeController> gripFinger = new NifOsg::KeyframeController;
        handGrip.mKeyframeControllers.emplace("bip01 r finger11", gripFinger);

        const osg::ref_ptr<SceneUtil::KeyframeHolder> merged
            = mergeFonvWeaponControllerOverlay(twoHandAim, handGrip);

        ASSERT_NE(merged, nullptr);
        ASSERT_EQ(merged->mKeyframeControllers.size(), 2u);
        const auto weaponTrack = merged->mKeyframeControllers.find("Weapon");
        ASSERT_NE(weaponTrack, merged->mKeyframeControllers.end());
        EXPECT_EQ(weaponTrack->second.get(), weapon.get());
        const auto gripTrack = merged->mKeyframeControllers.find("bip01 r finger11");
        ASSERT_NE(gripTrack, merged->mKeyframeControllers.end());
        EXPECT_EQ(gripTrack->second.get(), gripFinger.get());
    }

    TEST(FalloutWeaponAnimationTest, samplesRetailOneHandAndTwoHandEquipEndpoints)
    {
        const osg::Vec3f oneHandEndpoint(8.67439556f, 2.21316051f, 1.06902659f);
        const osg::Quat oneHandRotation(0.699569225f, 0.060529605f, 0.034792986f, 0.711145937f);
        SceneUtil::KeyframeHolder oneHandEquip;
        oneHandEquip.mKeyframeControllers.emplace("Weapon",
            makeEndpointController(osg::Vec3f(6.171f, 1.989f, 0.759f), oneHandEndpoint, osg::Quat(),
                oneHandRotation));

        const osg::Vec3f twoHandEndpoint(7.87627888f, 2.19404268f, -0.525382161f);
        const osg::Quat twoHandRotation(
            -0.669910542f, 0.193622099f, -0.203717153f, 0.687189690f);
        SceneUtil::KeyframeHolder twoHandEquip;
        twoHandEquip.mKeyframeControllers.emplace("Weapon",
            makeEndpointController(osg::Vec3f(6.171f, 1.989f, 0.759f), twoHandEndpoint, osg::Quat(),
                twoHandRotation));

        const auto oneHand = sampleFonvWeaponAttachmentEndpoint(oneHandEquip);
        const auto twoHand = sampleFonvWeaponAttachmentEndpoint(twoHandEquip);
        ASSERT_TRUE(oneHand.has_value());
        ASSERT_TRUE(twoHand.has_value());
        expectEndpoint(*oneHand, oneHandEndpoint, oneHandRotation);
        expectEndpoint(*twoHand, twoHandEndpoint, twoHandRotation);
    }
}
