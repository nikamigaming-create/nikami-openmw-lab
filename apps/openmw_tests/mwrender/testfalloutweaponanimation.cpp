#include <gtest/gtest.h>

#include "apps/openmw/mwrender/falloutanimationtargets.hpp"
#include "apps/openmw/mwrender/falloutweaponanimation.hpp"
#include "apps/openmw/mwmechanics/weapontype.hpp"

#include <components/nif/controller.hpp>
#include <components/nif/data.hpp>
#include <components/nifosg/controller.hpp>
#include <components/esm3/loadweap.hpp>

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

        EXPECT_TRUE(matchesFonvWeaponActionSource(*pistol, "attack3", pistol->mPath));
        EXPECT_FALSE(matchesFonvWeaponActionSource(*pistol, "attack3", rifle->mPath));
        EXPECT_FALSE(matchesFonvWeaponActionSource(*pistol, "attack2", pistol->mPath));
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
