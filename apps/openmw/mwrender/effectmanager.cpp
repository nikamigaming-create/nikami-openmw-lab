#include "effectmanager.hpp"

<<<<<<< HEAD
#include <osg/PositionAttitudeTransform>

=======
#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/PolygonOffset>
#include <osg/PositionAttitudeTransform>
#include <osg/Texture2D>

#include <components/misc/resourcehelpers.hpp>
#include <components/resource/imagemanager.hpp>
>>>>>>> origin/main
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>

#include <components/sceneutil/controller.hpp>
<<<<<<< HEAD
=======
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/texturetype.hpp>
>>>>>>> origin/main

#include "animation.hpp"
#include "util.hpp"
#include "vismask.hpp"

#include <algorithm>
<<<<<<< HEAD
=======
#include <cmath>
>>>>>>> origin/main

namespace MWRender
{

    EffectManager::EffectManager(osg::ref_ptr<osg::Group> parent, Resource::ResourceSystem* resourceSystem)
        : mParentNode(std::move(parent))
        , mResourceSystem(resourceSystem)
    {
    }

    EffectManager::~EffectManager()
    {
        clear();
    }

    void EffectManager::addEffect(VFS::Path::NormalizedView model, std::string_view textureOverride,
<<<<<<< HEAD
        const osg::Vec3f& worldPosition, float scale, bool isMagicVFX, bool useAmbientLight, std::string_view effectId,
        bool loop)
=======
        const osg::Vec3f& worldPosition, float scale, bool isMagicVFX, bool useAmbientLight,
        const ESM4::Light* light, bool isExterior, const osg::Quat& orientation,
        float authoredDuration)
