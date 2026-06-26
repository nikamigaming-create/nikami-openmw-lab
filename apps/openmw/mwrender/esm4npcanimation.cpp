#include "esm4npcanimation.hpp"

#include <components/esm4/loadarma.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadeyes.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/esm4/loadhair.hpp>
#include <components/esm4/loadhdpt.hpp>
#include <components/esm4/loadidle.hpp>
#include <components/esm4/loadidlm.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadpack.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadsndr.hpp>
#include <components/esm4/loadsoun.hpp>
#include <components/esm4/loadstat.hpp>
#include <components/esm4/loadweap.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <regex>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/attach.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <array>
#include "../mwmechanics/character.hpp"

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/ComputeBoundsVisitor>
#include <osg/FrontFace>
#include <osg/Geode>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/NodeCallback>
#include <osg/NodeVisitor>
#include <osg/PositionAttitudeTransform>
#include <osg/PrimitiveSet>
#include <osg/Texture2D>
#include <osgAnimation/Bone>
#include <osgAnimation/UpdateBone>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <initializer_list>
#include <istream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwclass/esm4npc.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/timestamp.hpp"
#include "util.hpp"

namespace MWRender
{
    namespace
    {
        class TintMaterialVisitor : public osg::NodeVisitor
        {
        public:
            TintMaterialVisitor(
                const osg::Vec4f& tint, float emissionStrength = 0.f, bool replaceVertexColorRgb = false,
                bool neutralMaterialWhenReplacing = false, bool preserveVertexColorIntensity = false)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mTint(tint)
                , mEmissionStrength(emissionStrength)
                , mReplaceVertexColorRgb(replaceVertexColorRgb)
                , mNeutralMaterialWhenReplacing(neutralMaterialWhenReplacing)
                , mPreserveVertexColorIntensity(preserveVertexColorIntensity)
            {
            }

            void apply(osg::Node& node) override
            {
                applyTint(node.getOrCreateStateSet());
                traverse(node);
            }

            void apply(osg::Geode& geode) override
            {
                applyTint(geode.getOrCreateStateSet());
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        applyDrawable(*drawable);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

        private:
            bool neutralizeVertexColors(osg::Geometry& geometry) const
            {
                osg::Array* existingColors = geometry.getColorArray();
                if (existingColors == nullptr)
                    return false;

                if (osg::Vec4Array* colors = dynamic_cast<osg::Vec4Array*>(existingColors))
                {
                    float maxRed = 0.f;
                    if (mReplaceVertexColorRgb && mPreserveVertexColorIntensity)
                    {
                        for (const osg::Vec4f& color : *colors)
                            maxRed = std::max(maxRed, color.x());
                    }
                    for (osg::Vec4f& color : *colors)
                    {
                        if (mReplaceVertexColorRgb)
                        {
                            const float intensity = mPreserveVertexColorIntensity && maxRed > 0.001f
                                ? std::clamp(color.x() / maxRed, 0.f, 1.f)
                                : 1.f;
                            color.x() = mTint.x() * intensity;
                            color.y() = mTint.y() * intensity;
                            color.z() = mTint.z() * intensity;
                        }
                        else
                        {
                            color.x() *= mTint.x();
                            color.y() *= mTint.y();
                            color.z() *= mTint.z();
                        }
                    }
                    colors->dirty();
                    if (mReplaceVertexColorRgb && mPreserveVertexColorIntensity)
                        ++mPreservedVertexColorIntensityArrays;
                    return true;
                }

                if (osg::Vec4ubArray* colors = dynamic_cast<osg::Vec4ubArray*>(existingColors))
                {
                    const auto tintByte = [](float value) {
                        return static_cast<unsigned char>(std::clamp(value * 255.f + 0.5f, 0.f, 255.f));
                    };
                    unsigned char maxRed = 0;
                    if (mReplaceVertexColorRgb && mPreserveVertexColorIntensity)
                    {
                        for (const osg::Vec4ub& color : *colors)
                            maxRed = std::max(maxRed, color.r());
                    }
                    for (osg::Vec4ub& color : *colors)
                    {
                        if (mReplaceVertexColorRgb)
                        {
                            const float intensity = mPreserveVertexColorIntensity && maxRed > 0
                                ? std::clamp(static_cast<float>(color.r()) / static_cast<float>(maxRed), 0.f, 1.f)
                                : 1.f;
                            color.r() = tintByte(mTint.x() * intensity);
                            color.g() = tintByte(mTint.y() * intensity);
                            color.b() = tintByte(mTint.z() * intensity);
                        }
                        else
                        {
                            color.r() = static_cast<unsigned char>(std::clamp(color.r() * mTint.x(), 0.f, 255.f));
                            color.g() = static_cast<unsigned char>(std::clamp(color.g() * mTint.y(), 0.f, 255.f));
                            color.b() = static_cast<unsigned char>(std::clamp(color.b() * mTint.z(), 0.f, 255.f));
                        }
                    }
                    colors->dirty();
                    if (mReplaceVertexColorRgb && mPreserveVertexColorIntensity)
                        ++mPreservedVertexColorIntensityArrays;
                    return true;
                }

                return false;
            }

            void applyTint(osg::StateSet* stateSet) const
            {
                osg::ref_ptr<osg::Material> material = new osg::Material;
                if (const osg::Material* existing
                    = dynamic_cast<const osg::Material*>(stateSet->getAttribute(osg::StateAttribute::MATERIAL)))
                    material = static_cast<osg::Material*>(existing->clone(osg::CopyOp::DEEP_COPY_ALL));

                const bool neutralTint = mTint == osg::Vec4f(1.f, 1.f, 1.f, 1.f);
                const osg::Vec4f materialTint
                    = mNeutralMaterialWhenReplacing ? osg::Vec4f(1.f, 1.f, 1.f, 1.f) : mTint;
                material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
                material->setDiffuse(osg::Material::FRONT_AND_BACK, materialTint);
                material->setAmbient(osg::Material::FRONT_AND_BACK, materialTint);
                if (neutralTint)
                {
                    material->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
                    const float emission = std::min(mEmissionStrength, 1.f);
                    material->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(emission, emission, emission, 1.f));
                    material->setShininess(osg::Material::FRONT_AND_BACK, 0.f);
                    if (mEmissionStrength > 0.f)
                        stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                }
                else if (mEmissionStrength > 0.f)
                {
                    osg::Vec4f emission(std::min(mTint.x() * mEmissionStrength, 1.f),
                        std::min(mTint.y() * mEmissionStrength, 1.f),
                        std::min(mTint.z() * mEmissionStrength, 1.f), 1.f);
                    material->setEmission(osg::Material::FRONT_AND_BACK, emission);
                    material->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.15f, 0.15f, 0.15f, 1.f));
                    material->setShininess(osg::Material::FRONT_AND_BACK, 12.f);
                    stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                }
                stateSet->setAttributeAndModes(material, osg::StateAttribute::ON);
            }

            void applyDrawable(osg::Drawable& drawable) const
            {
                const bool neutralTint = mTint == osg::Vec4f(1.f, 1.f, 1.f, 1.f);
                applyTint(drawable.getOrCreateStateSet());
                if (osg::Geometry* geometry = drawable.asGeometry())
                    if (!neutralTint && neutralizeVertexColors(*geometry))
                        ++mNeutralizedVertexColorArrays;
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* source = rig->getSourceGeometry())
                    {
                        applyTint(source->getOrCreateStateSet());
                        if (!neutralTint && neutralizeVertexColors(*source))
                            ++mNeutralizedVertexColorArrays;
                    }
                    for (unsigned int i = 0; i < 2; ++i)
                        if (osg::Geometry* geometry = rig->getRenderGeometry(i))
                        {
                            applyTint(geometry->getOrCreateStateSet());
                            if (!neutralTint && neutralizeVertexColors(*geometry))
                                ++mNeutralizedVertexColorArrays;
                        }
                }
            }

            osg::Vec4f mTint;
            float mEmissionStrength = 0.f;
            bool mReplaceVertexColorRgb = false;
            bool mNeutralMaterialWhenReplacing = false;
            bool mPreserveVertexColorIntensity = false;

        public:
            mutable unsigned int mNeutralizedVertexColorArrays = 0;
            mutable unsigned int mPreservedVertexColorIntensityArrays = 0;
        };

        class DisableCullVisitor : public osg::NodeVisitor
        {
        public:
            DisableCullVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                disableCull(node.getOrCreateStateSet());
                traverse(node);
            }

            void apply(osg::Geode& geode) override
            {
                disableCull(geode.getOrCreateStateSet());
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        disableCull(drawable->getOrCreateStateSet());
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { disableCull(drawable.getOrCreateStateSet()); }

        private:
            void disableCull(osg::StateSet* stateSet) const
            {
                stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            }
        };

        class FalloutCutoutAlphaVisitor : public osg::NodeVisitor
        {
        public:
            FalloutCutoutAlphaVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                applyCutoutState(node.getOrCreateStateSet());
                traverse(node);
            }

            void apply(osg::Geode& geode) override
            {
                applyCutoutState(geode.getOrCreateStateSet());
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        applyDrawable(*drawable);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

            unsigned int getAppliedCount() const { return mApplied; }

        private:
            void applyDrawable(osg::Drawable& drawable)
            {
                applyCutoutState(drawable.getOrCreateStateSet());
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* source = rig->getSourceGeometry())
                        applyCutoutState(source->getOrCreateStateSet());
                    for (unsigned int i = 0; i < 2; ++i)
                        if (osg::Geometry* geometry = rig->getRenderGeometry(i))
                            applyCutoutState(geometry->getOrCreateStateSet());
                }
            }

            void applyCutoutState(osg::StateSet* stateSet)
            {
                if (stateSet == nullptr)
                    return;

                osg::ref_ptr<osg::AlphaFunc> alphaFunc = new osg::AlphaFunc(osg::AlphaFunc::GREATER, 0.5f);
                stateSet->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                stateSet->setMode(GL_ALPHA_TEST, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateSet->setAttributeAndModes(alphaFunc, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateSet->setRenderingHint(osg::StateSet::OPAQUE_BIN);
                // Hair/beard geometry uses vertex or texture alpha for strand cutouts.
                // Keep blending disabled so it does not bleed through hats, but do not force final alpha to 1.
                stateSet->setDefine("FORCE_OPAQUE", "0", osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                ++mApplied;
            }

            unsigned int mApplied = 0;
        };

        class ForceOpaqueNoBlendVisitor : public osg::NodeVisitor
        {
        public:
            ForceOpaqueNoBlendVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                applyOpaqueState(node.getOrCreateStateSet());
                traverse(node);
            }

            void apply(osg::Geode& geode) override
            {
                applyOpaqueState(geode.getOrCreateStateSet());
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        applyDrawable(*drawable);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

            unsigned int getAppliedCount() const { return mApplied; }

        private:
            void applyDrawable(osg::Drawable& drawable)
            {
                applyOpaqueState(drawable.getOrCreateStateSet());
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* source = rig->getSourceGeometry())
                        applyOpaqueState(source->getOrCreateStateSet());
                    for (unsigned int i = 0; i < 2; ++i)
                        if (osg::Geometry* geometry = rig->getRenderGeometry(i))
                            applyOpaqueState(geometry->getOrCreateStateSet());
                }
            }

            void applyOpaqueState(osg::StateSet* stateSet)
            {
                if (stateSet == nullptr)
                    return;

                stateSet->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                stateSet->setMode(GL_ALPHA_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                stateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                osg::ref_ptr<osg::Depth> depth = new SceneUtil::AutoDepth(osg::Depth::LEQUAL);
                depth->setWriteMask(true);
                stateSet->setAttributeAndModes(depth, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateSet->setRenderingHint(osg::StateSet::OPAQUE_BIN);
                stateSet->setDefine("FORCE_OPAQUE", "1", osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                ++mApplied;
            }

            unsigned int mApplied = 0;
        };

        class FalloutProofMouthDriver : public osg::NodeCallback
        {
        public:
            FalloutProofMouthDriver(const MWWorld::Ptr& actor, std::string model)
                : mActor(actor)
                , mModel(Misc::StringUtils::lowerCase(std::move(model)))
            {
            }

            void operator()(osg::Node* node, osg::NodeVisitor* nv) override
            {
                osg::PositionAttitudeTransform* transform = dynamic_cast<osg::PositionAttitudeTransform*>(node);
                if (transform != nullptr && !mBaseCaptured)
                {
                    mBasePosition = transform->getPosition();
                    mBaseCaptured = true;
                }
                const bool forceOpen = std::getenv("OPENMW_FNV_PROOF_MOUTH_FORCE_OPEN") != nullptr;
                if (transform != nullptr && (forceOpen || MWBase::Environment::get().getSoundManager()->sayActive(mActor)))
                {
                    const float lip = forceOpen ? 1.f
                                                : MWBase::Environment::get().getSoundManager()->getSaySoundLipValue(mActor);
                    const float loudness = forceOpen
                        ? 1.f
                        : MWBase::Environment::get().getSoundManager()->getSaySoundLoudness(mActor);
                    const float open = forceOpen ? 1.f : std::clamp(std::max(lip, loudness * 5.0f), 0.f, 0.65f);
                    osg::Vec3f offset(0.f, -0.15f * open, -1.8f * open);
                    osg::Vec3f scale(1.f, 1.f, 1.f + 0.24f * open);
                    if (mModel.find("teethlower") != std::string::npos)
                    {
                        offset.set(0.f, -0.25f * open, -3.2f * open);
                        scale.set(1.f, 1.f, 1.f);
                    }
                    else if (mModel.find("teethupper") != std::string::npos)
                    {
                        offset.set(0.f, -0.05f * open, 0.1f * open);
                        scale.set(1.f, 1.f, 1.f);
                    }
                    else if (mModel.find("tongue") != std::string::npos)
                    {
                        offset.set(0.f, -0.25f * open, -2.4f * open);
                        scale.set(1.f, 1.f, 1.f);
                    }
                    transform->setScale(scale);
                    transform->setPosition(mBasePosition + offset);

                    if (!mLogged)
                    {
                        Log(Debug::Info) << "FNV/ESM4 proof: mouth driver active for " << mActor.toString()
                                         << " model=" << mModel << " lip=" << lip << " loudness=" << loudness
                                         << " open=" << open << " force=" << forceOpen
                                         << " offset=(" << offset.x() << "," << offset.y() << "," << offset.z()
                                         << ")";
                        mLogged = true;
                    }
                }
                else if (transform != nullptr)
                {
                    transform->setScale(osg::Vec3f(1.f, 1.f, 1.f));
                    transform->setPosition(mBasePosition);
                }

                traverse(node, nv);
            }

        private:
            MWWorld::Ptr mActor;
            std::string mModel;
            osg::Vec3f mBasePosition;
            bool mLogged = false;
            bool mBaseCaptured = false;
        };

        class FalloutProofDialogueBonePose : public osg::NodeCallback
        {
        public:
            FalloutProofDialogueBonePose(std::string boneName, const osg::Quat& rotation)
                : mBoneName(std::move(boneName))
                , mRotation(rotation)
            {
            }

            void operator()(osg::Node* node, osg::NodeVisitor* nv) override
            {
                osgAnimation::Bone* bone = dynamic_cast<osgAnimation::Bone*>(node);
                osg::MatrixTransform* transform = bone != nullptr ? bone : dynamic_cast<osg::MatrixTransform*>(node);
                if (transform != nullptr)
                {
                    if (!mBaseCaptured)
                    {
                        mBaseMatrix = transform->getMatrix();
                        mBaseCaptured = true;
                    }

                    osg::Matrixf posed = mBaseMatrix;
                    posed.setRotate(mRotation * mBaseMatrix.getRotate());
                    transform->setMatrix(posed);

                    if (bone != nullptr)
                    {
                        if (osgAnimation::Bone* parent = bone->getBoneParent())
                            bone->setMatrixInSkeletonSpace(posed * parent->getMatrixInSkeletonSpace());
                        else
                            bone->setMatrixInSkeletonSpace(posed);
                    }

                    if (!mLogged)
                    {
                        const osg::Vec3f trans = posed.getTrans();
                        Log(Debug::Info) << "FNV/ESM4 proof: dialogue pose applied bone=" << mBoneName
                                         << " localTrans=(" << trans.x() << "," << trans.y() << "," << trans.z()
                                         << ")";
                        mLogged = true;
                    }
                }

                traverse(node, nv);
            }

        private:
            std::string mBoneName;
            osg::Quat mRotation;
            osg::Matrixf mBaseMatrix;
            bool mBaseCaptured = false;
            bool mLogged = false;
        };

        osg::Vec4f getHairTint(const ESM4::Npc& traits)
        {
            return osg::Vec4f(traits.mHairColour.red / 255.f, traits.mHairColour.green / 255.f,
                traits.mHairColour.blue / 255.f, 1.f);
        }

        osg::Vec4f getFalloutRenderHairTint(const ESM4::Npc& traits) { return getHairTint(traits); }

        float getFalloutHairEmissionStrength(const osg::Vec4f&)
        {
            const char* value = std::getenv("OPENMW_FNV_HAIR_EMISSION_STRENGTH");
            if (value == nullptr || value[0] == '\0')
                return 0.f;

            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value)
                return 0.f;

            return std::clamp(parsed, 0.f, 1.f);
        }

        bool isFonvMiscHeadPart(const ESM4::HeadPart& part)
        {
            if (part.mType != ESM4::HeadPart::Type_Misc)
                return false;

            const std::string name = Misc::StringUtils::lowerCase(part.mEditorId + " " + part.mModel);
            return name.find("beard") != std::string::npos || name.find("eyebrow") != std::string::npos
                || name.find("hair") != std::string::npos;
        }

        bool isFonvFacialHairHeadPart(const ESM4::HeadPart& part)
        {
            if (part.mType == ESM4::HeadPart::Type_FacialHair)
                return true;

            const std::string name = Misc::StringUtils::lowerCase(part.mEditorId + " " + part.mModel);
            return name.find("beard") != std::string::npos || name.find("facial") != std::string::npos;
        }

        bool containsAny(std::string_view value, std::initializer_list<std::string_view> needles)
        {
            for (std::string_view needle : needles)
            {
                if (value.find(needle) != std::string_view::npos)
                    return true;
            }
            return false;
        }

        std::pair<unsigned int, float> summarizeCoefficients(const std::vector<float>& values)
        {
            unsigned int nonZero = 0;
            float absTotal = 0.f;
            for (float value : values)
            {
                if (std::abs(value) <= 0.0001f)
                    continue;
                ++nonZero;
                absTotal += std::abs(value);
            }
            return { nonZero, absTotal };
        }

        std::string formatFalloutFormIndex(const ESM::FormId& id)
        {
            std::ostringstream stream;
            stream << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << id.mIndex;
            return stream.str();
        }

        std::string falloutProofFormToken(std::string_view value)
        {
            while (!value.empty()
                && (std::isspace(static_cast<unsigned char>(value.front())) || value.front() == '"' || value.front() == '\''))
                value.remove_prefix(1);
            while (!value.empty()
                && (std::isspace(static_cast<unsigned char>(value.back())) || value.back() == '"' || value.back() == '\''))
                value.remove_suffix(1);

            std::string text(value);
            Misc::StringUtils::lowerCaseInPlace(text);
            constexpr std::string_view prefix = "formid:";
            if (text.rfind(prefix, 0) == 0)
                text.erase(0, prefix.size());
            if (text.rfind("0x", 0) == 0)
                text.erase(0, 2);
            if (text.empty())
                return {};
            if (!std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
                return {};
            if (text.size() > 6)
                text.erase(0, text.size() - 6);
            while (text.size() < 6)
                text.insert(text.begin(), '0');
            return text;
        }

        std::string escapeFalloutProofTargetRegex(std::string_view value)
        {
            std::string escaped;
            escaped.reserve(value.size() * 2);
            for (char c : value)
            {
                switch (c)
                {
                    case '\\':
                    case '.':
                    case '^':
                    case '$':
                    case '|':
                    case '(':
                    case ')':
                    case '[':
                    case ']':
                    case '{':
                    case '}':
                    case '*':
                    case '+':
                    case '?':
                        escaped.push_back('\\');
                        break;
                    default:
                        break;
                }
                escaped.push_back(c);
            }
            return escaped;
        }

        std::string unescapeFalloutProofTargetJsonString(std::string_view value)
        {
            std::string result;
            result.reserve(value.size());
            bool escaped = false;
            for (char c : value)
            {
                if (escaped)
                {
                    switch (c)
                    {
                        case '"':
                        case '\\':
                        case '/':
                            result.push_back(c);
                            break;
                        case 'n':
                            result.push_back('\n');
                            break;
                        case 'r':
                            result.push_back('\r');
                            break;
                        case 't':
                            result.push_back('\t');
                            break;
                        default:
                            result.push_back(c);
                            break;
                    }
                    escaped = false;
                    continue;
                }
                if (c == '\\')
                {
                    escaped = true;
                    continue;
                }
                result.push_back(c);
            }
            return result;
        }

        bool readFalloutProofTargetRuntimeString(const std::string& content, std::string_view key, std::string& value)
        {
            try
            {
                const std::regex pattern("\"" + escapeFalloutProofTargetRegex(key)
                    + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
                std::smatch match;
                if (!std::regex_search(content, match, pattern))
                    return false;
                value = unescapeFalloutProofTargetJsonString(match[1].str());
                return !value.empty();
            }
            catch (...)
            {
                return false;
            }
        }

        bool readFalloutProofTargetRuntimeFile(const char* path, std::string& content)
        {
            if (path == nullptr || path[0] == '\0')
                return false;
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            std::ostringstream stream;
            stream << input.rdbuf();
            content = stream.str();
            return true;
        }

        void logFalloutLiveRuntimeActorKitSelector(std::string_view key, const std::string& value)
        {
            if (value.empty())
                return;

            static std::map<std::string, std::string> loggedValues;
            const std::string keyString(key);
            const std::string fingerprint = keyString + "=" + value;
            if (loggedValues[keyString] == fingerprint)
                return;

            loggedValues[keyString] = fingerprint;
            Log(Debug::Info) << "FNV/ESM4 live runtime: actor-kit selector key=" << key
                             << " value=\"" << value << "\" file=\""
                             << (std::getenv("OPENMW_FNV_LIVE_RUNTIME_COMMAND_FILE") != nullptr
                                     ? std::getenv("OPENMW_FNV_LIVE_RUNTIME_COMMAND_FILE")
                                     : "")
                             << "\" runtime=runtime-supported gate=runtime-live-actor-kit-controls";
        }

        std::string readFalloutLiveRuntimeCommandString(std::initializer_list<std::string_view> keys)
        {
            const char* livePath = std::getenv("OPENMW_FNV_LIVE_RUNTIME_COMMAND_FILE");
            if (livePath == nullptr || livePath[0] == '\0')
                return {};

            std::string content;
            if (!readFalloutProofTargetRuntimeFile(livePath, content))
                return {};

            std::string value;
            for (std::string_view key : keys)
            {
                if (readFalloutProofTargetRuntimeString(content, key, value))
                {
                    logFalloutLiveRuntimeActorKitSelector(key, value);
                    return value;
                }
            }
            return {};
        }

        std::string readFalloutLiveRuntimeActorKitFingerprint()
        {
            const char* livePath = std::getenv("OPENMW_FNV_LIVE_RUNTIME_COMMAND_FILE");
            if (livePath == nullptr || livePath[0] == '\0')
                return {};

            std::string content;
            if (!readFalloutProofTargetRuntimeFile(livePath, content))
                return {};

            const auto readValue = [&](std::initializer_list<std::string_view> keys) {
                std::string value;
                for (std::string_view key : keys)
                {
                    if (readFalloutProofTargetRuntimeString(content, key, value))
                        return value;
                }
                return std::string();
            };

            std::ostringstream stream;
            const auto append = [&](std::string_view key, const std::string& value) {
                if (!value.empty())
                    stream << key << '=' << value << ';';
            };
            append("actorTarget", readValue({ "actorTarget", "runtimeTarget", "target" }));
            append("phase", readValue({ "characterBuilderPhase", "phase" }));
            append("parts", readValue({ "actorKitParts", "parts" }));
            append("partModels", readValue({ "actorKitPartModels", "partModels" }));
            append("propSlots", readValue({ "actorKitPropSlots", "propSlots" }));
            append("propModels", readValue({ "actorKitPropModels", "propModels" }));
            append("animationSource", readValue({ "actorKitAnimationSource", "animationSource" }));
            append("animationStartPoint", readValue({ "actorKitAnimationStartPoint", "animationStartPoint" }));
            append("animationGroup", readValue({ "actorKitAnimationGroup", "animationGroup" }));
            append("dialogueMode", readValue({ "actorKitDialogueMode", "dialogueMode" }));
            return stream.str();
        }

        std::string readFalloutLiveRuntimeOrEnvString(
            const char* envName, std::initializer_list<std::string_view> liveKeys)
        {
            if (const std::string liveValue = readFalloutLiveRuntimeCommandString(liveKeys); !liveValue.empty())
                return liveValue;

            const char* value = std::getenv(envName);
            return value != nullptr && value[0] != '\0' ? std::string(value) : std::string();
        }

        std::string readFalloutActorKitRuntimeSelectorForEnv(const char* envName)
        {
            if (std::string_view(envName) == "OPENMW_FNV_CHARACTER_BUILDER_PHASE")
                return readFalloutLiveRuntimeCommandString({ "characterBuilderPhase", "phase" });
            if (std::string_view(envName) == "OPENMW_FNV_ACTOR_KIT_PARTS")
                return readFalloutLiveRuntimeCommandString({ "actorKitParts", "parts" });
            if (std::string_view(envName) == "OPENMW_FNV_ACTOR_KIT_PART_MODELS")
                return readFalloutLiveRuntimeCommandString({ "actorKitPartModels", "partModels" });
            if (std::string_view(envName) == "OPENMW_FNV_ACTOR_KIT_PROP_SLOTS")
                return readFalloutLiveRuntimeCommandString({ "actorKitPropSlots", "propSlots" });
            if (std::string_view(envName) == "OPENMW_FNV_ACTOR_KIT_PROP_MODELS")
                return readFalloutLiveRuntimeCommandString({ "actorKitPropModels", "propModels" });
            if (std::string_view(envName) == "OPENMW_FNV_ACTOR_KIT_ANIMATION_GROUP")
                return readFalloutLiveRuntimeCommandString({ "actorKitAnimationGroup", "animationGroup" });
            if (std::string_view(envName) == "OPENMW_FNV_ACTOR_KIT_ANIMATION_SOURCE")
                return readFalloutLiveRuntimeCommandString({ "actorKitAnimationSource", "animationSource" });
            if (std::string_view(envName) == "OPENMW_FNV_ACTOR_KIT_ANIMATION_STARTPOINT")
                return readFalloutLiveRuntimeCommandString({ "actorKitAnimationStartPoint", "animationStartPoint" });
            return {};
        }

        bool isFalloutLiveRuntimeActorKitActive()
        {
            static constexpr const char* envNames[] = {
                "OPENMW_FNV_CHARACTER_BUILDER_PHASE",
                "OPENMW_FNV_ACTOR_KIT_PARTS",
                "OPENMW_FNV_ACTOR_KIT_PART_MODELS",
                "OPENMW_FNV_ACTOR_KIT_PROP_SLOTS",
                "OPENMW_FNV_ACTOR_KIT_PROP_MODELS",
                "OPENMW_FNV_ACTOR_KIT_ANIMATION_GROUP",
                "OPENMW_FNV_ACTOR_KIT_ANIMATION_SOURCE",
                "OPENMW_FNV_ACTOR_KIT_ANIMATION_STARTPOINT",
            };
            for (const char* envName : envNames)
            {
                if (!readFalloutActorKitRuntimeSelectorForEnv(envName).empty())
                    return true;
            }
            return false;
        }

        bool falloutProofFormTargetMatches(std::string_view candidate, const char* target)
        {
            if (target == nullptr || target[0] == '\0')
                return false;
            const std::string candidateToken = falloutProofFormToken(candidate);
            return !candidateToken.empty() && candidateToken == falloutProofFormToken(target);
        }

        bool isEasyPeteProofActor(const ESM4::Npc& traits)
        {
            return Misc::StringUtils::ciEqual(traits.mEditorId, "GSEasyPete")
                || traits.mId.mIndex == 0x00104c7f;
        }

        const char* getFonvProofActorTarget()
        {
            static std::string liveTarget;
            liveTarget.clear();
            if (const char* livePath = std::getenv("OPENMW_FNV_LIVE_RUNTIME_COMMAND_FILE"))
            {
                std::string content;
                if (readFalloutProofTargetRuntimeFile(livePath, content))
                {
                    for (std::string_view key : { std::string_view("actorTarget"),
                             std::string_view("runtimeTarget"), std::string_view("target") })
                    {
                        if (readFalloutProofTargetRuntimeString(content, key, liveTarget))
                            return liveTarget.c_str();
                    }
                }
            }

            const char* target = std::getenv("OPENMW_PROOF_ACTOR_TARGET");
            if (target == nullptr || target[0] == '\0')
                target = std::getenv("OPENMW_FNV_PROOF_TARGET_NPC");
            return target;
        }

        bool falloutTargetMatches(std::string_view candidate, const char* target)
        {
            return target != nullptr && target[0] != '\0' && !candidate.empty()
                && Misc::StringUtils::ciEqual(candidate, target);
        }

        bool falloutTargetMatchesFormId(const ESM::FormId& id, const char* target)
        {
            return falloutTargetMatches(ESM::RefId(id).toDebugString(), target)
                || falloutTargetMatches(formatFalloutFormIndex(id), target)
                || falloutProofFormTargetMatches(ESM::RefId(id).toDebugString(), target)
                || falloutProofFormTargetMatches(formatFalloutFormIndex(id), target);
        }

        bool isFonvProofTargetActor(const MWWorld::Ptr& ptr, const ESM4::Npc& traits)
        {
            if (std::getenv("OPENMW_FNV_PROOF_ONLY_EASY_PETE") != nullptr)
                return isEasyPeteProofActor(traits);

            const char* target = getFonvProofActorTarget();
            if (target == nullptr || target[0] == '\0')
                return true;

            const ESM4::Npc* base = nullptr;
            if (const MWWorld::LiveCellRef<ESM4::Npc>* ref = ptr.get<ESM4::Npc>())
                base = ref->mBase;

            const std::string refAlias = ptr.getCellRef().getRefId().toDebugString();
            std::string refNum;
            if (ptr.getCell() != nullptr)
                refNum = ptr.getCellRef().getRefNum().toString("FormId:");

            return falloutTargetMatches(refAlias, target)
                || falloutTargetMatches(refNum, target)
                || falloutTargetMatches(traits.mEditorId, target)
                || falloutTargetMatchesFormId(traits.mId, target)
                || (base != nullptr
                    && (falloutTargetMatches(base->mEditorId, target)
                        || falloutTargetMatchesFormId(base->mId, target)));
        }

        std::string getFalloutActorKitAnimationGroup()
        {
            std::string group = readFalloutLiveRuntimeOrEnvString("OPENMW_FNV_ACTOR_KIT_ANIMATION_GROUP",
                { "actorKitAnimationGroup", "animationGroup" });
            if (group.empty())
                return {};

            while (!group.empty()
                && (group.front() == ' ' || group.front() == '\t' || group.front() == '\r'
                    || group.front() == '\n' || group.front() == '"' || group.front() == '\''))
                group.erase(group.begin());
            while (!group.empty()
                && (group.back() == ' ' || group.back() == '\t' || group.back() == '\r'
                    || group.back() == '\n' || group.back() == '"' || group.back() == '\''))
                group.pop_back();
            Misc::StringUtils::lowerCaseInPlace(group);
            return group;
        }

        std::string getFalloutActorKitAnimationSource()
        {
            std::string source = readFalloutLiveRuntimeOrEnvString("OPENMW_FNV_ACTOR_KIT_ANIMATION_SOURCE",
                { "actorKitAnimationSource", "animationSource" });
            if (source.empty())
                return {};

            while (!source.empty()
                && (source.front() == ' ' || source.front() == '\t' || source.front() == '\r'
                    || source.front() == '\n' || source.front() == '"' || source.front() == '\''))
                source.erase(source.begin());
            while (!source.empty()
                && (source.back() == ' ' || source.back() == '\t' || source.back() == '\r'
                    || source.back() == '\n' || source.back() == '"' || source.back() == '\''))
                source.pop_back();
            Misc::StringUtils::lowerCaseInPlace(source);
            if (source == "mtidle" || source == "neutral" || source == "standing")
                return "meshes/characters/_male/locomotion/mtidle.kf";
            if (source == "hands-at-side" || source == "handsatside" || source == "talk-hands-at-side")
                return "meshes/characters/_male/idleanims/talk_handsatside_still2.kf";
            if (source == "pistol-pose" || source == "pistol" || source == "1hpistol")
                return "meshes/characters/_male/idleanims/dlcanch1hpistolpose.kf";
            if (source == "rifle-loiter" || source == "rifle" || source == "2hrloiter")
                return "meshes/characters/_male/idleanims/2hrloiter.kf";
            VFS::Path::normalizeFilenameInPlace(source);
            if (source.rfind("meshes/", 0) != 0)
                source = "meshes/" + source;
            if (!Misc::StringUtils::ciEndsWith(source, ".kf"))
                return {};
            return source;
        }

        float getFalloutActorKitAnimationStartPoint()
        {
            const std::string value = readFalloutLiveRuntimeOrEnvString("OPENMW_FNV_ACTOR_KIT_ANIMATION_STARTPOINT",
                { "actorKitAnimationStartPoint", "animationStartPoint" });
            if (value.empty())
                return 0.f;

            char* end = nullptr;
            const float parsed = std::strtof(value.c_str(), &end);
            if (end == value.c_str() || !std::isfinite(parsed))
                return 0.f;
            return std::clamp(parsed, 0.f, 0.999f);
        }

        void requestFalloutActorKitAnimation(Animation& animation, const MWWorld::Ptr& ptr, const ESM4::Npc& traits)
        {
            const std::string group = getFalloutActorKitAnimationGroup();
            if (group.empty() || !isFonvProofTargetActor(ptr, traits))
                return;

            const float startPoint = getFalloutActorKitAnimationStartPoint();
            const bool available = animation.hasAnimation(group);
            Log(Debug::Info) << "FNV/ESM4 proof: actor-kit animation request actor=" << traits.mEditorId
                             << " ref=" << ptr.getCellRef().getRefId()
                             << " group=" << group
                             << " startPoint=" << startPoint
                             << " available=" << available
                             << " runtime=" << (available ? "runtime-supported" : "known-blocked");

            animation.play(group, MWMechanics::Priority_Scripted, BlendMask_All, false, 1.f, "start", "stop",
                startPoint, 0, true);
        }

        unsigned int countFalloutActorKitSelectorTokens(std::string value)
        {
            for (char& ch : value)
            {
                if (ch == ';' || ch == '|' || ch == '\n' || ch == '\r' || ch == '\t')
                    ch = ',';
            }

            std::istringstream stream(value);
            std::string token;
            unsigned int count = 0;
            while (std::getline(stream, token, ','))
            {
                const auto begin = std::find_if_not(token.begin(), token.end(),
                    [](unsigned char ch) { return std::isspace(ch) || ch == '"' || ch == '\''; });
                const auto end = std::find_if_not(token.rbegin(), token.rend(),
                                     [](unsigned char ch) { return std::isspace(ch) || ch == '"' || ch == '\''; })
                                     .base();
                if (begin < end)
                    ++count;
            }
            return count;
        }

        struct FalloutRuntimePartTreeSummary
        {
            unsigned int total = 0;
            unsigned int duplicateNames = 0;
            std::string firstNames;
        };

        class FalloutRuntimePartTreeVisitor : public osg::NodeVisitor
        {
        public:
            FalloutRuntimePartTreeVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                visit(node);
            }

            void apply(osg::Geode& node) override
            {
                visit(node);
            }

            void apply(osg::Group& node) override
            {
                visit(node);
            }

            void apply(osg::Transform& node) override
            {
                visit(node);
            }

            void apply(osg::MatrixTransform& node) override
            {
                visit(node);
            }

            void apply(osg::PositionAttitudeTransform& node) override
            {
                visit(node);
            }

            FalloutRuntimePartTreeSummary summary() const
            {
                FalloutRuntimePartTreeSummary result;
                result.total = static_cast<unsigned int>(mParts.size());
                for (const auto& [name, count] : mNameCounts)
                {
                    if (count > 1)
                        result.duplicateNames += count - 1;
                    if (result.firstNames.size() < 220)
                    {
                        if (!result.firstNames.empty())
                            result.firstNames += ",";
                        result.firstNames += name;
                    }
                }
                return result;
            }

            const std::vector<osg::ref_ptr<osg::Node>>& parts() const { return mParts; }

        private:
            void visit(osg::Node& node)
            {
                if (Misc::StringUtils::ciStartsWith(node.getName(), "FNV Part "))
                {
                    mParts.push_back(&node);
                    ++mNameCounts[node.getName()];
                }
                traverse(node);
            }

            std::vector<osg::ref_ptr<osg::Node>> mParts;
            std::map<std::string, unsigned int> mNameCounts;
        };

        bool isFalloutRuntimePartParentUnderRoot(const osg::Node* node, const osg::Node* root)
        {
            if (node == nullptr || root == nullptr)
                return false;
            if (node == root)
                return true;
            for (unsigned int i = 0; i < node->getNumParents(); ++i)
            {
                if (isFalloutRuntimePartParentUnderRoot(node->getParent(i), root))
                    return true;
            }
            return false;
        }

        FalloutRuntimePartTreeSummary summarizeFalloutRuntimePartTree(osg::Node* root)
        {
            if (root == nullptr)
                return {};
            FalloutRuntimePartTreeVisitor visitor;
            root->accept(visitor);
            return visitor.summary();
        }

        unsigned int removeFalloutRuntimePartTree(osg::Node* root, unsigned int& staleAfterRemoval)
        {
            staleAfterRemoval = 0;
            if (root == nullptr)
                return 0;

            FalloutRuntimePartTreeVisitor visitor;
            root->accept(visitor);
            const std::vector<osg::ref_ptr<osg::Node>> parts = visitor.parts();
            unsigned int removedParents = 0;
            for (const osg::ref_ptr<osg::Node>& part : parts)
            {
                for (unsigned int parentIndex = 0; part != nullptr && parentIndex < part->getNumParents();)
                {
                    osg::Group* parent = part->getParent(parentIndex);
                    if (parent == nullptr || !isFalloutRuntimePartParentUnderRoot(parent, root))
                    {
                        ++parentIndex;
                        continue;
                    }
                    if (!parent->removeChild(part.get()))
                    {
                        ++parentIndex;
                        break;
                    }
                    ++removedParents;
                }
            }

            for (const osg::ref_ptr<osg::Node>& part : parts)
            {
                if (part == nullptr)
                    continue;
                for (unsigned int i = 0; i < part->getNumParents(); ++i)
                {
                    if (!isFalloutRuntimePartParentUnderRoot(part->getParent(i), root))
                        continue;
                    ++staleAfterRemoval;
                    break;
                }
            }
            return removedParents;
        }

        std::string formatFormIdList(const std::vector<ESM::FormId>& ids, std::size_t maxCount = 16)
        {
            std::ostringstream stream;
            for (std::size_t i = 0; i < ids.size() && i < maxCount; ++i)
            {
                if (i != 0)
                    stream << ",";
                stream << ESM::RefId(ids[i]);
            }
            if (ids.size() > maxCount)
                stream << ",...";
            return stream.str();
        }

        std::string getFonvPackageTypeName(int type)
        {
            switch (type)
            {
                case 0:
                    return "Find";
                case 1:
                    return "Follow";
                case 2:
                    return "Escort";
                case 3:
                    return "Eat";
                case 4:
                    return "Sleep";
                case 5:
                    return "Wander";
                case 6:
                    return "Travel";
                case 7:
                    return "Accompany";
                case 8:
                    return "UseItemAt";
                case 9:
                    return "Ambush";
                case 10:
                    return "Flee";
                case 11:
                    return "Sandbox";
                case 12:
                    return "Sandbox";
                case 13:
                    return "Patrol";
                case 14:
                    return "Guard";
                case 15:
                    return "Dialogue";
                case 16:
                    return "UseWeapon";
                default:
                    return "Type" + std::to_string(type);
            }
        }

        ESM::FormId formIdFromRaw(ESM::FormId32 id)
        {
            return ESM::FormId::fromUint32(id);
        }

        std::string resolvePackageReferenceDetail(const MWWorld::ESMStore* store, ESM::FormId32 rawId)
        {
            if (store == nullptr || rawId == 0)
                return {};

            const ESM::FormId id = formIdFromRaw(rawId);
            const ESM4::Reference* ref = store->get<ESM4::Reference>().search(id);
            if (ref == nullptr)
                return {};

            std::ostringstream stream;
            stream << " refEdid=" << ref->mEditorId << " base=" << ESM::RefId(ref->mBaseObj) << " pos=("
                   << ref->mPos.pos[0] << "," << ref->mPos.pos[1] << "," << ref->mPos.pos[2] << ") rotZ="
                   << ref->mPos.rot[2];

            if (const ESM4::Furniture* furniture = store->get<ESM4::Furniture>().search(ref->mBaseObj))
                stream << " baseFURN=" << furniture->mEditorId << " model=" << furniture->mModel
                       << " activeMarkers=0x" << std::hex << furniture->mActiveMarkerFlags << std::dec;
            else if (const ESM4::Static* stat = store->get<ESM4::Static>().search(ref->mBaseObj))
                stream << " baseSTAT=" << stat->mEditorId << " model=" << stat->mModel;

            return stream.str();
        }

        std::string formatPackageLocation(const ESM4::AIPackage::PLDT& location, const MWWorld::ESMStore* store)
        {
            std::ostringstream stream;
            stream << "type=" << location.type;
            switch (location.type)
            {
                case 0:
                    stream << "(nearRef) ref=" << ESM::RefId(formIdFromRaw(location.location));
                    break;
                case 1:
                    stream << "(inCell) cell=" << ESM::RefId(formIdFromRaw(location.location));
                    break;
                case 2:
                    stream << "(nearCurrent)";
                    break;
                case 3:
                    stream << "(nearEditorLocation)";
                    break;
                case 4:
                    stream << "(objectId) object=" << ESM::RefId(formIdFromRaw(location.location));
                    break;
                case 5:
                    stream << "(objectType) objectType=" << location.location;
                    break;
                case 0xff:
                    stream << "(none)";
                    break;
                default:
                    stream << " raw=" << ESM::RefId(formIdFromRaw(location.location));
                    break;
            }
            stream << resolvePackageReferenceDetail(store, location.location);
            stream << " radius=" << location.radius;
            return stream.str();
        }

        std::string formatPackageTarget(
            const ESM4::AIPackage::PTDT& target, float extra, const MWWorld::ESMStore* store)
        {
            std::ostringstream stream;
            stream << "type=" << target.type;
            switch (target.type)
            {
                case 0:
                    stream << "(specificRef) ref=" << ESM::RefId(formIdFromRaw(target.target));
                    break;
                case 1:
                    stream << "(objectId) object=" << ESM::RefId(formIdFromRaw(target.target));
                    break;
                case 2:
                    stream << "(objectType) objectType=" << target.target;
                    break;
                case 0xff:
                    stream << "(none)";
                    break;
                default:
                    stream << " raw=" << ESM::RefId(formIdFromRaw(target.target));
                    break;
            }
            stream << resolvePackageReferenceDetail(store, target.target);
            stream << " distance=" << target.distance;
            if (extra != 0.f)
                stream << " extra=" << extra;
            return stream.str();
        }

        std::string formatPackageSummary(const ESM4::AIPackage& package, const MWWorld::ESMStore* store)
        {
            std::ostringstream stream;
            stream << getFonvPackageTypeName(package.mData.type) << "(" << package.mData.type << ")"
                   << " schedule={month=" << static_cast<unsigned int>(package.mSchedule.month)
                   << " dayOfWeek=" << static_cast<unsigned int>(package.mSchedule.dayOfWeek)
                   << " date=" << static_cast<unsigned int>(package.mSchedule.date)
                   << " time=" << static_cast<unsigned int>(package.mSchedule.time)
                   << " duration=" << package.mSchedule.duration << "}"
                   << " flags=0x" << std::hex << package.mFo3PackageFlags
                   << " procFlags=0x" << package.mFo3ProcedureFlags
                   << " typeFlags=0x" << package.mFo3TypeSpecificFlags << std::dec
                   << " loc={" << formatPackageLocation(package.mLocation, store) << "}"
                   << " target={" << formatPackageTarget(package.mTarget, package.mFo3TargetUnknown, store) << "}"
                   << " extraLocs=" << package.mExtraLocations.size()
                   << " extraTargets=" << package.mExtraTargets.size();
            return stream.str();
        }

        std::string findExistingTexture(Resource::ResourceSystem* resourceSystem, const std::vector<std::string>& candidates)
        {
            const VFS::Manager* vfs = resourceSystem->getVFS();
            for (const std::string& texture : candidates)
            {
                const VFS::Path::Normalized correctedTexture
                    = Misc::ResourceHelpers::correctTexturePath(VFS::Path::toNormalized(texture), *vfs);
                if (vfs->exists(correctedTexture))
                    return texture;
            }

            return {};
        }

        osg::ref_ptr<osg::Image> getExistingTextureImage(Resource::ResourceSystem* resourceSystem, std::string_view texture)
        {
            if (texture.empty())
                return nullptr;

            const VFS::Path::Normalized correctedTexture
                = Misc::ResourceHelpers::correctTexturePath(VFS::Path::toNormalized(texture), *resourceSystem->getVFS());
            if (!resourceSystem->getVFS()->exists(correctedTexture))
                return nullptr;

            return resourceSystem->getImageManager()->getImage(correctedTexture);
        }

        bool isFullSizeSkinDiffuse(Resource::ResourceSystem* resourceSystem, std::string_view texture)
        {
            osg::ref_ptr<osg::Image> image = getExistingTextureImage(resourceSystem, texture);
            return image != nullptr && image->s() >= 64 && image->t() >= 64;
        }

        std::string findTextureCompanion(
            Resource::ResourceSystem* resourceSystem, std::string_view texture, std::string_view suffix)
        {
            if (texture.empty())
                return {};

            std::string candidate(texture);
            const std::size_t dot = candidate.find_last_of('.');
            if (dot == std::string::npos)
                return {};

            candidate.insert(dot, suffix);
            return findExistingTexture(resourceSystem, { candidate });
        }

        std::string formatTextureImageSummary(Resource::ResourceSystem* resourceSystem, std::string_view texture)
        {
            if (texture.empty())
                return "<none>";

            osg::ref_ptr<osg::Image> image = getExistingTextureImage(resourceSystem, texture);
            if (image == nullptr)
                return std::string(texture) + " <missing>";

            std::ostringstream stream;
            stream << texture << " " << image->s() << "x" << image->t() << " pf=0x" << std::hex
                   << image->getPixelFormat() << std::dec;
            return stream.str();
        }

        std::string findFonvTextureNormalCompanion(Resource::ResourceSystem* resourceSystem, std::string_view texture)
        {
            return findTextureCompanion(resourceSystem, texture, "_n");
        }

        std::string findFonvTextureSkinCompanion(Resource::ResourceSystem* resourceSystem, std::string_view texture)
        {
            return findTextureCompanion(resourceSystem, texture, "_sk");
        }

        std::string findFonvNpcFaceTexture(Resource::ResourceSystem* resourceSystem, const ESM4::Npc& traits)
        {
            const std::string formIndex = formatFalloutFormIndex(traits.mId);
            const std::string texture = findExistingTexture(resourceSystem,
                { "textures/characters/facemods/falloutnv.esm/" + formIndex + "_0.dds",
                    "textures/characters/facemods/falloutnv.esm/" + formIndex + "_1.dds" });
            if (texture.empty())
                return {};

            osg::ref_ptr<osg::Image> image = getExistingTextureImage(resourceSystem, texture);
            if (image == nullptr || image->s() < 64 || image->t() < 64)
                return {};

            Log(Debug::Info) << "FNV/ESM4 diag: loaded exported NPC FaceGen texture source " << texture << " for "
                             << traits.mEditorId << " "
                             << (image != nullptr ? std::to_string(image->s()) + "x" + std::to_string(image->t())
                                                  : std::string("<unloaded>"))
                             << " runtime=loaded-pending-exact-facegen-texture-synthesis gate=runtime-fnv-facegen-source";
            return texture;
        }

        std::string findFonvNpcFaceDetailTexture(Resource::ResourceSystem* resourceSystem, const ESM4::Npc& traits)
        {
            const std::string formIndex = formatFalloutFormIndex(traits.mId);
            const std::string texture = findExistingTexture(resourceSystem,
                { "textures/characters/facemods/falloutnv.esm/" + formIndex + "_0.dds" });
            if (texture.empty())
                return {};

            osg::ref_ptr<osg::Image> image = getExistingTextureImage(resourceSystem, texture);
            if (image == nullptr || image->s() < 64 || image->t() < 64)
                return {};

            return texture;
        }

        std::string findFonvNpcFaceNormalTexture(Resource::ResourceSystem* resourceSystem, const ESM4::Npc& traits)
        {
            const std::string formIndex = formatFalloutFormIndex(traits.mId);
            return findExistingTexture(resourceSystem,
                { "textures/characters/facemods/falloutnv.esm/" + formIndex + "_1.dds",
                    "textures/characters/facemods/falloutnv.esm/" + formIndex + "_n.dds" });
        }

        std::string getFonvFaceGenTextureMode()
        {
            const char* value = std::getenv("OPENMW_FNV_FACEGEN_TEXTURE_MODE");
            if (value == nullptr || value[0] == '\0')
                return "detail";

            std::string mode(value);
            Misc::StringUtils::lowerCaseInPlace(mode);
            if (mode == "generated" || mode == "generated-diffuse" || mode == "generated_diffuse"
                || mode == "composite" || mode == "composite-diffuse")
                return "generated-diffuse";
            if (mode == "direct" || mode == "diffuse" || mode == "direct-diffuse" || mode == "direct_diffuse")
                return "direct-diffuse";
            if (mode == "none" || mode == "source-only" || mode == "disabled")
                return "source-only";
            return "detail";
        }

        std::string findFonvNpcBodyTexture(Resource::ResourceSystem* resourceSystem, const ESM4::Npc& traits, bool isFemale)
        {
            const std::string formIndex = formatFalloutFormIndex(traits.mId);
            const std::string suffix = isFemale ? "modbodyfemale.dds" : "modbodymale.dds";
            const std::string texture = findExistingTexture(
                resourceSystem, { "textures/characters/bodymods/falloutnv.esm/" + formIndex + suffix });
            if (texture.empty())
                return {};

            osg::ref_ptr<osg::Image> image = getExistingTextureImage(resourceSystem, texture);
            if (image == nullptr || isFullSizeSkinDiffuse(resourceSystem, texture))
                return texture;

            Log(Debug::Info) << "FNV/ESM4 diag: treating baked NPC body texture " << texture << " for "
                             << traits.mEditorId << " as tint-only " << image->s() << "x" << image->t()
                             << "; preserving race body diffuse";
            return {};
        }

        std::string findFonvNpcBodyDetailTexture(
            Resource::ResourceSystem* resourceSystem, const ESM4::Npc& traits, bool isFemale)
        {
            const std::string formIndex = formatFalloutFormIndex(traits.mId);
            const std::string suffix = isFemale ? "modbodyfemale.dds" : "modbodymale.dds";
            const std::string texture = findExistingTexture(
                resourceSystem, { "textures/characters/bodymods/falloutnv.esm/" + formIndex + suffix });
            if (texture.empty())
                return {};

            osg::ref_ptr<osg::Image> image = getExistingTextureImage(resourceSystem, texture);
            if (image == nullptr)
                return {};

            return texture;
        }

        bool getAverageTextureTint(Resource::ResourceSystem* resourceSystem, std::string_view texture, osg::Vec4f& tint,
            int& width, int& height)
        {
            osg::ref_ptr<osg::Image> image = getExistingTextureImage(resourceSystem, texture);
            if (image == nullptr || image->s() <= 0 || image->t() <= 0)
                return false;

            width = image->s();
            height = image->t();

            osg::Vec4f total(0.f, 0.f, 0.f, 0.f);
            unsigned int samples = 0;
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const osg::Vec4f sample = image->getColor(x, y);
                    if (!std::isfinite(sample.x()) || !std::isfinite(sample.y()) || !std::isfinite(sample.z()))
                        continue;

                    total.x() += sample.x();
                    total.y() += sample.y();
                    total.z() += sample.z();
                    total.w() += std::isfinite(sample.w()) ? sample.w() : 1.f;
                    ++samples;
                }
            }

            if (samples == 0)
                return false;

            const float invSamples = 1.f / static_cast<float>(samples);
            const osg::Vec4f average = total * invSamples;
            tint = osg::Vec4f(std::clamp(average.x(), 0.f, 1.f),
                std::clamp(average.y(), 0.f, 1.f), std::clamp(average.z(), 0.f, 1.f), 1.f);
            return true;
        }

        osg::Vec4f sampleImageColorClamped(const osg::Image& image, int x, int y)
        {
            if (image.s() <= 0 || image.t() <= 0)
                return osg::Vec4f(1.f, 1.f, 1.f, 1.f);

            const int clampedX = std::clamp(x, 0, image.s() - 1);
            const int clampedY = std::clamp(y, 0, image.t() - 1);
            osg::Vec4f color = image.getColor(clampedX, clampedY);
            color.x() = std::isfinite(color.x()) ? std::clamp(color.x(), 0.f, 1.f) : 1.f;
            color.y() = std::isfinite(color.y()) ? std::clamp(color.y(), 0.f, 1.f) : 1.f;
            color.z() = std::isfinite(color.z()) ? std::clamp(color.z(), 0.f, 1.f) : 1.f;
            color.w() = std::isfinite(color.w()) ? std::clamp(color.w(), 0.f, 1.f) : 1.f;
            return color;
        }

        osg::ref_ptr<osg::Image> generateFonvFaceGenDiffuse(Resource::ResourceSystem* resourceSystem,
            std::string_view baseDiffuseTexture, std::string_view npcFaceTexture, const ESM4::Npc& traits)
        {
            osg::ref_ptr<osg::Image> base = getExistingTextureImage(resourceSystem, baseDiffuseTexture);
            osg::ref_ptr<osg::Image> face = getExistingTextureImage(resourceSystem, npcFaceTexture);
            if (base == nullptr || face == nullptr || base->s() <= 0 || base->t() <= 0 || face->s() <= 0
                || face->t() <= 0)
                return nullptr;

            osg::ref_ptr<osg::Image> generated = new osg::Image;
            generated->setFileName("generated/fnv-facegen/" + formatFalloutFormIndex(traits.mId) + "-head-diffuse");
            generated->setOrigin(osg::Image::TOP_LEFT);
            generated->allocateImage(base->s(), base->t(), 1, GL_RGBA, GL_UNSIGNED_BYTE);

            for (int y = 0; y < base->t(); ++y)
            {
                const int faceY = static_cast<int>(
                    (static_cast<float>(y) + 0.5f) * static_cast<float>(face->t()) / static_cast<float>(base->t()));
                for (int x = 0; x < base->s(); ++x)
                {
                    const int faceX = static_cast<int>((static_cast<float>(x) + 0.5f) * static_cast<float>(face->s())
                        / static_cast<float>(base->s()));
                    const osg::Vec4f baseColor = sampleImageColorClamped(*base, x, y);
                    const osg::Vec4f faceColor = sampleImageColorClamped(*face, faceX, faceY);
                    osg::Vec4f composed(std::clamp(baseColor.x() * faceColor.x() * 2.f, 0.f, 1.f),
                        std::clamp(baseColor.y() * faceColor.y() * 2.f, 0.f, 1.f),
                        std::clamp(baseColor.z() * faceColor.z() * 2.f, 0.f, 1.f), baseColor.w());
                    generated->setColor(composed, x, y);
                }
            }

            Log(Debug::Info) << "FNV/ESM4 diag: generated NPC FaceGen diffuse " << generated->getFileName()
                             << " base=" << baseDiffuseTexture << " " << base->s() << "x" << base->t()
                             << " face=" << npcFaceTexture << " " << face->s() << "x" << face->t()
                             << " for " << traits.mEditorId
                             << " runtime=loaded-pending-exact-facegen-texture-synthesis"
                             << " gate=runtime-fnv-facegen-generated-diffuse-applied";
            return generated;
        }

        std::string findFonvNpcBodyNormalTexture(
            Resource::ResourceSystem* resourceSystem, const ESM4::Npc& traits, bool isFemale)
        {
            const std::string formIndex = formatFalloutFormIndex(traits.mId);
            const std::string suffix = isFemale ? "modbodyfemale" : "modbodymale";
            return findExistingTexture(resourceSystem,
                { "textures/characters/bodymods/falloutnv.esm/" + formIndex + suffix + "_n.dds",
                    "textures/characters/bodymods/falloutnv.esm/" + formIndex + suffix + "_1.dds" });
        }

        void overrideTextureSlot(std::string_view texture, std::string_view textureType, unsigned int unit,
            Resource::ResourceSystem* resourceSystem, osg::StateSet& stateset)
        {
            if (texture.empty())
                return;

            const VFS::Path::Normalized correctedTexture
                = Misc::ResourceHelpers::correctTexturePath(VFS::Path::toNormalized(texture), *resourceSystem->getVFS());
            osg::ref_ptr<osg::Texture2D> tex
                = new osg::Texture2D(resourceSystem->getImageManager()->getImage(correctedTexture));
            tex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            tex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            resourceSystem->getSceneManager()->applyFilterSettings(tex);

            stateset.setTextureAttributeAndModes(unit, tex, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateset.setTextureAttributeAndModes(unit, new SceneUtil::TextureType(std::string(textureType)),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }

        void overrideTextureSlot(osg::Image* image, std::string_view textureType, unsigned int unit,
            Resource::ResourceSystem* resourceSystem, osg::StateSet& stateset)
        {
            if (image == nullptr)
                return;

            osg::ref_ptr<osg::Texture2D> tex = new osg::Texture2D(image);
            tex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            tex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            resourceSystem->getSceneManager()->applyFilterSettings(tex);

            stateset.setTextureAttributeAndModes(unit, tex, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateset.setTextureAttributeAndModes(unit, new SceneUtil::TextureType(std::string(textureType)),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }

        void overrideTextureSlot(std::string_view texture, std::string_view textureType, unsigned int unit,
            Resource::ResourceSystem* resourceSystem, osg::Node& node)
        {
            if (texture.empty())
                return;

            osg::ref_ptr<osg::StateSet> stateset;
            if (const osg::StateSet* const src = node.getStateSet())
                stateset = new osg::StateSet(*src, osg::CopyOp::SHALLOW_COPY);
            else
                stateset = new osg::StateSet;

            overrideTextureSlot(texture, textureType, unit, resourceSystem, *stateset);
            node.setStateSet(stateset);
        }

        void overrideTextureSlot(osg::Image* image, std::string_view textureType, unsigned int unit,
            Resource::ResourceSystem* resourceSystem, osg::Node& node)
        {
            if (image == nullptr)
                return;

            osg::ref_ptr<osg::StateSet> stateset;
            if (const osg::StateSet* const src = node.getStateSet())
                stateset = new osg::StateSet(*src, osg::CopyOp::SHALLOW_COPY);
            else
                stateset = new osg::StateSet;

            overrideTextureSlot(image, textureType, unit, resourceSystem, *stateset);
            node.setStateSet(stateset);
        }

        class FalloutPartTextureOverrideVisitor : public osg::NodeVisitor
        {
        public:
            FalloutPartTextureOverrideVisitor(std::string_view texture, std::string_view textureType, unsigned int unit,
                Resource::ResourceSystem* resourceSystem)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mTexture(texture)
                , mTextureType(textureType)
                , mUnit(unit)
                , mResourceSystem(resourceSystem)
            {
            }

            void apply(osg::Node& node) override
            {
                overrideTextureSlot(mTexture, mTextureType, mUnit, mResourceSystem, node);
                traverse(node);
            }

            void apply(osg::Drawable& drawable) override
            {
                overrideTextureSlot(mTexture, mTextureType, mUnit, mResourceSystem, *drawable.getOrCreateStateSet());
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* source = rig->getSourceGeometry())
                        overrideTextureSlot(mTexture, mTextureType, mUnit, mResourceSystem, *source->getOrCreateStateSet());
                    for (unsigned int i = 0; i < 2; ++i)
                    {
                        if (osg::Geometry* geometry = rig->getRenderGeometry(i))
                            overrideTextureSlot(
                                mTexture, mTextureType, mUnit, mResourceSystem, *geometry->getOrCreateStateSet());
                    }
                }
            }

        private:
            std::string_view mTexture;
            std::string_view mTextureType;
            unsigned int mUnit;
            Resource::ResourceSystem* mResourceSystem;
        };

        class FalloutPartImageTextureOverrideVisitor : public osg::NodeVisitor
        {
        public:
            FalloutPartImageTextureOverrideVisitor(
                osg::Image* image, std::string_view textureType, unsigned int unit, Resource::ResourceSystem* resourceSystem)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mImage(image)
                , mTextureType(textureType)
                , mUnit(unit)
                , mResourceSystem(resourceSystem)
            {
            }

            void apply(osg::Node& node) override
            {
                overrideTextureSlot(mImage.get(), mTextureType, mUnit, mResourceSystem, node);
                traverse(node);
            }

            void apply(osg::Drawable& drawable) override
            {
                overrideTextureSlot(mImage.get(), mTextureType, mUnit, mResourceSystem, *drawable.getOrCreateStateSet());
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* source = rig->getSourceGeometry())
                        overrideTextureSlot(
                            mImage.get(), mTextureType, mUnit, mResourceSystem, *source->getOrCreateStateSet());
                    for (unsigned int i = 0; i < 2; ++i)
                    {
                        if (osg::Geometry* geometry = rig->getRenderGeometry(i))
                            overrideTextureSlot(
                                mImage.get(), mTextureType, mUnit, mResourceSystem, *geometry->getOrCreateStateSet());
                    }
                }
            }

        private:
            osg::ref_ptr<osg::Image> mImage;
            std::string_view mTextureType;
            unsigned int mUnit;
            Resource::ResourceSystem* mResourceSystem;
        };

        void overrideFalloutPartTexture(std::string_view texture, std::string_view textureType, unsigned int unit,
            Resource::ResourceSystem* resourceSystem, osg::Node& node)
        {
            if (texture.empty())
                return;

            FalloutPartTextureOverrideVisitor visitor(texture, textureType, unit, resourceSystem);
            node.accept(visitor);
        }

        void overrideFalloutPartTexture(osg::Image* image, std::string_view textureType, unsigned int unit,
            Resource::ResourceSystem* resourceSystem, osg::Node& node)
        {
            if (image == nullptr)
                return;

            FalloutPartImageTextureOverrideVisitor visitor(image, textureType, unit, resourceSystem);
            node.accept(visitor);
        }

        void overrideFalloutPartDiffuseTexture(
            std::string_view texture, Resource::ResourceSystem* resourceSystem, osg::Node& node)
        {
            overrideFalloutPartTexture(texture, "diffuseMap", 0, resourceSystem, node);
        }

        void overrideFalloutPartDiffuseTexture(
            osg::Image* image, Resource::ResourceSystem* resourceSystem, osg::Node& node)
        {
            overrideFalloutPartTexture(image, "diffuseMap", 0, resourceSystem, node);
        }

        void overrideFalloutPartNormalTexture(
            std::string_view texture, Resource::ResourceSystem* resourceSystem, osg::Node& node)
        {
            overrideFalloutPartTexture(texture, "normalMap", 1, resourceSystem, node);
            resourceSystem->getSceneManager()->applyShaders(node);
        }

        void overrideFalloutPartSpecularTexture(
            std::string_view texture, Resource::ResourceSystem* resourceSystem, osg::Node& node)
        {
            overrideFalloutPartTexture(texture, "specularMap", 2, resourceSystem, node);
            resourceSystem->getSceneManager()->applyShaders(node);
        }

        void overrideFalloutPartDetailTexture(
            std::string_view texture, Resource::ResourceSystem* resourceSystem, osg::Node& node)
        {
            overrideFalloutPartTexture(texture, "detailMap", 4, resourceSystem, node);
            resourceSystem->getSceneManager()->applyShaders(node);
        }

        void overrideFalloutPartSkinTexture(
            std::string_view texture, Resource::ResourceSystem* resourceSystem, osg::Node& node)
        {
            overrideFalloutPartTexture(texture, "skinMap", 5, resourceSystem, node);
            resourceSystem->getSceneManager()->applyShaders(node);
        }

        bool isFonvHeadSurfacePart(std::size_t index)
        {
            return index == ESM4::Race::Head;
        }

        bool isFonvEyePart(std::size_t index)
        {
            // FNV RACE head parts are: 0 head, 1 ears, 2 mouth, 3 lower teeth, 4 upper teeth, 5 tongue,
            // 6 left eye, 7 right eye. TES4 has the eye slots shifted by one.
            return index == 6 || index == 7;
        }

        const char* getFonvRaceHeadPartRole(std::size_t index)
        {
            switch (index)
            {
                case 0:
                    return "head";
                case 1:
                    return "ears";
                case 2:
                    return "mouth";
                case 3:
                    return "lowerTeeth";
                case 4:
                    return "upperTeeth";
                case 5:
                    return "tongue";
                case 6:
                    return "leftEye";
                case 7:
                    return "rightEye";
                default:
                    return "extra";
            }
        }

        const char* getFonvFacePartStatus(bool attached, bool hasMesh)
        {
            if (attached)
                return "OK";
            return hasMesh ? "MISSING" : "IN_HEAD";
        }

        bool isFonvRaceSkinSurface(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("upperbody") != std::string::npos || lowered.find("lefthand") != std::string::npos
                || lowered.find("righthand") != std::string::npos || lowered.find("hand") != std::string::npos;
        }

        template <class T>
        bool readBinary(std::istream& stream, T& value)
        {
            stream.read(reinterpret_cast<char*>(&value), sizeof(T));
            return static_cast<bool>(stream);
        }

        struct FaceGenEgm
        {
            std::uint32_t mVertexCount = 0;
            std::vector<std::vector<osg::Vec3f>> mSymmetricModes;
            std::vector<std::vector<osg::Vec3f>> mAsymmetricModes;
        };

        struct FaceGenTri
        {
            std::uint32_t mVertexCount = 0;
            std::vector<osg::Vec3f> mBaseVertices;
            std::map<std::string, std::vector<osg::Vec3f>, std::less<>> mDiffMorphs;
            std::map<std::string, std::vector<std::pair<std::uint32_t, osg::Vec3f>>, std::less<>> mStaticMorphs;
        };

        struct FaceGenCtl
        {
            std::uint32_t mBasisVersion = 0;
            std::uint32_t mSymmetricShapeModeCount = 0;
            std::uint32_t mAsymmetricShapeModeCount = 0;
            std::uint32_t mTextureModeCount = 0;
            std::uint32_t mPayloadBytes = 0;
        };

        struct FaceGenEgt
        {
            std::uint32_t mWidth = 0;
            std::uint32_t mHeight = 0;
            std::uint32_t mSymmetricTextureModeCount = 0;
            std::uint32_t mAsymmetricTextureModeCount = 0;
            std::uint32_t mTextureModeCount = 0;
            std::uint32_t mBasisVersion = 0;
            std::vector<osg::Vec3f> mModeAverages;
            osg::Vec3f mMeanAverage{ 0.f, 0.f, 0.f };
        };

        bool readFaceGenString(std::istream& stream, std::string& value)
        {
            std::int32_t length = 0;
            if (!readBinary(stream, length) || length < 0)
                return false;

            value.resize(static_cast<std::size_t>(length));
            if (length > 0)
                stream.read(value.data(), length);
            while (!value.empty() && value.back() == '\0')
                value.pop_back();
            return static_cast<bool>(stream);
        }

        std::shared_ptr<const FaceGenCtl> loadFaceGenCtl(Resource::ResourceSystem* resourceSystem)
        {
            const VFS::Path::Normalized correctedPath("facegen/si.ctl");
            const std::string cacheKey = correctedPath.value();
            static std::map<std::string, std::shared_ptr<const FaceGenCtl>> sCache;
            if (const auto found = sCache.find(cacheKey); found != sCache.end())
                return found->second;

            const VFS::Manager* vfs = resourceSystem->getVFS();
            if (!vfs->exists(correctedPath))
            {
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            auto stream = vfs->get(correctedPath);
            char magic[8] = {};
            stream->read(magic, sizeof(magic));
            if (!*stream || std::string_view(magic, sizeof(magic)) != "FRCTL001")
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: unsupported FaceGen CTL " << cacheKey;
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            std::uint32_t controlSignature = 0;
            auto ctl = std::make_shared<FaceGenCtl>();
            if (!readBinary(*stream, controlSignature) || !readBinary(*stream, ctl->mBasisVersion)
                || !readBinary(*stream, ctl->mSymmetricShapeModeCount)
                || !readBinary(*stream, ctl->mAsymmetricShapeModeCount) || !readBinary(*stream, ctl->mTextureModeCount))
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: failed to read FaceGen CTL header " << cacheKey;
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            std::uint32_t reserved = 0;
            if (!readBinary(*stream, reserved))
                reserved = 0;

            stream->seekg(0, std::ios::end);
            const std::streamoff totalBytes = stream->tellg();
            if (totalBytes > 32)
                ctl->mPayloadBytes = static_cast<std::uint32_t>(std::min<std::streamoff>(totalBytes - 32,
                    static_cast<std::streamoff>(std::numeric_limits<std::uint32_t>::max())));

            if (ctl->mBasisVersion == 0 || ctl->mSymmetricShapeModeCount == 0
                || ctl->mAsymmetricShapeModeCount == 0 || ctl->mTextureModeCount == 0)
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: invalid FaceGen CTL counts " << cacheKey << " basis="
                                    << ctl->mBasisVersion << " symShape=" << ctl->mSymmetricShapeModeCount
                                    << " asymShape=" << ctl->mAsymmetricShapeModeCount
                                    << " texture=" << ctl->mTextureModeCount;
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            Log(Debug::Info) << "FNV/ESM4 diag: loaded FaceGen CTL " << cacheKey << " basis="
                             << ctl->mBasisVersion << " symShape=" << ctl->mSymmetricShapeModeCount
                             << " asymShape=" << ctl->mAsymmetricShapeModeCount
                             << " texture=" << ctl->mTextureModeCount << " payloadBytes=" << ctl->mPayloadBytes
                             << " signature=0x" << std::hex << controlSignature << std::dec;
            sCache.emplace(cacheKey, ctl);
            return ctl;
        }

        std::shared_ptr<const FaceGenEgm> loadFaceGenEgm(Resource::ResourceSystem* resourceSystem, std::string_view model)
        {
            std::string egmPath(model);
            const std::size_t dot = egmPath.find_last_of('.');
            if (dot == std::string::npos)
                return nullptr;
            egmPath.replace(dot, std::string::npos, ".egm");

            const VFS::Path::Normalized correctedPath = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(egmPath));
            const std::string cacheKey = correctedPath.value();
            static std::map<std::string, std::shared_ptr<const FaceGenEgm>> sCache;
            if (const auto found = sCache.find(cacheKey); found != sCache.end())
                return found->second;

            const VFS::Manager* vfs = resourceSystem->getVFS();
            if (!vfs->exists(correctedPath))
            {
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            auto stream = vfs->get(correctedPath);
            char magic[8] = {};
            stream->read(magic, sizeof(magic));
            if (!*stream || std::string_view(magic, sizeof(magic)) != "FREGM002")
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: unsupported FaceGen EGM " << cacheKey;
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            std::uint32_t vertexCount = 0;
            std::uint32_t symmetricCount = 0;
            std::uint32_t asymmetricCount = 0;
            std::uint32_t geometryBasisVersion = 0;
            if (!readBinary(*stream, vertexCount) || !readBinary(*stream, symmetricCount)
                || !readBinary(*stream, asymmetricCount) || !readBinary(*stream, geometryBasisVersion))
            {
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            stream->ignore(40);

            auto egm = std::make_shared<FaceGenEgm>();
            egm->mVertexCount = vertexCount;
            const auto readModes = [&](std::uint32_t count, std::vector<std::vector<osg::Vec3f>>& modes) {
                modes.resize(count);
                for (std::uint32_t mode = 0; mode < count; ++mode)
                {
                    float scale = 0.f;
                    if (!readBinary(*stream, scale))
                        return false;

                    modes[mode].resize(vertexCount);
                    for (std::uint32_t vertex = 0; vertex < vertexCount; ++vertex)
                    {
                        std::int16_t x = 0;
                        std::int16_t y = 0;
                        std::int16_t z = 0;
                        if (!readBinary(*stream, x) || !readBinary(*stream, y) || !readBinary(*stream, z))
                            return false;
                        modes[mode][vertex].set(x * scale, y * scale, z * scale);
                    }
                }
                return true;
            };

            if (!readModes(symmetricCount, egm->mSymmetricModes) || !readModes(asymmetricCount, egm->mAsymmetricModes))
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: failed to read FaceGen EGM " << cacheKey;
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            Log(Debug::Info) << "FNV/ESM4 diag: loaded FaceGen EGM " << cacheKey << " vertices=" << vertexCount
                             << " symmetric=" << symmetricCount << " asymmetric=" << asymmetricCount
                             << " basis=" << geometryBasisVersion;
            sCache.emplace(cacheKey, egm);
            return egm;
        }

        std::shared_ptr<const FaceGenTri> loadFaceGenTri(Resource::ResourceSystem* resourceSystem, std::string_view model)
        {
            std::string triPath(model);
            const std::size_t dot = triPath.find_last_of('.');
            if (dot == std::string::npos)
                return nullptr;
            triPath.replace(dot, std::string::npos, ".tri");

            const VFS::Path::Normalized correctedPath = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(triPath));
            const std::string cacheKey = correctedPath.value();
            static std::map<std::string, std::shared_ptr<const FaceGenTri>> sCache;
            if (const auto found = sCache.find(cacheKey); found != sCache.end())
                return found->second;

            const VFS::Manager* vfs = resourceSystem->getVFS();
            if (!vfs->exists(correctedPath))
            {
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            auto stream = vfs->get(correctedPath);
            char magic[8] = {};
            stream->read(magic, sizeof(magic));
            if (!*stream || std::string_view(magic, sizeof(magic)) != "FRTRI003")
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: unsupported FaceGen TRI " << cacheKey;
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            std::int32_t vertexCount = 0;
            std::int32_t triangleCount = 0;
            std::int32_t quadCount = 0;
            std::int32_t labelledVertexCount = 0;
            std::int32_t labelledSurfaceCount = 0;
            std::int32_t uvVertexCount = 0;
            std::int32_t extensionFlags = 0;
            std::int32_t diffMorphCount = 0;
            std::int32_t staticMorphCount = 0;
            std::int32_t addedVertexCount = 0;
            if (!readBinary(*stream, vertexCount) || !readBinary(*stream, triangleCount)
                || !readBinary(*stream, quadCount) || !readBinary(*stream, labelledVertexCount)
                || !readBinary(*stream, labelledSurfaceCount) || !readBinary(*stream, uvVertexCount)
                || !readBinary(*stream, extensionFlags) || !readBinary(*stream, diffMorphCount)
                || !readBinary(*stream, staticMorphCount) || !readBinary(*stream, addedVertexCount)
                || vertexCount < 0 || triangleCount < 0 || quadCount < 0 || labelledVertexCount < 0
                || labelledSurfaceCount < 0 || uvVertexCount < 0 || diffMorphCount < 0 || staticMorphCount < 0
                || addedVertexCount < 0)
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: failed to read FaceGen TRI header " << cacheKey;
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            stream->ignore(16);
            std::vector<osg::Vec3f> triVertices(static_cast<std::size_t>(vertexCount + addedVertexCount));
            for (osg::Vec3f& vertex : triVertices)
            {
                float x = 0.f;
                float y = 0.f;
                float z = 0.f;
                if (!readBinary(*stream, x) || !readBinary(*stream, y) || !readBinary(*stream, z))
                    break;
                vertex.set(x, y, z);
            }
            stream->ignore(static_cast<std::streamsize>(triangleCount * 12 + quadCount * 16));

            for (std::int32_t i = 0; i < labelledVertexCount; ++i)
            {
                stream->ignore(4);
                std::string ignored;
                if (!readFaceGenString(*stream, ignored))
                    break;
            }
            for (std::int32_t i = 0; i < labelledSurfaceCount; ++i)
            {
                stream->ignore(16);
                std::string ignored;
                if (!readFaceGenString(*stream, ignored))
                    break;
            }

            if ((extensionFlags & 1) != 0)
            {
                if (uvVertexCount == 0)
                    stream->ignore(static_cast<std::streamsize>(vertexCount * 8));
                else
                {
                    stream->ignore(static_cast<std::streamsize>(uvVertexCount * 8));
                    stream->ignore(static_cast<std::streamsize>(triangleCount * 12 + quadCount * 16));
                }
            }

            auto tri = std::make_shared<FaceGenTri>();
            tri->mVertexCount = static_cast<std::uint32_t>(vertexCount);
            tri->mBaseVertices.assign(triVertices.begin(), triVertices.begin() + vertexCount);
            for (std::int32_t morph = 0; morph < diffMorphCount; ++morph)
            {
                std::string name;
                float scale = 0.f;
                if (!readFaceGenString(*stream, name) || !readBinary(*stream, scale))
                    break;

                std::vector<osg::Vec3f> deltas(static_cast<std::size_t>(vertexCount));
                for (std::int32_t vertex = 0; vertex < vertexCount; ++vertex)
                {
                    std::int16_t x = 0;
                    std::int16_t y = 0;
                    std::int16_t z = 0;
                    if (!readBinary(*stream, x) || !readBinary(*stream, y) || !readBinary(*stream, z))
                        break;
                    deltas[static_cast<std::size_t>(vertex)].set(x * scale, y * scale, z * scale);
                }
                tri->mDiffMorphs.emplace(std::move(name), std::move(deltas));
            }

            std::size_t addedVertexOffset = static_cast<std::size_t>(vertexCount);
            for (std::int32_t morph = 0; morph < staticMorphCount; ++morph)
            {
                std::string name;
                std::int32_t indexCount = 0;
                if (!readFaceGenString(*stream, name) || !readBinary(*stream, indexCount) || indexCount < 0)
                    break;

                std::vector<std::pair<std::uint32_t, osg::Vec3f>> replacements;
                replacements.reserve(static_cast<std::size_t>(indexCount));
                for (std::int32_t i = 0; i < indexCount; ++i)
                {
                    std::int32_t vertexIndex = 0;
                    if (!readBinary(*stream, vertexIndex))
                        break;
                    const std::size_t replacementIndex = addedVertexOffset + static_cast<std::size_t>(i);
                    if (vertexIndex >= 0 && vertexIndex < vertexCount && replacementIndex < triVertices.size())
                        replacements.emplace_back(static_cast<std::uint32_t>(vertexIndex), triVertices[replacementIndex]);
                }
                addedVertexOffset += static_cast<std::size_t>(indexCount);
                if (!replacements.empty())
                    tri->mStaticMorphs.emplace(std::move(name), std::move(replacements));
            }

            Log(Debug::Info) << "FNV/ESM4 diag: loaded FaceGen TRI " << cacheKey << " vertices=" << vertexCount
                             << " diffMorphs=" << tri->mDiffMorphs.size() << " staticMorphs="
                             << tri->mStaticMorphs.size();
            sCache.emplace(cacheKey, tri);
            return tri;
        }

        std::shared_ptr<const FaceGenEgt> loadFaceGenEgt(Resource::ResourceSystem* resourceSystem, std::string_view model)
        {
            std::string egtPath(model);
            const std::size_t dot = egtPath.find_last_of('.');
            if (dot == std::string::npos)
                return nullptr;
            egtPath.replace(dot, std::string::npos, ".egt");

            const VFS::Path::Normalized correctedPath = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(egtPath));
            const std::string cacheKey = correctedPath.value();
            static std::map<std::string, std::shared_ptr<const FaceGenEgt>> sCache;
            if (const auto found = sCache.find(cacheKey); found != sCache.end())
                return found->second;

            const VFS::Manager* vfs = resourceSystem->getVFS();
            if (!vfs->exists(correctedPath))
            {
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            auto stream = vfs->get(correctedPath);
            char magic[8] = {};
            stream->read(magic, sizeof(magic));
            if (!*stream || std::string_view(magic, sizeof(magic)) != "FREGT003")
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: unsupported FaceGen EGT " << cacheKey;
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            constexpr std::uint32_t headerSize = 64;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t symmetricTextureModeCount = 0;
            std::uint32_t asymmetricTextureModeCount = 0;
            std::uint32_t basisVersion = 0;
            if (!readBinary(*stream, width) || !readBinary(*stream, height)
                || !readBinary(*stream, symmetricTextureModeCount) || !readBinary(*stream, asymmetricTextureModeCount)
                || !readBinary(*stream, basisVersion) || width == 0 || height == 0 || width > 4096
                || height > 4096 || symmetricTextureModeCount + asymmetricTextureModeCount == 0
                || symmetricTextureModeCount + asymmetricTextureModeCount > 256)
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: failed to read FaceGen EGT header " << cacheKey;
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            stream->ignore(headerSize - 28);
            if (!*stream)
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: truncated FaceGen EGT header " << cacheKey;
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            const std::uint32_t textureModeCount = symmetricTextureModeCount + asymmetricTextureModeCount;
            const std::uint64_t pixelCount64 = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
            const std::uint64_t modeByteCount64 = sizeof(float) + pixelCount64 * 3u;
            const std::uint64_t payloadByteCount64 = modeByteCount64 * textureModeCount;
            if (pixelCount64 == 0 || modeByteCount64 > 64u * 1024u * 1024u
                || payloadByteCount64 > 512u * 1024u * 1024u)
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: rejected oversized FaceGen EGT " << cacheKey << " size="
                                    << width << "x" << height << " modes=" << textureModeCount;
                sCache.emplace(cacheKey, nullptr);
                return nullptr;
            }

            const std::size_t modeByteCount = static_cast<std::size_t>(modeByteCount64);
            const std::size_t pixelCount = static_cast<std::size_t>(pixelCount64);
            std::vector<std::int8_t> pixels(pixelCount);
            auto egt = std::make_shared<FaceGenEgt>();
            egt->mWidth = width;
            egt->mHeight = height;
            egt->mSymmetricTextureModeCount = symmetricTextureModeCount;
            egt->mAsymmetricTextureModeCount = asymmetricTextureModeCount;
            egt->mTextureModeCount = textureModeCount;
            egt->mBasisVersion = basisVersion;
            egt->mModeAverages.reserve(textureModeCount);

            for (std::uint32_t mode = 0; mode < textureModeCount; ++mode)
            {
                float modeScale = 0.f;
                if (!readBinary(*stream, modeScale))
                {
                    Log(Debug::Warning) << "FNV/ESM4 diag: truncated FaceGen EGT payload " << cacheKey
                                        << " mode=" << mode << "/" << textureModeCount;
                    sCache.emplace(cacheKey, nullptr);
                    return nullptr;
                }

                double channelAverage[3] = {};
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    stream->read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
                    if (!*stream)
                    {
                        Log(Debug::Warning) << "FNV/ESM4 diag: truncated FaceGen EGT payload " << cacheKey
                                            << " mode=" << mode << "/" << textureModeCount << " channel=" << channel;
                        sCache.emplace(cacheKey, nullptr);
                        return nullptr;
                    }

                    double total = 0.0;
                    for (const std::int8_t pixel : pixels)
                        total += static_cast<double>(pixel) * static_cast<double>(modeScale);
                    channelAverage[channel] = total / (static_cast<double>(pixelCount64) * 255.0);
                }

                const osg::Vec3f average(static_cast<float>(channelAverage[0]), static_cast<float>(channelAverage[1]),
                    static_cast<float>(channelAverage[2]));
                egt->mModeAverages.push_back(average);
                egt->mMeanAverage += average;
            }

            egt->mMeanAverage /= static_cast<float>(egt->mModeAverages.size());

            Log(Debug::Info) << "FNV/ESM4 diag: loaded FaceGen EGT " << cacheKey << " size=" << width << "x"
                             << height << " modes=" << textureModeCount << " sym=" << symmetricTextureModeCount
                             << " asym=" << asymmetricTextureModeCount << " basis=" << basisVersion
                             << " headerBytes=" << headerSize << " perModeBytes=" << modeByteCount
                             << " payloadBytes=" << payloadByteCount64
                             << " mean=(" << egt->mMeanAverage.x() << ", " << egt->mMeanAverage.y() << ", "
                             << egt->mMeanAverage.z() << ")";
            sCache.emplace(cacheKey, egt);
            return egt;
        }

        osg::Vec4f deriveFaceGenEgtMaterialTint(const FaceGenEgt& egt, const ESM4::Npc& traits)
        {
            osg::Vec3f tint(1.f, 1.f, 1.f);
            osg::Vec3f textureDelta(0.f, 0.f, 0.f);
            float totalWeight = 0.f;
            const std::size_t modeCount = std::min(egt.mModeAverages.size(), traits.mSymTextureModeCoefficients.size());
            for (std::size_t mode = 0; mode < modeCount; ++mode)
            {
                const float coefficient = traits.mSymTextureModeCoefficients[mode];
                if (std::abs(coefficient) <= 0.0001f)
                    continue;

                textureDelta += egt.mModeAverages[mode] * coefficient;
                totalWeight += std::abs(coefficient);
            }

            if (totalWeight > 0.0001f)
                tint += textureDelta;

            for (const ESM4::Npc::TintLayer& layer : traits.mTintLayers)
            {
                if (!layer.hasColor)
                    continue;

                const float value = std::clamp(layer.hasValue ? layer.value : 1.f, 0.f, 1.f);
                if (value <= 0.0001f)
                    continue;

                const osg::Vec3f color(layer.color.red / 255.f, layer.color.green / 255.f, layer.color.blue / 255.f);
                tint += (color - tint) * (value * 0.12f);
            }

            return osg::Vec4f(std::clamp(tint.x(), 0.45f, 1.55f), std::clamp(tint.y(), 0.45f, 1.55f),
                std::clamp(tint.z(), 0.45f, 1.55f), 1.f);
        }

        osg::ref_ptr<osg::Geometry> makeFaceGenMorphedGeometry(const osg::Geometry& source, const FaceGenEgm& egm,
            const std::vector<float>& symmetricCoefficients, const std::vector<float>& asymmetricCoefficients,
            float& maxDelta)
        {
            const osg::Vec3Array* sourceVertices = dynamic_cast<const osg::Vec3Array*>(source.getVertexArray());
            if (sourceVertices == nullptr || sourceVertices->size() > egm.mVertexCount)
                return nullptr;

            osg::ref_ptr<osg::Geometry> morphed = new osg::Geometry(source, osg::CopyOp::SHALLOW_COPY);
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array(*sourceVertices);
            morphed->setVertexArray(vertices);

            maxDelta = 0.f;
            const auto applyModes = [&](const std::vector<std::vector<osg::Vec3f>>& modes,
                                        const std::vector<float>& coefficients) {
                const std::size_t count = std::min(modes.size(), coefficients.size());
                for (std::size_t mode = 0; mode < count; ++mode)
                {
                    const float coefficient = coefficients[mode];
                    if (std::abs(coefficient) <= 0.0001f)
                        continue;

                    for (std::size_t vertex = 0; vertex < vertices->size(); ++vertex)
                    {
                        const osg::Vec3f delta = modes[mode][vertex] * coefficient;
                        (*vertices)[vertex] += delta;
                        maxDelta = std::max(maxDelta, delta.length());
                    }
                }
            };

            applyModes(egm.mSymmetricModes, symmetricCoefficients);
            applyModes(egm.mAsymmetricModes, asymmetricCoefficients);
            vertices->dirty();
            return morphed;
        }

        osg::ref_ptr<osg::Geometry> makeFaceGenTriMorphedGeometry(
            const osg::Geometry& source, const FaceGenTri& tri, std::string_view morphName, float weight, float& maxDelta)
        {
            const osg::Vec3Array* sourceVertices = dynamic_cast<const osg::Vec3Array*>(source.getVertexArray());
            if (sourceVertices == nullptr || sourceVertices->size() > tri.mVertexCount)
                return nullptr;

            const auto found = tri.mDiffMorphs.find(morphName);
            if (found == tri.mDiffMorphs.end())
                return nullptr;

            osg::ref_ptr<osg::Geometry> morphed = new osg::Geometry(source, osg::CopyOp::SHALLOW_COPY);
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array(*sourceVertices);
            morphed->setVertexArray(vertices);

            maxDelta = 0.f;
            const std::vector<osg::Vec3f>& deltas = found->second;
            for (std::size_t vertex = 0; vertex < vertices->size(); ++vertex)
            {
                const osg::Vec3f delta = deltas[vertex] * weight;
                (*vertices)[vertex] += delta;
                maxDelta = std::max(maxDelta, delta.length());
            }

            vertices->dirty();
            return morphed;
        }

        osg::ref_ptr<osg::Geometry> makeFaceGenTriStaticMorphedGeometry(
            const osg::Geometry& source, const FaceGenTri& tri, std::string_view morphName, float weight, float& maxDelta)
        {
            const osg::Vec3Array* sourceVertices = dynamic_cast<const osg::Vec3Array*>(source.getVertexArray());
            if (sourceVertices == nullptr || sourceVertices->size() > tri.mVertexCount)
                return nullptr;

            const auto found = tri.mStaticMorphs.find(morphName);
            if (found == tri.mStaticMorphs.end())
                return nullptr;

            osg::ref_ptr<osg::Geometry> morphed = new osg::Geometry(source, osg::CopyOp::SHALLOW_COPY);
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array(*sourceVertices);
            morphed->setVertexArray(vertices);

            maxDelta = 0.f;
            for (const auto& [vertexIndex, target] : found->second)
            {
                if (vertexIndex >= vertices->size() || vertexIndex >= tri.mBaseVertices.size())
                    continue;
                osg::Vec3f& vertex = (*vertices)[vertexIndex];
                const osg::Vec3f delta = (target - tri.mBaseVertices[vertexIndex]) * weight;
                vertex += delta;
                maxDelta = std::max(maxDelta, delta.length());
            }

            vertices->dirty();
            return morphed;
        }

        osg::ref_ptr<osg::Geometry> makeFalloutProofTalkingHeadGeometry(const osg::Geometry& source, float& maxDelta)
        {
            const osg::Vec3Array* sourceVertices = dynamic_cast<const osg::Vec3Array*>(source.getVertexArray());
            if (sourceVertices == nullptr)
                return nullptr;

            osg::ref_ptr<osg::Geometry> morphed = new osg::Geometry(source, osg::CopyOp::SHALLOW_COPY);
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array(*sourceVertices);
            morphed->setVertexArray(vertices);

            osg::BoundingBox bounds;
            for (const osg::Vec3f& vertex : *vertices)
                bounds.expandBy(vertex);
            if (!bounds.valid())
                return nullptr;

            const float centerX = (bounds.xMin() + bounds.xMax()) * 0.5f;
            const float width = std::max(bounds.xMax() - bounds.xMin(), 0.001f);
            const float depth = std::max(bounds.yMax() - bounds.yMin(), 0.001f);
            const float height = std::max(bounds.zMax() - bounds.zMin(), 0.001f);
            const float mouthZMin = bounds.zMin() + height * 0.28f;
            const float mouthZMax = bounds.zMin() + height * 0.40f;
            const float chinZMin = bounds.zMin() + height * 0.20f;
            const float chinZMax = bounds.zMin() + height * 0.30f;
            const float frontY = bounds.yMin() + depth * 0.35f;
            const float noseTipY = bounds.yMin() + depth * 0.82f;

            maxDelta = 0.f;
            unsigned int lowerLipCount = 0;
            unsigned int chinCount = 0;
            for (osg::Vec3f& vertex : *vertices)
            {
                const float absX = std::abs(vertex.x() - centerX);
                const bool lowerLip = absX < width * 0.24f && vertex.y() > frontY && vertex.y() < noseTipY
                    && vertex.z() > mouthZMin && vertex.z() < mouthZMax;
                const bool chin = absX < width * 0.30f && vertex.y() > frontY && vertex.y() < noseTipY
                    && vertex.z() > chinZMin && vertex.z() <= chinZMax;
                if (!lowerLip && !chin)
                    continue;

                const osg::Vec3f before = vertex;
                if (lowerLip)
                {
                    ++lowerLipCount;
                    vertex.y() += depth * 0.04f;
                    vertex.z() -= height * 0.055f;
                }
                else
                {
                    ++chinCount;
                    vertex.y() += depth * 0.02f;
                    vertex.z() -= height * 0.025f;
                }
                maxDelta = std::max(maxDelta, (vertex - before).length());
            }

            static unsigned int sLogCount = 0;
            if (sLogCount < 12)
            {
                Log(Debug::Info) << "FNV/ESM4 proof: talking head source bounds x=(" << bounds.xMin() << ","
                                 << bounds.xMax() << ") y=(" << bounds.yMin() << "," << bounds.yMax()
                                 << ") z=(" << bounds.zMin() << "," << bounds.zMax() << ") selected lowerLip="
                                 << lowerLipCount << " chin=" << chinCount << " maxDelta=" << maxDelta;
                ++sLogCount;
            }

            if (maxDelta <= 0.f)
                return nullptr;

            vertices->dirty();
            return morphed;
        }

        class FaceGenTriMorphVisitor : public osg::NodeVisitor
        {
        public:
            FaceGenTriMorphVisitor(const FaceGenTri& tri, std::string_view morphName, float weight, bool staticMorph = false)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mTri(tri)
                , mMorphName(morphName)
                , mWeight(weight)
                , mStaticMorph(staticMorph)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    applyDrawable(*geode.getDrawable(i));
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

            unsigned int getMorphedDrawableCount() const { return mMorphedDrawableCount; }
            float getMaxDelta() const { return mMaxDelta; }

        private:
            void applyDrawable(osg::Drawable& drawable)
            {
                float maxDelta = 0.f;
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (rig->getSourceGeometry() == nullptr)
                        return;
                    osg::ref_ptr<osg::Geometry> morphed
                        = mStaticMorph
                        ? makeFaceGenTriStaticMorphedGeometry(
                            *rig->getSourceGeometry(), mTri, mMorphName, mWeight, maxDelta)
                        : makeFaceGenTriMorphedGeometry(*rig->getSourceGeometry(), mTri, mMorphName, mWeight, maxDelta);
                    if (morphed == nullptr)
                        return;
                    rig->setSourceGeometry(morphed);
                }
                else if (osg::Geometry* geometry = dynamic_cast<osg::Geometry*>(&drawable))
                {
                    osg::ref_ptr<osg::Geometry> morphed
                        = mStaticMorph
                        ? makeFaceGenTriStaticMorphedGeometry(*geometry, mTri, mMorphName, mWeight, maxDelta)
                        : makeFaceGenTriMorphedGeometry(*geometry, mTri, mMorphName, mWeight, maxDelta);
                    if (morphed == nullptr)
                        return;
                    geodeReplace(*geometry, morphed);
                }
                else
                    return;

                ++mMorphedDrawableCount;
                mMaxDelta = std::max(mMaxDelta, maxDelta);
            }

            void geodeReplace(osg::Geometry& geometry, osg::ref_ptr<osg::Geometry> morphed)
            {
                for (unsigned int parentIndex = 0; parentIndex < geometry.getNumParents(); ++parentIndex)
                {
                    osg::Geode* parent = dynamic_cast<osg::Geode*>(geometry.getParent(parentIndex));
                    if (parent == nullptr)
                        continue;
                    const unsigned int drawableIndex = parent->getDrawableIndex(&geometry);
                    if (drawableIndex < parent->getNumDrawables())
                        parent->setDrawable(drawableIndex, morphed);
                }
            }

            const FaceGenTri& mTri;
            std::string mMorphName;
            float mWeight;
            bool mStaticMorph = false;
            unsigned int mMorphedDrawableCount = 0;
            float mMaxDelta = 0.f;
        };

        class FalloutProofTriMorphDriver : public osg::NodeCallback
        {
        public:
            FalloutProofTriMorphDriver(const MWWorld::Ptr& actor, const FaceGenTri& tri, std::string_view morphName,
                std::string model)
                : mActor(actor)
                , mMorphName(morphName)
                , mModel(std::move(model))
            {
                const auto found = tri.mDiffMorphs.find(mMorphName);
                if (found != tri.mDiffMorphs.end())
                    mDeltas = found->second;
            }

            void addRig(SceneUtil::RigGeometry& rig)
            {
                osg::Geometry* source = rig.getSourceGeometry();
                if (source == nullptr)
                    return;
                const osg::Vec3Array* vertices = dynamic_cast<const osg::Vec3Array*>(source->getVertexArray());
                if (vertices == nullptr || vertices->size() > mDeltas.size())
                    return;

                Target target;
                target.mRig = &rig;
                target.mTemplateGeometry = source;
                target.mBaseVertices = new osg::Vec3Array(*vertices);
                mTargets.push_back(std::move(target));
            }

            void addGeometry(osg::Geometry& geometry)
            {
                const osg::Vec3Array* vertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
                if (vertices == nullptr || vertices->size() > mDeltas.size())
                    return;

                Target target;
                target.mGeometry = &geometry;
                target.mBaseVertices = new osg::Vec3Array(*vertices);
                mTargets.push_back(std::move(target));
            }

            bool empty() const { return mTargets.empty() || mDeltas.empty(); }

            void operator()(osg::Node* node, osg::NodeVisitor* nv) override
            {
                const bool forceOpen = std::getenv("OPENMW_FNV_PROOF_MOUTH_FORCE_OPEN") != nullptr;
                float open = 0.f;
                if (forceOpen)
                    open = 1.f;
                else if (MWBase::Environment::get().getSoundManager()->sayActive(mActor))
                {
                    const float lip = MWBase::Environment::get().getSoundManager()->getSaySoundLipValue(mActor);
                    const float loudness = MWBase::Environment::get().getSoundManager()->getSaySoundLoudness(mActor);
                    open = std::clamp(std::max(lip, loudness * 5.0f), 0.f, 0.65f);
                }

                if (open != mLastOpen)
                {
                    for (Target& target : mTargets)
                        applyTarget(target, open);
                    mLastOpen = open;
                }

                if (!mLogged && open > 0.f)
                {
                    Log(Debug::Info) << "FNV/ESM4 proof: TRI morph driver active for " << mActor.toString()
                                     << " model=" << mModel << " morph=" << mMorphName << " targets="
                                     << mTargets.size() << " open=" << open << " force=" << forceOpen;
                    mLogged = true;
                }

                traverse(node, nv);
            }

        private:
            struct Target
            {
                osg::ref_ptr<SceneUtil::RigGeometry> mRig;
                osg::ref_ptr<osg::Geometry> mGeometry;
                osg::ref_ptr<osg::Geometry> mTemplateGeometry;
                osg::ref_ptr<osg::Vec3Array> mBaseVertices;
            };

            void applyTarget(Target& target, float open)
            {
                if (target.mBaseVertices == nullptr)
                    return;

                osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array(*target.mBaseVertices);
                for (std::size_t i = 0; i < vertices->size(); ++i)
                    (*vertices)[i] += mDeltas[i] * open;
                vertices->dirty();

                if (target.mRig != nullptr && target.mTemplateGeometry != nullptr)
                {
                    osg::ref_ptr<osg::Geometry> morphed
                        = new osg::Geometry(*target.mTemplateGeometry, osg::CopyOp::SHALLOW_COPY);
                    morphed->setVertexArray(vertices);
                    target.mRig->setSourceGeometry(morphed);
                }
                else if (target.mGeometry != nullptr)
                {
                    target.mGeometry->setVertexArray(vertices);
                    target.mGeometry->dirtyDisplayList();
                    target.mGeometry->dirtyBound();
                }
            }

            MWWorld::Ptr mActor;
            std::string mMorphName;
            std::string mModel;
            std::vector<osg::Vec3f> mDeltas;
            std::vector<Target> mTargets;
            float mLastOpen = -1.f;
            bool mLogged = false;
        };

        class FalloutProofTriMorphTargetVisitor : public osg::NodeVisitor
        {
        public:
            FalloutProofTriMorphTargetVisitor(FalloutProofTriMorphDriver& driver)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mDriver(driver)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    applyDrawable(*geode.getDrawable(i));
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

        private:
            void applyDrawable(osg::Drawable& drawable)
            {
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                    mDriver.addRig(*rig);
                else if (osg::Geometry* geometry = dynamic_cast<osg::Geometry*>(&drawable))
                    mDriver.addGeometry(*geometry);
            }

            FalloutProofTriMorphDriver& mDriver;
        };

        class FalloutProofTalkingHeadVisitor : public osg::NodeVisitor
        {
        public:
            FalloutProofTalkingHeadVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    applyDrawable(*geode.getDrawable(i));
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

            unsigned int getMorphedDrawableCount() const { return mMorphedDrawableCount; }
            float getMaxDelta() const { return mMaxDelta; }

        private:
            void applyDrawable(osg::Drawable& drawable)
            {
                float maxDelta = 0.f;
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (rig->getSourceGeometry() == nullptr)
                        return;
                    osg::ref_ptr<osg::Geometry> morphed
                        = makeFalloutProofTalkingHeadGeometry(*rig->getSourceGeometry(), maxDelta);
                    if (morphed == nullptr)
                        return;
                    rig->setSourceGeometry(morphed);
                }
                else if (osg::Geometry* geometry = dynamic_cast<osg::Geometry*>(&drawable))
                {
                    osg::ref_ptr<osg::Geometry> morphed = makeFalloutProofTalkingHeadGeometry(*geometry, maxDelta);
                    if (morphed == nullptr)
                        return;
                    geodeReplace(*geometry, morphed);
                }
                else
                    return;

                ++mMorphedDrawableCount;
                mMaxDelta = std::max(mMaxDelta, maxDelta);
            }

            void geodeReplace(osg::Geometry& geometry, osg::ref_ptr<osg::Geometry> morphed)
            {
                for (unsigned int parentIndex = 0; parentIndex < geometry.getNumParents(); ++parentIndex)
                {
                    osg::Geode* parent = dynamic_cast<osg::Geode*>(geometry.getParent(parentIndex));
                    if (parent == nullptr)
                        continue;
                    const unsigned int drawableIndex = parent->getDrawableIndex(&geometry);
                    if (drawableIndex < parent->getNumDrawables())
                        parent->setDrawable(drawableIndex, morphed);
                }
            }

            unsigned int mMorphedDrawableCount = 0;
            float mMaxDelta = 0.f;
        };

        class FaceGenMorphVisitor : public osg::NodeVisitor
        {
        public:
            FaceGenMorphVisitor(const FaceGenEgm& egm, const ESM4::Npc& traits)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mEgm(egm)
                , mTraits(traits)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    applyDrawable(*geode.getDrawable(i));
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

            unsigned int getMorphedDrawableCount() const { return mMorphedDrawableCount; }
            unsigned int getCandidateDrawableCount() const { return mCandidateDrawableCount; }
            std::size_t getFirstVertexCount() const { return mFirstVertexCount; }
            float getMaxDelta() const { return mMaxDelta; }

        private:
            void applyDrawable(osg::Drawable& drawable)
            {
                float maxDelta = 0.f;
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    ++mCandidateDrawableCount;
                    if (const osg::Geometry* source = rig->getSourceGeometry())
                    {
                        if (const osg::Vec3Array* vertices = dynamic_cast<const osg::Vec3Array*>(source->getVertexArray()))
                            if (mFirstVertexCount == 0)
                                mFirstVertexCount = vertices->size();
                    }
                    osg::ref_ptr<osg::Geometry> morphed = makeFaceGenMorphedGeometry(*rig->getSourceGeometry(), mEgm,
                        mTraits.mSymShapeModeCoefficients, mTraits.mAsymShapeModeCoefficients, maxDelta);
                    if (morphed == nullptr)
                        return;

                    rig->setSourceGeometry(morphed);
                }
                else if (osg::Geometry* geometry = dynamic_cast<osg::Geometry*>(&drawable))
                {
                    ++mCandidateDrawableCount;
                    if (const osg::Vec3Array* vertices = dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray()))
                        if (mFirstVertexCount == 0)
                            mFirstVertexCount = vertices->size();
                    osg::ref_ptr<osg::Geometry> morphed = makeFaceGenMorphedGeometry(
                        *geometry, mEgm, mTraits.mSymShapeModeCoefficients, mTraits.mAsymShapeModeCoefficients, maxDelta);
                    if (morphed == nullptr)
                        return;

                    geodeReplace(*geometry, morphed);
                }
                else
                    return;

                ++mMorphedDrawableCount;
                mMaxDelta = std::max(mMaxDelta, maxDelta);
            }

            void geodeReplace(osg::Geometry& geometry, osg::ref_ptr<osg::Geometry> morphed)
            {
                for (unsigned int parentIndex = 0; parentIndex < geometry.getNumParents(); ++parentIndex)
                {
                    osg::Geode* parent = dynamic_cast<osg::Geode*>(geometry.getParent(parentIndex));
                    if (parent == nullptr)
                        continue;
                    const unsigned int drawableIndex = parent->getDrawableIndex(&geometry);
                    if (drawableIndex < parent->getNumDrawables())
                        parent->setDrawable(drawableIndex, morphed);
                }
            }

            const FaceGenEgm& mEgm;
            const ESM4::Npc& mTraits;
            unsigned int mCandidateDrawableCount = 0;
            unsigned int mMorphedDrawableCount = 0;
            std::size_t mFirstVertexCount = 0;
            float mMaxDelta = 0.f;
        };

        class FalloutPartShapeSummaryVisitor : public osg::NodeVisitor
        {
        public:
            FalloutPartShapeSummaryVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                {
                    osg::Drawable* drawable = geode.getDrawable(i);
                    if (drawable != nullptr)
                        summarizeDrawable(*drawable);
                }

                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override
            {
                summarizeDrawable(drawable);
            }

            void summarizeDrawable(osg::Drawable& drawable)
            {
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    ++mRigGeometryCount;
                    drawable.setCullingActive(false);
                    if (mFirstRigRootBone.empty())
                    {
                        mFirstRigRootBone = rig->getRootBone();
                        mFirstRigBoneCount = rig->getBoneCount();
                        const std::size_t sampleCount = std::min<std::size_t>(mFirstRigBoneCount, 4);
                        for (std::size_t i = 0; i < sampleCount; ++i)
                        {
                            if (i != 0)
                                mFirstRigBoneSample << ",";
                            mFirstRigBoneSample << rig->getBoneName(i);
                        }
                    }
                }
                else if (dynamic_cast<osg::Geometry*>(&drawable) != nullptr)
                    ++mStaticGeometryCount;
            }

            unsigned int mRigGeometryCount = 0;
            unsigned int mStaticGeometryCount = 0;
            std::string mFirstRigRootBone;
            std::size_t mFirstRigBoneCount = 0;
            std::ostringstream mFirstRigBoneSample;
        };

        class FalloutVisibleGeometryVisitor : public osg::NodeVisitor
        {
        public:
            FalloutVisibleGeometryVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        summarizeDrawable(*drawable);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { summarizeDrawable(drawable); }

            void summarizeDrawable(osg::Drawable& drawable)
            {
                if (drawable.getNodeMask() == 0)
                    return;

                const auto vertexCount = [](const osg::Geometry* geometry) -> unsigned int {
                    if (geometry == nullptr || geometry->getVertexArray() == nullptr)
                        return 0;
                    return geometry->getVertexArray()->getNumElements();
                };

                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    const unsigned int sourceVertices = vertexCount(rig->getSourceGeometry());
                    unsigned int renderVertices = 0;
                    for (unsigned int i = 0; i < 2; ++i)
                        renderVertices = std::max(renderVertices, vertexCount(rig->getRenderGeometry(i)));
                    const unsigned int vertices = std::max(sourceVertices, renderVertices);
                    if (vertices == 0)
                        return;
                    ++mVisibleGeometryCount;
                    mVisibleVertexCount += vertices;
                }
                else if (const osg::Geometry* geometry = dynamic_cast<const osg::Geometry*>(&drawable))
                {
                    const unsigned int vertices = vertexCount(geometry);
                    if (vertices == 0)
                        return;
                    ++mVisibleGeometryCount;
                    mVisibleVertexCount += vertices;
                }
            }

            unsigned int mVisibleGeometryCount = 0;
            unsigned int mVisibleVertexCount = 0;
        };

        FalloutVisibleGeometryVisitor countFalloutVisibleGeometry(osg::Node* node)
        {
            FalloutVisibleGeometryVisitor visitor;
            if (node != nullptr)
                node->accept(visitor);
            return visitor;
        }

        class MarkFalloutRigGeometryVisitor : public osg::NodeVisitor
        {
        public:
            MarkFalloutRigGeometryVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(geode.getDrawable(i)))
                    {
                        ++mMarked;
                    }

                traverse(geode);
            }

            unsigned int mMarked = 0;
        };

        class StaticizeFalloutRiggedGeometryVisitor : public osg::NodeVisitor
        {
        public:
            StaticizeFalloutRiggedGeometryVisitor(std::string actor = {}, std::string model = {})
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mActor(std::move(actor))
                , mModel(std::move(model))
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                {
                    osg::ref_ptr<osg::Geometry> staticGeometry = makeStaticGeometry(*geode.getDrawable(i));
                    if (staticGeometry == nullptr)
                        continue;

                    geode.setDrawable(i, staticGeometry);
                    ++mStaticizedRigGeometryCount;
                }

                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override
            {
                osg::ref_ptr<osg::Geometry> staticGeometry = makeStaticGeometry(drawable);
                if (staticGeometry == nullptr)
                    return;

                bool replaced = false;
                while (drawable.getNumParents() > 0)
                {
                    osg::Group* parent = drawable.getParent(0);
                    if (osg::Geode* geode = dynamic_cast<osg::Geode*>(parent))
                    {
                        if (!geode->replaceDrawable(&drawable, staticGeometry.get()))
                            break;
                        replaced = true;
                        continue;
                    }

                    osg::Node* drawableNode = dynamic_cast<osg::Node*>(&drawable);
                    if (parent == nullptr || drawableNode == nullptr)
                        break;

                    osg::ref_ptr<osg::Geode> staticGeode = new osg::Geode;
                    staticGeode->setName(drawable.getName().empty() ? std::string("FNV Staticized Rig Drawable")
                                                                     : "FNV Staticized " + drawable.getName());
                    staticGeode->addDrawable(staticGeometry.get());
                    if (!parent->replaceChild(drawableNode, staticGeode.get()))
                        break;
                    replaced = true;
                }
                if (replaced)
                    ++mStaticizedRigGeometryCount;
                else
                    Log(Debug::Warning) << "FNV/ESM4 diag: staticize could not replace direct rig drawable name="
                                        << drawable.getName() << " parents=" << drawable.getNumParents();
            }

            osg::ref_ptr<osg::Geometry> makeStaticGeometry(osg::Drawable& drawable)
            {
                osg::Geometry* source = nullptr;
                std::string sourceName;
                std::string rootBone;
                std::size_t boneCount = 0;
                const char* kind = nullptr;
                bool hasFingerWeights = false;
                std::array<std::vector<float>, 15> fingerBoneWeights;
                auto vertexCount = [](const osg::Geometry* geometry) -> unsigned int {
                    if (geometry == nullptr || geometry->getVertexArray() == nullptr)
                        return 0;
                    return geometry->getVertexArray()->getNumElements();
                };
                auto weightedSlotCount = [](const std::array<std::vector<float>, 15>& weights) -> unsigned int {
                    unsigned int count = 0;
                    for (const auto& slot : weights)
                    {
                        if (std::any_of(slot.begin(), slot.end(), [](float weight) { return weight > 0.f; }))
                            ++count;
                    }
                    return count;
                };
                auto weightedVertexCount = [](const std::array<std::vector<float>, 15>& weights) -> unsigned int {
                    std::size_t vertexCount = 0;
                    for (const auto& slot : weights)
                        vertexCount = std::max(vertexCount, slot.size());

                    unsigned int count = 0;
                    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
                    {
                        bool weighted = false;
                        for (const auto& slot : weights)
                        {
                            if (vertex < slot.size() && slot[vertex] > 0.f)
                            {
                                weighted = true;
                                break;
                            }
                        }
                        if (weighted)
                            ++count;
                    }
                    return count;
                };

                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    ++mSeenRigGeometryCount;
                    source = rig->getSourceGeometry();
                    if (vertexCount(source) == 0)
                    {
                        for (unsigned int i = 0; i < 2; ++i)
                        {
                            osg::Geometry* renderGeometry = rig->getRenderGeometry(i);
                            if (vertexCount(renderGeometry) == 0)
                                continue;
                            source = renderGeometry;
                            break;
                        }
                    }
                    sourceName = source != nullptr ? source->getName() : std::string();
                    rootBone = std::string(rig->getRootBone());
                    boneCount = rig->getBoneCount();
                    hasFingerWeights = rig->getFalloutFingerBoneVertexWeights(fingerBoneWeights);
                    kind = "SceneUtil::RigGeometry";
                }
                else if (SceneUtil::RigGeometryHolder* holder = dynamic_cast<SceneUtil::RigGeometryHolder*>(&drawable))
                {
                    ++mSeenRigGeometryCount;
                    osg::ref_ptr<SceneUtil::OsgaRigGeometry> holderSource = holder->getSourceRigGeometry();
                    source = dynamic_cast<osg::Geometry*>(holderSource.get());
                    sourceName = source != nullptr ? source->getName() : std::string();
                    kind = "SceneUtil::RigGeometryHolder";
                }
                else
                    return nullptr;

                if (source == nullptr)
                {
                    ++mMissingSourceGeometryCount;
                    Log(Debug::Info) << "FNV/ESM4 diag: staticize rig drawable has no source geometry kind=" << kind
                                     << " name=" << drawable.getName() << " rootBone=" << rootBone
                                     << " bones=" << boneCount;
                    return nullptr;
                }

                Log(Debug::Info) << "FNV/ESM4 diag: staticize replacing rig drawable kind=" << kind
                                 << " name=" << drawable.getName() << " source=" << sourceName
                                 << " rootBone=" << rootBone << " bones=" << boneCount
                                 << " vertices=" << vertexCount(source);

                const std::string staticSource = Misc::StringUtils::lowerCase(
                    drawable.getName() + " " + sourceName + " " + rootBone);
                const bool staticHand = staticSource.find("hand") != std::string::npos
                    || staticSource.find("glove") != std::string::npos;
                if (staticHand)
                {
                    const bool leftHand = staticSource.find("left") != std::string::npos
                        || staticSource.find("bip01 l ") != std::string::npos;
                    const bool rightHand = staticSource.find("right") != std::string::npos
                        || staticSource.find("bip01 r ") != std::string::npos;
                    Log(Debug::Info) << "FNV/ESM4 diag: actor static hand no-twist proof kind=" << kind
                                     << " actor=" << (mActor.empty() ? std::string("<unknown>") : mActor)
                                     << " model=" << (mModel.empty() ? std::string("<unknown>") : mModel)
                                     << " side="
                                     << (leftHand ? "left" : rightHand ? "right" : "unknown")
                                     << " name=" << drawable.getName() << " source=" << sourceName
                                     << " rootBone=" << rootBone << " bones=" << boneCount
                                     << " vertices=" << vertexCount(source)
                                     << " fingerWeights=" << (hasFingerWeights ? "LOADED" : "MISSING")
                                     << " weightedSlots="
                                     << (hasFingerWeights ? weightedSlotCount(fingerBoneWeights) : 0)
                                     << " weightedVertices="
                                     << (hasFingerWeights ? weightedVertexCount(fingerBoneWeights) : 0)
                                     << " runtime=runtime-supported gate=runtime-fnv-static-hand-no-twist"
                                     << " fingerArticulation=loaded-pending-runtime"
                                     << " articulationGate=runtime-fnv-actor-hand-finger-articulation";
                }

                osg::ref_ptr<osg::Geometry> staticGeometry = osg::clone(source, osg::CopyOp::DEEP_COPY_ALL);
                staticGeometry->setName(drawable.getName().empty() ? source->getName() : drawable.getName());
                staticGeometry->setNodeMask(~0u);
                staticGeometry->setCullingActive(false);
                staticGeometry->setComputeBoundingBoxCallback(nullptr);
                staticGeometry->setComputeBoundingSphereCallback(nullptr);
                staticGeometry->dirtyBound();
                if (drawable.getStateSet() != nullptr)
                    staticGeometry->setStateSet(osg::clone(drawable.getStateSet(), osg::CopyOp::DEEP_COPY_ALL));
                return staticGeometry;
            }

            unsigned int mSeenRigGeometryCount = 0;
            unsigned int mMissingSourceGeometryCount = 0;
            unsigned int mStaticizedRigGeometryCount = 0;

        private:
            std::string mActor;
            std::string mModel;
        };

        bool isFalloutHiddenMorphShape(std::string_view name)
        {
            return Misc::StringUtils::ciStartsWith(name, "Tri ");
        }

        bool isFalloutHiddenMorphDrawable(const osg::Drawable& drawable)
        {
            if (isFalloutHiddenMorphShape(drawable.getName()))
                return true;

            const SceneUtil::RigGeometry* rig = dynamic_cast<const SceneUtil::RigGeometry*>(&drawable);
            if (rig == nullptr)
                return false;

            if (const osg::Geometry* source = rig->getSourceGeometry())
                if (isFalloutHiddenMorphShape(source->getName()))
                    return true;

            for (unsigned int i = 0; i < 2; ++i)
                if (const osg::Geometry* geometry = rig->getRenderGeometry(i))
                    if (isFalloutHiddenMorphShape(geometry->getName()))
                        return true;

            return false;
        }

        class HideFalloutHiddenMorphVisitor : public osg::NodeVisitor
        {
        public:
            HideFalloutHiddenMorphVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Drawable& drawable) override
            {
                if (!isFalloutHiddenMorphDrawable(drawable))
                    return;

                drawable.setNodeMask(0);
                drawable.setCullingActive(true);
                ++mHidden;
            }

            unsigned int mHidden = 0;
        };

        unsigned int hideFalloutDrawableAndRigGeometry(osg::Drawable& drawable)
        {
            unsigned int hidden = 0;
            const auto hideDrawable = [&](osg::Drawable* target) {
                if (target == nullptr)
                    return;
                target->setNodeMask(0);
                target->setCullingActive(true);
                ++hidden;
            };

            hideDrawable(&drawable);
            if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
            {
                hideDrawable(rig->getSourceGeometry());
                for (unsigned int i = 0; i < 2; ++i)
                    hideDrawable(rig->getRenderGeometry(i));
            }
            return hidden;
        }

        bool isFonvNoHatHairDrawable(const osg::Drawable& drawable)
        {
            if (Misc::StringUtils::ciEqual(drawable.getName(), "NoHat"))
                return true;

            const SceneUtil::RigGeometry* rig = dynamic_cast<const SceneUtil::RigGeometry*>(&drawable);
            if (rig == nullptr)
                return false;

            if (const osg::Geometry* source = rig->getSourceGeometry())
                if (Misc::StringUtils::ciEqual(source->getName(), "NoHat"))
                    return true;

            for (unsigned int i = 0; i < 2; ++i)
                if (const osg::Geometry* geometry = rig->getRenderGeometry(i))
                    if (Misc::StringUtils::ciEqual(geometry->getName(), "NoHat"))
                        return true;

            return false;
        }

        bool isFonvHatHairDrawable(const osg::Drawable& drawable)
        {
            if (Misc::StringUtils::ciEqual(drawable.getName(), "Hat"))
                return true;

            const SceneUtil::RigGeometry* rig = dynamic_cast<const SceneUtil::RigGeometry*>(&drawable);
            if (rig == nullptr)
                return false;

            if (const osg::Geometry* source = rig->getSourceGeometry())
                if (Misc::StringUtils::ciEqual(source->getName(), "Hat"))
                    return true;

            for (unsigned int i = 0; i < 2; ++i)
                if (const osg::Geometry* geometry = rig->getRenderGeometry(i))
                    if (Misc::StringUtils::ciEqual(geometry->getName(), "Hat"))
                        return true;

            return false;
        }

        class HideFonvNoHatHairVariantVisitor : public osg::NodeVisitor
        {
        public:
            HideFonvNoHatHairVariantVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Drawable& drawable) override
            {
                if (!isFonvNoHatHairDrawable(drawable))
                    return;

                mHiddenGeometry += hideFalloutDrawableAndRigGeometry(drawable);
                ++mHidden;
            }

            unsigned int mHidden = 0;
            unsigned int mHiddenGeometry = 0;
        };

        class HideFonvHatHairVariantVisitor : public osg::NodeVisitor
        {
        public:
            HideFonvHatHairVariantVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Drawable& drawable) override
            {
                if (!isFonvHatHairDrawable(drawable))
                    return;

                mHiddenGeometry += hideFalloutDrawableAndRigGeometry(drawable);
                ++mHidden;
            }

            unsigned int mHidden = 0;
            unsigned int mHiddenGeometry = 0;
        };

        bool isFalloutDialogueHeadSkinSurface(std::string_view model);
        bool fonvCoveredSlotsHideScalpHair(uint32_t coveredSlots);

        float getFonvCoveredHeadSkinScalpMaskZFraction()
        {
            const char* value = std::getenv("OPENMW_FNV_COVERED_HEAD_SKIN_SCALP_Z_FRACTION");
            if (value == nullptr || value[0] == '\0')
                return 0.76f;

            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || !std::isfinite(parsed))
                return 0.76f;

            return std::clamp(parsed, 0.60f, 0.92f);
        }

        struct FonvCoveredHeadSkinCapMaskStats
        {
            unsigned int mRemovedPrimitiveSets = 0;
            unsigned int mRewrittenPrimitiveSets = 0;
            unsigned int mSkippedPrimitiveSets = 0;
            unsigned int mRemovedTriangles = 0;
            unsigned int mKeptTriangles = 0;
            float mLowestRemovedZ = std::numeric_limits<float>::max();
            float mHighestKeptZ = -std::numeric_limits<float>::max();
            float mCutoffZ = 0.f;
        };

        unsigned int removeFonvCoveredHeadSkinCapPrimitiveSets(osg::Geometry& geometry)
        {
            const unsigned int primitiveSets = geometry.getNumPrimitiveSets();
            if (primitiveSets <= 1)
                return 0;

            geometry.removePrimitiveSet(1, primitiveSets - 1);
            geometry.dirtyDisplayList();
            geometry.dirtyBound();
            return primitiveSets - 1;
        }

        void removeFonvCoveredHeadSkinScalpTriangles(osg::Geometry& geometry, FonvCoveredHeadSkinCapMaskStats& stats)
        {
            const osg::Vec3Array* vertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
            if (vertices == nullptr || vertices->empty() || geometry.getNumPrimitiveSets() == 0)
                return;

            osg::BoundingBox bounds;
            for (const osg::Vec3f& vertex : *vertices)
                bounds.expandBy(vertex);
            if (!bounds.valid())
                return;

            const osg::Vec3f extent(bounds.xMax() - bounds.xMin(), bounds.yMax() - bounds.yMin(),
                bounds.zMax() - bounds.zMin());
            if (extent.z() <= 0.001f)
                return;

            const float cutoffZ = bounds.zMin() + extent.z() * getFonvCoveredHeadSkinScalpMaskZFraction();
            stats.mCutoffZ = cutoffZ;

            std::vector<osg::ref_ptr<osg::PrimitiveSet>> primitiveSets;
            primitiveSets.reserve(geometry.getNumPrimitiveSets());
            bool changed = false;

            for (unsigned int primitiveIndex = 0; primitiveIndex < geometry.getNumPrimitiveSets(); ++primitiveIndex)
            {
                osg::PrimitiveSet* primitive = geometry.getPrimitiveSet(primitiveIndex);
                if (primitive == nullptr || primitive->getMode() != osg::PrimitiveSet::TRIANGLES
                    || primitive->getNumIndices() < 3 || primitive->getNumIndices() % 3 != 0)
                {
                    primitiveSets.emplace_back(primitive);
                    ++stats.mSkippedPrimitiveSets;
                    continue;
                }

                osg::ref_ptr<osg::DrawElementsUInt> kept
                    = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES);
                unsigned int removedTriangles = 0;
                for (unsigned int index = 0; index + 2 < primitive->getNumIndices(); index += 3)
                {
                    const unsigned int a = primitive->index(index);
                    const unsigned int b = primitive->index(index + 1);
                    const unsigned int c = primitive->index(index + 2);
                    if (a >= vertices->size() || b >= vertices->size() || c >= vertices->size())
                    {
                        kept->push_back(a);
                        kept->push_back(b);
                        kept->push_back(c);
                        ++stats.mKeptTriangles;
                        continue;
                    }

                    const osg::Vec3f& av = (*vertices)[a];
                    const osg::Vec3f& bv = (*vertices)[b];
                    const osg::Vec3f& cv = (*vertices)[c];
                    const float minZ = std::min({ av.z(), bv.z(), cv.z() });
                    const float maxZ = std::max({ av.z(), bv.z(), cv.z() });
                    if (minZ > cutoffZ)
                    {
                        ++removedTriangles;
                        ++stats.mRemovedTriangles;
                        stats.mLowestRemovedZ = std::min(stats.mLowestRemovedZ, minZ);
                        continue;
                    }

                    stats.mHighestKeptZ = std::max(stats.mHighestKeptZ, maxZ);
                    kept->push_back(a);
                    kept->push_back(b);
                    kept->push_back(c);
                    ++stats.mKeptTriangles;
                }

                if (removedTriangles == 0)
                    primitiveSets.emplace_back(primitive);
                else
                {
                    primitiveSets.emplace_back(kept);
                    ++stats.mRewrittenPrimitiveSets;
                    changed = true;
                }
            }

            if (!changed)
                return;

            geometry.removePrimitiveSet(0, geometry.getNumPrimitiveSets());
            for (const osg::ref_ptr<osg::PrimitiveSet>& primitive : primitiveSets)
                geometry.addPrimitiveSet(primitive.get());
            geometry.dirtyDisplayList();
            geometry.dirtyBound();
        }

        class HideFonvCoveredHeadSkinCapVisitor : public osg::NodeVisitor
        {
        public:
            HideFonvCoveredHeadSkinCapVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        applyDrawable(*drawable);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

            unsigned int mVisitedGeometry = 0;
            FonvCoveredHeadSkinCapMaskStats mStats;

        private:
            void applyGeometry(osg::Geometry& geometry)
            {
                ++mVisitedGeometry;
                mStats.mRemovedPrimitiveSets += removeFonvCoveredHeadSkinCapPrimitiveSets(geometry);
                removeFonvCoveredHeadSkinScalpTriangles(geometry, mStats);
            }

            void applyDrawable(osg::Drawable& drawable)
            {
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* source = rig->getSourceGeometry())
                        applyGeometry(*source);
                    for (unsigned int i = 0; i < 2; ++i)
                        if (osg::Geometry* render = rig->getRenderGeometry(i))
                            applyGeometry(*render);
                    return;
                }

                if (osg::Geometry* geometry = drawable.asGeometry())
                    applyGeometry(*geometry);
            }
        };

        void hideFonvCoveredHeadSkinCap(osg::Node* attached, std::string_view model, const ESM4::Npc& traits,
            uint32_t coveredSlots)
        {
            if (attached == nullptr || !traits.mIsFONV || !fonvCoveredSlotsHideScalpHair(coveredSlots)
                || !isFalloutDialogueHeadSkinSurface(model))
                return;

            HideFonvCoveredHeadSkinCapVisitor visitor;
            attached->accept(visitor);
            if (visitor.mStats.mRemovedPrimitiveSets == 0 && visitor.mStats.mRemovedTriangles == 0)
                return;

            Log(Debug::Info) << "FNV/ESM4 diag: hid covered head-skin cap primitiveSet(s)="
                             << visitor.mStats.mRemovedPrimitiveSets
                             << " scalpTriangles=" << visitor.mStats.mRemovedTriangles
                             << " keptTriangles=" << visitor.mStats.mKeptTriangles
                             << " rewrittenPrimitiveSets=" << visitor.mStats.mRewrittenPrimitiveSets
                             << " skippedPrimitiveSets=" << visitor.mStats.mSkippedPrimitiveSets
                             << " cutoffZ=" << visitor.mStats.mCutoffZ
                             << " lowestRemovedZ="
                             << (visitor.mStats.mLowestRemovedZ == std::numeric_limits<float>::max()
                                     ? 0.f
                                     : visitor.mStats.mLowestRemovedZ)
                             << " highestKeptZ="
                             << (visitor.mStats.mHighestKeptZ == -std::numeric_limits<float>::max()
                                     ? 0.f
                                     : visitor.mStats.mHighestKeptZ)
                             << " geometry=" << visitor.mVisitedGeometry
                             << " on " << model << " for " << traits.mEditorId << " slots=0x" << std::hex
                             << coveredSlots << std::dec
                             << " runtime=runtime-supported gate=runtime-fnv-covered-head-skin-cap-mask";
        }

        class ForceFalloutActorPartVisibleVisitor : public osg::NodeVisitor
        {
        public:
            ForceFalloutActorPartVisibleVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                if (node.getName().find("MeatCap") != std::string::npos
                    || isFalloutHiddenMorphShape(node.getName()))
                    return;

                traverse(node);
            }

            void apply(osg::Drawable& drawable) override
            {
                if (drawable.getName().find("MeatCap") != std::string::npos
                    || isFalloutHiddenMorphDrawable(drawable))
                    return;

                if (dynamic_cast<SceneUtil::RigGeometry*>(&drawable) == nullptr)
                    return;

                drawable.setNodeMask(~0u);
                drawable.setCullingActive(false);
                for (osg::Node* node : getNodePath())
                {
                    if (node != nullptr)
                        node->setNodeMask(~0u);
                }
                ++mRigGeometryCount;
            }

            unsigned int mRigGeometryCount = 0;
        };

        bool isFalloutFaceGenMorphTargetModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return containsAny(lowered,
                { "characters/head/", "characters\\head\\", "characters/hair/", "characters\\hair\\",
                    "characters/mouth/", "characters\\mouth\\" });
        }

        bool falloutFaceGenEgmExists(Resource::ResourceSystem* resourceSystem, std::string_view model)
        {
            std::string egmPath(model);
            const std::size_t dot = egmPath.find_last_of('.');
            if (dot == std::string::npos)
                return false;

            egmPath.replace(dot, std::string::npos, ".egm");
            const VFS::Path::Normalized correctedPath
                = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(egmPath));
            return resourceSystem->getVFS()->exists(correctedPath);
        }

        bool applyFaceGenEgmMorph(Resource::ResourceSystem* resourceSystem, osg::Node* attached, std::string_view model,
            const ESM4::Npc& traits)
        {
            if (attached == nullptr)
                return false;

            if (!isFalloutFaceGenMorphTargetModel(model))
            {
                if (falloutFaceGenEgmExists(resourceSystem, model))
                    Log(Debug::Info) << "FNV/ESM4 diag: not applying FaceGen EGM morph to non-face model "
                                     << model << " for " << traits.mEditorId
                                     << " runtime=loaded-pending-equipment-facegen-fit";
                return false;
            }

            const std::shared_ptr<const FaceGenEgm> egm = loadFaceGenEgm(resourceSystem, model);
            if (!egm)
                return false;

            FaceGenMorphVisitor morphVisitor(*egm, traits);
            attached->accept(morphVisitor);
            if (morphVisitor.getMorphedDrawableCount() > 0)
            {
                Log(Debug::Info) << "FNV/ESM4 diag: applied FaceGen EGM morph " << model << " to "
                                 << morphVisitor.getMorphedDrawableCount() << " drawable(s) for " << traits.mEditorId
                                 << " maxVertexDelta=" << morphVisitor.getMaxDelta();
                return true;
            }

            Log(Debug::Warning) << "FNV/ESM4 diag: FaceGen EGM vertex layout did not match " << model << " for "
                                << traits.mEditorId << " candidates=" << morphVisitor.getCandidateDrawableCount()
                                << " firstVertices=" << morphVisitor.getFirstVertexCount()
                                << " egmVertices=" << egm->mVertexCount;
            return false;
        }

        bool applyFalloutProofTriStaticMorph(Resource::ResourceSystem* resourceSystem, osg::Node* attached,
            std::string_view model, const ESM4::Npc& traits)
        {
            if (attached == nullptr)
                return false;

            const char* morphEnv = std::getenv("OPENMW_FNV_PROOF_TRI_STATIC_MORPH");
            if (morphEnv == nullptr || *morphEnv == '\0')
                return false;

            const std::shared_ptr<const FaceGenTri> tri = loadFaceGenTri(resourceSystem, model);
            if (!tri)
                return false;

            const std::string_view morphName(morphEnv);
            const auto found = tri->mStaticMorphs.find(morphName);
            if (found == tri->mStaticMorphs.end())
            {
                std::string names;
                for (const auto& [name, ignored] : tri->mStaticMorphs)
                {
                    if (!names.empty())
                        names += ",";
                    names += name;
                }
                Log(Debug::Info) << "FNV/ESM4 proof: static TRI morph " << morphName << " not applied to " << model
                                 << " for " << traits.mEditorId << " available=[" << names << "]";
                return false;
            }

            FaceGenTriMorphVisitor morphVisitor(*tri, morphName, 1.f, true);
            attached->accept(morphVisitor);
            if (morphVisitor.getMorphedDrawableCount() == 0)
                return false;

            Log(Debug::Info) << "FNV/ESM4 proof: applied static TRI morph " << morphName << " on " << model << " to "
                             << morphVisitor.getMorphedDrawableCount() << " drawable(s) for " << traits.mEditorId
                             << " maxVertexDelta=" << morphVisitor.getMaxDelta();
            return true;
        }

        class FalloutDialogueMorphDriver : public osg::NodeCallback
        {
        public:
            FalloutDialogueMorphDriver(Animation* animation, const FaceGenTri& tri, const std::string& model)
                : mAnimation(animation)
                , mModel(model)
            {
                for (const auto& [morphName, deltas] : tri.mDiffMorphs)
                {
                    Morph m;
                    m.name = morphName;
                    m.deltas = deltas;
                    mMorphs.push_back(std::move(m));
                }
                mLastValues.resize(mMorphs.size(), -1.f);
            }

            bool empty() const { return mTargets.empty() || mMorphs.empty(); }

            void operator()(osg::Node* node, osg::NodeVisitor* nv) override
            {
                if (mAnimation == nullptr || mMorphs.empty() || mTargets.empty())
                {
                    traverse(node, nv);
                    return;
                }

                std::vector<float> values;
                values.reserve(mMorphs.size());

                for (const Morph& morph : mMorphs)
                {
                    float val = mAnimation->getFalloutHeadAnimTrackValue(morph.name);
                    values.push_back(val);
                }

                if (mLastValues != values)
                {
                    for (Target& target : mTargets)
                        applyTarget(target, values);
                    mLastValues = values;
                }

                traverse(node, nv);
            }

            struct Target
            {
                osg::ref_ptr<SceneUtil::RigGeometry> mRig;
                osg::ref_ptr<osg::Geometry> mGeometry;
                osg::ref_ptr<osg::Vec3Array> mBaseVertices;
                osg::ref_ptr<osg::Vec3Array> mActiveVertices;
            };

            void addRig(SceneUtil::RigGeometry& rig)
            {
                if (rig.getSourceGeometry() == nullptr)
                    return;

                const osg::Vec3Array* baseVertices = dynamic_cast<const osg::Vec3Array*>(rig.getSourceGeometry()->getVertexArray());
                if (baseVertices == nullptr)
                    return;

                osg::ref_ptr<osg::Geometry> morphed = new osg::Geometry(*rig.getSourceGeometry(), osg::CopyOp::SHALLOW_COPY);
                osg::ref_ptr<osg::Vec3Array> activeVertices = new osg::Vec3Array(*baseVertices);
                morphed->setVertexArray(activeVertices);

                morphed->setDataVariance(osg::Object::DYNAMIC);
                morphed->setUseDisplayList(false);
                morphed->setUseVertexBufferObjects(true);
                activeVertices->setDataVariance(osg::Object::DYNAMIC);

                rig.setSourceGeometry(morphed);

                Target t;
                t.mRig = &rig;
                t.mBaseVertices = new osg::Vec3Array(*baseVertices);
                t.mActiveVertices = activeVertices;
                mTargets.push_back(t);
            }

            void addGeometry(osg::Geometry& geometry)
            {
                const osg::Vec3Array* baseVertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
                if (baseVertices == nullptr)
                    return;

                osg::ref_ptr<osg::Geometry> morphed = new osg::Geometry(geometry, osg::CopyOp::SHALLOW_COPY);
                osg::ref_ptr<osg::Vec3Array> activeVertices = new osg::Vec3Array(*baseVertices);
                morphed->setVertexArray(activeVertices);

                morphed->setDataVariance(osg::Object::DYNAMIC);
                morphed->setUseDisplayList(false);
                morphed->setUseVertexBufferObjects(true);
                activeVertices->setDataVariance(osg::Object::DYNAMIC);

                for (unsigned int parentIndex = 0; parentIndex < geometry.getNumParents(); ++parentIndex)
                {
                    if (osg::Geode* parent = dynamic_cast<osg::Geode*>(geometry.getParent(parentIndex)))
                    {
                        const unsigned int drawableIndex = parent->getDrawableIndex(&geometry);
                        if (drawableIndex < parent->getNumDrawables())
                            parent->setDrawable(drawableIndex, morphed);
                    }
                }

                Target t;
                t.mGeometry = morphed;
                t.mBaseVertices = new osg::Vec3Array(*baseVertices);
                t.mActiveVertices = activeVertices;
                mTargets.push_back(t);
            }

        private:
            struct Morph
            {
                std::string name;
                std::vector<osg::Vec3f> deltas;
            };

            void applyTarget(Target& target, const std::vector<float>& values)
            {
                if (target.mBaseVertices == nullptr || target.mActiveVertices == nullptr)
                    return;

                for (std::size_t i = 0; i < target.mBaseVertices->size(); ++i)
                    (*target.mActiveVertices)[i] = (*target.mBaseVertices)[i];

                for (size_t m = 0; m < mMorphs.size(); ++m)
                {
                    float val = values[m];
                    if (val == 0.f)
                        continue;

                    const auto& deltas = mMorphs[m].deltas;
                    if (target.mActiveVertices->size() != deltas.size())
                        continue;

                    for (std::size_t i = 0; i < target.mActiveVertices->size(); ++i)
                        (*target.mActiveVertices)[i] += deltas[i] * val;
                }

                target.mActiveVertices->dirty();

                if (target.mGeometry != nullptr)
                {
                    target.mGeometry->dirtyDisplayList();
                    target.mGeometry->dirtyBound();
                }
            }

            Animation* mAnimation;
            std::string mModel;
            std::vector<Morph> mMorphs;
            std::vector<Target> mTargets;
            std::vector<float> mLastValues;
        };

        class FalloutDialogueMorphTargetVisitor : public osg::NodeVisitor
        {
        public:
            FalloutDialogueMorphTargetVisitor(FalloutDialogueMorphDriver& driver)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mDriver(driver)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    applyDrawable(*geode.getDrawable(i));
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

        private:
            void applyDrawable(osg::Drawable& drawable)
            {
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                    mDriver.addRig(*rig);
                else if (osg::Geometry* geometry = dynamic_cast<osg::Geometry*>(&drawable))
                    mDriver.addGeometry(*geometry);
            }

            FalloutDialogueMorphDriver& mDriver;
        };

        bool isFalloutDialogueHeadSkinSurface(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("characters/head/head") != std::string::npos
                || lowered.find("characters\\head\\head") != std::string::npos;
        }

        bool applyFalloutDialogueMorph(
            Resource::ResourceSystem* resourceSystem, Animation* animation, osg::Node* attached,
            std::string_view model, const ESM4::Npc& traits)
        {
            if (attached == nullptr)
                return false;

            const std::shared_ptr<const FaceGenTri> tri = loadFaceGenTri(resourceSystem, model);
            if (!tri)
                return false;

            if (isFalloutDialogueHeadSkinSurface(model))
            {
                Log(Debug::Info) << "FNV/ESM4 diag: loaded head TRI dialogue morph source " << model
                                 << " vertices=" << tri->mVertexCount << " diffMorphs=" << tri->mDiffMorphs.size()
                                 << " staticMorphs=" << tri->mStaticMorphs.size() << " for " << traits.mEditorId
                                 << " runtime=loaded-pending-runtime-head-dialogue-morph";
                return false;
            }

            osg::ref_ptr<FalloutDialogueMorphDriver> driver
                = new FalloutDialogueMorphDriver(animation, *tri, std::string(model));
            FalloutDialogueMorphTargetVisitor targetVisitor(*driver);
            attached->accept(targetVisitor);

            if (driver->empty())
                return false;

            attached->addUpdateCallback(driver);
            return true;
        }


        bool applyFalloutProofTriOpenMorph(
            Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& actor, osg::Node* attached,
            std::string_view model, const ESM4::Npc& traits)
        {
            if (attached == nullptr)
                return false;

            const std::shared_ptr<const FaceGenTri> tri = loadFaceGenTri(resourceSystem, model);
            if (!tri)
                return false;

            constexpr std::string_view morphName = "BigAah";
            if (tri->mDiffMorphs.find(morphName) == tri->mDiffMorphs.end())
            {
                Log(Debug::Info) << "FNV/ESM4 proof: FaceGen TRI morph " << morphName << " not applied to " << model
                                 << " for " << traits.mEditorId << " morphs=" << tri->mDiffMorphs.size();
                return false;
            }

            if (std::getenv("OPENMW_FNV_PROOF_MOUTH_FORCE_OPEN") != nullptr)
            {
                FaceGenTriMorphVisitor morphVisitor(*tri, morphName, 1.f);
                attached->accept(morphVisitor);
                if (morphVisitor.getMorphedDrawableCount() == 0)
                    return false;

                Log(Debug::Info) << "FNV/ESM4 proof: applied FaceGen TRI morph " << morphName << " on " << model
                                 << " to " << morphVisitor.getMorphedDrawableCount() << " drawable(s) for "
                                 << traits.mEditorId << " maxVertexDelta=" << morphVisitor.getMaxDelta();
                return true;
            }

            osg::ref_ptr<FalloutProofTriMorphDriver> driver
                = new FalloutProofTriMorphDriver(actor, *tri, morphName, std::string(model));
            FalloutProofTriMorphTargetVisitor targetVisitor(*driver);
            attached->accept(targetVisitor);
            if (driver->empty())
                return false;

            attached->addUpdateCallback(driver);
            Log(Debug::Info) << "FNV/ESM4 proof: installed FaceGen TRI morph driver " << morphName << " on " << model
                             << " for " << traits.mEditorId;
            return true;
        }

        osg::Group* findBestAttachmentNode(
            const Animation::NodeMap& nodeMap, std::initializer_list<std::string_view> names)
        {
            for (std::string_view name : names)
            {
                const auto found = nodeMap.find(name);
                if (found != nodeMap.end())
                    return found->second.get();
            }

            for (std::string_view name : names)
            {
                for (const auto& [nodeName, node] : nodeMap)
                    if (Misc::StringUtils::ciEqual(nodeName, name))
                        return node.get();
            }

            return nullptr;
        }

        bool shouldAttachFalloutStaticPartToHead(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("characters/head/head") != std::string::npos
                || lowered.find("characters\\head\\head") != std::string::npos
                || lowered.find("mouth") != std::string::npos || lowered.find("teeth") != std::string::npos
                || lowered.find("tongue") != std::string::npos || lowered.find("eye") != std::string::npos
                || lowered.find("hair") != std::string::npos || lowered.find("beard") != std::string::npos
                || lowered.find("brow") != std::string::npos || lowered.find("headgear") != std::string::npos
                || lowered.find("hat") != std::string::npos;
        }

        bool isFalloutHeadSurfaceModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("characters/head/head") != std::string::npos
                || lowered.find("characters\\head\\head") != std::string::npos;
        }

        bool isFalloutBareHandSurfaceModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("characters/_male/lefthand.nif") != std::string::npos
                || lowered.find("characters\\_male\\lefthand.nif") != std::string::npos
                || lowered.find("characters/_male/righthand.nif") != std::string::npos
                || lowered.find("characters\\_male\\righthand.nif") != std::string::npos;
        }

        bool isFalloutLeftHandSurfaceModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("lefthand.nif") != std::string::npos;
        }

        bool isFalloutStaticHeadgearPart(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("headgear") != std::string::npos || lowered.find("hat") != std::string::npos;
        }

        bool isFalloutHairTintModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("characters/hair/") != std::string::npos
                || lowered.find("characters\\hair\\") != std::string::npos
                || lowered.find("beard") != std::string::npos || lowered.find("brow") != std::string::npos;
        }

        bool isFalloutEyeSurfaceModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("characters/head/eye") != std::string::npos
                || lowered.find("characters\\head\\eye") != std::string::npos;
        }

        std::string getFalloutHairTintDiffuseOverride(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            if (lowered.find("hairbaseold") != std::string::npos)
                return "textures/characters/hair/hairafricanamericanbase_hl.dds";
            return {};
        }

        bool isFalloutMouthDriverPart(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("mouth") != std::string::npos || lowered.find("teeth") != std::string::npos
                || lowered.find("tongue") != std::string::npos;
        }

        bool isFalloutBrowModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("brow") != std::string::npos;
        }

        bool isFalloutEyeModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("eye") != std::string::npos && !isFalloutBrowModel(lowered);
        }

        bool isFalloutFaceHairModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("beard") != std::string::npos || lowered.find("facial") != std::string::npos;
        }

        bool isFalloutScalpHairModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("hair") != std::string::npos && !isFalloutBrowModel(lowered)
                && !isFalloutFaceHairModel(lowered);
        }

        bool fonvCoveredSlotsHideScalpHair(uint32_t coveredSlots)
        {
            constexpr uint32_t hairCoverSlots = ESM4::Armor::FO3_Head | ESM4::Armor::FO3_Hair
                | ESM4::Armor::FO3_Headband | ESM4::Armor::FO3_Hat;
            return (coveredSlots & hairCoverSlots) != 0;
        }

        bool fonvCoveredSlotsUseHeadStack(uint32_t coveredSlots)
        {
            constexpr uint32_t headStackSlots = ESM4::Armor::FO3_Head | ESM4::Armor::FO3_Hair
                | ESM4::Armor::FO3_Necklace | ESM4::Armor::FO3_Headband | ESM4::Armor::FO3_Hat
                | ESM4::Armor::FO3_EyeGlasses | ESM4::Armor::FO3_NoseRing | ESM4::Armor::FO3_Earrings
                | ESM4::Armor::FO3_Mask | ESM4::Armor::FO3_Choker | ESM4::Armor::FO3_MouthObject;
            return (coveredSlots & headStackSlots) != 0;
        }

        const char* getFonvEquipmentAssemblyLayer(uint32_t coveredSlots)
        {
            if (!fonvCoveredSlotsUseHeadStack(coveredSlots))
                return "body";
            return fonvCoveredSlotsHideScalpHair(coveredSlots) ? "headgear-final" : "head-face-final";
        }

        bool isFonvEquipmentHeadStackLayer(std::string_view layer)
        {
            return layer == "headgear-final" || layer == "head-face-final";
        }

        bool shouldUseFonvHatCompatibleHairVariant(const ESM4::HeadPart& part, uint32_t coveredSlots)
        {
            if (!fonvCoveredSlotsHideScalpHair(coveredSlots))
                return false;

            if (part.mType == ESM4::HeadPart::Type_Hair)
                return true;

            return part.mType == ESM4::HeadPart::Type_Misc && isFalloutScalpHairModel(part.mModel)
                && !isFonvFacialHairHeadPart(part);
        }

        std::string getFalloutCharacterBuilderPhase()
        {
            std::string phase = readFalloutLiveRuntimeOrEnvString("OPENMW_FNV_CHARACTER_BUILDER_PHASE",
                { "characterBuilderPhase", "phase" });
            if (phase.empty())
                return "full";

            Misc::StringUtils::lowerCaseInPlace(phase);
            return phase;
        }

        bool isFalloutCharacterBuilderActive()
        {
            static constexpr const char* envNames[] = {
                "OPENMW_FNV_CHARACTER_BUILDER_PHASE",
                "OPENMW_FNV_ACTOR_KIT_PARTS",
                "OPENMW_FNV_ACTOR_KIT_PART_MODELS",
                "OPENMW_FNV_ACTOR_KIT_PROP_SLOTS",
                "OPENMW_FNV_ACTOR_KIT_PROP_MODELS",
            };
            for (const char* envName : envNames)
            {
                const char* value = std::getenv(envName);
                if (value != nullptr && value[0] != '\0')
                    return true;
            }
            return isFalloutLiveRuntimeActorKitActive();
        }

        int getFalloutCharacterBuilderRank(std::string_view value)
        {
            if (Misc::StringUtils::ciEqual(value, "body") || Misc::StringUtils::ciEqual(value, "body-skin"))
                return 10;
            if (Misc::StringUtils::ciEqual(value, "head") || Misc::StringUtils::ciEqual(value, "race-head")
                || Misc::StringUtils::ciEqual(value, "head-skin"))
                return 20;
            if (Misc::StringUtils::ciEqual(value, "face") || Misc::StringUtils::ciEqual(value, "face-organs")
                || Misc::StringUtils::ciEqual(value, "mouth") || Misc::StringUtils::ciEqual(value, "eyes"))
                return 30;
            if (Misc::StringUtils::ciEqual(value, "hair") || Misc::StringUtils::ciEqual(value, "hair-beard")
                || Misc::StringUtils::ciEqual(value, "beard"))
                return 40;
            if (Misc::StringUtils::ciEqual(value, "equipment")
                || Misc::StringUtils::ciEqual(value, "equipment-body"))
                return 50;
            if (Misc::StringUtils::ciEqual(value, "weapon") || Misc::StringUtils::ciEqual(value, "weapons"))
                return 60;
            if (Misc::StringUtils::ciEqual(value, "headgear") || Misc::StringUtils::ciEqual(value, "hat"))
                return 70;
            if (Misc::StringUtils::ciEqual(value, "talk") || Misc::StringUtils::ciEqual(value, "dialogue")
                || Misc::StringUtils::ciEqual(value, "animate") || Misc::StringUtils::ciEqual(value, "animation"))
                return 80;
            return 100;
        }

        std::string trimFalloutActorKitToken(std::string token)
        {
            while (!token.empty()
                && (token.front() == ' ' || token.front() == '\t' || token.front() == '\r'
                    || token.front() == '\n' || token.front() == '"' || token.front() == '\''))
                token.erase(token.begin());
            while (!token.empty()
                && (token.back() == ' ' || token.back() == '\t' || token.back() == '\r'
                    || token.back() == '\n' || token.back() == '"' || token.back() == '\''))
                token.pop_back();
            return token;
        }

        std::string normalizeFalloutActorKitCategoryToken(std::string_view value)
        {
            std::string token = trimFalloutActorKitToken(std::string(value));
            Misc::StringUtils::lowerCaseInPlace(token);
            std::replace(token.begin(), token.end(), '_', '-');
            return token;
        }

        std::string normalizeFalloutActorKitModelToken(std::string_view value)
        {
            std::string token = trimFalloutActorKitToken(std::string(value));
            Misc::StringUtils::lowerCaseInPlace(token);
            std::replace(token.begin(), token.end(), '/', '\\');
            return token;
        }

        std::vector<std::string> getFalloutActorKitCategoryTokens(const char* envName)
        {
            std::vector<std::string> tokens;
            const std::string liveRaw = readFalloutActorKitRuntimeSelectorForEnv(envName);
            const char* raw = liveRaw.empty() ? std::getenv(envName) : liveRaw.c_str();
            if (raw == nullptr || raw[0] == '\0')
                return tokens;

            std::string current;
            for (const char c : std::string_view(raw))
            {
                if (c == ',' || c == ';' || c == '|' || c == '\r' || c == '\n')
                {
                    const std::string token = normalizeFalloutActorKitCategoryToken(current);
                    if (!token.empty())
                        tokens.push_back(token);
                    current.clear();
                    continue;
                }
                current.push_back(c);
            }
            const std::string token = normalizeFalloutActorKitCategoryToken(current);
            if (!token.empty())
                tokens.push_back(token);
            return tokens;
        }

        std::vector<std::string> getFalloutActorKitModelTokens(const char* envName)
        {
            std::vector<std::string> tokens;
            const std::string liveRaw = readFalloutActorKitRuntimeSelectorForEnv(envName);
            const char* raw = liveRaw.empty() ? std::getenv(envName) : liveRaw.c_str();
            if (raw == nullptr || raw[0] == '\0')
                return tokens;

            std::string current;
            for (const char c : std::string_view(raw))
            {
                if (c == ',' || c == ';' || c == '|' || c == '\r' || c == '\n')
                {
                    const std::string token = normalizeFalloutActorKitModelToken(current);
                    if (!token.empty())
                        tokens.push_back(token);
                    current.clear();
                    continue;
                }
                current.push_back(c);
            }
            const std::string token = normalizeFalloutActorKitModelToken(current);
            if (!token.empty())
                tokens.push_back(token);
            return tokens;
        }

        bool isFalloutActorKitAllToken(std::string_view token)
        {
            return token == "all" || token == "*";
        }

        bool isFalloutActorKitPropCategory(std::string_view category)
        {
            return Misc::StringUtils::ciEqual(category, "equipment-body")
                || Misc::StringUtils::ciEqual(category, "weapon")
                || Misc::StringUtils::ciEqual(category, "headgear");
        }

        bool falloutActorKitContextDependencyMatches(std::string_view token, std::string_view normalizedCategory)
        {
            if (token == "face" || token == "face-organs" || token == "mouth" || token == "eyes"
                || token == "teeth" || token == "tongue" || token == "brow")
                return normalizedCategory == "body-skin" || normalizedCategory == "head-skin"
                    || normalizedCategory == "hair-beard";

            if (token == "hair" || token == "hair-beard" || token == "beard")
                return normalizedCategory == "body-skin" || normalizedCategory == "head-skin";

            if (token == "head" || token == "head-skin" || token == "race-head")
                return normalizedCategory == "body-skin";

            if (token == "headgear" || token == "hat")
                return normalizedCategory == "body-skin" || normalizedCategory == "head-skin"
                    || normalizedCategory == "hair-beard";

            if (token == "weapon" || token == "weapons")
                return normalizedCategory == "body-skin" || normalizedCategory == "equipment-body";

            if (token == "talk" || token == "dialogue" || token == "animate" || token == "animation")
                return getFalloutCharacterBuilderRank(normalizedCategory) <= getFalloutCharacterBuilderRank("talk");

            return false;
        }

        bool falloutActorKitCategoryTokenMatches(std::string_view token, std::string_view category)
        {
            if (isFalloutActorKitAllToken(token))
                return true;

            const std::string normalizedCategory = normalizeFalloutActorKitCategoryToken(category);
            if (token == normalizedCategory)
                return true;

            if (token == "body-equipment" && normalizedCategory == "equipment-body")
                return true;

            if (falloutActorKitContextDependencyMatches(token, normalizedCategory))
                return true;

            const int tokenRank = getFalloutCharacterBuilderRank(token);
            return tokenRank != 100 && tokenRank == getFalloutCharacterBuilderRank(category);
        }

        bool falloutActorKitCategoryExplicitlySelected(std::string_view category)
        {
            const std::vector<std::string> partTokens = getFalloutActorKitCategoryTokens("OPENMW_FNV_ACTOR_KIT_PARTS");
            if (!partTokens.empty()
                && std::any_of(partTokens.begin(), partTokens.end(),
                    [&](const std::string& token) { return falloutActorKitCategoryTokenMatches(token, category); }))
                return true;

            if (isFalloutActorKitPropCategory(category))
            {
                const std::vector<std::string> slotTokens
                    = getFalloutActorKitCategoryTokens("OPENMW_FNV_ACTOR_KIT_PROP_SLOTS");
                if (!slotTokens.empty()
                    && std::any_of(slotTokens.begin(), slotTokens.end(),
                        [&](const std::string& token) { return falloutActorKitCategoryTokenMatches(token, category); }))
                    return true;
            }

            return false;
        }

        bool falloutActorKitCategoryAllows(std::string_view category)
        {
            const std::vector<std::string> partTokens = getFalloutActorKitCategoryTokens("OPENMW_FNV_ACTOR_KIT_PARTS");
            if (!partTokens.empty()
                && std::none_of(partTokens.begin(), partTokens.end(),
                    [&](const std::string& token) { return falloutActorKitCategoryTokenMatches(token, category); }))
                return false;

            if (isFalloutActorKitPropCategory(category))
            {
                const std::vector<std::string> slotTokens
                    = getFalloutActorKitCategoryTokens("OPENMW_FNV_ACTOR_KIT_PROP_SLOTS");
                if (!slotTokens.empty()
                    && std::none_of(slotTokens.begin(), slotTokens.end(),
                        [&](const std::string& token) { return falloutActorKitCategoryTokenMatches(token, category); }))
                    return false;
            }

            return true;
        }

        std::string falloutActorKitModelBasename(std::string_view model)
        {
            const std::string normalized = normalizeFalloutActorKitModelToken(model);
            const std::size_t slash = normalized.find_last_of('\\');
            return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
        }

        bool falloutActorKitModelTokenMatches(std::string_view token, std::string_view model)
        {
            if (isFalloutActorKitAllToken(token))
                return true;

            const std::string normalizedModel = normalizeFalloutActorKitModelToken(model);
            if (token == normalizedModel)
                return true;

            return token == falloutActorKitModelBasename(normalizedModel);
        }

        bool falloutActorKitModelListAllows(const char* envName, std::string_view model)
        {
            const std::vector<std::string> modelTokens = getFalloutActorKitModelTokens(envName);
            if (modelTokens.empty())
                return true;

            return std::any_of(modelTokens.begin(), modelTokens.end(),
                [&](const std::string& token) { return falloutActorKitModelTokenMatches(token, model); });
        }

        bool falloutActorKitModelAllows(std::string_view category, std::string_view model)
        {
            if (!falloutActorKitModelListAllows("OPENMW_FNV_ACTOR_KIT_PART_MODELS", model))
                return false;

            if (isFalloutActorKitPropCategory(category)
                && !falloutActorKitModelListAllows("OPENMW_FNV_ACTOR_KIT_PROP_MODELS", model))
                return false;

            return true;
        }

        bool falloutCharacterBuilderAllows(std::string_view category)
        {
            if (!isFalloutCharacterBuilderActive())
                return true;

            const std::string phase = getFalloutCharacterBuilderPhase();
            const bool phaseAllows = getFalloutCharacterBuilderRank(phase) >= getFalloutCharacterBuilderRank(category);
            const bool selectorExplicitlyAllows = falloutActorKitCategoryExplicitlySelected(category);
            return (phaseAllows || selectorExplicitlyAllows)
                && falloutActorKitCategoryAllows(category);
        }

        bool falloutCharacterBuilderAllows(std::string_view category, std::string_view model)
        {
            return falloutCharacterBuilderAllows(category) && falloutActorKitModelAllows(category, model);
        }

        void logFalloutCharacterBuilderGate(bool included, std::string_view category, std::string_view model,
            const MWWorld::Ptr& ptr, const ESM4::Npc& traits)
        {
            if (!isFalloutCharacterBuilderActive())
                return;

            const std::string phase = getFalloutCharacterBuilderPhase();
            Log(Debug::Info) << "FNV/ESM4 CHARACTER BUILDER " << (included ? "include" : "skip")
                             << " phase=" << phase
                             << " category=" << category
                             << " actor=" << traits.mEditorId
                             << " ref=" << ptr.getCellRef().getRefId()
                             << " model=" << (model.empty() ? std::string("<none>") : std::string(model))
                             << " classification="
                             << (included ? "runtime-supported" : "intentionally-excluded-with-proof");
        }

        void hideFonvUnsafeCoveredHatHairVariant(
            osg::Node* attached, std::string_view model, const ESM4::Npc& traits, uint32_t coveredSlots)
        {
            if (attached == nullptr || !fonvCoveredSlotsHideScalpHair(coveredSlots))
                return;

            if (std::getenv("OPENMW_FNV_HIDE_UNSAFE_HAT_HAIR_VARIANT") == nullptr)
            {
                Log(Debug::Info) << "FNV/ESM4 diag: preserved hat-compatible covered hair drawable(s) on "
                                 << model << " for " << traits.mEditorId
                                 << " classification=runtime-supported reason=hair-under-headgear"
                                 << " optIn=OPENMW_FNV_HIDE_UNSAFE_HAT_HAIR_VARIANT";
                return;
            }

            HideFonvHatHairVariantVisitor visitor;
            attached->accept(visitor);
            if (visitor.mHidden == 0)
                return;

            Log(Debug::Info) << "FNV/ESM4 diag: hid " << visitor.mHidden << " opaque-unsafe hat hair drawable(s) on "
                             << model << " for " << traits.mEditorId
                             << " because this covered hair variant draws over the face under opaque headgear slots=0x"
                             << std::hex << coveredSlots << std::dec
                             << " hiddenGeometry=" << visitor.mHiddenGeometry;
        }

        void hideFonvNoHatHairVariant(
            osg::Node* attached, std::string_view model, const ESM4::Npc& traits, uint32_t coveredSlots)
        {
            if (attached == nullptr || !fonvCoveredSlotsHideScalpHair(coveredSlots))
                return;

            HideFonvNoHatHairVariantVisitor visitor;
            attached->accept(visitor);
            if (visitor.mHidden != 0)
                Log(Debug::Info) << "FNV/ESM4 diag: hid " << visitor.mHidden << " no-hat hair drawable(s) on "
                                 << model << " for " << traits.mEditorId
                                 << " because equipped headgear uses hat-compatible hair slots=0x" << std::hex
                                 << coveredSlots << std::dec << " hiddenGeometry=" << visitor.mHiddenGeometry;

            hideFonvUnsafeCoveredHatHairVariant(attached, model, traits, coveredSlots);
        }

        bool isFalloutMouthSurfaceModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("teeth") != std::string::npos || lowered.find("tongue") != std::string::npos;
        }

        float readFalloutProofFloat(const char* name, float fallback)
        {
            if (const char* value = std::getenv(name))
            {
                char* end = nullptr;
                const float parsed = std::strtof(value, &end);
                if (end != value)
                    return parsed;
            }
            return fallback;
        }

        osg::Vec3f getFalloutHeadFrameSurfaceOffset(std::string_view model, bool headgearStaticPart)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);

            if (headgearStaticPart)
                return osg::Vec3f(readFalloutProofFloat("OPENMW_FNV_HEADGEAR_OFFSET_X", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_HEADGEAR_OFFSET_Y", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_HEADGEAR_OFFSET_Z", 0.f));
            if (lowered.find("brow") != std::string::npos)
                return osg::Vec3f(readFalloutProofFloat("OPENMW_FNV_BROW_OFFSET_X", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_BROW_OFFSET_Y", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_BROW_OFFSET_Z", 0.f));
            if (lowered.find("eye") != std::string::npos)
                return osg::Vec3f(readFalloutProofFloat("OPENMW_FNV_EYE_OFFSET_X", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_EYE_OFFSET_Y", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_EYE_OFFSET_Z", 0.f));
            if (lowered.find("beard") != std::string::npos)
                return osg::Vec3f(readFalloutProofFloat("OPENMW_FNV_BEARD_OFFSET_X", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_BEARD_OFFSET_Y", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_BEARD_OFFSET_Z", 0.f));
            if (lowered.find("mouth") != std::string::npos || lowered.find("teeth") != std::string::npos
                || lowered.find("tongue") != std::string::npos)
                return osg::Vec3f(readFalloutProofFloat("OPENMW_FNV_MOUTH_OFFSET_X", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_MOUTH_OFFSET_Y", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_MOUTH_OFFSET_Z", 0.f));
            if (lowered.find("hair") != std::string::npos)
                return osg::Vec3f(readFalloutProofFloat("OPENMW_FNV_HAIR_OFFSET_X", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_HAIR_OFFSET_Y", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_HAIR_OFFSET_Z", 0.f));
            return osg::Vec3f();
        }

        std::string getFalloutHeadFrameSurfacePrefix(std::string_view model, bool headgearStaticPart)
        {
            if (headgearStaticPart)
                return "OPENMW_FNV_HEADGEAR";

            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            if (lowered.find("brow") != std::string::npos)
                return "OPENMW_FNV_BROW";
            if (lowered.find("eye") != std::string::npos)
                return "OPENMW_FNV_EYE";
            if (lowered.find("beard") != std::string::npos)
                return "OPENMW_FNV_BEARD";
            if (lowered.find("mouth") != std::string::npos || lowered.find("teeth") != std::string::npos
                || lowered.find("tongue") != std::string::npos)
                return "OPENMW_FNV_MOUTH";
            if (lowered.find("hair") != std::string::npos)
                return "OPENMW_FNV_HAIR";
            return {};
        }

        osg::Quat makeFalloutEulerAttitude(const osg::Vec3f& degrees)
        {
            constexpr float degreesToRadians = 0.017453292519943295f;
            const osg::Quat x(degrees.x() * degreesToRadians, osg::Vec3f(1.f, 0.f, 0.f));
            const osg::Quat y(degrees.y() * degreesToRadians, osg::Vec3f(0.f, 1.f, 0.f));
            const osg::Quat z(degrees.z() * degreesToRadians, osg::Vec3f(0.f, 0.f, 1.f));
            return z * y * x;
        }

        float getFalloutHeadFrameSurfaceZFallback(std::string_view prefix)
        {
            const bool faceInternal = prefix == "OPENMW_FNV_EYE" || prefix == "OPENMW_FNV_MOUTH"
                || prefix == "OPENMW_FNV_BEARD" || prefix == "OPENMW_FNV_BROW";
            return (faceInternal || prefix == "OPENMW_FNV_HAIR") ? -90.f : 0.f;
        }

        osg::Vec3f getFalloutHeadFrameSurfaceRotationDegrees(std::string_view prefix)
        {
            if (prefix.empty())
                return osg::Vec3f();

            const std::string keyPrefix(prefix);
            return osg::Vec3f(readFalloutProofFloat((keyPrefix + "_ROTATION_X").c_str(), 0.f),
                readFalloutProofFloat((keyPrefix + "_ROTATION_Y").c_str(), 0.f),
                readFalloutProofFloat((keyPrefix + "_ROTATION_Z").c_str(), getFalloutHeadFrameSurfaceZFallback(prefix)));
        }

        osg::Vec3f getFalloutHeadFrameSurfacePivot(std::string_view prefix, const osg::Vec3f& fallback)
        {
            if (prefix.empty())
                return fallback;

            const std::string keyPrefix(prefix);
            osg::Vec3f pivot(readFalloutProofFloat((keyPrefix + "_PIVOT_X").c_str(), fallback.x()),
                readFalloutProofFloat((keyPrefix + "_PIVOT_Y").c_str(), fallback.y()),
                readFalloutProofFloat((keyPrefix + "_PIVOT_Z").c_str(), fallback.z()));
            pivot += osg::Vec3f(readFalloutProofFloat((keyPrefix + "_PIVOT_OFFSET_X").c_str(), 0.f),
                readFalloutProofFloat((keyPrefix + "_PIVOT_OFFSET_Y").c_str(), 0.f),
                readFalloutProofFloat((keyPrefix + "_PIVOT_OFFSET_Z").c_str(), 0.f));
            return pivot;
        }

        osg::Quat getFalloutHeadFrameSurfaceAttitude(std::string_view model, bool headgearStaticPart)
        {
            const std::string prefix = getFalloutHeadFrameSurfacePrefix(model, headgearStaticPart);
            if (prefix.empty())
                return osg::Quat();

            return makeFalloutEulerAttitude(getFalloutHeadFrameSurfaceRotationDegrees(prefix));
        }

        std::string escapeFalloutLiveAuthoringRegex(std::string_view value)
        {
            std::string escaped;
            escaped.reserve(value.size() * 2);
            for (char c : value)
            {
                switch (c)
                {
                    case '\\':
                    case '.':
                    case '^':
                    case '$':
                    case '|':
                    case '(':
                    case ')':
                    case '[':
                    case ']':
                    case '{':
                    case '}':
                    case '*':
                    case '+':
                    case '?':
                        escaped.push_back('\\');
                        break;
                    default:
                        break;
                }
                escaped.push_back(c);
            }
            return escaped;
        }

        bool readFalloutLiveAuthoringFloat(const std::string& content, const std::string& key, float& value)
        {
            try
            {
                const std::regex pattern("\"" + escapeFalloutLiveAuthoringRegex(key)
                    + "\"\\s*:\\s*(-?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?)");
                std::smatch match;
                if (!std::regex_search(content, match, pattern))
                    return false;
                value = std::stof(match[1].str());
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool readFalloutLiveAuthoringBool(const std::string& content, const std::string& key, bool& value)
        {
            try
            {
                const std::regex pattern(
                    "\"" + escapeFalloutLiveAuthoringRegex(key) + "\"\\s*:\\s*(true|false|0|1)",
                    std::regex_constants::icase);
                std::smatch match;
                if (!std::regex_search(content, match, pattern))
                    return false;
                std::string parsed = match[1].str();
                Misc::StringUtils::lowerCaseInPlace(parsed);
                value = parsed == "true" || parsed == "1";
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        const char* getFalloutLiveAuthoringFile()
        {
            const char* path = std::getenv("OPENMW_FNV_LIVE_AUTHORING_FILE");
            return path != nullptr && path[0] != '\0' ? path : nullptr;
        }

        bool readFalloutLiveAuthoringFile(const char* path, std::string& content)
        {
            if (path == nullptr || path[0] == '\0')
                return false;
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            std::ostringstream stream;
            stream << input.rdbuf();
            content = stream.str();
            return true;
        }

        osg::Matrix makeFalloutHeadSurfaceMatrix(
            const osg::Vec3f& offset, const osg::Quat& attitude, const osg::Vec3f& pivot, bool pivotMode)
        {
            if (pivotMode && !attitude.zeroRotation())
                return osg::Matrix::translate(-pivot) * osg::Matrix::rotate(attitude)
                    * osg::Matrix::translate(pivot + offset);
            return osg::Matrix::rotate(attitude) * osg::Matrix::translate(offset);
        }

        class FalloutLiveHeadSurfaceAuthoringCallback : public osg::NodeCallback
        {
        public:
            FalloutLiveHeadSurfaceAuthoringCallback(std::string prefix, std::string model, const osg::Vec3f& offset,
                const osg::Vec3f& rotationDegrees, const osg::Vec3f& pivot, bool pivotMode)
                : mPrefix(std::move(prefix))
                , mModel(std::move(model))
                , mDefaultOffset(offset)
                , mDefaultRotationDegrees(rotationDegrees)
                , mPivot(pivot)
                , mDefaultPivotMode(pivotMode)
            {
            }

            void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
            {
                const char* livePath = getFalloutLiveAuthoringFile();
                if (livePath != nullptr && mTick++ % 6 == 0)
                {
                    std::string content;
                    if (readFalloutLiveAuthoringFile(livePath, content) && content != mLastContent)
                    {
                        mLastContent = content;
                        osg::Vec3f offset = mDefaultOffset;
                        osg::Vec3f rotation = mDefaultRotationDegrees;
                        osg::Vec3f pivot = mPivot;
                        osg::Vec3f pivotOffset;
                        bool pivotMode = mDefaultPivotMode;
                        readFalloutLiveAuthoringFloat(content, mPrefix + "_OFFSET_X", offset.x());
                        readFalloutLiveAuthoringFloat(content, mPrefix + "_OFFSET_Y", offset.y());
                        readFalloutLiveAuthoringFloat(content, mPrefix + "_OFFSET_Z", offset.z());
                        readFalloutLiveAuthoringFloat(content, mPrefix + "_ROTATION_X", rotation.x());
                        readFalloutLiveAuthoringFloat(content, mPrefix + "_ROTATION_Y", rotation.y());
                        readFalloutLiveAuthoringFloat(content, mPrefix + "_ROTATION_Z", rotation.z());
                        readFalloutLiveAuthoringFloat(content, mPrefix + "_PIVOT_X", pivot.x());
                        readFalloutLiveAuthoringFloat(content, mPrefix + "_PIVOT_Y", pivot.y());
                        readFalloutLiveAuthoringFloat(content, mPrefix + "_PIVOT_Z", pivot.z());
                        readFalloutLiveAuthoringFloat(content, mPrefix + "_PIVOT_OFFSET_X", pivotOffset.x());
                        readFalloutLiveAuthoringFloat(content, mPrefix + "_PIVOT_OFFSET_Y", pivotOffset.y());
                        readFalloutLiveAuthoringFloat(content, mPrefix + "_PIVOT_OFFSET_Z", pivotOffset.z());
                        pivot += pivotOffset;
                        readFalloutLiveAuthoringBool(content, mPrefix + "_PIVOT_MODE", pivotMode);
                        if (osg::MatrixTransform* matrixNode = dynamic_cast<osg::MatrixTransform*>(node))
                        {
                            matrixNode->setMatrix(
                                makeFalloutHeadSurfaceMatrix(offset, makeFalloutEulerAttitude(rotation), pivot, pivotMode));
                            matrixNode->dirtyBound();
                            Log(Debug::Info)
                                << "FNV/ESM4 live authoring: applied head surface authoring model=" << mModel
                                << " prefix=" << mPrefix << " file=" << livePath << " offset=(" << offset.x() << ","
                                << offset.y() << "," << offset.z() << ") rotation=(" << rotation.x() << ","
                                << rotation.y() << "," << rotation.z() << ") pivot=(" << pivot.x() << ","
                                << pivot.y() << "," << pivot.z() << ") pivotOffset=(" << pivotOffset.x() << ","
                                << pivotOffset.y() << "," << pivotOffset.z() << ") pivotMode=" << pivotMode;
                        }
                    }
                }
                traverse(node, visitor);
            }

        private:
            std::string mPrefix;
            std::string mModel;
            osg::Vec3f mDefaultOffset;
            osg::Vec3f mDefaultRotationDegrees;
            osg::Vec3f mPivot;
            bool mDefaultPivotMode = false;
            unsigned int mTick = 0;
            std::string mLastContent;
        };

        osg::Quat getFalloutFaceSurfaceAttitude()
        {
            constexpr float degreesToRadians = 0.017453292519943295f;
            const float xDegrees = readFalloutProofFloat("OPENMW_FNV_FACE_ROTATION_X", 0.f);
            const float yDegrees = readFalloutProofFloat("OPENMW_FNV_FACE_ROTATION_Y", 0.f);
            const float zDegrees = readFalloutProofFloat("OPENMW_FNV_FACE_ROTATION_Z", 0.f);
            const osg::Quat x(xDegrees * degreesToRadians, osg::Vec3f(1.f, 0.f, 0.f));
            const osg::Quat y(yDegrees * degreesToRadians, osg::Vec3f(0.f, 1.f, 0.f));
            const osg::Quat z(zDegrees * degreesToRadians, osg::Vec3f(0.f, 0.f, 1.f));
            return z * y * x;
        }

        osg::Vec3f getFalloutFaceSurfacePosition()
        {
            return osg::Vec3f(readFalloutProofFloat("OPENMW_FNV_FACE_OFFSET_X", 0.f),
                readFalloutProofFloat("OPENMW_FNV_FACE_OFFSET_Y", 0.f),
                readFalloutProofFloat("OPENMW_FNV_FACE_OFFSET_Z", 0.f));
        }

        osg::Quat getFalloutHeadFrameAttitude()
        {
            constexpr float degreesToRadians = 0.017453292519943295f;
            const float xDegrees = readFalloutProofFloat("OPENMW_FNV_HEAD_FRAME_ROTATION_X", 0.f);
            const float yDegrees = readFalloutProofFloat("OPENMW_FNV_HEAD_FRAME_ROTATION_Y", 0.f);
            const float zDegrees = readFalloutProofFloat("OPENMW_FNV_HEAD_FRAME_ROTATION_Z", 0.f);
            const osg::Quat x(xDegrees * degreesToRadians, osg::Vec3f(1.f, 0.f, 0.f));
            const osg::Quat y(yDegrees * degreesToRadians, osg::Vec3f(0.f, 1.f, 0.f));
            const osg::Quat z(zDegrees * degreesToRadians, osg::Vec3f(0.f, 0.f, 1.f));
            return z * y * x;
        }

        osg::Group* makeFalloutFaceSurfaceFrameHelper(osg::Group& parent)
        {
            constexpr std::string_view helperName = "FNV Face Surface Frame";
            const osg::Quat attitude = getFalloutFaceSurfaceAttitude();
            const osg::Vec3f position = getFalloutFaceSurfacePosition();
            for (unsigned int i = 0; i < parent.getNumChildren(); ++i)
            {
                osg::PositionAttitudeTransform* existing
                    = dynamic_cast<osg::PositionAttitudeTransform*>(parent.getChild(i));
                if (existing != nullptr && existing->getName() == std::string(helperName))
                {
                    existing->setAttitude(attitude);
                    existing->setPosition(position);
                    return existing;
                }
            }

            osg::ref_ptr<osg::PositionAttitudeTransform> helper = new osg::PositionAttitudeTransform;
            helper->setName(std::string(helperName));
            helper->setAttitude(attitude);
            helper->setPosition(position);
            parent.addChild(helper);
            Log(Debug::Info) << "FNV/ESM4 diag: inserted face surface frame under " << parent.getName()
                             << " rotation=(" << readFalloutProofFloat("OPENMW_FNV_FACE_ROTATION_X", 0.f) << ","
                             << readFalloutProofFloat("OPENMW_FNV_FACE_ROTATION_Y", 0.f) << ","
                             << readFalloutProofFloat("OPENMW_FNV_FACE_ROTATION_Z", 0.f) << ") offset=("
                             << position.x() << "," << position.y() << "," << position.z() << ")";
            return helper.get();
        }

        bool isFalloutStaticFaceChildPart(std::string_view model)
        {
            return shouldAttachFalloutStaticPartToHead(model) && !isFalloutStaticHeadgearPart(model)
                && !isFalloutHeadSurfaceModel(model);
        }

        bool isFalloutStaticHeadAttachmentPart(std::string_view model)
        {
            return shouldAttachFalloutStaticPartToHead(model) && !isFalloutHeadSurfaceModel(model);
        }

        osg::Matrix getNodeWorldMatrix(osg::Node* node)
        {
            if (node == nullptr)
                return osg::Matrix::identity();

            const osg::NodePathList paths = node->getParentalNodePaths();
            if (!paths.empty())
                return osg::computeLocalToWorld(paths.front());

            if (const osg::MatrixTransform* transform = dynamic_cast<const osg::MatrixTransform*>(node))
                return transform->getMatrix();

            return osg::Matrix::identity();
        }

        osg::ref_ptr<osg::MatrixTransform> makeFalloutHeadFrameHelper(osg::Group& bip01, osg::Group& head)
        {
            constexpr std::string_view helperName = "FNV Head Frame";
            for (unsigned int i = 0; i < bip01.getNumChildren(); ++i)
            {
                if (osg::MatrixTransform* existing = dynamic_cast<osg::MatrixTransform*>(bip01.getChild(i)))
                    if (existing->getName() == std::string(helperName))
                        return existing;
            }

            const osg::Matrix headWorld = getNodeWorldMatrix(&head);
            const osg::Matrix bipWorld = getNodeWorldMatrix(&bip01);
            const osg::Matrix headInBip = headWorld * osg::Matrix::inverse(bipWorld);

            osg::ref_ptr<osg::MatrixTransform> helper = new osg::MatrixTransform;
            helper->setName(std::string(helperName));
            const bool useFullHeadFrame = std::getenv("OPENMW_FNV_HEAD_FRAME_FULL_MATRIX") != nullptr;
            osg::Matrix headFrame = useFullHeadFrame ? headInBip : osg::Matrix::translate(headInBip.getTrans());
            const osg::Quat headFrameAttitude = getFalloutHeadFrameAttitude();
            const bool parentOrder = std::getenv("OPENMW_FNV_HEAD_FRAME_ROTATION_PARENT_ORDER") != nullptr;
            if (!headFrameAttitude.zeroRotation())
            {
                const osg::Matrix rotation = osg::Matrix::rotate(headFrameAttitude);
                headFrame = parentOrder ? headFrame * rotation : rotation * headFrame;
            }
            helper->setMatrix(headFrame);
            bip01.addChild(helper);
            Log(Debug::Info) << "FNV/ESM4 diag: inserted head frame helper at ("
                             << headInBip.getTrans().x() << ", " << headInBip.getTrans().y() << ", "
                             << headInBip.getTrans().z() << ") under " << bip01.getName()
                             << " fullMatrix=" << useFullHeadFrame << " rotation=("
                             << readFalloutProofFloat("OPENMW_FNV_HEAD_FRAME_ROTATION_X", 0.f) << ","
                             << readFalloutProofFloat("OPENMW_FNV_HEAD_FRAME_ROTATION_Y", 0.f) << ","
                             << readFalloutProofFloat("OPENMW_FNV_HEAD_FRAME_ROTATION_Z", 0.f) << ") parentOrder="
                             << parentOrder;
            return helper;
        }

        osg::ref_ptr<osg::MatrixTransform> makeFalloutAnimatedHeadFrameHelper(osg::Group& bip01, osg::Group& head)
        {
            constexpr std::string_view helperName = "FNV Animated Head Frame";
            for (unsigned int i = 0; i < head.getNumChildren(); ++i)
            {
                if (osg::MatrixTransform* existing = dynamic_cast<osg::MatrixTransform*>(head.getChild(i)))
                    if (existing->getName() == std::string(helperName))
                        return existing;
            }

            const osg::Matrix headWorld = getNodeWorldMatrix(&head);
            const osg::Matrix bipWorld = getNodeWorldMatrix(&bip01);
            const osg::Matrix headInBip = headWorld * osg::Matrix::inverse(bipWorld);

            const bool useFullHeadFrame = std::getenv("OPENMW_FNV_HEAD_FRAME_FULL_MATRIX") != nullptr;
            osg::Matrix headFrameInBip = useFullHeadFrame ? headInBip : osg::Matrix::translate(headInBip.getTrans());
            const osg::Quat headFrameAttitude = getFalloutHeadFrameAttitude();
            const bool parentOrder = std::getenv("OPENMW_FNV_HEAD_FRAME_ROTATION_PARENT_ORDER") != nullptr;
            if (!headFrameAttitude.zeroRotation())
            {
                const osg::Matrix rotation = osg::Matrix::rotate(headFrameAttitude);
                headFrameInBip = parentOrder ? headFrameInBip * rotation : rotation * headFrameInBip;
            }

            osg::Matrix localInHead = headFrameInBip * osg::Matrix::inverse(headInBip);
            if (std::getenv("OPENMW_FNV_ANIMATED_HEAD_FRAME_ALT_ORDER") != nullptr)
                localInHead = osg::Matrix::inverse(headInBip) * headFrameInBip;
            osg::ref_ptr<osg::MatrixTransform> helper = new osg::MatrixTransform;
            helper->setName(std::string(helperName));
            helper->setMatrix(localInHead);
            head.addChild(helper);
            osg::Quat localRotation = localInHead.getRotate();
            Log(Debug::Info) << "FNV/ESM4 diag: inserted animated head frame helper local=("
                             << localInHead.getTrans().x() << ", " << localInHead.getTrans().y() << ", "
                             << localInHead.getTrans().z() << ") localQuat=(" << localRotation.x() << ", "
                             << localRotation.y() << ", " << localRotation.z() << ", " << localRotation.w()
                             << ") under " << head.getName()
                             << " fullMatrix=" << useFullHeadFrame << " rotation=("
                             << readFalloutProofFloat("OPENMW_FNV_HEAD_FRAME_ROTATION_X", 0.f) << ","
                             << readFalloutProofFloat("OPENMW_FNV_HEAD_FRAME_ROTATION_Y", 0.f) << ","
                             << readFalloutProofFloat("OPENMW_FNV_HEAD_FRAME_ROTATION_Z", 0.f) << ") parentOrder="
                             << parentOrder;
            return helper;
        }

        osg::ref_ptr<osg::MatrixTransform> makeFalloutAnimatedBoneBindFrameHelper(
            osg::Group& bip01, osg::Group& bone, std::string_view helperName)
        {
            for (unsigned int i = 0; i < bone.getNumChildren(); ++i)
            {
                if (osg::MatrixTransform* existing = dynamic_cast<osg::MatrixTransform*>(bone.getChild(i)))
                    if (existing->getName() == std::string(helperName))
                        return existing;
            }

            const osg::Matrix boneWorld = getNodeWorldMatrix(&bone);
            const osg::Matrix bipWorld = getNodeWorldMatrix(&bip01);
            const osg::Matrix boneInBip = boneWorld * osg::Matrix::inverse(bipWorld);
            osg::Matrix localInBone;
            if (!localInBone.invert(boneInBip))
                localInBone.makeIdentity();

            osg::ref_ptr<osg::MatrixTransform> helper = new osg::MatrixTransform;
            helper->setName(std::string(helperName));
            helper->setMatrix(localInBone);
            bone.addChild(helper);

            const osg::Quat localRotation = localInBone.getRotate();
            Log(Debug::Info) << "FNV/ESM4 diag: inserted animated bone bind-frame helper " << helperName
                             << " under " << bone.getName() << " local=(" << localInBone.getTrans().x()
                             << ", " << localInBone.getTrans().y() << ", " << localInBone.getTrans().z()
                             << ") localQuat=(" << localRotation.x() << ", " << localRotation.y() << ", "
                             << localRotation.z() << ", " << localRotation.w() << ")";
            return helper;
        }

        std::string_view getFonvStaticFaceAttachMode(std::string_view model)
        {
            const char* mode = std::getenv("OPENMW_FNV_STATIC_HEAD_ATTACH_MODE");
            if (mode == nullptr || mode[0] == '\0')
                return "headframe";
            return mode;
        }

        std::string_view getFalloutHeadgearAttitudeMode()
        {
            const char* mode = std::getenv("OPENMW_FNV_HEADGEAR_ROTATION_MODE");
            if (mode == nullptr || mode[0] == '\0')
                return "z90";
            return mode;
        }

        osg::Quat getFalloutHeadgearAttitude()
        {
            const std::string_view mode = getFalloutHeadgearAttitudeMode();
            constexpr double halfPi = 1.57079632679489661923;
            if (Misc::StringUtils::ciEqual(mode, "none") || Misc::StringUtils::ciEqual(mode, "identity"))
                return osg::Quat();
            if (Misc::StringUtils::ciEqual(mode, "x90"))
                return osg::Quat(halfPi, osg::Vec3d(1.0, 0.0, 0.0));
            if (Misc::StringUtils::ciEqual(mode, "x-90"))
                return osg::Quat(-halfPi, osg::Vec3d(1.0, 0.0, 0.0));
            if (Misc::StringUtils::ciEqual(mode, "y90"))
                return osg::Quat(halfPi, osg::Vec3d(0.0, 1.0, 0.0));
            if (Misc::StringUtils::ciEqual(mode, "y-90"))
                return osg::Quat(-halfPi, osg::Vec3d(0.0, 1.0, 0.0));
            if (Misc::StringUtils::ciEqual(mode, "z90"))
                return osg::Quat(halfPi, osg::Vec3d(0.0, 0.0, 1.0));
            if (Misc::StringUtils::ciEqual(mode, "z-90"))
                return osg::Quat(-halfPi, osg::Vec3d(0.0, 0.0, 1.0));
            return osg::Quat();
        }

        void logFalloutPartShapeSummary(osg::Node* attached, std::string_view model, const MWWorld::Ptr& ptr)
        {
            if (attached == nullptr)
                return;

            FalloutPartShapeSummaryVisitor visitor;
            attached->accept(visitor);
            Log(Debug::Info) << "FNV/ESM4 diag: NPC part shape summary " << model << " for "
                             << ptr.getCellRef().getRefId() << " rigGeometry=" << visitor.mRigGeometryCount
                             << " staticGeometry=" << visitor.mStaticGeometryCount
                             << " firstRigRoot='" << visitor.mFirstRigRootBone << "' firstRigBones="
                             << visitor.mFirstRigBoneCount << " sample=[" << visitor.mFirstRigBoneSample.str() << "]";
        }

        osg::Vec3f boundingBoxExtent(const osg::BoundingBox& box)
        {
            if (!box.valid())
                return osg::Vec3f();
            return osg::Vec3f(box.xMax() - box.xMin(), box.yMax() - box.yMin(), box.zMax() - box.zMin());
        }

        osg::Vec3f localBoundsCenter(osg::Node& node)
        {
            osg::ComputeBoundsVisitor boundsVisitor;
            node.accept(boundsVisitor);
            const osg::BoundingBox box = boundsVisitor.getBoundingBox();
            return box.valid() ? box.center() : osg::Vec3f();
        }

        bool isFalloutAccessoryModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return containsAny(lowered,
                { "weapon", "headgear", "hat", "hair", "beard", "brow", "eye", "mouth", "teeth", "tongue" });
        }

        bool isFalloutHeadRelativeModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("characters/head/head") != std::string::npos
                || lowered.find("characters\\head\\head") != std::string::npos
                || containsAny(
                    lowered, { "headgear", "hat", "hair", "beard", "brow", "eye", "mouth", "teeth", "tongue" });
        }

        bool isFalloutFaceTightModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("characters/head/head") != std::string::npos
                || lowered.find("characters\\head\\head") != std::string::npos
                || containsAny(lowered, { "beard", "brow", "eye", "mouth", "teeth", "tongue" });
        }

        osg::Vec3f transformPoint(const osg::Vec3f& point, const osg::Matrix& matrix)
        {
            const osg::Vec3d transformed = osg::Vec3d(point) * matrix;
            return osg::Vec3f(transformed.x(), transformed.y(), transformed.z());
        }

        class FindNamedNodeVisitor : public osg::NodeVisitor
        {
        public:
            FindNamedNodeVisitor(std::string_view name)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mName(name)
            {
            }

            void apply(osg::Node& node) override
            {
                if (mFound == nullptr && Misc::StringUtils::ciEqual(node.getName(), mName))
                    mFound = &node;
                if (mFound == nullptr)
                    traverse(node);
            }

            osg::Node* mFound = nullptr;

        private:
            std::string mName;
        };

        osg::StateSet* getFalloutMirroredFrontFaceStateSet()
        {
            static osg::ref_ptr<osg::StateSet> stateSet;
            if (stateSet == nullptr)
            {
                osg::ref_ptr<osg::FrontFace> frontFace = new osg::FrontFace;
                frontFace->setMode(osg::FrontFace::CLOCKWISE);
                stateSet = new osg::StateSet;
                stateSet->setAttributeAndModes(frontFace, osg::StateAttribute::ON);
            }
            return stateSet.get();
        }

        osg::ref_ptr<osg::Node> attachStaticizedFalloutPart(osg::ref_ptr<const osg::Node> templateNode,
            osg::Group* attachNode, Resource::SceneManager* sceneManager, bool normalizeLargeBounds)
        {
            osg::ref_ptr<osg::Node> cloned = sceneManager->getInstance(templateNode);
            FindNamedNodeVisitor findBoneOffset("BoneOffset");
            cloned->accept(findBoneOffset);

            osg::ref_ptr<osg::PositionAttitudeTransform> transform;
            if (osg::MatrixTransform* boneOffset = dynamic_cast<osg::MatrixTransform*>(findBoneOffset.mFound))
            {
                transform = new osg::PositionAttitudeTransform;
                transform->setPosition(boneOffset->getMatrix().getTrans());
                if (std::getenv("OPENMW_FNV_APPLY_BONEOFFSET_ROTATION") != nullptr)
                    transform->setAttitude(boneOffset->getMatrix().getRotate());

                if (boneOffset->getNumChildren() == 0 && boneOffset->getNumParents() == 1)
                    boneOffset->getParent(0)->removeChild(boneOffset);
            }

            if (attachNode != nullptr && attachNode->getName().find("Left") != std::string::npos)
            {
                if (transform == nullptr)
                    transform = new osg::PositionAttitudeTransform;
                transform->setScale(osg::Vec3f(-1.f, 1.f, 1.f));
                transform->setStateSet(getFalloutMirroredFrontFaceStateSet());
            }

            osg::ComputeBoundsVisitor boundsVisitor;
            cloned->accept(boundsVisitor);
            const osg::BoundingBox bounds = boundsVisitor.getBoundingBox();
            if (bounds.valid() && normalizeLargeBounds)
            {
                const osg::Vec3f center = bounds.center();
                if (center.length() > 40.f)
                {
                    if (transform == nullptr)
                        transform = new osg::PositionAttitudeTransform;
                    transform->setPosition(transform->getPosition() - center);
                    Log(Debug::Info) << "FNV/ESM4 diag: normalized staticized rig part local center=("
                                     << center.x() << "," << center.y() << "," << center.z()
                                     << ") distance=" << center.length();
                }
            }
            else if (bounds.valid() && !normalizeLargeBounds)
            {
                const osg::Vec3f center = bounds.center();
                Log(Debug::Info) << "FNV/ESM4 diag: kept staticized rig part skeleton-space local center=("
                                 << center.x() << "," << center.y() << "," << center.z()
                                 << ") distance=" << center.length();
            }

            if (transform != nullptr)
            {
                attachNode->addChild(transform);
                transform->addChild(cloned);
                return transform;
            }

            attachNode->addChild(cloned);
            return cloned;
        }

        void logFalloutAttachmentBounds(osg::Node* attached, osg::Group* attachNode, osg::Group* headNode,
            std::string_view model, const MWWorld::Ptr& ptr)
        {
            if (attached == nullptr)
                return;

            osg::ComputeBoundsVisitor boundsVisitor;
            attached->accept(boundsVisitor);
            const osg::BoundingBox box = boundsVisitor.getBoundingBox();
            if (!box.valid())
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: attachment bounds invalid for " << model << " on "
                                    << ptr.getCellRef().getRefId() << " parent="
                                    << (attachNode != nullptr ? attachNode->getName() : std::string("<none>"));
                return;
            }

            const osg::Vec3f center = box.center();
            const osg::Vec3f extent = boundingBoxExtent(box);
            const osg::Vec3f worldCenter
                = attachNode != nullptr ? transformPoint(center, getNodeWorldMatrix(attachNode)) : center;
            const osg::Matrix headWorld = headNode != nullptr ? getNodeWorldMatrix(headNode) : osg::Matrix::identity();
            const osg::Vec3f headOrigin
                = headNode != nullptr ? transformPoint(osg::Vec3f(), headWorld) : osg::Vec3f();
            const osg::Vec3f headDelta = worldCenter - headOrigin;
            osg::Matrix headWorldInverse;
            if (!headWorldInverse.invert(headWorld))
                headWorldInverse.makeIdentity();
            const osg::Vec3f headLocalCenter
                = headNode != nullptr ? transformPoint(worldCenter, headWorldInverse) : headDelta;
            const float centerDistance = center.length();
            const float diagonal = extent.length();
            const bool accessory = isFalloutAccessoryModel(model);
            const bool headRelative = headNode != nullptr && isFalloutHeadRelativeModel(model);
            const bool faceTight = headRelative && isFalloutFaceTightModel(model);
            const bool mouthLike = isFalloutMouthDriverPart(model);
            const bool eyeLike = isFalloutEyeModel(model);
            const bool browLike = isFalloutBrowModel(model);
            const bool faceHairLike = isFalloutFaceHairModel(model);
            const bool checksFaceAxes = faceTight && (mouthLike || eyeLike || browLike || faceHairLike);
            const osg::Vec3f headFrameCenter = headDelta;
            bool headAxisDetached = false;
            const char* headAxisReason = "";
            if (checksFaceAxes)
            {
                const float absX = std::abs(headFrameCenter.x());
                // FNV face attachments are authored in head space with +Y as the face/front axis and +Z up.
                if (mouthLike
                    && (absX > 4.5f || headFrameCenter.y() < 2.f || headFrameCenter.y() > 8.5f
                        || headFrameCenter.z() < -1.f || headFrameCenter.z() > 7.5f))
                {
                    headAxisDetached = true;
                    headAxisReason = "mouth-not-front";
                }
                else if (eyeLike
                    && (absX < 0.75f || absX > 4.25f || headFrameCenter.y() < 4.5f
                        || headFrameCenter.y() > 8.5f || headFrameCenter.z() < 5.f || headFrameCenter.z() > 10.f))
                {
                    headAxisDetached = true;
                    headAxisReason = "eye-not-front";
                }
                else if (browLike
                    && (absX > 7.5f || headFrameCenter.y() < 3.5f || headFrameCenter.y() > 9.5f
                        || headFrameCenter.z() < 5.5f || headFrameCenter.z() > 12.f))
                {
                    headAxisDetached = true;
                    headAxisReason = "brow-not-front";
                }
                else if (faceHairLike
                    && (absX > 8.f || headFrameCenter.y() < 0.5f || headFrameCenter.y() > 9.f
                        || headFrameCenter.z() < -3.f || headFrameCenter.z() > 10.f))
                {
                    headAxisDetached = true;
                    headAxisReason = "facehair-not-front";
                }
            }
            const float maxHeadPlanarDelta = faceTight ? 18.f : 42.f;
            const float maxHeadVerticalDelta = faceTight ? 24.f : 46.f;
            const float headPlanarDelta = std::sqrt(headDelta.x() * headDelta.x() + headDelta.y() * headDelta.y());
            const bool headDetached = headRelative && !isFalloutHeadSurfaceModel(model)
                && (headPlanarDelta > maxHeadPlanarDelta || std::abs(headDelta.z()) > maxHeadVerticalDelta);
            const bool suspicious = (accessory && (centerDistance > 180.f || diagonal > 260.f))
                || (!accessory && centerDistance > 420.f) || headDetached || headAxisDetached;

            std::ostringstream message;
            message << "FNV/ESM4 diag: attachment bounds " << model << " for " << ptr.getCellRef().getRefId()
                    << " parent=" << (attachNode != nullptr ? attachNode->getName() : std::string("<none>"))
                    << " center=(" << center.x() << "," << center.y() << "," << center.z() << ")"
                    << " extent=(" << extent.x() << "," << extent.y() << "," << extent.z() << ")"
                    << " worldCenter=(" << worldCenter.x() << "," << worldCenter.y() << "," << worldCenter.z()
                    << ") attachLocal=(" << center.x() << "," << center.y() << "," << center.z()
                    << ") headRel=(" << headDelta.x() << "," << headDelta.y() << "," << headDelta.z()
                    << ") headLocal=(" << headLocalCenter.x() << "," << headLocalCenter.y() << ","
                    << headLocalCenter.z() << ") headFrame=(" << headFrameCenter.x() << ","
                    << headFrameCenter.y() << "," << headFrameCenter.z() << ")";
            if (headNode != nullptr)
                message << " headDelta=(" << headDelta.x() << "," << headDelta.y() << "," << headDelta.z()
                        << ") headPlanarDelta=" << headPlanarDelta;
            else
                message << " headDelta=(n/a)";
            message << " centerDistance=" << centerDistance << " diagonal=" << diagonal
                    << " headRelative=" << headRelative << " faceTight=" << faceTight
                    << " faceAxisChecked=" << checksFaceAxes << " faceAxisReason="
                    << (headAxisDetached ? headAxisReason : "OK")
                    << " verdict=" << (suspicious ? "SUSPECT" : "OK");

            if (suspicious)
                Log(Debug::Warning) << message.str();
            else
                Log(Debug::Info) << message.str();
        }

        osg::ref_ptr<osg::MatrixTransform> makeFalloutAttachmentHelper(
            osg::Group& parent, std::string_view name, Animation::NodeMap& nodeMap)
        {
            osg::ref_ptr<osg::MatrixTransform> helper = new osg::MatrixTransform;
            helper->setName(std::string(name));
            helper->setDataVariance(osg::Object::DYNAMIC);
            if (Misc::StringUtils::ciEqual(name, "Weapon"))
            {
                const osg::Vec3f offset(readFalloutProofFloat("OPENMW_FNV_WEAPON_OFFSET_X", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_WEAPON_OFFSET_Y", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_WEAPON_OFFSET_Z", 0.f));
                constexpr float degreesToRadians = 0.017453292519943295f;
                const float xDegrees = readFalloutProofFloat("OPENMW_FNV_WEAPON_ROTATION_X", 0.f);
                const float yDegrees = readFalloutProofFloat("OPENMW_FNV_WEAPON_ROTATION_Y", 0.f);
                const float zDegrees = readFalloutProofFloat("OPENMW_FNV_WEAPON_ROTATION_Z", 0.f);
                const osg::Quat x(xDegrees * degreesToRadians, osg::Vec3f(1.f, 0.f, 0.f));
                const osg::Quat y(yDegrees * degreesToRadians, osg::Vec3f(0.f, 1.f, 0.f));
                const osg::Quat z(zDegrees * degreesToRadians, osg::Vec3f(0.f, 0.f, 1.f));
                const osg::Quat rotation = z * y * x;
                helper->setMatrix(osg::Matrix::rotate(rotation) * osg::Matrix::translate(offset));
                Log(Debug::Info) << "FNV/ESM4 diag: weapon helper bone-local offset=(" << offset.x() << ","
                                 << offset.y() << "," << offset.z() << ") rotation=(" << xDegrees << ","
                                 << yDegrees << "," << zDegrees << ")";
            }
            parent.addChild(helper);
            nodeMap.emplace(std::string(name), helper);
            return helper;
        }

        osg::Quat makeFalloutProofDialoguePoseRotation(float xDegrees, float yDegrees, float zDegrees)
        {
            constexpr float degreesToRadians = 0.017453292519943295f;
            const osg::Quat x(xDegrees * degreesToRadians, osg::Vec3f(1.f, 0.f, 0.f));
            const osg::Quat y(yDegrees * degreesToRadians, osg::Vec3f(0.f, 1.f, 0.f));
            const osg::Quat z(zDegrees * degreesToRadians, osg::Vec3f(0.f, 0.f, 1.f));
            return z * y * x;
        }

        float readFalloutProofPoseDegrees(const char* name, float fallback)
        {
            if (const char* value = std::getenv(name))
            {
                char* end = nullptr;
                const float parsed = std::strtof(value, &end);
                if (end != value)
                    return parsed;
            }
            return fallback;
        }

        osg::Quat makeFalloutProofDialoguePoseRotationFromEnv(
            const char* prefix, float fallbackX, float fallbackY, float fallbackZ)
        {
            const std::string base(prefix);
            return makeFalloutProofDialoguePoseRotation(
                readFalloutProofPoseDegrees((base + "_X").c_str(), fallbackX),
                readFalloutProofPoseDegrees((base + "_Y").c_str(), fallbackY),
                readFalloutProofPoseDegrees((base + "_Z").c_str(), fallbackZ));
        }

        bool addFalloutProofDialogueBonePose(osg::MatrixTransform& bone, std::string_view name, const osg::Quat& rotation)
        {
            osg::ref_ptr<osg::Callback> pose = new FalloutProofDialogueBonePose(std::string(name), rotation);
            osg::Callback* updateCb = bone.getUpdateCallback();
            while (updateCb != nullptr)
            {
                if (dynamic_cast<osgAnimation::UpdateBone*>(updateCb) != nullptr)
                {
                    osg::ref_ptr<osg::Callback> nested = updateCb->getNestedCallback();
                    updateCb->setNestedCallback(pose);
                    if (nested != nullptr)
                        pose->setNestedCallback(nested);
                    return true;
                }
                updateCb = updateCb->getNestedCallback();
            }

            bone.addUpdateCallback(pose);
            return false;
        }

        void addFalloutProofDialoguePose(const Animation::NodeMap& nodeMap, const MWWorld::Ptr& ptr, const ESM4::Npc& traits)
        {
            if (std::getenv("OPENMW_FNV_PROOF_DIALOGUE_POSE") == nullptr)
                return;
            if (!isFonvProofTargetActor(ptr, traits))
                return;

            struct BonePose
            {
                std::string_view mName;
                osg::Quat mRotation;
            };

            std::vector<BonePose> poses = {
                { "Bip01 L UpperArm",
                    makeFalloutProofDialoguePoseRotationFromEnv(
                        "OPENMW_FNV_PROOF_POSE_L_UPPERARM", 0.f, 0.f, -82.f) },
                { "Bip01 R UpperArm",
                    makeFalloutProofDialoguePoseRotationFromEnv(
                        "OPENMW_FNV_PROOF_POSE_R_UPPERARM", 0.f, 0.f, 82.f) },
                { "Bip01 L Forearm",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_L_FOREARM", 0.f, 0.f, 4.f) },
                { "Bip01 R Forearm",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_R_FOREARM", 0.f, 0.f, -4.f) },
                { "Bip01 L Hand",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_L_HAND", 0.f, 0.f, 4.f) },
                { "Bip01 R Hand",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_R_HAND", 0.f, 0.f, -4.f) },
                { "Bip01 Head",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_HEAD", -2.f, 0.f, 0.f) },
            };

            if (std::getenv("OPENMW_FNV_PROOF_SEATED_POSE") != nullptr)
            {
                poses.push_back({ "Bip01 Pelvis",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_PELVIS", 0.f, 0.f, 0.f) });
                poses.push_back({ "Bip01 Spine",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_SPINE", 0.f, 0.f, 0.f) });
                poses.push_back({ "Bip01 Spine1",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_SPINE1", 0.f, 0.f, 0.f) });
                poses.push_back({ "Bip01 Spine2",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_SPINE2", 0.f, 0.f, 0.f) });
                poses.push_back({ "Bip01 L Thigh",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_L_THIGH", -82.f, 0.f, 0.f) });
                poses.push_back({ "Bip01 R Thigh",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_R_THIGH", -82.f, 0.f, 0.f) });
                poses.push_back({ "Bip01 L Calf",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_L_CALF", 78.f, 0.f, 0.f) });
                poses.push_back({ "Bip01 R Calf",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_R_CALF", 78.f, 0.f, 0.f) });
                poses.push_back({ "Bip01 L Foot",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_L_FOOT", -10.f, 0.f, 0.f) });
                poses.push_back({ "Bip01 R Foot",
                    makeFalloutProofDialoguePoseRotationFromEnv("OPENMW_FNV_PROOF_POSE_R_FOOT", -10.f, 0.f, 0.f) });
            }

            unsigned int applied = 0;
            unsigned int afterUpdateBone = 0;
            for (const BonePose& pose : poses)
            {
                osg::Group* node = findBestAttachmentNode(nodeMap, { pose.mName });
                osg::MatrixTransform* matrix = dynamic_cast<osg::MatrixTransform*>(node);
                if (matrix == nullptr)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: dialogue pose missing bone=" << pose.mName
                                        << " for " << traits.mEditorId;
                    continue;
                }

                if (addFalloutProofDialogueBonePose(*matrix, pose.mName, pose.mRotation))
                    ++afterUpdateBone;
                ++applied;
            }

            Log(Debug::Info) << "FNV/ESM4 proof: dialogue pose installed for " << traits.mEditorId
                             << " bones=" << applied << " afterUpdateBone=" << afterUpdateBone;
        }

        std::string getDiffuseTextureName(const osg::StateSet* stateSet)
        {
            if (stateSet == nullptr)
                return {};

            const osg::Texture2D* texture = dynamic_cast<const osg::Texture2D*>(
                stateSet->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
            if (texture == nullptr || texture->getImage() == nullptr)
                return {};

            std::string name = texture->getImage()->getFileName();
            Misc::StringUtils::lowerCaseInPlace(name);
            return name;
        }

        std::string getFalloutCullFaceMode(const osg::StateSet* stateSet)
        {
            if (stateSet == nullptr)
                return "inherit";

            const osg::StateAttribute::GLModeValue value = stateSet->getMode(GL_CULL_FACE);
            if ((value & osg::StateAttribute::ON) != 0)
                return "on";
            if ((value & osg::StateAttribute::OFF) != 0)
                return "off";
            return "inherit";
        }

        std::string getFalloutGlModeSummary(const osg::StateSet* stateSet, GLenum mode)
        {
            if (stateSet == nullptr)
                return "inherit";

            const osg::StateAttribute::GLModeValue value = stateSet->getMode(mode);
            if ((value & osg::StateAttribute::ON) != 0)
                return "on";
            if ((value & osg::StateAttribute::OFF) != 0)
                return "off";
            return "inherit";
        }

        std::string getFalloutMaterialSummary(const osg::StateSet* stateSet)
        {
            const osg::Material* material = stateSet == nullptr ? nullptr
                : dynamic_cast<const osg::Material*>(stateSet->getAttribute(osg::StateAttribute::MATERIAL));
            if (material == nullptr)
                return "none";

            const osg::Vec4 diffuse = material->getDiffuse(osg::Material::FRONT);
            const osg::Vec4 ambient = material->getAmbient(osg::Material::FRONT);
            const osg::Vec4 emission = material->getEmission(osg::Material::FRONT);
            std::ostringstream stream;
            stream << "diffuse=(" << diffuse.r() << "," << diffuse.g() << "," << diffuse.b() << "," << diffuse.a()
                   << ") ambient=(" << ambient.r() << "," << ambient.g() << "," << ambient.b() << ","
                   << ambient.a() << ") emission=(" << emission.r() << "," << emission.g() << "," << emission.b()
                   << "," << emission.a() << ")";
            return stream.str();
        }

        std::string getFalloutStateSetSummary(const osg::StateSet* stateSet)
        {
            if (stateSet == nullptr)
                return "none";

            std::ostringstream stream;
            stream << "texture=" << getDiffuseTextureName(stateSet)
                   << ",cull=" << getFalloutGlModeSummary(stateSet, GL_CULL_FACE)
                   << ",blend=" << getFalloutGlModeSummary(stateSet, GL_BLEND)
                   << ",depth=" << getFalloutGlModeSummary(stateSet, GL_DEPTH_TEST)
                   << ",material={" << getFalloutMaterialSummary(stateSet) << "}";
            return stream.str();
        }

        std::string getFalloutGeometryColorSummary(const osg::Geometry* geometry)
        {
            if (geometry == nullptr || geometry->getColorArray() == nullptr)
                return "none";

            std::ostringstream stream;
            stream << "size=" << geometry->getColorArray()->getNumElements()
                   << ",binding=" << static_cast<int>(geometry->getColorBinding());

            if (const osg::Vec4Array* colors = dynamic_cast<const osg::Vec4Array*>(geometry->getColorArray()))
            {
                if (!colors->empty())
                {
                    osg::Vec3f rgbMin((*colors)[0].r(), (*colors)[0].g(), (*colors)[0].b());
                    osg::Vec3f rgbMax = rgbMin;
                    float alphaMin = (*colors)[0].a();
                    float alphaMax = (*colors)[0].a();
                    for (const osg::Vec4f& color : *colors)
                    {
                        rgbMin.x() = std::min(rgbMin.x(), color.r());
                        rgbMin.y() = std::min(rgbMin.y(), color.g());
                        rgbMin.z() = std::min(rgbMin.z(), color.b());
                        rgbMax.x() = std::max(rgbMax.x(), color.r());
                        rgbMax.y() = std::max(rgbMax.y(), color.g());
                        rgbMax.z() = std::max(rgbMax.z(), color.b());
                        alphaMin = std::min(alphaMin, color.a());
                        alphaMax = std::max(alphaMax, color.a());
                    }
                    stream << ",rgbMin=(" << rgbMin.x() << "," << rgbMin.y() << "," << rgbMin.z()
                           << "),rgbMax=(" << rgbMax.x() << "," << rgbMax.y() << "," << rgbMax.z()
                           << "),alpha=(" << alphaMin << "," << alphaMax << ")";
                }
            }
            else if (const osg::Vec4ubArray* byteColors
                = dynamic_cast<const osg::Vec4ubArray*>(geometry->getColorArray()))
            {
                if (!byteColors->empty())
                {
                    unsigned int rMin = (*byteColors)[0].r();
                    unsigned int gMin = (*byteColors)[0].g();
                    unsigned int bMin = (*byteColors)[0].b();
                    unsigned int rMax = rMin;
                    unsigned int gMax = gMin;
                    unsigned int bMax = bMin;
                    unsigned int alphaMin = (*byteColors)[0].a();
                    unsigned int alphaMax = (*byteColors)[0].a();
                    for (const osg::Vec4ub& color : *byteColors)
                    {
                        rMin = std::min<unsigned int>(rMin, color.r());
                        gMin = std::min<unsigned int>(gMin, color.g());
                        bMin = std::min<unsigned int>(bMin, color.b());
                        rMax = std::max<unsigned int>(rMax, color.r());
                        gMax = std::max<unsigned int>(gMax, color.g());
                        bMax = std::max<unsigned int>(bMax, color.b());
                        alphaMin = std::min<unsigned int>(alphaMin, color.a());
                        alphaMax = std::max<unsigned int>(alphaMax, color.a());
                    }
                    stream << ",rgbMin=(" << rMin << "," << gMin << "," << bMin << "),rgbMax=(" << rMax
                           << "," << gMax << "," << bMax << "),alpha=(" << alphaMin << "," << alphaMax << ")";
                }
            }
            return stream.str();
        }

        unsigned int getFalloutGeometryVertexCount(const osg::Geometry* geometry)
        {
            if (geometry == nullptr || geometry->getVertexArray() == nullptr)
                return 0;

            return geometry->getVertexArray()->getNumElements();
        }

        class FalloutFaceDrawableAuditVisitor : public osg::NodeVisitor
        {
        public:
            FalloutFaceDrawableAuditVisitor(std::string_view model, const MWWorld::Ptr& ptr, std::string_view phase)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mModel(model)
                , mPhase(phase)
            {
                std::ostringstream stream;
                stream << ptr.getCellRef().getRefId();
                mRefId = stream.str();
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        auditDrawable(*drawable, &geode);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { auditDrawable(drawable, nullptr); }

        private:
            void auditDrawable(osg::Drawable& drawable, osg::Geode* geode)
            {
                osg::Geometry* geometry = dynamic_cast<osg::Geometry*>(&drawable);
                SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable);
                osg::Geometry* sourceGeometry = rig == nullptr ? nullptr : rig->getSourceGeometry();
                osg::Geometry* renderGeometry = rig == nullptr ? geometry : nullptr;

                const osg::BoundingBox renderBox = renderGeometry == nullptr ? osg::BoundingBox()
                                                                             : renderGeometry->getBoundingBox();
                const osg::BoundingBox sourceBox = sourceGeometry == nullptr ? osg::BoundingBox()
                                                                             : sourceGeometry->getBoundingBox();
                const osg::Vec3f renderExtent = boundingBoxExtent(renderBox);
                const osg::Vec3f sourceExtent = boundingBoxExtent(sourceBox);

                std::string texture = getDiffuseTextureName(drawable.getStateSet());
                if (texture.empty() && sourceGeometry != nullptr)
                    texture = getDiffuseTextureName(sourceGeometry->getStateSet());
                if (texture.empty() && geode != nullptr)
                    texture = getDiffuseTextureName(geode->getStateSet());

                Log(Debug::Info) << "FNV/ESM4 FACE DRAWABLE AUDIT " << mRefId << " model=" << mModel
                                 << " phase=" << mPhase
                                 << " drawable='" << drawable.getName() << "' kind="
                                 << (rig != nullptr ? "SceneUtil::RigGeometry" : geometry != nullptr ? "osg::Geometry" : "other")
                                 << " nodeMask=0x" << std::hex << drawable.getNodeMask() << std::dec
                                 << " cullingActive=" << drawable.getCullingActive()
                                 << " drawableVertices=" << getFalloutGeometryVertexCount(geometry)
                                 << " sourceName='"
                                 << (sourceGeometry == nullptr ? std::string() : sourceGeometry->getName()) << "'"
                                 << " sourceVertices=" << getFalloutGeometryVertexCount(sourceGeometry)
                                 << " sourceMask=0x" << std::hex
                                 << (sourceGeometry == nullptr ? 0u : sourceGeometry->getNodeMask()) << std::dec
                                 << " renderName='"
                                 << (renderGeometry == nullptr ? std::string() : renderGeometry->getName()) << "'"
                                 << " renderVertices=" << getFalloutGeometryVertexCount(renderGeometry)
                                 << " renderMask=0x" << std::hex
                                 << (renderGeometry == nullptr ? 0u : renderGeometry->getNodeMask()) << std::dec
                                 << " drawableCull=" << getFalloutCullFaceMode(drawable.getStateSet())
                                 << " sourceCull="
                                 << (sourceGeometry == nullptr ? std::string("none")
                                                               : getFalloutCullFaceMode(sourceGeometry->getStateSet()))
                                 << " geodeCull=" << (geode == nullptr ? std::string("none")
                                                                       : getFalloutCullFaceMode(geode->getStateSet()))
                                 << " texture=" << (texture.empty() ? std::string("<none>") : texture)
                                 << " material=" << getFalloutMaterialSummary(drawable.getStateSet())
                                 << " drawableColors=" << getFalloutGeometryColorSummary(geometry)
                                 << " sourceColors=" << getFalloutGeometryColorSummary(sourceGeometry)
                                 << " renderColors=" << getFalloutGeometryColorSummary(renderGeometry)
                                 << " drawableState={" << getFalloutStateSetSummary(drawable.getStateSet()) << "}"
                                 << " sourceState={"
                                 << (sourceGeometry == nullptr ? std::string("none")
                                                               : getFalloutStateSetSummary(sourceGeometry->getStateSet()))
                                 << "}"
                                 << " renderState={"
                                 << (renderGeometry == nullptr ? std::string("none")
                                                               : getFalloutStateSetSummary(renderGeometry->getStateSet()))
                                 << "}"
                                 << " geodeState={"
                                 << (geode == nullptr ? std::string("none") : getFalloutStateSetSummary(geode->getStateSet()))
                                 << "}"
                                 << " renderValid=" << renderBox.valid()
                                 << " renderCenter=(" << (renderBox.valid() ? renderBox.center().x() : 0.f) << ","
                                 << (renderBox.valid() ? renderBox.center().y() : 0.f) << ","
                                 << (renderBox.valid() ? renderBox.center().z() : 0.f) << ")"
                                 << " renderExtent=(" << renderExtent.x() << "," << renderExtent.y() << ","
                                 << renderExtent.z() << ")"
                                 << " sourceValid=" << sourceBox.valid()
                                 << " sourceCenter=(" << (sourceBox.valid() ? sourceBox.center().x() : 0.f) << ","
                                 << (sourceBox.valid() ? sourceBox.center().y() : 0.f) << ","
                                 << (sourceBox.valid() ? sourceBox.center().z() : 0.f) << ")"
                                 << " sourceExtent=(" << sourceExtent.x() << "," << sourceExtent.y() << ","
                                 << sourceExtent.z() << ")";
            }

            std::string mModel;
            std::string mPhase;
            std::string mRefId;
        };

        void logFalloutFaceDrawableAudit(
            osg::Node* attached, std::string_view model, const MWWorld::Ptr& ptr, std::string_view phase = "insert")
        {
            if (attached == nullptr || std::getenv("OPENMW_FNV_PART_MATRIX_AUDIT") == nullptr
                || !isFalloutHeadRelativeModel(model))
                return;

            FalloutFaceDrawableAuditVisitor visitor(model, ptr, phase);
            attached->accept(visitor);
        }

        bool looksLikeFalloutSkinTexture(std::string_view texture)
        {
            return containsAny(texture,
                { "characters/_male/", "characters\\_male\\", "characters/body", "characters\\body",
                    "characters/head", "characters\\head", "bodymods/", "bodymods\\", "facemods/", "facemods\\",
                    "upperbody", "bodymale", "bodyfemale", "handmale", "handfemale", "raider" });
        }

        bool looksLikeFalloutExposedSkinShape(std::string_view name)
        {
            return containsAny(name, { " lefthand", " righthand", "left hand", "right hand", "arms", "forearm",
                "upperarm", "neck", "skin" });
        }

        class FalloutEquipmentSkinTextureVisitor : public osg::NodeVisitor
        {
        public:
            FalloutEquipmentSkinTextureVisitor(Resource::ResourceSystem* resourceSystem, std::string_view bodyTexture,
                std::string_view faceTexture)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mResourceSystem(resourceSystem)
                , mBodyTexture(bodyTexture)
                , mFaceTexture(faceTexture)
            {
            }

            void apply(osg::Drawable& drawable) override
            {
                std::string name = drawable.getName();
                for (unsigned int i = 0; i < drawable.getNumParents(); ++i)
                {
                    if (osg::Node* parent = drawable.getParent(i))
                    {
                        name += " ";
                        name += parent->getName();
                    }
                }

                osg::StateSet* sourceStateSet = nullptr;
                if (const SceneUtil::RigGeometry* rig = dynamic_cast<const SceneUtil::RigGeometry*>(&drawable))
                {
                    osg::ref_ptr<osg::Geometry> source = rig->getSourceGeometry();
                    if (source != nullptr)
                    {
                        name += " ";
                        name += source->getName();
                        sourceStateSet = source->getStateSet();
                    }
                }
                Misc::StringUtils::lowerCaseInPlace(name);
                if (name.find("meatcap") != std::string::npos)
                    return;

                std::string textureName = getDiffuseTextureName(drawable.getStateSet());
                if (textureName.empty())
                    textureName = getDiffuseTextureName(sourceStateSet);
                for (unsigned int i = 0; textureName.empty() && i < drawable.getNumParents(); ++i)
                    textureName = getDiffuseTextureName(drawable.getParent(i)->getStateSet());

                const bool skinTexture = looksLikeFalloutSkinTexture(textureName);
                const bool exposedSkinShape = looksLikeFalloutExposedSkinShape(name);
                if (!skinTexture && !exposedSkinShape)
                    return;

                const bool faceSurface = name.find("head") != std::string::npos
                    || textureName.find("facemods") != std::string::npos
                    || textureName.find("characters/head") != std::string::npos
                    || textureName.find("characters\\head") != std::string::npos;
                const std::string_view texture = faceSurface && !mFaceTexture.empty() ? mFaceTexture : mBodyTexture;
                if (texture.empty())
                    return;

                overrideTextureSlot(texture, "diffuseMap", 0, mResourceSystem, *drawable.getOrCreateStateSet());
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* source = rig->getSourceGeometry())
                        overrideTextureSlot(texture, "diffuseMap", 0, mResourceSystem, *source->getOrCreateStateSet());
                    for (unsigned int i = 0; i < 2; ++i)
                    {
                        if (osg::Geometry* geometry = rig->getRenderGeometry(i))
                            overrideTextureSlot(
                                texture, "diffuseMap", 0, mResourceSystem, *geometry->getOrCreateStateSet());
                    }
                }
                ++mOverridden;
            }

            unsigned int mOverridden = 0;

        private:
            Resource::ResourceSystem* mResourceSystem;
            std::string_view mBodyTexture;
            std::string_view mFaceTexture;
        };

        void forceFalloutActorPartVisible(osg::Node* attached, std::string_view model, const ESM4::Npc& traits)
        {
            if (attached == nullptr)
                return;

            HideFalloutHiddenMorphVisitor hideVisitor;
            attached->accept(hideVisitor);
            if (hideVisitor.mHidden > 0)
                Log(Debug::Info) << "FNV/ESM4 diag: hid " << hideVisitor.mHidden
                                 << " Fallout hidden morph drawable(s) on " << model << " for " << traits.mEditorId;

            ForceFalloutActorPartVisibleVisitor visitor;
            attached->accept(visitor);
            if (visitor.mRigGeometryCount > 0)
                Log(Debug::Info) << "FNV/ESM4 diag: forced render mask on " << visitor.mRigGeometryCount
                                 << " rigged drawable(s) for " << model << " on " << traits.mEditorId;
        }

        void neutralizeFalloutSkinMaterial(osg::Node* attached, std::string_view model, const ESM4::Npc& traits)
        {
            if (attached == nullptr)
                return;

            TintMaterialVisitor visitor(osg::Vec4f(1.f, 1.f, 1.f, 1.f));
            attached->accept(visitor);
            Log(Debug::Info) << "FNV/ESM4 diag: neutralized skin material color on " << model << " for "
                             << traits.mEditorId << " vertexColorArrays=" << visitor.mNeutralizedVertexColorArrays;
        }

        void forceFalloutSkinOpaqueNoBlend(osg::Node* attached, std::string_view model, const ESM4::Npc& traits)
        {
            if (attached == nullptr)
                return;

            ForceOpaqueNoBlendVisitor opaque;
            attached->accept(opaque);
            Log(Debug::Info) << "FNV/ESM4 diag: forced opaque no-blend skin surface " << model << " states="
                             << opaque.getAppliedCount() << " for " << traits.mEditorId;
        }

        void tintFalloutSkinMaterial(
            osg::Node* attached, std::string_view model, const ESM4::Npc& traits, const osg::Vec4f& tint)
        {
            if (attached == nullptr)
                return;

            TintMaterialVisitor visitor(tint);
            attached->accept(visitor);
            Log(Debug::Info) << "FNV/ESM4 diag: applied skin material tint (" << tint.x() << ", " << tint.y()
                             << ", " << tint.z() << ") on " << model << " for " << traits.mEditorId
                             << " vertexColorArrays=" << visitor.mNeutralizedVertexColorArrays;
        }

        bool applyFaceGenEgtTint(Resource::ResourceSystem* resourceSystem, osg::Node* attached, std::string_view model,
            const ESM4::Npc& traits)
        {
            if (attached == nullptr)
                return false;

            const std::shared_ptr<const FaceGenEgt> egt = loadFaceGenEgt(resourceSystem, model);
            if (!egt)
                return false;

            const bool egtMaterialTintDisabled = std::getenv("OPENMW_FNV_DISABLE_EGT_MATERIAL_TINT") != nullptr;
            if (egtMaterialTintDisabled || std::getenv("OPENMW_FNV_USE_EGT_MATERIAL_TINT") == nullptr)
            {
                Log(Debug::Info) << "FNV/ESM4 diag: loaded FaceGen EGT complexion " << model << " size="
                                 << egt->mWidth << "x" << egt->mHeight << " modes=" << egt->mTextureModeCount
                                 << " for " << traits.mEditorId
                                 << " runtime=loaded-pending-exact-facegen-texture-synthesis"
                                 << " optIn=OPENMW_FNV_USE_EGT_MATERIAL_TINT"
                                 << " optOut=OPENMW_FNV_DISABLE_EGT_MATERIAL_TINT disabled="
                                 << egtMaterialTintDisabled;
                return false;
            }

            const osg::Vec4f tint = deriveFaceGenEgtMaterialTint(*egt, traits);
            tintFalloutSkinMaterial(attached, model, traits, tint);
            const auto [textureNonZero, textureTotal] = summarizeCoefficients(traits.mSymTextureModeCoefficients);
            Log(Debug::Info) << "FNV/ESM4 diag: applied FaceGen EGT complexion " << model << " size=" << egt->mWidth
                             << "x" << egt->mHeight << " modes=" << egt->mTextureModeCount << " textureCoeffs="
                             << textureNonZero << "/" << traits.mSymTextureModeCoefficients.size()
                             << " sumAbs=" << textureTotal << " tintLayers=" << traits.mTintLayers.size()
                             << " materialTint=(" << tint.x() << ", " << tint.y() << ", " << tint.z() << ") for "
                             << traits.mEditorId << " runtime=runtime-fnv-egt-material-tint-applied"
                             << " optOut=OPENMW_FNV_DISABLE_EGT_MATERIAL_TINT";
            return true;
        }

        void validateFaceGenCtlBasis(const FaceGenCtl* ctl, const ESM4::Npc& traits)
        {
            if (ctl == nullptr)
                return;

            const auto matches = [](std::size_t actual, std::uint32_t expected) {
                return actual == 0 || actual == static_cast<std::size_t>(expected);
            };
            const bool shapeOk = matches(traits.mSymShapeModeCoefficients.size(), ctl->mSymmetricShapeModeCount);
            const bool asymOk = matches(traits.mAsymShapeModeCoefficients.size(), ctl->mAsymmetricShapeModeCount);
            const bool textureOk = matches(traits.mSymTextureModeCoefficients.size(), ctl->mTextureModeCount);
            if (shapeOk && asymOk && textureOk)
            {
                Log(Debug::Info) << "FNV/ESM4 diag: FaceGen CTL basis validated for " << traits.mEditorId
                                 << " basis=" << ctl->mBasisVersion << " shape="
                                 << traits.mSymShapeModeCoefficients.size() << "/" << ctl->mSymmetricShapeModeCount
                                 << " asym=" << traits.mAsymShapeModeCoefficients.size() << "/"
                                 << ctl->mAsymmetricShapeModeCount << " texture="
                                 << traits.mSymTextureModeCoefficients.size() << "/" << ctl->mTextureModeCount;
                return;
            }

            Log(Debug::Warning) << "FNV/ESM4 diag: FaceGen CTL basis mismatch for " << traits.mEditorId
                                << " basis=" << ctl->mBasisVersion << " shape="
                                << traits.mSymShapeModeCoefficients.size() << "/" << ctl->mSymmetricShapeModeCount
                                << " asym=" << traits.mAsymShapeModeCoefficients.size() << "/"
                                << ctl->mAsymmetricShapeModeCount << " texture="
                                << traits.mSymTextureModeCoefficients.size() << "/" << ctl->mTextureModeCount;
        }

        void overrideFalloutEquipmentSkinTextures(osg::Node* attached, std::string_view model, const ESM4::Npc& traits,
            Resource::ResourceSystem* resourceSystem, std::string_view bodyTexture, std::string_view faceTexture)
        {
            if (attached == nullptr || (bodyTexture.empty() && faceTexture.empty()))
                return;

            FalloutEquipmentSkinTextureVisitor visitor(resourceSystem, bodyTexture, faceTexture);
            attached->accept(visitor);
            if (visitor.mOverridden > 0)
            {
                Log(Debug::Info) << "FNV/ESM4 diag: overrode " << visitor.mOverridden
                                 << " equipment skin drawable texture(s) on " << model << " for "
                                 << traits.mEditorId;
                neutralizeFalloutSkinMaterial(attached, model, traits);
            }
        }

        std::string formatFalloutWeaponSoundRefs(const ESM4::Weapon& weapon)
        {
            std::ostringstream stream;
            for (std::size_t i = 0; i < weapon.mSoundRefs.size(); ++i)
            {
                if (i != 0)
                    stream << ",";
                stream << ESM::printName(weapon.mSoundRefs[i].mType) << "=" << ESM::RefId(weapon.mSoundRefs[i].mSound);
            }
            return stream.str();
        }

        std::string resolveFalloutSoundFile(
            const MWWorld::ESMStore& store, ESM::FormId soundId, unsigned int depth = 0)
        {
            constexpr unsigned int maxDepth = 8;
            if (soundId.isZeroOrUnset() || depth >= maxDepth)
                return {};

            if (const ESM4::SoundReference* reference = store.get<ESM4::SoundReference>().search(soundId))
            {
                if (!reference->mSoundFile.empty())
                    return reference->mSoundFile;
                if (!reference->mSoundId.isZeroOrUnset())
                    return resolveFalloutSoundFile(store, reference->mSoundId, depth + 1);
            }

            if (const ESM4::Sound* sound = store.get<ESM4::Sound>().search(soundId))
                return sound->mSoundFile;

            return {};
        }

        std::string formatFalloutWeaponSoundFiles(const ESM4::Weapon& weapon, const MWWorld::ESMStore& store)
        {
            std::ostringstream stream;
            for (std::size_t i = 0; i < weapon.mSoundRefs.size(); ++i)
            {
                if (i != 0)
                    stream << ",";
                const ESM4::Weapon::SoundRef& ref = weapon.mSoundRefs[i];
                const std::string file = resolveFalloutSoundFile(store, ref.mSound);
                stream << ESM::printName(ref.mType) << "=" << ESM::RefId(ref.mSound) << ":"
                       << (file.empty() ? std::string_view("<unresolved>") : std::string_view(file));
            }
            return stream.str();
        }

        std::string getFonvWeaponIdlePoseKf(const ESM4::Weapon* weapon)
        {
            if (weapon == nullptr)
                return "meshes/characters/_male/idleanims/talk_handsatside_still2.kf";

            std::string label = weapon->mEditorId + " " + weapon->mModel;
            Misc::StringUtils::lowerCaseInPlace(label);
            if (containsAny(label, { "rifle", "shotgun", "sniper", "launcher", "2hand", "2hr", "varmint" }))
                return "meshes/characters/_male/idleanims/2hrloiter.kf";
            if (containsAny(label, { "pistol", "revolver", "357", "10mm", "9mm", "1hand", "1hp" }))
                return "meshes/characters/_male/idleanims/dlcanch1hpistolpose.kf";

            return "meshes/characters/_male/idleanims/talk_handsatside_still2.kf";
        }

        std::string normalizeFonvAnimationPath(std::string path)
        {
            if (path.empty())
                return {};

            VFS::Path::normalizeFilenameInPlace(path);
            if (path.rfind("meshes/", 0) != 0)
                path = "meshes/" + path;

            if (!Misc::StringUtils::ciEndsWith(path, ".kf"))
                return {};

            return path;
        }

        std::vector<std::string> collectFonvIdleAnimationSources(
            const MWWorld::ESMStore& store, Resource::ResourceSystem* resourceSystem, const ESM4::Npc& traits,
            const std::vector<ESM::FormId>& packageIds)
        {
            std::vector<std::string> result;
            const auto& packageStore = store.get<ESM4::AIPackage>();
            const auto& idleStore = store.get<ESM4::IdleAnimation>();
            const auto& markerStore = store.get<ESM4::IdleMarker>();

            static bool loggedStoreSummary = false;
            if (!loggedStoreSummary)
            {
                Log(Debug::Info) << "FNV/ESM4 diag: loaded PACK records=" << packageStore.getSize()
                                 << " IDLE records=" << idleStore.getSize()
                                 << " IDLM records=" << markerStore.getSize();

                unsigned int loggedMarkers = 0;
                for (std::size_t i = 0; i < markerStore.getSize() && loggedMarkers < 8; ++i)
                {
                    const ESM4::IdleMarker* marker = markerStore.at(i);
                    if (marker == nullptr || marker->mIdleAnim.empty())
                        continue;

                    std::ostringstream ids;
                    for (std::size_t j = 0; j < marker->mIdleAnim.size() && j < 8; ++j)
                    {
                        if (j != 0)
                            ids << ",";
                        ids << ESM::RefId(marker->mIdleAnim[j]);
                    }

                    Log(Debug::Info) << "FNV/ESM4 diag: IDLM " << marker->mEditorId << " count="
                                     << marker->mIdleAnim.size() << " ids=" << ids.str();
                    ++loggedMarkers;
                }
                loggedStoreSummary = true;
            }

            const VFS::Manager* vfs = resourceSystem->getVFS();
            const auto addIdle = [&](const ESM4::IdleAnimation& idle, std::string_view source) {
                const std::string path = normalizeFonvAnimationPath(idle.mModel);
                if (path.empty() || !vfs->exists(VFS::Path::Normalized(path)))
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: package-selected IDLE " << idle.mEditorId << " from "
                                     << source << " has missing/non-KF model=" << idle.mModel << " for "
                                     << traits.mEditorId;
                    return;
                }

                if (std::find(result.begin(), result.end(), path) != result.end())
                    return;

                Log(Debug::Info) << "FNV/ESM4 diag: package-selected IDLE source " << idle.mEditorId << " ("
                                 << ESM::RefId(idle.mId) << ") from " << source << " model=" << path << " for "
                                 << traits.mEditorId;
                result.push_back(path);
            };

            const auto addIdleId = [&](const auto& self, ESM::FormId idleId, std::string_view source,
                                       unsigned int depth) -> void {
                if (idleId.isZeroOrUnset() || depth >= 4 || result.size() >= 32)
                    return;

                if (const ESM4::IdleAnimation* idle = idleStore.search(idleId))
                {
                    addIdle(*idle, source);
                    return;
                }

                if (const ESM4::IdleMarker* marker = markerStore.search(idleId))
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: expanding package-selected IDLM " << marker->mEditorId
                                     << " (" << ESM::RefId(marker->mId) << ") count=" << marker->mIdleAnim.size()
                                     << " from " << source << " for " << traits.mEditorId;
                    for (ESM::FormId markerIdleId : marker->mIdleAnim)
                        self(self, markerIdleId, marker->mEditorId, depth + 1);
                    return;
                }

                Log(Debug::Info) << "FNV/ESM4 diag: unresolved package idle id " << ESM::RefId(idleId) << " from "
                                 << source << " for " << traits.mEditorId;
            };

            if (packageIds.empty())
                Log(Debug::Info) << "FNV/ESM4 diag: no AI package IDs available for authored idle lookup on "
                                 << traits.mEditorId;

            for (ESM::FormId packageId : packageIds)
            {
                if (result.size() >= 32)
                    break;
                const ESM4::AIPackage* package = packageStore.search(packageId);
                if (package == nullptr)
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: AI package " << ESM::RefId(packageId)
                                     << " not loaded for authored idle lookup on " << traits.mEditorId;
                    continue;
                }

                Log(Debug::Info) << "FNV/ESM4 diag: evaluating AI package " << package->mEditorId << " ("
                                 << ESM::RefId(package->mId) << ") " << formatPackageSummary(*package, &store)
                                 << " conditionCount=" << package->mConditions.size()
                                 << " idleCount=" << package->mIdleAnim.size() << " for " << traits.mEditorId;
                for (std::size_t i = 0; i < package->mExtraLocations.size(); ++i)
                    Log(Debug::Info) << "FNV/ESM4 diag: AI package " << package->mEditorId << " extraLoc[" << i
                                     << "]={" << formatPackageLocation(package->mExtraLocations[i], &store) << "} for "
                                     << traits.mEditorId;
                for (std::size_t i = 0; i < package->mExtraTargets.size(); ++i)
                {
                    const float extra = i < package->mExtraTargetUnknowns.size() ? package->mExtraTargetUnknowns[i] : 0.f;
                    Log(Debug::Info) << "FNV/ESM4 diag: AI package " << package->mEditorId << " extraTarget[" << i
                                     << "]={" << formatPackageTarget(package->mExtraTargets[i], extra, &store)
                                     << "} for " << traits.mEditorId;
                }
                for (ESM::FormId idleId : package->mIdleAnim)
                {
                    if (result.size() >= 32)
                        break;
                    addIdleId(addIdleId, idleId, package->mEditorId, 0);
                }
            }

            return result;
        }

        bool shouldDisableFalloutPackageProcedureAnimationSources()
        {
            return std::getenv("OPENMW_FNV_DISABLE_PACKAGE_PROCEDURE_IDLES") != nullptr;
        }

        bool fonvPackageHasExplicitTime(const ESM4::AIPackage& package)
        {
            return package.mSchedule.time != 0xff;
        }

        bool fonvPackageHasUnevaluatedConditions(const ESM4::AIPackage& package)
        {
            return !package.mConditions.empty();
        }

        bool fonvPackageCoversHour(const ESM4::AIPackage& package, float hour)
        {
            if (package.mSchedule.time == 0xff)
                return false;

            const float start = static_cast<float>(package.mSchedule.time);
            const float duration = package.mSchedule.duration == 0
                ? 24.f
                : static_cast<float>(std::min<std::uint32_t>(package.mSchedule.duration, 24));
            const float end = std::fmod(start + duration, 24.f);
            if (duration >= 24.f)
                return true;
            if (start <= end)
                return hour >= start && hour < end;
            return hour >= start || hour < end;
        }

        float getFonvPackageProcedureHour(bool& usedHourOverride)
        {
            usedHourOverride = false;
            float hour = 0.f;
            if (MWBase::Environment::get().getWorld() != nullptr)
                hour = MWBase::Environment::get().getWorld()->getTimeStamp().getHour();
            if (const char* env = std::getenv("OPENMW_FNV_PROCEDURE_HOUR"))
            {
                char* end = nullptr;
                const float overrideHour = std::strtof(env, &end);
                if (end != env && std::isfinite(overrideHour))
                {
                    hour = std::fmod(std::max(0.f, overrideHour), 24.f);
                    usedHourOverride = true;
                }
            }
            return hour;
        }

        const ESM4::Reference* resolvePackageReference(
            const MWWorld::ESMStore& store, const ESM4::AIPackage::PLDT& location)
        {
            if (location.type != 0 && location.type != 4)
                return nullptr;
            return store.get<ESM4::Reference>().search(formIdFromRaw(location.location));
        }

        bool fonvPackageLocationClearlyExcludesActor(
            const MWWorld::Ptr& ptr, const MWWorld::ESMStore& store, const ESM4::AIPackage& package)
        {
            if (ptr.getCell() == nullptr || ptr.getCell()->getCell() == nullptr)
                return false;

            const ESM::RefId& currentCellId = ptr.getCell()->getCell()->getId();
            switch (package.mLocation.type)
            {
                case 0: // Near reference.
                case 4: // Object id.
                {
                    const ESM4::Reference* ref = resolvePackageReference(store, package.mLocation);
                    return ref != nullptr && !ref->mParent.empty() && ref->mParent != currentCellId;
                }
                case 1: // In cell.
                    return package.mLocation.location != 0
                        && ESM::RefId(formIdFromRaw(package.mLocation.location)) != currentCellId;
                default:
                    return false;
            }
        }

        std::string getPackageReferenceFurnitureModel(const MWWorld::ESMStore& store, const ESM4::AIPackage& package)
        {
            const ESM4::Reference* ref = resolvePackageReference(store, package.mLocation);
            if (ref == nullptr)
                return {};

            if (const ESM4::Furniture* furniture = store.get<ESM4::Furniture>().search(ref->mBaseObj))
                return furniture->mModel;

            return {};
        }

        void logFonvPackageProcedurePlacement(
            const MWWorld::Ptr& ptr, const ESM4::Npc& traits, const MWWorld::ESMStore& store,
            const ESM4::AIPackage& package)
        {
            const ESM4::Reference* ref = resolvePackageReference(store, package.mLocation);
            if (ref == nullptr || ptr.getCell() == nullptr || ptr.getCell()->getCell() == nullptr)
                return;

            const MWWorld::Cell* currentCell = ptr.getCell()->getCell();
            const ESM::RefId& currentCellId = currentCell->getId();
            const bool sameCell = ref->mParent == currentCellId;
            Log(Debug::Info) << "FNV/ESM4 diag: package procedure placement " << package.mEditorId
                             << " targetRef=" << ref->mEditorId << " targetParent=" << ref->mParent
                             << " currentCell=" << currentCellId << " sameCell=" << sameCell << " for "
                             << traits.mEditorId;

            if (sameCell)
            {
                const ESM::Position& pos = ptr.getRefData().getPosition();
                Log(Debug::Info) << "FNV/ESM4 diag: confirmed same-cell package procedure placement "
                                 << package.mEditorId << " targetRef=" << ref->mEditorId << " refPos=("
                                 << pos.pos[0] << "," << pos.pos[1] << "," << pos.pos[2]
                                 << ") targetPos=(" << ref->mPos.pos[0] << "," << ref->mPos.pos[1] << ","
                                 << ref->mPos.pos[2] << ") rotZ=" << pos.rot[2] << " targetRotZ="
                                 << ref->mPos.rot[2] << " for " << traits.mEditorId;
            }
        }

        void addProcedureSourceIfPresent(std::vector<std::string>& result, const VFS::Manager& vfs, std::string path)
        {
            VFS::Path::normalizeFilenameInPlace(path);
            if (std::find(result.begin(), result.end(), path) != result.end())
                return;
            if (vfs.exists(VFS::Path::Normalized(path)))
                result.push_back(std::move(path));
        }

        std::vector<std::string> collectFonvPackageProcedureAnimationSources(const MWWorld::Ptr& ptr,
            const MWWorld::ESMStore& store, Resource::ResourceSystem* resourceSystem, const ESM4::Npc& traits,
            const std::vector<ESM::FormId>& packageIds)
        {
            std::vector<std::string> result;
            const auto& packageStore = store.get<ESM4::AIPackage>();
            bool usedHourOverride = false;
            const float hour = getFonvPackageProcedureHour(usedHourOverride);

            const ESM4::AIPackage* selected = nullptr;
            for (ESM::FormId packageId : packageIds)
            {
                const ESM4::AIPackage* package = packageStore.search(packageId);
                if (package == nullptr || fonvPackageHasUnevaluatedConditions(*package)
                    || !fonvPackageCoversHour(*package, hour)
                    || fonvPackageLocationClearlyExcludesActor(ptr, store, *package))
                {
                    if (package != nullptr)
                        Log(Debug::Info) << "FNV/ESM4 diag: skipped AI package " << package->mEditorId
                                         << " for " << traits.mEditorId << " hour=" << hour
                                         << " conditionBlocked=" << fonvPackageHasUnevaluatedConditions(*package)
                                         << " conditionCount=" << package->mConditions.size()
                                         << " covers=" << fonvPackageCoversHour(*package, hour)
                                         << " locExcluded="
                                         << fonvPackageLocationClearlyExcludesActor(ptr, store, *package);
                    continue;
                }

                if (selected == nullptr || (fonvPackageHasExplicitTime(*package) && !fonvPackageHasExplicitTime(*selected)))
                {
                    selected = package;
                    if (fonvPackageHasExplicitTime(*selected))
                        break;
                }
            }

            if (isEasyPeteProofActor(traits))
                Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: package procedure currentHour=" << hour
                                 << " override=" << usedHourOverride
                                 << " selected="
                                 << (selected != nullptr ? selected->mEditorId : std::string_view("<none>"));

            if (selected == nullptr)
                return result;

            logFonvPackageProcedurePlacement(ptr, traits, store, *selected);

            const VFS::Manager* vfs = resourceSystem->getVFS();
            const std::string furnitureModel = getPackageReferenceFurnitureModel(store, *selected);
            std::string lowerFurniture = furnitureModel;
            Misc::StringUtils::lowerCaseInPlace(lowerFurniture);
            const bool usesTableSeat = lowerFurniture.find("dinerbooth") != std::string::npos
                || lowerFurniture.find("table") != std::string::npos;
            const bool usesChairSeat = lowerFurniture.find("chair") != std::string::npos || usesTableSeat;

            switch (selected->mData.type)
            {
                case 3: // Eat
                    if (usesTableSeat)
                        addProcedureSourceIfPresent(
                            result, *vfs, "meshes/characters/_male/idleanims/sittablechaireata.kf");
                    else
                        addProcedureSourceIfPresent(
                            result, *vfs, "meshes/characters/_male/idleanims/sitchaireata.kf");
                    if (usesChairSeat)
                        addProcedureSourceIfPresent(
                            result, *vfs, "meshes/characters/_male/idleanims/dynamicidle_chairsit.kf");
                    addProcedureSourceIfPresent(result, *vfs, "meshes/characters/_male/idleanims/dynamicidle_sit.kf");
                    break;
                case 4: // Sleep
                    addProcedureSourceIfPresent(
                        result, *vfs, "meshes/characters/_male/idleanims/dynamicidle_sleep.kf");
                    break;
                case 6: // Travel-to-ref, used by Pete's scheduled chair packages.
                case 8: // Use item at / furniture.
                    if (!furnitureModel.empty())
                    {
                        addProcedureSourceIfPresent(
                            result, *vfs, "meshes/characters/_male/idleanims/sitchairlistena.kf");
                        addProcedureSourceIfPresent(
                            result, *vfs, "meshes/characters/_male/idleanims/sitchairtalktoplayera.kf");
                        if (usesChairSeat)
                            addProcedureSourceIfPresent(
                                result, *vfs, "meshes/characters/_male/idleanims/dynamicidle_chairsit.kf");
                        addProcedureSourceIfPresent(
                            result, *vfs, "meshes/characters/_male/idleanims/dynamicidle_sit.kf");
                    }
                    else if (const ESM4::Weapon* weapon = MWClass::ESM4Npc::getEquippedWeapon(ptr))
                        Log(Debug::Info) << "FNV/ESM4 diag: package procedure keeps neutral idle for "
                                         << traits.mEditorId << " package=" << selected->mEditorId
                                         << " type=" << getFonvPackageTypeName(selected->mData.type)
                                         << " equippedWeapon=" << weapon->mEditorId
                                         << " because no furniture/action target model was resolved";
                    break;
                default:
                    break;
            }

            for (const std::string& path : result)
                Log(Debug::Info) << "FNV/ESM4 diag: package procedure animation source " << path << " from "
                                 << selected->mEditorId << " type=" << getFonvPackageTypeName(selected->mData.type)
                                 << " furniture=" << furnitureModel << " for " << traits.mEditorId;

            return result;
        }
    }

    ESM4NpcAnimation::ESM4NpcAnimation(
        const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode, Resource::ResourceSystem* resourceSystem)
        : Animation(ptr, std::move(parentNode), resourceSystem)
    {
        const std::string skeletonModel = mPtr.getClass().getCorrectedModel(mPtr);
        setObjectRoot(skeletonModel, true, true, false);

        const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(mPtr);
        if (traits != nullptr && traits->mIsFONV)
        {
            const NodeMap& currentNodeMap = getNodeMap();
            const auto ensureHelper = [&](std::string_view name, std::initializer_list<std::string_view> parents) {
                if (findBestAttachmentNode(currentNodeMap, { name }) != nullptr)
                    return;
                osg::Group* parent = findBestAttachmentNode(currentNodeMap, parents);
                if (parent == nullptr)
                    parent = mObjectRoot.get();
                makeFalloutAttachmentHelper(*parent, name, mNodeMap);
                Log(Debug::Info) << "FNV/ESM4 diag: inserted attachment helper '" << name << "' under "
                                 << parent->getName() << " for " << traits->mEditorId;
            };

            ensureHelper("Weapon", { "Bip01 R Hand", "bip01 r hand", "Bip01", "bip01" });
            ensureHelper("Torch", { "Bip01 L Hand", "bip01 l hand" });
            ensureHelper("SideWeapon", { "Bip01 Pelvis", "bip01 pelvis", "Bip01 R Thigh", "bip01 r thigh" });
            ensureHelper("BackWeapon", { "Bip01 Spine2", "bip01 spine2", "Bip01 Spine1", "bip01 spine1" });
            ensureHelper("Quiver", { "Bip01 Spine2", "bip01 spine2", "Bip01 Spine1", "bip01 spine1" });
        }
        updateParts();

        if (traits != nullptr && traits->mIsFONV)
        {
            const ESM4::Npc* animationRecord = MWClass::ESM4Npc::getModelRecord(mPtr);
            if (animationRecord == nullptr)
                animationRecord = traits;

            bool addedAnimationSource = false;
            std::vector<std::string> procedureIdleSources;
            const auto addFonvAnimationSource = [&](const std::string& kfPath, std::string_view reason,
                                                   bool countsAsPrimary = true,
                                                   bool falloutProcedureIdle = false) {
                if (kfPath.empty())
                    return;
                Log(Debug::Info) << "FNV/ESM4 diag: adding FONV NPC " << reason << " animation source " << kfPath
                                 << " for " << traits->mEditorId;
                if (addSingleAnimSource(kfPath, skeletonModel, falloutProcedureIdle) != nullptr)
                    addedAnimationSource = addedAnimationSource || countsAsPrimary;
            };

            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store != nullptr)
            {
                const ESM4::Npc* packageRecord = MWClass::ESM4Npc::getAIPackageRecord(mPtr);
                const std::vector<ESM::FormId>& packageIds
                    = packageRecord != nullptr ? packageRecord->mAIPackages : traits->mAIPackages;
                if (isEasyPeteProofActor(*traits))
                    Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: aiPackageRecord="
                                     << (packageRecord != nullptr ? packageRecord->mEditorId : "<traits>")
                                     << " packages=[" << formatFormIdList(packageIds) << "]";
                const std::vector<std::string> authoredIdleSources
                    = collectFonvIdleAnimationSources(*store, mResourceSystem, *traits, packageIds);
                if (isEasyPeteProofActor(*traits))
                    Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: authored idle candidates="
                                     << authoredIdleSources.size();
                for (const std::string& kfPath : authoredIdleSources)
                {
                    if (isEasyPeteProofActor(*traits))
                        Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: authored idle source=" << kfPath;
                    addFonvAnimationSource(kfPath, "authored IDLE", false);
                }
                if (shouldDisableFalloutPackageProcedureAnimationSources())
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: package procedure animation sources disabled for "
                                     << traits->mEditorId
                                     << " by OPENMW_FNV_DISABLE_PACKAGE_PROCEDURE_IDLES";
                }
                else
                {
                    procedureIdleSources
                        = collectFonvPackageProcedureAnimationSources(mPtr, *store, mResourceSystem, *traits, packageIds);
                }
            }

            if (!animationRecord->mKf.empty())
            {
                std::string animationDirectory = skeletonModel;
                const std::size_t slash = animationDirectory.find_last_of("/\\");
                animationDirectory = slash == std::string::npos ? std::string() : animationDirectory.substr(0, slash + 1);

                Log(Debug::Info) << "FNV/ESM4 diag: using NPC KFFZ animation list from "
                                 << animationRecord->mEditorId << " for " << traits->mEditorId
                                 << " count=" << animationRecord->mKf.size();

                for (const std::string& kf : animationRecord->mKf)
                {
                    if (kf.empty())
                        continue;

                    const std::string kfPath = animationDirectory + kf;
                    addFonvAnimationSource(kfPath, "record");
                }
            }
            else
                Log(Debug::Warning) << "FNV/ESM4 diag: no FONV NPC KFFZ animation list for " << traits->mEditorId
                                    << " animationRecord=" << animationRecord->mEditorId;

            if (!addedAnimationSource)
            {
                const bool isFemale = MWClass::ESM4Npc::isFemale(mPtr);
                const std::string locomotionDir
                    = isFemale ? "meshes/characters/_male/locomotion/female/" : "meshes/characters/_male/locomotion/male/";

                addFonvAnimationSource("meshes/characters/_male/locomotion/mtidle.kf", "fallback");
                // Do not use the talking idle as the neutral fallback. It drives helper/twist bones that are not
                // present in the base FNV skeleton path yet and visibly mangles skinned actors.
                addFonvAnimationSource(locomotionDir + "mtforward.kf", "fallback");
                addFonvAnimationSource(locomotionDir + "mtbackward.kf", "fallback");
                addFonvAnimationSource(locomotionDir + "mtleft.kf", "fallback");
                addFonvAnimationSource(locomotionDir + "mtright.kf", "fallback");
                addFonvAnimationSource(locomotionDir + "mtfastforward.kf", "fallback");
                addFonvAnimationSource(locomotionDir + "mtfastbackward.kf", "fallback");
                addFonvAnimationSource(locomotionDir + "mtfastleft.kf", "fallback");
                addFonvAnimationSource(locomotionDir + "mtfastright.kf", "fallback");
                addFonvAnimationSource("meshes/characters/_male/locomotion/mtturnleft.kf", "fallback");
                addFonvAnimationSource("meshes/characters/_male/locomotion/mtturnright.kf", "fallback");

                // Fallout's Idle Manager selects idles by conditions and animation group. A holstered weapon in
                // inventory is not enough to force a combat/weapon pose onto ambient porch NPCs like Easy Pete.
                // Until those condition-selected idles are implemented, keep the fallback on locomotion/mtidle only.
                if (const ESM4::Weapon* weapon = MWClass::ESM4Npc::getEquippedWeapon(mPtr))
                    Log(Debug::Info) << "FNV/ESM4 diag: keeping ambient neutral idle for " << traits->mEditorId
                                     << " despite equipped weapon=" << weapon->mEditorId;
            }

            // Add scheduled package procedure sources last because Animation::play resolves sources in reverse order.
            // These are narrow candidates such as Easy Pete's chair/eat package and should beat neutral mTIdle.
            for (const std::string& kfPath : procedureIdleSources)
                addFonvAnimationSource(kfPath, "scheduled package procedure", false, true);

            if (const std::string actorKitSource = getFalloutActorKitAnimationSource(); !actorKitSource.empty())
            {
                Log(Debug::Info) << "FNV/ESM4 proof: actor-kit animation source request actor=" << traits->mEditorId
                                 << " ref=" << mPtr.getCellRef().getRefId()
                                 << " source=" << actorKitSource;
                addFonvAnimationSource(actorKitSource, "actor-kit requested", false);
            }

            requestFalloutActorKitAnimation(*this, mPtr, *traits);
            // Initial construction may already read the generated live command through updateParts(),
            // but the live authoring gate must be proven by the frame path so the selected target
            // logs runtime selector consumption and a target part rebuild instead of silently
            // accepting constructor-time state as proof.
            mLiveRuntimeActorKitFingerprint.clear();
        }
    }

    void ESM4NpcAnimation::applyLiveHeadSurfaceAuthoring()
    {
        const char* livePath = getFalloutLiveAuthoringFile();
        if (livePath == nullptr || mLiveHeadSurfaceAuthoringTargets.empty())
            return;
        if (++mLiveHeadSurfaceAuthoringTick % 6 != 0)
            return;

        std::string content;
        if (!readFalloutLiveAuthoringFile(livePath, content) || content == mLiveHeadSurfaceAuthoringContent)
            return;

        mLiveHeadSurfaceAuthoringContent = content;
        for (const LiveHeadSurfaceAuthoringTarget& target : mLiveHeadSurfaceAuthoringTargets)
        {
            osg::MatrixTransform* matrixNode = target.node.get();
            if (matrixNode == nullptr || target.prefix.empty())
                continue;

            osg::Vec3f offset = target.defaultOffset;
            osg::Vec3f rotation = target.defaultRotationDegrees;
            osg::Vec3f pivot = target.pivot;
            osg::Vec3f pivotOffset;
            bool pivotMode = target.defaultPivotMode;
            readFalloutLiveAuthoringFloat(content, target.prefix + "_OFFSET_X", offset.x());
            readFalloutLiveAuthoringFloat(content, target.prefix + "_OFFSET_Y", offset.y());
            readFalloutLiveAuthoringFloat(content, target.prefix + "_OFFSET_Z", offset.z());
            readFalloutLiveAuthoringFloat(content, target.prefix + "_ROTATION_X", rotation.x());
            readFalloutLiveAuthoringFloat(content, target.prefix + "_ROTATION_Y", rotation.y());
            readFalloutLiveAuthoringFloat(content, target.prefix + "_ROTATION_Z", rotation.z());
            readFalloutLiveAuthoringFloat(content, target.prefix + "_PIVOT_X", pivot.x());
            readFalloutLiveAuthoringFloat(content, target.prefix + "_PIVOT_Y", pivot.y());
            readFalloutLiveAuthoringFloat(content, target.prefix + "_PIVOT_Z", pivot.z());
            readFalloutLiveAuthoringFloat(content, target.prefix + "_PIVOT_OFFSET_X", pivotOffset.x());
            readFalloutLiveAuthoringFloat(content, target.prefix + "_PIVOT_OFFSET_Y", pivotOffset.y());
            readFalloutLiveAuthoringFloat(content, target.prefix + "_PIVOT_OFFSET_Z", pivotOffset.z());
            pivot += pivotOffset;
            readFalloutLiveAuthoringBool(content, target.prefix + "_PIVOT_MODE", pivotMode);

            matrixNode->setMatrix(
                makeFalloutHeadSurfaceMatrix(offset, makeFalloutEulerAttitude(rotation), pivot, pivotMode));
            matrixNode->dirtyBound();
            Log(Debug::Info) << "FNV/ESM4 live authoring: frame-applied head surface authoring model="
                             << target.model << " prefix=" << target.prefix << " file=" << livePath << " offset=("
                             << offset.x() << "," << offset.y() << "," << offset.z() << ") rotation=("
                             << rotation.x() << "," << rotation.y() << "," << rotation.z() << ") pivot=("
                             << pivot.x() << "," << pivot.y() << "," << pivot.z() << ") pivotOffset=("
                             << pivotOffset.x() << "," << pivotOffset.y() << "," << pivotOffset.z()
                             << ") pivotMode=" << pivotMode << " for " << mPtr.getCellRef().getRefId();
        }
    }

    void ESM4NpcAnimation::applyLiveRuntimeActorKitSelectors()
    {
        const char* livePath = std::getenv("OPENMW_FNV_LIVE_RUNTIME_COMMAND_FILE");
        if (livePath == nullptr || livePath[0] == '\0')
            return;
        const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(mPtr);
        if (traits == nullptr || !traits->mIsFONV)
            return;
        if (++mLiveRuntimeActorKitTick % 6 != 0)
            return;

        const std::string fingerprint = readFalloutLiveRuntimeActorKitFingerprint();
        if (fingerprint.empty() || fingerprint == mLiveRuntimeActorKitFingerprint)
            return;

        ++mLiveRuntimeActorKitGeneration;

        const bool targetMatches = mPtr.getCell() == nullptr || isFonvProofTargetActor(mPtr, *traits);
        const bool partRebuilt = targetMatches
            && rebuildLiveRuntimeActorKitParts(*traits, mLiveRuntimeActorKitGeneration, fingerprint);
        if (targetMatches)
            requestFalloutActorKitAnimation(*this, mPtr, *traits);

        Log(Debug::Info) << "FNV/ESM4 live runtime: actor-kit post-construction selector generation="
                         << mLiveRuntimeActorKitGeneration
                         << " actor=" << traits->mEditorId
                         << " ref=" << mPtr.getCellRef().getRefId()
                         << " file=\"" << livePath << "\""
                         << " targetMatches=" << targetMatches
                         << " fingerprint=\"" << fingerprint << "\""
                         << " animationRequest=" << (targetMatches ? "issued" : "skipped-non-target")
                         << " partRebuild=" << (partRebuilt ? "runtime-supported" : "loaded-pending-runtime")
                         << " runtime=" << (partRebuilt ? "runtime-supported" : "loaded-pending-runtime")
                         << " gate=runtime-live-actor-kit-post-construction-selector";
        // Only mark the live actor-kit command consumed after target rebuild/post-construction proof logging completes.
        mLiveRuntimeActorKitFingerprint = fingerprint;
    }

    bool ESM4NpcAnimation::rebuildLiveRuntimeActorKitParts(
        const ESM4::Npc& traits, unsigned int generation, std::string_view fingerprint)
    {
        if (mObjectRoot == nullptr)
            return false;

        const FalloutRuntimePartTreeSummary before = summarizeFalloutRuntimePartTree(mObjectRoot.get());
        unsigned int staleAfterRemoval = 0;
        const unsigned int removedParents = removeFalloutRuntimePartTree(mObjectRoot.get(), staleAfterRemoval);
        updateParts();
        const FalloutRuntimePartTreeSummary after = summarizeFalloutRuntimePartTree(mObjectRoot.get());

        const unsigned int requestedParts = countFalloutActorKitSelectorTokens(
            readFalloutLiveRuntimeCommandString({ "actorKitParts", "parts" }));
        const unsigned int requestedPropSlots = countFalloutActorKitSelectorTokens(
            readFalloutLiveRuntimeCommandString({ "actorKitPropSlots", "propSlots" }));
        const unsigned int requestedPartModels = countFalloutActorKitSelectorTokens(
            readFalloutLiveRuntimeCommandString({ "actorKitPartModels", "partModels" }));
        const unsigned int requestedPropModels = countFalloutActorKitSelectorTokens(
            readFalloutLiveRuntimeCommandString({ "actorKitPropModels", "propModels" }));
        const unsigned int requestedTotal = requestedParts + requestedPropSlots + requestedPartModels + requestedPropModels;
        const unsigned int failedParts = requestedTotal > 0 && after.total == 0 ? requestedTotal : 0;
        const bool runtimeSupported = staleAfterRemoval == 0 && after.total > 0 && failedParts == 0;

        Log(runtimeSupported ? Debug::Info : Debug::Warning)
            << "FNV/ESM4 live runtime: actor-kit part rebuild generation=" << generation
            << " actor=" << traits.mEditorId
            << " ref=" << mPtr.getCellRef().getRefId()
            << " targetMatches=1"
            << " requestedParts=" << requestedTotal
            << " requestedSelectorParts=" << requestedParts
            << " requestedPropSlots=" << requestedPropSlots
            << " requestedPartModels=" << requestedPartModels
            << " requestedPropModels=" << requestedPropModels
            << " beforeParts=" << before.total
            << " removedParents=" << removedParents
            << " staleAfterRemoval=" << staleAfterRemoval
            << " rebuiltParts=" << after.total
            << " attachedParts=" << after.total
            << " duplicateParts=" << after.duplicateNames
            << " failedParts=" << failedParts
            << " beforeDuplicateParts=" << before.duplicateNames
            << " fingerprint=\"" << fingerprint << "\""
            << " firstParts=\"" << after.firstNames << "\""
            << " runtime=" << (runtimeSupported ? "runtime-supported" : "loaded-pending-runtime")
            << " gate=runtime-live-actor-kit-part-rebuild";
        return runtimeSupported;
    }

    osg::Vec3f ESM4NpcAnimation::runAnimation(float duration)
    {
        osg::Vec3f movement = Animation::runAnimation(duration);
        applyLiveRuntimeActorKitSelectors();
        applyLiveHeadSurfaceAuthoring();
        return movement;
    }

    void ESM4NpcAnimation::updateParts()
    {
        if (mObjectRoot == nullptr)
            return;
        const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(mPtr);
        if (traits == nullptr)
            return;
        mLiveHeadSurfaceAuthoringTargets.clear();
        mLiveHeadSurfaceAuthoringContent.clear();
        mLiveHeadSurfaceAuthoringTick = 0;
        if (traits->mIsTES4)
            updatePartsTES4(*traits);
        else if (traits->mIsFONV)
            updatePartsFONV(*traits);
        else
        {
            // There is no easy way to distinguish TES5 and FO3.
            // In case of FO3 the function shouldn't crash the game and will
            // only lead to the NPC not being rendered.
            updatePartsTES5(*traits);
        }
    }

    osg::ref_ptr<osg::Node> ESM4NpcAnimation::insertPart(
        std::string_view model, const osg::Vec4f* tint, std::string_view diffuseTexture)
    {
        if (model.empty())
        {
            Log(Debug::Verbose) << "FNV/ESM4 diag: skipped empty NPC model part for "
                                << mPtr.getCellRef().getRefId();
            return nullptr;
        }
        if (Misc::StringUtils::ciEndsWith(model, ".egt") || Misc::StringUtils::ciEndsWith(model, ".egm")
            || Misc::StringUtils::ciEndsWith(model, ".tri"))
        {
            Log(Debug::Verbose) << "FNV/ESM4 diag: skipped non-render NPC data part " << model << " for "
                                << mPtr.getCellRef().getRefId();
            return nullptr;
        }
        const VFS::Path::Normalized correctedModel = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(model));
        osg::ref_ptr<const osg::Node> templateNode = mResourceSystem->getSceneManager()->getTemplate(correctedModel);

        osg::Group* attachNode = mObjectRoot.get();
        const NodeMap& nodeMap = getNodeMap();
        const bool headAttachedStaticPart = isFalloutStaticHeadAttachmentPart(model);
        const bool headgearStaticPart = isFalloutStaticHeadgearPart(model);
        const bool bareHandSurfacePart = isFalloutBareHandSurfaceModel(model);
        if (headAttachedStaticPart)
        {
            const std::string_view mode = headgearStaticPart ? std::string_view("headgear") : getFonvStaticFaceAttachMode(model);
            if (headgearStaticPart)
            {
                const char* headgearMode = std::getenv("OPENMW_FNV_HEADGEAR_ATTACH_MODE");
                if (headgearMode != nullptr && Misc::StringUtils::ciEqual(headgearMode, "head"))
                {
                    if (osg::Group* head = findBestAttachmentNode(nodeMap, { "Bip01 Head", "bip01 head" }))
                        attachNode = head;
                }
                else
                {
                    osg::Group* bip01 = nullptr;
                    if (const auto found = nodeMap.find("Bip01"); found != nodeMap.end())
                        bip01 = found->second.get();
                    osg::Group* head = findBestAttachmentNode(nodeMap, { "Bip01 Head", "bip01 head" });
                    if (bip01 != nullptr && head != nullptr)
                    {
                        if (headgearMode != nullptr && Misc::StringUtils::ciEqual(headgearMode, "animatedheadframe"))
                            attachNode = makeFalloutAnimatedHeadFrameHelper(*bip01, *head);
                        else
                            attachNode = makeFalloutHeadFrameHelper(*bip01, *head);
                    }
                }
            }
            else if (Misc::StringUtils::ciEqual(mode, "root"))
                attachNode = mObjectRoot.get();
            else if (Misc::StringUtils::ciEqual(mode, "bip01"))
            {
                if (const auto bip01 = nodeMap.find("Bip01"); bip01 != nodeMap.end())
                    attachNode = bip01->second.get();
            }
            else if (Misc::StringUtils::ciEqual(mode, "headframe")
                || Misc::StringUtils::ciEqual(mode, "headtranslation"))
            {
                osg::Group* bip01 = nullptr;
                if (const auto found = nodeMap.find("Bip01"); found != nodeMap.end())
                    bip01 = found->second.get();
                osg::Group* head = findBestAttachmentNode(nodeMap, { "Bip01 Head", "bip01 head" });
                if (bip01 != nullptr && head != nullptr)
                    attachNode = makeFalloutHeadFrameHelper(*bip01, *head);
            }
            else if (Misc::StringUtils::ciEqual(mode, "animatedheadframe"))
            {
                osg::Group* bip01 = nullptr;
                if (const auto found = nodeMap.find("Bip01"); found != nodeMap.end())
                    bip01 = found->second.get();
                osg::Group* head = findBestAttachmentNode(nodeMap, { "Bip01 Head", "bip01 head" });
                if (bip01 != nullptr && head != nullptr)
                    attachNode = makeFalloutAnimatedHeadFrameHelper(*bip01, *head);
            }
            else if (osg::Group* head = findBestAttachmentNode(nodeMap, { "Bip01 Head", "bip01 head" }))
                attachNode = head;

            Log(Debug::Info) << "FNV/ESM4 diag: static head attachment mode=" << mode << " model="
                             << correctedModel.value() << " attachNode=" << attachNode->getName() << " for "
                             << mPtr.getCellRef().getRefId();
        }
        if (isFalloutStaticFaceChildPart(model) && attachNode != nullptr)
            attachNode = makeFalloutFaceSurfaceFrameHelper(*attachNode);
        if (bareHandSurfacePart)
        {
            osg::Group* bip01 = nullptr;
            if (const auto found = nodeMap.find("Bip01"); found != nodeMap.end())
                bip01 = found->second.get();
            osg::Group* hand = findBestAttachmentNode(nodeMap,
                isFalloutLeftHandSurfaceModel(model) ? std::initializer_list<std::string_view>{ "Bip01 L Hand", "bip01 l hand" }
                                                     : std::initializer_list<std::string_view>{ "Bip01 R Hand", "bip01 r hand" });
            if (bip01 != nullptr && hand != nullptr)
            {
                if (std::getenv("OPENMW_FNV_HAND_BIND_FRAME_ATTACH") != nullptr)
                {
                    attachNode = makeFalloutAnimatedBoneBindFrameHelper(*bip01, *hand,
                        isFalloutLeftHandSurfaceModel(model) ? std::string_view("FNV Animated L Hand Bind Frame")
                                                             : std::string_view("FNV Animated R Hand Bind Frame"));
                    Log(Debug::Info) << "FNV/ESM4 diag: bare hand bind-frame attachment model="
                                     << correctedModel.value() << " attachNode=" << attachNode->getName() << " for "
                                     << mPtr.getCellRef().getRefId();
                }
                else
                {
                    attachNode = hand;
                    Log(Debug::Info) << "FNV/ESM4 diag: bare hand bone attachment model="
                                     << correctedModel.value() << " attachNode=" << attachNode->getName() << " for "
                                     << mPtr.getCellRef().getRefId();
                }
            }
            else
                Log(Debug::Warning) << "FNV/ESM4 diag: bare hand bind-frame attachment missing bone model="
                                    << correctedModel.value() << " bip01=" << static_cast<bool>(bip01)
                                    << " hand=" << static_cast<bool>(hand) << " for "
                                    << mPtr.getCellRef().getRefId();
        }
        if (attachNode == mObjectRoot.get())
        {
            auto bip01 = nodeMap.find("Bip01");
            if (bip01 != nodeMap.end())
                attachNode = bip01->second.get();
        }

        Log(Debug::Verbose) << "FNV/ESM4 diag: rig-aware attaching NPC model part " << correctedModel.value()
                            << " to " << mPtr.getCellRef().getRefId() << " at " << attachNode->getName();
        FalloutPartShapeSummaryVisitor rigProbe;
        const_cast<osg::Node*>(templateNode.get())->accept(rigProbe);
        osg::ref_ptr<const osg::Node> attachTemplateNode = templateNode;
        bool staticizedHeadPartRig = false;
        bool staticizedBareHandPartRig = false;
        const bool wantsStaticizedHeadPartRig = headAttachedStaticPart && rigProbe.mRigGeometryCount > 0
            && std::getenv("OPENMW_FNV_KEEP_RIGGED_HEAD_PARTS") == nullptr;
        const bool wantsStaticizedBareHandPartRig = bareHandSurfacePart && rigProbe.mRigGeometryCount > 0
            && std::getenv("OPENMW_FNV_KEEP_RIGGED_HAND_PARTS") == nullptr;
        if (wantsStaticizedHeadPartRig)
        {
            osg::ref_ptr<osg::Node> staticTemplate = osg::clone(templateNode.get(), osg::CopyOp::DEEP_COPY_ALL);
            StaticizeFalloutRiggedGeometryVisitor staticizeVisitor(
                mPtr.getCellRef().getRefId().toDebugString(), std::string(correctedModel.value()));
            staticTemplate->accept(staticizeVisitor);
            if (staticizeVisitor.mStaticizedRigGeometryCount > 0)
            {
                staticizedHeadPartRig = true;
                attachTemplateNode = staticTemplate;
                Log(Debug::Info) << "FNV/ESM4 diag: staticized "
                                 << staticizeVisitor.mStaticizedRigGeometryCount
                                 << " rigged head-part drawable(s) for " << correctedModel.value() << " on "
                                 << mPtr.getCellRef().getRefId();
                FalloutPartShapeSummaryVisitor staticizedSummary;
                staticTemplate->accept(staticizedSummary);
                Log(Debug::Info) << "FNV/ESM4 diag: staticized template summary " << correctedModel.value()
                                 << " for " << mPtr.getCellRef().getRefId()
                                 << " rigGeometry=" << staticizedSummary.mRigGeometryCount
                                 << " staticGeometry=" << staticizedSummary.mStaticGeometryCount;
            }
            else
                Log(Debug::Warning) << "FNV/ESM4 diag: failed to staticize rigged head-part "
                                    << correctedModel.value() << " on " << mPtr.getCellRef().getRefId()
                                    << " rigProbe=" << rigProbe.mRigGeometryCount
                                    << " seen=" << staticizeVisitor.mSeenRigGeometryCount
                                    << " missingSource=" << staticizeVisitor.mMissingSourceGeometryCount;
        }
        if (wantsStaticizedBareHandPartRig)
        {
            osg::ref_ptr<osg::Node> staticTemplate = osg::clone(templateNode.get(), osg::CopyOp::DEEP_COPY_ALL);
            StaticizeFalloutRiggedGeometryVisitor staticizeVisitor(
                mPtr.getCellRef().getRefId().toDebugString(), std::string(correctedModel.value()));
            staticTemplate->accept(staticizeVisitor);
            if (staticizeVisitor.mStaticizedRigGeometryCount > 0)
            {
                staticizedBareHandPartRig = true;
                attachTemplateNode = staticTemplate;
                Log(Debug::Info) << "FNV/ESM4 diag: staticized "
                                 << staticizeVisitor.mStaticizedRigGeometryCount
                                 << " rigged bare-hand drawable(s) for " << correctedModel.value() << " on "
                                 << mPtr.getCellRef().getRefId();
                FalloutPartShapeSummaryVisitor staticizedSummary;
                staticTemplate->accept(staticizedSummary);
                Log(Debug::Info) << "FNV/ESM4 diag: staticized template summary " << correctedModel.value()
                                 << " for " << mPtr.getCellRef().getRefId()
                                 << " rigGeometry=" << staticizedSummary.mRigGeometryCount
                                 << " staticGeometry=" << staticizedSummary.mStaticGeometryCount;
            }
            else
                Log(Debug::Warning) << "FNV/ESM4 diag: failed to staticize rigged bare-hand part "
                                    << correctedModel.value() << " on " << mPtr.getCellRef().getRefId()
                                    << " rigProbe=" << rigProbe.mRigGeometryCount
                                    << " seen=" << staticizeVisitor.mSeenRigGeometryCount
                                    << " missingSource=" << staticizeVisitor.mMissingSourceGeometryCount;
        }
        osg::ref_ptr<osg::Node> attached;
        osg::Group* rigPartMaster = mSkeleton != nullptr ? static_cast<osg::Group*>(mSkeleton) : mObjectRoot.get();
        if ((staticizedHeadPartRig || staticizedBareHandPartRig)
            && std::getenv("OPENMW_FNV_DIRECT_ATTACH_STATICIZED_RIG_PARTS") != nullptr)
        {
            attached = mResourceSystem->getSceneManager()->getInstance(attachTemplateNode);
            attachNode->addChild(attached);
            Log(Debug::Info) << "FNV/ESM4 diag: direct-attached staticized rig part "
                             << correctedModel.value() << " to " << mPtr.getCellRef().getRefId()
                             << " attachNode=" << attachNode->getName()
                             << " staticizedHead=" << staticizedHeadPartRig
                             << " staticizedHand=" << staticizedBareHandPartRig;
        }
        else if (staticizedHeadPartRig || staticizedBareHandPartRig)
        {
            attached = attachStaticizedFalloutPart(attachTemplateNode, attachNode, mResourceSystem->getSceneManager(),
                true);
            Log(Debug::Info) << "FNV/ESM4 diag: offset-attached staticized rig part "
                             << correctedModel.value() << " to " << mPtr.getCellRef().getRefId()
                             << " attachNode=" << attachNode->getName()
                             << " staticizedHead=" << staticizedHeadPartRig
                             << " staticizedHand=" << staticizedBareHandPartRig;
        }
        else if (!staticizedHeadPartRig && wantsStaticizedHeadPartRig)
        {
            attached = mResourceSystem->getSceneManager()->getInstance(attachTemplateNode);
            attachNode->addChild(attached);

            StaticizeFalloutRiggedGeometryVisitor liveStaticizeVisitor(
                mPtr.getCellRef().getRefId().toDebugString(), std::string(correctedModel.value()));
            attached->accept(liveStaticizeVisitor);
            if (liveStaticizeVisitor.mStaticizedRigGeometryCount > 0)
            {
                staticizedHeadPartRig = true;
                Log(Debug::Info) << "FNV/ESM4 diag: live-staticized "
                                 << liveStaticizeVisitor.mStaticizedRigGeometryCount
                                 << " rigged head-part drawable(s) for " << correctedModel.value() << " on "
                                 << mPtr.getCellRef().getRefId() << " attachNode=" << attachNode->getName();
            }
            else
                Log(Debug::Warning) << "FNV/ESM4 diag: failed to live-staticize rigged head-part "
                                    << correctedModel.value() << " on " << mPtr.getCellRef().getRefId()
                                    << " rigProbe=" << rigProbe.mRigGeometryCount
                                    << " seen=" << liveStaticizeVisitor.mSeenRigGeometryCount
                                    << " missingSource=" << liveStaticizeVisitor.mMissingSourceGeometryCount;
        }
        else if (!staticizedHeadPartRig && std::getenv("OPENMW_FNV_ATTACH_FULL_RIG_PARTS") != nullptr
            && rigProbe.mRigGeometryCount > 0)
        {
            attached = mResourceSystem->getSceneManager()->getInstance(attachTemplateNode);
            rigPartMaster->addChild(attached);
            Log(Debug::Info) << "FNV/ESM4 diag: full-subtree attaching rigged NPC model part "
                             << correctedModel.value() << " to " << mPtr.getCellRef().getRefId()
                             << " rigGeometry=" << rigProbe.mRigGeometryCount << " master=" << rigPartMaster->getName();
        }
        else
        {
            osg::Quat headgearAttitude;
            const osg::Quat* attitude = nullptr;
            if (headgearStaticPart)
            {
                headgearAttitude = getFalloutHeadgearAttitude();
                Log(Debug::Info) << "FNV/ESM4 diag: applying FNV headgear coordinate basis "
                                 << getFalloutHeadgearAttitudeMode() << " to " << correctedModel.value()
                                 << " on " << mPtr.getCellRef().getRefId();
                if (!headgearAttitude.zeroRotation())
                    attitude = &headgearAttitude;
            }
            osg::Node* attachMaster = rigProbe.mRigGeometryCount > 0 && !staticizedHeadPartRig
                    && !staticizedBareHandPartRig
                ? static_cast<osg::Node*>(rigPartMaster)
                : static_cast<osg::Node*>(mObjectRoot.get());
            attached = SceneUtil::attach(std::move(attachTemplateNode), attachMaster, {}, attachNode,
                mResourceSystem->getSceneManager(), attitude);
            if (rigProbe.mRigGeometryCount > 0 && !staticizedHeadPartRig && !staticizedBareHandPartRig)
            {
                Log(Debug::Info) << "FNV/ESM4 diag: rigged NPC model part master "
                                 << correctedModel.value() << " for " << mPtr.getCellRef().getRefId()
                                 << " master=" << rigPartMaster->getName()
                                 << " objectRoot=" << mObjectRoot->getName()
                                 << " attachNode=" << attachNode->getName();
            }
        }
        if (attached != nullptr && headAttachedStaticPart)
        {
            const osg::Vec3f surfaceOffset = getFalloutHeadFrameSurfaceOffset(model, headgearStaticPart);
            const std::string surfacePrefix = getFalloutHeadFrameSurfacePrefix(model, headgearStaticPart);
            const osg::Vec3f surfaceRotationDegrees = getFalloutHeadFrameSurfaceRotationDegrees(surfacePrefix);
            const osg::Quat surfaceAttitude = makeFalloutEulerAttitude(surfaceRotationDegrees);
            const char* liveAuthoringFile = getFalloutLiveAuthoringFile();
            if (surfaceOffset.length2() > 0.f || !surfaceAttitude.zeroRotation()
                || (liveAuthoringFile != nullptr && !surfacePrefix.empty()))
            {
                osg::ref_ptr<osg::Transform> offsetNode;
                const osg::Vec3f boundsPivot = localBoundsCenter(*attached);
                const osg::Vec3f pivot = getFalloutHeadFrameSurfacePivot(surfacePrefix, boundsPivot);
                const bool pivotMode = std::getenv("OPENMW_FNV_HEAD_SURFACE_PIVOT_ROTATION") != nullptr;
                if (liveAuthoringFile != nullptr)
                {
                    osg::ref_ptr<osg::MatrixTransform> matrixNode = new osg::MatrixTransform;
                    matrixNode->setDataVariance(osg::Object::DYNAMIC);
                    matrixNode->setMatrix(makeFalloutHeadSurfaceMatrix(surfaceOffset, surfaceAttitude, pivot, pivotMode));
                    matrixNode->setUpdateCallback(new FalloutLiveHeadSurfaceAuthoringCallback(surfacePrefix,
                        correctedModel.value(), surfaceOffset, surfaceRotationDegrees, pivot, pivotMode));
                    mLiveHeadSurfaceAuthoringTargets.push_back(
                        { matrixNode, surfacePrefix, std::string(correctedModel.value()), surfaceOffset,
                            surfaceRotationDegrees, pivot, pivotMode });
                    offsetNode = matrixNode;
                    Log(Debug::Info) << "FNV/ESM4 live authoring: installed head surface authoring model="
                                     << correctedModel.value() << " prefix=" << surfacePrefix << " file="
                                     << liveAuthoringFile << " offset=(" << surfaceOffset.x() << ","
                                     << surfaceOffset.y() << "," << surfaceOffset.z() << ") rotation=("
                                     << surfaceRotationDegrees.x() << "," << surfaceRotationDegrees.y() << ","
                                     << surfaceRotationDegrees.z() << ") boundsPivot=(" << boundsPivot.x() << ","
                                     << boundsPivot.y() << "," << boundsPivot.z() << ") pivot=(" << pivot.x() << ","
                                     << pivot.y() << "," << pivot.z() << ") pivotMode=" << pivotMode;
                }
                else if (!surfaceAttitude.zeroRotation() && pivotMode)
                {
                    osg::ref_ptr<osg::MatrixTransform> matrixNode = new osg::MatrixTransform;
                    matrixNode->setMatrix(makeFalloutHeadSurfaceMatrix(surfaceOffset, surfaceAttitude, pivot, true));
                    offsetNode = matrixNode;
                }
                else
                {
                    osg::ref_ptr<osg::PositionAttitudeTransform> pat = new osg::PositionAttitudeTransform;
                    pat->setPosition(surfaceOffset);
                    pat->setAttitude(surfaceAttitude);
                    offsetNode = pat;
                }
                offsetNode->setName("FNV Head Frame Surface Offset " + correctedModel.value());
                if (attached->getNumParents() > 0)
                {
                    osg::Group* parent = attached->getParent(0);
                    if (parent != nullptr && parent->replaceChild(attached.get(), offsetNode.get()))
                        offsetNode->addChild(attached);
                }
                if (offsetNode->getNumChildren() == 0)
                    offsetNode->addChild(attached);
                attached = offsetNode;
                Log(Debug::Info) << "FNV/ESM4 diag: applied head frame surface offset model="
                                 << correctedModel.value() << " offset=(" << surfaceOffset.x() << ","
                                 << surfaceOffset.y() << "," << surfaceOffset.z() << ") rotationPrefix="
                                 << surfacePrefix << " boundsPivot=(" << boundsPivot.x() << "," << boundsPivot.y()
                                 << "," << boundsPivot.z() << ") pivot=(" << pivot.x() << "," << pivot.y() << ","
                                 << pivot.z() << ") pivotMode=" << pivotMode << " liveFile="
                                 << (liveAuthoringFile != nullptr ? liveAuthoringFile : "")
                                 << " for " << mPtr.getCellRef().getRefId();
            }
        }
        if (!diffuseTexture.empty() && attached != nullptr)
        {
            Log(Debug::Info) << "FNV/ESM4 diag: overriding NPC part texture " << diffuseTexture << " on "
                             << correctedModel.value() << " for " << mPtr.getCellRef().getRefId();
            overrideFalloutPartDiffuseTexture(diffuseTexture, mResourceSystem, *attached);
        }
        else if (attached != nullptr)
        {
            const std::string hairDiffuse = getFalloutHairTintDiffuseOverride(correctedModel.value());
            if (!hairDiffuse.empty())
            {
                Log(Debug::Info) << "FNV/ESM4 diag: overriding hair tint diffuse " << hairDiffuse << " on "
                                 << correctedModel.value() << " for " << mPtr.getCellRef().getRefId();
                overrideFalloutPartDiffuseTexture(hairDiffuse, mResourceSystem, *attached);
            }
        }
        if (tint != nullptr && attached != nullptr)
        {
            const bool hairTintModel = isFalloutHairTintModel(correctedModel.value());
            const float emissionStrength = hairTintModel ? getFalloutHairEmissionStrength(*tint) : 0.f;
            TintMaterialVisitor visitor(*tint, emissionStrength, hairTintModel, hairTintModel, hairTintModel);
            attached->accept(visitor);
            if (hairTintModel)
                Log(Debug::Info) << "FNV/ESM4 diag: applied hair tint material " << correctedModel.value()
                                 << " tint=(" << tint->x() << ", " << tint->y() << ", " << tint->z()
                                 << ") emissionStrength=" << emissionStrength
                                 << " preservedVertexIntensityArrays="
                                 << visitor.mPreservedVertexColorIntensityArrays << " for "
                                 << mPtr.getCellRef().getRefId();
        }
        if (attached != nullptr
            && (isFalloutHairTintModel(correctedModel.value()) || isFalloutScalpHairModel(correctedModel.value())
                || isFalloutFaceHairModel(correctedModel.value()) || isFalloutBrowModel(correctedModel.value())))
        {
            FalloutCutoutAlphaVisitor cutoutAlpha;
            attached->accept(cutoutAlpha);
            Log(Debug::Info) << "FNV/ESM4 diag: enabled opaque cutout alpha on " << cutoutAlpha.getAppliedCount()
                             << " hair/brow state(s) for " << correctedModel.value() << " on "
                             << mPtr.getCellRef().getRefId();
        }
        if (attached != nullptr && isFalloutEyeSurfaceModel(correctedModel.value()))
        {
            TintMaterialVisitor eyeMaterial(osg::Vec4f(1.f, 1.f, 1.f, 1.f));
            attached->accept(eyeMaterial);
            Log(Debug::Info) << "FNV/ESM4 diag: applied neutral eye material " << correctedModel.value()
                             << " for " << mPtr.getCellRef().getRefId();
            DisableCullVisitor visitor;
            attached->accept(visitor);
            Log(Debug::Info) << "FNV/ESM4 diag: made eye surface double-sided " << correctedModel.value()
                             << " for " << mPtr.getCellRef().getRefId();
        }
        if (attached != nullptr && isFalloutMouthSurfaceModel(correctedModel.value()))
        {
            TintMaterialVisitor mouthMaterial(osg::Vec4f(1.f, 1.f, 1.f, 1.f));
            attached->accept(mouthMaterial);
            DisableCullVisitor visitor;
            attached->accept(visitor);
            Log(Debug::Info) << "FNV/ESM4 diag: made mouth interior surface double-sided "
                             << correctedModel.value() << " for " << mPtr.getCellRef().getRefId();
        }
        if (attached != nullptr && std::getenv("OPENMW_FNV_PROOF_MOUTH_FORCE_OPEN") != nullptr
            && isFalloutMouthDriverPart(correctedModel.value()))
        {
            attached->addUpdateCallback(new FalloutProofMouthDriver(mPtr, std::string(correctedModel.value())));
            Log(Debug::Info) << "FNV/ESM4 proof: installed mouth open driver on "
                             << correctedModel.value() << " for " << mPtr.getCellRef().getRefId();
        }
        if (attached != nullptr)
        {
            MarkFalloutRigGeometryVisitor markFalloutRigs;
            attached->accept(markFalloutRigs);
            if (markFalloutRigs.mMarked > 0)
                Log(Debug::Info) << "FNV/ESM4 diag: marked " << markFalloutRigs.mMarked
                                 << " Fallout rigged drawable(s) on " << correctedModel.value() << " for "
                                 << mPtr.getCellRef().getRefId();
            attached->setName("FNV Part " + correctedModel.value());
        }
        logFalloutPartShapeSummary(attached.get(), correctedModel.value(), mPtr);
        logFalloutAttachmentBounds(
            attached.get(), attachNode, findBestAttachmentNode(nodeMap, { "Bip01 Head", "bip01 head" }),
            correctedModel.value(), mPtr);
        logFalloutFaceDrawableAudit(attached.get(), correctedModel.value(), mPtr);
        return attached;
    }

    osg::ref_ptr<osg::Node> ESM4NpcAnimation::insertAttachedPart(std::string_view model, std::string_view preferredBone)
    {
        if (model.empty())
            return nullptr;

        const VFS::Path::Normalized correctedModel = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(model));
        osg::ref_ptr<const osg::Node> templateNode = mResourceSystem->getSceneManager()->getTemplate(correctedModel);

        const NodeMap& nodeMap = getNodeMap();
        osg::Group* attachNode = nullptr;
        if (!preferredBone.empty())
            attachNode = findBestAttachmentNode(nodeMap, { preferredBone });
        if (attachNode == nullptr)
            attachNode = findBestAttachmentNode(
                nodeMap, { "Bip01 Weapon", "Weapon", "weapon", "Weapon Bone", "weapon bone", "Bip01 R Hand" });
        if (attachNode == nullptr)
        {
            const auto root = nodeMap.find("Bip01");
            if (root != nodeMap.end())
                attachNode = root->second.get();
        }
        if (attachNode == nullptr)
            attachNode = mObjectRoot.get();

        Log(Debug::Info) << "FNV/ESM4 diag: attaching NPC carried model " << correctedModel.value() << " to "
                         << mPtr.getCellRef().getRefId() << " at " << attachNode->getName();
        osg::ref_ptr<osg::Node> attached = SceneUtil::attach(
            std::move(templateNode), mObjectRoot.get(), {}, attachNode, mResourceSystem->getSceneManager());
        if (attached != nullptr)
            attached->setName("FNV Part " + correctedModel.value());
        logFalloutPartShapeSummary(attached.get(), correctedModel.value(), mPtr);
        logFalloutAttachmentBounds(
            attached.get(), attachNode, findBestAttachmentNode(nodeMap, { "Bip01 Head", "bip01 head" }),
            correctedModel.value(), mPtr);
        return attached;
    }

    static bool fonvRaceBodyPartCovered(std::string_view model, uint32_t coveredSlots)
    {
        std::string lowered(model);
        Misc::StringUtils::lowerCaseInPlace(lowered);

        if ((coveredSlots & ESM4::Armor::FO3_UpperBody) != 0
            && (lowered.find("upperbody") != std::string::npos || lowered.find("lowerbody") != std::string::npos
                || lowered.find("foot") != std::string::npos || lowered.find("feet") != std::string::npos))
            return true;

        if ((coveredSlots & ESM4::Armor::FO3_LeftHand) != 0 && lowered.find("lefthand") != std::string::npos)
            return true;

        if ((coveredSlots & ESM4::Armor::FO3_RightHand) != 0 && lowered.find("righthand") != std::string::npos)
            return true;

        return false;
    }

    static uint32_t getFonvCoveredBodySlots(const MWWorld::Ptr& ptr)
    {
        uint32_t covered = 0;
        for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(ptr))
            covered |= armor->mArmorFlags;
        for (const ESM4::Clothing* clothing : MWClass::ESM4Npc::getEquippedClothing(ptr))
            covered |= clothing->mClothingFlags;
        return covered;
    }

    template <class Record>
    static std::string_view chooseTes4EquipmentModel(const Record* rec, bool isFemale)
    {
        if (isFemale && !rec->mModelFemale.empty())
            return rec->mModelFemale;
        else if (!isFemale && !rec->mModelMale.empty())
            return rec->mModelMale;
        else
            return rec->mModel;
    }

    void ESM4NpcAnimation::updatePartsTES4(const ESM4::Npc& traits)
    {
        const ESM4::Race* race = MWClass::ESM4Npc::getRace(mPtr);
        bool isFemale = MWClass::ESM4Npc::isFemale(mPtr);

        // TODO: Body and head parts are placed incorrectly, need to attach to bones

        for (const ESM4::Race::BodyPart& bodyPart : (isFemale ? race->mBodyPartsFemale : race->mBodyPartsMale))
            insertPart(bodyPart.mesh);
        for (const ESM4::Race::BodyPart& bodyPart : race->mHeadParts)
            insertPart(bodyPart.mesh);
        if (!traits.mHair.isZeroOrUnset())
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (const ESM4::Hair* hair = store->get<ESM4::Hair>().search(traits.mHair))
                insertPart(hair->mModel);
            else
                Log(Debug::Error) << "Hair not found: " << ESM::RefId(traits.mHair);
        }

        for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(mPtr))
            insertPart(chooseTes4EquipmentModel(armor, isFemale));
        for (const ESM4::Clothing* clothing : MWClass::ESM4Npc::getEquippedClothing(mPtr))
            insertPart(chooseTes4EquipmentModel(clothing, isFemale));
    }

    void ESM4NpcAnimation::updatePartsFONV(const ESM4::Npc& traits)
    {
        if (std::getenv("OPENMW_FNV_HIDE_PLAYER_PROOF_PARTS") != nullptr
            && mPtr.getCell() != nullptr
            && mPtr == MWBase::Environment::get().getWorld()->getPlayerPtr())
        {
            Log(Debug::Info) << "FNV/ESM4 proof: skipped player actor part assembly for clean NPC screenshot";
            return;
        }

        if (mPtr.getCell() != nullptr && !isFonvProofTargetActor(mPtr, traits))
        {
            Log(Debug::Info) << "FNV/ESM4 proof: skipped non-target actor part assembly for "
                             << traits.mEditorId << " ref=" << mPtr.getCellRef().getRefId();
            return;
        }

        if (const char* proofTarget = getFonvProofActorTarget(); proofTarget != nullptr && proofTarget[0] != '\0')
        {
            const ESM4::Npc* baseRecord = nullptr;
            if (const MWWorld::LiveCellRef<ESM4::Npc>* ref = mPtr.get<ESM4::Npc>())
                baseRecord = ref->mBase;

            Log(Debug::Info) << "FNV/ESM4 proof: actor part assembly target match target=\""
                             << proofTarget
                             << "\" actor=" << traits.mEditorId
                             << " refAlias=" << mPtr.getCellRef().getRefId()
                             << " ref=" << mPtr.getCellRef().getRefNum().toString("FormId:")
                             << " baseEditor=" << (baseRecord != nullptr ? baseRecord->mEditorId : "<none>")
                             << " baseForm="
                             << (baseRecord != nullptr ? ESM::RefId(baseRecord->mId).toDebugString()
                                                       : std::string("<none>"))
                             << " traitsForm=" << ESM::RefId(traits.mId)
                             << " runtime=runtime-supported";
        }

        const ESM4::Race* race = MWClass::ESM4Npc::getRace(mPtr);
        if (race == nullptr)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: race not found while assembling FONV NPC parts for "
                                << traits.mEditorId;
            return;
        }

        const bool isFemale = MWClass::ESM4Npc::isFemale(mPtr);
        const std::shared_ptr<const FaceGenCtl> faceGenCtl = loadFaceGenCtl(mResourceSystem);
        validateFaceGenCtlBasis(faceGenCtl.get(), traits);
        const uint32_t coveredBodySlots = getFonvCoveredBodySlots(mPtr);
        const std::string npcFaceTexture = findFonvNpcFaceTexture(mResourceSystem, traits);
        const std::string faceGenTextureMode = getFonvFaceGenTextureMode();
        const bool useNpcFaceAsGeneratedDiffuse = faceGenTextureMode == "generated-diffuse";
        const bool useNpcFaceAsDirectDiffuse = faceGenTextureMode == "direct-diffuse";
        const bool useNpcFaceAsDetail = faceGenTextureMode == "detail"
            || std::getenv("OPENMW_FNV_PROOF_FACE_AS_DETAIL") != nullptr;
        const std::string npcFaceDetailTexture = useNpcFaceAsDetail
            ? findFonvNpcFaceDetailTexture(mResourceSystem, traits)
            : std::string();
        const std::string npcFaceNormalTexture = findFonvNpcFaceNormalTexture(mResourceSystem, traits);
        const std::string npcBodyTexture = findFonvNpcBodyTexture(mResourceSystem, traits, isFemale);
        const std::string npcBodyDetailTexture = findFonvNpcBodyDetailTexture(mResourceSystem, traits, isFemale);
        const std::string npcBodyNormalTexture = findFonvNpcBodyNormalTexture(mResourceSystem, traits, isFemale);
        osg::Vec4f npcBodyMaterialTint(1.f, 1.f, 1.f, 1.f);
        int npcBodyTintWidth = 0;
        int npcBodyTintHeight = 0;
        const bool npcBodyDetailIsTinyTint = !npcBodyDetailTexture.empty()
            && getAverageTextureTint(
                mResourceSystem, npcBodyDetailTexture, npcBodyMaterialTint, npcBodyTintWidth, npcBodyTintHeight)
            && npcBodyTintWidth <= 16 && npcBodyTintHeight <= 16;
        if (!npcFaceTexture.empty())
            Log(Debug::Info) << "FNV/ESM4 diag: accounting baked NPC FaceGen texture " << npcFaceTexture
                             << " as source data for " << traits.mEditorId
                             << " runtime=loaded-pending-exact-facegen-texture-synthesis";
        if (!npcFaceDetailTexture.empty())
            Log(Debug::Info) << "FNV/ESM4 diag: using baked NPC face texture as face detail component "
                             << npcFaceDetailTexture << " for " << traits.mEditorId
                             << " mode=" << faceGenTextureMode
                             << " runtime=loaded-pending-exact-facegen-texture-synthesis"
                             << " gate=runtime-fnv-facegen-detail-source";
        if (!npcFaceNormalTexture.empty())
            Log(Debug::Info) << "FNV/ESM4 diag: using baked NPC face normal texture " << npcFaceNormalTexture
                             << " for " << traits.mEditorId;
        if (!npcBodyTexture.empty())
            Log(Debug::Info) << "FNV/ESM4 diag: using baked NPC body texture " << npcBodyTexture << " for "
                             << traits.mEditorId;
        if (!npcBodyDetailTexture.empty())
            Log(Debug::Info) << "FNV/ESM4 diag: using baked NPC body tint/detail " << npcBodyDetailTexture
                             << " for " << traits.mEditorId;
        if (npcBodyDetailIsTinyTint)
            Log(Debug::Info) << "FNV/ESM4 diag: treating NPC body tint/detail " << npcBodyDetailTexture
                             << " as tiny BSA body tint swatch size=" << npcBodyTintWidth << "x" << npcBodyTintHeight
                             << " average=(" << npcBodyMaterialTint.x() << ", " << npcBodyMaterialTint.y() << ", "
                             << npcBodyMaterialTint.z() << ") multiplier=none for " << traits.mEditorId;
        if (!npcBodyNormalTexture.empty())
            Log(Debug::Info) << "FNV/ESM4 diag: using baked NPC body normal texture " << npcBodyNormalTexture
                             << " for " << traits.mEditorId;

        const bool proofActor = isEasyPeteProofActor(traits);
        if (proofActor)
        {
            const ESM4::Npc* traitsRecord = MWClass::ESM4Npc::getTraitsRecord(mPtr);
            const ESM4::Npc* modelRecord = MWClass::ESM4Npc::getModelRecord(mPtr);
            Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: BEGIN ref="
                             << mPtr.getCellRef().getRefId() << " npc=" << traits.mEditorId
                             << " form=" << ESM::RefId(traits.mId) << " race=" << ESM::RefId(traits.mRace)
                             << " raceEditor=" << race->mEditorId << " female=" << isFemale
                             << " skeleton=" << mPtr.getClass().getCorrectedModel(mPtr)
                             << " traitsRecord=" << (traitsRecord != nullptr ? traitsRecord->mEditorId : "<none>")
                             << " modelRecord=" << (modelRecord != nullptr ? modelRecord->mEditorId : "<none>")
                             << " kffz=" << (modelRecord != nullptr ? modelRecord->mKf.size() : 0);
            Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: textures face="
                             << (npcFaceTexture.empty() ? std::string("<race/default>") : npcFaceTexture)
                             << " faceDetail="
                             << (npcFaceDetailTexture.empty() ? std::string("<none>") : npcFaceDetailTexture)
                             << " faceNormal="
                             << (npcFaceNormalTexture.empty() ? std::string("<none>") : npcFaceNormalTexture)
                             << " body=" << (npcBodyTexture.empty() ? std::string("<race/default>") : npcBodyTexture)
                             << " bodyDetail="
                             << (npcBodyDetailTexture.empty() ? std::string("<none>") : npcBodyDetailTexture)
                             << " bodyNormal="
                             << (npcBodyNormalTexture.empty() ? std::string("<none>") : npcBodyNormalTexture)
                             << " bodyDetailTiny=" << npcBodyDetailIsTinyTint << " coveredSlots=0x" << std::hex
                             << coveredBodySlots << std::dec;
            Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: facegen fgRace=" << traits.mFgRace
                             << " hair=" << ESM::RefId(traits.mHair) << " eyes=" << ESM::RefId(traits.mEyes)
                             << " headParts=[" << formatFormIdList(traits.mHeadParts) << "]"
                             << " tintLayers=" << traits.mTintLayers.size();
        }
        const bool characterBuilderActive = isFalloutCharacterBuilderActive();
        const std::string characterBuilderPhase
            = characterBuilderActive ? getFalloutCharacterBuilderPhase() : std::string();
        if (characterBuilderActive)
            Log(Debug::Info) << "FNV/ESM4 CHARACTER BUILDER begin phase=" << characterBuilderPhase
                             << " actor=" << traits.mEditorId
                             << " ref=" << mPtr.getCellRef().getRefId()
                             << " ladder=body,head,face,hair,equipment,weapon,headgear,talk,full";

        for (const ESM4::Race::BodyPart& bodyPart : (isFemale ? race->mBodyPartsFemale : race->mBodyPartsMale))
        {
            if (!falloutCharacterBuilderAllows("body-skin", bodyPart.mesh))
            {
                logFalloutCharacterBuilderGate(false, "body-skin", bodyPart.mesh, mPtr, traits);
                continue;
            }
            logFalloutCharacterBuilderGate(true, "body-skin", bodyPart.mesh, mPtr, traits);
            if (fonvRaceBodyPartCovered(bodyPart.mesh, coveredBodySlots))
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: skipped covered race body skin " << bodyPart.mesh << " for "
                                    << traits.mEditorId << " slots=0x" << std::hex << coveredBodySlots << std::dec;
                continue;
            }
            const std::string_view texture
                = !npcBodyTexture.empty() && isFonvRaceSkinSurface(bodyPart.mesh) ? npcBodyTexture : bodyPart.texture;
            const std::string bodyNormalTexture = !npcBodyNormalTexture.empty() ? npcBodyNormalTexture
                : findFonvTextureNormalCompanion(mResourceSystem, texture);
            const std::string bodySkinTexture = findFonvTextureSkinCompanion(mResourceSystem, texture);
            osg::ref_ptr<osg::Node> attached = insertPart(bodyPart.mesh, nullptr, texture);
            if (attached != nullptr && isFonvRaceSkinSurface(bodyPart.mesh))
            {
                forceFalloutActorPartVisible(attached.get(), bodyPart.mesh, traits);
                neutralizeFalloutSkinMaterial(attached.get(), bodyPart.mesh, traits);
                const bool appliedEgtTint = applyFaceGenEgtTint(mResourceSystem, attached.get(), bodyPart.mesh, traits);
                if (!appliedEgtTint && npcBodyDetailIsTinyTint)
                {
                    const bool rawBodyTintDisabled = std::getenv("OPENMW_FNV_DISABLE_RAW_BODY_TINT_SWATCH") != nullptr;
                    if (!rawBodyTintDisabled && std::getenv("OPENMW_FNV_USE_RAW_BODY_TINT_SWATCH") != nullptr)
                    {
                        tintFalloutSkinMaterial(attached.get(), bodyPart.mesh, traits, npcBodyMaterialTint);
                        Log(Debug::Info) << "FNV/ESM4 diag: applied raw BSA body tint swatch " << npcBodyDetailTexture
                                         << " size=" << npcBodyTintWidth << "x" << npcBodyTintHeight << " average=("
                                         << npcBodyMaterialTint.x() << ", " << npcBodyMaterialTint.y() << ", "
                                         << npcBodyMaterialTint.z() << ") on " << bodyPart.mesh << " for "
                                         << traits.mEditorId
                                         << " runtime=runtime-fnv-body-tint-swatch-applied multiplier=none"
                                         << " optOut=OPENMW_FNV_DISABLE_RAW_BODY_TINT_SWATCH";
                    }
                    else
                    {
                        Log(Debug::Info) << "FNV/ESM4 diag: loaded raw BSA body tint swatch " << npcBodyDetailTexture
                                         << " size=" << npcBodyTintWidth << "x" << npcBodyTintHeight << " average=("
                                         << npcBodyMaterialTint.x() << ", " << npcBodyMaterialTint.y() << ", "
                                         << npcBodyMaterialTint.z() << ") on " << bodyPart.mesh << " for "
                                         << traits.mEditorId
                                         << " runtime=loaded-pending-exact-body-tint-synthesis multiplier=none"
                                         << " optIn=OPENMW_FNV_USE_RAW_BODY_TINT_SWATCH"
                                         << " optOut=OPENMW_FNV_DISABLE_RAW_BODY_TINT_SWATCH"
                                         << " disabled=" << rawBodyTintDisabled;
                    }
                }
                if (!npcBodyDetailTexture.empty() && !npcBodyDetailIsTinyTint)
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: applying NPC body tint/detail " << npcBodyDetailTexture
                                     << " on " << bodyPart.mesh << " for " << traits.mEditorId;
                    overrideFalloutPartDetailTexture(npcBodyDetailTexture, mResourceSystem, *attached);
                }
                if (!bodyNormalTexture.empty())
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: binding data-derived body normal map " << bodyNormalTexture
                                     << " on " << bodyPart.mesh << " for " << traits.mEditorId;
                    overrideFalloutPartNormalTexture(bodyNormalTexture, mResourceSystem, *attached);
                }
                if (!bodySkinTexture.empty())
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: binding FNV body skin/subsurface companion "
                                     << formatTextureImageSummary(mResourceSystem, bodySkinTexture) << " as skinMap on "
                                     << bodyPart.mesh << " for " << traits.mEditorId
                                     << " runtime=runtime-supported gate=runtime-fnv-skin-map-bound";
                    overrideFalloutPartSkinTexture(bodySkinTexture, mResourceSystem, *attached);
                }
                forceFalloutSkinOpaqueNoBlend(attached.get(), bodyPart.mesh, traits);
            }
        }
        std::string_view eyeTexture;
        if (!traits.mEyes.isZeroOrUnset())
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (const ESM4::Eyes* eyes = store->get<ESM4::Eyes>().search(traits.mEyes))
                eyeTexture = eyes->mIcon;
            else
                Log(Debug::Error) << "Eyes not found: " << ESM::RefId(traits.mEyes);
        }

        bool raceFacePartAttached[8] = {};
        bool raceFacePartHasMesh[8] = {};
        bool resolvedFaceNormalTexture = false;
        std::string resolvedFaceSkinCompanionStatus = "MISSING";
        std::string resolvedFaceGenTextureStatus = npcFaceTexture.empty() ? "MISSING" : "LOADED_PENDING";
        std::string resolvedFaceDiffuseTexture;
        std::string resolvedFaceDiffuseSource = "MISSING";
        const std::vector<ESM4::Race::BodyPart>& raceHeadParts = isFemale ? race->mHeadPartsFemale : race->mHeadParts;
        for (std::size_t i = 0; i < raceHeadParts.size(); ++i)
        {
            const ESM4::Race::BodyPart& headPart = raceHeadParts[i];
            const bool eyePart = isFonvEyePart(i);
            const bool headSurface = isFonvHeadSurfacePart(i);
            const std::string_view builderCategory = headSurface ? std::string_view("head-skin")
                : std::string_view("face-organs");
            if (!falloutCharacterBuilderAllows(builderCategory, headPart.mesh))
            {
                if (i < 8)
                {
                    raceFacePartAttached[i] = false;
                    raceFacePartHasMesh[i] = !headPart.mesh.empty();
                }
                logFalloutCharacterBuilderGate(false, builderCategory, headPart.mesh, mPtr, traits);
                Log(Debug::Info) << "FNV/ESM4 diag: race face part " << getFonvRaceHeadPartRole(i)
                                 << " index=" << i << " mesh=" << headPart.mesh
                                 << " attached=0 status=intentionally-excluded-with-proof for "
                                 << traits.mEditorId << " characterBuilderPhase="
                                 << characterBuilderPhase;
                continue;
            }
            logFalloutCharacterBuilderGate(true, builderCategory, headPart.mesh, mPtr, traits);
            const std::string_view baseTexture = eyePart && !eyeTexture.empty() ? eyeTexture : headPart.texture;
            const std::string headDiffuseTexture = std::string(baseTexture);
            osg::ref_ptr<osg::Image> generatedHeadDiffuse;
            if (headSurface && useNpcFaceAsGeneratedDiffuse && !npcFaceTexture.empty() && !headDiffuseTexture.empty())
            {
                generatedHeadDiffuse
                    = generateFonvFaceGenDiffuse(mResourceSystem, headDiffuseTexture, npcFaceTexture, traits);
                if (generatedHeadDiffuse == nullptr)
                    Log(Debug::Warning) << "FNV/ESM4 diag: failed to generate NPC FaceGen diffuse base="
                                        << headDiffuseTexture << " face=" << npcFaceTexture << " for "
                                        << traits.mEditorId
                                        << " runtime=loaded-pending-exact-facegen-texture-synthesis";
            }
            const std::string appliedHeadDiffuseTexture = headSurface && useNpcFaceAsDirectDiffuse && !npcFaceTexture.empty()
                ? npcFaceTexture
                : headDiffuseTexture;
            if (headSurface)
            {
                resolvedFaceDiffuseTexture = appliedHeadDiffuseTexture;
                resolvedFaceDiffuseSource = !appliedHeadDiffuseTexture.empty()
                    ? (generatedHeadDiffuse != nullptr
                            ? "NPC_FACEGEN_GENERATED_DIFFUSE"
                        : !npcFaceTexture.empty() && useNpcFaceAsDirectDiffuse
                            ? "NPC_FACEGEN_DIRECT_DIFFUSE"
                            : (!npcFaceDetailTexture.empty() ? "RACE_FACEGEN_DETAIL" : "RACE"))
                    : "MISSING";
                if (!npcFaceTexture.empty() && !headDiffuseTexture.empty() && useNpcFaceAsDirectDiffuse)
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: applying NPC FaceGen diffuse " << npcFaceTexture
                                     << " over race head diffuse " << headDiffuseTexture
                                     << " for " << traits.mEditorId
                                     << " runtime=runtime-supported gate=runtime-fnv-facegen-diffuse-applied";
                }
            }
            const std::string headNormalTexture = headSurface
                ? (!npcFaceNormalTexture.empty() ? npcFaceNormalTexture
                                                 : findFonvTextureNormalCompanion(mResourceSystem, baseTexture))
                : std::string();
            const std::string headSkinTexture = headSurface
                ? findFonvTextureSkinCompanion(mResourceSystem, headDiffuseTexture)
                : std::string();
            osg::ref_ptr<osg::Node> attached = insertPart(headPart.mesh, nullptr, appliedHeadDiffuseTexture);
            if (i < 8)
            {
                raceFacePartAttached[i] = attached != nullptr;
                raceFacePartHasMesh[i] = !headPart.mesh.empty();
            }
            Log(Debug::Info) << "FNV/ESM4 diag: race face part " << getFonvRaceHeadPartRole(i)
                             << " index=" << i << " mesh=" << headPart.mesh << " texture="
                             << (appliedHeadDiffuseTexture.empty() ? std::string("<none>") : appliedHeadDiffuseTexture)
                             << " attached=" << (attached != nullptr) << " status="
                             << getFonvFacePartStatus(attached != nullptr, !headPart.mesh.empty()) << " for "
                             << traits.mEditorId;
            if (headSurface && attached != nullptr)
            {
                forceFalloutActorPartVisible(attached.get(), headPart.mesh, traits);
                applyFaceGenEgmMorph(mResourceSystem, attached.get(), headPart.mesh, traits);
                if (falloutCharacterBuilderAllows("headgear"))
                    hideFonvCoveredHeadSkinCap(attached.get(), headPart.mesh, traits, coveredBodySlots);
                else if (traits.mIsFONV && fonvCoveredSlotsHideScalpHair(coveredBodySlots)
                    && isFalloutDialogueHeadSkinSurface(headPart.mesh))
                {
                    Log(Debug::Info)
                        << "FNV/ESM4 diag: skipped covered head-skin cap mask on " << headPart.mesh << " for "
                        << traits.mEditorId
                        << " because headgear is excluded by the current character builder phase"
                        << " status=intentionally-excluded-with-proof gate=runtime-fnv-covered-head-skin-cap-mask";
                }
                if (generatedHeadDiffuse != nullptr)
                {
                    overrideFalloutPartDiffuseTexture(generatedHeadDiffuse.get(), mResourceSystem, *attached);
                    resolvedFaceGenTextureStatus = "GENERATED_DIFFUSE_APPLIED_PENDING_EXACT";
                }
                else if (!appliedHeadDiffuseTexture.empty())
                {
                    overrideFalloutPartDiffuseTexture(appliedHeadDiffuseTexture, mResourceSystem, *attached);
                    if (!npcFaceTexture.empty() && useNpcFaceAsDirectDiffuse)
                        resolvedFaceGenTextureStatus = "DIFFUSE_APPLIED";
                }
                if (!npcFaceDetailTexture.empty())
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: applying NPC FaceGen detail component " << npcFaceDetailTexture
                                     << " over race head diffuse "
                                     << (headDiffuseTexture.empty() ? std::string("<none>") : headDiffuseTexture)
                                     << " on " << headPart.mesh << " for " << traits.mEditorId
                                     << " runtime=loaded-pending-exact-facegen-texture-synthesis"
                                     << " gate=runtime-fnv-facegen-detail-applied";
                    overrideFalloutPartDetailTexture(npcFaceDetailTexture, mResourceSystem, *attached);
                    resolvedFaceGenTextureStatus = "DETAIL_APPLIED_PENDING_EXACT";
                }
                if (!headNormalTexture.empty())
                {
                    resolvedFaceNormalTexture = true;
                    Log(Debug::Info) << "FNV/ESM4 diag: binding data-derived face normal map " << headNormalTexture
                                     << " on " << headPart.mesh << " for " << traits.mEditorId;
                    overrideFalloutPartNormalTexture(headNormalTexture, mResourceSystem, *attached);
                }
                if (!headSkinTexture.empty())
                {
                    resolvedFaceSkinCompanionStatus = "BOUND";
                    Log(Debug::Info) << "FNV/ESM4 diag: binding FNV face skin/subsurface companion "
                                     << formatTextureImageSummary(mResourceSystem, headSkinTexture) << " as skinMap on "
                                     << headPart.mesh << " for " << traits.mEditorId
                                     << " runtime=runtime-supported gate=runtime-fnv-skin-map-bound";
                    overrideFalloutPartSkinTexture(headSkinTexture, mResourceSystem, *attached);
                }
                neutralizeFalloutSkinMaterial(attached.get(), headPart.mesh, traits);
                applyFaceGenEgtTint(mResourceSystem, attached.get(), headPart.mesh, traits);
                forceFalloutSkinOpaqueNoBlend(attached.get(), headPart.mesh, traits);
                DisableCullVisitor visitor;
                attached->accept(visitor);
                Log(Debug::Info) << "FNV/ESM4 diag: made head skin surface double-sided " << headPart.mesh
                                 << " for " << traits.mEditorId;
                if (std::getenv("OPENMW_FNV_PROOF_MOUTH_FORCE_OPEN") != nullptr)
                    applyFalloutProofTriOpenMorph(mResourceSystem, mPtr, attached.get(), headPart.mesh, traits);
            }
            if (attached != nullptr)
                applyFalloutDialogueMorph(mResourceSystem, this, attached.get(), headPart.mesh, traits);
            logFalloutFaceDrawableAudit(attached.get(), headPart.mesh, mPtr, "final-race-head");
        }

        std::set<uint32_t> usedHeadPartTypes;
        unsigned int visibleHairGeometry = 0;
        unsigned int insertedHeadParts
            = insertHeadParts(traits, traits.mHeadParts, usedHeadPartTypes, coveredBodySlots, &visibleHairGeometry);
        bool fallbackHairAttached = false;
        if (!traits.mHair.isZeroOrUnset() && usedHeadPartTypes.count(ESM4::HeadPart::Type_Hair) == 0)
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (const ESM4::Hair* hair = store->get<ESM4::Hair>().search(traits.mHair))
            {
                if (!falloutCharacterBuilderAllows("hair-beard", hair->mModel))
                {
                    logFalloutCharacterBuilderGate(false, "hair-beard", hair->mModel, mPtr, traits);
                    Log(Debug::Info) << "FNV/ESM4 diag: skipped fallback FONV NPC hair "
                                     << hair->mEditorId << " model=" << hair->mModel
                                     << " for " << traits.mEditorId
                                     << " status=intentionally-excluded-with-proof characterBuilderPhase="
                                     << characterBuilderPhase;
                    goto afterFallbackHair;
                }
                logFalloutCharacterBuilderGate(true, "hair-beard", hair->mModel, mPtr, traits);
                usedHeadPartTypes.insert(ESM4::HeadPart::Type_Hair);
                const osg::Vec4f rawHairTint = getHairTint(traits);
                const osg::Vec4f hairTint = getFalloutRenderHairTint(traits);
                Log(Debug::Info) << "FNV/ESM4 diag: inserting FONV NPC hair " << hair->mEditorId << " model="
                                 << hair->mModel << " rawTint=(" << rawHairTint.x() << ", " << rawHairTint.y()
                                 << ", " << rawHairTint.z() << ") renderTint=(" << hairTint.x() << ", "
                                 << hairTint.y() << ", " << hairTint.z() << ") for " << traits.mEditorId;
                osg::ref_ptr<osg::Node> attached = insertPart(hair->mModel, &hairTint);
                fallbackHairAttached = attached != nullptr;
                applyFaceGenEgmMorph(mResourceSystem, attached.get(), hair->mModel, traits);
                if (attached != nullptr)
                    applyFalloutDialogueMorph(mResourceSystem, this, attached.get(), hair->mModel, traits);
                if (attached != nullptr)
                {
                    TintMaterialVisitor visitor(
                        hairTint, getFalloutHairEmissionStrength(hairTint), true, true, true);
                    attached->accept(visitor);
                    Log(Debug::Info) << "FNV/ESM4 diag: applied hair tint material " << hair->mModel
                                     << " tint=(" << hairTint.x() << ", " << hairTint.y() << ", " << hairTint.z()
                                     << ") emissionStrength=" << getFalloutHairEmissionStrength(hairTint)
                                     << " preservedVertexIntensityArrays="
                                     << visitor.mPreservedVertexColorIntensityArrays
                                     << " for " << traits.mEditorId;
                    hideFonvNoHatHairVariant(attached.get(), hair->mModel, traits, coveredBodySlots);
                    logFalloutFaceDrawableAudit(attached.get(), hair->mModel, mPtr, "final-hat-hair");
                    const FalloutVisibleGeometryVisitor hairVisibility = countFalloutVisibleGeometry(attached.get());
                    visibleHairGeometry += hairVisibility.mVisibleGeometryCount;
                    Log(Debug::Info) << "FNV/ESM4 diag: hair visibility proof " << hair->mModel
                                     << " for " << traits.mEditorId
                                     << " visibleGeometry=" << hairVisibility.mVisibleGeometryCount
                                     << " visibleVertices=" << hairVisibility.mVisibleVertexCount
                                     << " runtime="
                                     << (hairVisibility.mVisibleGeometryCount > 0 ? "runtime-supported"
                                                                                  : "loaded-pending-runtime")
                                     << " gate=runtime-fnv-visible-hair-geometry";
                }
                ++insertedHeadParts;
            }
            else
                Log(Debug::Error) << "Hair not found: " << ESM::RefId(traits.mHair);
        }
    afterFallbackHair:

        if (insertedHeadParts > 0)
            Log(Debug::Info) << "FNV/ESM4 diag: using " << insertedHeadParts
                             << " NPC-specific head mesh part(s) for " << traits.mEditorId;

        const char* hairAttachStatus = "MISSING";
        if (usedHeadPartTypes.count(ESM4::HeadPart::Type_Hair) != 0 || fallbackHairAttached)
            hairAttachStatus = visibleHairGeometry > 0 ? "OK" : "EMPTY";

        Log(Debug::Info) << "FNV/ESM4 FACE CHECK " << traits.mEditorId
                         << ": head=" << getFonvFacePartStatus(raceFacePartAttached[0], raceFacePartHasMesh[0])
                         << " ears=" << getFonvFacePartStatus(raceFacePartAttached[1], raceFacePartHasMesh[1])
                         << " mouth=" << getFonvFacePartStatus(raceFacePartAttached[2], raceFacePartHasMesh[2])
                         << " lowerTeeth=" << getFonvFacePartStatus(raceFacePartAttached[3], raceFacePartHasMesh[3])
                         << " upperTeeth=" << getFonvFacePartStatus(raceFacePartAttached[4], raceFacePartHasMesh[4])
                         << " tongue=" << getFonvFacePartStatus(raceFacePartAttached[5], raceFacePartHasMesh[5])
                         << " leftEye=" << getFonvFacePartStatus(raceFacePartAttached[6], raceFacePartHasMesh[6])
                         << " rightEye=" << getFonvFacePartStatus(raceFacePartAttached[7], raceFacePartHasMesh[7])
                         << " eyesRecord=" << (!traits.mEyes.isZeroOrUnset() ? "OK" : "MISSING")
                         << " eyeTexture=" << (!eyeTexture.empty() ? "OK" : "MISSING")
                         << " hairRecord=" << (!traits.mHair.isZeroOrUnset() ? "OK" : "MISSING")
                         << " hairAttached=" << hairAttachStatus
                         << " facialHairType="
                         << (usedHeadPartTypes.count(ESM4::HeadPart::Type_FacialHair) != 0 ? "OK" : "UNKNOWN")
                         << " npcSpecificHeadParts=" << insertedHeadParts
                         << " hairVisibleGeometry=" << visibleHairGeometry
                         << " faceDiffuse=" << resolvedFaceDiffuseSource
                         << " faceGenTexture=" << resolvedFaceGenTextureStatus
                         << " faceNormal=" << (resolvedFaceNormalTexture ? "OK" : "RACE")
                         << " faceSkinCompanion=" << resolvedFaceSkinCompanionStatus
                         << " tintLayers=" << traits.mTintLayers.size();

        const auto [shapeNonZero, shapeTotal] = summarizeCoefficients(traits.mSymShapeModeCoefficients);
        const auto [asymNonZero, asymTotal] = summarizeCoefficients(traits.mAsymShapeModeCoefficients);
        const auto [textureNonZero, textureTotal] = summarizeCoefficients(traits.mSymTextureModeCoefficients);
        Log(Debug::Info) << "FNV/ESM4 diag: FaceGen summary for " << traits.mEditorId << " fgRace=" << traits.mFgRace
                         << " shape=" << shapeNonZero << "/" << traits.mSymShapeModeCoefficients.size()
                         << " sumAbs=" << shapeTotal << " asym=" << asymNonZero << "/"
                         << traits.mAsymShapeModeCoefficients.size() << " sumAbs=" << asymTotal << " texture="
                         << textureNonZero << "/" << traits.mSymTextureModeCoefficients.size()
                         << " sumAbs=" << textureTotal << " tintLayers=" << traits.mTintLayers.size();
        unsigned int tintLogCount = 0;
        for (const ESM4::Npc::TintLayer& tint : traits.mTintLayers)
        {
            if (tintLogCount >= 8)
                break;
            Log(Debug::Info) << "FNV/ESM4 diag: tint layer " << tintLogCount << " for " << traits.mEditorId
                             << " hasIndex=" << tint.hasIndex << " index=" << tint.index
                             << " hasValue=" << tint.hasValue << " value=" << tint.value
                             << " hasColor=" << tint.hasColor << " color=("
                             << static_cast<unsigned int>(tint.color.red) << ", "
                             << static_cast<unsigned int>(tint.color.green) << ", "
                             << static_cast<unsigned int>(tint.color.blue) << ", "
                             << static_cast<unsigned int>(tint.color.custom) << ")";
            ++tintLogCount;
        }

        const auto& equippedArmor = MWClass::ESM4Npc::getEquippedArmor(mPtr);
        const auto& equippedClothing = MWClass::ESM4Npc::getEquippedClothing(mPtr);
        const auto insertArmorEquipment = [&](const ESM4::Armor* armor, const char* layer) {
            const std::string_view model = MWClass::ESM4Npc::chooseEquipmentModel(armor, isFemale);
            const std::string_view builderCategory
                = isFonvEquipmentHeadStackLayer(layer) ? std::string_view("headgear")
                                                       : std::string_view("equipment-body");
            if (proofActor)
                Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: armor " << armor->mEditorId
                                 << " form=" << ESM::RefId(armor->mId) << " model=" << model << " flags=0x"
                                 << std::hex << armor->mArmorFlags << std::dec << " layer=" << layer;
            if (!falloutCharacterBuilderAllows(builderCategory, model))
            {
                logFalloutCharacterBuilderGate(false, builderCategory, model, mPtr, traits);
                Log(Debug::Info) << "FNV/ESM4 diag: skipped armor " << armor->mEditorId
                                 << " model=" << model << " layer=" << layer << " for "
                                 << traits.mEditorId
                                 << " status=intentionally-excluded-with-proof characterBuilderPhase="
                                 << characterBuilderPhase;
                return;
            }
            logFalloutCharacterBuilderGate(true, builderCategory, model, mPtr, traits);
            osg::ref_ptr<osg::Node> attached = insertPart(model);
            forceFalloutActorPartVisible(attached.get(), model, traits);
            overrideFalloutEquipmentSkinTextures(
                attached.get(), model, traits, mResourceSystem, npcBodyTexture, resolvedFaceDiffuseTexture);
            applyFaceGenEgmMorph(mResourceSystem, attached.get(), model, traits);
            overrideFalloutEquipmentSkinTextures(
                attached.get(), model, traits, mResourceSystem, npcBodyTexture, resolvedFaceDiffuseTexture);
            if (std::string_view(layer) == "headgear-final" && attached != nullptr)
            {
                DisableCullVisitor disableCull;
                attached->accept(disableCull);
                Log(Debug::Info) << "FNV/ESM4 diag: made headgear surface double-sided " << armor->mEditorId
                                 << " for " << traits.mEditorId
                                 << " gate=runtime-fnv-headgear-double-sided";
                ForceOpaqueNoBlendVisitor opaque;
                attached->accept(opaque);
                Log(Debug::Info) << "FNV/ESM4 diag: forced opaque no-blend headgear " << armor->mEditorId
                                 << " states=" << opaque.getAppliedCount() << " for " << traits.mEditorId;
            }
        };
        const auto insertClothingEquipment = [&](const ESM4::Clothing* clothing, const char* layer) {
            const std::string_view model = MWClass::ESM4Npc::chooseEquipmentModel(clothing, isFemale);
            const std::string_view builderCategory
                = isFonvEquipmentHeadStackLayer(layer) ? std::string_view("headgear")
                                                       : std::string_view("equipment-body");
            if (proofActor)
                Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: clothing " << clothing->mEditorId
                                 << " form=" << ESM::RefId(clothing->mId) << " model=" << model << " flags=0x"
                                 << std::hex << clothing->mClothingFlags << std::dec << " layer=" << layer;
            if (!falloutCharacterBuilderAllows(builderCategory, model))
            {
                logFalloutCharacterBuilderGate(false, builderCategory, model, mPtr, traits);
                Log(Debug::Info) << "FNV/ESM4 diag: skipped clothing " << clothing->mEditorId
                                 << " model=" << model << " layer=" << layer << " for "
                                 << traits.mEditorId
                                 << " status=intentionally-excluded-with-proof characterBuilderPhase="
                                 << characterBuilderPhase;
                return;
            }
            logFalloutCharacterBuilderGate(true, builderCategory, model, mPtr, traits);
            osg::ref_ptr<osg::Node> attached = insertPart(model);
            forceFalloutActorPartVisible(attached.get(), model, traits);
            overrideFalloutEquipmentSkinTextures(
                attached.get(), model, traits, mResourceSystem, npcBodyTexture, resolvedFaceDiffuseTexture);
            applyFaceGenEgmMorph(mResourceSystem, attached.get(), model, traits);
            overrideFalloutEquipmentSkinTextures(
                attached.get(), model, traits, mResourceSystem, npcBodyTexture, resolvedFaceDiffuseTexture);
            if (std::string_view(layer) == "headgear-final" && attached != nullptr)
            {
                DisableCullVisitor disableCull;
                attached->accept(disableCull);
                Log(Debug::Info) << "FNV/ESM4 diag: made headgear surface double-sided " << clothing->mEditorId
                                 << " for " << traits.mEditorId
                                 << " gate=runtime-fnv-headgear-double-sided";
                ForceOpaqueNoBlendVisitor opaque;
                attached->accept(opaque);
                Log(Debug::Info) << "FNV/ESM4 diag: forced opaque no-blend headgear " << clothing->mEditorId
                                 << " states=" << opaque.getAppliedCount() << " for " << traits.mEditorId;
            }
        };

        for (const ESM4::Armor* armor : equippedArmor)
            if (!fonvCoveredSlotsUseHeadStack(armor->mArmorFlags))
                insertArmorEquipment(armor, getFonvEquipmentAssemblyLayer(armor->mArmorFlags));
        for (const ESM4::Clothing* clothing : equippedClothing)
            if (!fonvCoveredSlotsUseHeadStack(clothing->mClothingFlags))
                insertClothingEquipment(clothing, getFonvEquipmentAssemblyLayer(clothing->mClothingFlags));

        if (const ESM4::Weapon* weapon = MWClass::ESM4Npc::getEquippedWeapon(mPtr))
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            osg::ref_ptr<osg::Node> attached;
            if (!falloutCharacterBuilderAllows("weapon", weapon->mModel))
            {
                logFalloutCharacterBuilderGate(false, "weapon", weapon->mModel, mPtr, traits);
                Log(Debug::Info) << "FNV/ESM4 diag: skipped equipped NPC weapon " << weapon->mEditorId
                                 << " model=" << weapon->mModel << " for " << traits.mEditorId
                                 << " status=intentionally-excluded-with-proof characterBuilderPhase="
                                 << characterBuilderPhase;
            }
            else
            {
                logFalloutCharacterBuilderGate(true, "weapon", weapon->mModel, mPtr, traits);
                attached = insertAttachedPart(weapon->mModel, "Weapon");
            }
            Log(Debug::Info) << "FNV/ESM4 diag: equipped NPC weapon " << weapon->mEditorId << " model="
                             << weapon->mModel << " damage=" << weapon->mData.damage << " for "
                             << traits.mEditorId << " attached=" << (attached != nullptr);
            Log(Debug::Info) << "FNV/ESM4 diag: weapon metadata " << weapon->mEditorId
                             << " ammo=" << ESM::RefId(weapon->mAmmo)
                             << " repairList=" << ESM::RefId(weapon->mRepairList)
                             << " equipType=" << ESM::RefId(weapon->mEquipType)
                             << " impactDataSet=" << ESM::RefId(weapon->mImpactDataSet)
                             << " worldModel=" << ESM::RefId(weapon->mWorldModel)
                             << " clipSize=" << static_cast<unsigned int>(weapon->mData.clipSize)
                             << " modItems=[" << ESM::RefId(weapon->mModItem[0]) << ","
                             << ESM::RefId(weapon->mModItem[1]) << "," << ESM::RefId(weapon->mModItem[2])
                             << "] sounds=[" << formatFalloutWeaponSoundRefs(*weapon) << "]";
            Log(Debug::Info) << "FNV/ESM4 diag: weapon sound files " << weapon->mEditorId << " ["
                             << formatFalloutWeaponSoundFiles(*weapon, *store) << "]";
        }

        for (const ESM4::Armor* armor : equippedArmor)
            if (fonvCoveredSlotsUseHeadStack(armor->mArmorFlags))
                insertArmorEquipment(armor, getFonvEquipmentAssemblyLayer(armor->mArmorFlags));
        for (const ESM4::Clothing* clothing : equippedClothing)
            if (fonvCoveredSlotsUseHeadStack(clothing->mClothingFlags))
                insertClothingEquipment(clothing, getFonvEquipmentAssemblyLayer(clothing->mClothingFlags));

        if (proofActor)
            Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: END parts assembled";
    }

    unsigned int ESM4NpcAnimation::insertHeadParts(const ESM4::Npc& traits,
        const std::vector<ESM::FormId>& partIds, std::set<uint32_t>& usedHeadPartTypes, uint32_t coveredBodySlots,
        unsigned int* visibleHairGeometry)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        unsigned int inserted = 0;
        const bool characterBuilderActive = isFalloutCharacterBuilderActive();
        const std::string characterBuilderPhase
            = characterBuilderActive ? getFalloutCharacterBuilderPhase() : std::string();
        for (ESM::FormId partId : partIds)
        {
            if (partId.isZeroOrUnset())
                continue;
            const ESM4::HeadPart* part = store->get<ESM4::HeadPart>().search(partId);
            if (!part)
            {
                Log(Debug::Error) << "Head part not found: " << ESM::RefId(partId);
                continue;
            }
            const bool miscPart = traits.mIsFONV && isFonvMiscHeadPart(*part);
            const bool useHatHairVariant
                = traits.mIsFONV && shouldUseFonvHatCompatibleHairVariant(*part, coveredBodySlots);
            const bool partTypeAlreadyUsed = usedHeadPartTypes.count(part->mType) != 0;
            if (miscPart || !partTypeAlreadyUsed)
            {
                const std::string_view builderCategory = "hair-beard";
                if (!falloutCharacterBuilderAllows(builderCategory, part->mModel))
                {
                    logFalloutCharacterBuilderGate(false, builderCategory, part->mModel, mPtr, traits);
                    Log(Debug::Info) << "FNV/ESM4 diag: skipped NPC head part " << part->mEditorId
                                     << " type=" << part->mType << " model=" << part->mModel
                                     << " for " << mPtr.getCellRef().getRefId()
                                     << " status=intentionally-excluded-with-proof characterBuilderPhase="
                                     << characterBuilderPhase;
                    continue;
                }
                logFalloutCharacterBuilderGate(true, builderCategory, part->mModel, mPtr, traits);
                const osg::Vec4f rawHairTint = getHairTint(traits);
                const osg::Vec4f hairTint = getFalloutRenderHairTint(traits);
                const osg::Vec4f* tint = miscPart || part->mType == ESM4::HeadPart::Type_Hair
                    || part->mType == ESM4::HeadPart::Type_FacialHair
                    || part->mType == ESM4::HeadPart::Type_Eyebrows
                    ? &hairTint
                    : nullptr;
                if (!miscPart)
                    usedHeadPartTypes.insert(part->mType);
                Log(Debug::Info) << "FNV/ESM4 diag: inserting NPC head part " << part->mEditorId << " type="
                                 << part->mType << " model=" << part->mModel << " rawHairTint=("
                                 << rawHairTint.x() << ", " << rawHairTint.y() << ", " << rawHairTint.z()
                                 << ") renderHairTint=(" << hairTint.x() << ", " << hairTint.y() << ", "
                                 << hairTint.z() << ") for " << mPtr.getCellRef().getRefId();
                osg::ref_ptr<osg::Node> attached = insertPart(part->mModel);
                applyFaceGenEgmMorph(mResourceSystem, attached.get(), part->mModel, traits);
                if (attached != nullptr)
                    applyFalloutDialogueMorph(mResourceSystem, this, attached.get(), part->mModel, traits);
                if (attached != nullptr && tint != nullptr)
                {
                    const bool hairTintModel = isFalloutHairTintModel(part->mModel);
                    const float emissionStrength = hairTintModel ? getFalloutHairEmissionStrength(*tint) : 0.f;
                    TintMaterialVisitor visitor(*tint, emissionStrength, hairTintModel, hairTintModel, hairTintModel);
                    attached->accept(visitor);
                    if (hairTintModel)
                        Log(Debug::Info) << "FNV/ESM4 diag: applied hair tint material " << part->mModel
                                         << " tint=(" << tint->x() << ", " << tint->y() << ", " << tint->z()
                                         << ") emissionStrength=" << emissionStrength
                                         << " preservedVertexIntensityArrays="
                                         << visitor.mPreservedVertexColorIntensityArrays << " for "
                                         << mPtr.getCellRef().getRefId();
                }
                if (useHatHairVariant)
                    hideFonvNoHatHairVariant(attached.get(), part->mModel, traits, coveredBodySlots);
                logFalloutFaceDrawableAudit(attached.get(), part->mModel, mPtr, "final-headpart");
                if (part->mType == ESM4::HeadPart::Type_Hair)
                {
                    const FalloutVisibleGeometryVisitor hairVisibility = countFalloutVisibleGeometry(attached.get());
                    if (visibleHairGeometry != nullptr)
                        *visibleHairGeometry += hairVisibility.mVisibleGeometryCount;
                    Log(Debug::Info) << "FNV/ESM4 diag: hair visibility proof " << part->mModel
                                     << " for " << mPtr.getCellRef().getRefId()
                                     << " visibleGeometry=" << hairVisibility.mVisibleGeometryCount
                                     << " visibleVertices=" << hairVisibility.mVisibleVertexCount
                                     << " runtime="
                                     << (hairVisibility.mVisibleGeometryCount > 0 ? "runtime-supported"
                                                                                  : "loaded-pending-runtime")
                                     << " gate=runtime-fnv-visible-hair-geometry";
                }
                if (isFonvFacialHairHeadPart(*part))
                    usedHeadPartTypes.insert(ESM4::HeadPart::Type_FacialHair);
                ++inserted;
                for (ESM::FormId extraPartId : part->mExtraParts)
                {
                    if (extraPartId.isZeroOrUnset())
                        continue;
                    const ESM4::HeadPart* extraPart = store->get<ESM4::HeadPart>().search(extraPartId);
                    if (!extraPart)
                    {
                        Log(Debug::Error) << "Extra head part not found: " << ESM::RefId(extraPartId);
                        continue;
                    }
                    const bool useExtraHatHairVariant
                        = traits.mIsFONV && shouldUseFonvHatCompatibleHairVariant(*extraPart, coveredBodySlots);
                    if (!falloutCharacterBuilderAllows(builderCategory, extraPart->mModel))
                    {
                        logFalloutCharacterBuilderGate(false, builderCategory, extraPart->mModel, mPtr, traits);
                        Log(Debug::Info) << "FNV/ESM4 diag: skipped extra NPC head part "
                                         << extraPart->mEditorId << " type=" << extraPart->mType
                                         << " model=" << extraPart->mModel << " for "
                                         << mPtr.getCellRef().getRefId()
                                         << " status=intentionally-excluded-with-proof characterBuilderPhase="
                                         << characterBuilderPhase;
                        continue;
                    }
                    logFalloutCharacterBuilderGate(true, builderCategory, extraPart->mModel, mPtr, traits);
                    osg::ref_ptr<osg::Node> extraAttached = insertPart(extraPart->mModel);
                    applyFaceGenEgmMorph(mResourceSystem, extraAttached.get(), extraPart->mModel, traits);
                    if (extraAttached != nullptr)
                        applyFalloutDialogueMorph(mResourceSystem, this, extraAttached.get(), extraPart->mModel, traits);
                    if (extraAttached != nullptr && tint != nullptr)
                    {
                        const bool hairTintModel = isFalloutHairTintModel(extraPart->mModel);
                        const float emissionStrength = hairTintModel ? getFalloutHairEmissionStrength(*tint) : 0.f;
                        TintMaterialVisitor visitor(
                            *tint, emissionStrength, hairTintModel, hairTintModel, hairTintModel);
                        extraAttached->accept(visitor);
                        if (hairTintModel)
                            Log(Debug::Info) << "FNV/ESM4 diag: applied hair tint material " << extraPart->mModel
                                             << " tint=(" << tint->x() << ", " << tint->y() << ", " << tint->z()
                                             << ") emissionStrength=" << emissionStrength
                                             << " preservedVertexIntensityArrays="
                                             << visitor.mPreservedVertexColorIntensityArrays << " for "
                                             << mPtr.getCellRef().getRefId();
                    }
                    if (useExtraHatHairVariant)
                        hideFonvNoHatHairVariant(extraAttached.get(), extraPart->mModel, traits, coveredBodySlots);
                    logFalloutFaceDrawableAudit(extraAttached.get(), extraPart->mModel, mPtr, "final-extra-headpart");
                    if (extraPart->mType == ESM4::HeadPart::Type_Hair)
                    {
                        const FalloutVisibleGeometryVisitor hairVisibility
                            = countFalloutVisibleGeometry(extraAttached.get());
                        if (visibleHairGeometry != nullptr)
                            *visibleHairGeometry += hairVisibility.mVisibleGeometryCount;
                        Log(Debug::Info) << "FNV/ESM4 diag: hair visibility proof " << extraPart->mModel
                                         << " for " << mPtr.getCellRef().getRefId()
                                         << " visibleGeometry=" << hairVisibility.mVisibleGeometryCount
                                         << " visibleVertices=" << hairVisibility.mVisibleVertexCount
                                         << " runtime="
                                         << (hairVisibility.mVisibleGeometryCount > 0 ? "runtime-supported"
                                                                                      : "loaded-pending-runtime")
                                         << " gate=runtime-fnv-visible-hair-geometry";
                    }
                    if (isFonvFacialHairHeadPart(*extraPart))
                        usedHeadPartTypes.insert(ESM4::HeadPart::Type_FacialHair);
                    ++inserted;
                }
            }
        }
        return inserted;
    }

    void ESM4NpcAnimation::updatePartsTES5(const ESM4::Npc& traits)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();

        const ESM4::Race* race = MWClass::ESM4Npc::getRace(mPtr);
        bool isFemale = MWClass::ESM4Npc::isFemale(mPtr);

        std::vector<const ESM4::ArmorAddon*> armorAddons;

        auto findArmorAddons = [&](const ESM4::Armor* armor) {
            for (ESM::FormId armaId : armor->mAddOns)
            {
                if (armaId.isZeroOrUnset())
                    continue;
                const ESM4::ArmorAddon* arma = store->get<ESM4::ArmorAddon>().search(armaId);
                if (!arma)
                {
                    Log(Debug::Error) << "ArmorAddon not found: " << ESM::RefId(armaId);
                    continue;
                }
                bool compatibleRace = arma->mRacePrimary == traits.mRace;
                for (auto r : arma->mRaces)
                    if (r == traits.mRace)
                        compatibleRace = true;
                if (compatibleRace)
                    armorAddons.push_back(arma);
            }
        };

        for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(mPtr))
            findArmorAddons(armor);
        if (!traits.mWornArmor.isZeroOrUnset())
        {
            if (const ESM4::Armor* armor = store->get<ESM4::Armor>().search(traits.mWornArmor))
                findArmorAddons(armor);
            else
                Log(Debug::Error) << "Worn armor not found: " << ESM::RefId(traits.mWornArmor);
        }
        if (!race->mSkin.isZeroOrUnset())
        {
            if (const ESM4::Armor* armor = store->get<ESM4::Armor>().search(race->mSkin))
                findArmorAddons(armor);
            else
                Log(Debug::Error) << "Skin not found: " << ESM::RefId(race->mSkin);
        }

        if (isFemale)
            std::sort(armorAddons.begin(), armorAddons.end(),
                [](auto x, auto y) { return x->mFemalePriority > y->mFemalePriority; });
        else
            std::sort(armorAddons.begin(), armorAddons.end(),
                [](auto x, auto y) { return x->mMalePriority > y->mMalePriority; });

        uint32_t usedParts = 0;
        for (const ESM4::ArmorAddon* arma : armorAddons)
        {
            const uint32_t covers = arma->mBodyTemplate.bodyPart;
            // if body is already covered, skip to avoid clipping
            if (covers & usedParts & ESM4::Armor::TES5_Body)
                continue;
            // if covers at least something that wasn't covered before - add model
            if (covers & ~usedParts)
            {
                usedParts |= covers;
                insertPart(isFemale ? arma->mModelFemale : arma->mModelMale);
            }
        }

        std::set<uint32_t> usedHeadPartTypes;
        if (usedParts & ESM4::Armor::TES5_Hair)
            usedHeadPartTypes.insert(ESM4::HeadPart::Type_Hair);
        insertHeadParts(traits, traits.mHeadParts, usedHeadPartTypes);
        insertHeadParts(traits, isFemale ? race->mHeadPartIdsFemale : race->mHeadPartIdsMale, usedHeadPartTypes);
    }
}
