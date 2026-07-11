#include <components/nif/controller.hpp>
#include <components/nif/data.hpp>
#include <components/nifosg/controller.hpp>
#include <components/sceneutil/controller.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

namespace
{
    class ConstantControllerSource final : public SceneUtil::ControllerSource
    {
    public:
        explicit ConstantControllerSource(float value)
            : mValue(value)
        {
        }

        float getValue(osg::NodeVisitor*) override { return mValue; }

    private:
        float mValue;
    };

    TEST(NifOsgControllerTest, shouldDecodeCompactBSplineQuaternionAsWXYZ)
    {
        Nif::NiBSplineData splineData;
        Nif::NiBSplineBasisData basisData;
        Nif::NiBSplineCompTransformInterpolator interpolator;

        basisData.mNumControlPoints = 4;
        interpolator.recType = Nif::RC_NiBSplineCompTransformInterpolator;
        interpolator.mStartTime = 0.f;
        interpolator.mStopTime = 1.f;
        interpolator.mSplineData = Nif::NiBSplineDataPtr(&splineData);
        interpolator.mBasisData = Nif::NiBSplineBasisDataPtr(&basisData);
        interpolator.mTranslationHandle = std::numeric_limits<std::uint32_t>::max();
        interpolator.mRotationHandle = 0;
        interpolator.mScaleHandle = std::numeric_limits<std::uint32_t>::max();
        interpolator.mRotationOffset = 0.f;
        interpolator.mRotationHalfRange = 1.f;

        // Four identical identity quaternion control points serialized as W, X, Y, Z.
        splineData.mCompactControlPoints = {
            32767, 0, 0, 0,
            32767, 0, 0, 0,
            32767, 0, 0, 0,
            32767, 0, 0, 0,
        };

        NifOsg::KeyframeController controller(&interpolator);
        controller.setSource(std::make_shared<ConstantControllerSource>(0.5f));
        const auto transform = controller.getCurrentTransformation(nullptr);

        ASSERT_TRUE(transform.mRotation.has_value());
        EXPECT_NEAR(transform.mRotation->x(), 0.f, 1e-6f);
        EXPECT_NEAR(transform.mRotation->y(), 0.f, 1e-6f);
        EXPECT_NEAR(transform.mRotation->z(), 0.f, 1e-6f);
        EXPECT_NEAR(transform.mRotation->w(), 1.f, 1e-6f);
    }

    TEST(NifOsgControllerTest, shouldUseClampedBSplineEndpoints)
    {
        Nif::NiBSplineData splineData;
        Nif::NiBSplineBasisData basisData;
        Nif::NiBSplineCompTransformInterpolator interpolator;

        basisData.mNumControlPoints = 5;
        interpolator.recType = Nif::RC_NiBSplineCompTransformInterpolator;
        interpolator.mStartTime = 0.f;
        interpolator.mStopTime = 1.f;
        interpolator.mSplineData = Nif::NiBSplineDataPtr(&splineData);
        interpolator.mBasisData = Nif::NiBSplineBasisDataPtr(&basisData);
        interpolator.mTranslationHandle = 0;
        interpolator.mRotationHandle = std::numeric_limits<std::uint16_t>::max();
        interpolator.mScaleHandle = std::numeric_limits<std::uint16_t>::max();
        interpolator.mTranslationOffset = 0.f;
        interpolator.mTranslationHalfRange = 32767.f;
        splineData.mCompactControlPoints = {
            1, 2, 3,
            10, 20, 30,
            100, 200, 300,
            1000, 2000, 3000,
            10000, 20000, 30000,
        };

        NifOsg::KeyframeController controller(&interpolator);
        EXPECT_EQ(controller.getTranslation(0.f), osg::Vec3f(1.f, 2.f, 3.f));
        EXPECT_EQ(controller.getTranslation(0.5f), osg::Vec3f(302.5f, 605.f, 907.5f));
        EXPECT_EQ(controller.getTranslation(1.f), osg::Vec3f(10000.f, 20000.f, 30000.f));

        auto source = std::make_shared<ConstantControllerSource>(0.f);
        controller.setSource(source);
        auto transform = controller.getCurrentTransformation(nullptr);
        ASSERT_TRUE(transform.mTranslation.has_value());
        EXPECT_EQ(*transform.mTranslation, osg::Vec3f(1.f, 2.f, 3.f));

        source = std::make_shared<ConstantControllerSource>(0.5f);
        controller.setSource(source);
        transform = controller.getCurrentTransformation(nullptr);
        ASSERT_TRUE(transform.mTranslation.has_value());
        EXPECT_EQ(*transform.mTranslation, osg::Vec3f(302.5f, 605.f, 907.5f));

        source = std::make_shared<ConstantControllerSource>(1.f);
        controller.setSource(source);
        transform = controller.getCurrentTransformation(nullptr);
        ASSERT_TRUE(transform.mTranslation.has_value());
        EXPECT_EQ(*transform.mTranslation, osg::Vec3f(10000.f, 20000.f, 30000.f));
    }

