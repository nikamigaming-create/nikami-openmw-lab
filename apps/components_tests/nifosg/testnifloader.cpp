#include "../nif/node.hpp"

#include <components/nif/node.hpp>
#include <components/nif/property.hpp>
#include <components/nif/texture.hpp>
#include <components/nifosg/controller.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/serialize.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/vfs/manager.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/FrameStamp>
#include <osg/Material>
#include <osg/NodeVisitor>
#include <osg/Switch>
#include <osgDB/Registry>

#include <array>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    using namespace testing;
    using namespace NifOsg;
    using namespace Nif::Testing;

    constexpr VFS::Path::NormalizedView testNif("test.nif");
    constexpr VFS::Path::NormalizedView managedMaterialNif(
        "meshes/clutter/open_24hours_sign/test.nif");

    struct BaseNifOsgLoaderTest
    {
        VFS::Manager mVfs;
        Resource::ImageManager mImageManager{ &mVfs, 0 };
        Resource::BgsmFileManager mMaterialManager{ &mVfs, 0 };
        const osgDB::ReaderWriter* mReaderWriter = osgDB::Registry::instance()->getReaderWriterForExtension("osgt");
        osg::ref_ptr<osgDB::Options> mOptions = new osgDB::Options;

        BaseNifOsgLoaderTest()
        {
            SceneUtil::registerSerializers();

            if (mReaderWriter == nullptr)
                throw std::runtime_error("osgt reader writer is not found");

            mOptions->setPluginStringData("fileType", "Ascii");
            mOptions->setPluginStringData("WriteImageHint", "UseExternal");
        }

        std::string serialize(const osg::Node& node) const
        {
            std::stringstream stream;
            mReaderWriter->writeNode(node, stream, mOptions);
            std::string result;
            for (std::string line; std::getline(stream, line);)
            {
                if (line.starts_with('#'))
                    continue;
                line.erase(line.find_last_not_of(" \t\n\r\f\v") + 1);
                result += line;
                result += '\n';
            }
            return result;
        }
    };

    struct NifOsgLoaderTest : Test, BaseNifOsgLoaderTest
    {
    };

    struct DrawableCountVisitor : osg::NodeVisitor
    {
        DrawableCountVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override { traverse(node); }
        void apply(osg::Drawable&) override { ++mCount; }

        unsigned int mCount = 0;
    };

    struct PrimitiveSetCountVisitor : osg::NodeVisitor
    {
        PrimitiveSetCountVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override { traverse(node); }

        void apply(osg::Drawable& drawable) override
        {
            if (const auto* geometry = dynamic_cast<const osg::Geometry*>(&drawable))
                mCount += geometry->getNumPrimitiveSets();
            else if (const auto* rig = dynamic_cast<const SceneUtil::RigGeometry*>(&drawable))
            {
                if (const osg::ref_ptr<osg::Geometry> source = rig->getSourceGeometry())
                    mCount += source->getNumPrimitiveSets();
            }
        }

        unsigned int mCount = 0;
    };

    struct FalloutMaterialOnlyGeometry
    {
        explicit FalloutMaterialOnlyGeometry(std::string_view name)
        {
            init(mGeometry);
            mGeometry.mName = name;
            mGeometry.mShaderProperty = Nif::BSShaderPropertyPtr(nullptr);
            mGeometry.mAlphaProperty = Nif::NiAlphaPropertyPtr(nullptr);

            mData.recType = Nif::RC_NiTriShapeData;
            mData.mVertices = { osg::Vec3f(0.f, 0.f, 0.f), osg::Vec3f(1.f, 0.f, 0.f),
                osg::Vec3f(0.f, 1.f, 0.f) };
            mData.mTriangles = { 0, 1, 2 };
            mGeometry.mData = Nif::NiGeometryDataPtr(&mData);

            init(static_cast<Nif::NiObjectNET&>(mMaterial));
            mMaterial.recType = Nif::RC_NiMaterialProperty;
            mGeometry.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&mMaterial));

            init(static_cast<Nif::NiObjectNET&>(mAlpha));
            mAlpha.recType = Nif::RC_NiAlphaProperty;
        }

        void addAlpha(uint16_t flags)
        {
            mAlpha.mFlags = flags;
            mAlpha.mThreshold = 0;
            mGeometry.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&mAlpha));
        }

        Nif::NiTriShape mGeometry;
        Nif::NiTriShapeData mData;
        Nif::NiMaterialProperty mMaterial;
        Nif::NiAlphaProperty mAlpha;
    };

    struct FalloutDismemberGeometry
    {
        FalloutDismemberGeometry()
        {
            init(mGeometry);
            mGeometry.mName = "Object03:0";
            mGeometry.mShaderProperty = Nif::BSShaderPropertyPtr(nullptr);
            mGeometry.mAlphaProperty = Nif::NiAlphaPropertyPtr(nullptr);

            mData.recType = Nif::RC_NiTriShapeData;
            mData.mVertices = { osg::Vec3f(0.f, 0.f, 0.f), osg::Vec3f(1.f, 0.f, 0.f),
                osg::Vec3f(0.f, 1.f, 0.f) };
            mGeometry.mData = Nif::NiGeometryDataPtr(&mData);

            init(static_cast<Nif::NiSkinInstance&>(mSkin));
            mSkin.recType = Nif::RC_BSDismemberSkinInstance;
            mSkin.mData = Nif::NiSkinDataPtr(&mSkinData);
            mSkin.mPartitions = Nif::NiSkinPartitionPtr(&mPartitions);
            mGeometry.mSkin = Nif::NiSkinInstancePtr(&mSkin);

            init(mSkinData.mTransform);
            constexpr std::array<std::uint16_t, 4> bodyPartTypes{ 0, 7000, 107, 205 };
            for (std::uint16_t type : bodyPartTypes)
            {
                Nif::NiSkinPartition::Partition partition{};
                partition.mTrueTriangles = { 0, 1, 2 };
                mPartitions.mPartitions.push_back(std::move(partition));
                mSkin.mParts.push_back(
                    { type == 0 || type == 7000 ? std::uint16_t{ 257 } : std::uint16_t{ 256 }, type });
            }
        }

        Nif::NiTriShape mGeometry;
        Nif::NiTriShapeData mData;
        Nif::BSDismemberSkinInstance mSkin;
        Nif::NiSkinData mSkinData;
        Nif::NiSkinPartition mPartitions;
    };

    struct FalloutPPLightingGeometry : FalloutMaterialOnlyGeometry
    {
        explicit FalloutPPLightingGeometry(uint32_t shaderFlags1)
            : FalloutMaterialOnlyGeometry("Window:0")
        {
            mData.mUVList.push_back({ osg::Vec2f(0.f, 0.f), osg::Vec2f(1.f, 0.f), osg::Vec2f(0.f, 1.f) });
            init(static_cast<Nif::NiObjectNET&>(mShader));
            mShader.recType = Nif::RC_BSShaderPPLightingProperty;
            mShader.mType = static_cast<unsigned int>(Nif::BSShaderType::ShaderType_Default);
            mShader.mShaderFlags1 = shaderFlags1;
            mShader.mController = nullptr;
            mShader.mTextureSet = Nif::BSShaderTextureSetPtr(&mTextureSet);
            mGeometry.mShaderProperty = Nif::BSShaderPropertyPtr(&mShader);

            mTextureSet.mTextures.resize(6);
            mTextureSet.mTextures[0] = "textures/test/diffuse.dds";
            mTextureSet.mTextures[1] = "textures/test/normal.dds";
            mTextureSet.mTextures[4] = "textures/test/environment.dds";
            mTextureSet.mTextures[5] = "textures/test/environmentmask.dds";
        }

        Nif::BSShaderPPLightingProperty mShader;
        Nif::BSShaderTextureSet mTextureSet;
    };

    struct FalloutPPLightingSiblingGeometry
    {
        FalloutPPLightingSiblingGeometry()
            : mWindow(
                  Nif::BSShaderFlags1::BSSFlag1_WindowEnvironmentMapping | Nif::BSShaderFlags1::BSSFlag1_AlphaTexture)
            , mOpaque(Nif::BSShaderFlags1::BSSFlag1_AlphaTexture)
        {
            init(static_cast<Nif::NiAVObject&>(mRoot));
            mRoot.recType = Nif::RC_NiNode;
            mRoot.mName = "Root";
            mOpaque.mGeometry.mName = "Opaque:0";
            mRoot.mChildren = { Nif::NiAVObjectPtr(&mWindow.mGeometry), Nif::NiAVObjectPtr(&mOpaque.mGeometry) };
        }

        Nif::NiNode mRoot;
        FalloutPPLightingGeometry mWindow;
        FalloutPPLightingGeometry mOpaque;
    };

    struct FalloutManagedMaterialGeometry
    {
        FalloutManagedMaterialGeometry()
        {
            init(static_cast<Nif::NiAVObject&>(mRoot));
            mRoot.recType = Nif::RC_NiNode;
            mRoot.mName = "Root";

            init(static_cast<Nif::NiTimeController&>(mManager));
            mManager.recType = Nif::RC_NiControllerManager;
            mManager.mFlags = Nif::NiTimeController::Flag_Active;
            mManager.mObjectPalette = Nif::NiDefaultAVObjectPalettePtr(nullptr);
            mRoot.mController = Nif::NiTimeControllerPtr(&mManager);

            init(static_cast<Nif::NiTimeController&>(mMaterialController));
            mMaterialController.recType = Nif::RC_NiMaterialColorController;
            mMaterialController.mFlags = Nif::NiTimeController::Flag_Active;
            mMaterialController.mTargetColor = Nif::NiMaterialColorController::TargetColor::Emissive;
            mMaterialController.mData = Nif::NiPosDataPtr(nullptr);

            mBlendInterpolator.recType = Nif::RC_NiBlendPoint3Interpolator;
            mBlendInterpolator.mFlags = Nif::NiBlendInterpolator::Flag_ManagerControlled;
            const float invalidColor = -std::numeric_limits<float>::max();
            mBlendInterpolator.mValue = osg::Vec3f(invalidColor, invalidColor, invalidColor);
            mMaterialController.mInterpolator = Nif::NiInterpolatorPtr(&mBlendInterpolator);

            init(static_cast<Nif::NiObjectNET&>(mMaterial));
            mMaterial.recType = Nif::RC_NiMaterialProperty;
            mMaterial.mAlpha = 1.f;
            mMaterial.mController = Nif::NiTimeControllerPtr(&mMaterialController);

            init(static_cast<Nif::NiObjectNET&>(mShader));
            mShader.recType = Nif::RC_BSShaderPPLightingProperty;
            mShader.mType = static_cast<unsigned int>(Nif::BSShaderType::ShaderType_Default);
            mShader.mController = Nif::NiTimeControllerPtr(nullptr);
            mShader.mTextureSet = Nif::BSShaderTextureSetPtr(&mTextureSet);
            mTextureSet.mTextures
                = { "textures/test/diffuse.dds", "textures/test/normal.dds", "textures/test/emissive.dds" };

            mData.recType = Nif::RC_NiTriShapeData;
            mData.mVertices = { osg::Vec3f(0.f, 0.f, 0.f), osg::Vec3f(1.f, 0.f, 0.f), osg::Vec3f(0.f, 1.f, 0.f) };
            mData.mUVList.push_back({ osg::Vec2f(0.f, 0.f), osg::Vec2f(1.f, 0.f), osg::Vec2f(0.f, 1.f) });
            mData.mTriangles = { 0, 1, 2 };

            initGeometry(mFront, "Object02:0");
            initGeometry(mBack, "Sign_24hr:0");
            mRoot.mChildren = { Nif::NiAVObjectPtr(&mFront), Nif::NiAVObjectPtr(&mBack) };

            mEmissionKeys = std::make_shared<Nif::Vector3KeyMap>();
            mEmissionKeys->mInterpolationType = Nif::InterpolationType_Linear;
            Nif::KeyT<osg::Vec3f> emissionKey{};
            emissionKey.mValue = osg::Vec3f(0.25f, 0.5f, 0.75f);
            mEmissionKeys->mKeys = { { 0.f, emissionKey }, { 10.f, emissionKey } };
            mEmissionData.recType = Nif::RC_NiPosData;
            mEmissionData.mKeyList = mEmissionKeys;
            mEmissionInterpolator.recType = Nif::RC_NiPoint3Interpolator;
            mEmissionInterpolator.mDefaultValue = emissionKey.mValue;
            mEmissionInterpolator.mData = Nif::NiPosDataPtr(&mEmissionData);

            mSequence.recType = Nif::RC_NiControllerSequence;
            mSequence.mName = "SpecialIdle";
            mSequence.mExtrapolationMode = Nif::NiTimeController::Cycle;
            mSequence.mFrequency = 1.f;
            mSequence.mPhase = 0.f;
            mSequence.mStartTime = 0.f;
            mSequence.mStopTime = 10.f;
            mSequence.mManager = Nif::NiControllerManagerPtr(&mManager);
            Nif::ControlledBlock block{};
            block.mTargetName = "Object02:0";
            block.mNodeName = "Object02:0";
            block.mPropertyType = "NiMaterialProperty";
            block.mControllerType = "NiMaterialColorController";
            block.mControllerId = "SELF_ILLUM";
            block.mInterpolator = Nif::NiInterpolatorPtr(&mEmissionInterpolator);
            block.mController = Nif::NiTimeControllerPtr(&mMaterialController);
            mSequence.mControlledBlocks.push_back(std::move(block));
            mManager.mSequences.push_back(Nif::RecordPtrT<Nif::NiControllerSequence>(&mSequence));
        }

        void initGeometry(Nif::NiTriShape& geometry, std::string_view name)
        {
            init(geometry);
            geometry.mName = name;
            geometry.mData = Nif::NiGeometryDataPtr(&mData);
            geometry.mShaderProperty = Nif::BSShaderPropertyPtr(&mShader);
            geometry.mAlphaProperty = Nif::NiAlphaPropertyPtr(nullptr);
            geometry.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&mMaterial));
        }

        Nif::NiNode mRoot;
        Nif::NiTriShape mFront;
        Nif::NiTriShape mBack;
        Nif::NiTriShapeData mData;
        Nif::NiMaterialProperty mMaterial;
        Nif::BSShaderPPLightingProperty mShader;
        Nif::BSShaderTextureSet mTextureSet;
        Nif::NiMaterialColorController mMaterialController;
        Nif::NiBlendPoint3Interpolator mBlendInterpolator;
        Nif::NiControllerManager mManager;
        Nif::NiControllerSequence mSequence;
        Nif::NiPoint3Interpolator mEmissionInterpolator;
        Nif::NiPosData mEmissionData;
        std::shared_ptr<Nif::Vector3KeyMap> mEmissionKeys;
    };

    struct FindNamedNodeStateSetVisitor : osg::NodeVisitor
    {
        explicit FindNamedNodeStateSetVisitor(std::string_view name)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mName(name)
        {
        }

        void apply(osg::Node& node) override
        {
            if (mStateSet == nullptr && node.getName() == mName && node.getStateSet() != nullptr)
            {
                mNode = &node;
                mStateSet = node.getStateSet();
            }
            traverse(node);
        }

        std::string mName;
        osg::Node* mNode = nullptr;
        const osg::StateSet* mStateSet = nullptr;
    };

    struct FindNamedNodeVisitor : osg::NodeVisitor
    {
        explicit FindNamedNodeVisitor(std::string_view name)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mName(name)
        {
        }

        void apply(osg::Node& node) override
        {
            if (mNode == nullptr && node.getName() == mName)
                mNode = &node;
            traverse(node);
        }

        std::string mName;
        osg::Node* mNode = nullptr;
    };

    bool hasTextureType(const osg::StateSet& stateSet, std::string_view name)
    {
        for (unsigned int unit = 0; unit < stateSet.getTextureAttributeList().size(); ++unit)
        {
            const auto* type = dynamic_cast<const SceneUtil::TextureType*>(
                stateSet.getTextureAttribute(unit, SceneUtil::TextureType::AttributeType));
            if (type != nullptr && type->getName() == name)
                return true;
        }
        return false;
    }

    void expectOpaqueState(const osg::StateSet& stateSet)
    {
        EXPECT_EQ(stateSet.getMode(GL_BLEND) & osg::StateAttribute::ON, 0u);
        EXPECT_EQ(stateSet.getAttribute(osg::StateAttribute::BLENDFUNC), nullptr);
        EXPECT_NE(stateSet.getRenderingHint(), osg::StateSet::TRANSPARENT_BIN);
    }

    osg::ref_ptr<osg::Node> loadFalloutPPLightingGeometry(FalloutPPLightingGeometry& fixture,
        Resource::ImageManager& imageManager, Resource::BgsmFileManager& materialManager)
    {
        Nif::NIFFile file(testNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mUserVersion = 11;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&fixture.mGeometry);
        return Loader::load(file, &imageManager, &materialManager);
    }

    unsigned int loadFalloutGeometryAndCountDrawables(FalloutMaterialOnlyGeometry& fixture,
        VFS::Path::NormalizedView path, Resource::ImageManager& imageManager,
        Resource::BgsmFileManager& materialManager)
    {
        Nif::NIFFile file(path);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mUserVersion = 11;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&fixture.mGeometry);

        osg::ref_ptr<osg::Node> result = Loader::load(file, &imageManager, &materialManager);
        EXPECT_NE(result, nullptr);
        if (result == nullptr)
            return 0;

        DrawableCountVisitor visitor;
        result->accept(visitor);
        return visitor.mCount;
    }

    TEST_F(NifOsgLoaderTest, shouldLoadFileWithDefaultNode)
    {
        Nif::NiAVObject node;
        init(node);
        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&node);
        auto result = Loader::load(file, &mImageManager, &mMaterialManager);
        EXPECT_EQ(serialize(*result), R"(
osg::Group {
  UniqueID 1
  DataVariance STATIC
  UserDataContainer TRUE {
    osg::DefaultUserDataContainer {
      UniqueID 2
      UDC_UserObjects 1 {
        osg::StringValueObject {
          UniqueID 3
          Name "fileHash"
        }
      }
    }
  }
  Children 1 {
    osg::Group {
      UniqueID 4
      DataVariance STATIC
      UserDataContainer TRUE {
        osg::DefaultUserDataContainer {
          UniqueID 5
          UDC_UserObjects 1 {
            osg::UIntValueObject {
              UniqueID 6
              Name "recIndex"
              Value 4294967295
            }
          }
        }
      }
    }
  }
}
)");
    }

    TEST_F(NifOsgLoaderTest, shouldEnableAuthoredInitialSwitchChildAfterLoadingChildren)
    {
        Nif::NiSwitchNode switchRecord;
        init(static_cast<Nif::NiAVObject&>(switchRecord));
        switchRecord.recType = Nif::RC_NiSwitchNode;
        switchRecord.mName = "screen selector";
        switchRecord.mInitialIndex = 1;

        Nif::NiNode firstChild;
        init(static_cast<Nif::NiAVObject&>(firstChild));
        firstChild.mName = "static";
        firstChild.mParents.push_back(&switchRecord);

        Nif::NiNode secondChild;
        init(static_cast<Nif::NiAVObject&>(secondChild));
        secondChild.mName = "face";
        secondChild.mParents.push_back(&switchRecord);

        switchRecord.mChildren
            = { Nif::NiAVObjectPtr(&firstChild), Nif::NiAVObjectPtr(&secondChild) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&switchRecord);
        osg::ref_ptr<osg::Node> result = Loader::load(file, &mImageManager, &mMaterialManager);

        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->asGroup());
        ASSERT_EQ(result->asGroup()->getNumChildren(), 1u);
        osg::Group* switchTransform = result->asGroup()->getChild(0)->asGroup();
        ASSERT_NE(switchTransform, nullptr);
        ASSERT_EQ(switchTransform->getNumChildren(), 1u);
        osg::Switch* loadedSwitch = dynamic_cast<osg::Switch*>(switchTransform->getChild(0));
        ASSERT_NE(loadedSwitch, nullptr);
        ASSERT_EQ(loadedSwitch->getNumChildren(), 2u);
        EXPECT_FALSE(loadedSwitch->getValue(0));
        EXPECT_TRUE(loadedSwitch->getValue(1));
    }

    TEST_F(NifOsgLoaderTest, shouldSkipMaterialOnlyRootHelperGeometryInFalloutActorAddon)
    {
        FalloutMaterialOnlyGeometry fixture("Screen01Root:0");

        EXPECT_EQ(loadFalloutGeometryAndCountDrawables(fixture,
                      VFS::Path::NormalizedView("meshes/creatures/test/actor-addon.nif"), mImageManager,
                      mMaterialManager),
            0u);
    }

    TEST_F(NifOsgLoaderTest, shouldKeepRenderableOrNonActorGeometryThatResemblesFalloutRootHelper)
    {
        FalloutMaterialOnlyGeometry renderable("Screen01Root:0");
        renderable.mData.mUVList.push_back(
            { osg::Vec2f(0.f, 0.f), osg::Vec2f(1.f, 0.f), osg::Vec2f(0.f, 1.f) });
        EXPECT_EQ(loadFalloutGeometryAndCountDrawables(renderable,
                      VFS::Path::NormalizedView("meshes/creatures/test/actor-addon.nif"), mImageManager,
                      mMaterialManager),
            1u);

        FalloutMaterialOnlyGeometry worldGeometry("Screen01Root:0");
        EXPECT_EQ(loadFalloutGeometryAndCountDrawables(worldGeometry,
                      VFS::Path::NormalizedView("meshes/architecture/test/world.nif"), mImageManager,
                      mMaterialManager),
            1u);
    }

    TEST_F(NifOsgLoaderTest, shouldHideOnlyConditionalFalloutDismemberCapPartitionsForIntactActors)
    {
        FalloutDismemberGeometry fixture;
        Nif::NIFFile file(VFS::Path::NormalizedView("meshes/armor/test/generic-object-name.nif"));
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mUserVersion = 11;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&fixture.mGeometry);

        osg::ref_ptr<osg::Node> result = Loader::load(file, &mImageManager, &mMaterialManager);
        ASSERT_NE(result, nullptr);

        PrimitiveSetCountVisitor visitor;
        result->accept(visitor);
        EXPECT_EQ(visitor.mCount, 2u);
    }

    TEST_F(NifOsgLoaderTest, shouldProtectAuthoredNonstandardNiAlphaBlendFromAncestorOverride)
    {
        FalloutMaterialOnlyGeometry overlay("Screenreflection01:0");
        overlay.addAlpha(0x1001); // blending enabled, ONE / ONE

        Nif::NIFFile file(VFS::Path::NormalizedView("meshes/creatures/test/actor-addon.nif"));
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mUserVersion = 11;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&overlay.mGeometry);
        osg::ref_ptr<osg::Node> result = Loader::load(file, &mImageManager, &mMaterialManager);
        ASSERT_NE(result, nullptr);

        FindNamedNodeStateSetVisitor visitor("Screenreflection01:0");
        result->accept(visitor);
        ASSERT_NE(visitor.mStateSet, nullptr);
        EXPECT_NE(visitor.mStateSet->getMode(GL_BLEND) & osg::StateAttribute::PROTECTED, 0u);
        const auto* blend = dynamic_cast<const osg::BlendFunc*>(
            visitor.mStateSet->getAttribute(osg::StateAttribute::BLENDFUNC));
        ASSERT_NE(blend, nullptr);
        EXPECT_EQ(blend->getSource(), osg::BlendFunc::ONE);
        EXPECT_EQ(blend->getDestination(), osg::BlendFunc::ONE);
    }

    TEST_F(NifOsgLoaderTest, shouldApplyBsPpLightingDepthFlags)
    {
        Nif::NiAVObject node;
        init(node);
        Nif::BSShaderPPLightingProperty property;
        property.recType = Nif::RC_BSShaderPPLightingProperty;
        property.mTextureSet = nullptr;
        property.mController = nullptr;
        property.mShaderFlags1 = Nif::BSShaderFlags1::BSSFlag1_DepthTest;
        property.mShaderFlags2 = 0;
        node.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&property));
        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&node);
        osg::ref_ptr<osg::Node> result = Loader::load(file, &mImageManager, &mMaterialManager);

        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(result->asGroup());
        ASSERT_EQ(result->asGroup()->getNumChildren(), 1u);
        const osg::StateSet* stateSet = result->asGroup()->getChild(0)->getStateSet();
        ASSERT_NE(stateSet, nullptr);
        const auto* depth = dynamic_cast<const osg::Depth*>(stateSet->getAttribute(osg::StateAttribute::DEPTH));
        ASSERT_NE(depth, nullptr);
        EXPECT_EQ(depth->getFunction(), osg::Depth::LEQUAL);
        EXPECT_FALSE(depth->getWriteMask());
    }

    TEST_F(NifOsgLoaderTest, shouldOnlyBindPpLightingEnvironmentTexturesWhenAuthored)
    {
        FalloutPPLightingGeometry unflagged(0);
        osg::ref_ptr<osg::Node> unflaggedResult
            = loadFalloutPPLightingGeometry(unflagged, mImageManager, mMaterialManager);
        ASSERT_NE(unflaggedResult, nullptr);
        FindNamedNodeStateSetVisitor unflaggedVisitor("Window:0");
        unflaggedResult->accept(unflaggedVisitor);
        ASSERT_NE(unflaggedVisitor.mStateSet, nullptr);
        EXPECT_TRUE(hasTextureType(*unflaggedVisitor.mStateSet, "diffuseMap"));
        EXPECT_TRUE(hasTextureType(*unflaggedVisitor.mStateSet, "normalMap"));
        EXPECT_FALSE(hasTextureType(*unflaggedVisitor.mStateSet, "envMap"));
        EXPECT_FALSE(hasTextureType(*unflaggedVisitor.mStateSet, "glossMap"));

        FalloutPPLightingGeometry authored(Nif::BSShaderFlags1::BSSFlag1_EnvironmentMapping);
        osg::ref_ptr<osg::Node> authoredResult
            = loadFalloutPPLightingGeometry(authored, mImageManager, mMaterialManager);
        ASSERT_NE(authoredResult, nullptr);
        FindNamedNodeStateSetVisitor authoredVisitor("Window:0");
        authoredResult->accept(authoredVisitor);
        ASSERT_NE(authoredVisitor.mStateSet, nullptr);
        EXPECT_TRUE(hasTextureType(*authoredVisitor.mStateSet, "envMap"));
        EXPECT_TRUE(hasTextureType(*authoredVisitor.mStateSet, "glossMap"));
    }

    TEST_F(NifOsgLoaderTest, shouldApplyStandardAlphaBlendForPpLightingWindowWithoutNiAlphaProperty)
    {
        FalloutPPLightingGeometry fixture(
            Nif::BSShaderFlags1::BSSFlag1_WindowEnvironmentMapping | Nif::BSShaderFlags1::BSSFlag1_AlphaTexture);
        osg::ref_ptr<osg::Node> result = loadFalloutPPLightingGeometry(fixture, mImageManager, mMaterialManager);
        ASSERT_NE(result, nullptr);

        FindNamedNodeStateSetVisitor visitor("Window:0");
        result->accept(visitor);
        ASSERT_NE(visitor.mStateSet, nullptr);
        EXPECT_TRUE(hasTextureType(*visitor.mStateSet, "envMap"));
        EXPECT_TRUE(hasTextureType(*visitor.mStateSet, "glossMap"));
        EXPECT_NE(visitor.mStateSet->getMode(GL_BLEND) & osg::StateAttribute::ON, 0u);
        const auto* blend
            = dynamic_cast<const osg::BlendFunc*>(visitor.mStateSet->getAttribute(osg::StateAttribute::BLENDFUNC));
        ASSERT_NE(blend, nullptr);
        EXPECT_EQ(blend->getSource(), osg::BlendFunc::SRC_ALPHA);
        EXPECT_EQ(blend->getDestination(), osg::BlendFunc::ONE_MINUS_SRC_ALPHA);
        EXPECT_EQ(visitor.mStateSet->getRenderingHint(), osg::StateSet::TRANSPARENT_BIN);
    }

    TEST_F(NifOsgLoaderTest, shouldKeepExplicitNiAlphaBlendAuthoritativeForPpLightingWindow)
    {
        FalloutPPLightingGeometry fixture(
            Nif::BSShaderFlags1::BSSFlag1_WindowEnvironmentMapping | Nif::BSShaderFlags1::BSSFlag1_AlphaTexture);
        fixture.addAlpha(0x1001); // blending enabled, ONE / ONE
        osg::ref_ptr<osg::Node> result = loadFalloutPPLightingGeometry(fixture, mImageManager, mMaterialManager);
        ASSERT_NE(result, nullptr);

        FindNamedNodeStateSetVisitor visitor("Window:0");
        result->accept(visitor);
        ASSERT_NE(visitor.mStateSet, nullptr);
        const auto* blend
            = dynamic_cast<const osg::BlendFunc*>(visitor.mStateSet->getAttribute(osg::StateAttribute::BLENDFUNC));
        ASSERT_NE(blend, nullptr);
        EXPECT_EQ(blend->getSource(), osg::BlendFunc::ONE);
        EXPECT_EQ(blend->getDestination(), osg::BlendFunc::ONE);
        EXPECT_NE(visitor.mStateSet->getMode(GL_BLEND) & osg::StateAttribute::PROTECTED, 0u);
    }

    TEST_F(NifOsgLoaderTest, shouldNotApplyWindowBlendForPpLightingAlphaTextureAlone)
    {
        FalloutPPLightingGeometry fixture(Nif::BSShaderFlags1::BSSFlag1_AlphaTexture);
        osg::ref_ptr<osg::Node> result = loadFalloutPPLightingGeometry(fixture, mImageManager, mMaterialManager);
        ASSERT_NE(result, nullptr);

        FindNamedNodeStateSetVisitor visitor("Window:0");
        result->accept(visitor);
        ASSERT_NE(visitor.mStateSet, nullptr);
        EXPECT_FALSE(hasTextureType(*visitor.mStateSet, "envMap"));
        EXPECT_FALSE(hasTextureType(*visitor.mStateSet, "glossMap"));
        expectOpaqueState(*visitor.mStateSet);
    }

    TEST_F(NifOsgLoaderTest, shouldNotApplyWindowReflectionForPpLightingWindowFlagAlone)
    {
        FalloutPPLightingGeometry fixture(Nif::BSShaderFlags1::BSSFlag1_WindowEnvironmentMapping);
        osg::ref_ptr<osg::Node> result = loadFalloutPPLightingGeometry(fixture, mImageManager, mMaterialManager);
        ASSERT_NE(result, nullptr);

        FindNamedNodeStateSetVisitor visitor("Window:0");
        result->accept(visitor);
        ASSERT_NE(visitor.mStateSet, nullptr);
        EXPECT_FALSE(hasTextureType(*visitor.mStateSet, "envMap"));
        EXPECT_FALSE(hasTextureType(*visitor.mStateSet, "glossMap"));
        expectOpaqueState(*visitor.mStateSet);
    }

    TEST_F(NifOsgLoaderTest, shouldKeepPpLightingWindowBlendLocalToWindowSibling)
    {
        FalloutPPLightingSiblingGeometry fixture;
        Nif::NIFFile file(testNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mUserVersion = 11;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&fixture.mRoot);
        osg::ref_ptr<osg::Node> result = Loader::load(file, &mImageManager, &mMaterialManager);
        ASSERT_NE(result, nullptr);

        FindNamedNodeStateSetVisitor windowVisitor("Window:0");
        result->accept(windowVisitor);
        ASSERT_NE(windowVisitor.mStateSet, nullptr);
        EXPECT_NE(windowVisitor.mStateSet->getMode(GL_BLEND) & osg::StateAttribute::ON, 0u);
        EXPECT_EQ(windowVisitor.mStateSet->getRenderingHint(), osg::StateSet::TRANSPARENT_BIN);

        FindNamedNodeStateSetVisitor opaqueVisitor("Opaque:0");
        result->accept(opaqueVisitor);
        ASSERT_NE(opaqueVisitor.mStateSet, nullptr);
        EXPECT_FALSE(hasTextureType(*opaqueVisitor.mStateSet, "envMap"));
        EXPECT_FALSE(hasTextureType(*opaqueVisitor.mStateSet, "glossMap"));
        expectOpaqueState(*opaqueVisitor.mStateSet);

        FindNamedNodeVisitor rootVisitor("Root");
        result->accept(rootVisitor);
        ASSERT_NE(rootVisitor.mNode, nullptr);
        const osg::StateSet* rootStateSet = rootVisitor.mNode->getStateSet();
        if (rootStateSet != nullptr)
            expectOpaqueState(*rootStateSet);
    }

    TEST_F(NifOsgLoaderTest, shouldBindEmbeddedMaterialSequenceToEverySharedMaterialInstance)
    {
        FalloutManagedMaterialGeometry fixture;
        Nif::NIFFile file(managedMaterialNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mUserVersion = 11;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&fixture.mRoot);
        osg::ref_ptr<osg::Node> result = Loader::load(file, &mImageManager, &mMaterialManager);
        ASSERT_NE(result, nullptr);

        osg::ref_ptr<osg::FrameStamp> frameStamp = new osg::FrameStamp;
        frameStamp->setSimulationTime(5.0);
        osg::NodeVisitor visitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN);
        visitor.setFrameStamp(frameStamp);

        for (std::string_view name : { "Object02:0", "Sign_24hr:0" })
        {
            FindNamedNodeStateSetVisitor find(name);
            result->accept(find);
            ASSERT_NE(find.mNode, nullptr) << name;
            ASSERT_NE(find.mStateSet, nullptr) << name;
            EXPECT_TRUE(hasTextureType(*find.mStateSet, "diffuseMap")) << name;
            EXPECT_TRUE(hasTextureType(*find.mStateSet, "normalMap")) << name;
            EXPECT_TRUE(hasTextureType(*find.mStateSet, "emissiveMap")) << name;

            auto* callback = dynamic_cast<MaterialColorController*>(find.mNode->getCullCallback());
            ASSERT_NE(callback, nullptr) << name;
            ASSERT_NE(callback->getSource(), nullptr) << name;

            osg::ref_ptr<osg::StateSet> animatedState = new osg::StateSet;
            callback->setDefaults(animatedState);
            callback->apply(animatedState, &visitor);
            const auto* material
                = dynamic_cast<const osg::Material*>(animatedState->getAttribute(osg::StateAttribute::MATERIAL));
            ASSERT_NE(material, nullptr) << name;
            const osg::Vec4f emission = material->getEmission(osg::Material::FRONT_AND_BACK);
            EXPECT_FLOAT_EQ(emission.r(), 0.25f) << name;
            EXPECT_FLOAT_EQ(emission.g(), 0.5f) << name;
            EXPECT_FLOAT_EQ(emission.b(), 0.75f) << name;
        }
    }

    std::string formatOsgNodeForBSShaderProperty(std::string_view shaderPrefix)
    {
        std::ostringstream oss;
        oss << R"(
osg::Group {
  UniqueID 1
  DataVariance STATIC
  UserDataContainer TRUE {
    osg::DefaultUserDataContainer {
      UniqueID 2
      UDC_UserObjects 1 {
        osg::StringValueObject {
          UniqueID 3
          Name "fileHash"
        }
      }
    }
  }
  Children 1 {
    osg::Group {
      UniqueID 4
      DataVariance STATIC
      UserDataContainer TRUE {
        osg::DefaultUserDataContainer {
          UniqueID 5
          UDC_UserObjects 3 {
            osg::UIntValueObject {
              UniqueID 6
              Name "recIndex"
              Value 4294967295
            }
            osg::StringValueObject {
              UniqueID 7
              Name "shaderPrefix"
              Value ")"
            << shaderPrefix << R"("
            }
            osg::BoolValueObject {
              UniqueID 8
              Name "shaderRequired"
              Value TRUE
            }
          }
        }
      }
      StateSet TRUE {
        osg::StateSet {
          UniqueID 9
          ModeList 1 {
            GL_DEPTH_TEST OFF
          }
        }
      }
    }
  }
}
)";
        return oss.str();
    }

    std::string formatOsgNodeForBSLightingShaderProperty(std::string_view shaderPrefix)
    {
        std::ostringstream oss;
        oss << R"(
osg::Group {
  UniqueID 1
  DataVariance STATIC
  UserDataContainer TRUE {
    osg::DefaultUserDataContainer {
      UniqueID 2
      UDC_UserObjects 1 {
        osg::StringValueObject {
          UniqueID 3
          Name "fileHash"
        }
      }
    }
  }
  Children 1 {
    osg::Group {
      UniqueID 4
      DataVariance STATIC
      UserDataContainer TRUE {
        osg::DefaultUserDataContainer {
          UniqueID 5
          UDC_UserObjects 3 {
            osg::UIntValueObject {
              UniqueID 6
              Name "recIndex"
              Value 4294967295
            }
            osg::StringValueObject {
              UniqueID 7
              Name "shaderPrefix"
              Value ")"
            << shaderPrefix << R"("
            }
            osg::BoolValueObject {
              UniqueID 8
              Name "shaderRequired"
              Value TRUE
            }
          }
        }
      }
      StateSet TRUE {
        osg::StateSet {
          UniqueID 9
          ModeList 1 {
            GL_DEPTH_TEST ON
          }
          AttributeList 1 {
            osg::Depth {
              UniqueID 10
              Function LEQUAL
            }
            Value OFF
          }
        }
      }
    }
  }
}
)";
        return oss.str();
    }

    struct ShaderPrefixParams
    {
        unsigned int mShaderType;
        std::string_view mExpectedShaderPrefix;
    };

    struct NifOsgLoaderBSShaderPrefixTest : TestWithParam<ShaderPrefixParams>, BaseNifOsgLoaderTest
    {
        static constexpr std::array sParams = {
            ShaderPrefixParams{ static_cast<unsigned int>(Nif::BSShaderType::ShaderType_Default), "bs/default" },
            ShaderPrefixParams{ static_cast<unsigned int>(Nif::BSShaderType::ShaderType_NoLighting), "bs/nolighting" },
            ShaderPrefixParams{ static_cast<unsigned int>(Nif::BSShaderType::ShaderType_Skin), "bs/skin" },
            ShaderPrefixParams{ static_cast<unsigned int>(Nif::BSShaderType::ShaderType_Tile), "bs/default" },
            ShaderPrefixParams{ std::numeric_limits<unsigned int>::max(), "bs/default" },
        };
    };

    TEST_P(NifOsgLoaderBSShaderPrefixTest, shouldAddShaderPrefix)
    {
        Nif::NiAVObject node;
        init(node);
        Nif::BSShaderPPLightingProperty property;
        property.recType = Nif::RC_BSShaderPPLightingProperty;
        property.mTextureSet = nullptr;
        property.mController = nullptr;
        property.mType = GetParam().mShaderType;
        node.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&property));
        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&node);
        auto result = Loader::load(file, &mImageManager, &mMaterialManager);
        EXPECT_EQ(serialize(*result), formatOsgNodeForBSShaderProperty(GetParam().mExpectedShaderPrefix));
    }

    INSTANTIATE_TEST_SUITE_P(Params, NifOsgLoaderBSShaderPrefixTest, ValuesIn(NifOsgLoaderBSShaderPrefixTest::sParams));

    struct NifOsgLoaderBSLightingShaderPrefixTest : TestWithParam<ShaderPrefixParams>, BaseNifOsgLoaderTest
    {
        static constexpr std::array sParams = {
            ShaderPrefixParams{
                static_cast<unsigned int>(Nif::BSLightingShaderType::ShaderType_Default), "bs/default" },
            ShaderPrefixParams{ static_cast<unsigned int>(Nif::BSLightingShaderType::ShaderType_Cloud), "bs/default" },
            ShaderPrefixParams{ std::numeric_limits<unsigned int>::max(), "bs/default" },
        };
    };

    TEST_P(NifOsgLoaderBSLightingShaderPrefixTest, shouldAddShaderPrefix)
    {
        Nif::NiAVObject node;
        init(node);
        Nif::BSLightingShaderProperty property;
        property.recType = Nif::RC_BSLightingShaderProperty;
        property.mTextureSet = nullptr;
        property.mController = nullptr;
        property.mType = GetParam().mShaderType;
        property.mShaderFlags1 |= Nif::BSShaderFlags1::BSSFlag1_DepthTest;
        property.mShaderFlags2 |= Nif::BSShaderFlags2::BSSFlag2_DepthWrite;
        node.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&property));
        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&node);
        auto result = Loader::load(file, &mImageManager, &mMaterialManager);
        EXPECT_EQ(serialize(*result), formatOsgNodeForBSLightingShaderProperty(GetParam().mExpectedShaderPrefix));
    }

    INSTANTIATE_TEST_SUITE_P(
        Params, NifOsgLoaderBSLightingShaderPrefixTest, ValuesIn(NifOsgLoaderBSLightingShaderPrefixTest::sParams));
}
