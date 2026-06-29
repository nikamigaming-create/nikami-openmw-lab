#include "standalonemodelpreview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include <osg/Camera>
#include <osg/ComputeBoundsVisitor>
#include <osg/Fog>
#include <osg/Geode>
#include <osg/Group>
#include <osg/Light>
#include <osg/LineWidth>
#include <osg/Material>
#include <osg/Math>
#include <osg/PolygonMode>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
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
#include <components/vfs/manager.hpp>

#include "util.hpp"
#include "vismask.hpp"

namespace MWRender
{
    class StandaloneModelDrawOnceCallback : public SceneUtil::NodeCallback<StandaloneModelDrawOnceCallback>
    {
    public:
        explicit StandaloneModelDrawOnceCallback(osg::Node* subgraph)
            : mSubgraph(subgraph)
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            if (!mRendered)
            {
                mRendered = true;

                osg::ref_ptr<osg::FrameStamp> previousFramestamp
                    = const_cast<osg::FrameStamp*>(nv->getFrameStamp());
                osg::FrameStamp* fs = new osg::FrameStamp(*previousFramestamp);
                fs->setSimulationTime(0.0);
                nv->setFrameStamp(fs);

                mSubgraph->accept(*nv);
                traverse(node, nv);

                nv->setFrameStamp(previousFramestamp);
            }
            else
                node->setNodeMask(0);
        }

        void redrawNextFrame() { mRendered = false; }

