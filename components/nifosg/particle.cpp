#include "particle.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/TriangleFunctor>
#include <osg/ValueObject>
#include <osgParticle/ConstantRateCounter>

#include <components/debug/debuglog.hpp>
#include <components/misc/rng.hpp>
#include <components/nif/data.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>

namespace
{
    class FindFirstGeometry : public osg::NodeVisitor
    {
    public:
        FindFirstGeometry()
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
            , mGeometry(nullptr)
        {
        }

        void apply(osg::Node& node) override
        {
            if (mGeometry)
                return;

            traverse(node);
        }

        void apply(osg::Drawable& drawable) override
        {
            if (auto morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
            {
                mGeometry = morph->getSourceGeometry();
                return;
            }
            else if (auto rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
            {
                mGeometry = rig->getSourceGeometry();
                return;
            }

            traverse(drawable);
        }

        void apply(osg::Geometry& geometry) override { mGeometry = &geometry; }

        osg::Geometry* mGeometry;
    };

    class LocalToWorldAccumulator : public osg::NodeVisitor
    {
    public:
        LocalToWorldAccumulator(osg::Matrix& matrix)
            : osg::NodeVisitor()
            , mMatrix(matrix)
        {
        }

        virtual void apply(osg::Transform& transform)
        {
            if (&transform != mLastAppliedTransform)
            {
                mLastAppliedTransform = &transform;
                mLastMatrix = mMatrix;
            }
            transform.computeLocalToWorldMatrix(mMatrix, this);
        }

        void accumulate(const osg::NodePath& path)
        {
            if (path.empty())
                return;

            size_t i = path.size();

            for (auto rit = path.rbegin(); rit != path.rend(); rit++, --i)
            {
                const osg::Camera* camera = (*rit)->asCamera();
                if (camera
                    && (camera->getReferenceFrame() != osg::Transform::RELATIVE_RF || camera->getParents().empty()))
                    break;
            }

            for (; i < path.size(); ++i)
                path[i]->accept(*this);
        }

        osg::Matrix& mMatrix;
        std::optional<osg::Matrix> mLastMatrix;
        osg::Transform* mLastAppliedTransform = nullptr;
    };
}

namespace NifOsg
{

    ParticleSystem::ParticleSystem()
        : osgParticle::ParticleSystem()
        , mQuota(std::numeric_limits<int>::max())
    {
        mNormalArray = new osg::Vec3Array(1);
        mNormalArray->setBinding(osg::Array::BIND_OVERALL);
        (*mNormalArray.get())[0] = osg::Vec3(0.3, 0.3, 0.3);
    }

    ParticleSystem::ParticleSystem(const ParticleSystem& copy, const osg::CopyOp& copyop)
        : osgParticle::ParticleSystem(copy, copyop)
        , mQuota(copy.mQuota)
        , mGlobalAlpha(copy.mGlobalAlpha)
    {
        mNormalArray = new osg::Vec3Array(1);
        mNormalArray->setBinding(osg::Array::BIND_OVERALL);
        (*mNormalArray.get())[0] = osg::Vec3(0.3, 0.3, 0.3);

        // For some reason the osgParticle constructor doesn't copy the particles
        for (int i = 0; i < copy.numParticles() - copy.numDeadParticles(); ++i)
            ParticleSystem::createParticle(copy.getParticle(i));
    }

    void ParticleSystem::setQuota(int quota)
    {
        mQuota = quota;
    }

    osgParticle::Particle* ParticleSystem::createParticle(const osgParticle::Particle* ptemplate)
    {
        if (numParticles() - numDeadParticles() < mQuota)
            return osgParticle::ParticleSystem::createParticle(ptemplate);
        return nullptr;
    }

    void ParticleSystem::drawImplementation(osg::RenderInfo& renderInfo) const
    {
        osg::State& state = *renderInfo.getState();
        if (state.useVertexArrayObject(getUseVertexArrayObject()))
        {
            state.getCurrentVertexArrayState()->assignNormalArrayDispatcher();
            state.getCurrentVertexArrayState()->setNormalArray(state, mNormalArray);
        }
        else
        {
            state.getAttributeDispatchers().activateNormalArray(mNormalArray);
        }
        osgParticle::ParticleSystem::drawImplementation(renderInfo);
    }

    void InverseWorldMatrix::operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv)
    {
        osg::NodePath path = nv->getNodePath();
        path.pop_back();

        osg::Matrix mat = osg::computeLocalToWorld(path);
        mat.orthoNormalize(mat); // don't undo the scale
        mat.invert(mat);
        node->setMatrix(mat);

        traverse(node, nv);
    }

    ParticleShooter::ParticleShooter(float minSpeed, float maxSpeed, float horizontalDir, float horizontalAngle,
        float verticalDir, float verticalAngle, float lifetime, float lifetimeRandom)
        : mMinSpeed(minSpeed)
        , mMaxSpeed(maxSpeed)
        , mHorizontalDir(horizontalDir)
        , mHorizontalAngle(horizontalAngle)
        , mVerticalDir(verticalDir)
        , mVerticalAngle(verticalAngle)
        , mLifetime(lifetime)
        , mLifetimeRandom(lifetimeRandom)
    {
    }

