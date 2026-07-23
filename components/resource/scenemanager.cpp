#include "scenemanager.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>

#include <osg/AlphaFunc>
#include <osg/Capability>
#include <osg/ColorMaski>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Material>
#include <osg/Node>
#include <osg/Program>
#include <osg/Texture2D>
#include <osg/UserDataContainer>

#include <osgAnimation/BasicAnimationManager>
#include <osgAnimation/Bone>
#include <osgAnimation/RigGeometry>
#include <osgAnimation/UpdateBone>

#include <osgParticle/ParticleSystem>

#include <osgUtil/IncrementalCompileOperation>

#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <osgDB/SharedStateManager>

#include <components/debug/debuglog.hpp>

#include <components/nifosg/controller.hpp>
#include <components/nifosg/nifloader.hpp>

#include <components/nif/niffile.hpp>

#include <components/misc/algorithm.hpp>
#include <components/misc/osguservalues.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/conversion.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <components/sceneutil/clone.hpp>
#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/optimizer.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sceneutil/visitor.hpp>

#include <components/shader/shadermanager.hpp>
#include <components/shader/shadervisitor.hpp>

#include <components/files/conversion.hpp>
#include <components/files/hash.hpp>
#include <components/files/memorystream.hpp>

#include "bgsmfilemanager.hpp"
#include "errormarker.hpp"
#include "imagemanager.hpp"
#include "niffilemanager.hpp"
#include "objectcache.hpp"

namespace
{
    class InitWorldSpaceParticlesCallback
        : public SceneUtil::NodeCallback<InitWorldSpaceParticlesCallback, osgParticle::ParticleSystem*>
    {
    public:
        void operator()(osgParticle::ParticleSystem* node, osg::NodeVisitor* nv)
        {
            // HACK: Ignore the InverseWorldMatrix transform the particle system is attached to
            if (node->getNumParents() && node->getParent(0)->getNumParents())
                transformInitialParticles(node, node->getParent(0)->getParent(0));

            node->removeUpdateCallback(this);
        }

        void transformInitialParticles(osgParticle::ParticleSystem* partsys, osg::Node* node)
        {
            osg::NodePathList nodepaths = node->getParentalNodePaths();
            if (nodepaths.empty())
                return;
            osg::Matrixf worldMat = osg::computeLocalToWorld(nodepaths[0]);
            worldMat.orthoNormalize(worldMat); // scale is already applied on the particle node
            for (int i = 0; i < partsys->numParticles(); ++i)
            {
                partsys->getParticle(i)->transformPositionVelocity(worldMat);
            }

            // transform initial bounds to worldspace
            osg::BoundingSphere sphere(partsys->getInitialBound());
            SceneUtil::transformBoundingSphere(worldMat, sphere);
            osg::BoundingBox box;
            box.expandBy(sphere);
            partsys->setInitialBound(box);
        }
    };

    class InitParticlesVisitor : public osg::NodeVisitor
    {
    public:
        /// @param mask The node mask to set on ParticleSystem nodes.
        InitParticlesVisitor(unsigned int mask)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mMask(mask)
        {
        }

        bool isWorldSpaceParticleSystem(osgParticle::ParticleSystem* partsys)
        {
            // HACK: ParticleSystem has no getReferenceFrame()
            return (partsys->getUserDataContainer() && partsys->getUserDataContainer()->getNumDescriptions() > 0
                && partsys->getUserDataContainer()->getDescriptions()[0] == "worldspace");
        }

        void apply(osg::Drawable& drw) override
        {
            if (osgParticle::ParticleSystem* partsys = dynamic_cast<osgParticle::ParticleSystem*>(&drw))
            {
                if (isWorldSpaceParticleSystem(partsys))
                {
                    partsys->addUpdateCallback(new InitWorldSpaceParticlesCallback);
                }
                partsys->setNodeMask(mMask);
            }
        }

    private:
        unsigned int mMask;
    };
}

namespace Resource
{
    void TemplateMultiRef::addRef(const osg::Node* node)
    {
        mObjects.emplace_back(node);
    }

    class SharedStateManager : public osgDB::SharedStateManager
    {
    public:
        unsigned int getNumSharedTextures() const { return _sharedTextureList.size(); }

        unsigned int getNumSharedStateSets() const { return _sharedStateSetList.size(); }

        void clearCache()
        {
            std::lock_guard<OpenThreads::Mutex> lock(_listMutex);
            _sharedTextureList.clear();
            _sharedStateSetList.clear();
        }
    };

    /// Set texture filtering settings on textures contained in a FlipController.
    class SetFilterSettingsControllerVisitor : public SceneUtil::ControllerVisitor
    {
    public:
        SetFilterSettingsControllerVisitor(
            osg::Texture::FilterMode minFilter, osg::Texture::FilterMode magFilter, int maxAnisotropy)
            : mMinFilter(minFilter)
            , mMagFilter(magFilter)
            , mMaxAnisotropy(maxAnisotropy)
        {
        }

        void visit(osg::Node& node, SceneUtil::Controller& ctrl) override
        {
            if (NifOsg::FlipController* flipctrl = dynamic_cast<NifOsg::FlipController*>(&ctrl))
            {
                for (std::vector<osg::ref_ptr<osg::Texture2D>>::iterator it = flipctrl->getTextures().begin();
                     it != flipctrl->getTextures().end(); ++it)
                {
                    osg::Texture* tex = *it;
                    tex->setFilter(osg::Texture::MIN_FILTER, mMinFilter);
                    tex->setFilter(osg::Texture::MAG_FILTER, mMagFilter);
                    tex->setMaxAnisotropy(mMaxAnisotropy);
                }
            }
        }

    private:
        osg::Texture::FilterMode mMinFilter;
        osg::Texture::FilterMode mMagFilter;
        int mMaxAnisotropy;
    };

    /// Set texture filtering settings on textures contained in StateSets.
    class SetFilterSettingsVisitor : public osg::NodeVisitor
    {
    public:
        SetFilterSettingsVisitor(
            osg::Texture::FilterMode minFilter, osg::Texture::FilterMode magFilter, int maxAnisotropy)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mMinFilter(minFilter)
            , mMagFilter(magFilter)
            , mMaxAnisotropy(maxAnisotropy)
        {
        }

        void apply(osg::Node& node) override
        {
            osg::StateSet* stateset = node.getStateSet();
            if (stateset)
                applyStateSet(stateset);

            traverse(node);
        }

        void applyStateSet(osg::StateSet* stateset)
        {
            const osg::StateSet::TextureAttributeList& texAttributes = stateset->getTextureAttributeList();
            for (unsigned int unit = 0; unit < texAttributes.size(); ++unit)
            {
                osg::StateAttribute* texture = stateset->getTextureAttribute(unit, osg::StateAttribute::TEXTURE);
                if (texture)
                    applyStateAttribute(texture);
            }
        }

        void applyStateAttribute(osg::StateAttribute* attr)
        {
            osg::Texture* tex = attr->asTexture();
            if (tex)
            {
                tex->setFilter(osg::Texture::MIN_FILTER, mMinFilter);
                tex->setFilter(osg::Texture::MAG_FILTER, mMagFilter);
                tex->setMaxAnisotropy(mMaxAnisotropy);
            }
        }

    private:
        osg::Texture::FilterMode mMinFilter;
        osg::Texture::FilterMode mMagFilter;
        int mMaxAnisotropy;
    };

    // Check Collada extra descriptions
    class ColladaDescriptionVisitor : public osg::NodeVisitor
    {
    public:
        ColladaDescriptionVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mSkeleton(nullptr)
        {
        }