>>>>>>> origin/main
    {
        osg::ref_ptr<osg::Node> node = mResourceSystem->getSceneManager()->getInstance(model);

        node->setNodeMask(Mask_Effect);

        Effect effect;
        effect.mAnimTime = std::make_shared<EffectAnimationTime>();
<<<<<<< HEAD
        effect.mLoop = loop;
        effect.mEffectId = effectId;
=======
>>>>>>> origin/main

        SceneUtil::FindMaxControllerLengthVisitor findMaxLengthVisitor;
        node->accept(findMaxLengthVisitor);
        effect.mMaxControllerLength = findMaxLengthVisitor.getMaxLength();
<<<<<<< HEAD

        osg::ref_ptr<osg::PositionAttitudeTransform> trans = new osg::PositionAttitudeTransform;
        trans->setPosition(worldPosition);
        trans->setScale(osg::Vec3f(scale, scale, scale));
        trans->addChild(node);

        effect.mTransform = trans;

        SceneUtil::AssignControllerSourcesVisitor assignVisitor(effect.mAnimTime);
        node->accept(assignVisitor);

        if (isMagicVFX)
            overrideFirstRootTexture(VFS::Path::toNormalized(textureOverride), mResourceSystem, *node);
        else
            overrideTexture(VFS::Path::toNormalized(textureOverride), mResourceSystem, *node);

        mParentNode->addChild(trans);

        if (useAmbientLight)
        {
            // Morrowind has a white ambient light attached to the root VFX node of the scenegraph
            node->getOrCreateStateSet()->setAttributeAndModes(
                getVFXLightModelInstance(), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }

        mResourceSystem->getSceneManager()->setUpNormalsRTForStateSet(node->getOrCreateStateSet(), false);

        mEffects.push_back(std::move(effect));
    }

    void EffectManager::removeEffect(std::string_view effectId)
    {
        mEffects.erase(std::remove_if(mEffects.begin(), mEffects.end(),
                           [effectId, this](Effect& effect) {
                               if (effectId == effect.mEffectId)
                               {
                                   mParentNode->removeChild(effect.mTransform);
                                   return true;
                               }

                               return false;
                           }),
            mEffects.end());
    }

    void EffectManager::update(float dt)
    {
        mEffects.erase(std::remove_if(mEffects.begin(), mEffects.end(),
                           [dt, this](Effect& effect) {
                               bool remove = false;
                               effect.mAnimTime->addTime(dt);
                               if (effect.mAnimTime->getTime() >= effect.mMaxControllerLength)
                               {
                                   if (effect.mLoop)
                                   {
                                       float remainder = effect.mAnimTime->getTime() - effect.mMaxControllerLength;
                                       effect.mAnimTime->resetTime(remainder);
                                   }
                                   else
                                   {
                                       mParentNode->removeChild(effect.mTransform);
                                       remove = true;
                                   }
                               }

                               return remove;
                           }),
            mEffects.end());
=======
        if (std::isfinite(authoredDuration) && authoredDuration > 0.f)
        {
            if (std::isfinite(effect.mMaxControllerLength) && effect.mMaxControllerLength > 0.f)
            {
                // Fallout's PROJ/IPCT duration is the wall-clock lifetime of the complete effect. Preserve the
                // controller's authored beginning, peak, and fade by fitting its animation clock into that lifetime.
                effect.mPlaybackRate = effect.mMaxControllerLength / authoredDuration;
            }
            else
            {
                // Keep controller-less effects (notably an attached light) alive for the authored lifetime.
                effect.mMaxControllerLength = authoredDuration;
            }
        }

        osg::ref_ptr<osg::PositionAttitudeTransform> trans = new osg::PositionAttitudeTransform;
        trans->setPosition(worldPosition);
        trans->setAttitude(orientation);
        trans->setScale(osg::Vec3f(scale, scale, scale));
        trans->addChild(node);
        if (light != nullptr)
            SceneUtil::addLight(trans, SceneUtil::LightCommon(*light), Mask_Lighting, isExterior);

        effect.mTransform = trans;

        SceneUtil::AssignControllerSourcesVisitor assignVisitor(effect.mAnimTime);
        node->accept(assignVisitor);

        if (isMagicVFX)
            overrideFirstRootTexture(textureOverride, mResourceSystem, *node);
        else
            overrideTexture(textureOverride, mResourceSystem, *node);

        mParentNode->addChild(trans);

        if (useAmbientLight)
        {
            // Morrowind has a white ambient light attached to the root VFX node of the scenegraph
            node->getOrCreateStateSet()->setAttributeAndModes(
                getVFXLightModelInstance(), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }

        mResourceSystem->getSceneManager()->setUpNormalsRTForStateSet(node->getOrCreateStateSet(), false);

        mEffects.push_back(std::move(effect));
    }

    void EffectManager::addDecal(VFS::Path::NormalizedView texture, const osg::Vec3f& worldPosition,
        const osg::Vec3f& surfaceNormal, float width, float height, float depth,
        const osg::Vec4f& color, bool alphaBlend, bool alphaTest, float lifetime)
    {
        osg::Vec3f normal = surfaceNormal;
        if (normal.normalize() == 0.f || width <= 0.f || height <= 0.f
            || !std::isfinite(depth) || depth < 0.f || lifetime <= 0.f)
            return;

        osg::Vec3f tangent = std::abs(normal.z()) < 0.9f
            ? normal ^ osg::Vec3f(0.f, 0.f, 1.f)
            : normal ^ osg::Vec3f(0.f, 1.f, 0.f);
        if (tangent.normalize() == 0.f)
            return;
        osg::Vec3f bitangent = normal ^ tangent;
        bitangent.normalize();

        const float halfWidth = width * 0.5f;
        const float halfHeight = height * 0.5f;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(-tangent * halfWidth - bitangent * halfHeight);
        vertices->push_back( tangent * halfWidth - bitangent * halfHeight);
        vertices->push_back( tangent * halfWidth + bitangent * halfHeight);
        vertices->push_back(-tangent * halfWidth + bitangent * halfHeight);

        osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
        normals->push_back(normal);
        osg::ref_ptr<osg::Vec2Array> texCoords = new osg::Vec2Array;
        texCoords->push_back(osg::Vec2f(0.f, 0.f));
        texCoords->push_back(osg::Vec2f(1.f, 0.f));
        texCoords->push_back(osg::Vec2f(1.f, 1.f));
        texCoords->push_back(osg::Vec2f(0.f, 1.f));
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        colors->push_back(color);

        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        geometry->setVertexArray(vertices);
        geometry->setNormalArray(normals, osg::Array::BIND_OVERALL);
        geometry->setTexCoordArray(0, texCoords, osg::Array::BIND_PER_VERTEX);
        geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
        geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::QUADS, 0, 4));

        const VFS::Path::Normalized correctedTexture
            = Misc::ResourceHelpers::correctTexturePath(texture.value(), mResourceSystem->getVFS());
        osg::ref_ptr<osg::Texture2D> texture2d
            = new osg::Texture2D(mResourceSystem->getImageManager()->getImage(correctedTexture));
        texture2d->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture2d->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mResourceSystem->getSceneManager()->applyFilterSettings(texture2d);

        osg::StateSet* stateSet = geometry->getOrCreateStateSet();
        stateSet->setTextureAttributeAndModes(0, texture2d, osg::StateAttribute::ON);
        stateSet->setTextureAttributeAndModes(
            0, new SceneUtil::TextureType("diffuseMap"), osg::StateAttribute::ON);
        stateSet->setAttributeAndModes(new osg::PolygonOffset(-1.f, -1.f), osg::StateAttribute::ON);
        osg::ref_ptr<osg::Depth> depthState = new osg::Depth(osg::Depth::LEQUAL);
        depthState->setWriteMask(false);
        stateSet->setAttributeAndModes(depthState, osg::StateAttribute::ON);
        if (alphaBlend)
        {
            stateSet->setMode(GL_BLEND, osg::StateAttribute::ON);
            stateSet->setAttributeAndModes(
                new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON);
            stateSet->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
        }
        if (alphaTest)
            stateSet->setAttributeAndModes(
                new osg::AlphaFunc(osg::AlphaFunc::GREATER, 0.05f), osg::StateAttribute::ON);

        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(geometry);
        osg::ref_ptr<osg::PositionAttitudeTransform> transform = new osg::PositionAttitudeTransform;
        transform->setNodeMask(Mask_Effect);
        // DODT depth describes the decal projection volume; it is not a world-space standoff. Moving the quad by
        // the full authored depth (commonly 16 units) detached bullet marks from the surface that was hit.
        transform->setPosition(worldPosition + normal * 0.25f);
        transform->addChild(geode);
        mParentNode->addChild(transform);

        // Fallout.ini authors fDecalLifetime=10 and uMaxDecalCount=100.
        while (mDecals.size() >= 100)
        {
            mParentNode->removeChild(mDecals.front().mTransform);
            mDecals.erase(mDecals.begin());
        }
        mDecals.push_back({ lifetime, transform });
    }

    void EffectManager::update(float dt)
    {
        mEffects.erase(std::remove_if(mEffects.begin(), mEffects.end(),
                           [dt, this](Effect& effect) {
                               effect.mAnimTime->addTime(dt * effect.mPlaybackRate);
                               const auto remove = effect.mAnimTime->getTime() >= effect.mMaxControllerLength;
                               if (remove)
                                   mParentNode->removeChild(effect.mTransform);
                               return remove;
                           }),
            mEffects.end());
        mDecals.erase(std::remove_if(mDecals.begin(), mDecals.end(),
                          [dt, this](Decal& decal) {
                              decal.mRemainingLifetime -= dt;
                              if (decal.mRemainingLifetime > 0.f)
                                  return false;
                              mParentNode->removeChild(decal.mTransform);
                              return true;
                          }),
            mDecals.end());
>>>>>>> origin/main
    }

    void EffectManager::clear()
    {
        for (const auto& effect : mEffects)
        {
            mParentNode->removeChild(effect.mTransform);
        }
        mEffects.clear();
<<<<<<< HEAD
=======
        for (const Decal& decal : mDecals)
            mParentNode->removeChild(decal.mTransform);
        mDecals.clear();
>>>>>>> origin/main
    }

}