    ParticleShooter::ParticleShooter()
        : mMinSpeed(0.f)
        , mMaxSpeed(0.f)
        , mHorizontalDir(0.f)
        , mHorizontalAngle(0.f)
        , mVerticalDir(0.f)
        , mVerticalAngle(0.f)
        , mLifetime(0.f)
        , mLifetimeRandom(0.f)
    {
    }

    ParticleShooter::ParticleShooter(const ParticleShooter& copy, const osg::CopyOp& copyop)
        : osgParticle::Shooter(copy, copyop)
    {
        mMinSpeed = copy.mMinSpeed;
        mMaxSpeed = copy.mMaxSpeed;
        mHorizontalDir = copy.mHorizontalDir;
        mHorizontalAngle = copy.mHorizontalAngle;
        mVerticalDir = copy.mVerticalDir;
        mVerticalAngle = copy.mVerticalAngle;
        mLifetime = copy.mLifetime;
        mLifetimeRandom = copy.mLifetimeRandom;
    }

    void ParticleShooter::shoot(osgParticle::Particle* particle) const
    {
        float hdir = mHorizontalDir + mHorizontalAngle * (Misc::Rng::rollClosedProbability() - 0.5f);
        float vdir = mVerticalDir + mVerticalAngle * (Misc::Rng::rollClosedProbability() - 0.5f);

        osg::Vec3f dir
            = (osg::Quat(vdir, osg::Vec3f(0, 1, 0)) * osg::Quat(hdir, osg::Vec3f(0, 0, 1))) * osg::Vec3f(0, 0, 1);

        float vel = mMinSpeed + (mMaxSpeed - mMinSpeed) * Misc::Rng::rollClosedProbability();
        particle->setVelocity(dir * vel);

        // Not supposed to set this here, but there doesn't seem to be a better way of doing it
        particle->setLifeTime(std::max(
            std::numeric_limits<float>::epsilon(), mLifetime + mLifetimeRandom * (Misc::Rng::rollClosedProbability() - 0.5f)));
    }

    GrowFadeAffector::GrowFadeAffector(float growTime, float fadeTime)
        : mGrowTime(growTime)
        , mFadeTime(fadeTime)
        , mCachedDefaultSize(0.f)
    {
    }

    GrowFadeAffector::GrowFadeAffector()
        : mGrowTime(0.f)
        , mFadeTime(0.f)
        , mCachedDefaultSize(0.f)
    {
    }

    GrowFadeAffector::GrowFadeAffector(const GrowFadeAffector& copy, const osg::CopyOp& copyop)
        : osgParticle::Operator(copy, copyop)
    {
        mGrowTime = copy.mGrowTime;
        mFadeTime = copy.mFadeTime;
        mCachedDefaultSize = copy.mCachedDefaultSize;
    }

    void GrowFadeAffector::beginOperate(osgParticle::Program* program)
    {
        mCachedDefaultSize = program->getParticleSystem()->getDefaultParticleTemplate().getSizeRange().minimum;
    }

    void GrowFadeAffector::operate(osgParticle::Particle* particle, double /* dt */)
    {
        float size = mCachedDefaultSize;
        if (particle->getAge() < mGrowTime && mGrowTime != 0.f)
            size *= particle->getAge() / mGrowTime;
        if (particle->getLifeTime() - particle->getAge() < mFadeTime && mFadeTime != 0.f)
            size *= (particle->getLifeTime() - particle->getAge()) / mFadeTime;
        particle->setSizeRange(osgParticle::rangef(size, size));
    }

    ParticleColorAffector::ParticleColorAffector(const Nif::NiColorData* clrdata)
        : mData(clrdata->mKeyMap, osg::Vec4f(1, 1, 1, 1))
    {
    }

    ParticleColorAffector::ParticleColorAffector() {}

    ParticleColorAffector::ParticleColorAffector(const ParticleColorAffector& copy, const osg::CopyOp& copyop)
        : osgParticle::Operator(copy, copyop)
    {
        mData = copy.mData;
    }

    void ParticleColorAffector::operate(osgParticle::Particle* particle, double /* dt */)
    {
        assert(particle->getLifeTime() > 0);
        float time = static_cast<float>(particle->getAge() / particle->getLifeTime());
        osg::Vec4f color = mData.interpKey(time);
        float alpha = color.a();
        color.a() = 1.0f;

        particle->setColorRange(osgParticle::rangev4(color, color));
        particle->setAlphaRange(osgParticle::rangef(alpha, alpha));
    }

    BethesdaParticleColorAffector::BethesdaParticleColorAffector(const Nif::BSPSysSimpleColorModifier* modifier)
        : mFadeInPercent(modifier->mFadeInPercent)
        , mFadeOutPercent(modifier->mFadeOutPercent)
        , mColor1EndPercent(modifier->mColor1EndPercent)
        , mColor1StartPercent(modifier->mColor1StartPercent)
        , mColor2EndPercent(modifier->mColor2EndPercent)
        , mColor2StartPercent(modifier->mColor2StartPercent)
    {
        for (std::size_t i = 0; i < std::min(mColors.size(), modifier->mColors.size()); ++i)
            mColors[i] = modifier->mColors[i];
    }

