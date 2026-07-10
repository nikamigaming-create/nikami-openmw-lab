#include "sky.hpp"

#include <osg/Depth>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/PositionAttitudeTransform>

#include <osgParticle/BoxPlacer>
#include <osgParticle/ModularEmitter>
#include <osgParticle/ModularProgram>
#include <osgParticle/Operator>
#include <osgParticle/ParticleSystemUpdater>

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include <components/settings/values.hpp>

#include <components/debug/debuglog.hpp>

#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/visitor.hpp>

#include <components/resource/imagemanager.hpp>
#include <components/resource/scenemanager.hpp>

#include <components/vfs/manager.hpp>

#include <components/misc/resourcehelpers.hpp>
#include <components/stereo/stereomanager.hpp>

#include <components/nifosg/particle.hpp>

#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/weather.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "renderbin.hpp"
#include "skyutil.hpp"
#include "util.hpp"
#include "vismask.hpp"

namespace
{
    bool logMissingSkyAssets()
    {
        return std::getenv("OPENMW_FNV_SKY_MISSING_LOG") != nullptr;
    }

    bool isFalloutSkyMesh(VFS::Path::NormalizedView model)
    {
        const std::string_view value = model.value();
        return value.rfind("meshes/sky/", 0) == 0;
    }

    bool hasConfiguredFalloutSkyModels()
    {
        return isFalloutSkyMesh(Settings::models().mSkyatmosphere.get())
            || isFalloutSkyMesh(Settings::models().mSkyclouds.get())
            || isFalloutSkyMesh(Settings::models().mSkynight01.get())
            || isFalloutSkyMesh(Settings::models().mSkynight02.get());
    }

    bool hasAvailableConfiguredFalloutSkyModels(Resource::SceneManager& sceneManager)
    {
        const VFS::Manager* vfs = sceneManager.getVFS();
        if (vfs == nullptr || !hasConfiguredFalloutSkyModels())
            return false;

        return (isFalloutSkyMesh(Settings::models().mSkyatmosphere.get())
                   && vfs->exists(Settings::models().mSkyatmosphere.get()))
            || (isFalloutSkyMesh(Settings::models().mSkyclouds.get())
                && vfs->exists(Settings::models().mSkyclouds.get()))
            || (isFalloutSkyMesh(Settings::models().mSkynight01.get())
                && vfs->exists(Settings::models().mSkynight01.get()))
            || (isFalloutSkyMesh(Settings::models().mSkynight02.get())
                && vfs->exists(Settings::models().mSkynight02.get()));
    }

    void logInterpretedFalloutSkyMaterial(std::string_view label, VFS::Path::NormalizedView model,
        std::string_view skyPass, std::string_view vertexAlphaMode, std::string_view vertexColorRgbMode)
    {
        Log(Debug::Info) << "FNV/ESM4: interpreted sky material " << label << " (" << model.value()
                         << ") nativeMaterial=0 skyProgram=sky skyPass=" << skyPass
                         << " updatersAttached=1 vertexAlpha=" << vertexAlphaMode
                         << " vertexColorRgb=" << vertexColorRgbMode;
    }

    void logFalloutSkyCloudUvMode(std::string_view label, VFS::Path::NormalizedView model)
    {
        Log(Debug::Info) << "FNV/ESM4: cloud texture coordinates " << label << " (" << model.value()
                         << ") mode=openmw-stock repeat-wrap uv-scroll=stock-negative runtime-supported";
    }

    VFS::Path::Normalized chooseExistingSkyTexture(
        const VFS::Manager* vfs, std::initializer_list<std::string_view> candidates, std::string_view fallback)
    {
        if (vfs != nullptr)
        {
            for (std::string_view candidate : candidates)
            {
                VFS::Path::Normalized normalized{ std::string(candidate) };
                if (vfs->exists(normalized))
                    return normalized;
            }
        }

        return VFS::Path::Normalized{ std::string(fallback) };
    }

    VFS::Path::Normalized resolveWeatherCloudTexture(
        const VFS::Manager* vfs, VFS::Path::NormalizedView requested)
    {
        if (vfs != nullptr && vfs->exists(requested))
            return VFS::Path::Normalized(requested);

        const std::string_view value = requested.value();

        if (value.find("clear") != std::string_view::npos)
        {
            return chooseExistingSkyTexture(vfs,
                { "textures/sky/nvskyclouds.dds", "textures/sky/cloudsclear.dds",
                    "textures/sky/cloudsclear02.dds", "textures/sky/skyrimcloudsupper01.dds",
                    "textures/sky/cloudsupper01_d.dds", "textures/sky/wastelandcloudcloudyupper01.dds" },
                value);
        }

        if (value.find("cloudy") != std::string_view::npos)
        {
            return chooseExistingSkyTexture(vfs,
                { "textures/sky/nvskyclouds.dds", "textures/sky/cloudscloudy.dds",
                    "textures/sky/wastelandcloudcloudyupper01.dds", "textures/sky/skyrimcloudsupper02.dds",
                    "textures/sky/cloudsupper02_d.dds" },
                value);
        }

        if (value.find("fog") != std::string_view::npos)
        {
            return chooseExistingSkyTexture(vfs,
                { "textures/sky/cloudsfog.dds", "textures/sky/cloudsfoglower.dds",
                    "textures/sky/wastelandcloudhorizon01.dds", "textures/sky/skyrimcloudshorizon01.dds",
                    "textures/sky/cloudshorizon01_d.dds" },
                value);
        }

        if (value.find("thunder") != std::string_view::npos)
        {
            return chooseExistingSkyTexture(vfs,
                { "textures/sky/cloudsthunderstorm.dds", "textures/sky/wastelandcloudcloudyupper01.dds",
                    "textures/sky/skyrimcloudsupper04.dds", "textures/sky/cloudsupper04_d.dds" },
                value);
        }

        if (value.find("rain") != std::string_view::npos)
        {
            return chooseExistingSkyTexture(vfs,
                { "textures/sky/cloudsrain.dds", "textures/sky/wastelandcloudcloudyupper01.dds",
                    "textures/sky/skyrimcloudsupper04.dds", "textures/sky/cloudsupper04_d.dds" },
                value);
        }

        if (value.find("overcast") != std::string_view::npos)
        {
            return chooseExistingSkyTexture(vfs,
                { "textures/sky/cloudsovercast.dds", "textures/sky/urbancloudovercastupper01.dds",
                    "textures/sky/skyrimcloudsupper03.dds", "textures/sky/cloudsupper03_d.dds" },
                value);
        }

        if (value.find("snow") != std::string_view::npos || value.find("blizzard") != std::string_view::npos)
        {
            return chooseExistingSkyTexture(vfs,
                { "textures/sky/cloudssnow.dds", "textures/sky/skyrimcloudsupper04.dds",
                    "textures/sky/cloudsupper04_d.dds", "textures/sky/wastelandcloudcloudyupper01.dds" },
                value);
        }

        return chooseExistingSkyTexture(vfs,
            { "textures/sky/nvskyclouds.dds", "textures/sky/cloudsclear.dds",
                "textures/sky/skyrimcloudsupper01.dds", "textures/sky/cloudsupper01_d.dds",
                "textures/sky/wastelandcloudcloudyupper01.dds" },
            value);
    }

    void pushFirstExistingSkyTexture(Resource::SceneManager& sceneManager,
        std::vector<VFS::Path::Normalized>& textures, std::initializer_list<std::string_view> candidates,
        std::string_view label)
    {
        const VFS::Manager* vfs = sceneManager.getVFS();
        if (vfs != nullptr)
        {
            for (std::string_view candidate : candidates)
            {
                VFS::Path::Normalized normalized{ std::string(candidate) };
                if (vfs->exists(normalized))
                {
                    textures.push_back(normalized);
                    return;
                }
            }
        }

        if (logMissingSkyAssets())
            Log(Debug::Info) << "FNV/ESM4: skipped preload for missing isolated sky texture " << label;
    }