        osg::AlphaFunc::ComparisonFunction getTestMode(std::string mode)
        {
            if (mode == "ALWAYS")
                return osg::AlphaFunc::ALWAYS;
            if (mode == "LESS")
                return osg::AlphaFunc::LESS;
            if (mode == "EQUAL")
                return osg::AlphaFunc::EQUAL;
            if (mode == "LEQUAL")
                return osg::AlphaFunc::LEQUAL;
            if (mode == "GREATER")
                return osg::AlphaFunc::GREATER;
            if (mode == "NOTEQUAL")
                return osg::AlphaFunc::NOTEQUAL;
            if (mode == "GEQUAL")
                return osg::AlphaFunc::GEQUAL;
            if (mode == "NEVER")
                return osg::AlphaFunc::NEVER;

            Log(Debug::Warning) << "Unexpected alpha testing mode: " << mode;
            return osg::AlphaFunc::LEQUAL;
        }

        void apply(osg::Node& node) override
        {
            if (osg::StateSet* stateset = node.getStateSet())
            {
                if (stateset->getRenderingHint() == osg::StateSet::TRANSPARENT_BIN)
                {
                    osg::ref_ptr<osg::Depth> depth = new SceneUtil::AutoDepth;
                    depth->setWriteMask(false);

                    stateset->setAttributeAndModes(depth, osg::StateAttribute::ON);
                }
                else if (stateset->getRenderingHint() == osg::StateSet::OPAQUE_BIN)
                {
                    osg::ref_ptr<osg::Depth> depth = new SceneUtil::AutoDepth;
                    depth->setWriteMask(true);

                    stateset->setAttributeAndModes(depth, osg::StateAttribute::ON);
                }
            }
            /* Check if the <node> has <extra type="Node"> <technique profile="OpenSceneGraph"> <Descriptions>
               <Description> correct format for OpenMW: <Description>alphatest mode value MaterialName</Description> e.g
               <Description>alphatest GEQUAL 0.8 MyAlphaTestedMaterial</Description> */
            std::vector<std::string> descriptions = node.getDescriptions();
            for (const auto& description : descriptions)
            {
                mDescriptions.emplace_back(description);
            }

            // Iterate each description, and see if the current node uses the specified material for alpha testing
            if (node.getStateSet())
            {
                for (const auto& description : mDescriptions)
                {
                    std::vector<std::string> descriptionParts;
                    std::istringstream descriptionStringStream(description);
                    for (std::string part; std::getline(descriptionStringStream, part, ' ');)
                    {
                        descriptionParts.emplace_back(part);
                    }

                    if (descriptionParts.size() > (3) && descriptionParts.at(3) == node.getStateSet()->getName())
                    {
                        if (descriptionParts.at(0) == "alphatest")
                        {
                            osg::AlphaFunc::ComparisonFunction mode = getTestMode(descriptionParts.at(1));
                            osg::ref_ptr<osg::AlphaFunc> alphaFunc(new osg::AlphaFunc(
                                mode, Misc::StringUtils::toNumeric<float>(descriptionParts.at(2), 0.0f)));
                            node.getStateSet()->setAttributeAndModes(alphaFunc, osg::StateAttribute::ON);
                        }
                    }

                    if (descriptionParts.size() > (0) && descriptionParts.at(0) == "bodypart")
                    {
                        SceneUtil::FindByClassVisitor osgaRigFinder("RigGeometryHolder");
                        node.accept(osgaRigFinder);
                        for (osg::Node* foundRigNode : osgaRigFinder.mFoundNodes)
                        {
                            if (SceneUtil::RigGeometryHolder* rigGeometryHolder
                                = dynamic_cast<SceneUtil::RigGeometryHolder*>(foundRigNode))
                                mRigGeometryHolders.emplace_back(
                                    osg::ref_ptr<SceneUtil::RigGeometryHolder>(rigGeometryHolder));
                            else
                                Log(Debug::Error) << "Converted RigGeometryHolder is of a wrong type.";
                        }

                        if (!mRigGeometryHolders.empty())
                        {
                            osgAnimation::RigGeometry::FindNearestParentSkeleton skeletonFinder;
                            mRigGeometryHolders[0]->accept(skeletonFinder);
                            if (skeletonFinder._root.valid())
                                mSkeleton = skeletonFinder._root;
                        }
                    }
                }
            }

            traverse(node);
        }

    private:
        std::vector<std::string> mDescriptions;

    public:
        osgAnimation::Skeleton* mSkeleton; // pointer is valid only if the model is a bodypart, osg::ref_ptr<Skeleton>
        std::vector<osg::ref_ptr<SceneUtil::RigGeometryHolder>> mRigGeometryHolders;
    };

    class ReplaceAnimationUnderscoresVisitor : public osg::NodeVisitor
    {
    public:
        ReplaceAnimationUnderscoresVisitor()
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override
        {
            // NOTE: MUST update the animation manager names first!
            if (auto* animationManager = dynamic_cast<osgAnimation::BasicAnimationManager*>(node.getUpdateCallback()))
                renameAnimationChannelTargets(*animationManager);

            // Then, any applicable node names
            if (auto* rigGeometry = dynamic_cast<osgAnimation::RigGeometry*>(&node))
            {
                renameNode(*rigGeometry);
                updateVertexInfluenceMap(*rigGeometry);
            }
            else if (auto* matrixTransform = dynamic_cast<osg::MatrixTransform*>(&node))
            {
                renameNode(*matrixTransform);
                renameUpdateCallbacks(*matrixTransform);
            }

            traverse(node);
        }

    private:
        inline void renameNode(osg::Node& node)
        {
            node.setName(Misc::StringUtils::underscoresToSpaces(node.getName()));
        }

        void renameUpdateCallbacks(osg::MatrixTransform& node)
        {
            osg::Callback* cb = node.getUpdateCallback();
            while (cb)
            {
                auto* animCb = dynamic_cast<osgAnimation::AnimationUpdateCallback<osg::NodeCallback>*>(cb);
                if (animCb)
                {
                    std::string newAnimCbName = Misc::StringUtils::underscoresToSpaces(animCb->getName());
                    animCb->setName(newAnimCbName);
                }
                cb = cb->getNestedCallback();
            }
        }

        void updateVertexInfluenceMap(osgAnimation::RigGeometry& rig)
        {
            osgAnimation::VertexInfluenceMap* vertexInfluenceMap = rig.getInfluenceMap();
            if (!vertexInfluenceMap)
                return;

            std::vector<std::pair<std::string, std::string>> renameList;
            for (const auto& [oldBoneName, _] : *vertexInfluenceMap)
            {
                const std::string newBoneName = Misc::StringUtils::underscoresToSpaces(oldBoneName);
                if (newBoneName != oldBoneName)
                    renameList.emplace_back(oldBoneName, newBoneName);
            }

            for (const auto& [oldName, newName] : renameList)
            {
                if (vertexInfluenceMap->find(newName) == vertexInfluenceMap->end())
                    (*vertexInfluenceMap)[newName] = std::move((*vertexInfluenceMap)[oldName]);
                vertexInfluenceMap->erase(oldName);
            }
        }

        void renameAnimationChannelTargets(osgAnimation::BasicAnimationManager& animManager)
        {
            for (const osgAnimation::Animation* animation : animManager.getAnimationList())
            {
                if (animation)
                {
                    const osgAnimation::ChannelList& channels = animation->getChannels();
                    for (osgAnimation::Channel* channel : channels)
                        channel->setTargetName(Misc::StringUtils::underscoresToSpaces(channel->getTargetName()));
                }
            }
        }
    };

    SceneManager::SceneManager(const VFS::Manager* vfs, Resource::ImageManager* imageManager,
        Resource::NifFileManager* nifFileManager, Resource::BgsmFileManager* bgsmFileManager, double expiryDelay)
        : ResourceManager(vfs, expiryDelay)
        , mShaderManager(new Shader::ShaderManager)
        , mSharedStateManager(new SharedStateManager)
        , mImageManager(imageManager)
        , mNifFileManager(nifFileManager)
        , mBgsmFileManager(bgsmFileManager)
        , mMinFilter(osg::Texture::LINEAR_MIPMAP_LINEAR)
        , mMagFilter(osg::Texture::LINEAR)
        , mMaxAnisotropy(1)
        , mParticleSystemMask(~0u)
        , mLightingMethod(SceneUtil::LightingMethod::FFP)
    {
    }