    BethesdaParticleColorAffector::BethesdaParticleColorAffector(
        const BethesdaParticleColorAffector& copy, const osg::CopyOp& copyop)
        : osgParticle::Operator(copy, copyop)
        , mFadeInPercent(copy.mFadeInPercent)
        , mFadeOutPercent(copy.mFadeOutPercent)
        , mColor1EndPercent(copy.mColor1EndPercent)
        , mColor1StartPercent(copy.mColor1StartPercent)
        , mColor2EndPercent(copy.mColor2EndPercent)
        , mColor2StartPercent(copy.mColor2StartPercent)
        , mColors(copy.mColors)
        , mGlobalAlpha(copy.mGlobalAlpha)
    {
    }

    void BethesdaParticleColorAffector::beginOperate(osgParticle::Program* program)
    {
        const auto* particleSystem = dynamic_cast<const ParticleSystem*>(program->getParticleSystem());
        mGlobalAlpha = particleSystem ? particleSystem->getGlobalAlpha() : 1.f;
    }

    osg::Vec4f BethesdaParticleColorAffector::evaluate(float normalizedAge) const
    {
        const float time = std::clamp(normalizedAge, 0.f, 1.f);
        const bool constantMiddleColor = mColor1EndPercent == 0.f && mColor1StartPercent == 0.f
            && mColor2EndPercent == 0.f && mColor2StartPercent == 0.f;

        osg::Vec4f color;
        if (constantMiddleColor)
            color = mColors[1];
        else if (time <= mColor1EndPercent)
            color = mColors[0];
        else if (time < mColor1StartPercent && mColor1StartPercent > mColor1EndPercent)
        {
            const float amount = (time - mColor1EndPercent) / (mColor1StartPercent - mColor1EndPercent);
            color = mColors[0] * (1.f - amount) + mColors[1] * amount;
        }
        else if (time <= mColor2EndPercent)
            color = mColors[1];
        else if (time < mColor2StartPercent && mColor2StartPercent > mColor2EndPercent)
        {
            const float amount = (time - mColor2EndPercent) / (mColor2StartPercent - mColor2EndPercent);
            color = mColors[1] * (1.f - amount) + mColors[2] * amount;
        }
        else
            color = mColors[2];

        float fade = 1.f;
        if (mFadeInPercent > 0.f && time < mFadeInPercent)
            fade *= time / mFadeInPercent;
        if (mFadeOutPercent < 1.f && time > mFadeOutPercent)
            fade *= (1.f - time) / (1.f - mFadeOutPercent);
        color.a() *= std::clamp(fade, 0.f, 1.f);
        return color;
    }

    void BethesdaParticleColorAffector::operate(osgParticle::Particle* particle, double /* dt */)
    {
        assert(particle->getLifeTime() > 0.f);
        osg::Vec4f color = evaluate(static_cast<float>(particle->getAge() / particle->getLifeTime()));
        const float alpha = std::clamp(color.a() * mGlobalAlpha, 0.f, 1.f);
        color.a() = 1.f;
        particle->setColorRange(osgParticle::rangev4(color, color));
        particle->setAlphaRange(osgParticle::rangef(alpha, alpha));
    }

    GravityAffector::GravityAffector(const Nif::NiGravity* gravity)
        : mForce(gravity->mForce)
        , mType(gravity->mType)
        , mPosition(gravity->mPosition)
        , mDirection(gravity->mDirection)
        , mDecay(gravity->mDecay)
    {
    }

    GravityAffector::GravityAffector(float force, Nif::ForceType type, const osg::Vec3f& position,
        const osg::Vec3f& direction, float decay)
        : mForce(force)
        , mType(type)
        , mPosition(position)
        , mDirection(direction)
        , mDecay(decay)
    {
    }

    GravityAffector::GravityAffector(const GravityAffector& copy, const osg::CopyOp& copyop)
        : osgParticle::Operator(copy, copyop)
    {
        mForce = copy.mForce;
        mType = copy.mType;
        mPosition = copy.mPosition;
        mDirection = copy.mDirection;
        mDecay = copy.mDecay;
        mCachedWorldPosition = copy.mCachedWorldPosition;
        mCachedWorldDirection = copy.mCachedWorldDirection;
    }

    void GravityAffector::beginOperate(osgParticle::Program* program)
    {
        bool absolute = (program->getReferenceFrame() == osgParticle::ParticleProcessor::ABSOLUTE_RF);

        // We don't need the position for Wind gravity, except if decay is being applied
        if (mType == Nif::ForceType::Point || mDecay != 0.f)
            mCachedWorldPosition = absolute ? program->transformLocalToWorld(mPosition) : mPosition;

        mCachedWorldDirection = absolute ? program->rotateLocalToWorld(mDirection) : mDirection;
        mCachedWorldDirection.normalize();
    }

