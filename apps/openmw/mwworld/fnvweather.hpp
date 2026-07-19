#ifndef OPENMW_MWWORLD_FNVWEATHER_H
#define OPENMW_MWWORLD_FNVWEATHER_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <components/esm/formid.hpp>
#include <components/esm4/loadwthr.hpp>

#include <osg/Vec4f>

#include "fnvplayerstate.hpp"

namespace ESM4
{
    struct Climate;
    struct World;
}

namespace MWWorld
{
    template <class T>
    class Store;

    using FalloutWeatherColorSamples = std::array<osg::Vec4f, ESM4::Weather::sTimeCount>;

    struct FalloutWeatherColorTable
    {
        std::array<FalloutWeatherColorSamples, ESM4::Weather::sColorTypeCount> mColors{};
        std::array<FalloutWeatherColorSamples, ESM4::Weather::sCloudLayerCount> mCloudColors{};
    };

    struct FalloutNativeWeatherSnapshot
    {
        ESM::FormId mWeather;
        std::array<std::string, ESM4::Weather::sCloudLayerCount> mCloudTextures{};
        std::array<std::uint8_t, ESM4::Weather::sCloudLayerCount> mCloudSpeeds{};
        std::uint32_t mMaxCloudLayers = 0;
        FalloutWeatherColorTable mColorTable;
        std::array<float, ESM4::Weather::sTimeCount> mFogDistance{};
        ESM4::Weather::Data mData{};
        osg::Vec4f mLightningColor{};
    };

    // This is deliberately a validation snapshot rather than WeatherManager state. The save application passes only
    // mCurrent.mWeather to WeatherManager, which retains all four authored cloud layers and the complete native
    // fog/celestial/image-space pipeline. No generic TES3 WeatherResult can be constructed from this type.
    struct FalloutWeatherModel
    {
        ESM::FormId mWorldspace;
        ESM::FormId mClimate;
        FalloutNativeWeatherSnapshot mCurrent;
        FalloutNativeWeatherSnapshot mDefault;
        bool mCurrentWeatherInClimateList = false;
        float mGameHour = 0.f;
        float mHighNoonToDayFactor = 0.f;
        std::array<osg::Vec4f, ESM4::Weather::sColorTypeCount> mCurrentColors{};
        std::array<osg::Vec4f, ESM4::Weather::sCloudLayerCount> mCurrentCloudColors{};
    };

    struct FalloutWeatherModelResolution
    {
        std::optional<FalloutWeatherModel> mModel;
        std::string mError;

        explicit operator bool() const { return mModel.has_value(); }
    };

    // FNV's fourth WTHR color byte is unused data, not opacity. This conversion is shared by ordinary and cloud color
    // rows so no value of that byte can make geometry or a HUD quad disappear.
    osg::Vec4f convertFalloutWeatherColor(const ESM4::Weather::Color& value);

    FalloutWeatherModelResolution resolveFalloutWeatherModel(const Store<ESM4::World>& worlds,
        const Store<ESM4::Climate>& climates, const Store<ESM4::Weather>& weather,
        const FalloutExteriorPlayerPlacement& placement, const FalloutSaveLoadPlan::SceneState& scene);
}

#endif
