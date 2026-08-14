#ifndef OPENMW_COMPONENTS_FX_STATEUPDATER_H
#define OPENMW_COMPONENTS_FX_STATEUPDATER_H

#include <osg/BufferTemplate>

#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/std140/ubo.hpp>

namespace Fx
{
    class StateUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        StateUpdater(bool useUBO);

        void setProjectionMatrix(const osg::Matrixf& matrix)
        {
            mData.get<ProjectionMatrix>() = matrix;
            mData.get<InvProjectionMatrix>() = osg::Matrixf::inverse(matrix);
        }

        void setViewMatrix(const osg::Matrixf& matrix)
        {
            mData.get<ViewMatrix>() = matrix;
            mData.get<InvViewMatrix>() = osg::Matrixf::inverse(matrix);
        }

        void setPrevViewMatrix(const osg::Matrixf& matrix) { mData.get<PrevViewMatrix>() = matrix; }

        void setEyePos(const osg::Vec3f& pos) { mData.get<EyePos>() = osg::Vec4f(pos, 0.f); }

        void setEyeVec(const osg::Vec3f& vec) { mData.get<EyeVec>() = osg::Vec4f(vec, 0.f); }

        void setFogColor(const osg::Vec4f& color) { mData.get<FogColor>() = color; }

        void setAmbientColor(const osg::Vec4f& color) { mData.get<AmbientColor>() = color; }

        void setSkyColor(const osg::Vec4f& color) { mData.get<SkyColor>() = color; }

        void setSunColor(const osg::Vec4f& color) { mData.get<SunColor>() = color; }

        void setSunPos(const osg::Vec4f& pos, bool night)
        {
            mData.get<SunPos>() = pos;
            mData.get<SunPos>().normalize();

            if (night)
                mData.get<SunPos>().z() *= -1.f;
        }

<<<<<<< HEAD
        void setSunVec(const osg::Vec4f& vec)
        {
            mData.get<SunVec>() = vec;
            mData.get<SunVec>().normalize();
        }

=======
>>>>>>> origin/main
        void setResolution(const osg::Vec2f& size)
        {
            mData.get<Resolution>() = size;
            mData.get<RcpResolution>() = { 1.f / size.x(), 1.f / size.y() };
        }

        void setSunVis(float vis) { mData.get<SunVis>() = vis; }

        void setFogRange(float near, float far)
        {
            mData.get<FogNear>() = near;
            mData.get<FogFar>() = far;
        }

        void setNearFar(float near, float far)
        {
            mData.get<Near>() = near;
            mData.get<Far>() = far;
        }

        void setIsUnderwater(bool underwater) { mData.get<IsUnderwater>() = underwater; }

        void setIsInterior(bool interior) { mData.get<IsInterior>() = interior; }

        void setFov(float fov) { mData.get<Fov>() = fov; }

        void setGameHour(float hour) { mData.get<GameHour>() = hour; }

        void setWeatherId(int id) { mData.get<WeatherID>() = id; }

        void setNextWeatherId(int id) { mData.get<NextWeatherID>() = id; }

        void setWaterHeight(float height) { mData.get<WaterHeight>() = height; }

        void setIsWaterEnabled(bool enabled) { mData.get<IsWaterEnabled>() = enabled; }

        void setSimulationTime(float time) { mData.get<SimulationTime>() = time; }

        void setDeltaSimulationTime(float time) { mData.get<DeltaSimulationTime>() = time; }

        void setFrameNumber(int frame) { mData.get<FrameNumber>() = frame; }

        void setWindSpeed(float speed) { mData.get<WindSpeed>() = speed; }

        void setWeatherTransition(float transition)
        {
            mData.get<WeatherTransition>() = transition > 0 ? 1 - transition : 0;
        }

        void bindPointLights(std::shared_ptr<SceneUtil::PPLightBuffer> buffer)
        {
            mPointLightBuffer = std::move(buffer);
        }

        static const std::string& getStructDefinition() { return sDefinition; }

        void setDefaults(osg::StateSet* stateset) override;

        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override;

    private:
<<<<<<< HEAD
        struct ProjectionMatrix : Std140::Mat4
=======
        struct ProjectionMatrix : std140::Mat4
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "projectionMatrix";
        };

<<<<<<< HEAD
        struct InvProjectionMatrix : Std140::Mat4
=======
        struct InvProjectionMatrix : std140::Mat4
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "invProjectionMatrix";
        };

<<<<<<< HEAD
        struct ViewMatrix : Std140::Mat4
=======
        struct ViewMatrix : std140::Mat4
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "viewMatrix";
        };

<<<<<<< HEAD
        struct PrevViewMatrix : Std140::Mat4
=======
        struct PrevViewMatrix : std140::Mat4
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "prevViewMatrix";
        };

<<<<<<< HEAD
        struct InvViewMatrix : Std140::Mat4
=======
        struct InvViewMatrix : std140::Mat4
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "invViewMatrix";
        };

<<<<<<< HEAD
        struct EyePos : Std140::Vec4
=======
        struct EyePos : std140::Vec4
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "eyePos";
        };

<<<<<<< HEAD
        struct EyeVec : Std140::Vec4
=======
        struct EyeVec : std140::Vec4
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "eyeVec";
        };

<<<<<<< HEAD
        struct AmbientColor : Std140::Vec4