    void GravityAffector::operate(osgParticle::Particle* particle, double dt)
    {
        const float magic = 1.6f;
        switch (mType)
        {
            case Nif::ForceType::Wind:
            {
                float decayFactor = 1.f;
                if (mDecay != 0.f)
                {
                    osg::Plane gravityPlane(mCachedWorldDirection, mCachedWorldPosition);
                    float distance = std::abs(gravityPlane.distance(particle->getPosition()));
                    decayFactor = std::exp(-1.f * mDecay * distance);
                }

                particle->addVelocity(mCachedWorldDirection * mForce * dt * decayFactor * magic);

                break;
            }
            case Nif::ForceType::Point:
            {
                osg::Vec3f diff = mCachedWorldPosition - particle->getPosition();

                float decayFactor = 1.f;
                if (mDecay != 0.f)
                    decayFactor = std::exp(-1.f * mDecay * diff.length());

                diff.normalize();

                particle->addVelocity(diff * mForce * dt * decayFactor * magic);
                break;
            }
        }
    }

    ParticleBomb::ParticleBomb(const Nif::NiParticleBomb* bomb)
        : mRange(bomb->mRange)
        , mStrength(bomb->mStrength)
        , mDecayType(bomb->mDecayType)
        , mSymmetryType(bomb->mSymmetryType)
        , mPosition(bomb->mPosition)
        , mDirection(bomb->mDirection)
    {
    }

    ParticleBomb::ParticleBomb(float range, float strength, Nif::DecayType decayType,
        Nif::SymmetryType symmetryType, const osg::Vec3f& position, const osg::Vec3f& direction)
        : mRange(range)
        , mStrength(strength)
        , mDecayType(decayType)
        , mSymmetryType(symmetryType)
        , mPosition(position)
        , mDirection(direction)
    {
    }

    ParticleBomb::ParticleBomb(const ParticleBomb& copy, const osg::CopyOp& copyop)
        : osgParticle::Operator(copy, copyop)
    {
        mRange = copy.mRange;
        mStrength = copy.mStrength;
        mDecayType = copy.mDecayType;
        mSymmetryType = copy.mSymmetryType;
        mCachedWorldPosition = copy.mCachedWorldPosition;
        mCachedWorldDirection = copy.mCachedWorldDirection;
    }

    void ParticleBomb::beginOperate(osgParticle::Program* program)
    {
        bool absolute = (program->getReferenceFrame() == osgParticle::ParticleProcessor::ABSOLUTE_RF);

        mCachedWorldPosition = absolute ? program->transformLocalToWorld(mPosition) : mPosition;

        // We don't need the direction for Spherical bomb
        if (mSymmetryType != Nif::SymmetryType::Spherical)
        {
            mCachedWorldDirection = absolute ? program->rotateLocalToWorld(mDirection) : mDirection;
            mCachedWorldDirection.normalize();
        }
    }

    void ParticleBomb::operate(osgParticle::Particle* particle, double dt)
    {
        float decay = 1.f;
        osg::Vec3f explosionDir;

        osg::Vec3f particleDir = particle->getPosition() - mCachedWorldPosition;
        float distance = particleDir.length();
        particleDir.normalize();

        switch (mDecayType)
        {
            case Nif::DecayType::None:
                break;
            case Nif::DecayType::Linear:
                decay = 1.f - distance / mRange;
                break;
            case Nif::DecayType::Exponential:
                decay = std::exp(-distance / mRange);
                break;
        }

        if (decay <= 0.f)
            return;

        switch (mSymmetryType)
        {
            case Nif::SymmetryType::Spherical:
                explosionDir = particleDir;
                break;
            case Nif::SymmetryType::Cylindrical:
                explosionDir = particleDir - mCachedWorldDirection * (mCachedWorldDirection * particleDir);
                explosionDir.normalize();
                break;
            case Nif::SymmetryType::Planar:
                explosionDir = mCachedWorldDirection;
                if (explosionDir * particleDir < 0)
                    explosionDir = -explosionDir;
                break;
        }

        particle->addVelocity(explosionDir * mStrength * decay * dt);
    }

    Emitter::Emitter()
        : osgParticle::Emitter()
        , mFlags(0)
        , mGeometryEmitterTarget(std::nullopt)
    {
    }

    Emitter::Emitter(const Emitter& copy, const osg::CopyOp& copyop)
        : osgParticle::Emitter(copy, copyop)
        , mTargets(copy.mTargets)
        , mPlacer(copy.mPlacer)
        , mShooter(copy.mShooter)
        // need a deep copy because the remainder is stored in the object
        , mCounter(static_cast<osgParticle::Counter*>(copy.mCounter->clone(osg::CopyOp::DEEP_COPY_ALL)))
        , mFlags(copy.mFlags)
        , mParticleRadius(copy.mParticleRadius)
        , mParticleRadiusVariation(copy.mParticleRadiusVariation)
        , mVelocityType(copy.mVelocityType)
        , mEmissionType(copy.mEmissionType)
        , mEmissionAxis(copy.mEmissionAxis)
        , mGeometryEmitterTarget(copy.mGeometryEmitterTarget)
        , mCachedGeometryEmitter(copy.mCachedGeometryEmitter)
        , mCachedTriangles(copy.mCachedTriangles)
        , mCachedTriangleArea(copy.mCachedTriangleArea)
    {
    }