    float falloutSkyMeshScaleMultiplier()
    {
        if (const char* value = std::getenv("OPENMW_FNV_SKY_MESH_SCALE"))
        {
            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end != value && parsed > 0.f)
                return parsed;
        }

        return 1.f;
    }

    float nativeSkyTargetRadius()
    {
        if (const char* value = std::getenv("OPENMW_FNV_SKY_TARGET_RADIUS"))
        {
            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end != value && parsed > 1.f)
                return parsed;
        }

        return 1024.f;
    }

    float nativeSkyCloudOpacity()
    {
        if (const char* value = std::getenv("OPENMW_FNV_NATIVE_CLOUD_OPACITY"))
        {
            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end != value)
                return std::clamp(parsed, 0.f, 1.f);
        }

        return 0.f;
    }

    struct FalloutSkyMeshFit
    {
        float mRadius = 0.f;
        float mViewDistance = 0.f;
        float mTargetRadius = 0.f;
        float mScale = 0.f;
        bool mApplied = false;
    };

    struct SkyVertexColorStats
    {
        unsigned int mColorArrays = 0;
        unsigned int mSamples = 0;
        osg::Vec4f mMin = osg::Vec4f(
            std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
        osg::Vec4f mMax = osg::Vec4f(
            std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());

        void add(const osg::Vec4f& color)
        {
            ++mSamples;
            for (int i = 0; i < 4; ++i)
            {
                mMin[i] = std::min(mMin[i], color[i]);
                mMax[i] = std::max(mMax[i], color[i]);
            }
        }

        bool hasNonZeroRgb() const
        {
            return mSamples != 0 && std::max({ mMax.r(), mMax.g(), mMax.b() }) > 0.001f;
        }

        bool hasVaryingRgb() const
        {
            if (mSamples == 0)
                return false;

            return std::max({ mMax.r() - mMin.r(), mMax.g() - mMin.g(), mMax.b() - mMin.b() }) > 0.001f;
        }
    };

    struct FalloutAtmosphereAlphaStats
    {
        float mMinZ = 0.f;
        float mMaxZ = 0.f;
        unsigned int mVertexArrays = 0;
        unsigned int mVertexSamples = 0;
        bool mUsedVertexZRange = false;
        bool mApplied = false;
    };

    class SkyVertexZRangeVisitor : public osg::NodeVisitor
    {
    public:
        SkyVertexZRangeVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Geometry& geometry) override
        {
            const osg::Vec3Array* vertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
            if (vertices == nullptr)
                return;

            ++mStats.mVertexArrays;
            for (const osg::Vec3f& vertex : *vertices)
            {
                if (mStats.mVertexSamples == 0)
                {
                    mStats.mMinZ = vertex.z();
                    mStats.mMaxZ = vertex.z();
                }
                else
                {
                    mStats.mMinZ = std::min(mStats.mMinZ, vertex.z());
                    mStats.mMaxZ = std::max(mStats.mMaxZ, vertex.z());
                }
                ++mStats.mVertexSamples;
            }
        }

        const FalloutAtmosphereAlphaStats& getStats() const { return mStats; }

    private:
        FalloutAtmosphereAlphaStats mStats;
    };

    class SkyVertexColorStatsVisitor : public osg::NodeVisitor
    {
    public:
        SkyVertexColorStatsVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Geode& geode) override
        {
            for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
            {
                const osg::Geometry* geometry = dynamic_cast<const osg::Geometry*>(geode.getDrawable(i));
                if (geometry == nullptr)
                    continue;

                const osg::Vec4Array* colors = dynamic_cast<const osg::Vec4Array*>(geometry->getColorArray());
                if (colors == nullptr)
                    continue;

                ++mStats.mColorArrays;
                for (const osg::Vec4f& color : *colors)
                    mStats.add(color);
            }

            traverse(geode);
        }

        const SkyVertexColorStats& getStats() const { return mStats; }

    private:
        SkyVertexColorStats mStats;
    };

    std::string formatVec4(const osg::Vec4f& value)
    {
        std::ostringstream stream;
        stream << value.r() << "," << value.g() << "," << value.b() << "," << value.a();
        return stream.str();
    }

    void logFalloutSkyVertexColorStats(std::string_view label, VFS::Path::NormalizedView model, osg::Node& instance)
    {
        SkyVertexColorStatsVisitor visitor;
        instance.accept(visitor);
        const SkyVertexColorStats& stats = visitor.getStats();
        Log(Debug::Info) << "FNV/ESM4: sky mesh vertex colors " << label << " (" << model.value()
                         << ") colorArrays=" << stats.mColorArrays << " samples=" << stats.mSamples
                         << " rgbNonzero=" << stats.hasNonZeroRgb() << " rgbVarying=" << stats.hasVaryingRgb()
                         << " min=" << (stats.mSamples == 0 ? "none" : formatVec4(stats.mMin))
                         << " max=" << (stats.mSamples == 0 ? "none" : formatVec4(stats.mMax));
    }

    void logFalloutAtmosphereAlphaStats(
        std::string_view label, VFS::Path::NormalizedView model, const FalloutAtmosphereAlphaStats& stats)
    {
        Log(Debug::Info) << "FNV/ESM4: generated atmosphere shader alpha " << label << " (" << model.value()
                         << ") mode=" << (stats.mUsedVertexZRange ? "vertex-z-gradient" : "bound-z-gradient")
                         << " vertexArrays=" << stats.mVertexArrays
                         << " vertexSamples=" << stats.mVertexSamples
                         << " zMin=" << (stats.mApplied ? stats.mMinZ : 0.f)
                         << " zMax=" << (stats.mApplied ? stats.mMaxZ : 0.f)
                         << " alphaMin=0 alphaMax=1 applied=" << stats.mApplied;
    }

    FalloutAtmosphereAlphaStats calculateFalloutAtmosphereAlpha(osg::Node& instance)
    {
        SkyVertexZRangeVisitor visitor;
        instance.accept(visitor);
        FalloutAtmosphereAlphaStats stats = visitor.getStats();
        stats.mUsedVertexZRange = stats.mVertexSamples != 0 && stats.mMaxZ - stats.mMinZ > 0.001f;
        if (stats.mUsedVertexZRange)
        {
            stats.mApplied = true;
            return stats;
        }

        const osg::BoundingSphere bound = instance.getBound();
        if (bound.valid() && bound.radius() > 1.f)
        {
            stats.mMinZ = bound.center().z() - bound.radius();
            stats.mMaxZ = bound.center().z() + bound.radius();
            stats.mApplied = stats.mMaxZ - stats.mMinZ > 0.001f;
        }
        return stats;
    }

    osg::PositionAttitudeTransform* createFalloutSkyMeshRoot(osg::Group* parentNode, VFS::Path::NormalizedView model)
    {
        osg::ref_ptr<osg::PositionAttitudeTransform> root = new osg::PositionAttitudeTransform;
        root->setName(std::string("FNV camera-relative sky mesh: ") + std::string(model.value()));
        root->setCullingActive(false);

        osg::StateSet* stateset = root->getOrCreateStateSet();
        osg::ref_ptr<osg::Depth> depth = new SceneUtil::AutoDepth(osg::Depth::ALWAYS, 0.0, 1.0, false);
        stateset->setAttributeAndModes(depth, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        stateset->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        stateset->setMode(GL_FOG, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        stateset->setRenderBinDetails(MWRender::RenderBin_Sky, "RenderBin", osg::StateSet::OVERRIDE_RENDERBIN_DETAILS);
        stateset->setNestRenderBins(false);

        parentNode->addChild(root);
        return root.release();
    }

    FalloutSkyMeshFit fitFalloutSkyMeshToViewDistance(osg::PositionAttitudeTransform& root, const osg::Node& instance)
    {
        FalloutSkyMeshFit fit;
        const float radius = static_cast<float>(instance.getBound().radius());
        if (radius <= 1.f)
            return fit;

        const float viewDistance = Settings::camera().mViewingDistance.get();
        const float targetRadius = nativeSkyTargetRadius();
        const float scale = (targetRadius / radius) * falloutSkyMeshScaleMultiplier();
        root.setScale(osg::Vec3f(scale, scale, scale));
        fit.mRadius = radius;
        fit.mViewDistance = viewDistance;
        fit.mTargetRadius = targetRadius;
        fit.mScale = scale;
        fit.mApplied = true;
        return fit;
    }

    osg::ref_ptr<osg::Node> getOptionalSkyInstance(Resource::SceneManager& sceneManager,
        VFS::Path::NormalizedView model, osg::Group* parentNode, std::string_view label)
    {
        if (!sceneManager.getVFS()->exists(model))
        {
            if (logMissingSkyAssets())
                Log(Debug::Info) << "FNV/ESM4: skipped missing OpenMW sky mesh " << label << " (" << model.value()
                                 << ")";
            return nullptr;
        }

        if (isFalloutSkyMesh(model))
        {
            osg::PositionAttitudeTransform* skyMeshRoot = createFalloutSkyMeshRoot(parentNode, model);
            osg::ref_ptr<osg::Node> instance = sceneManager.getInstance(model, skyMeshRoot);
            const FalloutSkyMeshFit fit = fitFalloutSkyMeshToViewDistance(*skyMeshRoot, *instance);
            if (logMissingSkyAssets())
            {
                Log(Debug::Info) << "FNV/ESM4: wrapped sky mesh " << label << " (" << model.value()
                                 << ") radius=" << instance->getBound().radius() << " viewDistance="
                                 << fit.mViewDistance << " targetRadius=" << fit.mTargetRadius
                                 << " scale=" << skyMeshRoot->getScale().x() << " fitApplied=" << fit.mApplied;
                logFalloutSkyVertexColorStats(label, model, *instance);
            }
            return instance;
        }

        return sceneManager.getInstance(model, parentNode);
    }

    void attachSkyNodeIfUnattached(osg::Group& parentNode, osg::Node& node)
    {
        if (node.getNumParents() == 0)
            parentNode.addChild(&node);
    }

    void pushOptionalSkyModel(Resource::SceneManager& sceneManager, std::vector<VFS::Path::Normalized>& models,
        const VFS::Path::Normalized& model, std::string_view label)
    {
        if (!sceneManager.getVFS()->exists(model))
        {
            if (logMissingSkyAssets())
                Log(Debug::Info) << "FNV/ESM4: skipped preload for missing OpenMW sky mesh " << label << " ("
                                 << model.value() << ")";
            return;
        }

        models.push_back(model);
    }

    class WrapAroundOperator : public osgParticle::Operator
    {
    public:
        WrapAroundOperator(osg::Camera* camera, const osg::Vec3& wrapRange)
            : osgParticle::Operator()
            , mCamera(camera)
            , mWrapRange(wrapRange)
            , mHalfWrapRange(mWrapRange / 2.0)
        {
            mPreviousCameraPosition = getCameraPosition();
        }

        osg::Object* cloneType() const override { return nullptr; }

        osg::Object* clone(const osg::CopyOp& op) const override { return nullptr; }

        void operate(osgParticle::Particle* particle, double dt) override {}

        void operateParticles(osgParticle::ParticleSystem* ps, double dt) override
        {
            osg::Vec3 position = getCameraPosition();
            osg::Vec3 positionDifference = position - mPreviousCameraPosition;

            osg::Matrix toWorld, toLocal;

            std::vector<osg::Matrix> worldMatrices = ps->getWorldMatrices();

            if (!worldMatrices.empty())
            {
                toWorld = worldMatrices[0];
                toLocal.invert(toWorld);
            }

            for (int i = 0; i < ps->numParticles(); ++i)
            {
                osgParticle::Particle* p = ps->getParticle(i);
                p->setPosition(toWorld.preMult(p->getPosition()));
                p->setPosition(p->getPosition() - positionDifference);

                for (int j = 0; j < 3; ++j) // wrap-around in all 3 dimensions
                {
                    osg::Vec3 pos = p->getPosition();

                    if (pos[j] < -mHalfWrapRange[j])
                        pos[j] = mHalfWrapRange[j] + fmod(pos[j] - mHalfWrapRange[j], mWrapRange[j]);
                    else if (pos[j] > mHalfWrapRange[j])
                        pos[j] = fmod(pos[j] + mHalfWrapRange[j], mWrapRange[j]) - mHalfWrapRange[j];

                    p->setPosition(pos);
                }

                p->setPosition(toLocal.preMult(p->getPosition()));
            }

            mPreviousCameraPosition = position;
        }

    protected:
        osg::Camera* mCamera;
        osg::Vec3 mPreviousCameraPosition;
        osg::Vec3 mWrapRange;
        osg::Vec3 mHalfWrapRange;

        osg::Vec3 getCameraPosition() { return mCamera->getInverseViewMatrix().getTrans(); }
    };

    class WeatherAlphaOperator : public osgParticle::Operator
    {
    public:
        WeatherAlphaOperator(float& alpha, bool rain)
            : mAlpha(alpha)
            , mIsRain(rain)
        {
        }

        osg::Object* cloneType() const override { return nullptr; }

        osg::Object* clone(const osg::CopyOp& op) const override { return nullptr; }

        void operate(osgParticle::Particle* particle, double dt) override
        {
            constexpr float rainThreshold = 0.6f; // Rain_Threshold?
            float alpha = mIsRain ? mAlpha * rainThreshold : mAlpha;
            particle->setAlphaRange(osgParticle::rangef(alpha, alpha));
        }

    private:
        float& mAlpha;
        bool mIsRain;
    };

    // Updater for alpha value on a node's StateSet. Assumes the node has an existing Material StateAttribute.
    class AlphaFader : public SceneUtil::StateSetUpdater
    {
    public:
        /// @param alpha the variable alpha value is recovered from
        AlphaFader(const float& alpha)
            : mAlpha(alpha)
        {
        }

        void setDefaults(osg::StateSet* stateset) override
        {
            // need to create a deep copy of StateAttributes we will modify
            osg::Material* mat = static_cast<osg::Material*>(stateset->getAttribute(osg::StateAttribute::MATERIAL));
            stateset->setAttribute(osg::clone(mat, osg::CopyOp::DEEP_COPY_ALL), osg::StateAttribute::ON);
        }

        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override
        {
            osg::Material* mat = static_cast<osg::Material*>(stateset->getAttribute(osg::StateAttribute::MATERIAL));
            mat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, mAlpha));
        }

    protected:
        const float& mAlpha;
    };

    // Helper for adding AlphaFaders to a subgraph
    class SetupVisitor : public osg::NodeVisitor
    {
    public:
        SetupVisitor(const float& alpha)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mAlpha(alpha)
        {
        }

        void apply(osg::Node& node) override
        {
            if (osg::StateSet* stateset = node.getStateSet())
            {
                if (stateset->getAttribute(osg::StateAttribute::MATERIAL))
                {
                    SceneUtil::CompositeStateSetUpdater* composite = nullptr;
                    osg::Callback* callback = node.getUpdateCallback();

                    while (callback)
                    {
                        composite = dynamic_cast<SceneUtil::CompositeStateSetUpdater*>(callback);
                        if (composite)
                            break;

                        callback = callback->getNestedCallback();
                    }

                    osg::ref_ptr<AlphaFader> alphaFader = new AlphaFader(mAlpha);

                    if (composite)
                        composite->addController(alphaFader);
                    else
                        node.addUpdateCallback(alphaFader);
                }
            }

            traverse(node);
        }

    private:
        const float& mAlpha;
    };

    class SkyRTT : public SceneUtil::RTTNode
    {
    public:
        SkyRTT(osg::Vec2f size, osg::Group* earlyRenderBinRoot)
            : RTTNode(static_cast<int>(size.x()), static_cast<int>(size.y()), 0, false, 1, StereoAwareness::Aware,
                MWRender::shouldAddMSAAIntermediateTarget())
            , mEarlyRenderBinRoot(earlyRenderBinRoot)
        {
            setDepthBufferInternalFormat(GL_DEPTH24_STENCIL8);
        }

        void setDefaults(osg::Camera* camera) override
        {
            camera->setReferenceFrame(osg::Camera::RELATIVE_RF);
            camera->setName("SkyCamera");
            camera->setNodeMask(MWRender::Mask_RenderToTexture);
            camera->setCullMask(MWRender::Mask_Sky);
            camera->addChild(mEarlyRenderBinRoot);
            SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*camera->getOrCreateStateSet());
        }

    private:
        osg::ref_ptr<osg::Group> mEarlyRenderBinRoot;
    };

}