    void SceneManager::setForceShaders(bool force)
    {
        mForceShaders = force;
    }

    bool SceneManager::getForceShaders() const
    {
        return mForceShaders;
    }

    void SceneManager::recreateShaders(osg::ref_ptr<osg::Node> node, const std::string& shaderPrefix,
        bool forceShadersForNode, const osg::Program* programTemplate)
    {
        osg::ref_ptr<Shader::ShaderVisitor> shaderVisitor(createShaderVisitor(shaderPrefix));
        shaderVisitor->setAllowedToModifyStateSets(false);
        shaderVisitor->setProgramTemplate(programTemplate);
        if (forceShadersForNode)
            shaderVisitor->setForceShaders(true);
        node->accept(*shaderVisitor);
    }

    void SceneManager::reinstateRemovedState(osg::ref_ptr<osg::Node> node)
    {
        osg::ref_ptr<Shader::ReinstateRemovedStateVisitor> reinstateRemovedStateVisitor
            = new Shader::ReinstateRemovedStateVisitor(false);
        node->accept(*reinstateRemovedStateVisitor);
    }

    void SceneManager::setClampLighting(bool clamp)
    {
        mClampLighting = clamp;
    }

    bool SceneManager::getClampLighting() const
    {
        return mClampLighting;
    }

    void SceneManager::setAutoUseNormalMaps(bool use)
    {
        mAutoUseNormalMaps = use;
    }

    void SceneManager::setNormalMapPattern(const std::string& pattern)
    {
        mNormalMapPattern = pattern;
    }

    void SceneManager::setNormalHeightMapPattern(const std::string& pattern)
    {
        mNormalHeightMapPattern = pattern;
    }

    void SceneManager::setAutoUseSpecularMaps(bool use)
    {
        mAutoUseSpecularMaps = use;
    }

    void SceneManager::setSpecularMapPattern(const std::string& pattern)
    {
        mSpecularMapPattern = pattern;
    }

    void SceneManager::setApplyLightingToEnvMaps(bool apply)
    {
        mApplyLightingToEnvMaps = apply;
    }

    void SceneManager::setSupportedLightingMethods(const SceneUtil::LightManager::SupportedMethods& supported)
    {
        mSupportedLightingMethods = supported;
    }

    bool SceneManager::isSupportedLightingMethod(SceneUtil::LightingMethod method) const
    {
        return mSupportedLightingMethods[static_cast<int>(method)];
    }

    void SceneManager::setLightingMethod(SceneUtil::LightingMethod method)
    {
        mLightingMethod = method;

        if (mLightingMethod == SceneUtil::LightingMethod::SingleUBO)
        {
            osg::ref_ptr<osg::Program> program = new osg::Program;
            program->addBindUniformBlock("LightBufferBinding", static_cast<int>(UBOBinding::LightBuffer));
            mShaderManager->setProgramTemplate(program);
        }
    }

    SceneUtil::LightingMethod SceneManager::getLightingMethod() const
    {
        return mLightingMethod;
    }

    void SceneManager::setConvertAlphaTestToAlphaToCoverage(bool convert)
    {
        mConvertAlphaTestToAlphaToCoverage = convert;
    }

    void SceneManager::setAdjustCoverageForAlphaTest(bool adjustCoverage)
    {
        mAdjustCoverageForAlphaTest = adjustCoverage;
    }

    void SceneManager::setOpaqueDepthTex(osg::ref_ptr<osg::Texture> texturePing, osg::ref_ptr<osg::Texture> texturePong)
    {
        mOpaqueDepthTex = { texturePing, texturePong };
    }

    osg::ref_ptr<osg::Texture> SceneManager::getOpaqueDepthTex(size_t frame)
    {
        return mOpaqueDepthTex[frame % 2];
    }

    SceneManager::~SceneManager()
    {
        // this has to be defined in the .cpp file as we can't delete incomplete types
    }

    Shader::ShaderManager& SceneManager::getShaderManager()
    {
        return *mShaderManager.get();
    }

    void SceneManager::setShaderPath(const std::filesystem::path& path)
    {
        mShaderManager->setShaderPath(path);
    }

    bool SceneManager::checkLoaded(VFS::Path::NormalizedView name, double timeStamp)
    {
        return mCache->checkInObjectCache(name, timeStamp);
    }

    void SceneManager::setUpNormalsRTForStateSet(osg::StateSet* stateset, bool enabled)
    {
        if (!getSupportsNormalsRT())
            return;
        stateset->setAttributeAndModes(new osg::ColorMaski(1, enabled, enabled, enabled, enabled));

        if (enabled)
            stateset->setAttributeAndModes(new osg::Disablei(GL_BLEND, 1));
    }

    /// @brief Callback to read image files from the VFS.
    class ImageReadCallback : public osgDB::ReadFileCallback
    {
    public:
        ImageReadCallback(Resource::ImageManager* imageMgr)
            : mImageManager(imageMgr)
        {
        }

        osgDB::ReaderWriter::ReadResult readImage(const std::string& filename, const osgDB::Options* options) override
        {
            auto filePath = Files::pathFromUnicodeString(filename);
            if (filePath.is_absolute())
                // It is a hack. Needed because either OSG or libcollada-dom tries to make an absolute path from
                // our relative VFS path by adding current working directory.
                filePath = std::filesystem::relative(filename, osgDB::getCurrentWorkingDirectory());
            try
            {
                return osgDB::ReaderWriter::ReadResult(
                    mImageManager->getImage(VFS::Path::toNormalized(Files::pathToUnicodeString(filePath))),
                    osgDB::ReaderWriter::ReadResult::FILE_LOADED);
            }
            catch (std::exception& e)
            {
                return osgDB::ReaderWriter::ReadResult(e.what());
            }
        }

    private:
        Resource::ImageManager* mImageManager;
    };

    namespace
    {
        bool worldViewerEnvEnabled(const char* name)
        {
            const char* value = std::getenv(name);
            return value != nullptr && *value != '\0' && value[0] != '0';
        }

        bool worldViewerMeshLoadTelemetryEnabled()
        {
            return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_MESH_LOAD_TELEMETRY")
                || worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_ACTOR_TELEMETRY")
                || worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_TELEMETRY");
        }

        bool worldViewerForceFlatTemplateMaterials()
        {
            return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FORCE_FLAT_NIF_MATERIALS")
                || worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FORCE_FLAT_WORLD_MATERIALS");
        }

        bool worldViewerForceFullbrightTemplateMaterials()
        {
            return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FULLBRIGHT_NIF_MATERIALS")
                || worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FULLBRIGHT_WORLD_MATERIALS");
        }

        bool isWorldViewerActorMeshPath(std::string_view lowered)
        {
            return lowered.find("meshes/actors/") != std::string::npos
                || lowered.find("meshes/characters/") != std::string::npos
                || lowered.find("meshes/armor/") != std::string::npos
                || lowered.find("meshes/clothes/") != std::string::npos;
        }

        bool shouldLogWorldViewerMeshLoad(VFS::Path::NormalizedView path)
        {
            if (!worldViewerMeshLoadTelemetryEnabled())
                return false;

            const std::string lowered = Misc::StringUtils::lowerCase(path.value());
            return isWorldViewerActorMeshPath(lowered);
        }

