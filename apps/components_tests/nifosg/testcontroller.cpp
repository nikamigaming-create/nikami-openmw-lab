#include <components/nif/controller.hpp>
#include <components/nif/data.hpp>
#include <components/nifosg/controller.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/statesetupdater.hpp>

#include <gtest/gtest.h>

#include <osg/Material>
#include <osg/StateSet>
#include <osg/TexMat>

#include <array>
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

    class TestEmbeddedStateSetUpdater final : public SceneUtil::StateSetUpdater
    {
    public:
        TestEmbeddedStateSetUpdater()
        {
            mTransform.mOffset = osg::Vec2f();
            mTransform.mScale = osg::Vec2f(1.f, 1.f);
            mTransform.mRotation = 0.f;
            mTransform.mTransformMethod = Nif::NiTextureTransform::Method::Max;
            mTransform.mOrigin = osg::Vec2f(0.5f, 0.5f);
        }

        explicit TestEmbeddedStateSetUpdater(const Nif::NiTextureTransform& transform)
            : mTransform(transform)
        {
        }

        TestEmbeddedStateSetUpdater(const TestEmbeddedStateSetUpdater& copy, const osg::CopyOp& copyop)
            : SceneUtil::StateSetUpdater(copy, copyop)
            , mTransform(copy.mTransform)
        {
        }

        META_Object(Test, TestEmbeddedStateSetUpdater)

        void setDefaults(osg::StateSet* stateset) override
        {
            osg::ref_ptr<osg::Material> material = new osg::Material;
            material->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.01f, 0.02f, 0.03f, 1.f));
            stateset->setAttributeAndModes(material, osg::StateAttribute::ON);
            NifOsg::setTextureTransformDefaults(stateset, 0, mTransform);
            osg::ref_ptr<osg::TexMat> texMat = new osg::TexMat;
            texMat->setMatrix(NifOsg::makeTextureTransformMatrix(mTransform));
            stateset->setTextureAttributeAndModes(0, texMat, osg::StateAttribute::ON);
        }

        void apply(osg::StateSet*, osg::NodeVisitor*) override {}

    private:
        Nif::NiTextureTransform mTransform;
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

    TEST(NifOsgControllerTest, shouldComposeAllTextureTransformChannelsAtomically)
    {
        std::array<Nif::NiFloatInterpolator, 5> interpolators;
        std::array<Nif::NiTextureTransformController, 5> nifControllers;
        const std::array<float, 5> values = { 0.1f, 0.2f, 0.f, 2.f, 3.f };
        std::vector<const Nif::NiTextureTransformController*> controllers;

        for (std::size_t i = 0; i < nifControllers.size(); ++i)
        {
            interpolators[i].recType = Nif::RC_NiFloatInterpolator;
            interpolators[i].mDefaultValue = values[i];
            interpolators[i].mData = Nif::NiFloatDataPtr(nullptr);

            Nif::NiTextureTransformController& controller = nifControllers[i];
            controller.mFlags = Nif::NiTimeController::Flag_Active | Nif::NiTimeController::Constant;
            controller.mFrequency = 1.f;
            controller.mPhase = 0.f;
            controller.mTimeStart = 0.f;
            controller.mTimeStop = 1.f;
            controller.mShaderMap = false;
            controller.mTexSlot = Nif::NiTexturingProperty::BaseTexture;
            controller.mTransformMember = static_cast<std::uint32_t>(i);
            controller.mInterpolator = Nif::NiInterpolatorPtr(&interpolators[i]);
            controllers.push_back(&controller);
        }

        Nif::NiTextureTransform authoredBase;
        authoredBase.mOffset = osg::Vec2f(0.f, 0.f);
        authoredBase.mScale = osg::Vec2f(1.f, 1.f);
        authoredBase.mRotation = 0.f;
        authoredBase.mTransformMethod = Nif::NiTextureTransform::Method::Max;
        authoredBase.mOrigin = osg::Vec2f(0.5f, 0.5f);

        NifOsg::TextureTransformController controller(controllers, 0, &authoredBase);
        controller.setSource(std::make_shared<ConstantControllerSource>(0.f));
        osg::ref_ptr<osg::StateSet> stateSet = new osg::StateSet;
        controller.setDefaults(stateSet);
        controller.apply(stateSet, nullptr);

        const auto* texMat = dynamic_cast<const osg::TexMat*>(
            stateSet->getTextureAttribute(0, osg::StateAttribute::TEXMAT));
        ASSERT_NE(texMat, nullptr);
        const osg::Vec3f transformed = osg::Vec3f(0.75f, 0.75f, 0.f) * texMat->getMatrix();
        EXPECT_NEAR(transformed.x(), 0.9f, 1e-6f);
        EXPECT_NEAR(transformed.y(), 1.05f, 1e-6f);
    }

    TEST(NifOsgControllerTest, shouldPreserveAuthoredTextureScaleWhenApplyingRotationChannel)
    {
        Nif::NiFloatInterpolator interpolator;
        interpolator.recType = Nif::RC_NiFloatInterpolator;
        interpolator.mDefaultValue = 0.37f;
        interpolator.mData = Nif::NiFloatDataPtr(nullptr);

        Nif::NiTextureTransformController nifController;
        nifController.mFlags = Nif::NiTimeController::Flag_Active | Nif::NiTimeController::Constant;
        nifController.mFrequency = 1.f;
        nifController.mPhase = 0.f;
        nifController.mTimeStart = 0.f;
        nifController.mTimeStop = 1.f;
        nifController.mShaderMap = false;
        nifController.mTexSlot = Nif::NiTexturingProperty::BaseTexture;
        nifController.mTransformMember = 2;
        nifController.mInterpolator = Nif::NiInterpolatorPtr(&interpolator);

        Nif::NiTextureTransform authoredBase;
        authoredBase.mOffset = osg::Vec2f(0.f, 0.f);
        authoredBase.mScale = osg::Vec2f(0.0536136329f, -0.6919949055f);
        authoredBase.mRotation = 0.f;
        authoredBase.mTransformMethod = Nif::NiTextureTransform::Method::Max;
        authoredBase.mOrigin = osg::Vec2f(0.5f, 0.5f);

        NifOsg::TextureTransformController controller({ &nifController }, 0, &authoredBase);
        controller.setSource(std::make_shared<ConstantControllerSource>(0.f));
        osg::ref_ptr<osg::StateSet> stateSet = new osg::StateSet;
        controller.setDefaults(stateSet);
        controller.apply(stateSet, nullptr);

        const auto* texMat = dynamic_cast<const osg::TexMat*>(
            stateSet->getTextureAttribute(0, osg::StateAttribute::TEXMAT));
        ASSERT_NE(texMat, nullptr);
        const osg::Matrixf& matrix = texMat->getMatrix();
        EXPECT_NEAR(std::hypot(matrix(0, 0), matrix(0, 1)), std::abs(authoredBase.mScale.x()), 1e-6f);
        EXPECT_NEAR(std::hypot(matrix(1, 0), matrix(1, 1)), std::abs(authoredBase.mScale.y()), 1e-6f);
    }

    TEST(NifOsgControllerTest, shouldComposeExternalTransformAndPropertyTracksOverEmbeddedState)
    {
        Nif::NiBSplineData splineData;
        Nif::NiBSplineBasisData basisData;
        Nif::NiBSplineCompPoint3Interpolator emission;
        basisData.mNumControlPoints = 4;
        emission.recType = Nif::RC_NiBSplineCompPoint3Interpolator;
        emission.mStartTime = 0.f;
        emission.mStopTime = 1.f;
        emission.mSplineData = Nif::NiBSplineDataPtr(&splineData);
        emission.mBasisData = Nif::NiBSplineBasisDataPtr(&basisData);
        emission.mHandle = 0;
        emission.mOffset = 0.f;
        emission.mHalfRange = 1.f;
        // Four equal RGB control points make the expected value independent of sample time.
        splineData.mCompactControlPoints = {
            3277, 6553, 9830,
            3277, 6553, 9830,
            3277, 6553, 9830,
            3277, 6553, 9830,
        };

        Nif::NiFloatInterpolator rotation;
        rotation.recType = Nif::RC_NiFloatInterpolator;
        rotation.mDefaultValue = 0.37f;
        rotation.mData = Nif::NiFloatDataPtr(nullptr);

        Nif::NiTransformInterpolator transform;
        transform.recType = Nif::RC_NiTransformInterpolator;
        transform.mData = Nif::NiKeyframeDataPtr(nullptr);
        transform.mDefaultValue.mTranslation = osg::Vec3f(4.f, 5.f, 6.f);
        transform.mDefaultValue.mRotation = osg::Quat();
        transform.mDefaultValue.mScale = 1.f;

        Nif::NiTextureTransform authored;
        authored.mOffset = osg::Vec2f(0.12f, -0.34f);
        authored.mScale = osg::Vec2f(0.0536136329f, -0.6919949055f);
        authored.mRotation = -0.11f;
        authored.mTransformMethod = Nif::NiTextureTransform::Method::Max;
        authored.mOrigin = osg::Vec2f(0.5f, 0.5f);

        osg::ref_ptr<NifOsg::KeyframeController> controller = new NifOsg::KeyframeController(&transform);
        ASSERT_TRUE(controller->addMaterialColorChannel(
            Nif::NiMaterialColorController::TargetColor::Emissive, &emission));
        ASSERT_TRUE(controller->addTextureTransformChannel(
            false, Nif::NiTexturingProperty::BaseTexture, 2, &rotation));
        ASSERT_TRUE(controller->hasPropertyChannels());

        controller->setSource(std::make_shared<ConstantControllerSource>(0.5f));
        osg::ref_ptr<NifOsg::MatrixTransform> node = new NifOsg::MatrixTransform;
        node->addUpdateCallback(new TestEmbeddedStateSetUpdater(authored));
        node->addUpdateCallback(controller);
        osg::NodeVisitor visitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN);
        ASSERT_TRUE(node->getUpdateCallback()->run(node, &visitor));

        EXPECT_EQ(node->getMatrix().getTrans(), transform.mDefaultValue.mTranslation);

        const auto* material
            = dynamic_cast<const osg::Material*>(node->getStateSet()->getAttribute(osg::StateAttribute::MATERIAL));
        ASSERT_NE(material, nullptr);
        const osg::Vec4f color = material->getEmission(osg::Material::FRONT_AND_BACK);
        EXPECT_NEAR(color.r(), 3277.f / 32767.f, 1e-6f);
        EXPECT_NEAR(color.g(), 6553.f / 32767.f, 1e-6f);
        EXPECT_NEAR(color.b(), 9830.f / 32767.f, 1e-6f);

        const auto* texMat = dynamic_cast<const osg::TexMat*>(
            node->getStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXMAT));
        ASSERT_NE(texMat, nullptr);

        Nif::NiTextureTransform expectedTransform = authored;
        expectedTransform.mRotation = rotation.mDefaultValue;
        const osg::Matrixf expectedMatrix = NifOsg::makeTextureTransformMatrix(expectedTransform);
        for (std::size_t i = 0; i < 16; ++i)
            EXPECT_NEAR(texMat->getMatrix().ptr()[i], expectedMatrix.ptr()[i], 1e-6f);
    }

    TEST(NifOsgControllerTest, shouldRestorePropertyOnlyTargetStateWithoutTakingTransformOwnership)
    {
        Nif::NiPoint3Interpolator emission;
        emission.recType = Nif::RC_NiPoint3Interpolator;
        emission.mDefaultValue = osg::Vec3f(0.2f, 0.3f, 0.4f);
        emission.mData = Nif::NiPosDataPtr(nullptr);

        osg::ref_ptr<NifOsg::KeyframeController> controller = new NifOsg::KeyframeController;
        ASSERT_TRUE(controller->addMaterialColorChannel(
            Nif::NiMaterialColorController::TargetColor::Emissive, &emission));
        EXPECT_FALSE(controller->hasTransformChannels());
        EXPECT_TRUE(controller->hasPropertyChannels());
        controller->setSource(std::make_shared<ConstantControllerSource>(0.f));

        osg::ref_ptr<NifOsg::MatrixTransform> node = new NifOsg::MatrixTransform;
        node->setTranslation(osg::Vec3f(9.f, 8.f, 7.f));
        node->addUpdateCallback(controller);
        osg::NodeVisitor visitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN);
        ASSERT_TRUE(node->getUpdateCallback()->run(node, &visitor));
        ASSERT_NE(node->getStateSet(), nullptr);
        EXPECT_EQ(node->getMatrix().getTrans(), osg::Vec3f(9.f, 8.f, 7.f));

        controller->restorePropertyState();
        EXPECT_EQ(node->getStateSet(), nullptr);
    }

    TEST(NifOsgControllerTest, shouldIgnoreInvalidManagerControlledMaterialPlaceholder)
    {
        const float invalidColor = -std::numeric_limits<float>::max();
        Nif::NiBlendPoint3Interpolator interpolator;
        interpolator.recType = Nif::RC_NiBlendPoint3Interpolator;
        interpolator.mFlags = Nif::NiBlendInterpolator::Flag_ManagerControlled;
        interpolator.mValue = osg::Vec3f(invalidColor, invalidColor, invalidColor);

        Nif::NiMaterialColorController nifController;
        nifController.mTargetColor = Nif::NiMaterialColorController::TargetColor::Emissive;
        nifController.mInterpolator = Nif::NiInterpolatorPtr(&interpolator);
        nifController.mData = Nif::NiPosDataPtr(nullptr);

        osg::ref_ptr<osg::Material> baseMaterial = new osg::Material;
        const osg::Vec4f baseEmission(0.1f, 0.2f, 0.3f, 1.f);
        baseMaterial->setEmission(osg::Material::FRONT_AND_BACK, baseEmission);
        NifOsg::MaterialColorController controller(&nifController, baseMaterial);
        controller.setSource(std::make_shared<ConstantControllerSource>(0.f));

        osg::ref_ptr<osg::StateSet> stateSet = new osg::StateSet;
        controller.setDefaults(stateSet);
        controller.apply(stateSet, nullptr);
        const auto* material
            = dynamic_cast<const osg::Material*>(stateSet->getAttribute(osg::StateAttribute::MATERIAL));
        ASSERT_NE(material, nullptr);
        EXPECT_EQ(material->getEmission(osg::Material::FRONT_AND_BACK), baseEmission);
    }
}