namespace MWRender
{
    SkyManager::SkyManager(osg::Group* parentNode, osg::Group* rootNode, osg::Camera* camera,
        Resource::SceneManager* sceneManager, bool enableSkyRTT)
        : mSceneManager(sceneManager)
        , mCamera(camera)
        , mAtmosphereNightRoll(0.f)
        , mNativeAtmosphereNight(false)
        , mFalloutAtmosphereDay(false)
        , mCreated(false)
        , mIsStorm(false)
        , mTimescaleClouds(Fallback::Map::getBool("Weather_Timescale_Clouds"))
        , mCloudAnimationTimer(0.f)
        , mStormParticleDirection(MWWorld::Weather::defaultDirection())
        , mStormDirection(MWWorld::Weather::defaultDirection())
        , mClouds()
        , mNextClouds()
        , mCloudBlendFactor(0.f)
        , mCloudSpeed(0.f)
        , mStarsOpacity(0.f)
        , mCloudColour(0.f, 0.f, 0.f, 0.f)
        , mSkyColour(0.f, 0.f, 0.f, 0.f)
        , mSkyLowerColour(0.f, 0.f, 0.f, 0.f)
        , mSkyHorizonColour(0.f, 0.f, 0.f, 0.f)
        , mFogColour(0.f, 0.f, 0.f, 0.f)
        , mLoggedFalloutAtmosphereGradient(false)
        , mRainSpeed(0.f)
        , mRainDiameter(0.f)
        , mRainMinHeight(0.f)
        , mRainMaxHeight(0.f)
        , mRainEntranceSpeed(1.f)
        , mRainMaxRaindrops(0)
        , mRainRipplesEnabled(Fallback::Map::getBool("Weather_Rain_Ripples"))
        , mSnowRipplesEnabled(Fallback::Map::getBool("Weather_Snow_Ripples"))
        , mWindSpeed(0.f)
        , mBaseWindSpeed(0.f)
        , mEnabled(true)
        , mSunglareEnabled(true)
        , mPrecipitationAlpha(0.f)
        , mDirtyParticlesEffect(false)
    {
        mSkyRootNode = new CameraRelativeTransform;
        mSkyRootNode->setName("Sky Root");
        mSceneManager->setUpNormalsRTForStateSet(mSkyRootNode->getOrCreateStateSet(), false);
        SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*mSkyRootNode->getOrCreateStateSet());
        parentNode->addChild(mSkyRootNode);