        void applyWorldViewerFlatTemplateStateSet(osg::StateSet* stateSet, const osg::Vec4f& color)
        {
            if (stateSet == nullptr)
                return;

            osg::ref_ptr<osg::Material> material = new osg::Material;
            material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
            material->setDiffuse(osg::Material::FRONT_AND_BACK, color);
            material->setAmbient(osg::Material::FRONT_AND_BACK, color);
            material->setEmission(osg::Material::FRONT_AND_BACK, color);
            material->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
            material->setShininess(osg::Material::FRONT_AND_BACK, 0.f);

            stateSet->setAttributeAndModes(material, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateSet->setAttributeAndModes(new osg::Program, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet->setMode(GL_ALPHA_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            for (unsigned int unit = 0; unit < 8; ++unit)
                stateSet->setTextureMode(unit, GL_TEXTURE_2D, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet->setRenderingHint(osg::StateSet::DEFAULT_BIN);
        }

        void applyWorldViewerFlatTemplateGeometry(osg::Geometry* geometry, const osg::Vec4f& color)
        {
            if (geometry == nullptr)
                return;

            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            colors->push_back(color);
            geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
            geometry->dirtyDisplayList();
            geometry->dirtyBound();
        }

        void applyWorldViewerFullbrightTemplateStateSet(osg::StateSet* stateSet, unsigned int& textureUnitsKept,
            unsigned int& textureUnitsDisabled)
        {
            if (stateSet == nullptr)
                return;

            osg::ref_ptr<osg::Material> material = new osg::Material;
            material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
            material->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
            material->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
            material->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
            material->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
            material->setShininess(osg::Material::FRONT_AND_BACK, 0.f);

            stateSet->setAttributeAndModes(material, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateSet->setAttributeAndModes(new osg::Program, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            for (unsigned int unit = 0; unit < 8; ++unit)
            {
                if (stateSet->getTextureAttribute(unit, osg::StateAttribute::TEXTURE) == nullptr)
                    continue;

                if (unit == 0)
                {
                    stateSet->setTextureMode(unit, GL_TEXTURE_2D, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                    ++textureUnitsKept;
                }
                else
                {
                    stateSet->setTextureMode(unit, GL_TEXTURE_2D, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                    ++textureUnitsDisabled;
                }
            }
            stateSet->setRenderingHint(osg::StateSet::DEFAULT_BIN);
        }

        void applyWorldViewerFullbrightTemplateGeometry(osg::Geometry* geometry)
        {
            if (geometry == nullptr)
                return;

            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            colors->push_back(osg::Vec4f(1.f, 1.f, 1.f, 1.f));
            geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
            geometry->dirtyDisplayList();
            geometry->dirtyBound();
        }

        class WorldViewerFlatTemplateVisitor : public osg::NodeVisitor
        {
        public:
            explicit WorldViewerFlatTemplateVisitor(const osg::Vec4f& color)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mColor(color)
            {
            }

            void apply(osg::Node& node) override
            {
                node.setUserValue("shaderRequired", false);
                node.setUserValue("shaderPrefix", std::string());
                applyWorldViewerFlatTemplateStateSet(node.getOrCreateStateSet(), mColor);
                ++mStateSets;
                traverse(node);
            }

            void apply(osg::Geode& geode) override
            {
                geode.setUserValue("shaderRequired", false);
                geode.setUserValue("shaderPrefix", std::string());
                applyWorldViewerFlatTemplateStateSet(geode.getOrCreateStateSet(), mColor);
                ++mStateSets;
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        flattenDrawable(*drawable);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { flattenDrawable(drawable); }

            unsigned int mStateSets = 0;
            unsigned int mGeometries = 0;

        private:
            void flattenDrawable(osg::Drawable& drawable)
            {
                applyWorldViewerFlatTemplateStateSet(drawable.getOrCreateStateSet(), mColor);
                ++mStateSets;
                if (osg::Geometry* geometry = drawable.asGeometry())
                {
                    applyWorldViewerFlatTemplateGeometry(geometry, mColor);
                    ++mGeometries;
                }
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* source = rig->getSourceGeometry())
                    {
                        applyWorldViewerFlatTemplateStateSet(source->getOrCreateStateSet(), mColor);
                        applyWorldViewerFlatTemplateGeometry(source, mColor);
                        ++mStateSets;
                        ++mGeometries;
                    }
                    for (unsigned int i = 0; i < 2; ++i)
                        if (osg::Geometry* geometry = rig->getRenderGeometry(i))
                        {
                            applyWorldViewerFlatTemplateStateSet(geometry->getOrCreateStateSet(), mColor);
                            applyWorldViewerFlatTemplateGeometry(geometry, mColor);
                            ++mStateSets;
                            ++mGeometries;
                        }
                }
                if (SceneUtil::RigGeometryHolder* holder = dynamic_cast<SceneUtil::RigGeometryHolder*>(&drawable))
                {
                    for (unsigned int i = 0; i < 2; ++i)
                        if (osg::Geometry* geometry = holder->getGeometry(i))
                        {
                            applyWorldViewerFlatTemplateStateSet(geometry->getOrCreateStateSet(), mColor);
                            applyWorldViewerFlatTemplateGeometry(geometry, mColor);
                            ++mStateSets;
                            ++mGeometries;
                        }
                }
                if (SceneUtil::MorphGeometry* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
                    if (osg::Geometry* source = morph->getSourceGeometry())
                    {
                        applyWorldViewerFlatTemplateStateSet(source->getOrCreateStateSet(), mColor);
                        applyWorldViewerFlatTemplateGeometry(source, mColor);
                        ++mStateSets;
                        ++mGeometries;
                    }
            }

            osg::Vec4f mColor;
        };

        class WorldViewerFullbrightTemplateVisitor : public osg::NodeVisitor
        {
        public:
            WorldViewerFullbrightTemplateVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                node.setUserValue("shaderRequired", false);
                node.setUserValue("shaderPrefix", std::string());
                applyWorldViewerFullbrightTemplateStateSet(
                    node.getOrCreateStateSet(), mTextureUnitsKept, mTextureUnitsDisabled);
                ++mStateSets;
                traverse(node);
            }

            void apply(osg::Geode& geode) override
            {
                geode.setUserValue("shaderRequired", false);
                geode.setUserValue("shaderPrefix", std::string());
                applyWorldViewerFullbrightTemplateStateSet(
                    geode.getOrCreateStateSet(), mTextureUnitsKept, mTextureUnitsDisabled);
                ++mStateSets;
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        fullbrightDrawable(*drawable);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { fullbrightDrawable(drawable); }

            unsigned int mStateSets = 0;
            unsigned int mGeometries = 0;
            unsigned int mTextureUnitsKept = 0;
            unsigned int mTextureUnitsDisabled = 0;

        private:
            void fullbrightDrawable(osg::Drawable& drawable)
            {
                applyWorldViewerFullbrightTemplateStateSet(
                    drawable.getOrCreateStateSet(), mTextureUnitsKept, mTextureUnitsDisabled);
                ++mStateSets;
                if (osg::Geometry* geometry = drawable.asGeometry())
                {
                    applyWorldViewerFullbrightTemplateGeometry(geometry);
                    ++mGeometries;
                }
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* source = rig->getSourceGeometry())
                    {
                        applyWorldViewerFullbrightTemplateStateSet(
                            source->getOrCreateStateSet(), mTextureUnitsKept, mTextureUnitsDisabled);
                        applyWorldViewerFullbrightTemplateGeometry(source);
                        ++mStateSets;
                        ++mGeometries;
                    }
                    for (unsigned int i = 0; i < 2; ++i)
                        if (osg::Geometry* geometry = rig->getRenderGeometry(i))
                        {
                            applyWorldViewerFullbrightTemplateStateSet(
                                geometry->getOrCreateStateSet(), mTextureUnitsKept, mTextureUnitsDisabled);
                            applyWorldViewerFullbrightTemplateGeometry(geometry);
                            ++mStateSets;
                            ++mGeometries;
                        }
                }
                if (SceneUtil::RigGeometryHolder* holder = dynamic_cast<SceneUtil::RigGeometryHolder*>(&drawable))
                {
                    for (unsigned int i = 0; i < 2; ++i)
                        if (osg::Geometry* geometry = holder->getGeometry(i))
                        {
                            applyWorldViewerFullbrightTemplateStateSet(
                                geometry->getOrCreateStateSet(), mTextureUnitsKept, mTextureUnitsDisabled);
                            applyWorldViewerFullbrightTemplateGeometry(geometry);
                            ++mStateSets;
                            ++mGeometries;
                        }
                }
                if (SceneUtil::MorphGeometry* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
                    if (osg::Geometry* source = morph->getSourceGeometry())
                    {
                        applyWorldViewerFullbrightTemplateStateSet(
                            source->getOrCreateStateSet(), mTextureUnitsKept, mTextureUnitsDisabled);
                        applyWorldViewerFullbrightTemplateGeometry(source);
                        ++mStateSets;
                        ++mGeometries;
                    }
            }
        };

        void applyWorldViewerFlatTemplateMaterials(VFS::Path::NormalizedView path, osg::Node* node)
        {
            if (node == nullptr)
                return;

            const std::string lowered = Misc::StringUtils::lowerCase(path.value());
            const bool actorPath = isWorldViewerActorMeshPath(lowered);
            const bool flatMaterials = worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FORCE_FLAT_NIF_MATERIALS")
                || (worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FORCE_FLAT_WORLD_MATERIALS") && !actorPath);
            const bool fullbrightMaterials = !flatMaterials
                && (worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FULLBRIGHT_NIF_MATERIALS")
                    || (worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FULLBRIGHT_WORLD_MATERIALS") && !actorPath));
            if (!flatMaterials && !fullbrightMaterials)
                return;

            unsigned int stateSets = 0;
            unsigned int geometries = 0;
            unsigned int textureUnitsKept = 0;
            unsigned int textureUnitsDisabled = 0;

            if (flatMaterials)
            {
                const osg::Vec4f color = actorPath ? osg::Vec4f(0.86f, 0.82f, 0.74f, 1.f)
                                                   : osg::Vec4f(0.78f, 0.83f, 0.76f, 1.f);
                WorldViewerFlatTemplateVisitor visitor(color);
                node->accept(visitor);
                stateSets = visitor.mStateSets;
                geometries = visitor.mGeometries;
            }
            else
            {
                WorldViewerFullbrightTemplateVisitor visitor;
                node->accept(visitor);
                stateSets = visitor.mStateSets;
                geometries = visitor.mGeometries;
                textureUnitsKept = visitor.mTextureUnitsKept;
                textureUnitsDisabled = visitor.mTextureUnitsDisabled;
            }

            static std::atomic<int> logCount{ 0 };
            const int logIndex = logCount.fetch_add(1);
            if (logIndex < 180)
                Log(Debug::Info) << "World viewer template proof material: mode="
                                 << (flatMaterials ? "flat" : "fullbright")
                                 << " path=\"" << path.value() << "\""
                                 << " actorPath=" << actorPath
                                 << " stateSets=" << stateSets
                                 << " geometries=" << geometries
                                 << " textureUnitsKept=" << textureUnitsKept
                                 << " textureUnitsDisabled=" << textureUnitsDisabled;
            else if (logIndex == 180)
                Log(Debug::Info) << "World viewer template proof material: further logs suppressed";
        }

        class WorldViewerMeshAuditVisitor : public osg::NodeVisitor
        {
        public:
            WorldViewerMeshAuditVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                ++mNodes;
                if (node.getNodeMask() == 0)
                    ++mZeroMaskNodes;
                if (osg::Drawable* drawable = node.asDrawable())
                    auditDrawable(*drawable);
                traverse(node);
            }

            void apply(osg::Geode& geode) override
            {
                ++mNodes;
                ++mGeodes;
                if (geode.getNodeMask() == 0)
                    ++mZeroMaskNodes;
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        auditDrawable(*drawable);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override
            {
                ++mNodes;
                auditDrawable(drawable);
            }

            void apply(osg::Geometry& geometry) override
            {
                ++mNodes;
                auditDrawable(geometry);
            }

            unsigned int mNodes = 0;
            unsigned int mZeroMaskNodes = 0;
            unsigned int mGeodes = 0;
            unsigned int mDrawables = 0;
            unsigned int mGeometry = 0;
            unsigned int mRigGeometry = 0;
            unsigned int mRigGeometryHolder = 0;
            unsigned int mRigRenderGeometry = 0;
            unsigned int mMorphGeometry = 0;
            unsigned int mMorphSourceGeometry = 0;
            unsigned int mParticles = 0;

        private:
            void auditDrawable(osg::Drawable& drawable)
            {
                ++mDrawables;
                if (drawable.asGeometry() != nullptr)
                    ++mGeometry;
                if (dynamic_cast<osgParticle::ParticleSystem*>(&drawable) != nullptr)
                    ++mParticles;
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    ++mRigGeometry;
                    if (rig->getSourceGeometry() != nullptr)
                        ++mGeometry;
                    for (unsigned int i = 0; i < 2; ++i)
                        if (rig->getRenderGeometry(i) != nullptr)
                            ++mRigRenderGeometry;
                }
                if (SceneUtil::RigGeometryHolder* holder = dynamic_cast<SceneUtil::RigGeometryHolder*>(&drawable))
                {
                    ++mRigGeometryHolder;
                    if (holder->getSourceRigGeometry() != nullptr)
                        ++mGeometry;
                    for (unsigned int i = 0; i < 2; ++i)
                        if (holder->getGeometry(i) != nullptr)
                            ++mRigRenderGeometry;
                }
                if (SceneUtil::MorphGeometry* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
                {
                    ++mMorphGeometry;
                    if (morph->getSourceGeometry() != nullptr)
                    {
                        ++mMorphSourceGeometry;
                        ++mGeometry;
                    }
                }
            }
        };

        void logWorldViewerMeshAudit(VFS::Path::NormalizedView path, std::string_view stage, osg::Node* node)
        {
            if (!shouldLogWorldViewerMeshLoad(path) || node == nullptr)
                return;

            WorldViewerMeshAuditVisitor visitor;
            node->accept(visitor);
            const osg::BoundingSphere sphere = node->getBound();
            Log(Debug::Info) << "World viewer mesh ledger: stage=" << stage
                             << " path=\"" << path.value() << "\""
                             << " nodes=" << visitor.mNodes
                             << " zeroMaskNodes=" << visitor.mZeroMaskNodes
                             << " geodes=" << visitor.mGeodes
                             << " drawables=" << visitor.mDrawables
                             << " geometry=" << visitor.mGeometry
                             << " rigGeometry=" << visitor.mRigGeometry
                             << " rigGeometryHolder=" << visitor.mRigGeometryHolder
                             << " rigRenderGeometry=" << visitor.mRigRenderGeometry
                             << " morphGeometry=" << visitor.mMorphGeometry
                             << " morphSourceGeometry=" << visitor.mMorphSourceGeometry
                             << " particles=" << visitor.mParticles
                             << " boundValid=" << sphere.valid()
                             << " boundRadius=" << (sphere.valid() ? sphere.radius() : 0.f);
        }

        osg::ref_ptr<osg::Node> loadNonNif(
            VFS::Path::NormalizedView normalizedFilename, std::istream& model, Resource::ImageManager* imageManager)
        {
            const std::string_view ext = Misc::getFileExtension(normalizedFilename.value());
            const bool isColladaFile = ext == "dae";
            osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension(std::string(ext));
            if (!reader)
            {
                std::stringstream errormsg;
                errormsg << "Error loading " << normalizedFilename << ": no readerwriter for '" << ext << "' found"
                         << std::endl;
                throw std::runtime_error(errormsg.str());
            }

            osg::ref_ptr<osgDB::Options> options(new osgDB::Options);
            // Set a ReadFileCallback so that image files referenced in the model are read from our virtual file system
            // instead of the osgDB. Note, for some formats (.obj/.mtl) that reference other (non-image) files a
            // findFileCallback would be necessary. but findFileCallback does not support virtual files, so we can't
            // implement it.
            options->setReadFileCallback(new ImageReadCallback(imageManager));
            if (isColladaFile)
                options->setOptionString("daeUseSequencedTextureUnits");

            const std::array<std::uint64_t, 2> fileHash = Files::getHash(normalizedFilename.value(), model);

            osgDB::ReaderWriter::ReadResult result = reader->readNode(model, options);
            if (!result.success())
            {
                std::stringstream errormsg;
                errormsg << "Error loading " << normalizedFilename << ": " << result.message() << " code "
                         << result.status() << std::endl;
                throw std::runtime_error(errormsg.str());
            }

            // Recognize and hide collision node
            unsigned int hiddenNodeMask = 0;
            SceneUtil::FindByNameVisitor nameFinder("Collision");

            auto node = result.getNode();
            node->accept(nameFinder);
            if (nameFinder.mFoundNode)
                nameFinder.mFoundNode->setNodeMask(hiddenNodeMask);

            // Recognize and convert osgAnimation::RigGeometry to OpenMW-optimized type
            SceneUtil::FindByClassVisitor rigFinder("RigGeometry");
            node->accept(rigFinder);

            // If a collada file with rigs, we should replace underscores with spaces
            if (isColladaFile && !rigFinder.mFoundNodes.empty())
            {
                ReplaceAnimationUnderscoresVisitor renamingVisitor;
                node->accept(renamingVisitor);
            }

            // Replace osg::Depth with reverse-Z-compatible SceneUtil::AutoDepth
            SceneUtil::ReplaceDepthVisitor replaceDepthVisitor;
            node->accept(replaceDepthVisitor);

            for (osg::Node* foundRigNode : rigFinder.mFoundNodes)
            {
                if (foundRigNode->libraryName() == std::string_view("osgAnimation"))
                {
                    osgAnimation::RigGeometry* foundRigGeometry = static_cast<osgAnimation::RigGeometry*>(foundRigNode);

                    osg::ref_ptr<SceneUtil::RigGeometryHolder> newRig
                        = new SceneUtil::RigGeometryHolder(*foundRigGeometry, osg::CopyOp::DEEP_COPY_ALL);

                    if (foundRigGeometry->getStateSet())
                        newRig->setStateSet(foundRigGeometry->getStateSet());

                    if (osg::Group* parent = dynamic_cast<osg::Group*>(foundRigGeometry->getParent(0)))
                    {
                        parent->removeChild(foundRigGeometry);
                        parent->addChild(newRig);
                    }
                }
            }

            if (isColladaFile)
            {
                Resource::ColladaDescriptionVisitor colladaDescriptionVisitor;
                node->accept(colladaDescriptionVisitor);

                if (colladaDescriptionVisitor.mSkeleton)
                {
                    if (osg::Group* group = dynamic_cast<osg::Group*>(node))
                    {
                        group->removeChildren(0, group->getNumChildren());
                        for (osg::ref_ptr<SceneUtil::RigGeometryHolder> newRiggeometryHolder :
                            colladaDescriptionVisitor.mRigGeometryHolders)
                        {
                            osg::ref_ptr<osg::MatrixTransform> backToOriginTrans = new osg::MatrixTransform();

                            newRiggeometryHolder->getOrCreateUserDataContainer()->addUserObject(
                                new TemplateRef(newRiggeometryHolder->getGeometry(0)));
                            backToOriginTrans->getOrCreateUserDataContainer()->addUserObject(
                                new TemplateRef(newRiggeometryHolder->getGeometry(0)));

                            newRiggeometryHolder->setBodyPart(true);

                            for (int i = 0; i < 2; ++i)
                            {
                                if (newRiggeometryHolder->getGeometry(i))
                                    newRiggeometryHolder->getGeometry(i)->setSkeleton(nullptr);
                            }

                            backToOriginTrans->addChild(newRiggeometryHolder);
                            group->addChild(backToOriginTrans);

                            node->getOrCreateUserDataContainer()->addUserObject(
                                new TemplateRef(newRiggeometryHolder->getGeometry(0)));
                        }
                    }
                }

                node->getOrCreateStateSet()->addUniform(new osg::Uniform("emissiveMult", 1.f));
                node->getOrCreateStateSet()->addUniform(new osg::Uniform("specStrength", 1.f));
                node->getOrCreateStateSet()->addUniform(new osg::Uniform("envMapColor", osg::Vec4f(1, 1, 1, 1)));
                node->getOrCreateStateSet()->addUniform(new osg::Uniform("useFalloff", false));
                node->getOrCreateStateSet()->addUniform(new osg::Uniform("distortionStrength", 0.f));
            }

            node->setUserValue(Misc::OsgUserValues::sFileHash,
                std::string(reinterpret_cast<const char*>(fileHash.data()), fileHash.size() * sizeof(std::uint64_t)));

            return node;
        }

        std::vector<std::string> makeSortedReservedNames()
        {
            static constexpr std::string_view names[] = {
                "Head",
                "Neck",
                "Chest",
                "Groin",
                "Right Hand",
                "Left Hand",
                "Right Wrist",
                "Left Wrist",
                "Shield Bone",
                "Right Forearm",
                "Left Forearm",
                "Right Upper Arm",
                "Left Upper Arm",
                "Right Foot",
                "Left Foot",
                "Right Ankle",
                "Left Ankle",
                "Right Knee",
                "Left Knee",
                "Right Upper Leg",
                "Left Upper Leg",
                "Right Clavicle",
                "Left Clavicle",
                "Weapon Bone",
                "Tail",
                "Bip01",
                "Root Bone",
                "BoneOffset",
                "AttachLight",
                "Arrow",
                "Camera",
                "Collision",
                "ProjectileNode",
                "ShellCasingNode",
                "Right_Wrist",
                "Left_Wrist",
                "Shield_Bone",
                "Right_Forearm",
                "Left_Forearm",
                "Right_Upper_Arm",
                "Left_Clavicle",
                "Weapon_Bone",
                "Root_Bone",
            };

            std::vector<std::string> result;
            result.reserve(2 * std::size(names));

            for (std::string_view name : names)
            {
                result.emplace_back(name);
                std::string prefixedName("Tri ");
                prefixedName += name;
                result.push_back(std::move(prefixedName));
            }

            std::sort(result.begin(), result.end(), Misc::StringUtils::ciLess);

            return result;
        }
    }

    osg::ref_ptr<osg::Node> load(VFS::Path::NormalizedView normalizedFilename, const VFS::Manager* vfs,
        Resource::ImageManager* imageManager, Resource::NifFileManager* nifFileManager,
        Resource::BgsmFileManager* materialMgr)
    {
        const std::string_view ext = Misc::getFileExtension(normalizedFilename.value());
        if (ext == "nif")
            return NifOsg::Loader::load(*nifFileManager->get(normalizedFilename), imageManager, materialMgr);
        else if (ext == "spt")
        {
            Log(Debug::Warning) << "Ignoring SpeedTree data file " << normalizedFilename;
            return new osg::Node();
        }
        else
            return loadNonNif(normalizedFilename, *vfs->get(normalizedFilename), imageManager);
    }

    class CanOptimizeCallback : public SceneUtil::Optimizer::IsOperationPermissibleForObjectCallback
    {
    public:
        bool isReservedName(const std::string& name) const
        {
            if (name.empty())
                return false;

            static const std::vector<std::string> reservedNames = makeSortedReservedNames();

            const auto it = Misc::partialBinarySearch(reservedNames.begin(), reservedNames.end(), name);
            return it != reservedNames.end();
        }

        bool isOperationPermissibleForObjectImplementation(
            const SceneUtil::Optimizer* optimizer, const osg::Drawable* node, unsigned int option) const override
        {
            if (option & SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS)
            {
                if (node->asGeometry() && node->className() == std::string("Geometry"))
                    return true;
                else
                    return false; // ParticleSystem would have to convert space of all the processors, RigGeometry would
                                  // have to convert bones... theoretically possible, but very complicated
            }
            return (option & optimizer->getPermissibleOptimizationsForObject(node)) != 0;
        }

        bool isOperationPermissibleForObjectImplementation(
            const SceneUtil::Optimizer* optimizer, const osg::Node* node, unsigned int option) const override
        {
            if (node->getNumDescriptions() > 0)
                return false;
            if (node->getDataVariance() == osg::Object::DYNAMIC)
                return false;
            if (isReservedName(node->getName()))
                return false;

            return (option & optimizer->getPermissibleOptimizationsForObject(node)) != 0;
        }
    };

    static bool canOptimize(std::string_view filename)
    {
        const std::string_view::size_type slashpos = filename.find_last_of('/');
        if (slashpos != std::string_view::npos && slashpos + 1 < filename.size())
        {
            const std::string_view basename = filename.substr(slashpos + 1);
            // xmesh.nif can not be optimized because there are keyframes added in post
            if (!basename.empty() && basename[0] == 'x')
                return false;

            // NPC skeleton files can not be optimized because of keyframes added in post
            // (most of them are usually named like 'xbase_anim.nif' anyway, but not all of them :( )
            if (basename.starts_with("base_anim") || basename.starts_with("skin"))
                return false;
        }

        // For spell VFX, DummyXX nodes must remain intact. Not adding those to reservedNames to avoid being overly
        // cautious - instead, decide on filename
        if (filename.find("vfx_pattern") != std::string_view::npos)
            return false;
        return true;
    }

    unsigned int getOptimizationOptions()
    {
        using namespace SceneUtil;
        const char* env = getenv("OPENMW_OPTIMIZE");
        unsigned int options
            = Optimizer::FLATTEN_STATIC_TRANSFORMS | Optimizer::REMOVE_REDUNDANT_NODES | Optimizer::MERGE_GEOMETRY;
        if (env)
        {
            std::string str(env);

            if (str.find("OFF") != std::string::npos || str.find('0') != std::string::npos)
                options = 0;

            if (str.find("~FLATTEN_STATIC_TRANSFORMS") != std::string::npos)
                options ^= Optimizer::FLATTEN_STATIC_TRANSFORMS;
            else if (str.find("FLATTEN_STATIC_TRANSFORMS") != std::string::npos)
                options |= Optimizer::FLATTEN_STATIC_TRANSFORMS;

            if (str.find("~REMOVE_REDUNDANT_NODES") != std::string::npos)
                options ^= Optimizer::REMOVE_REDUNDANT_NODES;
            else if (str.find("REMOVE_REDUNDANT_NODES") != std::string::npos)
                options |= Optimizer::REMOVE_REDUNDANT_NODES;

            if (str.find("~MERGE_GEOMETRY") != std::string::npos)
                options ^= Optimizer::MERGE_GEOMETRY;
            else if (str.find("MERGE_GEOMETRY") != std::string::npos)
                options |= Optimizer::MERGE_GEOMETRY;
        }
        return options;
    }

    void SceneManager::shareState(osg::ref_ptr<osg::Node> node)
    {
        mSharedStateMutex.lock();
        mSharedStateManager->share(node.get());
        mSharedStateMutex.unlock();
    }

    osg::ref_ptr<osg::Node> SceneManager::loadErrorMarker()
    {
        try
        {
            VFS::Path::Normalized path("meshes/marker_error.****");
            for (const auto meshType : { "nif", "osg", "osgt", "osgb", "osgx", "osg2", "dae" })
            {
                path.changeExtension(meshType);
                if (mVFS->exists(path))
                    return load(path, mVFS, mImageManager, mNifFileManager, mBgsmFileManager);
            }
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "Failed to load error marker:" << e.what()
                                << ", using embedded marker_error instead";
        }
        Files::IMemStream file(ErrorMarker::sValue.data(), ErrorMarker::sValue.size());
        constexpr VFS::Path::NormalizedView errorMarker("error_marker.osgt");
        return loadNonNif(errorMarker, file, mImageManager);
    }

    void SceneManager::loadSelectionMarker(
        osg::ref_ptr<osg::Group> parentNode, const char* markerData, long long markerSize) const
    {
        Files::IMemStream file(markerData, markerSize);
        constexpr VFS::Path::NormalizedView selectionMarker("selectionmarker.osgt");
        parentNode->addChild(loadNonNif(selectionMarker, file, mImageManager));
    }

    osg::ref_ptr<osg::Node> SceneManager::cloneErrorMarker()
    {
        std::call_once(mErrorMarkerFlag, [this] { mErrorMarker = loadErrorMarker(); });

        return static_cast<osg::Node*>(mErrorMarker->clone(osg::CopyOp::DEEP_COPY_ALL));
    }

    osg::ref_ptr<const osg::Node> SceneManager::getTemplate(VFS::Path::NormalizedView path, bool compile)
    {
        osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(path);
        if (obj)
            return osg::ref_ptr<const osg::Node>(static_cast<osg::Node*>(obj.get()));
        else
        {
            osg::ref_ptr<osg::Node> loaded;
            try
            {
                loaded = load(path, mVFS, mImageManager, mNifFileManager, mBgsmFileManager);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Error) << "Failed to load '" << path << "': " << e.what() << ", using marker_error instead";
                loaded = cloneErrorMarker();
            }
            logWorldViewerMeshAudit(path, "loaded", loaded.get());

            // set filtering settings
            SetFilterSettingsVisitor setFilterSettingsVisitor(mMinFilter, mMagFilter, mMaxAnisotropy);
            loaded->accept(setFilterSettingsVisitor);
            SetFilterSettingsControllerVisitor setFilterSettingsControllerVisitor(
                mMinFilter, mMagFilter, mMaxAnisotropy);
            loaded->accept(setFilterSettingsControllerVisitor);

            osg::ref_ptr<Shader::ShaderVisitor> shaderVisitor(createShaderVisitor());
            loaded->accept(*shaderVisitor);
            logWorldViewerMeshAudit(path, "shader", loaded.get());

            if (canOptimize(path.value()))
            {
                SceneUtil::Optimizer optimizer;
                optimizer.setSharedStateManager(mSharedStateManager, &mSharedStateMutex);
                optimizer.setIsOperationPermissibleForObjectCallback(new CanOptimizeCallback);

                static const unsigned int options
                    = getOptimizationOptions() | SceneUtil::Optimizer::SHARE_DUPLICATE_STATE;

                optimizer.optimize(loaded, options);
                logWorldViewerMeshAudit(path, "optimized", loaded.get());
            }
            else
            {
                shareState(loaded);
                logWorldViewerMeshAudit(path, "shared", loaded.get());
            }

            if (compile && mIncrementalCompileOperation)
                mIncrementalCompileOperation->add(loaded);
            else
                loaded->getBound();

            applyWorldViewerFlatTemplateMaterials(path, loaded.get());
            logWorldViewerMeshAudit(path, "template-final", loaded.get());
            mCache->addEntryToObjectCache(path.value(), loaded);
            return loaded;
        }
    }

    osg::ref_ptr<osg::Node> SceneManager::getInstance(VFS::Path::NormalizedView path)
    {
        return getInstance(getTemplate(path));
    }

    osg::ref_ptr<osg::Node> SceneManager::cloneNode(const osg::Node* base)
    {
        SceneUtil::CopyOp copyop;
        if (const osg::Drawable* drawable = base->asDrawable())
        {
            if (drawable->asGeometry())
            {
                Log(Debug::Warning) << "SceneManager::cloneNode: attempting to clone osg::Geometry. For safety reasons "
                                       "this will be expensive. Consider avoiding this call.";
                copyop.setCopyFlags(
                    copyop.getCopyFlags() | osg::CopyOp::DEEP_COPY_ARRAYS | osg::CopyOp::DEEP_COPY_PRIMITIVES);
            }
        }
        osg::ref_ptr<osg::Node> cloned = static_cast<osg::Node*>(base->clone(copyop));
        // add a ref to the original template to help verify the safety of shallow cloning operations
        // in addition, if this node is managed by a cache, we hint to the cache that it's still being used and should
        // be kept in cache
        cloned->getOrCreateUserDataContainer()->addUserObject(new TemplateRef(base));
        return cloned;
    }

    osg::ref_ptr<osg::Node> SceneManager::getInstance(const osg::Node* base)
    {
        osg::ref_ptr<osg::Node> cloned = cloneNode(base);
        // we can skip any scene graphs without update callbacks since we know that particle emitters will have an
        // update callback set
        if (cloned->getNumChildrenRequiringUpdateTraversal() > 0)
        {
            InitParticlesVisitor visitor(mParticleSystemMask);
            cloned->accept(visitor);
        }

        return cloned;
    }

    osg::ref_ptr<osg::Node> SceneManager::getInstance(VFS::Path::NormalizedView path, osg::Group* parentNode)
    {
        osg::ref_ptr<osg::Node> cloned = getInstance(path);
        attachTo(cloned, parentNode);
        return cloned;
    }

    void SceneManager::attachTo(osg::Node* instance, osg::Group* parentNode) const
    {
        parentNode->addChild(instance);
    }

    void SceneManager::releaseGLObjects(osg::State* state)
    {
        mCache->releaseGLObjects(state);

        mShaderManager->releaseGLObjects(state);

        std::lock_guard<std::mutex> lock(mSharedStateMutex);
        mSharedStateManager->releaseGLObjects(state);
    }

    void SceneManager::setIncrementalCompileOperation(osgUtil::IncrementalCompileOperation* ico)
    {
        mIncrementalCompileOperation = ico;
    }

    osgUtil::IncrementalCompileOperation* SceneManager::getIncrementalCompileOperation()
    {
        return mIncrementalCompileOperation.get();
    }

    Resource::ImageManager* SceneManager::getImageManager()
    {
        return mImageManager;
    }

    void SceneManager::setParticleSystemMask(unsigned int mask)
    {
        mParticleSystemMask = mask;
    }

    void SceneManager::setFilterSettings(
        const std::string& magfilter, const std::string& minfilter, const std::string& mipmap, int maxAnisotropy)
    {
        osg::Texture::FilterMode min = osg::Texture::LINEAR;
        osg::Texture::FilterMode mag = osg::Texture::LINEAR;

        if (magfilter == "nearest")
            mag = osg::Texture::NEAREST;
        else if (magfilter != "linear")
            Log(Debug::Warning) << "Warning: Invalid texture mag filter: " << magfilter;

        if (minfilter == "nearest")
            min = osg::Texture::NEAREST;
        else if (minfilter != "linear")
            Log(Debug::Warning) << "Warning: Invalid texture min filter: " << minfilter;

        if (mipmap == "nearest")
        {
            if (min == osg::Texture::NEAREST)
                min = osg::Texture::NEAREST_MIPMAP_NEAREST;
            else if (min == osg::Texture::LINEAR)
                min = osg::Texture::LINEAR_MIPMAP_NEAREST;
        }
        else if (mipmap != "none")
        {
            if (mipmap != "linear")
                Log(Debug::Warning) << "Warning: Invalid texture mipmap: " << mipmap;
            if (min == osg::Texture::NEAREST)
                min = osg::Texture::NEAREST_MIPMAP_LINEAR;
            else if (min == osg::Texture::LINEAR)
                min = osg::Texture::LINEAR_MIPMAP_LINEAR;
        }

        mMinFilter = min;
        mMagFilter = mag;
        mMaxAnisotropy = std::max(1, maxAnisotropy);

        SetFilterSettingsControllerVisitor setFilterSettingsControllerVisitor(mMinFilter, mMagFilter, mMaxAnisotropy);
        SetFilterSettingsVisitor setFilterSettingsVisitor(mMinFilter, mMagFilter, mMaxAnisotropy);

        mCache->accept(setFilterSettingsVisitor);
        mCache->accept(setFilterSettingsControllerVisitor);
    }

    void SceneManager::applyFilterSettings(osg::Texture* tex)
    {
        tex->setFilter(osg::Texture::MIN_FILTER, mMinFilter);
        tex->setFilter(osg::Texture::MAG_FILTER, mMagFilter);
        tex->setMaxAnisotropy(mMaxAnisotropy);
    }

    void SceneManager::setUnRefImageDataAfterApply(bool unref)
    {
        mUnRefImageDataAfterApply = unref;
    }

    void SceneManager::updateCache(double referenceTime)
    {
        ResourceManager::updateCache(referenceTime);

        mSharedStateMutex.lock();
        mSharedStateManager->prune();
        mSharedStateMutex.unlock();

        if (mIncrementalCompileOperation)
        {
            std::lock_guard<OpenThreads::Mutex> lock(*mIncrementalCompileOperation->getToCompiledMutex());
            osgUtil::IncrementalCompileOperation::CompileSets& sets = mIncrementalCompileOperation->getToCompile();
            for (osgUtil::IncrementalCompileOperation::CompileSets::iterator it = sets.begin(); it != sets.end();)
            {
                int refcount = (*it)->_subgraphToCompile->referenceCount();
                if ((*it)->_subgraphToCompile->asDrawable())
                    refcount -= 1; // ref by CompileList.
                if (refcount <= 2) // ref by ObjectCache + ref by _subgraphToCompile.
                {
                    // no other ref = not needed anymore.
                    it = sets.erase(it);
                }
                else
                    ++it;
            }
        }
    }

    void SceneManager::clearCache()
    {
        ResourceManager::clearCache();

        std::lock_guard<std::mutex> lock(mSharedStateMutex);
        mSharedStateManager->clearCache();
    }

    void SceneManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        if (mIncrementalCompileOperation)
        {
            std::lock_guard<OpenThreads::Mutex> lock(*mIncrementalCompileOperation->getToCompiledMutex());
            stats->setAttribute(frameNumber, "Compiling", mIncrementalCompileOperation->getToCompile().size());
        }

        {
            std::lock_guard<std::mutex> lock(mSharedStateMutex);
            stats->setAttribute(frameNumber, "Texture", mSharedStateManager->getNumSharedTextures());
            stats->setAttribute(frameNumber, "StateSet", mSharedStateManager->getNumSharedStateSets());
        }

        Resource::reportStats("Node", frameNumber, mCache->getStats(), *stats);
    }

    osg::ref_ptr<Shader::ShaderVisitor> SceneManager::createShaderVisitor(const std::string& shaderPrefix)
    {
        osg::ref_ptr<Shader::ShaderVisitor> shaderVisitor(
            new Shader::ShaderVisitor(*mShaderManager.get(), *mImageManager, shaderPrefix));
        shaderVisitor->setForceShaders(mForceShaders);
        shaderVisitor->setAutoUseNormalMaps(mAutoUseNormalMaps);
        shaderVisitor->setNormalMapPattern(mNormalMapPattern);
        shaderVisitor->setNormalHeightMapPattern(mNormalHeightMapPattern);
        shaderVisitor->setAutoUseSpecularMaps(mAutoUseSpecularMaps);
        shaderVisitor->setSpecularMapPattern(mSpecularMapPattern);
        shaderVisitor->setApplyLightingToEnvMaps(mApplyLightingToEnvMaps);
        shaderVisitor->setConvertAlphaTestToAlphaToCoverage(mConvertAlphaTestToAlphaToCoverage);
        shaderVisitor->setAdjustCoverageForAlphaTest(mAdjustCoverageForAlphaTest);
        shaderVisitor->setSupportsNormalsRT(mSupportsNormalsRT);
        shaderVisitor->setWeatherParticleOcclusion(mWeatherParticleOcclusion);
        return shaderVisitor;
    }

    void SceneManager::applyShaders(osg::Node& node, const std::string& shaderPrefix)
    {
        osg::ref_ptr<Shader::ShaderVisitor> shaderVisitor(createShaderVisitor(shaderPrefix));
        shaderVisitor->setAllowedToModifyStateSets(true);
        node.accept(*shaderVisitor);
    }
}