    void Emitter::setEmissionRate(float rate)
    {
        if (auto* counter = dynamic_cast<osgParticle::ConstantRateCounter*>(mCounter.get()))
            counter->setNumberOfParticlesPerSecondToCreate(std::max(0.f, rate));
    }

    void Emitter::setParticleRadius(float radius, float variation)
    {
        mParticleRadius = std::max(0.f, radius);
        mParticleRadiusVariation = std::max(0.f, variation);
    }

    void Emitter::setModernMeshEmission(
        uint32_t velocityType, uint32_t emissionType, const osg::Vec3f& emissionAxis)
    {
        mVelocityType = static_cast<int>(velocityType);
        mEmissionType = static_cast<int>(emissionType);
        mEmissionAxis = emissionAxis;
        if (mEmissionAxis.length2() == 0.f)
            mEmissionAxis.set(0.f, 0.f, 1.f);
        else
            mEmissionAxis.normalize();
    }

    Emitter::Emitter(const std::vector<int>& targets)
        : mTargets(targets)
        , mFlags(0)
        , mGeometryEmitterTarget(std::nullopt)
    {
    }

    void Emitter::emitParticles(double dt)
    {
        int n = mCounter->numParticlesToCreate(dt);
        if (n == 0)
            return;

        osg::Matrix worldToPs;

        // maybe this could be optimized by halting at the lowest common ancestor of the particle and emitter nodes
        osg::NodePathList partsysNodePaths = getParticleSystem()->getParentalNodePaths();
        if (!partsysNodePaths.empty())
        {
            osg::Matrix psToWorld = osg::computeLocalToWorld(partsysNodePaths[0]);
            worldToPs = osg::Matrix::inverse(psToWorld);
        }

        const osg::Matrix& ltw = getLocalToWorldMatrix();
        osg::Matrix emitterToPs = ltw * worldToPs;

        osg::ref_ptr<osg::Vec3Array> geometryVertices = nullptr;
        osg::Geometry* geometry = nullptr;

        const bool useGeometryEmitter = mFlags & Nif::NiParticleSystemController::BSPArrayController_AtVertex;

        if (useGeometryEmitter || !mTargets.empty())
        {
            int recIndex;

            if (useGeometryEmitter)
            {
                if (!mGeometryEmitterTarget.has_value())
                    return;

                recIndex = mGeometryEmitterTarget.value();
            }
            else
            {
                int randomIndex = Misc::Rng::rollClosedProbability() * (mTargets.size() - 1);
                recIndex = mTargets[randomIndex];
            }

            // we could use a map here for faster lookup
            FindGroupByRecIndex visitor(recIndex);
            getParent(0)->accept(visitor);

            if (!visitor.mFound)
            {
                Log(Debug::Info) << "Can't find emitter node" << recIndex;
                return;
            }

            if (useGeometryEmitter)
            {
                if (!mCachedGeometryEmitter.lock(geometryVertices))
                {
                    FindFirstGeometry geometryVisitor;
                    visitor.mFound->accept(geometryVisitor);

                    if (geometryVisitor.mGeometry)
                    {
                        geometry = geometryVisitor.mGeometry;
                        if (auto* vertices = dynamic_cast<osg::Vec3Array*>(geometryVisitor.mGeometry->getVertexArray()))
                        {
                            mCachedGeometryEmitter = osg::observer_ptr<osg::Vec3Array>(vertices);
                            geometryVertices = vertices;
                        }
                    }
                }
                if (mEmissionType >= 1 && mCachedTriangles.empty() && geometry)
                {
                    struct TriangleCollector
                    {
                        std::vector<Triangle>* mTriangles{ nullptr };
                        float* mTotalArea{ nullptr };

                        void setTarget(std::vector<Triangle>* triangles, float* totalArea)
                        {
                            mTriangles = triangles;
                            mTotalArea = totalArea;
                        }

                        void operator()(const osg::Vec3& a, const osg::Vec3& b, const osg::Vec3& c,
                            bool /*temporary*/ = false)
                        {
                            osg::Vec3f normal = (b - a) ^ (c - a);
                            const float area = normal.length() * 0.5f;
                            if (area <= std::numeric_limits<float>::epsilon())
                                return;
                            normal.normalize();
                            *mTotalArea += area;
                            mTriangles->push_back({ a, b, c, normal, *mTotalArea });
                        }
                    };

                    osg::TriangleFunctor<TriangleCollector> collector;
                    collector.setTarget(&mCachedTriangles, &mCachedTriangleArea);
                    geometry->accept(collector);
                }
            }

            osg::NodePath path = visitor.mFoundPath;
            path.erase(path.begin());
            if (!useGeometryEmitter && (mFlags & Nif::NiParticleSystemController::BSPArrayController_AtNode)
                && path.size())
            {
                osg::Matrix current;

                LocalToWorldAccumulator accum(current);
                accum.accumulate(path);

                osg::Matrix parent = accum.mLastMatrix.value_or(current);

                auto p1 = parent.getTrans();
                auto p2 = current.getTrans();
                current.setTrans((p2 - p1) * Misc::Rng::rollClosedProbability() + p1);

                emitterToPs = current * emitterToPs;
            }
            else
            {
                emitterToPs = osg::computeLocalToWorld(path) * emitterToPs;
            }
        }

        emitterToPs.orthoNormalize(emitterToPs);

        if (useGeometryEmitter && (!geometryVertices.valid() || geometryVertices->empty()))
            return;

        for (int i = 0; i < n; ++i)
        {
            osgParticle::Particle* const particle = getParticleSystem()->createParticle(nullptr);
            if (particle)
            {
                osg::Vec3f emissionNormal;
                bool hasEmissionNormal = false;
                if (useGeometryEmitter && mEmissionType >= 1 && !mCachedTriangles.empty())
                {
                    std::size_t triangleIndex = 0;
                    if (mEmissionType == 3 && mCachedTriangleArea > 0.f)
                    {
                        const float area = Misc::Rng::rollClosedProbability() * mCachedTriangleArea;
                        triangleIndex = static_cast<std::size_t>(std::distance(mCachedTriangles.begin(),
                            std::lower_bound(mCachedTriangles.begin(), mCachedTriangles.end(), area,
                                [](const Triangle& triangle, float value) {
                                    return triangle.mCumulativeArea < value;
                                })));
                        triangleIndex = std::min(triangleIndex, mCachedTriangles.size() - 1);
                    }
                    else
                        triangleIndex = Misc::Rng::rollDice(static_cast<int>(mCachedTriangles.size()));

                    const Triangle& triangle = mCachedTriangles[triangleIndex];
                    if (mEmissionType == 1)
                        particle->setPosition((triangle.mA + triangle.mB + triangle.mC) / 3.f);
                    else if (mEmissionType == 2 || mEmissionType == 4)
                    {
                        const int edge = Misc::Rng::rollDice(3);
                        const osg::Vec3f& a = edge == 0 ? triangle.mA : edge == 1 ? triangle.mB : triangle.mC;
                        const osg::Vec3f& b = edge == 0 ? triangle.mB : edge == 1 ? triangle.mC : triangle.mA;
                        const float amount = mEmissionType == 2 ? 0.5f : Misc::Rng::rollClosedProbability();
                        particle->setPosition(a * (1.f - amount) + b * amount);
                    }
                    else
                    {
                        float u = Misc::Rng::rollClosedProbability();
                        float v = Misc::Rng::rollClosedProbability();
                        if (u + v > 1.f)
                        {
                            u = 1.f - u;
                            v = 1.f - v;
                        }
                        particle->setPosition(triangle.mA + (triangle.mB - triangle.mA) * u
                            + (triangle.mC - triangle.mA) * v);
                    }
                    emissionNormal = triangle.mNormal;
                    hasEmissionNormal = true;
                }
                else if (useGeometryEmitter)
                    particle->setPosition((*geometryVertices)[Misc::Rng::rollDice(geometryVertices->getNumElements())]);
                else if (mPlacer)
                    mPlacer->place(particle);

                mShooter->shoot(particle);

                if (mVelocityType == 0 && hasEmissionNormal)
                {
                    osg::Quat rotation;
                    rotation.makeRotate(osg::Vec3f(0.f, 0.f, 1.f), emissionNormal);
                    particle->setVelocity(rotation * particle->getVelocity());
                }
                else if (mVelocityType == 1)
                {
                    const float z = Misc::Rng::rollClosedProbability() * 2.f - 1.f;
                    const float angle = Misc::Rng::rollClosedProbability() * osg::PI * 2.f;
                    const float radial = std::sqrt(std::max(0.f, 1.f - z * z));
                    const float speed = particle->getVelocity().length();
                    particle->setVelocity(osg::Vec3f(std::cos(angle) * radial, std::sin(angle) * radial, z) * speed);
                }
                else if (mVelocityType == 2)
                {
                    osg::Quat rotation;
                    rotation.makeRotate(osg::Vec3f(0.f, 0.f, 1.f), mEmissionAxis);
                    particle->setVelocity(rotation * particle->getVelocity());
                }

                if (mParticleRadius > 0.f)
                {
                    const float radius = std::max(std::numeric_limits<float>::epsilon(),
                        mParticleRadius + mParticleRadiusVariation * (Misc::Rng::rollClosedProbability() - 0.5f));
                    particle->setSizeRange(osgParticle::rangef(radius, radius));
                }

                particle->transformPositionVelocity(emitterToPs);
            }
        }
    }

