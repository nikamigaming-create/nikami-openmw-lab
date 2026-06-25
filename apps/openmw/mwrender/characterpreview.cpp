#include "characterpreview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include <osg/BlendFunc>
#include <osg/Camera>
#include <osg/Fog>
#include <osg/Material>
#include <osg/PositionAttitudeTransform>
#include <osg/Texture2D>
#include <osg/ValueObject>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>

#include <components/debug/debuglog.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/values.hpp>
#include <components/stereo/multiview.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwclass/esm4npc.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/customdata.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/weapontype.hpp"

#include "animation.hpp"
#include "creatureanimation.hpp"
#include "esm4npcanimation.hpp"
#include "npcanimation.hpp"
#include "util.hpp"
#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
        const ESM4::Npc* findFalloutInventoryPlayerVisualRecord()
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr)
                return nullptr;

            const char* env = std::getenv("OPENMW_FNV_PLAYER_NPC");
            const std::string_view wanted = env != nullptr && *env != '\0' ? std::string_view(env) : "Player";
            const ESM4::Npc* fallback = nullptr;

            for (const ESM4::Npc& npc : store->get<ESM4::Npc>())
            {
                if (!npc.mIsFONV)
                    continue;
                if (Misc::StringUtils::ciEqual(npc.mEditorId, wanted))
                    return &npc;
                if (fallback == nullptr && Misc::StringUtils::ciEqual(npc.mEditorId, "Player"))
                    fallback = &npc;
            }

            return fallback;
        }

        const ESM4::Armor* findFalloutInventoryArmorByEditorId(std::string_view editorId)
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr || editorId.empty())
                return nullptr;

            for (const ESM4::Armor& armor : store->get<ESM4::Armor>())
                if (Misc::StringUtils::ciEqual(armor.mEditorId, editorId))
                    return &armor;

            return nullptr;
        }

        bool isFalloutContentLoaded()
        {
            const MWBase::World* world = MWBase::Environment::get().getWorld();
            if (world == nullptr)
                return false;

            for (const std::string& file : world->getContentFiles())
                if (Misc::StringUtils::ciEndsWith(file, "FalloutNV.esm"))
                    return true;

            return false;
        }

        bool shouldUseFalloutInventoryPlayerVisual()
        {
            if (std::getenv("OPENMW_FNV_DISABLE_INVENTORY_PREVIEW") != nullptr)
                return false;

            if (std::getenv("OPENMW_FNV_INVENTORY_PLAYER_PROXY") != nullptr)
                return true;

            return isFalloutContentLoaded();
        }

        void applyFalloutInventoryPlayerProxyProofOutfit(const MWWorld::Ptr& visualPtr)
        {
            const char* outfitEnv = std::getenv("OPENMW_FNV_PLAYER_OUTFIT");
            const std::string_view outfitEditorId
                = outfitEnv != nullptr && *outfitEnv != '\0' ? std::string_view(outfitEnv) : "OutfitRepublican02";
            const ESM4::Armor* armor = findFalloutInventoryArmorByEditorId(outfitEditorId);
            if (armor == nullptr)
            {
                Log(Debug::Warning) << "FNV/ESM4 proof: inventory player proxy outfit " << outfitEditorId
                                    << " not found";
                return;
            }

            visualPtr.getRefData().setCustomData(std::unique_ptr<MWWorld::CustomData>());
            const bool added = MWClass::ESM4Npc::addEquippedArmor(visualPtr, armor);
            Log(Debug::Info) << "FNV/ESM4 proof: inventory player proxy outfit " << armor->mEditorId << " model="
                             << MWClass::ESM4Npc::chooseEquipmentModel(
                                    armor, MWClass::ESM4Npc::isFemale(visualPtr))
                             << " added=" << added;
        }

        std::string getFalloutPreviewAnimationGroup()
        {
            const char* value = std::getenv("OPENMW_FNV_ACTOR_KIT_ANIMATION_GROUP");
            if (value == nullptr || value[0] == '\0')
                return "idle";

            std::string group(value);
            while (!group.empty()
                && (group.front() == ' ' || group.front() == '\t' || group.front() == '\r'
                    || group.front() == '\n' || group.front() == '"' || group.front() == '\''))
                group.erase(group.begin());
            while (!group.empty()
                && (group.back() == ' ' || group.back() == '\t' || group.back() == '\r'
                    || group.back() == '\n' || group.back() == '"' || group.back() == '\''))
                group.pop_back();
            Misc::StringUtils::lowerCaseInPlace(group);
            return group.empty() ? std::string("idle") : group;
        }

        float getFalloutPreviewAnimationStartPoint()
        {
            const char* value = std::getenv("OPENMW_FNV_ACTOR_KIT_ANIMATION_STARTPOINT");
            if (value == nullptr || value[0] == '\0')
                return 0.35f;

            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || !std::isfinite(parsed))
                return 0.35f;
            return std::clamp(parsed, 0.f, 0.999f);
        }

        std::string_view getFalloutNeutralActorPreviewProfile()
        {
            const char* value = std::getenv("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_PROFILE");
            if (value == nullptr || value[0] == '\0')
                return "upper";
            return value;
        }

        float getFalloutNeutralActorPreviewFloat(const char* name, float fallback)
        {
            const char* value = std::getenv(name);
            if (value == nullptr || value[0] == '\0')
                return fallback;

            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || !std::isfinite(parsed))
                return fallback;
            return parsed;
        }

        void applyFalloutNeutralActorOrbitCamera(FalloutActorPreview::ViewMode viewMode, float distance, float cameraZ,
            float lookAtZ, osg::Vec3f& position, osg::Vec3f& lookAt, const char*& viewName)
        {
            const float diagonal = distance * 0.70710678118f;
            position = osg::Vec3f(0.f, distance, cameraZ);
            lookAt = osg::Vec3f(0.f, 0.f, lookAtZ);
            viewName = "front";
            switch (viewMode)
            {
                case FalloutActorPreview::ViewMode::Front:
                    break;
                case FalloutActorPreview::ViewMode::FrontLeft:
                    position = osg::Vec3f(-diagonal, diagonal, cameraZ);
                    viewName = "front-left";
                    break;
                case FalloutActorPreview::ViewMode::FrontRight:
                    position = osg::Vec3f(diagonal, diagonal, cameraZ);
                    viewName = "front-right";
                    break;
            }
        }
    }

    class DrawOnceCallback : public SceneUtil::NodeCallback<DrawOnceCallback>
    {
    public:
        DrawOnceCallback(osg::Node* subgraph)
            : mRendered(false)
            , mLastRenderedFrame(0)
            , mSubgraph(subgraph)
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            if (!mRendered)
            {
                mRendered = true;

                mLastRenderedFrame = nv->getTraversalNumber();

                osg::ref_ptr<osg::FrameStamp> previousFramestamp = const_cast<osg::FrameStamp*>(nv->getFrameStamp());
                osg::FrameStamp* fs = new osg::FrameStamp(*previousFramestamp);
                fs->setSimulationTime(mSimulationTime);

                nv->setFrameStamp(fs);

                // Update keyframe controllers in the scene graph first...
                // RTTNode does not continue update traversal, so manually continue the update traversal since we need
                // it.
                mSubgraph->accept(*nv);
                traverse(node, nv);

                nv->setFrameStamp(previousFramestamp);
            }
            else
            {
                node->setNodeMask(0);
            }
        }

        void redrawNextFrame() { mRendered = false; }
        void setSimulationTime(double simulationTime) { mSimulationTime = simulationTime; }

        unsigned int getLastRenderedFrame() const { return mLastRenderedFrame; }

    private:
        bool mRendered;
        unsigned int mLastRenderedFrame;
        double mSimulationTime = 0.0;
        osg::ref_ptr<osg::Node> mSubgraph;
    };

    // Set up alpha blending mode to avoid issues caused by transparent objects writing onto the alpha value of the FBO
    // This makes the RTT have premultiplied alpha, though, so the source blend factor must be GL_ONE when it's applied
    class SetUpBlendVisitor : public osg::NodeVisitor
    {
    public:
        SetUpBlendVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override
        {
            if (osg::ref_ptr<osg::StateSet> stateset = node.getStateSet())
            {
                osg::ref_ptr<osg::StateSet> newStateSet;
                if (stateset->getAttribute(osg::StateAttribute::BLENDFUNC)
                    || stateset->getBinNumber() == osg::StateSet::TRANSPARENT_BIN)
                {
                    osg::BlendFunc* blendFunc
                        = static_cast<osg::BlendFunc*>(stateset->getAttribute(osg::StateAttribute::BLENDFUNC));

                    if (blendFunc)
                    {
                        newStateSet = new osg::StateSet(*stateset, osg::CopyOp::SHALLOW_COPY);
                        node.setStateSet(newStateSet);
                        osg::ref_ptr<osg::BlendFunc> newBlendFunc = new osg::BlendFunc(*blendFunc);
                        newStateSet->setAttribute(newBlendFunc, osg::StateAttribute::ON);
                        // I *think* (based on some by-hand maths) that the RGB and dest alpha factors are unchanged,
                        // and only dest determines source alpha factor This has the benefit of being idempotent if we
                        // assume nothing used glBlendFuncSeparate before we touched it
                        if (blendFunc->getDestination() == osg::BlendFunc::ONE_MINUS_SRC_ALPHA)
                            newBlendFunc->setSourceAlpha(osg::BlendFunc::ONE);
                        else if (blendFunc->getDestination() == osg::BlendFunc::ONE)
                            newBlendFunc->setSourceAlpha(osg::BlendFunc::ZERO);
                        // Other setups barely exist in the wild and aren't worth supporting as they're not equippable
                        // gear
                        else
                            Log(Debug::Info) << "Unable to adjust blend mode for character preview. Source factor 0x"
                                             << std::hex << blendFunc->getSource() << ", destination factor 0x"
                                             << blendFunc->getDestination() << std::dec;
                    }
                }
                if (stateset->getMode(GL_BLEND) & osg::StateAttribute::ON)
                {
                    if (!newStateSet)
                    {
                        newStateSet = new osg::StateSet(*stateset, osg::CopyOp::SHALLOW_COPY);
                        node.setStateSet(newStateSet);
                    }
                    // Disable noBlendAlphaEnv
                    newStateSet->setTextureMode(7, GL_TEXTURE_2D, osg::StateAttribute::OFF);
                    newStateSet->setDefine("FORCE_OPAQUE", "0", osg::StateAttribute::ON);
                }
            }
            traverse(node);
        }
    };

    class FalloutActorPreviewPartMaskVisitor : public osg::NodeVisitor
    {
    public:
        enum class Mode
        {
            FaceHeadgear,
            RightHandWeapon,
        };

        FalloutActorPreviewPartMaskVisitor(Mode mode)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mMode(mode)
        {
        }

        void apply(osg::Node& node) override
        {
            const std::string name = Misc::StringUtils::lowerCase(node.getName());
            if (name.rfind("fnv part ", 0) == 0)
            {
                const bool keep = mMode == Mode::FaceHeadgear ? keepFaceHeadgearPart(name) : keepRightHandWeaponPart(name);
                if (!keep)
                {
                    node.setNodeMask(0);
                    ++mMasked;
                    return;
                }
                ++mKept;
            }

            traverse(node);
        }

        unsigned int getKept() const { return mKept; }
        unsigned int getMasked() const { return mMasked; }

    private:
        static bool hasAny(std::string_view value, std::initializer_list<std::string_view> needles)
        {
            for (std::string_view needle : needles)
            {
                if (value.find(needle) != std::string_view::npos)
                    return true;
            }
            return false;
        }

        static bool keepFaceHeadgearPart(std::string_view name)
        {
            return hasAny(name,
                { "characters/head/", "characters\\head\\", "characters/hair/", "characters\\hair\\", "headhuman",
                    "headold", "mouth", "teeth", "tongue", "eye", "eyebrow", "beard", "hair", "headgear", "hat",
                    "cowboy", "bandana" });
        }

        static bool keepRightHandWeaponPart(std::string_view name)
        {
            return hasAny(name,
                { "righthand", "right hand", "weapon", "weap", "1hand", "pistol", "revolver", "rifle", "gun" });
        }

        Mode mMode;
        unsigned int mKept = 0;
        unsigned int mMasked = 0;
    };

    class CharacterPreviewRTTNode : public SceneUtil::RTTNode
    {
        static constexpr float fovYDegrees = 12.3f;
        static constexpr float znear = 4.0f;
        static constexpr float zfar = 10000.f;

    public:
        CharacterPreviewRTTNode(uint32_t sizeX, uint32_t sizeY)
            : RTTNode(sizeX, sizeY, Settings::video().mAntialiasing, false, 0,
                StereoAwareness::Unaware_MultiViewShaders, shouldAddMSAAIntermediateTarget())
            , mAspectRatio(static_cast<float>(sizeX) / static_cast<float>(sizeY))
        {
            if (SceneUtil::AutoDepth::isReversed())
                mPerspectiveMatrix = static_cast<osg::Matrixf>(
                    SceneUtil::getReversedZProjectionMatrixAsPerspective(fovYDegrees, mAspectRatio, znear, zfar));
            else
                mPerspectiveMatrix = osg::Matrixf::perspective(fovYDegrees, mAspectRatio, znear, zfar);
            mGroup->getOrCreateStateSet()->addUniform(new osg::Uniform("projectionMatrix", mPerspectiveMatrix));
            mViewMatrix = osg::Matrixf::identity();
            setColorBufferInternalFormat(GL_RGBA);
            setDepthBufferInternalFormat(GL_DEPTH24_STENCIL8);
        }

        void setDefaults(osg::Camera* camera) override
        {
            camera->setName("CharacterPreview");
            camera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
            camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT, osg::Camera::PIXEL_BUFFER_RTT);
            camera->setClearColor(osg::Vec4(0.f, 0.f, 0.f, 0.f));
            camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            camera->setProjectionMatrixAsPerspective(fovYDegrees, mAspectRatio, znear, zfar);
            camera->setViewport(0, 0, width(), height());
            camera->setRenderOrder(osg::Camera::PRE_RENDER);
            camera->setCullMask(~(Mask_UpdateVisitor));
            camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
            SceneUtil::setCameraClearDepth(camera);

            camera->setNodeMask(Mask_RenderToTexture);
            camera->addChild(mGroup);
        }

        void apply(osg::Camera* camera) override
        {
            if (mCameraStateset)
                camera->setStateSet(mCameraStateset);
            camera->setViewMatrix(mViewMatrix);

            if (shouldDoTextureArray())
                Stereo::setMultiviewMatrices(mGroup->getOrCreateStateSet(), { mPerspectiveMatrix, mPerspectiveMatrix });
        }

        void addChild(osg::Node* node) { mGroup->addChild(node); }

        void setCameraStateset(osg::StateSet* stateset) { mCameraStateset = stateset; }

        void setViewMatrix(const osg::Matrixf& viewMatrix) { mViewMatrix = viewMatrix; }

        osg::ref_ptr<osg::Group> mGroup = new osg::Group;
        osg::Matrixf mPerspectiveMatrix;
        osg::Matrixf mViewMatrix;
        osg::ref_ptr<osg::StateSet> mCameraStateset;
        float mAspectRatio;
    };

    CharacterPreview::CharacterPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem,
        const MWWorld::Ptr& character, int sizeX, int sizeY, const osg::Vec3f& position, const osg::Vec3f& lookAt)
        : mParent(parent)
        , mResourceSystem(resourceSystem)
        , mPosition(position)
        , mLookAt(lookAt)
        , mCharacter(character)
        , mAnimation(nullptr)
        , mSizeX(sizeX)
        , mSizeY(sizeY)
    {
        mTextureStateSet = new osg::StateSet;
        mTextureStateSet->setAttribute(new osg::BlendFunc(osg::BlendFunc::ONE, osg::BlendFunc::ONE_MINUS_SRC_ALPHA));

        mRTTNode = new CharacterPreviewRTTNode(sizeX, sizeY);
        mRTTNode->setNodeMask(Mask_RenderToTexture);

        osg::ref_ptr<SceneUtil::LightManager> lightManager = new SceneUtil::LightManager(
            SceneUtil::LightSettings{
                .mClusteredLighting = Settings::shaders().mClusteredLighting,
                .mMaxLights = Settings::shaders().mMaxLights,
                .mMaximumLightDistance = Settings::shaders().mMaximumLightDistance,
                .mLightFadeStart = Settings::shaders().mLightFadeStart,
                .mLightRadiusMultiplier = Settings::shaders().mLightRadiusMultiplier,
            },
            resourceSystem);
        osg::ref_ptr<osg::StateSet> stateset = lightManager->getOrCreateStateSet();
        stateset->setDefine("FORCE_OPAQUE", "1", osg::StateAttribute::ON);
        stateset->setMode(GL_NORMALIZE, osg::StateAttribute::ON);
        stateset->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
        osg::ref_ptr<osg::Material> defaultMat(new osg::Material);
        defaultMat->setColorMode(isFalloutContentLoaded() ? osg::Material::AMBIENT_AND_DIFFUSE : osg::Material::OFF);
        defaultMat->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        defaultMat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        defaultMat->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
        stateset->setAttribute(defaultMat);

        SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*stateset);

        // assign large value to effectively turn off fog
        // shaders don't respect glDisable(GL_FOG)
        osg::ref_ptr<osg::Fog> fog(new osg::Fog);
        fog->setStart(10000000);
        fog->setEnd(10000000);
        stateset->setAttributeAndModes(fog, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

        // TODO: Clean up this mess of loose uniforms that shaders depend on.
        // turn off sky blending
        stateset->addUniform(new osg::Uniform("far", 10000000.0f));
        stateset->addUniform(new osg::Uniform("skyBlendingStart", 8000000.0f));
        stateset->addUniform(new osg::Uniform("screenRes", osg::Vec2f{ 1, 1 }));

        stateset->addUniform(new osg::Uniform("emissiveMult", 1.f));

        osg::ref_ptr<osg::Texture2D> dummyTexture = new osg::Texture2D();
        dummyTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        dummyTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        dummyTexture->setInternalFormat(GL_DEPTH_COMPONENT);
        dummyTexture->setTextureSize(1, 1);
        // This might clash with a shadow map, so make sure it doesn't cast shadows
        dummyTexture->setShadowComparison(true);
        dummyTexture->setShadowCompareFunc(osg::Texture::ShadowCompareFunc::ALWAYS);
        stateset->setTextureAttributeAndModes(7, dummyTexture, osg::StateAttribute::ON);

        osg::ref_ptr<osg::Light> light = new osg::Light;
        float diffuseR = Fallback::Map::getFloat("Inventory_DirectionalDiffuseR");
        float diffuseG = Fallback::Map::getFloat("Inventory_DirectionalDiffuseG");
        float diffuseB = Fallback::Map::getFloat("Inventory_DirectionalDiffuseB");
        float ambientR = Fallback::Map::getFloat("Inventory_DirectionalAmbientR");
        float ambientG = Fallback::Map::getFloat("Inventory_DirectionalAmbientG");
        float ambientB = Fallback::Map::getFloat("Inventory_DirectionalAmbientB");
        float azimuth = osg::DegreesToRadians(Fallback::Map::getFloat("Inventory_DirectionalRotationX"));
        float altitude = osg::DegreesToRadians(Fallback::Map::getFloat("Inventory_DirectionalRotationY"));
        float positionX = -std::cos(azimuth) * std::sin(altitude);
        float positionY = std::sin(azimuth) * std::sin(altitude);
        float positionZ = std::cos(altitude);
        light->setPosition(osg::Vec4(positionX, positionY, positionZ, 0.0));
        if (isFalloutContentLoaded())
        {
            diffuseR = std::max(diffuseR, 0.95f);
            diffuseG = std::max(diffuseG, 0.95f);
            diffuseB = std::max(diffuseB, 0.95f);
            ambientR = std::max(ambientR, 0.45f);
            ambientG = std::max(ambientG, 0.45f);
            ambientB = std::max(ambientB, 0.45f);
        }
        light->setDiffuse(osg::Vec4(diffuseR, diffuseG, diffuseB, 1));
        light->setAmbient(osg::Vec4(ambientR, ambientG, ambientB, 1));
        light->setSpecular(osg::Vec4(0, 0, 0, 0));
        light->setConstantAttenuation(1.f);
        light->setLinearAttenuation(0.f);
        light->setQuadraticAttenuation(0.f);
        lightManager->setSunlight(light);

        mRTTNode->addChild(lightManager);

        mNode = new osg::PositionAttitudeTransform;
        lightManager->addChild(mNode);

        mDrawOnceCallback = new DrawOnceCallback(mRTTNode->mGroup);
        mRTTNode->addUpdateCallback(mDrawOnceCallback);

        mParent->addChild(mRTTNode);

        mCharacter.mCell = nullptr;
    }

    CharacterPreview::~CharacterPreview()
    {
        mParent->removeChild(mRTTNode);
    }

    int CharacterPreview::getTextureWidth() const
    {
        return mSizeX;
    }

    int CharacterPreview::getTextureHeight() const
    {
        return mSizeY;
    }

    void CharacterPreview::setBlendMode()
    {
        SetUpBlendVisitor visitor;
        mNode->accept(visitor);
    }

    void CharacterPreview::setRedrawSimulationTime(double simulationTime)
    {
        mDrawOnceCallback->setSimulationTime(simulationTime);
    }

    void CharacterPreview::onSetup()
    {
        setBlendMode();
    }

    osg::ref_ptr<Animation> CharacterPreview::createAnimation()
    {
        return new NpcAnimation(mCharacter, mNode, mResourceSystem, true,
            (renderHeadOnly() ? NpcAnimation::VM_HeadOnly : NpcAnimation::VM_Normal));
    }

    osg::ref_ptr<osg::Texture2D> CharacterPreview::getTexture()
    {
        return static_cast<osg::Texture2D*>(mRTTNode->getColorTexture(nullptr));
    }

    void CharacterPreview::rebuild()
    {
        mAnimation = nullptr;

        mAnimation = createAnimation();

        onSetup();

        redraw();
    }

    void CharacterPreview::redraw()
    {
        if (std::getenv("OPENMW_FNV_DISABLE_INVENTORY_PREVIEW") != nullptr && mNode != nullptr
            && mNode->getName() == "FNV Inventory Paper Doll Preview")
        {
            mRTTNode->setNodeMask(0);
            Log(Debug::Info) << "FNV/ESM4 proof: disabled inventory paper doll RTT for Android headset startup";
            return;
        }

        mRTTNode->setNodeMask(Mask_RenderToTexture);
        mDrawOnceCallback->redrawNextFrame();
    }

    // --------------------------------------------------------------------------------------------------

    InventoryPreview::InventoryPreview(
        osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character, ViewMode viewMode)
        : CharacterPreview(parent, resourceSystem, character, 512, 1024, osg::Vec3f(0, 700, 71), osg::Vec3f(0, 0, 71))
        , mViewMode(viewMode)
    {
        mNode->setName("FNV Inventory Paper Doll Preview");
    }

    osg::ref_ptr<Animation> InventoryPreview::createAnimation()
    {
        if (shouldUseFalloutInventoryPlayerVisual())
        {
            if (const ESM4::Npc* falloutPlayerVisual = findFalloutInventoryPlayerVisualRecord())
            {
                ESM::CellRef proxyRef;
                proxyRef.blank();
                proxyRef.mRefID = ESM::RefId::stringRefId("Player");
                mFalloutPreviewRef = std::make_unique<MWWorld::LiveCellRef<ESM4::Npc>>(proxyRef, falloutPlayerVisual);
                MWWorld::Ptr visualPtr(mFalloutPreviewRef.get(), nullptr);
                visualPtr.getRefData().setCustomData(std::unique_ptr<MWWorld::CustomData>());
                applyFalloutInventoryPlayerProxyProofOutfit(visualPtr);

                Log(Debug::Info) << "FNV/ESM4 proof: using Fallout inventory player visual proxy "
                                 << falloutPlayerVisual->mEditorId << " (" << ESM::RefId(falloutPlayerVisual->mId)
                                 << ")";
                return new ESM4NpcAnimation(visualPtr, mNode, mResourceSystem);
            }

            Log(Debug::Warning)
                << "FNV/ESM4 proof: requested Fallout inventory player visual proxy, but no FONV Player NPC was found";
        }

        mFalloutPreviewRef.reset();
        if (mCharacter.getType() == ESM::REC_NPC_4)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: using live Fallout inventory player visual " << mCharacter.toString();
            return new ESM4NpcAnimation(mCharacter, mNode, mResourceSystem);
        }

        return CharacterPreview::createAnimation();
    }

    void InventoryPreview::setViewport(int sizeX, int sizeY)
    {
        sizeX = std::max(sizeX, 0);
        sizeY = std::max(sizeY, 0);

        // NB Camera::setViewport has threading issues
        osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;
        // This expects Y-down convention; historically the origin was (0, mSizeY - sizeY)
        mViewport = new osg::Viewport(0, 0, std::min(mSizeX, sizeX), std::min(mSizeY, sizeY));
        stateset->setAttributeAndModes(mViewport);
        mRTTNode->setCameraStateset(stateset);

        redraw();
    }

    void InventoryPreview::update()
    {
        if (!mAnimation.get())
            return;

        NpcAnimation* npcAnimation = dynamic_cast<NpcAnimation*>(mAnimation.get());
        if (npcAnimation == nullptr)
        {
            const float previewStart = mFalloutPreviewRef != nullptr ? 0.35f : 0.0f;
            mDrawOnceCallback->setSimulationTime(previewStart);
            if (mAnimation->hasAnimation("idle"))
                mAnimation->play("idle", 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", previewStart, 0);
            mAnimation->runAnimation(previewStart);
            setBlendMode();
            redraw();
            return;
        }

        npcAnimation->showWeapons(true);
        npcAnimation->updateParts();

        MWWorld::InventoryStore& inv = mCharacter.getClass().getInventoryStore(mCharacter);
        MWWorld::ContainerStoreIterator iter = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        std::string groupname = "inventoryhandtohand";
        bool showCarriedLeft = true;
        if (iter != inv.end())
        {
            groupname = "inventoryweapononehand";
            if (iter->getType() == ESM::Weapon::sRecordId)
            {
                MWWorld::LiveCellRef<ESM::Weapon>* ref = iter->get<ESM::Weapon>();
                int type = ref->mBase->mData.mType;
                const ESM::WeaponType* weaponInfo = MWMechanics::getWeaponType(type);
                showCarriedLeft = !(weaponInfo->mFlags & ESM::WeaponType::TwoHanded);

                std::string inventoryGroup = weaponInfo->mLongGroup;
                inventoryGroup = "inventory" + inventoryGroup;

                // We still should use one-handed animation as fallback
                if (npcAnimation->hasAnimation(inventoryGroup))
                    groupname = std::move(inventoryGroup);
                else
                {
                    static const std::string oneHandFallback
                        = "inventory" + MWMechanics::getWeaponType(ESM::Weapon::LongBladeOneHand)->mLongGroup;
                    static const std::string twoHandFallback
                        = "inventory" + MWMechanics::getWeaponType(ESM::Weapon::LongBladeTwoHand)->mLongGroup;

                    // For real two-handed melee weapons use 2h swords animations as fallback, otherwise use the 1h ones
                    if (weaponInfo->mFlags & ESM::WeaponType::TwoHanded
                        && weaponInfo->mWeaponClass == ESM::WeaponType::Melee)
                        groupname = twoHandFallback;
                    else
                        groupname = oneHandFallback;
                }
            }
        }

        npcAnimation->showCarriedLeft(showCarriedLeft);

        const bool falloutPreview = mFalloutPreviewRef != nullptr || mCharacter.getType() == ESM::REC_NPC_4;
        if (falloutPreview && !npcAnimation->hasAnimation(groupname) && npcAnimation->hasAnimation("idle"))
        {
            Log(Debug::Info) << "FNV/ESM4 proof: inventory paper doll using Fallout idle pose instead of missing "
                             << groupname;
            groupname = "idle";
        }

        mCurrentAnimGroup = std::move(groupname);
        npcAnimation->play(mCurrentAnimGroup, 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", 0.0f, 0);

        MWWorld::ConstContainerStoreIterator torch = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
        if (torch != inv.end() && torch->getType() == ESM::Light::sRecordId && showCarriedLeft)
        {
            if (!npcAnimation->getInfo("torch"))
                npcAnimation->play("torch", 2, BlendMask::BlendMask_LeftArm, false, 1.0f, "start", "stop", 0.0f,
                    std::numeric_limits<uint32_t>::max(), true);
        }
        else if (npcAnimation->getInfo("torch"))
            npcAnimation->disable("torch");

        npcAnimation->runAnimation(0.0f);
        mDrawOnceCallback->setSimulationTime(0.0f);

        setBlendMode();

        redraw();
    }

    int InventoryPreview::getSlotSelected(int posX, int posY)
    {
        if (!mViewport)
            return -1;
        NpcAnimation* npcAnimation = dynamic_cast<NpcAnimation*>(mAnimation.get());
        if (npcAnimation == nullptr)
            return -1;

        double projX = (posX / mViewport->width()) * 2 - 1;
        double projY = (posY / mViewport->height()) * 2 - 1;
        // With Intersector::WINDOW, the intersection ratios are slightly inaccurate. Seems to be a
        // precision issue - compiling with OSG_USE_FLOAT_MATRIX=0, Intersector::WINDOW works ok.
        // Using Intersector::PROJECTION results in better precision because the start/end points and the model matrices
        // don't go through as many transformations.
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector(
            new osgUtil::LineSegmentIntersector(osgUtil::Intersector::PROJECTION, projX, projY));

        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);
        osgUtil::IntersectionVisitor visitor(intersector);
        visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);
        // Set the traversal number from the last draw, so that the frame switch used for RigGeometry double buffering
        // works correctly
        visitor.setTraversalNumber(mDrawOnceCallback->getLastRenderedFrame());

        auto* camera = mRTTNode->getCamera(nullptr);
        osg::Node::NodeMask nodeMask = camera->getNodeMask();
        camera->setNodeMask(~0u);
        camera->accept(visitor);
        camera->setNodeMask(nodeMask);

        if (intersector->containsIntersections())
        {
            osgUtil::LineSegmentIntersector::Intersection intersection = intersector->getFirstIntersection();
            return npcAnimation->getSlot(intersection.nodePath);
        }
        return -1;
    }

    void InventoryPreview::updatePtr(const MWWorld::Ptr& ptr)
    {
        mCharacter = MWWorld::Ptr(ptr.getBase(), nullptr);
    }

    void InventoryPreview::onSetup()
    {
        CharacterPreview::onSetup();
        osg::Vec3f scale(1.f, 1.f, 1.f);
        mCharacter.getClass().adjustScale(mCharacter, scale, true);

        mNode->setScale(scale);

        osg::Vec3f position = mPosition;
        osg::Vec3f lookAt = mLookAt;
        if (mFalloutPreviewRef != nullptr)
        {
            switch (mViewMode)
            {
                case ViewMode::Front:
                    position = osg::Vec3f(0, 700, 76);
                    lookAt = osg::Vec3f(0, 0, 76);
                    break;
                case ViewMode::Profile:
                    position = osg::Vec3f(320, 0, 104);
                    lookAt = osg::Vec3f(0, 0, 104);
                    break;
                case ViewMode::Top:
                    position = osg::Vec3f(0, -230, 310);
                    lookAt = osg::Vec3f(0, 0, 112);
                    break;
            }
            Log(Debug::Info) << "FNV/ESM4 proof: inventory paper doll camera "
                             << (mViewMode == ViewMode::Front ? "front"
                                     : (mViewMode == ViewMode::Profile ? "profile" : "top"))
                             << " position=(" << position.x() << "," << position.y() << "," << position.z()
                             << ") lookAt=(" << lookAt.x() << "," << lookAt.y() << "," << lookAt.z() << ")";
        }

        auto viewMatrix = osg::Matrixf::lookAt(position * scale.z(), lookAt * scale.z(), osg::Vec3f(0, 0, 1));
        mRTTNode->setViewMatrix(viewMatrix);
        if (mFalloutPreviewRef != nullptr)
            update();
    }

    // --------------------------------------------------------------------------------------------------

    RaceSelectionPreview::RaceSelectionPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem)
        : CharacterPreview(
            parent, resourceSystem, MWMechanics::getPlayer(), 512, 512, osg::Vec3f(0, 125, 8), osg::Vec3f(0, 0, 8))
        , mBase(*mCharacter.get<ESM::NPC>()->mBase)
        , mRef(ESM::makeBlankCellRef(), &mBase)
        , mPitchRadians(osg::DegreesToRadians(6.f))
    {
        mCharacter = MWWorld::Ptr(&mRef, nullptr);
    }

    RaceSelectionPreview::~RaceSelectionPreview() {}

    void RaceSelectionPreview::setAngle(float angleRadians)
    {
        mNode->setAttitude(osg::Quat(mPitchRadians, osg::Vec3(1, 0, 0)) * osg::Quat(angleRadians, osg::Vec3(0, 0, 1)));
        redraw();
    }

    void RaceSelectionPreview::setPrototype(const ESM::NPC& proto)
    {
        mBase = proto;
        mBase.mId = ESM::RefId::stringRefId("Player");
        rebuild();
    }

    class UpdateCameraCallback : public SceneUtil::NodeCallback<UpdateCameraCallback, CharacterPreviewRTTNode*>
    {
    public:
        UpdateCameraCallback(
            osg::ref_ptr<const osg::Node> nodeToFollow, const osg::Vec3& posOffset, const osg::Vec3& lookAtOffset)
            : mNodeToFollow(std::move(nodeToFollow))
            , mPosOffset(posOffset)
            , mLookAtOffset(lookAtOffset)
        {
        }

        void operator()(CharacterPreviewRTTNode* node, osg::NodeVisitor* nv)
        {
            // Update keyframe controllers in the scene graph first...
            traverse(node, nv);

            // Now update camera utilizing the updated head position
            osg::NodePathList nodepaths = mNodeToFollow->getParentalNodePaths();
            if (nodepaths.empty())
                return;
            osg::Matrix worldMat = osg::computeLocalToWorld(nodepaths[0]);
            osg::Vec3 headOffset = worldMat.getTrans();

            auto viewMatrix
                = osg::Matrixf::lookAt(headOffset + mPosOffset, headOffset + mLookAtOffset, osg::Vec3(0, 0, 1));
            node->setViewMatrix(viewMatrix);
        }

    private:
        osg::ref_ptr<const osg::Node> mNodeToFollow;
        osg::Vec3 mPosOffset;
        osg::Vec3 mLookAtOffset;
    };

    void RaceSelectionPreview::onSetup()
    {
        CharacterPreview::onSetup();
        mAnimation->play("idle", 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", 0.0f, 0);
        mAnimation->runAnimation(0.f);

        // attach camera to follow the head node
        if (mUpdateCameraCallback)
            mRTTNode->removeUpdateCallback(mUpdateCameraCallback);

        const osg::Node* head = mAnimation->getNode("Bip01 Head");
        if (head)
        {
            mUpdateCameraCallback = new UpdateCameraCallback(head, mPosition, mLookAt);
            mRTTNode->addUpdateCallback(mUpdateCameraCallback);
        }
        else
            Log(Debug::Error) << "Error: Bip01 Head node not found";
    }

    // --------------------------------------------------------------------------------------------------

    FalloutActorPreview::FalloutActorPreview(
        osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character, ViewMode viewMode)
        : CharacterPreview(parent, resourceSystem, MWWorld::Ptr(character.getBase(), nullptr), 720, 720,
            osg::Vec3f(0, 420, 112), osg::Vec3f(0, 0, 112))
        , mViewMode(viewMode)
    {
        mNode->setName("FNV Neutral Actor Preview");
    }

    osg::ref_ptr<Animation> FalloutActorPreview::createAnimation()
    {
        if (mCharacter.getType() == ESM::REC_NPC_4)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview using ESM4NpcAnimation for "
                             << mCharacter.toString();
            return new ESM4NpcAnimation(mCharacter, mNode, mResourceSystem);
        }
        if (mCharacter.getType() == ESM::REC_CREA4)
        {
            const std::string rawModel(mCharacter.getClass().getModel(mCharacter));
            std::string animationModel;
            if (!rawModel.empty())
                animationModel = Misc::ResourceHelpers::correctActorModelPath(
                    VFS::Path::toNormalized(rawModel), mResourceSystem->getVFS());
            const bool animated = !animationModel.empty()
                && !(animationModel == rawModel && Misc::StringUtils::ciEndsWith(animationModel, ".nif"));

            Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview using CreatureAnimation for "
                             << mCharacter.toString() << " model=\"" << rawModel << "\" correctedModel=\""
                             << animationModel << "\" animated=" << animated
                             << " classification=visual-preview-supported source=neutral-preview";
            return new CreatureAnimation(mCharacter, animationModel, mResourceSystem, animated, mNode);
        }

        Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview using base CharacterPreview animation for "
                         << mCharacter.toString();
        return CharacterPreview::createAnimation();
    }

    void FalloutActorPreview::onSetup()
    {
        CharacterPreview::onSetup();

        osg::Vec3f scale(1.f, 1.f, 1.f);
        mCharacter.getClass().adjustScale(mCharacter, scale, true);
        mNode->setScale(scale);

        osg::Vec3f position(0.f, 420.f, 112.f);
        osg::Vec3f lookAt(0.f, 0.f, 112.f);
        const char* viewName = "front";
        const std::string_view profile = getFalloutNeutralActorPreviewProfile();
        if (Misc::StringUtils::ciEqual(profile, "full-body") || Misc::StringUtils::ciEqual(profile, "fullbody"))
        {
            applyFalloutNeutralActorOrbitCamera(mViewMode,
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_DISTANCE", 760.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_CAMERA_Z", 78.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_LOOK_Z", 78.f), position, lookAt,
                viewName);
        }
        else if (Misc::StringUtils::ciEqual(profile, "face"))
        {
            applyFalloutNeutralActorOrbitCamera(mViewMode,
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_DISTANCE", 190.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_CAMERA_Z", 116.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_LOOK_Z", 116.f), position, lookAt,
                viewName);
        }
        else if (Misc::StringUtils::ciEqual(profile, "hands"))
        {
            switch (mViewMode)
            {
                case ViewMode::Front:
                    position = osg::Vec3f(0.f, 280.f, 91.f);
                    lookAt = osg::Vec3f(0.f, 0.f, 91.f);
                    viewName = "hands-wide";
                    break;
                case ViewMode::FrontLeft:
                    position = osg::Vec3f(-48.f, 190.f, 90.f);
                    lookAt = osg::Vec3f(-42.f, 0.f, 90.f);
                    viewName = "left-hand";
                    break;
                case ViewMode::FrontRight:
                    position = osg::Vec3f(48.f, 190.f, 90.f);
                    lookAt = osg::Vec3f(42.f, 0.f, 90.f);
                    viewName = "right-hand";
                    break;
            }
        }
        else if (Misc::StringUtils::ciEqual(profile, "audit") || Misc::StringUtils::ciEqual(profile, "bot-audit"))
        {
            switch (mViewMode)
            {
                case ViewMode::Front:
                    position = osg::Vec3f(0.f, 760.f, 78.f);
                    lookAt = osg::Vec3f(0.f, 0.f, 78.f);
                    viewName = "full-body";
                    break;
                case ViewMode::FrontLeft:
                    position = osg::Vec3f(0.f, 260.f, 118.f);
                    lookAt = osg::Vec3f(0.f, 0.f, 118.f);
                    viewName = "face-hat";
                    break;
                case ViewMode::FrontRight:
                    position = osg::Vec3f(28.f, 340.f, 128.f);
                    lookAt = osg::Vec3f(28.f, 24.f, 120.f);
                    viewName = "right-hand-weapon";
                    break;
            }
        }
        else
        {
            applyFalloutNeutralActorOrbitCamera(mViewMode,
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_DISTANCE", 420.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_CAMERA_Z", 112.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_LOOK_Z", 112.f), position, lookAt,
                viewName);
        }

        mRTTNode->setViewMatrix(osg::Matrixf::lookAt(position * scale.z(), lookAt * scale.z(), osg::Vec3f(0, 0, 1)));

        const std::string animationGroup = getFalloutPreviewAnimationGroup();
        const float previewStart = getFalloutPreviewAnimationStartPoint();
        if (mAnimation)
        {
            if (mAnimation->hasAnimation(animationGroup))
                mAnimation->play(animationGroup, 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", previewStart,
                    std::numeric_limits<uint32_t>::max(), true);
            mAnimation->runAnimation(0.0f);
        }
        setRedrawSimulationTime(previewStart);

        if (Misc::StringUtils::ciEqual(profile, "audit") || Misc::StringUtils::ciEqual(profile, "bot-audit"))
        {
            std::unique_ptr<FalloutActorPreviewPartMaskVisitor> partMask;
            if (mViewMode == ViewMode::FrontLeft)
                partMask = std::make_unique<FalloutActorPreviewPartMaskVisitor>(
                    FalloutActorPreviewPartMaskVisitor::Mode::FaceHeadgear);
            else if (mViewMode == ViewMode::FrontRight)
                partMask = std::make_unique<FalloutActorPreviewPartMaskVisitor>(
                    FalloutActorPreviewPartMaskVisitor::Mode::RightHandWeapon);

            if (partMask != nullptr)
            {
                mNode->accept(*partMask);
                Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview part mask view=" << viewName
                                 << " kept=" << partMask->getKept() << " masked=" << partMask->getMasked()
                                 << " runtime=runtime-supported gate=runtime-neutral-actor-preview";
            }
        }

        Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview camera view=" << viewName << " position=("
                         << position.x() << "," << position.y() << "," << position.z() << ") lookAt=("
                         << lookAt.x() << "," << lookAt.y() << "," << lookAt.z() << ") profile=" << profile
                         << " animationGroup=" << animationGroup << " startPoint=" << previewStart
                         << " simulationTime=" << previewStart
                         << " runtime=runtime-supported gate=runtime-neutral-actor-preview";
    }

}
