#include <gtest/gtest.h>

#include "apps/openmw/mwrender/falloutweaponanimation.hpp"

#include <components/nif/controller.hpp>
#include <components/nif/data.hpp>
#include <components/nifosg/controller.hpp>

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