    private:
        bool mRendered = false;
        osg::ref_ptr<osg::Node> mSubgraph;
    };

    class StandaloneModelRTTNode : public SceneUtil::RTTNode
    {
    public:
        StandaloneModelRTTNode(std::uint32_t sizeX, std::uint32_t sizeY, const osg::Vec4f& clearColor)
            : RTTNode(sizeX, sizeY, Settings::video().mAntialiasing, false, 0,
                StereoAwareness::Unaware_MultiViewShaders, shouldAddMSAAIntermediateTarget())
            , mAspectRatio(static_cast<float>(sizeX) / static_cast<float>(std::max<std::uint32_t>(sizeY, 1)))
            , mClearColor(clearColor)
        {
            if (SceneUtil::AutoDepth::isReversed())
                mPerspectiveMatrix = static_cast<osg::Matrixf>(
                    SceneUtil::getReversedZProjectionMatrixAsPerspective(35.f, mAspectRatio, 4.f, 10000.f));
            else
                mPerspectiveMatrix = osg::Matrixf::perspective(35.f, mAspectRatio, 4.f, 10000.f);
            setColorBufferInternalFormat(GL_RGBA);
            setDepthBufferInternalFormat(GL_DEPTH24_STENCIL8);
        }

        void setDefaults(osg::Camera* camera) override
        {
            camera->setName("StandaloneModelPreview");
            camera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
            camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT, osg::Camera::PIXEL_BUFFER_RTT);
            camera->setClearColor(mClearColor);
            camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            camera->setProjectionMatrix(mPerspectiveMatrix);
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
            camera->setProjectionMatrix(mPerspectiveMatrix);
            camera->setViewMatrix(mViewMatrix);

            if (shouldDoTextureArray())
                Stereo::setMultiviewMatrices(mGroup->getOrCreateStateSet(), { mPerspectiveMatrix, mPerspectiveMatrix });
        }

        void setViewMatrix(const osg::Matrixf& viewMatrix) { mViewMatrix = viewMatrix; }
        void addChild(osg::Node* node) { mGroup->addChild(node); }

        osg::ref_ptr<osg::Group> mGroup = new osg::Group;

    private:
        float mAspectRatio = 1.f;
        osg::Vec4f mClearColor;
        osg::Matrixf mPerspectiveMatrix;
        osg::Matrixf mViewMatrix = osg::Matrixf::identity();
    };

    namespace
    {
        bool getStandaloneModelPreviewBool(const char* name)
        {
            return std::getenv(name) != nullptr;
        }

        void applyStandaloneModelWireframeState(osg::StateSet& stateSet)
        {
            stateSet.setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet.setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet.setAttributeAndModes(
                new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::LINE),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateSet.setAttributeAndModes(new osg::LineWidth(3.f), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }

        class StandaloneModelWireframeVisitor : public osg::NodeVisitor
        {
        public:
            StandaloneModelWireframeVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                {
                    osg::Drawable* drawable = geode.getDrawable(i);
                    if (drawable == nullptr)
                        continue;
                    applyStandaloneModelWireframeState(*drawable->getOrCreateStateSet());
                    ++mDrawables;
                }
                traverse(geode);
            }

            unsigned int mDrawables = 0;
        };

        VFS::Path::Normalized resolveModel(Resource::ResourceSystem* resourceSystem, const std::string& model)
        {
            if (model.empty())
                throw std::runtime_error("standalone model preview requires a model path");

            VFS::Path::Normalized correctedModel = Misc::ResourceHelpers::correctMeshPath(VFS::Path::toNormalized(model));
            if (!resourceSystem->getVFS()->exists(correctedModel))
            {
                const VFS::Path::Normalized rawModel(VFS::Path::toNormalized(model));
                if (resourceSystem->getVFS()->exists(rawModel))
                    correctedModel = rawModel;
            }
            if (!resourceSystem->getVFS()->exists(correctedModel))
                throw std::runtime_error("standalone model preview could not resolve " + model);
            return correctedModel;
        }
    }

    StandaloneModelPreview::StandaloneModelPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem,
        const StandaloneModelPreviewSettings& settings)
        : mParent(parent)
        , mResourceSystem(resourceSystem)
        , mSettings(settings)
    {
        if (mParent == nullptr)
            throw std::runtime_error("standalone model preview requires a parent node");
        if (mResourceSystem == nullptr)
            throw std::runtime_error("standalone model preview requires a resource system");

        rebuild(settings);
    }

    StandaloneModelPreview::~StandaloneModelPreview()
    {
        if (mParent && mRTTNode)
            mParent->removeChild(mRTTNode);
    }

    void StandaloneModelPreview::rebuild(const StandaloneModelPreviewSettings& settings)
    {
        if (mParent && mRTTNode)
            mParent->removeChild(mRTTNode);

        mSettings = settings;
        mState = {};
        mRTTNode = new StandaloneModelRTTNode(mSettings.mWidth, mSettings.mHeight, mSettings.mClearColor);
        mRTTNode->setNodeMask(Mask_RenderToTexture);
        setupScene();
        mParent->addChild(mRTTNode);
        redraw();
    }

    void StandaloneModelPreview::setupScene()
    {
        mState.mCorrectedModel = resolveModel(mResourceSystem, mSettings.mModel);
        osg::ref_ptr<osg::Node> model = mResourceSystem->getSceneManager()->getInstance(mState.mCorrectedModel);
        model->setNodeMask(Mask_Object);

        osg::ComputeBoundsVisitor computeBoundsVisitor;
        computeBoundsVisitor.setTraversalMask(~(Mask_ParticleSystem | Mask_Effect));
        model->accept(computeBoundsVisitor);
        mState.mBounds = computeBoundsVisitor.getBoundingBox();
        mState.mBoundsValid = mState.mBounds.valid();
        if (mState.mBoundsValid)
        {
            mState.mBoundsCenter = mState.mBounds.center();
            mState.mBoundsSize = osg::Vec3f(mState.mBounds.xMax() - mState.mBounds.xMin(),
                mState.mBounds.yMax() - mState.mBounds.yMin(), mState.mBounds.zMax() - mState.mBounds.zMin());
            mState.mFrameRadius = std::max(
                { mState.mBoundsSize.x(), mState.mBoundsSize.y(), mState.mBoundsSize.z(), 16.f });
        }

        osg::ref_ptr<SceneUtil::LightManager> lightManager = new SceneUtil::LightManager(
            SceneUtil::LightSettings{
                .mClusteredLighting = Settings::shaders().mClusteredLighting,
                .mMaxLights = Settings::shaders().mMaxLights,
                .mMaximumLightDistance = Settings::shaders().mMaximumLightDistance,
                .mLightFadeStart = Settings::shaders().mLightFadeStart,
                .mLightRadiusMultiplier = Settings::shaders().mLightRadiusMultiplier,
            },
            mResourceSystem);
        osg::ref_ptr<osg::StateSet> stateset = lightManager->getOrCreateStateSet();
        stateset->setDefine("FORCE_OPAQUE", "1", osg::StateAttribute::ON);
        stateset->setMode(GL_NORMALIZE, osg::StateAttribute::ON);

        osg::ref_ptr<osg::Material> material = new osg::Material;
        material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
        material->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        material->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        material->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0, 0, 0, 0));
        stateset->setAttribute(material);

        osg::ref_ptr<osg::Fog> fog = new osg::Fog;
        fog->setStart(10000000);
        fog->setEnd(10000000);
        stateset->setAttributeAndModes(fog, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        stateset->addUniform(new osg::Uniform("far", 10000000.f));
        stateset->addUniform(new osg::Uniform("skyBlendingStart", 8000000.f));
        stateset->addUniform(new osg::Uniform("screenRes",
            osg::Vec2f(static_cast<float>(mSettings.mWidth), static_cast<float>(mSettings.mHeight))));
        stateset->addUniform(new osg::Uniform("emissiveMult", 1.f));
        SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*stateset);

        osg::ref_ptr<osg::Light> light = new osg::Light;
        light->setPosition(osg::Vec4(-0.35f, 0.55f, 0.75f, 0.f));
        light->setDiffuse(osg::Vec4(0.95f, 0.95f, 0.95f, 1.f));
        light->setAmbient(osg::Vec4(0.42f, 0.42f, 0.42f, 1.f));
        light->setSpecular(osg::Vec4(0.f, 0.f, 0.f, 0.f));
        light->setConstantAttenuation(1.f);
        lightManager->setSunlight(light);

        mModelNode = new osg::PositionAttitudeTransform;
        mModelNode->setScale(osg::Vec3f(mSettings.mScale, mSettings.mScale, mSettings.mScale));
        mModelNode->setAttitude(osg::Quat(osg::DegreesToRadians(mSettings.mRotation.x()), osg::Vec3f(1.f, 0.f, 0.f),
            osg::DegreesToRadians(mSettings.mRotation.y()), osg::Vec3f(0.f, 1.f, 0.f),
            osg::DegreesToRadians(mSettings.mRotation.z()), osg::Vec3f(0.f, 0.f, 1.f)));
        mModelNode->setPosition(-mState.mBoundsCenter * mSettings.mScale);
        mModelNode->addChild(model);

        if (getStandaloneModelPreviewBool("OPENMW_FNV_ACTOR_PREVIEW_WIREFRAME"))
        {
            applyStandaloneModelWireframeState(*mModelNode->getOrCreateStateSet());
            StandaloneModelWireframeVisitor wireframeVisitor;
            mModelNode->accept(wireframeVisitor);
            Log(Debug::Info) << "FNV/ESM4 standalone model preview wireframe model=\"" << mSettings.mModel
                             << "\" correctedModel=\"" << mState.mCorrectedModel
                             << "\" drawables=" << wireframeVisitor.mDrawables
                             << " runtime=runtime-supported gate=runtime-neutral-rtt-preview-wireframe";
        }

        lightManager->addChild(mModelNode);
        mRTTNode->addChild(lightManager);

        const osg::Vec3f lookAt = mSettings.mCameraPan;
        osg::Vec3f cameraDirection = mSettings.mCameraDirection;
        if (cameraDirection.normalize() == 0.f)
            cameraDirection = osg::Vec3f(0.f, 1.f, 0.f);
        if (std::isfinite(mSettings.mCameraTiltDegrees) && std::abs(mSettings.mCameraTiltDegrees) > 0.001f)
        {
            osg::Vec3f right = osg::Vec3f(0.f, 0.f, 1.f) ^ cameraDirection;
            if (right.normalize() == 0.f)
                right = osg::Vec3f(1.f, 0.f, 0.f);
            cameraDirection = osg::Quat(osg::DegreesToRadians(mSettings.mCameraTiltDegrees), right) * cameraDirection;
            cameraDirection.normalize();
        }
        const float cameraDistanceMultiplier = std::clamp(mSettings.mCameraDistanceMultiplier, 0.15f, 12.f);
        const float cameraDistance = std::max(48.f, mState.mFrameRadius * mSettings.mScale * 3.1f)
            * cameraDistanceMultiplier;
        mState.mLookAt = lookAt;
        mState.mCameraPosition = lookAt + cameraDirection * cameraDistance;
        mRTTNode->setViewMatrix(osg::Matrixf::lookAt(mState.mCameraPosition, mState.mLookAt, osg::Vec3f(0.f, 0.f, 1.f)));

        mDrawOnceCallback = new StandaloneModelDrawOnceCallback(mRTTNode->mGroup);
        mRTTNode->addUpdateCallback(mDrawOnceCallback);

        Log(Debug::Info) << "FNV/ESM4 standalone model preview prepared: model=\"" << mSettings.mModel
                         << "\" correctedModel=\"" << mState.mCorrectedModel << "\" boundsValid="
                         << mState.mBoundsValid << " frameRadius=" << mState.mFrameRadius << " cameraPos=("
                         << mState.mCameraPosition.x() << "," << mState.mCameraPosition.y() << ","
                         << mState.mCameraPosition.z() << ") gate=runtime-neutral-rtt-preview";
    }

    void StandaloneModelPreview::redraw()
    {
        if (mRTTNode)
            mRTTNode->setNodeMask(Mask_RenderToTexture);
        if (mDrawOnceCallback)
            mDrawOnceCallback->redrawNextFrame();
    }

    osg::Texture2D* StandaloneModelPreview::getTexture()
    {
        if (!mRTTNode)
            return nullptr;
        return static_cast<osg::Texture2D*>(mRTTNode->getColorTexture(nullptr));
    }
}
