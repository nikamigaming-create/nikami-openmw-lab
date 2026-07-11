#include "controller.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include <components/debug/debuglog.hpp>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/TexMat>
#include <osg/Texture2D>

#include <osgAnimation/Bone>

#include <osgParticle/Emitter>

#include <components/nif/data.hpp>
#include <components/sceneutil/morphgeometry.hpp>

#include "matrixtransform.hpp"

namespace NifOsg
{
    namespace
    {
        bool isReasonableFloat(float value, float maxAbs)
        {
            return std::isfinite(value) && std::abs(value) <= maxAbs;
        }

        bool isReasonableVec3(const osg::Vec3f& value, float maxAbs)
        {
            return isReasonableFloat(value.x(), maxAbs) && isReasonableFloat(value.y(), maxAbs)
                && isReasonableFloat(value.z(), maxAbs);
        }

        bool isReasonableQuat(const osg::Quat& value)
        {
            if (!isReasonableFloat(value.x(), 2.f) || !isReasonableFloat(value.y(), 2.f)
                || !isReasonableFloat(value.z(), 2.f) || !isReasonableFloat(value.w(), 2.f))
                return false;

            const float length2 = value.x() * value.x() + value.y() * value.y() + value.z() * value.z()
                + value.w() * value.w();
            return length2 > 0.0001f && length2 < 4.f;
        }

        bool isReasonableScale(float value)
        {
            return std::isfinite(value) && value > 0.0001f && value < 10000.f;
        }

        osg::Quat falloutHalfTurnX()
        {
            return osg::Quat(osg::PI, osg::Vec3f(1.f, 0.f, 0.f));
        }

        std::string asciiLower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return value;
        }

        const char* getEsm4RuntimeEnv(const char* name, const char* legacyName)
        {
            if (const char* env = std::getenv(name))
                return env;

            return std::getenv(legacyName);
        }

        bool isFalloutActorBasisBone(const std::string& bone)
        {
            const std::string lowerBone = asciiLower(bone);
            return lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || lowerBone.find("clavicle") != std::string::npos
                || lowerBone.find("upperarm") != std::string::npos
                || lowerBone.find("uparmtwist") != std::string::npos
                || lowerBone.find("forearm") != std::string::npos
                || lowerBone.find("foretwist") != std::string::npos
                || lowerBone.find("hand") != std::string::npos || lowerBone.find("finger") != std::string::npos
                || lowerBone.find("thumb") != std::string::npos;
        }

        bool isFalloutActorLowerBodyBone(const std::string& bone)
        {
            const std::string lowerBone = asciiLower(bone);
            return lowerBone.find("pelvis") != std::string::npos || lowerBone.find("thigh") != std::string::npos
                || lowerBone.find("calf") != std::string::npos || lowerBone.find("foot") != std::string::npos
                || lowerBone.find("toe") != std::string::npos;
        }

        bool shouldPinFalloutActorLowerBodyBindRotation()
        {
            if (const char* env = getEsm4RuntimeEnv("OPENMW_ESM4_PIN_ACTOR_LOWER_BODY_BIND_ROTATION",
                    "OPENMW_FNV_PIN_ACTOR_LOWER_BODY_BIND_ROTATION"))
                return std::string_view(env) != "0";
            return false;
        }

        bool shouldPinFalloutActorBindRotation(const std::string& bone)
        {
            const std::string lowerBone = asciiLower(bone);
            return lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || lowerBone.find("clavicle") != std::string::npos
                || lowerBone.find("upperarm") != std::string::npos
                || lowerBone.find("uparmtwist") != std::string::npos
                || lowerBone.find("forearm") != std::string::npos
                || lowerBone.find("foretwist") != std::string::npos || lowerBone.find("hand") != std::string::npos
                || lowerBone.find("finger") != std::string::npos || lowerBone.find("thumb") != std::string::npos;
        }

        bool esm4ActorBindPinningEnabled(bool defaultEnabled)
        {
            if (const char* env = getEsm4RuntimeEnv(
                    "OPENMW_ESM4_PIN_ACTOR_BIND_ROTATION", "OPENMW_FNV_PIN_ACTOR_BIND_ROTATION"))
                return std::string_view(env) != "0";
            return defaultEnabled;
        }

        bool shouldPinFalloutActorKeyTranslationsToBind()
        {
            if (const char* env = getEsm4RuntimeEnv("OPENMW_ESM4_PIN_ACTOR_KEY_TRANSLATIONS_TO_BIND",
                    "OPENMW_FNV_PIN_ACTOR_KEY_TRANSLATIONS_TO_BIND"))
                return std::string_view(env) != "0";
            return false;
        }

        bool shouldDropFalloutActorKeyTranslations()
        {
            if (const char* env = getEsm4RuntimeEnv(
                    "OPENMW_ESM4_DROP_ACTOR_KEY_TRANSLATIONS", "OPENMW_FNV_DROP_ACTOR_KEY_TRANSLATIONS"))
                return std::string_view(env) != "0";
            return false;
        }

        std::string_view getFalloutActorRotationCompositionMode()
        {
            if (const char* env = getEsm4RuntimeEnv(
                    "OPENMW_ESM4_ACTOR_ROTATION_COMPOSITION", "OPENMW_FNV_ACTOR_ROTATION_COMPOSITION"))
            {
                if (*env != '\0')
                    return env;
            }

            return "raw";
        }

        osg::Quat composeFalloutActorRotation(const osg::Quat& rawRotation, const osg::Quat& bindRotation)
        {
            const osg::Quat halfTurn = falloutHalfTurnX();
            const std::string_view mode = getFalloutActorRotationCompositionMode();
            if (mode == "raw")
                return rawRotation;
            if (mode == "bind")
                return bindRotation;
            if (mode == "rawBind")
                return rawRotation * bindRotation;
            if (mode == "bindRaw")
                return bindRotation * rawRotation;
            if (mode == "halfRaw")
                return halfTurn * rawRotation;
            if (mode == "rawHalf")
                return rawRotation * halfTurn;
            if (mode == "halfBind")
                return halfTurn * bindRotation;
            if (mode == "bindHalf")
                return bindRotation * halfTurn;
            if (mode == "halfRawBind")
                return halfTurn * rawRotation * bindRotation;
            if (mode == "rawBindHalf")
                return rawRotation * bindRotation * halfTurn;
            if (mode == "halfBindRaw")
                return halfTurn * bindRotation * rawRotation;
            if (mode == "bindHalfRaw")
                return bindRotation * halfTurn * rawRotation;
            if (mode == "bindRawHalf")
                return bindRotation * rawRotation * halfTurn;

            return rawRotation * halfTurn * bindRotation;
        }