    TEST(NifOsgControllerTest, shouldApplyConstantTransformInterpolatorDefaults)
    {
        Nif::NiTransformInterpolator interpolator;
        interpolator.recType = Nif::RC_NiTransformInterpolator;
        interpolator.mData = Nif::NiKeyframeDataPtr(nullptr);
        interpolator.mDefaultValue.mTranslation = osg::Vec3f(7.974f, 3.001f, -1.097f);
        interpolator.mDefaultValue.mRotation = osg::Quat(0.5f, osg::Vec3f(0.f, 0.f, 1.f));
        interpolator.mDefaultValue.mScale = 1.25f;

        NifOsg::KeyframeController controller(&interpolator);
        controller.setSource(std::make_shared<ConstantControllerSource>(0.5f));
        const auto transform = controller.getCurrentTransformation(nullptr);

        ASSERT_TRUE(transform.mTranslation.has_value());
        ASSERT_TRUE(transform.mRotation.has_value());
        ASSERT_TRUE(transform.mScale.has_value());
        EXPECT_NEAR(transform.mTranslation->x(), interpolator.mDefaultValue.mTranslation.x(), 1e-6f);
        EXPECT_NEAR(transform.mTranslation->y(), interpolator.mDefaultValue.mTranslation.y(), 1e-6f);
        EXPECT_NEAR(transform.mTranslation->z(), interpolator.mDefaultValue.mTranslation.z(), 1e-6f);
        EXPECT_NEAR(transform.mRotation->x(), interpolator.mDefaultValue.mRotation.x(), 1e-6f);
        EXPECT_NEAR(transform.mRotation->y(), interpolator.mDefaultValue.mRotation.y(), 1e-6f);
        EXPECT_NEAR(transform.mRotation->z(), interpolator.mDefaultValue.mRotation.z(), 1e-6f);
        EXPECT_NEAR(transform.mRotation->w(), interpolator.mDefaultValue.mRotation.w(), 1e-6f);
        EXPECT_FLOAT_EQ(*transform.mScale, interpolator.mDefaultValue.mScale);
    }

    TEST(NifOsgControllerTest, shouldPreferAuthoredXYZRotationKeysOverInterpolatorDefault)
    {
        Nif::NiKeyframeData keyData;
        keyData.mXRotations = std::make_shared<Nif::FloatKeyMap>();
        keyData.mXRotations->mInterpolationType = Nif::InterpolationType_Linear;
        keyData.mXRotations->mKeys = {
            { 0.f, Nif::KeyT<float>{ 0.f, 0.f, 0.f } },
            { 1.f, Nif::KeyT<float>{ 1.f, 0.f, 0.f } },
        };

        Nif::NiTransformInterpolator interpolator;
        interpolator.recType = Nif::RC_NiTransformInterpolator;
        interpolator.mData = Nif::NiKeyframeDataPtr(&keyData);
        interpolator.mDefaultValue.mRotation = osg::Quat();

        NifOsg::KeyframeController controller(&interpolator);
        controller.setSource(std::make_shared<ConstantControllerSource>(0.5f));
        const auto transform = controller.getCurrentTransformation(nullptr);

        ASSERT_TRUE(transform.mRotation.has_value());
        EXPECT_NEAR(transform.mRotation->x(), std::sin(0.25f), 1e-6f);
        EXPECT_NEAR(transform.mRotation->w(), std::cos(0.25f), 1e-6f);
    }

    TEST(NifOsgControllerTest, shouldApplyBSplineDefaultsForMissingChannels)
    {
        Nif::NiBSplineData splineData;
        Nif::NiBSplineBasisData basisData;
        Nif::NiBSplineTransformInterpolator interpolator;

        basisData.mNumControlPoints = 4;
        interpolator.recType = Nif::RC_NiBSplineTransformInterpolator;
        interpolator.mStartTime = 0.f;
        interpolator.mStopTime = 1.f;
        interpolator.mSplineData = Nif::NiBSplineDataPtr(&splineData);
        interpolator.mBasisData = Nif::NiBSplineBasisDataPtr(&basisData);
        interpolator.mTranslationHandle = std::numeric_limits<std::uint32_t>::max();
        interpolator.mRotationHandle = std::numeric_limits<std::uint32_t>::max();
        interpolator.mScaleHandle = std::numeric_limits<std::uint32_t>::max();
        interpolator.mValue.mTranslation = osg::Vec3f(1.f, 2.f, 3.f);
        interpolator.mValue.mRotation = osg::Quat(0.25f, osg::Vec3f(1.f, 0.f, 0.f));
        interpolator.mValue.mScale = 0.75f;

        NifOsg::KeyframeController controller(&interpolator);
        controller.setSource(std::make_shared<ConstantControllerSource>(0.5f));
        const auto transform = controller.getCurrentTransformation(nullptr);

        ASSERT_TRUE(transform.mTranslation.has_value());
        ASSERT_TRUE(transform.mRotation.has_value());
        ASSERT_TRUE(transform.mScale.has_value());
        EXPECT_NEAR(transform.mTranslation->x(), interpolator.mValue.mTranslation.x(), 1e-6f);
        EXPECT_NEAR(transform.mTranslation->y(), interpolator.mValue.mTranslation.y(), 1e-6f);
        EXPECT_NEAR(transform.mTranslation->z(), interpolator.mValue.mTranslation.z(), 1e-6f);
        EXPECT_NEAR(transform.mRotation->x(), interpolator.mValue.mRotation.x(), 1e-6f);
        EXPECT_NEAR(transform.mRotation->y(), interpolator.mValue.mRotation.y(), 1e-6f);
        EXPECT_NEAR(transform.mRotation->z(), interpolator.mValue.mRotation.z(), 1e-6f);
        EXPECT_NEAR(transform.mRotation->w(), interpolator.mValue.mRotation.w(), 1e-6f);
        EXPECT_FLOAT_EQ(*transform.mScale, interpolator.mValue.mScale);
    }
}
