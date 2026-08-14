#ifndef OPENMW_MWRENDER_EFFECTMANAGER_H
#define OPENMW_MWRENDER_EFFECTMANAGER_H

#include <memory>
#include <vector>

<<<<<<< HEAD
=======
#include <osg/Quat>
>>>>>>> origin/main
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>

namespace osg
{
    class Group;
    class Vec3f;
<<<<<<< HEAD
=======
    class Vec4f;
>>>>>>> origin/main
    class PositionAttitudeTransform;
}

namespace Resource
{
    class ResourceSystem;
}
<<<<<<< HEAD
=======

namespace ESM4
{
    struct Light;
}
>>>>>>> origin/main

namespace MWRender
{
    class EffectAnimationTime;

    // Note: effects attached to another object should be managed by MWRender::Animation::addEffect.
    // This class manages "free" effects, i.e. attached to a dedicated scene node in the world.
    class EffectManager
    {
    public:
        EffectManager(osg::ref_ptr<osg::Group> parent, Resource::ResourceSystem* resourceSystem);
        EffectManager(const EffectManager&) = delete;
        ~EffectManager();

        /// Add an effect. When it's finished playing, it will be removed automatically.
        void addEffect(VFS::Path::NormalizedView model, std::string_view textureOverride,
            const osg::Vec3f& worldPosition, float scale, bool isMagicVFX = true, bool useAmbientLight = true,
<<<<<<< HEAD
            std::string_view effectId = {}, bool loop = false);

        void removeEffect(std::string_view effectId);
=======
            const ESM4::Light* light = nullptr, bool isExterior = false,
            const osg::Quat& orientation = osg::Quat(), float authoredDuration = 0.f);

        /// Add a Fallout impact decal using the authored TXST diffuse texture and DODT dimensions.
        void addDecal(VFS::Path::NormalizedView texture, const osg::Vec3f& worldPosition,
            const osg::Vec3f& surfaceNormal, float width, float height, float depth,
            const osg::Vec4f& color, bool alphaBlend, bool alphaTest, float lifetime);
>>>>>>> origin/main

        void update(float dt);

        /// Remove all effects
        void clear();

    private:
        struct Effect
        {
<<<<<<< HEAD
            std::string mEffectId;
            float mMaxControllerLength;
            bool mLoop;
=======
            float mMaxControllerLength;
            float mPlaybackRate = 1.f;
>>>>>>> origin/main
            std::shared_ptr<EffectAnimationTime> mAnimTime;
            osg::ref_ptr<osg::PositionAttitudeTransform> mTransform;
        };

<<<<<<< HEAD
        std::vector<Effect> mEffects;
=======
        struct Decal
        {
            float mRemainingLifetime;
            osg::ref_ptr<osg::PositionAttitudeTransform> mTransform;
        };

        std::vector<Effect> mEffects;
        std::vector<Decal> mDecals;
>>>>>>> origin/main

        osg::ref_ptr<osg::Group> mParentNode;
        Resource::ResourceSystem* mResourceSystem;
    };

}

#endif