        void applyFalloutActorTranslationPolicy(SceneUtil::KeyframeController::KfTransform& transform,
            const std::string& lowerBone, const osg::Vec3f& bindTranslation)
        {
            if (lowerBone.empty())
                return;

            const bool dropTranslations = shouldDropFalloutActorKeyTranslations();
            const bool pinTranslations = shouldPinFalloutActorKeyTranslationsToBind();
            if (dropTranslations || pinTranslations)
            {
                const bool auditBone = lowerBone == "bip01 pelvis" || lowerBone.find("spine") != std::string::npos
                    || lowerBone.find("foot") != std::string::npos || lowerBone.find("hand") != std::string::npos;
                static unsigned int sTranslationPolicyLogs = 0;
                if (auditBone && sTranslationPolicyLogs < 96)
                {
                    ++sTranslationPolicyLogs;
                    const osg::Vec3f rawTranslation = transform.mTranslation.value_or(osg::Vec3f());
                    Log(Debug::Info) << "FNV/ESM4 ACTOR KEY TRANSLATION POLICY bone=" << lowerBone
                                     << " mode=" << (dropTranslations ? "drop" : "pin-bind")
                                     << " hadTranslation=" << static_cast<bool>(transform.mTranslation)
                                     << " raw=(" << rawTranslation.x() << "," << rawTranslation.y() << ","
                                     << rawTranslation.z() << ")"
                                     << " bind=(" << bindTranslation.x() << "," << bindTranslation.y() << ","
                                     << bindTranslation.z() << ")";
                }
            }

            if (dropTranslations)
                transform.mTranslation.reset();
            else if (pinTranslations)
                transform.mTranslation = bindTranslation;
        }

        void sanitizeTransform(SceneUtil::KeyframeController::KfTransform& transform)
        {
            if (transform.mTranslation && !isReasonableVec3(*transform.mTranslation, 1000000.f))
                transform.mTranslation.reset();
            if (transform.mRotation && !isReasonableQuat(*transform.mRotation))
                transform.mRotation.reset();
            if (transform.mScale && !isReasonableScale(*transform.mScale))
                transform.mScale.reset();
        }

        float quatAngleDeltaDegrees(osg::Quat left, osg::Quat right)
        {
            const double leftLength = std::sqrt(left.x() * left.x() + left.y() * left.y() + left.z() * left.z()
                + left.w() * left.w());
            const double rightLength = std::sqrt(right.x() * right.x() + right.y() * right.y() + right.z() * right.z()
                + right.w() * right.w());
            if (leftLength <= 0.000001 || rightLength <= 0.000001)
                return 0.f;

            double dot = (left.x() * right.x() + left.y() * right.y() + left.z() * right.z() + left.w() * right.w())
                / (leftLength * rightLength);
            dot = std::clamp(std::abs(dot), 0.0, 1.0);
            return static_cast<float>(2.0 * std::acos(dot) * 180.0 / osg::PI);
        }