=======
        struct AmbientColor : std140::Vec4
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "ambientColor";
        };

<<<<<<< HEAD
        struct SkyColor : Std140::Vec4
=======
        struct SkyColor : std140::Vec4
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "skyColor";
        };

<<<<<<< HEAD
        struct FogColor : Std140::Vec4
=======
        struct FogColor : std140::Vec4
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "fogColor";
        };

<<<<<<< HEAD
        struct SunColor : Std140::Vec4
=======
        struct SunColor : std140::Vec4
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "sunColor";
        };

<<<<<<< HEAD
        struct SunPos : Std140::Vec4
=======
        struct SunPos : std140::Vec4
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "sunPos";
        };

<<<<<<< HEAD
        struct SunVec : Std140::Vec4
        {
            static constexpr std::string_view sName = "sunVec";
        };

        struct Resolution : Std140::Vec2
=======
        struct Resolution : std140::Vec2
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "resolution";
        };

<<<<<<< HEAD
        struct RcpResolution : Std140::Vec2
=======
        struct RcpResolution : std140::Vec2
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "rcpResolution";
        };

<<<<<<< HEAD
        struct FogNear : Std140::Float
=======
        struct FogNear : std140::Float
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "fogNear";
        };

<<<<<<< HEAD
        struct FogFar : Std140::Float
=======
        struct FogFar : std140::Float
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "fogFar";
        };

<<<<<<< HEAD
        struct Near : Std140::Float
=======
        struct Near : std140::Float
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "near";
        };

<<<<<<< HEAD
        struct Far : Std140::Float
=======
        struct Far : std140::Float
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "far";
        };

<<<<<<< HEAD
        struct Fov : Std140::Float
=======
        struct Fov : std140::Float
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "fov";
        };

<<<<<<< HEAD
        struct GameHour : Std140::Float
=======
        struct GameHour : std140::Float
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "gameHour";
        };

<<<<<<< HEAD
        struct SunVis : Std140::Float
=======
        struct SunVis : std140::Float
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "sunVis";
        };

<<<<<<< HEAD
        struct WaterHeight : Std140::Float
=======
        struct WaterHeight : std140::Float
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "waterHeight";
        };

<<<<<<< HEAD
        struct IsWaterEnabled : Std140::Bool
=======
        struct IsWaterEnabled : std140::Bool
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "isWaterEnabled";
        };

<<<<<<< HEAD
        struct SimulationTime : Std140::Float
=======
        struct SimulationTime : std140::Float
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "simulationTime";
        };

<<<<<<< HEAD
        struct DeltaSimulationTime : Std140::Float
=======
        struct DeltaSimulationTime : std140::Float
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "deltaSimulationTime";
        };

<<<<<<< HEAD
        struct FrameNumber : Std140::Int
=======
        struct FrameNumber : std140::Int
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "frameNumber";
        };

<<<<<<< HEAD
        struct WindSpeed : Std140::Float
=======
        struct WindSpeed : std140::Float
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "windSpeed";
        };

<<<<<<< HEAD
        struct WeatherTransition : Std140::Float
=======
        struct WeatherTransition : std140::Float
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "weatherTransition";
        };

<<<<<<< HEAD
        struct WeatherID : Std140::Int
=======
        struct WeatherID : std140::Int
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "weatherID";
        };

<<<<<<< HEAD
        struct NextWeatherID : Std140::Int
=======
        struct NextWeatherID : std140::Int
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "nextWeatherID";
        };

<<<<<<< HEAD
        struct IsUnderwater : Std140::Bool
=======
        struct IsUnderwater : std140::Bool
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "isUnderwater";
        };

<<<<<<< HEAD
        struct IsInterior : Std140::Bool
=======
        struct IsInterior : std140::Bool
>>>>>>> origin/main
        {
            static constexpr std::string_view sName = "isInterior";
        };

<<<<<<< HEAD
        using UniformData = Std140::UBO<ProjectionMatrix, InvProjectionMatrix, ViewMatrix, PrevViewMatrix,
            InvViewMatrix, EyePos, EyeVec, FogColor, AmbientColor, SkyColor, SunColor, SunPos, SunVec, Resolution,
            RcpResolution, FogNear, FogFar, Near, Far, Fov, GameHour, SunVis, WaterHeight, IsWaterEnabled,
            SimulationTime, DeltaSimulationTime, FrameNumber, WindSpeed, WeatherTransition, WeatherID, NextWeatherID,
            IsUnderwater, IsInterior>;
=======
        using UniformData
            = std140::UBO<ProjectionMatrix, InvProjectionMatrix, ViewMatrix, PrevViewMatrix, InvViewMatrix, EyePos,
                EyeVec, FogColor, AmbientColor, SkyColor, SunColor, SunPos, Resolution, RcpResolution, FogNear, FogFar,
                Near, Far, Fov, GameHour, SunVis, WaterHeight, IsWaterEnabled, SimulationTime, DeltaSimulationTime,
                FrameNumber, WindSpeed, WeatherTransition, WeatherID, NextWeatherID, IsUnderwater, IsInterior>;
>>>>>>> origin/main

        UniformData mData;
        bool mUseUBO;

        static std::string sDefinition;

        std::shared_ptr<SceneUtil::PPLightBuffer> mPointLightBuffer;
    };
}

#endif