        mEarlyRenderBinRoot = new osg::Group;
        // render before the world is rendered
        mEarlyRenderBinRoot->getOrCreateStateSet()->setRenderBinDetails(RenderBin_Sky, "RenderBin");
        // Prevent unwanted clipping by water reflection camera's clipping plane
        mEarlyRenderBinRoot->getOrCreateStateSet()->setMode(GL_CLIP_PLANE0, osg::StateAttribute::OFF);

        if (enableSkyRTT)
        {
            mSkyRTT = new SkyRTT(Settings::fog().mSkyRttResolution, mEarlyRenderBinRoot);
            mSkyRootNode->addChild(mSkyRTT);
        }

        mSkyNode = new osg::Group;
        mSkyNode->setNodeMask(Mask_Sky);
        mSkyNode->addChild(mEarlyRenderBinRoot);
        mSkyRootNode->addChild(mSkyNode);

        mUnderwaterSwitch = new UnderwaterSwitchCallback(mSkyRootNode);

        mPrecipitationOcclusion = Settings::shaders().mWeatherParticleOcclusion;
        mPrecipitationOccluder = std::make_unique<PrecipitationOccluder>(mSkyRootNode, parentNode, rootNode, camera);
    }

    void SkyManager::create()
    {
        assert(!mCreated);

        const bool falloutSkyModels = hasAvailableConfiguredFalloutSkyModels(*mSceneManager);
        const bool forceShaders = Settings::shaders().mForceShaders;
        const bool useSkyShader = true;

        mAtmosphereDay = getOptionalSkyInstance(
            *mSceneManager, Settings::models().mSkyatmosphere.get(), mEarlyRenderBinRoot, "day atmosphere");
        if (mAtmosphereDay)
        {
            const bool falloutAtmosphere = isFalloutSkyMesh(Settings::models().mSkyatmosphere.get());
            mFalloutAtmosphereDay = falloutAtmosphere;
            FalloutAtmosphereAlphaStats falloutAlphaStats;
            if (!falloutAtmosphere)
            {
                ModVertexAlphaVisitor modAtmosphere(ModVertexAlphaVisitor::Atmosphere);
                mAtmosphereDay->accept(modAtmosphere);
            }
            else
            {
                falloutAlphaStats = calculateFalloutAtmosphereAlpha(*mAtmosphereDay);
                logFalloutAtmosphereAlphaStats(
                    "day atmosphere", Settings::models().mSkyatmosphere.get(), falloutAlphaStats);
            }

            mAtmosphereUpdater = new AtmosphereUpdater;
            if (falloutAtmosphere && falloutAlphaStats.mApplied)
                mAtmosphereUpdater->setFalloutAtmosphereZGradient(falloutAlphaStats.mMinZ, falloutAlphaStats.mMaxZ);
            mAtmosphereDay->addUpdateCallback(mAtmosphereUpdater);
            if (falloutAtmosphere)
                logInterpretedFalloutSkyMaterial(
                    "day atmosphere", Settings::models().mSkyatmosphere.get(), "atmosphere",
                    "generated-z-gradient", "not-used");
        }

        mAtmosphereNightNode = new osg::PositionAttitudeTransform;
        mAtmosphereNightNode->setNodeMask(0);
        mEarlyRenderBinRoot->addChild(mAtmosphereNightNode);

        osg::ref_ptr<osg::Node> atmosphereNight;
        VFS::Path::Normalized nightAtmosphereModel = Settings::models().mSkynight01.get();
        if (mSceneManager->getVFS()->exists(Settings::models().mSkynight02.get()))
        {
            nightAtmosphereModel = Settings::models().mSkynight02.get();
            atmosphereNight
                = getOptionalSkyInstance(*mSceneManager, nightAtmosphereModel, mAtmosphereNightNode, "night atmosphere");
        }
        else
            atmosphereNight
                = getOptionalSkyInstance(*mSceneManager, nightAtmosphereModel, mAtmosphereNightNode, "night atmosphere");
        if (atmosphereNight)
        {
            atmosphereNight->getOrCreateStateSet()->setAttributeAndModes(
                createAlphaTrackingUnlitMaterial(), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

            const bool falloutNightAtmosphere = isFalloutSkyMesh(nightAtmosphereModel);
            if (!falloutNightAtmosphere)
            {
                ModVertexAlphaVisitor modStars(ModVertexAlphaVisitor::Stars);
                atmosphereNight->accept(modStars);
            }
            mAtmosphereNightUpdater = new AtmosphereNightUpdater(mSceneManager->getImageManager());
            atmosphereNight->addUpdateCallback(mAtmosphereNightUpdater);
            if (falloutNightAtmosphere)
                logInterpretedFalloutSkyMaterial(
                    "night atmosphere", nightAtmosphereModel, "atmosphere-night", "texture-alpha", "not-used");
        }

        mSun = std::make_unique<Sun>(mEarlyRenderBinRoot, *mSceneManager);
        mSun->setSunglare(mSunglareEnabled);
        mMasser = std::make_unique<Moon>(
            mEarlyRenderBinRoot, *mSceneManager, Fallback::Map::getFloat("Moons_Masser_Size") / 125, Moon::Type_Masser);
        mSecunda = std::make_unique<Moon>(mEarlyRenderBinRoot, *mSceneManager,
            Fallback::Map::getFloat("Moons_Secunda_Size") / 125, Moon::Type_Secunda);

        mCloudNode = new osg::Group;
        mEarlyRenderBinRoot->addChild(mCloudNode);

        mCloudMesh = new osg::PositionAttitudeTransform;
        osg::ref_ptr<osg::Node> cloudMeshChild
            = getOptionalSkyInstance(*mSceneManager, Settings::models().mSkyclouds.get(), mCloudMesh, "clouds");
        if (cloudMeshChild)
        {
            mCloudUpdater = new CloudUpdater();
            const bool falloutClouds = isFalloutSkyMesh(Settings::models().mSkyclouds.get());
            mCloudUpdater->setOpacity(falloutClouds ? nativeSkyCloudOpacity() : 1.f);
            cloudMeshChild->addUpdateCallback(mCloudUpdater);
            if (falloutClouds)
            {
                logInterpretedFalloutSkyMaterial(
                    "clouds", Settings::models().mSkyclouds.get(), "clouds", "texture-alpha", "not-used");
                logFalloutSkyCloudUvMode("clouds", Settings::models().mSkyclouds.get());
            }
            attachSkyNodeIfUnattached(*mCloudMesh, *cloudMeshChild);
        }

        mNextCloudMesh = new osg::PositionAttitudeTransform;
        osg::ref_ptr<osg::Node> nextCloudMeshChild
            = getOptionalSkyInstance(*mSceneManager, Settings::models().mSkyclouds.get(), mNextCloudMesh, "next clouds");
        if (nextCloudMeshChild)
        {
            mNextCloudUpdater = new CloudUpdater();
            const bool falloutClouds = isFalloutSkyMesh(Settings::models().mSkyclouds.get());
            mNextCloudUpdater->setOpacity(0.f);
            nextCloudMeshChild->addUpdateCallback(mNextCloudUpdater);
            if (falloutClouds)
            {
                logInterpretedFalloutSkyMaterial(
                    "next clouds", Settings::models().mSkyclouds.get(), "clouds", "texture-alpha", "not-used");
                logFalloutSkyCloudUvMode("next clouds", Settings::models().mSkyclouds.get());
            }
            attachSkyNodeIfUnattached(*mNextCloudMesh, *nextCloudMeshChild);
        }
        mNextCloudMesh->setNodeMask(0);

        mCloudNode->addChild(mCloudMesh);
        mCloudNode->addChild(mNextCloudMesh);

        if (mCloudUpdater || mNextCloudUpdater)
        {
            if (!isFalloutSkyMesh(Settings::models().mSkyclouds.get()))
            {
                ModVertexAlphaVisitor modClouds(ModVertexAlphaVisitor::Clouds);
                mCloudMesh->accept(modClouds);
                mNextCloudMesh->accept(modClouds);
            }
        }

        if (useSkyShader)
        {
            Shader::ShaderManager::DefineMap defines = {};
            Stereo::shaderStereoDefines(defines);
            auto program = mSceneManager->getShaderManager().getProgram("sky", defines);
            mEarlyRenderBinRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("pass", -1));
            mEarlyRenderBinRoot->getOrCreateStateSet()->setAttributeAndModes(
                program, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }
        if (falloutSkyModels || logMissingSkyAssets())
        {
            Log(Debug::Info) << "FNV/ESM4: sky shader mode forceShaders=" << forceShaders
                             << " falloutSkyModels=" << falloutSkyModels << " program="
                             << (falloutSkyModels && !forceShaders ? "sky-interpreted" : "sky");
        }

        osg::ref_ptr<osg::Depth> depth = new SceneUtil::AutoDepth;
        depth->setWriteMask(false);
        mEarlyRenderBinRoot->getOrCreateStateSet()->setAttributeAndModes(depth);
        mEarlyRenderBinRoot->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
        mEarlyRenderBinRoot->getOrCreateStateSet()->setMode(GL_FOG, osg::StateAttribute::OFF);

        mMoonScriptColor = Fallback::Map::getColour("Moons_Script_Color");

        mCreated = true;
    }

    void SkyManager::createRain()
    {
        if (mRainNode)
            return;

        mRainNode = new osg::Group;

        mRainParticleSystem = new NifOsg::ParticleSystem;
        osg::Vec3 rainRange = osg::Vec3(mRainDiameter, mRainDiameter, (mRainMinHeight + mRainMaxHeight) / 2.f);

        mRainParticleSystem->setParticleAlignment(osgParticle::ParticleSystem::FIXED);
        // Vertical placement with some horizontal compression.
        // Z-down alignment is used so that the UV uses Y-down convention
        mRainParticleSystem->setAlignVectors(osg::Vec3f(0.1f, 0, 0), osg::Vec3f(0, 0, -1.f));

        osg::ref_ptr<osg::StateSet> stateset = mRainParticleSystem->getOrCreateStateSet();

        constexpr VFS::Path::NormalizedView raindropImage("textures/tx_raindrop_01.dds");
        osg::ref_ptr<osg::Texture2D> raindropTex
            = new osg::Texture2D(mSceneManager->getImageManager()->getImage(raindropImage));
        raindropTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        raindropTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

        stateset->setTextureAttributeAndModes(0, raindropTex);
        stateset->setNestRenderBins(false);
        stateset->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
        stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
        stateset->setMode(GL_BLEND, osg::StateAttribute::ON);

        osg::ref_ptr<osg::Material> mat = new osg::Material;
        mat->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        mat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        mat->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
        stateset->setAttributeAndModes(mat);

        osgParticle::Particle& particleTemplate = mRainParticleSystem->getDefaultParticleTemplate();
        particleTemplate.setSizeRange(osgParticle::rangef(5.f, 15.f));
        particleTemplate.setAlphaRange(osgParticle::rangef(1.f, 1.f));
        particleTemplate.setLifeTime(1);

        osg::ref_ptr<osgParticle::ModularEmitter> emitter = new osgParticle::ModularEmitter;
        emitter->setParticleSystem(mRainParticleSystem);

        osg::ref_ptr<osgParticle::BoxPlacer> placer = new osgParticle::BoxPlacer;
        placer->setXRange(-rainRange.x() / 2, rainRange.x() / 2);
        placer->setYRange(-rainRange.y() / 2, rainRange.y() / 2);
        placer->setZRange(-rainRange.z() / 2, rainRange.z() / 2);
        emitter->setPlacer(placer);
        mPlacer = placer;

        // FIXME: vanilla engine does not use a particle system to handle rain, it uses a NIF-file with 20 raindrops in
        // it. It spawns the (maxRaindrops-getParticleSystem()->numParticles())*dt/rainEntranceSpeed batches every frame
        // (near 1-2). Since the rain is a regular geometry, it produces water ripples, also in theory it can be removed
        // if collides with something.
        osg::ref_ptr<RainCounter> counter = new RainCounter;
        counter->setNumberOfParticlesPerSecondToCreate(mRainMaxRaindrops / mRainEntranceSpeed * 20);
        emitter->setCounter(counter);
        mCounter = counter;

        osg::ref_ptr<RainShooter> shooter = new RainShooter;
        mRainShooter = shooter;
        emitter->setShooter(shooter);

        osg::ref_ptr<osgParticle::ParticleSystemUpdater> updater = new osgParticle::ParticleSystemUpdater;
        updater->addParticleSystem(mRainParticleSystem);

        osg::ref_ptr<osgParticle::ModularProgram> program = new osgParticle::ModularProgram;
        program->addOperator(new WrapAroundOperator(mCamera, rainRange));
        program->addOperator(new WeatherAlphaOperator(mPrecipitationAlpha, true));
        program->setParticleSystem(mRainParticleSystem);
        mRainNode->addChild(program);

        mRainNode->addChild(emitter);
        mRainNode->addChild(mRainParticleSystem);
        mRainNode->addChild(updater);

        // Note: if we ever switch to regular geometry rain, it'll need to use an AlphaFader.
        mRainNode->addCullCallback(mUnderwaterSwitch);
        mRainNode->setNodeMask(Mask_WeatherParticles);

        mRainParticleSystem->setUserValue("simpleLighting", true);
        mRainParticleSystem->setUserValue("particleOcclusion", true);
        mSceneManager->recreateShaders(mRainNode);

        mSkyNode->addChild(mRainNode);
        if (mPrecipitationOcclusion)
            mPrecipitationOccluder->enable();
    }

    void SkyManager::destroyRain()
    {
        if (!mRainNode)
            return;

        mSkyNode->removeChild(mRainNode);
        mRainNode = nullptr;
        mPlacer = nullptr;
        mCounter = nullptr;
        mRainParticleSystem = nullptr;
        mRainShooter = nullptr;
        mPrecipitationOccluder->disable();
    }

    SkyManager::~SkyManager()
    {
        if (mSkyRootNode)
        {
            mSkyRootNode->getParent(0)->removeChild(mSkyRootNode);
            mSkyRootNode = nullptr;
        }
    }

    int SkyManager::getMasserPhase() const
    {
        if (!mCreated)
            return 0;
        return mMasser->getPhaseInt();
    }

    int SkyManager::getSecundaPhase() const
    {
        if (!mCreated)
            return 0;
        return mSecunda->getPhaseInt();
    }

    bool SkyManager::isEnabled()
    {
        return mEnabled;
    }

    bool SkyManager::hasRain() const
    {
        return mRainNode != nullptr;
    }

    bool SkyManager::getRainRipplesEnabled() const
    {
        if (!mEnabled)
            return false;

        if (hasRain())
            return mRainRipplesEnabled;

        if (mParticleNode && mCurrentParticleEffect == Settings::models().mWeathersnow.get())
            return mSnowRipplesEnabled;

        return false;
    }

    float SkyManager::getPrecipitationAlpha() const
    {
        return mPrecipitationAlpha;
    }

    void SkyManager::update(float duration)
    {
        if (!mEnabled)
            return;

        switchUnderwaterRain();

        if (mIsStorm && mParticleNode)
        {
            osg::Quat quat;
            quat.makeRotate(MWWorld::Weather::defaultDirection(), mStormParticleDirection);
            // Morrowind deliberately rotates the blizzard mesh, so so should we.
            if (mCurrentParticleEffect == Settings::models().mWeatherblizzard.get())
                quat.makeRotate(osg::Vec3f(-1, 0, 0), mStormParticleDirection);
            mParticleNode->setAttitude(quat);
        }

        const float timeScale = MWBase::Environment::get().getWorld()->getTimeManager()->getGameTimeScale();

        // UV Scroll the clouds
        float cloudDelta = duration * mCloudSpeed / 400.f;
        if (mTimescaleClouds)
            cloudDelta *= timeScale / 60.f;

        mCloudAnimationTimer += cloudDelta;
        if (mCloudAnimationTimer >= 4.f)
            mCloudAnimationTimer -= 4.f;

        if (mNextCloudUpdater)
            mNextCloudUpdater->setTextureCoord(mCloudAnimationTimer);
        if (mCloudUpdater)
            mCloudUpdater->setTextureCoord(mCloudAnimationTimer);

        // morrowind rotates each cloud mesh independently
        osg::Quat rotation;
        rotation.makeRotate(MWWorld::Weather::defaultDirection(), mStormDirection);
        if (mCloudMesh)
            mCloudMesh->setAttitude(rotation);

        if (mNextCloudMesh && mNextCloudMesh->getNodeMask())
        {
            rotation.makeRotate(MWWorld::Weather::defaultDirection(), mNextStormDirection);
            mNextCloudMesh->setAttitude(rotation);
        }

        // rotate the stars by 360 degrees every 4 days
        mAtmosphereNightRoll += timeScale * duration * osg::DegreesToRadians(360.f) / (3600 * 96.f);
        if (mAtmosphereNightNode && mAtmosphereNightNode->getNodeMask() != 0)
            mAtmosphereNightNode->setAttitude(osg::Quat(mAtmosphereNightRoll, osg::Vec3f(0, 0, 1)));
        mPrecipitationOccluder->update();
    }

    void SkyManager::setEnabled(bool enabled)
    {
        if (enabled && !mCreated)
            create();

        const osg::Node::NodeMask mask = enabled ? Mask_Sky : 0u;

        mEarlyRenderBinRoot->setNodeMask(mask);
        mSkyNode->setNodeMask(mask);

        if (!enabled && mParticleNode && mParticleEffect)
        {
            mCurrentParticleEffect.clear();
            mDirtyParticlesEffect = true;
        }

        mEnabled = enabled;
    }

    void SkyManager::setMoonColour(bool red)
    {
        if (!mCreated)
            return;
        mSecunda->setColor(red ? mMoonScriptColor : osg::Vec4f(1, 1, 1, 1));
    }

    void SkyManager::updateRainParameters()
    {
        if (mRainShooter)
        {
            float angle = -std::atan(mWindSpeed / 50.f);
            mRainShooter->setVelocity(osg::Vec3f(0, mRainSpeed * std::sin(angle), -mRainSpeed / std::cos(angle)));
            mRainShooter->setAngle(angle);

            osg::Vec3 rainRange = osg::Vec3(mRainDiameter, mRainDiameter, (mRainMinHeight + mRainMaxHeight) / 2.f);

            mPlacer->setXRange(-rainRange.x() / 2, rainRange.x() / 2);
            mPlacer->setYRange(-rainRange.y() / 2, rainRange.y() / 2);
            mPlacer->setZRange(-rainRange.z() / 2, rainRange.z() / 2);

            mCounter->setNumberOfParticlesPerSecondToCreate(mRainMaxRaindrops / mRainEntranceSpeed * 20);
            mPrecipitationOccluder->updateRange(rainRange);
        }
    }

    void SkyManager::switchUnderwaterRain()
    {
        if (!mRainParticleSystem)
            return;

        bool freeze = mUnderwaterSwitch->isUnderwater();
        mRainParticleSystem->setFrozen(freeze);
    }

    void SkyManager::setWeather(const WeatherResult& weather)
    {
        if (!mCreated)
            return;

        mRainEntranceSpeed = weather.mRainEntranceSpeed;
        mRainMaxRaindrops = weather.mRainMaxRaindrops;
        mRainDiameter = weather.mRainDiameter;
        mRainMinHeight = weather.mRainMinHeight;
        mRainMaxHeight = weather.mRainMaxHeight;
        mRainSpeed = weather.mRainSpeed;
        mWindSpeed = weather.mWindSpeed;
        mBaseWindSpeed = weather.mBaseWindSpeed;

        if (mRainEffect != weather.mRainEffect)
        {
            mRainEffect = weather.mRainEffect;
            if (!mRainEffect.empty())
            {
                createRain();
            }
            else
            {
                destroyRain();
            }
        }

        updateRainParameters();

        mIsStorm = weather.mIsStorm;

        if (mIsStorm)
            mStormDirection = weather.mStormDirection;

        if (mDirtyParticlesEffect || (mCurrentParticleEffect != weather.mParticleEffect))
        {
            mDirtyParticlesEffect = false;
            mCurrentParticleEffect = weather.mParticleEffect;

            // cleanup old particles
            if (mParticleEffect)
            {
                mParticleNode->removeChild(mParticleEffect);
                mParticleEffect = nullptr;
            }

            if (mCurrentParticleEffect.empty())
            {
                if (mParticleNode)
                {
                    mSkyNode->removeChild(mParticleNode);
                    mParticleNode = nullptr;
                }
                if (mRainEffect.empty())
                {
                    mPrecipitationOccluder->disable();
                }
            }
            else
            {
                if (!mSceneManager->getVFS()->exists(mCurrentParticleEffect))
                {
                    if (logMissingSkyAssets())
                        Log(Debug::Info) << "FNV/ESM4: skipped missing OpenMW weather mesh ("
                                         << mCurrentParticleEffect.value() << ")";
                    mCurrentParticleEffect.clear();
                    if (mParticleNode)
                    {
                        mSkyNode->removeChild(mParticleNode);
                        mParticleNode = nullptr;
                    }
                    if (mRainEffect.empty())
                        mPrecipitationOccluder->disable();
                }
                else
                {
                if (!mParticleNode)
                {
                    mParticleNode = new osg::PositionAttitudeTransform;
                    mParticleNode->addCullCallback(mUnderwaterSwitch);
                    mParticleNode->setNodeMask(Mask_WeatherParticles);
                    mSkyNode->addChild(mParticleNode);
                }

                mParticleEffect = mSceneManager->getInstance(mCurrentParticleEffect, mParticleNode);

                SceneUtil::AssignControllerSourcesVisitor assignVisitor(std::make_shared<SceneUtil::FrameTimeSource>());
                mParticleEffect->accept(assignVisitor);

                SetupVisitor alphaFaderSetupVisitor(mPrecipitationAlpha);
                mParticleEffect->accept(alphaFaderSetupVisitor);

                SceneUtil::FindByClassVisitor findPSVisitor("ParticleSystem");
                mParticleEffect->accept(findPSVisitor);

                const osg::Vec3 defaultWrapRange = osg::Vec3(1024, 1024, 800);
                const bool occlusionEnabledForEffect
                    = !mRainEffect.empty() || mCurrentParticleEffect == Settings::models().mWeathersnow.get();

                for (unsigned int i = 0; i < findPSVisitor.mFoundNodes.size(); ++i)
                {
                    osgParticle::ParticleSystem* ps
                        = static_cast<osgParticle::ParticleSystem*>(findPSVisitor.mFoundNodes[i]);

                    osg::ref_ptr<osgParticle::ModularProgram> program = new osgParticle::ModularProgram;
                    if (occlusionEnabledForEffect)
                        program->addOperator(new WrapAroundOperator(mCamera, defaultWrapRange));
                    program->addOperator(new WeatherAlphaOperator(mPrecipitationAlpha, false));
                    program->setParticleSystem(ps);
                    mParticleNode->addChild(program);

                    for (int particleIndex = 0; particleIndex < ps->numParticles(); ++particleIndex)
                    {
                        ps->getParticle(particleIndex)
                            ->setAlphaRange(osgParticle::rangef(mPrecipitationAlpha, mPrecipitationAlpha));
                        ps->getParticle(particleIndex)->update(0, true);
                    }

                    ps->setUserValue("simpleLighting", true);

                    if (occlusionEnabledForEffect)
                        ps->setUserValue("particleOcclusion", true);
                }

                mSceneManager->recreateShaders(mParticleNode);

                if (mPrecipitationOcclusion && occlusionEnabledForEffect)
                {
                    mPrecipitationOccluder->enable();
                    mPrecipitationOccluder->updateRange(defaultWrapRange);
                }
                }
            }
        }

        if (mClouds != weather.mCloudTexture)
        {
            mClouds = weather.mCloudTexture;

            if (mCloudUpdater)
            {
                const VFS::Path::Normalized requested = Misc::ResourceHelpers::correctTexturePath(
                    VFS::Path::toNormalized(mClouds), mSceneManager->getVFS());
                const VFS::Path::Normalized texture
                    = resolveWeatherCloudTexture(mSceneManager->getVFS(), requested);

                osg::ref_ptr<osg::Texture2D> cloudTex
                    = new osg::Texture2D(mSceneManager->getImageManager()->getImage(texture));
                cloudTex->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
                cloudTex->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);

                mCloudUpdater->setTexture(std::move(cloudTex));
                if (logMissingSkyAssets() && texture != requested)
                    Log(Debug::Info) << "FNV/ESM4: remapped weather cloud texture " << requested.value()
                                     << " -> " << texture.value();
            }
        }

        if (mStormDirection != weather.mStormDirection)
            mStormDirection = weather.mStormDirection;

        if (mNextStormDirection != weather.mNextStormDirection)
            mNextStormDirection = weather.mNextStormDirection;

        if (mNextClouds != weather.mNextCloudTexture)
        {
            mNextClouds = weather.mNextCloudTexture;

            if (!mNextClouds.empty() && mNextCloudUpdater)
            {
                const VFS::Path::Normalized requested = Misc::ResourceHelpers::correctTexturePath(
                    VFS::Path::toNormalized(mNextClouds), mSceneManager->getVFS());
                const VFS::Path::Normalized texture
                    = resolveWeatherCloudTexture(mSceneManager->getVFS(), requested);

                osg::ref_ptr<osg::Texture2D> cloudTex
                    = new osg::Texture2D(mSceneManager->getImageManager()->getImage(texture));
                cloudTex->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
                cloudTex->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);

                mNextCloudUpdater->setTexture(std::move(cloudTex));
                if (logMissingSkyAssets() && texture != requested)
                    Log(Debug::Info) << "FNV/ESM4: remapped next weather cloud texture " << requested.value()
                                     << " -> " << texture.value();
                mNextStormDirection = weather.mStormDirection;
            }
        }

        if (mCloudBlendFactor != weather.mCloudBlendFactor)
        {
            mCloudBlendFactor = std::clamp(weather.mCloudBlendFactor, 0.f, 1.f);
            const float nativeCloudOpacity
                = isFalloutSkyMesh(Settings::models().mSkyclouds.get()) ? nativeSkyCloudOpacity() : 1.f;

            if (mCloudUpdater)
                mCloudUpdater->setOpacity((1.f - mCloudBlendFactor) * nativeCloudOpacity);
            if (mNextCloudUpdater)
                mNextCloudUpdater->setOpacity(mCloudBlendFactor * nativeCloudOpacity);
            if (mNextCloudMesh)
                mNextCloudMesh->setNodeMask(
                    mCloudBlendFactor > 0.f && nativeCloudOpacity > 0.f && mNextCloudUpdater ? ~0u : 0);
        }

        if (mCloudColour != weather.mFogColor)
        {
            osg::Vec4f clr(weather.mFogColor);
            clr += osg::Vec4f(0.13f, 0.13f, 0.13f, 0.f);

            if (mCloudUpdater)
                mCloudUpdater->setEmissionColor(clr);
            if (mNextCloudUpdater)
                mNextCloudUpdater->setEmissionColor(clr);

            mCloudColour = weather.mFogColor;
        }

        if (mSkyColour != weather.mSkyColor)
        {
            mSkyColour = weather.mSkyColor;

            if (mAtmosphereUpdater)
                mAtmosphereUpdater->setEmissionColor(mSkyColour);
            mMasser->setAtmosphereColor(mSkyColour);
            mSecunda->setAtmosphereColor(mSkyColour);
        }

        if (mSkyLowerColour != weather.mSkyLowerColor || mSkyHorizonColour != weather.mSkyHorizonColor)
        {
            mSkyLowerColour = weather.mSkyLowerColor;
            mSkyHorizonColour = weather.mSkyHorizonColor;
        }

        if (mAtmosphereUpdater && mFalloutAtmosphereDay)
        {
            mAtmosphereUpdater->setFalloutAtmosphereGradientColors(mSkyColour, mSkyLowerColour, mSkyHorizonColour);
            if (!mLoggedFalloutAtmosphereGradient)
            {
                Log(Debug::Info) << "FNV/ESM4: atmosphere vertical colors runtime-supported skyUpper=("
                                 << formatVec4(mSkyColour) << ") skyLower=(" << formatVec4(mSkyLowerColour)
                                 << ") horizon=(" << formatVec4(mSkyHorizonColour) << ")";
                mLoggedFalloutAtmosphereGradient = true;
            }
        }

        if (mFogColour != weather.mFogColor)
        {
            mFogColour = weather.mFogColor;
        }

        mCloudSpeed = weather.mCloudSpeed;

        mMasser->adjustTransparency(weather.mGlareView);
        mSecunda->adjustTransparency(weather.mGlareView);

        mSun->setColor(weather.mSunDiscColor);
        mSun->adjustTransparency(weather.mGlareView * weather.mSunDiscColor.a());
        if (std::getenv("OPENMW_FNV_PROOF_WEATHER_ID") != nullptr)
        {
            static int proofSunDiscMaterialLogs = 0;
            if (proofSunDiscMaterialLogs < 12)
            {
                Log(Debug::Info) << "FNV/ESM4 proof: sky sun disc material runtime-supported sunDiscColor=("
                                 << formatVec4(weather.mSunDiscColor)
                                 << ") glareView=" << weather.mGlareView
                                 << " shader=emission-modulated-texture";
                ++proofSunDiscMaterialLogs;
            }
        }

        float nextStarsOpacity = weather.mNightFade * weather.mGlareView;

        if (weather.mNight && mStarsOpacity != nextStarsOpacity)
        {
            mStarsOpacity = nextStarsOpacity;

            if (mAtmosphereNightUpdater)
                mAtmosphereNightUpdater->setFade(mStarsOpacity);
        }

        if (mAtmosphereNightNode)
            mAtmosphereNightNode->setNodeMask(weather.mNight && (mAtmosphereNightUpdater || mNativeAtmosphereNight)
                    ? ~0u
                    : 0);
        mPrecipitationAlpha = weather.mPrecipitationAlpha;
    }

    float SkyManager::getBaseWindSpeed() const
    {
        if (!mCreated)
            return 0.f;

        return mBaseWindSpeed;
    }

    void SkyManager::setSunglare(bool enabled)
    {
        mSunglareEnabled = enabled;

        if (mSun)
            mSun->setSunglare(mSunglareEnabled);
    }

    void SkyManager::sunEnable()
    {
        if (!mCreated)
            return;

        mSun->setVisible(true);
    }

    void SkyManager::sunDisable()
    {
        if (!mCreated)
            return;

        mSun->setVisible(false);
    }

    void SkyManager::setStormParticleDirection(const osg::Vec3f& direction)
    {
        mStormParticleDirection = direction;
    }

    void SkyManager::setSunDirection(const osg::Vec3f& direction)
    {
        if (!mCreated)
            return;

        mSun->setDirection(direction);
    }

    void SkyManager::setMasserState(const MoonState& state)
    {
        if (!mCreated)
            return;

        mMasser->setState(state);
    }

    void SkyManager::setSecundaState(const MoonState& state)
    {
        if (!mCreated)
            return;

        mSecunda->setState(state);
    }

    void SkyManager::setGlareTimeOfDayFade(float val)
    {
        mSun->setGlareTimeOfDayFade(val);
    }

    void SkyManager::setWaterHeight(float height)
    {
        mUnderwaterSwitch->setWaterLevel(height);
    }

    void SkyManager::listAssetsToPreload(
        std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures)
    {
        pushOptionalSkyModel(*mSceneManager, models, Settings::models().mSkyatmosphere, "day atmosphere");
        if (mSceneManager->getVFS()->exists(Settings::models().mSkynight02.get()))
            pushOptionalSkyModel(*mSceneManager, models, Settings::models().mSkynight02, "night atmosphere 02");
        else
            pushOptionalSkyModel(*mSceneManager, models, Settings::models().mSkynight01, "night atmosphere 01");
        pushOptionalSkyModel(*mSceneManager, models, Settings::models().mSkyclouds, "clouds");

        pushOptionalSkyModel(*mSceneManager, models, Settings::models().mWeatherashcloud, "ash cloud");
        pushOptionalSkyModel(*mSceneManager, models, Settings::models().mWeatherblightcloud, "blight cloud");
        pushOptionalSkyModel(*mSceneManager, models, Settings::models().mWeathersnow, "snow");
        pushOptionalSkyModel(*mSceneManager, models, Settings::models().mWeatherblizzard, "blizzard");

        pushFirstExistingSkyTexture(*mSceneManager, textures,
            { "textures/sky/secunda_full.dds", "textures/sky/skymoonfull.dds",
                "textures/tx_mooncircle_full_s.dds" },
            "Secunda moon circle");
        pushFirstExistingSkyTexture(*mSceneManager, textures,
            { "textures/sky/masser_full.dds", "textures/tx_mooncircle_full_m.dds" },
            "Masser moon circle");

        for (std::string_view phase :
            { "new", "one_wax", "half_wax", "three_wax", "one_wan", "half_wan", "three_wan", "full" })
        {
            const std::string masserNative = "textures/sky/masser_" + std::string(phase) + ".dds";
            const std::string masserFallback = "textures/tx_masser_" + std::string(phase) + ".dds";
            pushFirstExistingSkyTexture(*mSceneManager, textures, { masserNative, masserFallback },
                std::string("Masser phase ") + std::string(phase));

            const std::string secundaNative = "textures/sky/secunda_" + std::string(phase) + ".dds";
            const std::string secundaFallback = "textures/tx_secunda_" + std::string(phase) + ".dds";
            pushFirstExistingSkyTexture(*mSceneManager, textures,
                { secundaNative, "textures/sky/skymoonfull.dds", secundaFallback },
                std::string("Secunda phase ") + std::string(phase));
        }

        pushFirstExistingSkyTexture(*mSceneManager, textures,
            { "textures/sky/sun.dds", "textures/sky/sun_d.dds", "textures/lensflare/sun01.dds",
                "textures/tx_sun_05.dds" },
            "sun disc");
        pushFirstExistingSkyTexture(*mSceneManager, textures,
            { "textures/sky/nv_sunglare.dds", "textures/sky/sunglare.dds", "textures/sky/sunglarenonhdr.dds",
                "textures/sky/sunglare_d.dds", "textures/lensflare/suncolor.dds",
                "textures/tx_sun_flash_grey_05.dds" },
            "sun glare");

        pushFirstExistingSkyTexture(*mSceneManager, textures,
            { "textures/sky/raindrop.dds", "textures/tx_raindrop_01.dds" },
            "raindrop");
    }

    void SkyManager::setWaterEnabled(bool enabled)
    {
        mUnderwaterSwitch->setEnabled(enabled);
    }
}