    ModernParticleController::ModernParticleController(Emitter* emitter, ParticleSystem* particleSystem,
        const Nif::NiControllerSequence* rateSequence, const Nif::NiFloatInterpolator* rateInterpolator,
        const Nif::NiControllerSequence* activeSequence, const Nif::NiBoolInterpolator* activeInterpolator,
        const Nif::NiControllerSequence* alphaSequence, const Nif::NiFloatInterpolator* alphaInterpolator)
        : mEmitter(emitter)
        , mParticleSystem(particleSystem)
        , mHasRate(rateSequence != nullptr && rateInterpolator != nullptr)
        , mHasActive(activeSequence != nullptr && activeInterpolator != nullptr)
        , mHasAlpha(alphaSequence != nullptr && alphaInterpolator != nullptr)
    {
        if (mHasRate)
        {
            mRate = FloatInterpolator(rateInterpolator);
            mRateFunction = std::make_shared<ControllerFunction>(rateSequence->mFrequency, rateSequence->mPhase,
                rateSequence->mStartTime, rateSequence->mStopTime, rateSequence->mExtrapolationMode);
        }
        if (mHasActive)
        {
            mActive = BoolInterpolator(activeInterpolator);
            mActiveFunction = std::make_shared<ControllerFunction>(activeSequence->mFrequency, activeSequence->mPhase,
                activeSequence->mStartTime, activeSequence->mStopTime, activeSequence->mExtrapolationMode);
        }
        if (mHasAlpha)
        {
            mAlpha = FloatInterpolator(alphaInterpolator);
            mAlphaFunction = std::make_shared<ControllerFunction>(alphaSequence->mFrequency, alphaSequence->mPhase,
                alphaSequence->mStartTime, alphaSequence->mStopTime, alphaSequence->mExtrapolationMode);
        }
    }

