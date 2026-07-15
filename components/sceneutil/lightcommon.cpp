
#include "lightcommon.hpp"
#include <components/esm3/loadligh.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/sceneutil/util.hpp>

namespace SceneUtil
{
    LightCommon::LightCommon(const ESM::Light& light)
        : mFlicker(light.mData.mFlags & ESM::Light::Flicker)
        , mFlickerSlow(light.mData.mFlags & ESM::Light::FlickerSlow)
        , mNegative(light.mData.mFlags & ESM::Light::Negative)
        , mPulse(light.mData.mFlags & ESM::Light::Pulse)
        , mPulseSlow(light.mData.mFlags & ESM::Light::PulseSlow)
        , mOffDefault(light.mData.mFlags & ESM::Light::OffDefault)
        , mColor(SceneUtil::colourFromRGB(light.mData.mColor))
        , mRadius(light.mData.mRadius)

    {
    }
    LightCommon::LightCommon(const ESM4::Light& light)
        : mFlicker(light.mData.flags & ESM4::Light::Flicker)
        , mFlickerSlow(light.mData.flags & ESM4::Light::FlickerSlow)
        , mNegative(light.mData.flags & ESM::Light::Negative)
        , mPulse(light.mData.flags & ESM4::Light::Pulse)
        , mPulseSlow(light.mData.flags & ESM4::Light::PulseSlow)
        , mOffDefault(light.mData.flags & ESM4::Light::OffDefault)
        , mColor(SceneUtil::colourFromRGB(light.mData.colour))
        , mRadius(light.mData.radius)

    {
        // FO3/FNV LIGH FNAM is the HDR multiplier used by retail for the
        // light's RGB shader constants. Keep the Vec4 alpha unchanged.
        mColor.x() *= light.mFade;
        mColor.y() *= light.mFade;
        mColor.z() *= light.mFade;
    }
}
