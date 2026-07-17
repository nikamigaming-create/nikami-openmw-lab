#ifndef OPENMW_MWRENDER_FOGMANAGER_H
#define OPENMW_MWRENDER_FOGMANAGER_H

#include <osg/Vec4f>

namespace MWWorld
{
    class Cell;
}

namespace MWRender
{
    bool isUsableFalloutFog(bool hasAuthoredFog, float fogNear, float fogFar, float power);
    float calculateFalloutFogFactor(float distance, float fogNear, float fogFar, float power);

    class FogManager
    {
    public:
        FogManager();

        void configure(float viewDistance, const MWWorld::Cell& cell);
        void configure(float viewDistance, float fogDepth, float underwaterFog, float dlFactor, float dlOffset,
            const osg::Vec4f& color, bool hasFalloutFog, float falloutFogNear, float falloutFogFar,
            float falloutFogPower);

        osg::Vec4f getFogColor(bool isUnderwater) const;
        float getFogStart(bool isUnderwater) const;
        float getFogEnd(bool isUnderwater) const;
        bool hasFalloutFog(bool isUnderwater) const;
        float getFalloutFogPower(bool isUnderwater) const;
        bool isFalloutFogStep(bool isUnderwater) const;

    private:
        float mLandFogStart;
        float mLandFogEnd;
        float mUnderwaterFogStart;
        float mUnderwaterFogEnd;
        bool mHasFalloutFog;
        bool mFalloutFogStep;
        float mFalloutFogPower;
        osg::Vec4f mFogColor;
        osg::Vec4f mUnderwaterColor;
        float mUnderwaterWeight;
        float mUnderwaterIndoorFog;
    };
}

#endif