    ModernParticleController::ModernParticleController(
        const ModernParticleController& copy, const osg::CopyOp& copyop)
        : osg::Object(copy, copyop)
        , SceneUtil::NodeCallback<ModernParticleController, osg::Node*>(copy, copyop)
        , mEmitter(copy.mEmitter)
        , mParticleSystem(copy.mParticleSystem)
        , mRate(copy.mRate)
        , mActive(copy.mActive)
        , mAlpha(copy.mAlpha)
        , mRateFunction(copy.mRateFunction)
        , mActiveFunction(copy.mActiveFunction)
        , mAlphaFunction(copy.mAlphaFunction)
        , mHasRate(copy.mHasRate)
        , mHasActive(copy.mHasActive)
        , mHasAlpha(copy.mHasAlpha)
    {
    }

    void ModernParticleController::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        const float input = nv && nv->getFrameStamp() ? static_cast<float>(nv->getFrameStamp()->getSimulationTime()) : 0.f;
        bool active = true;
        if (mHasActive && mActiveFunction)
            active = mActive.interpKey(mActiveFunction->calculate(input));
        if (mEmitter.valid() && mHasRate && mRateFunction)
            mEmitter->setEmissionRate(active ? mRate.interpKey(mRateFunction->calculate(input)) : 0.f);
        if (mParticleSystem.valid() && mHasAlpha && mAlphaFunction)
            mParticleSystem->setGlobalAlpha(std::clamp(mAlpha.interpKey(mAlphaFunction->calculate(input)), 0.f, 1.f));
        traverse(node, nv);
    }

    FindGroupByRecIndex::FindGroupByRecIndex(unsigned int recIndex)
        : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        , mFound(nullptr)
        , mRecIndex(recIndex)
    {
    }

    void FindGroupByRecIndex::apply(osg::Node& node)
    {
        applyNode(node);
    }

    void FindGroupByRecIndex::apply(osg::MatrixTransform& node)
    {
        applyNode(node);
    }

    void FindGroupByRecIndex::apply(osg::Geometry& node)
    {
        applyNode(node);
    }

    void FindGroupByRecIndex::applyNode(osg::Node& searchNode)
    {
        unsigned int recIndex;
        if (searchNode.getUserValue("recIndex", recIndex) && mRecIndex == recIndex)
        {
            osg::Group* group = searchNode.asGroup();
            if (!group)
                group = searchNode.getParent(0);

            mFound = group;
            mFoundPath = getNodePath();
            return;
        }
        traverse(searchNode);
    }

    PlanarCollider::PlanarCollider(const Nif::NiPlanarCollider* collider)
        : mBounceFactor(collider->mBounceFactor)
        , mExtents(collider->mExtents)
        , mPosition(collider->mPosition)
        , mXVector(collider->mXVector)
        , mYVector(collider->mYVector)
        , mPlane(-collider->mPlaneNormal, collider->mPlaneDistance)
    {
    }

    PlanarCollider::PlanarCollider(const PlanarCollider& copy, const osg::CopyOp& copyop)
        : osgParticle::Operator(copy, copyop)
        , mBounceFactor(copy.mBounceFactor)
        , mExtents(copy.mExtents)
        , mPosition(copy.mPosition)
        , mPositionInParticleSpace(copy.mPositionInParticleSpace)
        , mXVector(copy.mXVector)
        , mXVectorInParticleSpace(copy.mXVectorInParticleSpace)
        , mYVector(copy.mYVector)
        , mYVectorInParticleSpace(copy.mYVectorInParticleSpace)
        , mPlane(copy.mPlane)
        , mPlaneInParticleSpace(copy.mPlaneInParticleSpace)
    {
    }

