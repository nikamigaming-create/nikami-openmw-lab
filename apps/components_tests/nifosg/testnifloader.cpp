#include "../nif/node.hpp"

#include <components/nif/node.hpp>
#include <components/nif/property.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/sceneutil/serialize.hpp>
#include <components/vfs/manager.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/NodeVisitor>
#include <osgDB/Registry>
#include <osg/Switch>

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
                mStateSet = node.getStateSet();
            traverse(node);
        }

        std::string mName;
        const osg::StateSet* mStateSet = nullptr;
    };

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