        bool shouldAuditFalloutActorBasis()
        {
            return std::getenv("OPENMW_FNV_ACTOR_BASIS_AUDIT") != nullptr
                || std::getenv("OPENMW_ESM4_ACTOR_BASIS_AUDIT") != nullptr;
        }
    }

    ControllerFunction::ControllerFunction(const Nif::NiTimeController* ctrl)
        : mFrequency(ctrl->mFrequency)
        , mPhase(ctrl->mPhase)
        , mStartTime(ctrl->mTimeStart)
        , mStopTime(ctrl->mTimeStop)
        , mExtrapolationMode(ctrl->extrapolationMode())
    {
    }

    ControllerFunction::ControllerFunction(
        float frequency, float phase, float startTime, float stopTime, Nif::NiTimeController::ExtrapolationMode mode)
        : mFrequency(frequency)
        , mPhase(phase)
        , mStartTime(startTime)
        , mStopTime(stopTime)
        , mExtrapolationMode(mode)
    {
    }

    float ControllerFunction::calculate(float value) const
    {
        float time = mFrequency * value + mPhase;
        if (time >= mStartTime && time <= mStopTime)
            return time;
        switch (mExtrapolationMode)
        {
            case Nif::NiTimeController::ExtrapolationMode::Cycle:
            {
                float delta = mStopTime - mStartTime;
                if (delta <= 0)
                    return mStartTime;
                float cycles = (time - mStartTime) / delta;
                float remainder = (cycles - std::floor(cycles)) * delta;
                return mStartTime + remainder;
            }
            case Nif::NiTimeController::ExtrapolationMode::Reverse:
            {
                float delta = mStopTime - mStartTime;
                if (delta <= 0)
                    return mStartTime;

                float cycles = (time - mStartTime) / delta;
                float remainder = (cycles - std::floor(cycles)) * delta;

                // Even number of cycles?
                if ((static_cast<int>(std::fabs(std::floor(cycles))) % 2) == 0)
                    return mStartTime + remainder;

                return mStopTime - remainder;
            }
            case Nif::NiTimeController::ExtrapolationMode::Constant:
            default:
            {
                if (time < mStartTime)
                    return mStartTime;
                if (time > mStopTime)
                    return mStopTime;
                return time;
            }
        }
    }

    float ControllerFunction::getMaximum() const
    {
        return mStopTime;
    }

    KeyframeController::KeyframeController() {}

    KeyframeController::KeyframeController(const KeyframeController& copy, const osg::CopyOp& copyop)
        : osg::Object(copy, copyop)
        , SceneUtil::KeyframeController(copy)
        , SceneUtil::NodeCallback<KeyframeController, NifOsg::MatrixTransform*>(copy, copyop)
        , mRotations(copy.mRotations)
        , mXRotations(copy.mXRotations)
        , mYRotations(copy.mYRotations)
        , mZRotations(copy.mZRotations)
        , mTranslations(copy.mTranslations)
        , mScales(copy.mScales)
        , mAxisOrder(copy.mAxisOrder)
        , mHasDefaultTranslation(copy.mHasDefaultTranslation)
        , mHasDefaultRotation(copy.mHasDefaultRotation)
        , mHasDefaultScale(copy.mHasDefaultScale)
        , mUseFalloutActorRotationBasis(copy.mUseFalloutActorRotationBasis)
        , mPinFalloutActorBindRotation(copy.mPinFalloutActorBindRotation)
        , mFalloutLowerBone(copy.mFalloutLowerBone)
        , mFalloutBindTranslation(copy.mFalloutBindTranslation)
        , mFalloutBindRotation(copy.mFalloutBindRotation)
        , mFalloutBindScale(copy.mFalloutBindScale)
        , mBSplineTransform(copy.mBSplineTransform)
    {
    }

    KeyframeController::KeyframeController(const Nif::NiKeyframeController* keyctrl)
    {
        if (!keyctrl->mInterpolator.empty())
        {
            if (keyctrl->mInterpolator->recType == Nif::RC_NiTransformInterpolator)
            {
                const Nif::NiTransformInterpolator* interp
                    = static_cast<const Nif::NiTransformInterpolator*>(keyctrl->mInterpolator.getPtr());
                const Nif::NiQuatTransform& defaultTransform = interp->mDefaultValue;
                setDefaultTransformChannels(defaultTransform);
                if (!interp->mData.empty())
                {
                    mRotations = QuaternionInterpolator(interp->mData->mRotations, defaultTransform.mRotation);
                    mXRotations = FloatInterpolator(interp->mData->mXRotations);
                    mYRotations = FloatInterpolator(interp->mData->mYRotations);
                    mZRotations = FloatInterpolator(interp->mData->mZRotations);
                    mTranslations = Vec3Interpolator(interp->mData->mTranslations, defaultTransform.mTranslation);
                    mScales = FloatInterpolator(interp->mData->mScales, defaultTransform.mScale);

                    mAxisOrder = interp->mData->mAxisOrder;
                }
                else
                {
                    mRotations = QuaternionInterpolator(Nif::QuaternionKeyMapPtr(), defaultTransform.mRotation);
                    mTranslations = Vec3Interpolator(Nif::Vector3KeyMapPtr(), defaultTransform.mTranslation);
                    mScales = FloatInterpolator(Nif::FloatKeyMapPtr(), defaultTransform.mScale);
                }
            }
        }
        else if (!keyctrl->mData.empty())
        {
            const Nif::NiKeyframeData* keydata = keyctrl->mData.getPtr();
            mRotations = QuaternionInterpolator(keydata->mRotations);
            mXRotations = FloatInterpolator(keydata->mXRotations);
            mYRotations = FloatInterpolator(keydata->mYRotations);
            mZRotations = FloatInterpolator(keydata->mZRotations);
            mTranslations = Vec3Interpolator(keydata->mTranslations);
            mScales = FloatInterpolator(keydata->mScales, 1.f);

            mAxisOrder = keydata->mAxisOrder;
        }
    }

    KeyframeController::KeyframeController(const Nif::NiTransformInterpolator* interp)
    {
        const Nif::NiQuatTransform& defaultTransform = interp->mDefaultValue;
        setDefaultTransformChannels(defaultTransform);
        if (!interp->mData.empty())
        {
            mRotations = QuaternionInterpolator(interp->mData->mRotations, defaultTransform.mRotation);
            mXRotations = FloatInterpolator(interp->mData->mXRotations);
            mYRotations = FloatInterpolator(interp->mData->mYRotations);
            mZRotations = FloatInterpolator(interp->mData->mZRotations);
            mTranslations = Vec3Interpolator(interp->mData->mTranslations, defaultTransform.mTranslation);
            mScales = FloatInterpolator(interp->mData->mScales, defaultTransform.mScale);

            mAxisOrder = interp->mData->mAxisOrder;
        }
        else
        {
            mRotations = QuaternionInterpolator(Nif::QuaternionKeyMapPtr(), defaultTransform.mRotation);
            mTranslations = Vec3Interpolator(Nif::Vector3KeyMapPtr(), defaultTransform.mTranslation);
            mScales = FloatInterpolator(Nif::FloatKeyMapPtr(), defaultTransform.mScale);
        }
    }

    void KeyframeController::initFromDefaultTransform(const Nif::NiQuatTransform& transform)
    {
        setDefaultTransformChannels(transform);
        mRotations = QuaternionInterpolator(Nif::QuaternionKeyMapPtr(), transform.mRotation);
        mTranslations = Vec3Interpolator(Nif::Vector3KeyMapPtr(), transform.mTranslation);
        mScales = FloatInterpolator(Nif::FloatKeyMapPtr(), transform.mScale);
    }

    void KeyframeController::setDefaultTransformChannels(const Nif::NiQuatTransform& transform)
    {
        // Modern NIF interpolators use non-finite/FLT_MAX-style sentinels for channels with no authored default.
        // Preserve valid constant channels even when the corresponding key array (or spline handle) is absent.
        mHasDefaultTranslation = isReasonableVec3(transform.mTranslation, 1000000.f);
        mHasDefaultRotation = isReasonableQuat(transform.mRotation);
        mHasDefaultScale = isReasonableScale(transform.mScale);
    }

    void KeyframeController::setFalloutActorTransformBasis(
        const std::string& lowerBone, const osg::Vec3f& bindTranslation, const osg::Quat& bindRotation, float bindScale)
    {
        const bool pinLowerBodyBindRotation = shouldPinFalloutActorLowerBodyBindRotation();
        const bool pinActorBindRotation = esm4ActorBindPinningEnabled(false);
        mUseFalloutActorRotationBasis = isFalloutActorBasisBone(lowerBone)
            || (pinLowerBodyBindRotation && isFalloutActorLowerBodyBone(lowerBone));
        mPinFalloutActorBindRotation = (pinActorBindRotation && shouldPinFalloutActorBindRotation(lowerBone))
            || (pinLowerBodyBindRotation && isFalloutActorLowerBodyBone(lowerBone));
        mFalloutLowerBone = lowerBone;
        mFalloutBindTranslation = bindTranslation;
        mFalloutBindRotation = bindRotation;
        mFalloutBindScale = bindScale;
    }

    bool KeyframeController::initFromInterpolator(const Nif::NiInterpolator* interp)
    {
        if (interp == nullptr)
            return false;

        if (interp->recType == Nif::RC_NiTransformInterpolator)
        {
            const auto* transform = static_cast<const Nif::NiTransformInterpolator*>(interp);
            setDefaultTransformChannels(transform->mDefaultValue);
            if (!transform->mData.empty())
            {
                const Nif::NiQuatTransform& defaultTransform = transform->mDefaultValue;
                mRotations = QuaternionInterpolator(transform->mData->mRotations, defaultTransform.mRotation);
                mXRotations = FloatInterpolator(transform->mData->mXRotations);
                mYRotations = FloatInterpolator(transform->mData->mYRotations);
                mZRotations = FloatInterpolator(transform->mData->mZRotations);
                mTranslations = Vec3Interpolator(transform->mData->mTranslations, defaultTransform.mTranslation);
                mScales = FloatInterpolator(transform->mData->mScales, defaultTransform.mScale);
                mAxisOrder = transform->mData->mAxisOrder;
            }
            else
                initFromDefaultTransform(transform->mDefaultValue);
            return true;
        }

        if (interp->recType == Nif::RC_NiBSplineTransformInterpolator
            || interp->recType == Nif::RC_NiBSplineCompTransformInterpolator)
        {
            const auto* bspline = static_cast<const Nif::NiBSplineTransformInterpolator*>(interp);
            if (bspline->mSplineData.empty() || bspline->mBasisData.empty())
            {
                initFromDefaultTransform(bspline->mValue);
                return true;
            }

            BSplineTransform data;
            data.mStartTime = bspline->mStartTime;
            data.mStopTime = bspline->mStopTime;
            data.mDefaultValue = bspline->mValue;
            setDefaultTransformChannels(bspline->mValue);
            data.mFloatControlPoints = bspline->mSplineData->mFloatControlPoints;
            data.mCompactControlPoints = bspline->mSplineData->mCompactControlPoints;
            data.mNumControlPoints = bspline->mBasisData->mNumControlPoints;
            data.mTranslationHandle = bspline->mTranslationHandle;
            data.mRotationHandle = bspline->mRotationHandle;
            data.mScaleHandle = bspline->mScaleHandle;

            if (bspline->recType == Nif::RC_NiBSplineCompTransformInterpolator)
            {
                const auto* comp = static_cast<const Nif::NiBSplineCompTransformInterpolator*>(bspline);
                data.mCompressed = true;
                data.mTranslationOffset = comp->mTranslationOffset;
                data.mTranslationHalfRange = comp->mTranslationHalfRange;
                data.mRotationOffset = comp->mRotationOffset;
                data.mRotationHalfRange = comp->mRotationHalfRange;
                data.mScaleOffset = comp->mScaleOffset;
                data.mScaleHalfRange = comp->mScaleHalfRange;
            }

            mBSplineTransform = std::move(data);
            return true;
        }

        if (interp->recType == Nif::RC_NiBlendTransformInterpolator)
        {
            const auto* blend = static_cast<const Nif::NiBlendTransformInterpolator*>(interp);
            if (!blend->mSingleInterpolator.empty() && initFromInterpolator(blend->mSingleInterpolator.getPtr()))
                return true;

            const Nif::NiInterpolator* best = nullptr;
            float bestWeight = -1.f;
            for (const Nif::NiBlendInterpolator::Item& item : blend->mItems)
            {
                if (item.mInterpolator.empty())
                    continue;
                const float weight = item.mNormalizedWeight > 0.f ? item.mNormalizedWeight : item.mWeight;
                if (best == nullptr || weight > bestWeight)
                {
                    best = item.mInterpolator.getPtr();
                    bestWeight = weight;
                }
            }
            if (initFromInterpolator(best))
                return true;

            initFromDefaultTransform(blend->mValue);
            return true;
        }

        return false;
    }

    KeyframeController::KeyframeController(const Nif::NiBSplineTransformInterpolator* interp)
    {
        if (interp->mSplineData.empty() || interp->mBasisData.empty())
            return;

        BSplineTransform data;
        data.mStartTime = interp->mStartTime;
        data.mStopTime = interp->mStopTime;
        data.mDefaultValue = interp->mValue;
        setDefaultTransformChannels(interp->mValue);
        data.mFloatControlPoints = interp->mSplineData->mFloatControlPoints;
        data.mCompactControlPoints = interp->mSplineData->mCompactControlPoints;
        data.mNumControlPoints = interp->mBasisData->mNumControlPoints;
        data.mTranslationHandle = interp->mTranslationHandle;
        data.mRotationHandle = interp->mRotationHandle;
        data.mScaleHandle = interp->mScaleHandle;

        if (interp->recType == Nif::RC_NiBSplineCompTransformInterpolator)
        {
            const auto* comp = static_cast<const Nif::NiBSplineCompTransformInterpolator*>(interp);
            data.mCompressed = true;
            data.mTranslationOffset = comp->mTranslationOffset;
            data.mTranslationHalfRange = comp->mTranslationHalfRange;
            data.mRotationOffset = comp->mRotationOffset;
            data.mRotationHalfRange = comp->mRotationHalfRange;
            data.mScaleOffset = comp->mScaleOffset;
            data.mScaleHalfRange = comp->mScaleHalfRange;
        }

        mBSplineTransform = std::move(data);
    }

    KeyframeController::KeyframeController(const Nif::NiBlendTransformInterpolator* interp)
    {
        initFromInterpolator(interp);
    }

    bool isValidBSplineHandle(uint32_t handle)
    {
        // Gamebryo serializes these uint32 fields with the historic ushort sentinel used by NifSkope/NIFTools.
        return handle != std::numeric_limits<uint16_t>::max()
            && handle != std::numeric_limits<uint32_t>::max();
    }

    float sampleBSplineComponent(
        const KeyframeController::BSplineTransform& data, uint32_t handle, unsigned int stride, unsigned int component,
        float time, float defaultValue, float offset, float halfRange)
    {
        constexpr unsigned int degree = 3;
        if (!isValidBSplineHandle(handle) || data.mNumControlPoints <= degree
            || data.mStopTime <= data.mStartTime)
            return defaultValue;

        const float normalized = std::clamp((time - data.mStartTime) / (data.mStopTime - data.mStartTime), 0.f, 1.f);
        const auto controlPoint = [&](unsigned int point) -> float
        {
            const size_t index = static_cast<size_t>(handle) + static_cast<size_t>(point) * stride + component;
            float control = defaultValue;
            if (data.mCompressed)
            {
                if (index < data.mCompactControlPoints.size())
                    control = offset + halfRange * (static_cast<float>(data.mCompactControlPoints[index]) / 32767.f);
            }
            else if (index < data.mFloatControlPoints.size())
                control = data.mFloatControlPoints[index];
            return control;
        };

        const unsigned int lastPoint = data.mNumControlPoints - 1;
        if (normalized >= 1.f)
            return controlPoint(lastPoint);

        // NIF B-splines use an open/clamped uniform knot vector.  The former four-weight shortcut sampled an
        // unclamped uniform curve, so the first and last frames were mixtures of neighbouring control points and
        // did not match NifSkope or the retail Gamebryo evaluator.  Cubic de Boor evaluation is both exact and local.
        const float knotRange = static_cast<float>(data.mNumControlPoints - degree);
        const float parameter = normalized * knotRange;
        const unsigned int span
            = std::min(degree + static_cast<unsigned int>(parameter), lastPoint);
        const auto knot = [&](unsigned int index) -> float {
            if (index <= degree)
                return 0.f;
            if (index >= data.mNumControlPoints)
                return knotRange;
            return static_cast<float>(index - degree);
        };

        std::array<float, degree + 1> values;
        for (unsigned int i = 0; i <= degree; ++i)
            values[i] = controlPoint(span - degree + i);

        for (unsigned int level = 1; level <= degree; ++level)
        {
            for (unsigned int i = degree; i >= level; --i)
            {
                const float left = knot(span - degree + i);
                const float right = knot(span + 1 + i - level);
                const float denominator = right - left;
                const float alpha = denominator > 0.f ? (parameter - left) / denominator : 0.f;
                values[i] = (1.f - alpha) * values[i - 1] + alpha * values[i];
            }
        }

        return values[degree];
    }

    osg::Quat KeyframeController::getXYZRotation(float time) const
    {
        float xrot = 0, yrot = 0, zrot = 0;
        if (!mXRotations.empty())
            xrot = mXRotations.interpKey(time);
        if (!mYRotations.empty())
            yrot = mYRotations.interpKey(time);
        if (!mZRotations.empty())
            zrot = mZRotations.interpKey(time);
        osg::Quat xr(xrot, osg::X_AXIS);
        osg::Quat yr(yrot, osg::Y_AXIS);
        osg::Quat zr(zrot, osg::Z_AXIS);
        switch (mAxisOrder)
        {
            case Nif::NiKeyframeData::AxisOrder::Order_XYZ:
                return xr * yr * zr;
            case Nif::NiKeyframeData::AxisOrder::Order_XZY:
                return xr * zr * yr;
            case Nif::NiKeyframeData::AxisOrder::Order_YZX:
                return yr * zr * xr;
            case Nif::NiKeyframeData::AxisOrder::Order_YXZ:
                return yr * xr * zr;
            case Nif::NiKeyframeData::AxisOrder::Order_ZXY:
                return zr * xr * yr;
            case Nif::NiKeyframeData::AxisOrder::Order_ZYX:
                return zr * yr * xr;
            case Nif::NiKeyframeData::AxisOrder::Order_XYX:
                return xr * yr * xr;
            case Nif::NiKeyframeData::AxisOrder::Order_YZY:
                return yr * zr * yr;
            case Nif::NiKeyframeData::AxisOrder::Order_ZXZ:
                return zr * xr * zr;
        }
        return xr * yr * zr;
    }

    osg::Vec3f KeyframeController::getTranslation(float time) const
    {
        if (mBSplineTransform)
        {
            const BSplineTransform& data = *mBSplineTransform;
            if (isValidBSplineHandle(data.mTranslationHandle))
            {
                return osg::Vec3f(
                    sampleBSplineComponent(data, data.mTranslationHandle, 3, 0, time,
                        data.mDefaultValue.mTranslation.x(), data.mTranslationOffset, data.mTranslationHalfRange),
                    sampleBSplineComponent(data, data.mTranslationHandle, 3, 1, time,
                        data.mDefaultValue.mTranslation.y(), data.mTranslationOffset, data.mTranslationHalfRange),
                    sampleBSplineComponent(data, data.mTranslationHandle, 3, 2, time,
                        data.mDefaultValue.mTranslation.z(), data.mTranslationOffset, data.mTranslationHalfRange));
            }
            if (mHasDefaultTranslation)
                return data.mDefaultValue.mTranslation;
        }
        if (!mTranslations.empty())
            return mTranslations.interpKey(time);
        return osg::Vec3f();
    }

    void KeyframeController::operator()(NifOsg::MatrixTransform* node, osg::NodeVisitor* nv)
    {
        auto [translation, rotation, scale] = getCurrentTransformation(nv);

        if (shouldAuditFalloutActorBasis() && mUseFalloutActorRotationBasis)
        {
            static unsigned int sFalloutActorBasisAuditLines = 0;
            if (sFalloutActorBasisAuditLines < 256)
            {
                const KfTransform rawKeyframe = getCurrentTransformationWithoutFalloutActorBasis(nv);
                float rotationDeltaDegrees = 0.f;
                if (rotation && rawKeyframe.mRotation)
                    rotationDeltaDegrees = quatAngleDeltaDegrees(*rotation, *rawKeyframe.mRotation);
                float translationDelta = 0.f;
                if (translation && rawKeyframe.mTranslation)
                    translationDelta = (*translation - *rawKeyframe.mTranslation).length();

                Log(Debug::Info) << "FNV/ESM4 ACTOR BASIS CALLBACK AUDIT bone=" << mFalloutLowerBone
                                 << " appliedHasRotation=" << static_cast<bool>(rotation)
                                 << " rawHasRotation=" << static_cast<bool>(rawKeyframe.mRotation)
                                 << " rotationDeltaDegrees=" << rotationDeltaDegrees
                                 << " appliedHasTranslation=" << static_cast<bool>(translation)
                                 << " rawHasTranslation=" << static_cast<bool>(rawKeyframe.mTranslation)
                                 << " translationDelta=" << translationDelta
                                 << " compositionMode=" << getFalloutActorRotationCompositionMode()
                                 << " pinnedBindRotation=" << mPinFalloutActorBindRotation;
                ++sFalloutActorBasisAuditLines;
            }
        }

        if (rotation)
        {
            node->setRotation(*rotation);
        }
        else
        {
            // This is necessary to prevent first person animations glitching out due to RotationController
            node->setRotation(node->mRotationScale);
        }

        if (translation)
            node->setTranslation(*translation);

        if (scale)
            node->setScale(*scale);

        traverse(node, nv);
    }

    KeyframeController::KfTransform KeyframeController::getCurrentTransformation(osg::NodeVisitor* nv)
    {
        return getCurrentTransformation(nv, true);
    }

    KeyframeController::KfTransform KeyframeController::getCurrentTransformationWithoutFalloutActorBasis(
        osg::NodeVisitor* nv)
    {
        return getCurrentTransformation(nv, false);
    }

    KeyframeController::KfTransform KeyframeController::getCurrentTransformation(
        osg::NodeVisitor* nv, bool applyFalloutActorBasis)
    {
        KfTransform out;

        if (hasInput())
        {
            float time = getInputValue(nv);

            if (mBSplineTransform)
            {
                const BSplineTransform& data = *mBSplineTransform;

                if (isValidBSplineHandle(data.mTranslationHandle))
                {
                    out.mTranslation = osg::Vec3f(
                        sampleBSplineComponent(data, data.mTranslationHandle, 3, 0, time,
                            data.mDefaultValue.mTranslation.x(), data.mTranslationOffset,
                            data.mTranslationHalfRange),
                        sampleBSplineComponent(data, data.mTranslationHandle, 3, 1, time,
                            data.mDefaultValue.mTranslation.y(), data.mTranslationOffset,
                            data.mTranslationHalfRange),
                        sampleBSplineComponent(data, data.mTranslationHandle, 3, 2, time,
                            data.mDefaultValue.mTranslation.z(), data.mTranslationOffset,
                            data.mTranslationHalfRange));
                }
                else if (mHasDefaultTranslation)
                    out.mTranslation = data.mDefaultValue.mTranslation;

                if (isValidBSplineHandle(data.mRotationHandle))
                {
                    // NiBSplineData stores quaternion control points in serialized Quaternion field order:
                    // W, X, Y, Z.  osg::Quat's constructor takes X, Y, Z, W.  Treating the compact stream as
                    // X, Y, Z, W changes the rotation of every controlled bone on every frame and tears a rig
                    // apart as soon as animation starts.
                    const float w = sampleBSplineComponent(data, data.mRotationHandle, 4, 0, time,
                        data.mDefaultValue.mRotation.w(), data.mRotationOffset, data.mRotationHalfRange);
                    const float x = sampleBSplineComponent(data, data.mRotationHandle, 4, 1, time,
                        data.mDefaultValue.mRotation.x(), data.mRotationOffset, data.mRotationHalfRange);
                    const float y = sampleBSplineComponent(data, data.mRotationHandle, 4, 2, time,
                        data.mDefaultValue.mRotation.y(), data.mRotationOffset, data.mRotationHalfRange);
                    const float z = sampleBSplineComponent(data, data.mRotationHandle, 4, 3, time,
                        data.mDefaultValue.mRotation.z(), data.mRotationOffset, data.mRotationHalfRange);
                    const float length = std::sqrt(x * x + y * y + z * z + w * w);
                    if (length > 0.f)
                        out.mRotation = osg::Quat(x / length, y / length, z / length, w / length);
                }
                else if (mHasDefaultRotation)
                    out.mRotation = data.mDefaultValue.mRotation;

                if (isValidBSplineHandle(data.mScaleHandle))
                {
                    out.mScale = sampleBSplineComponent(data, data.mScaleHandle, 1, 0, time,
                        data.mDefaultValue.mScale, data.mScaleOffset, data.mScaleHalfRange);
                }
                else if (mHasDefaultScale)
                    out.mScale = data.mDefaultValue.mScale;

                if (applyFalloutActorBasis && mUseFalloutActorRotationBasis)
                {
                    if (mPinFalloutActorBindRotation)
                    {
                        out.mTranslation = mFalloutBindTranslation;
                        out.mRotation = mFalloutBindRotation;
                        out.mScale = mFalloutBindScale;
                    }
                    else if (out.mRotation)
                        out.mRotation = composeFalloutActorRotation(*out.mRotation, mFalloutBindRotation);
                }

                if (applyFalloutActorBasis)
                    applyFalloutActorTranslationPolicy(out, mFalloutLowerBone, mFalloutBindTranslation);

                sanitizeTransform(out);
                return out;
            }

            if (!mRotations.empty())
                out.mRotation = mRotations.interpKey(time);
            else if (!mXRotations.empty() || !mYRotations.empty() || !mZRotations.empty())
                out.mRotation = getXYZRotation(time);
            else if (mHasDefaultRotation)
                out.mRotation = mRotations.interpKey(time);

            if (!mTranslations.empty() || mHasDefaultTranslation)
                out.mTranslation = mTranslations.interpKey(time);

            if (!mScales.empty() || mHasDefaultScale)
                out.mScale = mScales.interpKey(time);

            if (applyFalloutActorBasis && mUseFalloutActorRotationBasis)
            {
                if (mPinFalloutActorBindRotation)
                {
                    out.mTranslation = mFalloutBindTranslation;
                    out.mRotation = mFalloutBindRotation;
                    out.mScale = mFalloutBindScale;
                }
                else if (out.mRotation)
                    out.mRotation = composeFalloutActorRotation(*out.mRotation, mFalloutBindRotation);
            }

            if (applyFalloutActorBasis)
                applyFalloutActorTranslationPolicy(out, mFalloutLowerBone, mFalloutBindTranslation);
        }

        sanitizeTransform(out);
        return out;
    }

    GeomMorpherController::GeomMorpherController() {}

    GeomMorpherController::GeomMorpherController(const GeomMorpherController& copy, const osg::CopyOp& copyop)
        : Controller(copy)
        , SceneUtil::NodeCallback<GeomMorpherController, SceneUtil::MorphGeometry*>(copy, copyop)
        , mKeyFrames(copy.mKeyFrames)
        , mWeights(copy.mWeights)
    {
    }

    GeomMorpherController::GeomMorpherController(const Nif::NiGeomMorpherController* ctrl)
    {
        if (ctrl->mInterpolators.size() == 0)
        {
            if (!ctrl->mData.empty())
            {
                for (const auto& morph : ctrl->mData->mMorphs)
                    mKeyFrames.emplace_back(morph.mKeyFrames);
            }
            return;
        }

        mKeyFrames.resize(ctrl->mInterpolators.size());
        mWeights = ctrl->mWeights;

        for (std::size_t i = 0, n = ctrl->mInterpolators.size(); i < n; ++i)
        {
            if (!ctrl->mInterpolators[i].empty() && ctrl->mInterpolators[i]->recType == Nif::RC_NiFloatInterpolator)
            {
                auto interpolator = static_cast<const Nif::NiFloatInterpolator*>(ctrl->mInterpolators[i].getPtr());
                mKeyFrames[i] = FloatInterpolator(interpolator);
            }
        }
    }

    void GeomMorpherController::operator()(SceneUtil::MorphGeometry* node, osg::NodeVisitor* nv)
    {
        if (hasInput())
        {
            if (mKeyFrames.size() <= 1)
                return;
            float input = getInputValue(nv);
            size_t i = 1;
            for (std::vector<FloatInterpolator>::iterator it = mKeyFrames.begin() + 1; it != mKeyFrames.end();
                 ++it, ++i)
            {
                float val = 0;
                if (!(*it).empty())
                {
                    val = it->interpKey(input);
                    if (i < mWeights.size())
                        val *= mWeights[i];
                }

                SceneUtil::MorphGeometry::MorphTarget& target = node->getMorphTarget(i);
                if (target.getWeight() != val)
                {
                    target.setWeight(val);
                    node->dirty();
                }
            }
        }
    }

    UVController::UVController(const Nif::NiUVData* data, const std::set<unsigned int>& textureUnits)
        : mUTrans(data->mKeyList[0], 0.f)
        , mVTrans(data->mKeyList[1], 0.f)
        , mUScale(data->mKeyList[2], 1.f)
        , mVScale(data->mKeyList[3], 1.f)
        , mTextureUnits(textureUnits)
    {
    }

    UVController::UVController(const UVController& copy, const osg::CopyOp& copyop)
        : osg::Object(copy, copyop)
        , StateSetUpdater(copy, copyop)
        , Controller(copy)
        , mUTrans(copy.mUTrans)
        , mVTrans(copy.mVTrans)
        , mUScale(copy.mUScale)
        , mVScale(copy.mVScale)
        , mTextureUnits(copy.mTextureUnits)
    {
    }

    void UVController::setDefaults(osg::StateSet* stateset)
    {
        osg::ref_ptr<osg::TexMat> texMat(new osg::TexMat);
        for (unsigned int unit : mTextureUnits)
            stateset->setTextureAttributeAndModes(unit, texMat, osg::StateAttribute::ON);
    }

    void UVController::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        if (hasInput())
        {
            float value = getInputValue(nv);

            // First scale the UV relative to its center, then apply the offset.
            // U offset is flipped regardless of the graphics library,
            // while V offset is flipped to account for OpenGL Y axis convention.
            osg::Vec3f uvOrigin(0.5f, 0.5f, 0.f);
            osg::Vec3f uvScale(mUScale.interpKey(value), mVScale.interpKey(value), 1.f);
            osg::Vec3f uvTrans(-mUTrans.interpKey(value), -mVTrans.interpKey(value), 0.f);

            osg::Matrixf mat = osg::Matrixf::translate(uvOrigin);
            mat.preMultScale(uvScale);
            mat.preMultTranslate(-uvOrigin);
            mat.setTrans(mat.getTrans() + uvTrans);

            // setting once is enough because all other texture units share the same TexMat (see setDefaults).
            if (!mTextureUnits.empty())
            {
                osg::TexMat* texMat = static_cast<osg::TexMat*>(
                    stateset->getTextureAttribute(*mTextureUnits.begin(), osg::StateAttribute::TEXMAT));
                texMat->setMatrix(mat);
            }
        }
    }

    TextureTransformController::TextureTransformController(
        const Nif::NiTextureTransformController* ctrl, unsigned int textureUnit)
        : mTextureUnit(textureUnit)
        , mTransformMember(ctrl->mTransformMember)
    {
        if (!ctrl->mInterpolator.empty() && ctrl->mInterpolator->recType == Nif::RC_NiFloatInterpolator)
            mData = static_cast<const Nif::NiFloatInterpolator*>(ctrl->mInterpolator.getPtr());
        else if (!ctrl->mInterpolator.empty() && ctrl->mInterpolator->recType == Nif::RC_NiBlendFloatInterpolator)
        {
            const auto* interpolator = static_cast<const Nif::NiBlendFloatInterpolator*>(ctrl->mInterpolator.getPtr());
            mData = FloatInterpolator(Nif::FloatKeyMapPtr(), interpolator->mValue);
        }
        else if (!ctrl->mData.empty())
            mData = FloatInterpolator(ctrl->mData->mKeyList, 0.f);
    }

    TextureTransformController::TextureTransformController(
        const TextureTransformController& copy, const osg::CopyOp& copyop)
        : osg::Object(copy, copyop)
        , StateSetUpdater(copy, copyop)
        , Controller(copy)
        , mData(copy.mData)
        , mTextureUnit(copy.mTextureUnit)
        , mTransformMember(copy.mTransformMember)
    {
    }

    void TextureTransformController::setDefaults(osg::StateSet* stateset)
    {
        if (!stateset->getTextureAttribute(mTextureUnit, osg::StateAttribute::TEXMAT))
            stateset->setTextureAttributeAndModes(mTextureUnit, new osg::TexMat, osg::StateAttribute::ON);
    }

    void TextureTransformController::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        if (!hasInput())
            return;

        osg::TexMat* texMat
            = static_cast<osg::TexMat*>(stateset->getTextureAttribute(mTextureUnit, osg::StateAttribute::TEXMAT));
        if (!texMat)
            return;

        osg::Matrixf mat = texMat->getMatrix();
        const float value = mData.empty() ? getInputValue(nv) : mData.interpKey(getInputValue(nv));

        switch (mTransformMember)
        {
            case 0: // U translation
                mat(3, 0) = -value;
                break;
            case 1: // V translation
                mat(3, 1) = -value;
                break;
            case 2: // Rotation around UV center
            {
                const osg::Vec3f trans = mat.getTrans();
                mat = osg::Matrixf::translate(osg::Vec3f(0.5f, 0.5f, 0.f));
                mat.preMultRotate(osg::Quat(value, osg::Z_AXIS));
                mat.preMultTranslate(osg::Vec3f(-0.5f, -0.5f, 0.f));
                mat.setTrans(trans);
                break;
            }
            case 3: // U scale
                mat(0, 0) = value;
                break;
            case 4: // V scale
                mat(1, 1) = value;
                break;
            default:
                break;
        }

        texMat->setMatrix(mat);
    }

    VisController::VisController(const Nif::NiVisController* ctrl, unsigned int mask)
        : mMask(mask)
    {
        if (!ctrl->mInterpolator.empty())
        {
            if (ctrl->mInterpolator->recType != Nif::RC_NiBoolInterpolator)
                return;

            mInterpolator = { static_cast<const Nif::NiBoolInterpolator*>(ctrl->mInterpolator.getPtr()) };
        }
        else if (!ctrl->mData.empty())
            mData = ctrl->mData->mKeys;
    }
    VisController::VisController(const Nif::NiBoolInterpolator* interpolator, unsigned int mask)
        : mMask(mask)
        , mInterpolator(interpolator)
    {
    }

    VisController::VisController() {}

    VisController::VisController(const VisController& copy, const osg::CopyOp& copyop)
        : SceneUtil::NodeCallback<VisController>(copy, copyop)
        , Controller(copy)
        , mData(copy.mData)
        , mInterpolator(copy.mInterpolator)
        , mMask(copy.mMask)
    {
    }

    bool VisController::calculate(float time) const
    {
        if (!mInterpolator.empty())
            return mInterpolator.interpKey(time);

        if (mData->empty())
            return true;

        auto iter = std::upper_bound(mData->begin(), mData->end(), time,
            [](float t, const std::pair<float, bool>& key) { return t < key.first; });
        if (iter != mData->begin())
            --iter;
        return iter->second;
    }

    void VisController::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        if (hasInput())
        {
            bool vis = calculate(getInputValue(nv));
            node->setNodeMask(vis ? ~0 : mMask);
        }
        traverse(node, nv);
    }

    RollController::RollController(const Nif::NiRollController* ctrl)
    {
        if (!ctrl->mInterpolator.empty())
        {
            if (ctrl->mInterpolator->recType == Nif::RC_NiFloatInterpolator)
                mData = FloatInterpolator(static_cast<const Nif::NiFloatInterpolator*>(ctrl->mInterpolator.getPtr()));
            else if (ctrl->mInterpolator->recType == Nif::RC_NiBlendFloatInterpolator)
            {
                const auto* interpolator
                    = static_cast<const Nif::NiBlendFloatInterpolator*>(ctrl->mInterpolator.getPtr());
                mData = FloatInterpolator(nullptr, interpolator->mValue);
            }
        }
        else if (!ctrl->mData.empty())
            mData = FloatInterpolator(ctrl->mData->mKeyList, 1.f);
    }

    RollController::RollController(const RollController& copy, const osg::CopyOp& copyop)
        : SceneUtil::NodeCallback<RollController, osg::MatrixTransform*>(copy, copyop)
        , Controller(copy)
        , mData(copy.mData)
        , mStartingTime(copy.mStartingTime)
    {
    }

    void RollController::operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv)
    {
        traverse(node, nv);

        if (hasInput())
        {
            double newTime = nv->getFrameStamp()->getSimulationTime();
            double duration = newTime - mStartingTime;
            mStartingTime = newTime;

            float value = mData.interpKey(getInputValue(nv));

            // Rotate around "roll" axis.
            // Note: in original game rotation speed is the framerate-dependent in a very tricky way.
            // Do not replicate this behaviour until we will really need it.
            // For now consider controller's current value as an angular speed in radians per 1/60 seconds.
            node->preMult(osg::Matrix::rotate(value * duration * 60.f, 0, 0, 1));

            // Note: doing it like this means RollControllers are not compatible with KeyframeControllers.
            // KeyframeController currently wins the conflict.
            // However unlikely that is, NetImmerse might combine the transformations somehow.
        }
    }

    AlphaController::AlphaController() {}

    AlphaController::AlphaController(const Nif::NiAlphaController* ctrl, const osg::Material* baseMaterial)
        : mBaseMaterial(baseMaterial)
    {
        if (!ctrl->mInterpolator.empty())
        {
            if (ctrl->mInterpolator->recType == Nif::RC_NiFloatInterpolator)
                mData = FloatInterpolator(static_cast<const Nif::NiFloatInterpolator*>(ctrl->mInterpolator.getPtr()));
            else if (ctrl->mInterpolator->recType == Nif::RC_NiBlendFloatInterpolator)
            {
                const auto* interpolator
                    = static_cast<const Nif::NiBlendFloatInterpolator*>(ctrl->mInterpolator.getPtr());
                mData = FloatInterpolator(Nif::FloatKeyMapPtr(), interpolator->mValue);
            }
        }
        else if (!ctrl->mData.empty())
            mData = FloatInterpolator(ctrl->mData->mKeyList, 1.f);
    }

    AlphaController::AlphaController(const AlphaController& copy, const osg::CopyOp& copyop)
        : StateSetUpdater(copy, copyop)
        , Controller(copy)
        , mData(copy.mData)
        , mBaseMaterial(copy.mBaseMaterial)
    {
    }

    void AlphaController::setDefaults(osg::StateSet* stateset)
    {
        stateset->setAttribute(
            static_cast<osg::Material*>(mBaseMaterial->clone(osg::CopyOp::DEEP_COPY_ALL)), osg::StateAttribute::ON);
    }

    void AlphaController::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        if (hasInput())
        {
            float value = mData.interpKey(getInputValue(nv));
            osg::Material* mat = static_cast<osg::Material*>(stateset->getAttribute(osg::StateAttribute::MATERIAL));
            osg::Vec4f diffuse = mat->getDiffuse(osg::Material::FRONT_AND_BACK);
            diffuse.a() = value;
            mat->setDiffuse(osg::Material::FRONT_AND_BACK, diffuse);
        }
    }

    MaterialColorController::MaterialColorController() = default;

    MaterialColorController::MaterialColorController(
        const Nif::NiMaterialColorController* ctrl, const osg::Material* baseMaterial)
        : mTargetColor(ctrl->mTargetColor)
        , mBaseMaterial(baseMaterial)
    {
        if (!ctrl->mInterpolator.empty())
        {
            if (ctrl->mInterpolator->recType == Nif::RC_NiPoint3Interpolator)
                mData = Vec3Interpolator(static_cast<const Nif::NiPoint3Interpolator*>(ctrl->mInterpolator.getPtr()));
            else if (ctrl->mInterpolator->recType == Nif::RC_NiBlendPoint3Interpolator)
            {
                const auto* interpolator
                    = static_cast<const Nif::NiBlendPoint3Interpolator*>(ctrl->mInterpolator.getPtr());
                mData = Vec3Interpolator(nullptr, interpolator->mValue);
            }
        }
        else if (!ctrl->mData.empty())
            mData = Vec3Interpolator(ctrl->mData->mKeyList, osg::Vec3f(1, 1, 1));
    }

    MaterialColorController::MaterialColorController(const MaterialColorController& copy, const osg::CopyOp& copyop)
        : StateSetUpdater(copy, copyop)
        , Controller(copy)
        , mData(copy.mData)
        , mTargetColor(copy.mTargetColor)
        , mBaseMaterial(copy.mBaseMaterial)
    {
    }

    void MaterialColorController::setDefaults(osg::StateSet* stateset)
    {
        stateset->setAttribute(
            static_cast<osg::Material*>(mBaseMaterial->clone(osg::CopyOp::DEEP_COPY_ALL)), osg::StateAttribute::ON);
    }

    void MaterialColorController::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        if (hasInput())
        {
            osg::Vec3f value = mData.interpKey(getInputValue(nv));
            osg::Material* mat = static_cast<osg::Material*>(stateset->getAttribute(osg::StateAttribute::MATERIAL));
            using TargetColor = Nif::NiMaterialColorController::TargetColor;
            switch (mTargetColor)
            {
                case TargetColor::Diffuse:
                {
                    osg::Vec4f diffuse = mat->getDiffuse(osg::Material::FRONT_AND_BACK);
                    diffuse.set(value.x(), value.y(), value.z(), diffuse.a());
                    mat->setDiffuse(osg::Material::FRONT_AND_BACK, diffuse);
                    break;
                }
                case TargetColor::Specular:
                {
                    osg::Vec4f specular = mat->getSpecular(osg::Material::FRONT_AND_BACK);
                    specular.set(value.x(), value.y(), value.z(), specular.a());
                    mat->setSpecular(osg::Material::FRONT_AND_BACK, specular);
                    break;
                }
                case TargetColor::Emissive:
                {
                    osg::Vec4f emissive = mat->getEmission(osg::Material::FRONT_AND_BACK);
                    emissive.set(value.x(), value.y(), value.z(), emissive.a());
                    mat->setEmission(osg::Material::FRONT_AND_BACK, emissive);
                    break;
                }
                case TargetColor::Ambient:
                default:
                {
                    osg::Vec4f ambient = mat->getAmbient(osg::Material::FRONT_AND_BACK);
                    ambient.set(value.x(), value.y(), value.z(), ambient.a());
                    mat->setAmbient(osg::Material::FRONT_AND_BACK, ambient);
                }
            }
        }
    }

    MaterialEmittanceMultController::MaterialEmittanceMultController() = default;

    MaterialEmittanceMultController::MaterialEmittanceMultController(
        const Nif::NiFloatInterpController* ctrl, const osg::Material* baseMaterial)
        : mData(Nif::FloatKeyMapPtr(), 1.f)
        , mBaseMaterial(baseMaterial)
        , mBaseEmission(baseMaterial->getEmission(osg::Material::FRONT_AND_BACK))
    {
        if (!ctrl->mInterpolator.empty())
        {
            if (ctrl->mInterpolator->recType == Nif::RC_NiFloatInterpolator)
                mData = FloatInterpolator(static_cast<const Nif::NiFloatInterpolator*>(ctrl->mInterpolator.getPtr()));
            else if (ctrl->mInterpolator->recType == Nif::RC_NiBlendFloatInterpolator)
            {
                const auto* interpolator
                    = static_cast<const Nif::NiBlendFloatInterpolator*>(ctrl->mInterpolator.getPtr());
                mData = FloatInterpolator(Nif::FloatKeyMapPtr(), interpolator->mValue);
            }
        }
    }

    MaterialEmittanceMultController::MaterialEmittanceMultController(
        const MaterialEmittanceMultController& copy, const osg::CopyOp& copyop)
        : StateSetUpdater(copy, copyop)
        , Controller(copy)
        , mData(copy.mData)
        , mBaseMaterial(copy.mBaseMaterial)
        , mBaseEmission(copy.mBaseEmission)
    {
    }

    void MaterialEmittanceMultController::setDefaults(osg::StateSet* stateset)
    {
        stateset->setAttribute(
            static_cast<osg::Material*>(mBaseMaterial->clone(osg::CopyOp::DEEP_COPY_ALL)), osg::StateAttribute::ON);
    }

    void MaterialEmittanceMultController::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        if (hasInput())
        {
            const float value = mData.interpKey(getInputValue(nv));
            osg::Material* mat = static_cast<osg::Material*>(stateset->getAttribute(osg::StateAttribute::MATERIAL));
            osg::Vec4f emissive = mBaseEmission;
            emissive.x() *= value;
            emissive.y() *= value;
            emissive.z() *= value;
            mat->setEmission(osg::Material::FRONT_AND_BACK, emissive);
        }
    }

    FlipController::FlipController(
        const Nif::NiFlipController* ctrl, const std::vector<osg::ref_ptr<osg::Texture2D>>& textures)
        : mTexSlot(0) // always affects diffuse
        , mDelta(ctrl->mDelta)
        , mTextures(textures)
    {
        if (!ctrl->mInterpolator.empty() && ctrl->mInterpolator->recType == Nif::RC_NiFloatInterpolator)
            mData = static_cast<const Nif::NiFloatInterpolator*>(ctrl->mInterpolator.getPtr());
    }

    FlipController::FlipController(int texSlot, float delta, const std::vector<osg::ref_ptr<osg::Texture2D>>& textures)
        : mTexSlot(texSlot)
        , mDelta(delta)
        , mTextures(textures)
    {
    }

    FlipController::FlipController(const FlipController& copy, const osg::CopyOp& copyop)
        : StateSetUpdater(copy, copyop)
        , Controller(copy)
        , mTexSlot(copy.mTexSlot)
        , mDelta(copy.mDelta)
        , mTextures(copy.mTextures)
        , mData(copy.mData)
    {
    }

    void FlipController::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        if (hasInput() && !mTextures.empty())
        {
            int curTexture = 0;
            if (mDelta != 0)
                curTexture = int(getInputValue(nv) / mDelta) % mTextures.size();
            else
                curTexture = int(mData.interpKey(getInputValue(nv))) % mTextures.size();
            stateset->setTextureAttribute(mTexSlot, mTextures[curTexture]);
        }
    }

    ParticleSystemController::ParticleSystemController(const Nif::NiParticleSystemController* ctrl)
        : mEmitStart(ctrl->mEmitStartTime)
        , mEmitStop(ctrl->mEmitStopTime)
    {
    }

    ParticleSystemController::ParticleSystemController(const ParticleSystemController& copy, const osg::CopyOp& copyop)
        : SceneUtil::NodeCallback<ParticleSystemController, osgParticle::ParticleProcessor*>(copy, copyop)
        , Controller(copy)
        , mEmitStart(copy.mEmitStart)
        , mEmitStop(copy.mEmitStop)
    {
    }

    void ParticleSystemController::operator()(osgParticle::ParticleProcessor* node, osg::NodeVisitor* nv)
    {
        if (hasInput())
        {
            float time = getInputValue(nv);
            node->getParticleSystem()->setFrozen(false);
            node->setEnabled(time >= mEmitStart && time < mEmitStop);
        }
        else
            node->getParticleSystem()->setFrozen(true);
        traverse(node, nv);
    }

    PathController::PathController(const PathController& copy, const osg::CopyOp& copyop)
        : SceneUtil::NodeCallback<PathController, NifOsg::MatrixTransform*>(copy, copyop)
        , Controller(copy)
        , mPath(copy.mPath)
        , mPercent(copy.mPercent)
        , mFlags(copy.mFlags)
    {
    }

    PathController::PathController(const Nif::NiPathController* ctrl)
        : mPath(ctrl->mPathData->mKeyList, osg::Vec3f())
        , mPercent(ctrl->mPercentData->mKeyList, 1.f)
        , mFlags(ctrl->mPathFlags)
    {
    }

    float PathController::getPercent(float time) const
    {
        float percent = mPercent.interpKey(time);
        if (percent < 0.f)
            percent = std::fmod(percent, 1.f) + 1.f;
        else if (percent > 1.f)
            percent = std::fmod(percent, 1.f);
        return percent;
    }

    void PathController::operator()(NifOsg::MatrixTransform* node, osg::NodeVisitor* nv)
    {
        if (mPath.empty() || mPercent.empty() || !hasInput())
        {
            traverse(node, nv);
            return;
        }

        float time = getInputValue(nv);
        float percent = getPercent(time);
        node->setTranslation(mPath.interpKey(percent));

        traverse(node, nv);
    }

}