    void PlanarCollider::beginOperate(osgParticle::Program* program)
    {
        mPositionInParticleSpace = mPosition;
        mPlaneInParticleSpace = mPlane;
        mXVectorInParticleSpace = mXVector;
        mYVectorInParticleSpace = mYVector;
        if (program->getReferenceFrame() == osgParticle::ParticleProcessor::ABSOLUTE_RF)
        {
            mPositionInParticleSpace = program->transformLocalToWorld(mPosition);
            mPlaneInParticleSpace.transform(program->getLocalToWorldMatrix());
            mXVectorInParticleSpace = program->rotateLocalToWorld(mXVector);
            mYVectorInParticleSpace = program->rotateLocalToWorld(mYVector);
        }
    }

    void PlanarCollider::operate(osgParticle::Particle* particle, double dt)
    {
        // Does the particle in question move towards the collider?
        float velDotProduct = particle->getVelocity() * mPlaneInParticleSpace.getNormal();
        if (velDotProduct <= 0)
            return;

        // Does it intersect the collider's plane?
        osg::BoundingSphere bs(particle->getPosition(), 0.f);
        if (mPlaneInParticleSpace.intersect(bs) != 1)
            return;

        // Is it inside the collider's bounds?
        osg::Vec3f relativePos = particle->getPosition() - mPositionInParticleSpace;
        float xDotProduct = relativePos * mXVectorInParticleSpace;
        float yDotProduct = relativePos * mYVectorInParticleSpace;
        if (-mExtents.x() * 0.5f > xDotProduct || mExtents.x() * 0.5f < xDotProduct)
            return;
        if (-mExtents.y() * 0.5f > yDotProduct || mExtents.y() * 0.5f < yDotProduct)
            return;

        // Deflect the particle
        osg::Vec3 reflectedVelocity = particle->getVelocity() - mPlaneInParticleSpace.getNormal() * (2 * velDotProduct);
        reflectedVelocity *= mBounceFactor;
        particle->setVelocity(reflectedVelocity);
    }

    SphericalCollider::SphericalCollider(const Nif::NiSphericalCollider* collider)
        : mBounceFactor(collider->mBounceFactor)
        , mSphere(collider->mCenter, collider->mRadius)
    {
    }

    SphericalCollider::SphericalCollider()
        : mBounceFactor(1.0f)
    {
    }

    SphericalCollider::SphericalCollider(const SphericalCollider& copy, const osg::CopyOp& copyop)
        : osgParticle::Operator(copy, copyop)
        , mBounceFactor(copy.mBounceFactor)
        , mSphere(copy.mSphere)
        , mSphereInParticleSpace(copy.mSphereInParticleSpace)
    {
    }

    void SphericalCollider::beginOperate(osgParticle::Program* program)
    {
        mSphereInParticleSpace = mSphere;
        if (program->getReferenceFrame() == osgParticle::ParticleProcessor::ABSOLUTE_RF)
            mSphereInParticleSpace.center() = program->transformLocalToWorld(mSphereInParticleSpace.center());
    }

    void SphericalCollider::operate(osgParticle::Particle* particle, double dt)
    {
        osg::Vec3f cent
            = (particle->getPosition() - mSphereInParticleSpace.center()); // vector from sphere center to particle

        bool insideSphere = cent.length2() <= mSphereInParticleSpace.radius2();

        if (insideSphere
            || (cent * particle->getVelocity()
                < 0.0f)) // if outside, make sure the particle is flying towards the sphere
        {
            // Collision test (finding point of contact) is performed by solving a quadratic equation:
            // ||vec(cent) + vec(vel)*k|| = R      /^2
            // k^2 + 2*k*(vec(cent)*vec(vel))/||vec(vel)||^2 + (||vec(cent)||^2 - R^2)/||vec(vel)||^2 = 0

            float b = -(cent * particle->getVelocity()) / particle->getVelocity().length2();

            osg::Vec3f u = cent + particle->getVelocity() * b;

            if (insideSphere || (u.length2() < mSphereInParticleSpace.radius2()))
            {
                float d = (mSphereInParticleSpace.radius2() - u.length2()) / particle->getVelocity().length2();
                float k = insideSphere ? (std::sqrt(d) + b) : (b - std::sqrt(d));

                if (k < dt)
                {
                    // collision detected; reflect off the tangent plane
                    osg::Vec3f contact = particle->getPosition() + particle->getVelocity() * k;

                    osg::Vec3 normal = (contact - mSphereInParticleSpace.center());
                    normal.normalize();

                    float dotproduct = particle->getVelocity() * normal;

                    osg::Vec3 reflectedVelocity = particle->getVelocity() - normal * (2 * dotproduct);
                    reflectedVelocity *= mBounceFactor;
                    particle->setVelocity(reflectedVelocity);
                }
            }
        }
    }

}
