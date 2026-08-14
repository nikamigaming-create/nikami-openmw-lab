Warning: truncated output (original token count: 177690)
Total output lines: 13265

#include "esm4npcanimation.hpp"

#include <components/esm4/loadarma.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadclfm.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadeyes.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadhair.hpp>
#include <components/esm4/loadhdpt.hpp>
#include <components/esm4/loadidle.hpp>
#include <components/esm4/script.hpp>
#include <components/esm4/loadidlm.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadpack.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadsndr.hpp>
#include <components/esm4/loadsoun.hpp>
#include <components/esm4/loadstat.hpp>
#include <components/esm4/loadweap.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <functional>
#include <set>
#include <stdexcept>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/attach.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwmechanics/character.hpp"
#include "../mwmechanics/creaturestats.hpp"

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/ComputeBoundsVisitor>
#include <osg/FrontFace>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/NodeCallback>
#include <osg/NodeVisitor>
#include <osg/PositionAttitudeTransform>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Switch>
#include <osg/TexEnv>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osgAnimation/Bone>
#include <osgAnimation/UpdateBone>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <istream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwclass/esm4npc.hpp"
#include "../mwdialogue/esm4dialogueutils.hpp"
#include "../mwclass/fnvsandbox.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/fnvplayerstate.hpp"
#include "../mwworld/timestamp.hpp"
#include "falloutweaponanimation.hpp"
#include "fonvpackageanimation.hpp"
#include "npcanimation.hpp"
#include "playervisualpolicy.hpp"
#include "util.hpp"
#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
        std::optional<float> readFalloutPipBoyFieldOfView(Resource::ResourceSystem* resourceSystem)
        {
            const VFS::Manager* const vfs = resourceSystem != nullptr ? resourceSystem->getVFS() : nullptr;
            const VFS::Path::Normalized esmPath("falloutnv.esm");
            if (vfs == nullptr || !vfs->exists(esmPath))
                return std::nullopt;

            constexpr std::string_view directoryPrefix = "DIR: ";
            const std::string archive = vfs->getArchive(esmPath);
            if (!archive.starts_with(directoryPrefix))
                return std::nullopt;

            const std::filesystem::path iniPath
                = std::filesystem::path(archive.substr(directoryPrefix.size())).parent_path() / "Fallout_default.ini";
            std::ifstream stream(iniPath);
            std::string line;
            while (stream && std::getline(stream, line))
            {
                const std::size_t separator = line.find('=');
                if (separator == std::string::npos)
                    continue;
                std::string key = line.substr(0, separator);
                key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char value) {
                    return std::isspace(value) != 0;
                }), key.end());
                Misc::StringUtils::lowerCaseInPlace(key);
                if (key != "fpipboy1stpersonfov")
                    continue;
                try
                {
                    const float value = std::stof(line.substr(separator + 1));
                    if (std::isfinite(value) && value > 0.f && value < 180.f)
                    {
                        // This value drives Fallout's first-person model projection, not the
                        // world-camera projection. Retained xNVSE Save330 frames show the
                        // model callback changing from 55 to the authored value 47 directly.
                        // Converting 47 through the 4:3 world-FOV helper yields 36.1233 and
                        // incorrectly enlarges the wrist model.
                        const float verticalFov = value;
                        Log(Debug::Info) << "FNV Pip-Boy projection: referenceFov=" << value
                                         << " verticalFov=" << verticalFov
                                         << " source=" << iniPath.string()
                                         << " key=fPipboy1stPersonFOV";
                        return verticalFov;
                    }
                }
                catch (const std::exception&)
                {
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        class FirstPersonArmorArmsOnlyVisitor final : public osg::NodeVisitor
        {
        public:
            FirstPersonArmorArmsOnlyVisitor()
                : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                {
                    osg::Drawable* drawable = geode.getDrawable(i);
                    if (drawable != nullptr)
                        filter(*drawable);
                }
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { filter(drawable); }

            std::size_t mKept = 0;
            std::size_t mHidden = 0;

        private:
            void filter(osg::Drawable& drawable)
            {
                SceneUtil::RigGeometry* const rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable);
                if (rig == nullptr)
                    return;

                const std::string name = Misc::StringUtils::lowerCase(drawable.getName());
                const bool genericArms = name == "arms" || Misc::StringUtils::ciStartsWith(name, "arms:");
                // Only the first-person Arms partition belongs in this actor-space
                // composition. Retained native review rejected the outfit's two
                // PipBoyOn body partitions: forcing them into the first-person rig
                // produced displaced black geometry and did not repair either cuff.
                const bool keep = genericArms;
                if (keep)
                    ++mKept;
                else
                {
                    drawable.setNodeMask(0u);
                    ++mHidden;
                }
            }
        };

        bool worldViewerEnvEnabled(const char* name)
        {
            const char* value = std::getenv(name);
            return value != nullptr && *value != '\0' && value[0] != '0';
        }

        bool worldViewerActorTelemetryEnabled()
        {
            return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_ACTOR_TELEMETRY")
                || worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_TELEMETRY");
        }

        bool worldViewerSkipMissingActorParts()
        {
            return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_SKIP_MISSING_ACTOR_PARTS");
        }

        bool worldViewerSkipUnmappedRiggedActorParts()
        {
            return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_SKIP_UNMAPPED_RIGGED_ACTOR_PARTS");
        }

        bool worldViewerForceFlatActorMaterials()
        {
            return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FORCE_FLAT_ACTOR_MATERIALS");
        }

        bool worldViewerForceFullbrightActorMaterials()
        {
            return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FULLBRIGHT_ACTOR_MATERIALS");
        }

        const char* worldViewerNpcGameTag(const ESM4::Npc& npc)
        {
            if (npc.mIsTES4)
                return "TES4";
            if (npc.mIsFO3)
                return "FO3";
            if (npc.mIsFONV)
                return "FONV";
            if (npc.mIsFO4)
                return "FO4";
            if (npc.mIsStarfield)
                return "STARFIELD";
            return "TES5_OR_UNKNOWN";
        }

        void logWorldViewerActorLedger(
            const MWWorld::Ptr& ptr, std::string_view phase, std::string_view details = {})
        {
            if (!worldViewerActorTelemetryEnabled())
                return;

            const ESM::Position& pos = ptr.getRefData().getPosition();
            Log(Debug::Info) << "World viewer actor ledger: phase=" << phase
                             << " ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                             << " base=" << ptr.getCellRef().getRefId().toDebugString()
                             << " type=\"" << ptr.getTypeDescription() << "\""
                             << " name=\"" << ptr.getClass().getName(ptr) << "\""
                             << " pos=(" << pos.pos[0] << "," << pos.pos[1] << "," << pos.pos[2] << ") "
                             << details;
        }

        template <class T>
        const T* searchEsm4ViewerRecordWithLocalFallback(
            const MWWorld::ESMStore& store, ESM::FormId id, ESM::FormId* resolvedId = nullptr)
        {
            if (const T* record = store.get<T>().search(id))
            {
                if (resolvedId)
                    *resolvedId = id;
                return record;
            }

            if (!id.hasContentFile() || id.mContentFile == 0)
                return nullptr;

            ESM::FormId localId = id;
            localId.mContentFile = 0;
            if (const T* record = store.get<T>().search(localId))
            {
                if (resolvedId)
                    *resolvedId = localId;
                return record;
            }

            return nullptr;
        }

        class TintMaterialVisitor : public osg::NodeVisitor
        {
        public:
            TintMaterialVisitor(const osg::Vec4f& tint, float emissionStrength = 0.f,
                bool replaceVertexRgbWithTint = false, bool preserveVertexRgb = false)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mTint(tint)
                , mEmissionStrength(emissionStrength)
                , mReplaceVertexRgbWithTint(replaceVertexRgbWithTint)
                , mPreserveVertexRgb(preserveVertexRgb)
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
                // Geode::traverse visits its drawables and dispatches the
                // Drawable overload below.  Applying them here as well
                // multiplied vertex colours twice per visitor pass.
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
                    for (osg::Vec4f& color : *colors)
                    {
                        if (mReplaceVertexRgbWithTint)
                            color.set(mTint.x(), mTint.y(), mTint.z(), color.w());
                        else
                        {
                            color.x() *= mTint.x();
                            color.y() *= mTint.y();
                            color.z() *= mTint.z();
                        }
                    }
                    colors->dirty();
                    return true;
                }

                if (osg::Vec4ubArray* colors = dynamic_cast<osg::Vec4ubArray*>(existingColors))
                {
                    for (osg::Vec4ub& color : *colors)
                    {
                        if (mReplaceVertexRgbWithTint)
                        {
                            color.set(static_cast<unsigned char>(std::clamp(mTint.r() * 255.f, 0.f, 255.f)),
                                static_cast<unsigned char>(std::clamp(mTint.g() * 255.f, 0.f, 255.f)),
                                static_cast<unsigned char>(std::clamp(mTint.b() * 255.f, 0.f, 255.f)), color.a());
                        }
                        else
                        {
                            color.r() = static_cast<unsigned char>(std::clamp(color.r() * mTint.x(), 0.f, 255.f));
                            color.g() = static_cast<unsigned char>(std::clamp(color.g() * mTint.y(), 0.f, 255.f));
                            color.b() = static_cast<unsigned char>(std::clamp(color.b() * mTint.z(), 0.f, 255.f));
                        }
                    }
                    colors->dirty();
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
                material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
                material->setDiffuse(osg::Material::FRONT_AND_BACK, mTint);
                material->setAmbient(osg::Material::FRONT_AND_BACK, mTint);
                if (neutralTint)
                {
                    const float emission = std::min(mEmissionStrength, 1.f);
                    material->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(emission, emission, emission, 1.f));
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
                    if (!neutralTint && !mPreserveVertexRgb && neutralizeVertexColors(*geometry))
                        ++mNeutralizedVertexColorArrays;
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* source = rig->getSourceGeometry())
                    {
                        applyTint(source->getOrCreateStateSet());
                        if (!neutralTint && !mPreserveVertexRgb && neutralizeVertexColors(*source))
                            ++mNeutralizedVertexColorArrays;
                    }
                    for (unsigned int i = 0; i < 2; ++i)
                        if (osg::Geometry* geometry = rig->getRenderGeometry(i))
                        {
                            applyTint(geometry->getOrCreateStateSet());
                            if (!neutralTint && !mPreserveVertexRgb && neutralizeVertexColors(*geometry))
                                ++mNeutralizedVertexColorArrays;
                        }
                }
            }

            osg::Vec4f mTint;
            float mEmissionStrength = 0.f;
            bool mReplaceVertexRgbWithTint = false;
            bool mPreserveVertexRgb = false;

        public:
            mutable unsigned int mNeutralizedVertexColorArrays = 0;
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

        class FlatActorMaterialVisitor : public osg::NodeVisitor
        {
        public:
            explicit FlatActorMaterialVisitor(const osg::Vec4f& color)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mColor(color)
            {
            }

            void apply(osg::Node& node) override
            {
                applyStateSet(node.getOrCreateStateSet());
                traverse(node);
            }

            void apply(osg::Geode& geode) override
            {
                applyStateSet(geode.getOrCreateStateSet());
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        applyDrawable(*drawable);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

            unsigned int getStateSetCount() const { return mStateSets; }
            unsigned int getGeometryCount() const { return mGeometries; }

        private:
            void applyStateSet(osg::StateSet* stateSet)
            {
                if (stateSet == nullptr)
                    return;

                osg::ref_ptr<osg::Material> material = new osg::Material;
                material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
                material->setDiffuse(osg::Material::FRONT_AND_BACK, mColor);
                material->setAmbient(osg::Material::FRONT_AND_BACK, mColor);
                material->setEmission(osg::Material::FRONT_AND_BACK, mColor);
                material->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
                material->setShininess(osg::Material::FRONT_AND_BACK, 0.f);

                stateSet->setAttributeAndModes(material, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateSet->setAttributeAndModes(new osg::Program, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                stateSet->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                stateSet->setMode(GL_ALPHA_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                for (unsigned int unit = 0; unit < 8; ++unit)
                    stateSet->setTextureMode(unit, GL_TEXTURE_2D, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                stateSet->setRenderingHint(osg::StateSet::DEFAULT_BIN);
                ++mStateSets;
            }

            void flattenGeometry(osg::Geometry& geometry)
            {
                osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
                colors->push_back(mColor);
                geometry.setColorArray(colors);
                geometry.setColorBinding(osg::Geometry::BIND_OVERALL);
                geometry.dirtyDisplayList();
                geometry.dirtyBound();
                ++mGeometries;
            }

            void applyDrawable(osg::Drawable& drawable)
            {
                applyStateSet(drawable.getOrCreateStateSet());
                if (osg::Geometry* geometry = drawable.asGeometry())
                    flattenGeometry(*geometry);
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* source = rig->getSourceGeometry())
                    {
                        applyStateSet(source->getOrCreateStateSet());
                        flattenGeometry(*source);
                    }
                    for (unsigned int i = 0; i < 2; ++i)
                        if (osg::Geometry* geometry = rig->getRenderGeometry(i))
                        {
                            applyStateSet(geometry->getOrCreateStateSet());
                            flattenGeometry(*geometry);
                        }
                }
            }

            osg::Vec4f mColor;
            unsigned int mStateSets = 0;
            unsigned int mGeometries = 0;
        };

        class FullbrightTexturedActorMaterialVisitor : public osg::NodeVisitor
        {
        public:
            FullbrightTexturedActorMaterialVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                applyStateSet(node.getOrCreateStateSet());
                traverse(node);
            }

            void apply(osg::Geode& geode) override
            {
                applyStateSet(geode.getOrCreateStateSet());
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        applyDrawable(*drawable);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

            unsigned int getStateSetCount() const { return mStateSets; }
            unsigned int getGeometryCount() const { return mGeometries; }
            unsigned int getTextureUnitsKept() const { return mTextureUnitsKept; }
            unsigned int getTextureUnitsDisabled() const { return mTextureUnitsDisabled; }
            unsigned int getTextureEnvsForced() const { return mTextureEnvsForced; }

        private:
            void applyStateSet(osg::StateSet* stateSet)
            {
                if (stateSet == nullptr)
                    return;

                osg::ref_ptr<osg::Material> material = new osg::Material;
                material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
                material->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
                material->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
                material->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
                material->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
                material->setShininess(osg::Material::FRONT_AND_BACK, 0.f);

                stateSet->setAttributeAndModes(material, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateSet->setAttributeAndModes(new osg::Program, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                for (unsigned int unit = 0; unit < 8; ++unit)
                {
                    if (stateSet->getTextureAttribute(unit, osg::StateAttribute::TEXTURE) == nullptr)
                        continue;

                    if (unit == 0)
                    {
                        stateSet->setTextureMode(unit, GL_TEXTURE_2D, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                        stateSet->setTextureAttributeAndModes(
                            unit, new osg::TexEnv(osg::TexEnv::REPLACE), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                        ++mTextureUnitsKept;
                        ++mTextureEnvsForced;
                    }
                    else
                    {
                        stateSet->setTextureMode(unit, GL_TEXTURE_2D, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                        ++mTextureUnitsDisabled;
                    }
                }
                ++mStateSets;
            }

            void whitenGeometry(osg::Geometry& geometry)
            {
                osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
                colors->push_back(osg::Vec4f(1.f, 1.f, 1.f, 1.f));
                geometry.setColorArray(colors);
                geometry.setColorBinding(osg::Geometry::BIND_OVERALL);
                geometry.dirtyDisplayList();
                geometry.dirtyBound();
                ++mGeometries;
            }

            void applyDrawable(osg::Drawable& drawable)
            {
                applyStateSet(drawable.getOrCreateStateSet());
                if (osg::Geometry* geometry = drawable.asGeometry())
                    whitenGeometry(*geometry);
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* source = rig->getSourceGeometry())
                    {
                        applyStateSet(source->getOrCreateStateSet());
                        whitenGeometry(*source);
                    }
                    for (unsigned int i = 0; i < 2; ++i)
                        if (osg::Geometry* geometry = rig->getRenderGeometry(i))
                        {
                            applyStateSet(geometry->getOrCreateStateSet());
                            whitenGeometry(*geometry);
                        }
                }
            }

            unsigned int mStateSets = 0;
            unsigned int mGeometries = 0;
            unsigned int mTextureUnitsKept = 0;
            unsigned int mTextureUnitsDisabled = 0;
            unsigned int mTextureEnvsForced = 0;
        };

        void applyWorldViewerFlatActorMaterials(osg::Node* root, const MWWorld::Ptr& ptr, std::string_view phase)
        {
            if (!worldViewerForceFlatActorMaterials() || root == nullptr)
                return;

            FlatActorMaterialVisitor visitor(osg::Vec4f(0.86f, 0.82f, 0.74f, 1.f));
            root->accept(visitor);

            std::ostringstream details;
            details << "phase=\"" << phase << "\""
                    << " stateSets=" << visitor.getStateSetCount()
                    << " geometries=" << visitor.getGeometryCount();
            logWorldViewerActorLedger(ptr, "flat-actor-material", details.str());
        }

        void applyWorldViewerFullbrightActorMaterials(osg::Node* root, const MWWorld::Ptr& ptr, std::string_view phase)
        {
            if (worldViewerForceFlatActorMaterials() || !worldViewerForceFullbrightActorMaterials() || root == nullptr)
                return;

            FullbrightTexturedActorMaterialVisitor visitor;
            root->accept(visitor);

            std::ostringstream details;
            details << "phase=\"" << phase << "\""
                    << " stateSets=" << visitor.getStateSetCount()
                    << " geometries=" << visitor.getGeometryCount()
                    << " textureUnitsKept=" << visitor.getTextureUnitsKept()
                    << " textureUnitsDisabled=" << visitor.getTextureUnitsDisabled()
                    << " textureEnvsForced=" << visitor.getTextureEnvsForced();
            logWorldViewerActorLedger(ptr, "fullbright-actor-material", details.str());
        }


        class ActorVisualAuditVisitor : public osg::NodeVisitor
        {
        public:
            ActorVisualAuditVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                ++mNodes;
                if (node.getNodeMask() == 0)
                    ++mZeroMaskNodes;
                if (osg::Drawable* drawable = node.asDrawable())
                    auditDrawable(*drawable);
                traverse(node);
            }

            void apply(osg::Geode& geode) override
            {
                ++mNodes;
                ++mGeodes;
                if (geode.getNodeMask() == 0)
                    ++mZeroMaskNodes;
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        auditDrawable(*drawable);
                // Geode traversal visits the same drawable objects again on OSG builds
                // where Drawable participates in NodeVisitor dispatch. We already account
                // for every drawable above, so traversing here doubled all assembly counts.
            }

            void apply(osg::Drawable& drawable) override
            {
                ++mNodes;
                auditDrawable(drawable);
            }

            void apply(osg::Geometry& geometry) override
            {
                ++mNodes;
                auditDrawable(geometry);
            }

            unsigned int mNodes = 0;
            unsigned int mZeroMaskNodes = 0;
            unsigned int mGeodes = 0;
            unsigned int mDrawables = 0;
            unsigned int mGeometry = 0;
            unsigned int mRigGeometry = 0;
            unsigned int mRigGeometryHolder = 0;
            unsigned int mRigRenderGeometry = 0;
            unsigned int mMorphGeometry = 0;
            unsigned int mMorphSourceGeometry = 0;
            unsigned int mPrimitiveSets = 0;
            unsigned int mRenderableGeometry = 0;
            unsigned int mRenderableGoreGeometry = 0;

        private:
            static bool isGoreGeometryName(std::string_view name)
            {
                std::string lower(name);
                Misc::StringUtils::lowerCaseInPlace(lower);
                return lower.find("meatcap") != std::string::npos
                    || lower.find("gorecap") != std::string::npos
                    || lower.find("bodycaps") != std::string::npos
                    || lower.find("limbcaps") != std::string::npos
                    || lower.find("meatneck") != std::string::npos
                    || lower.find("meathead") != std::string::npos;
            }

            void auditGeometry(const osg::Geometry* geometry, std::string_view fallbackName)
            {
                if (geometry == nullptr)
                    return;
                const unsigned int primitiveSets = geometry->getNumPrimitiveSets();
                mPrimitiveSets += primitiveSets;
                if (primitiveSets == 0 || geometry->getVertexArray() == nullptr
                    || geometry->getVertexArray()->getNumElements() == 0)
                    return;
                ++mRenderableGeometry;
                if (isGoreGeometryName(geometry->getName()) || isGoreGeometryName(fallbackName))
                    ++mRenderableGoreGeometry;
            }

            void auditDrawable(osg::Drawable& drawable)
            {
                ++mDrawables;
                if (osg::Geometry* geometry = drawable.asGeometry())
                {
                    ++mGeometry;
                    auditGeometry(geometry, drawable.getName());
                }
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    ++mRigGeometry;
                    if (rig->getSourceGeometry() != nullptr)
                    {
                        ++mGeometry;
                        auditGeometry(rig->getSourceGeometry(), drawable.getName());
                    }
                    for (unsigned int i = 0; i < 2; ++i)
                        if (rig->getRenderGeometry(i) != nullptr)
                            ++mRigRenderGeometry;
                }
                if (SceneUtil::RigGeometryHolder* holder = dynamic_cast<SceneUtil::RigGeometryHolder*>(&drawable))
                {
                    ++mRigGeometryHolder;
                    if (holder->getSourceRigGeometry() != nullptr)
                        ++mGeometry;
                    for (unsigned int i = 0; i < 2; ++i)
                        if (holder->getGeometry(i) != nullptr)
                            ++mRigRenderGeometry;
                }
                if (SceneUtil::MorphGeometry* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
                {
                    ++mMorphGeometry;
                    if (morph->getSourceGeometry() != nullptr)
                    {
                        ++mMorphSourceGeometry;
                        ++mGeometry;
                        auditGeometry(morph->getSourceGeometry(), drawable.getName());
                    }
                }
            }
        };

        std::string makeActorVisualAuditDetails(osg::Node* node)
        {
            if (node == nullptr)
                return " visualNode=0";

            ActorVisualAuditVisitor visitor;
            node->accept(visitor);

            osg::ComputeBoundsVisitor boundsVisitor;
            node->accept(boundsVisitor);
            const osg::BoundingBox box = boundsVisitor.getBoundingBox();
            const osg::BoundingSphere sphere = node->getBound();

            std::ostringstream details;
            details << " visualNode=1"
                    << " visualNodes=" << visitor.mNodes
                    << " visualZeroMaskNodes=" << visitor.mZeroMaskNodes
                    << " visualGeodes=" << visitor.mGeodes
                    << " visualDrawables=" << visitor.mDrawables
                    << " visualGeometry=" << visitor.mGeometry
                    << " visualRigGeometry=" << visitor.mRigGeometry
                    << " visualRigGeometryHolder=" << visitor.mRigGeometryHolder
                    << " visualRigRenderGeometry=" << visitor.mRigRenderGeometry
                    << " visualMorphGeometry=" << visitor.mMorphGeometry
                    << " visualMorphSourceGeometry=" << visitor.mMorphSourceGeometry
                    << " visualPrimitiveSets=" << visitor.mPrimitiveSets
                    << " visualRenderableGeometry=" << visitor.mRenderableGeometry
                    << " visualRenderableGoreGeometry=" << visitor.mRenderableGoreGeometry
                    << " visualRootMask=0x" << std::hex << node->getNodeMask() << std::dec
                    << " visualBoundValid=" << sphere.valid()
                    << " visualBoundRadius=" << (sphere.valid() ? sphere.radius() : 0.f);
            if (sphere.valid())
                details << " visualBoundCenter=(" << sphere.center().x() << "," << sphere.center().y() << ","
                        << sphere.center().z() << ")";
            if (box.valid())
            {
                const osg::Vec3f min = box._min;
                const osg::Vec3f max = box._max;
                details << " visualBoxValid=1"
                        << " visualBoxMin=(" << min.x() << "," << min.y() << "," << min.z() << ")"
                        << " visualBoxMax=(" << max.x() << "," << max.y() << "," << max.z() << ")";
            }
            else
                details << " visualBoxValid=0";
            return details.str();
        }

        bool actorPartHasRenderableGeometry(osg::Node* node)
        {
            if (node == nullptr)
                return false;
            ActorVisualAuditVisitor visitor;
            node->accept(visitor);
            return visitor.mRenderableGeometry > 0;
        }

        class ForceActorPartMaskVisitor : public osg::NodeVisitor
        {
        public:
            ForceActorPartMaskVisitor(osg::Node::NodeMask mask)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mMask(mask)
            {
            }

            void apply(osg::Node& node) override
            {
                node.setNodeMask(mMask);
                ++mNodes;
                traverse(node);
            }

            void apply(osg::Geode& geode) override
            {
                geode.setNodeMask(mMask);
                ++mNodes;
                ++mGeodes;
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        applyDrawable(*drawable);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

            unsigned int mNodes = 0;
            unsigned int mGeodes = 0;
            unsigned int mDrawables = 0;

        private:
            void applyDrawable(osg::Drawable& drawable)
            {
                drawable.setNodeMask(mMask);
                ++mDrawables;
            }

            osg::Node::NodeMask mMask = 0;
        };

        void forceWorldViewerActorPartMask(osg::Node* attached, std::string_view model, const MWWorld::Ptr& ptr)
        {
            if (attached == nullptr || !worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FORCE_ACTOR_PART_MASK"))
                return;

            ForceActorPartMaskVisitor visitor(Mask_Actor);
            attached->accept(visitor);

            std::ostringstream details;
            details << "model=\"" << model << "\""
                    << " forcedMask=0x" << std::hex << Mask_Actor << std::dec
                    << " nodes=" << visitor.mNodes
                    << " geodes=" << visitor.mGeodes
                    << " drawables=" << visitor.mDrawables
                    << makeActorVisualAuditDetails(attached);
            logWorldViewerActorLedger(ptr, "actor-part-mask", details.str());
        }

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

                osg::ref_ptr<osg::AlphaFunc> alphaFunc = new osg::AlphaFunc(osg::AlphaFunc::GREATER, 0.18f);
                osg::ref_ptr<osg::BlendFunc> blendFunc
                    = new osg::BlendFunc(osg::BlendFunc::SRC_ALPHA, osg::BlendFunc::ONE_MINUS_SRC_ALPHA);
                stateSet->setMode(GL_BLEND, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateSet->setMode(GL_ALPHA_TEST, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateSet->setAttributeAndModes(alphaFunc, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateSet->setAttributeAndModes(blendFunc, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateSet->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                stateSet->setDefine("FORCE_OPAQUE", "0", osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
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
                    const float loudness = forceOpen
                        ? 1.f
                        : MWBase::Environment::get().getSoundManager()->getSaySoundLoudness(mActor);
                    const float open = forceOpen ? 1.f : std::clamp(loudness * 5.0f, 0.f, 0.65f);
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
                                         << " model=" << mModel << " loudness=" << loudness << " open=" << open
                                         << " force=" << forceOpen
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
            if (!traits.mHairColourId.isZeroOrUnset())
            {
                const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
                if (store != nullptr)
                {
                    if (const ESM4::Colour* colour
                        = searchEsm4ViewerRecordWithLocalFallback<ESM4::Colour>(*store, traits.mHairColourId))
                    {
                        return osg::Vec4f(colour->mColour.red / 255.f, colour->mColour.green / 255.f,
                            colour->mColour.blue / 255.f, 1.f);
                    }
                }
            }
            return osg::Vec4f(traits.mHairColour.red / 255.f, traits.mHairColour.green / 255.f,
                traits.mHairColour.blue / 255.f, 1.f);
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

        std::string getStarfieldGeneratedFaceModel(const ESM4::Npc& traits)
        {
            if (!traits.mIsStarfield)
                return {};

            std::uint32_t faceFormIndex = traits.mId.mIndex;
            if (Misc::StringUtils::ciEqual(traits.mEditorId, "Player"))
            {
                if (const char* overrideId = std::getenv("OPENMW_WORLD_VIEWER_STARFIELD_PLAYER_FACE_FORM_ID"))
                {
                    char* end = nullptr;
                    const unsigned long parsed = std::strtoul(overrideId, &end, 0);
                    if (end != overrideId && *end == '\0' && parsed <= std::numeric_limits<std::uint32_t>::max())
                        faceFormIndex = static_cast<std::uint32_t>(parsed);
                }
            }

            std::ostringstream stream;
            stream << "actors/character/facegendata/facegeom/starfield.esm/"
                   << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << faceFormIndex << ".nif";
            return stream.str();
        }

        bool isEasyPeteProofActor(const ESM4::Npc& traits)
        {
            return Misc::StringUtils::ciEqual(traits.mEditorId, "GSEasyPete")
                || traits.mId.mIndex == 0x00104c7f;
        }

        bool isFonvProofTargetActor(const MWWorld::Ptr& ptr, const ESM4::Npc& traits)
        {
            if (std::getenv("OPENMW_FNV_PROOF_ONLY_EASY_PETE") != nullptr)
                return isEasyPeteProofActor(traits);

            const char* target = std::getenv("OPENMW_FNV_PROOF_TARGET_NPC");
            if (target == nullptr || target[0] == '\0')
                return true;

            std::string refId;
            if (ptr.getCell() != nullptr)
                refId = ptr.getCellRef().getRefNum().toString("FormId:");

            return Misc::StringUtils::ciEqual(traits.mEditorId, target)
                || Misc::StringUtils::ciEqual(ESM::RefId(traits.mId).toDebugString(), target)
                || Misc::StringUtils::ciEqual(formatFalloutFormIndex(traits.mId), target)
                || (!refId.empty() && Misc::StringUtils::ciEqual(refId, target));
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
            stream << resolvePackageReferenceDetail(store,â€¦147690 tokens truncatedâ€¦);
            const bool renderable = actorPartHasRenderableGeometry(attached.get());
            if (renderable)
                ++attachedRaceBodyParts;
            if (attached != nullptr && isFonvRaceSkinSurface(bodyPart.mesh))
            {
                forceFalloutActorPartVisible(attached.get(), bodyPart.mesh, traits);
                if (npcGeneratedSkinFaceGen0 != nullptr || !npcBodyDetailTexture.empty())
                {
                    Log(Debug::Verbose) << "FNV/ESM4 actor skin: binding generated FaceGen0 and "
                                     << (npcGeneratedFaceGen1 != nullptr ? "generated" : "raw NPC bodymod")
                                     << " FaceGen1 " << npcBodyDetailTexture << " on " << bodyPart.mesh
                                     << " for " << traits.mEditorId;
                    overrideFalloutPartFaceGenTextures({}, npcGeneratedSkinFaceGen0.get(), npcBodyDetailTexture,
                        npcGeneratedFaceGen1.get(), mResourceSystem, *attached);
                }
                if (!npcBodyNormalTexture.empty())
                {
                    Log(Debug::Verbose) << "FNV/ESM4 diag: overriding NPC part normal texture " << npcBodyNormalTexture
                                     << " on " << bodyPart.mesh << " for " << traits.mEditorId;
                    overrideFalloutPartNormalTexture(npcBodyNormalTexture, mResourceSystem, *attached);
                }
                logFalloutFaceDrawableAudit(attached.get(), bodyPart.mesh, mPtr, "final-race-skin");
            }
            std::ostringstream details;
            details << "game=FONV npc=\"" << traits.mEditorId << "\" kind=race-body"
                    << " index=" << bodyIndex << " model=\"" << bodyPart.mesh << "\""
                    << " required=1 coveredByEquipment=0 attached=" << (attached != nullptr)
                    << " renderable=" << renderable << makeActorVisualAuditDetails(attached.get());
            logWorldViewerActorLedger(mPtr, "actor-part-manifest", details.str());
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
        const std::vector<ESM4::Race::BodyPart>& raceHeadParts = isFemale ? race->mHeadPartsFemale : race->mHeadParts;
        for (std::size_t i = 0; i < raceHeadParts.size(); ++i)
        {
            const ESM4::Race::BodyPart& headPart = raceHeadParts[i];
            const bool eyePart = isFonvEyePart(i);
            const bool headSurface = isFonvHeadSurfacePart(i);
            const bool required = isRenderableFonvActorModel(headPart.mesh);
            if (required)
                ++requiredRaceFaceParts;
            const std::string_view texture = eyePart && !eyeTexture.empty() ? eyeTexture
                                                                            : headPart.texture;
            osg::ref_ptr<osg::Node> attached = insertPart(headPart.mesh, nullptr, texture);
            const bool renderable = actorPartHasRenderableGeometry(attached.get());
            if (required && renderable)
                ++attachedRaceFaceParts;
            if (i < 8)
            {
                raceFacePartAttached[i] = attached != nullptr;
                raceFacePartHasMesh[i] = !headPart.mesh.empty();
            }
            Log(Debug::Verbose) << "FNV/ESM4 diag: race face part " << getFonvRaceHeadPartRole(i)
                             << " index=" << i << " mesh=" << headPart.mesh << " texture="
                             << (texture.empty() ? std::string("<none>") : std::string(texture))
                             << " attached=" << (attached != nullptr) << " status="
                             << getFonvFacePartStatus(attached != nullptr, !headPart.mesh.empty()) << " for "
                             << traits.mEditorId;
            if (headSurface && attached != nullptr)
            {
                forceFalloutActorPartVisible(attached.get(), headPart.mesh, traits);
                applyFaceGenEgmMorph(mResourceSystem, attached.get(), headPart.mesh, traits, {}, race, isFemale);
                if (!texture.empty())
                    overrideFalloutPartDiffuseTexture(texture, mResourceSystem, *attached);
                if (!npcFaceDetailTexture.empty())
                {
                    Log(Debug::Verbose) << "FNV/ESM4 diag: binding NPC face FaceGen0 " << npcFaceDetailTexture
                                     << " on " << headPart.mesh << " for " << traits.mEditorId;
                }
                if (!npcBodyDetailTexture.empty())
                    Log(Debug::Verbose) << "FNV/ESM4 diag: binding "
                                     << (npcGeneratedFaceGen1 != nullptr ? "generated" : "raw NPC bodymod")
                                     << " FaceGen1 " << npcBodyDetailTexture << " on " << headPart.mesh
                                     << " for " << traits.mEditorId;
                overrideFalloutPartFaceGenTextures(npcFaceDetailTexture, nullptr, npcBodyDetailTexture,
                    npcGeneratedFaceGen1.get(), mResourceSystem, *attached);
                if (!npcFaceNormalTexture.empty())
                {
                    Log(Debug::Verbose) << "FNV/ESM4 diag: overriding NPC part normal texture " << npcFaceNormalTexture
                                     << " on " << headPart.mesh << " for " << traits.mEditorId;
                    overrideFalloutPartNormalTexture(npcFaceNormalTexture, mResourceSystem, *attached);
                }
                DisableCullVisitor visitor;
                attached->accept(visitor);
                Log(Debug::Verbose) << "FNV/ESM4 diag: made head skin surface double-sided " << headPart.mesh
                                 << " for " << traits.mEditorId;
            }
            if (attached != nullptr)
                applyFalloutProofTriStaticMorph(mResourceSystem, attached.get(), headPart.mesh, traits);
            if (attached != nullptr)
                applyFalloutDialogueMorph(mResourceSystem, mPtr, this, attached.get(), headPart.mesh, traits);
            logFalloutFaceDrawableAudit(attached.get(), headPart.mesh, mPtr, "final-race-head");
            std::ostringstream details;
            details << "game=FONV npc=\"" << traits.mEditorId << "\" kind=race-face"
                    << " role=\"" << getFonvRaceHeadPartRole(i) << "\""
                    << " index=" << i << " model=\"" << headPart.mesh << "\""
                    << " required=" << required << " attached=" << (attached != nullptr)
                    << " renderable=" << renderable << makeActorVisualAuditDetails(attached.get());
            logWorldViewerActorLedger(mPtr, "actor-part-manifest", details.str());
        }

        const unsigned int requiredNpcHeadPartRecords = static_cast<unsigned int>(std::count_if(
            traits.mHeadParts.begin(), traits.mHeadParts.end(), [](ESM::FormId part) { return !part.isZeroOrUnset(); }));
        std::set<uint32_t> usedHeadPartTypes;
        std::set<uint32_t> attachedHeadPartTypes;
        unsigned int attachedRequestedHeadParts = 0;
        unsigned int insertedHeadParts
            = insertHeadParts(traits, traits.mHeadParts, usedHeadPartTypes, &attachedHeadPartTypes,
                &attachedRequestedHeadParts, race, isFemale);
        const auto equippedArmorForHair = MWClass::ESM4Npc::getEquippedArmor(mPtr);
        const bool wearingHat = std::any_of(equippedArmorForHair.begin(), equippedArmorForHair.end(),
            [](const ESM4::Armor* armor) {
                constexpr std::uint32_t headgearSlots = ESM4::Armor::FO3_Head | ESM4::Armor::FO3_Hair
                    | ESM4::Armor::FO3_Headband | ESM4::Armor::FO3_Hat;
                return armor != nullptr && (armor->mArmorFlags & headgearSlots) != 0;
            });
        bool fallbackHairAttached = false;
        if (!traits.mHair.isZeroOrUnset() && usedHeadPartTypes.count(ESM4::HeadPart::Type_Hair) == 0)
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (const ESM4::Hair* hair = store->get<ESM4::Hair>().search(traits.mHair))
            {
                const osg::Vec4f hairTint = getHairTint(traits);
                Log(Debug::Verbose) << "FNV/ESM4 diag: inserting FONV NPC hair " << hair->mEditorId << " model="
                                 << hair->mModel << " tint=(" << hairTint.x() << ", " << hairTint.y() << ", "
                                 << hairTint.z() << ") for " << traits.mEditorId;
                osg::ref_ptr<osg::Node> attached = insertPart(hair->mModel, &hairTint);
                applyFaceGenHairEgmMorph(
                    mResourceSystem, attached.get(), hair->mModel, traits, wearingHat, race, isFemale);
                applyFalloutProofTriStaticMorph(mResourceSystem, attached.get(), hair->mModel, traits);
                fallbackHairAttached = actorPartHasRenderableGeometry(attached.get());
                if (fallbackHairAttached)
                {
                    usedHeadPartTypes.insert(ESM4::HeadPart::Type_Hair);
                    attachedHeadPartTypes.insert(ESM4::HeadPart::Type_Hair);
                    applyFalloutDialogueMorph(mResourceSystem, mPtr, this, attached.get(), hair->mModel, traits);
                    SelectFalloutHairVariantVisitor visitor(wearingHat);
                    attached->accept(visitor);
                    logFalloutFaceDrawableAudit(
                        attached.get(), hair->mModel, mPtr, "post-fallback-hair-variant");
                    Log(Debug::Info) << "FNV/ESM4 actor completeness: selected "
                                     << (wearingHat ? "Hat" : "NoHat") << " fallback hair variant actor="
                                     << traits.mEditorId << " model=" << hair->mModel
                                     << " selected=" << visitor.mSelected << " hidden=" << visitor.mHidden;
                    ++insertedHeadParts;
                }
                {
                    const bool renderable = actorPartHasRenderableGeometry(attached.get());
                    std::ostringstream details;
                    details << "game=FONV npc=\"" << traits.mEditorId << "\" kind=hair"
                            << " form=" << ESM::RefId(traits.mHair) << " editor=\"" << hair->mEditorId << "\""
                            << " model=\"" << hair->mModel << "\" wearingHat=" << wearingHat
                            << " fallback=1 attached=" << (attached != nullptr) << " renderable=" << renderable
                            << makeActorVisualAuditDetails(attached.get());
                    logWorldViewerActorLedger(mPtr, "actor-part-manifest", details.str());
                }
            }
            else
                Log(Debug::Error) << "Hair not found: " << ESM::RefId(traits.mHair);
        }

        if (insertedHeadParts > 0)
            Log(Debug::Verbose) << "FNV/ESM4 diag: using " << insertedHeadParts
                             << " NPC-specific head mesh part(s) for " << traits.mEditorId;

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
                          << " hairAttached="
                          << (attachedHeadPartTypes.count(ESM4::HeadPart::Type_Hair) != 0 || fallbackHairAttached
                                  ? "OK"
                                  : "MISSING")
                          << " facialHairType="
                          << (attachedHeadPartTypes.count(ESM4::HeadPart::Type_FacialHair) != 0 ? "OK" : "UNKNOWN")
                         << " npcSpecificHeadParts=" << insertedHeadParts
                         << " faceTexture="
                         << (!npcFaceDetailTexture.empty() ? "RACE+DETAIL" : "RACE")
                         << " faceNormal=" << (!npcFaceNormalTexture.empty() ? "OK" : "RACE")
                         << " tintLayers=" << traits.mTintLayers.size();

        const auto [shapeNonZero, shapeTotal] = summarizeCoefficients(traits.mSymShapeModeCoefficients);
        const auto [asymNonZero, asymTotal] = summarizeCoefficients(traits.mAsymShapeModeCoefficients);
        const auto [textureNonZero, textureTotal] = summarizeCoefficients(traits.mSymTextureModeCoefficients);
        Log(Debug::Verbose) << "FNV/ESM4 diag: FaceGen summary for " << traits.mEditorId << " fgRace=" << traits.mFgRace
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
            Log(Debug::Verbose) << "FNV/ESM4 diag: tint layer " << tintLogCount << " for " << traits.mEditorId
                             << " hasIndex=" << tint.hasIndex << " index=" << tint.index
                             << " hasValue=" << tint.hasValue << " value=" << tint.value
                             << " hasColor=" << tint.hasColor << " color=("
                             << static_cast<unsigned int>(tint.color.red) << ", "
                             << static_cast<unsigned int>(tint.color.green) << ", "
                             << static_cast<unsigned int>(tint.color.blue) << ", "
                             << static_cast<unsigned int>(tint.color.custom) << ")";
            ++tintLogCount;
        }

        // ARMO records and their BIPL/ARMA entries can point at the same biped
        // NIF. Attach each model once: duplicate actor-space skins z-fight and
        // make otherwise opaque clothing appear translucent.
        std::map<std::string, osg::ref_ptr<osg::Node>> attachedEquipmentModels;
        const auto attachEquipmentModel = [&](std::string_view model, bool authoredRigidAttachment = false) {
            const VFS::Path::Normalized corrected
                = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(model));
            std::string key = corrected.value();
            Misc::StringUtils::lowerCaseInPlace(key);
            if (const auto found = attachedEquipmentModels.find(key); found != attachedEquipmentModels.end())
                return std::make_pair(found->second, false);

            osg::ref_ptr<osg::Node> attached = authoredRigidAttachment
                ? insertAttachedPart(model, {})
                : insertPart(model);
            forceFalloutActorPartVisible(attached.get(), model, traits);
            overrideFalloutEquipmentSkinTextures(attached.get(), model, traits, mResourceSystem, npcBodyTexture,
                npcFaceTexture, npcFaceDetailTexture, npcGeneratedSkinFaceGen0.get(), npcBodyDetailTexture,
                npcGeneratedFaceGen1.get());
            applyFaceGenEgmMorph(mResourceSystem, attached.get(), model, traits, {}, race, isFemale);
            overrideFalloutEquipmentSkinTextures(attached.get(), model, traits, mResourceSystem, npcBodyTexture,
                npcFaceTexture, npcFaceDetailTexture, npcGeneratedSkinFaceGen0.get(), npcBodyDetailTexture,
                npcGeneratedFaceGen1.get());
            attachedEquipmentModels.emplace(std::move(key), attached);
            return std::make_pair(attached, true);
        };

        for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(mPtr))
        {
            const bool pipBoySlot = (armor->mArmorFlags & ESM4::Armor::FO3_PipBoy) != 0;
            std::string_view model = MWClass::ESM4Npc::chooseEquipmentModel(armor, isFemale);
            if (pipBoySlot)
            {
                // Retail substitutes the separate PipBoyNPC armor model for world/
                // third-person rendering even though Player owns the full PipBoy.
                const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
                if (store != nullptr)
                {
                    for (const ESM4::Armor& candidate : store->get<ESM4::Armor>())
                    {
                        if (!Misc::StringUtils::ciEqual(candidate.mEditorId, "PipBoyNPC"))
                            continue;
                        model = MWClass::ESM4Npc::chooseEquipmentModel(&candidate, isFemale);
                        break;
                    }
                }
                if (model.empty() || Misc::StringUtils::lowerCase(model).find("pipboyarmnpc.nif") == std::string::npos
                    && Misc::StringUtils::lowerCase(model).find("pipboyarmfemalenpc.nif") == std::string::npos)
                {
                    model = isFemale ? "PipBoy3000/PipBoyArmFemaleNPC.NIF"
                                     : "PipBoy3000/PipBoyArmNPC.NIF";
                }
                Log(Debug::Info) << "FNV/ESM4 PipBoy view substitution: actor=" << traits.mEditorId
                                 << " source=" << armor->mEditorId << " selected=" << model
                                 << " attachment=authored-Prn";
            }
            if (proofActor)
                Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: armor " << armor->mEditorId
                                 << " form=" << ESM::RefId(armor->mId) << " model=" << model;
            if (!isAvailableFonvActorModel(mResourceSystem, model))
            {
                std::ostringstream details;
                details << "game=FONV npc=\"" << traits.mEditorId << "\" kind=armor"
                        << " form=" << ESM::RefId(armor->mId) << " editor=\"" << armor->mEditorId << "\""
                        << " model=\"" << model << "\" female=" << isFemale
                        << " slots=0x" << std::hex << armor->mArmorFlags << std::dec
                        << " required=0 reason=\"no available biped model; composed by add-ons\"";
                logWorldViewerActorLedger(mPtr, "equipment-part", details.str());
                continue;
            }
            ++requiredArmorParts;
            auto [attached, firstAttachment] = attachEquipmentModel(model, pipBoySlot);
            const bool renderable = actorPartHasRenderableGeometry(attached.get());
            if (renderable)
                ++attachedArmorParts;
            std::ostringstream details;
            details << "game=FONV npc=\"" << traits.mEditorId << "\" kind=armor"
                    << " form=" << ESM::RefId(armor->mId) << " editor=\"" << armor->mEditorId << "\""
                    << " model=\"" << model << "\" female=" << isFemale
                    << " slots=0x" << std::hex << armor->mArmorFlags << std::dec
                    << " reusedModel=" << !firstAttachment
                    << " required=1 attached=" << (attached != nullptr) << " renderable=" << renderable
                    << makeActorVisualAuditDetails(attached.get());
            logWorldViewerActorLedger(mPtr, "equipment-part", details.str());
        }
        for (const ESM4::ArmorAddon* addon : armorAddons)
        {
            const std::string_view model = chooseFonvArmorAddonModel(*addon, isFemale);
            if (!isAvailableFonvActorModel(mResourceSystem, model))
            {
                std::ostringstream details;
                details << "game=FONV npc=\"" << traits.mEditorId << "\" kind=armor-addon"
                        << " form=" << ESM::RefId(addon->mId) << " editor=\"" << addon->mEditorId << "\""
                        << " model=\"" << model << "\" female=" << isFemale
                        << " slots=0x" << std::hex << addon->mBodyTemplate.bodyPart << std::dec
                        << " required=0 reason=\"no available sex-specific biped model\"";
                logWorldViewerActorLedger(mPtr, "equipment-part", details.str());
                continue;
            }
            ++requiredArmorParts;
            auto [attached, firstAttachment] = attachEquipmentModel(model);
            const bool renderable = actorPartHasRenderableGeometry(attached.get());
            if (renderable)
                ++attachedArmorParts;
            std::ostringstream details;
            details << "game=FONV npc=\"" << traits.mEditorId << "\" kind=armor-addon"
                    << " form=" << ESM::RefId(addon->mId) << " editor=\"" << addon->mEditorId << "\""
                    << " model=\"" << model << "\" female=" << isFemale
                    << " slots=0x" << std::hex << addon->mBodyTemplate.bodyPart << std::dec
                    << " reusedModel=" << !firstAttachment
                    << " required=1 attached=" << (attached != nullptr) << " renderable=" << renderable
                    << makeActorVisualAuditDetails(attached.get());
            logWorldViewerActorLedger(mPtr, "equipment-part", details.str());
        }
        for (const ESM4::Clothing* clothing : MWClass::ESM4Npc::getEquippedClothing(mPtr))
        {
            const std::string_view model = MWClass::ESM4Npc::chooseEquipmentModel(clothing, isFemale);
            if (proofActor)
                Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: clothing " << clothing->mEditorId
                                 << " form=" << ESM::RefId(clothing->mId) << " model=" << model;
            if (!isAvailableFonvActorModel(mResourceSystem, model))
            {
                std::ostringstream details;
                details << "game=FONV npc=\"" << traits.mEditorId << "\" kind=clothing"
                        << " form=" << ESM::RefId(clothing->mId) << " editor=\"" << clothing->mEditorId << "\""
                        << " model=\"" << model << "\" female=" << isFemale
                        << " slots=0x" << std::hex << clothing->mClothingFlags << std::dec
                        << " required=0 reason=\"no available biped model\"";
                logWorldViewerActorLedger(mPtr, "equipment-part", details.str());
                continue;
            }
            ++requiredClothingParts;
            auto [attached, firstAttachment] = attachEquipmentModel(model);
            const bool renderable = actorPartHasRenderableGeometry(attached.get());
            if (renderable)
                ++attachedClothingParts;
            std::ostringstream details;
            details << "game=FONV npc=\"" << traits.mEditorId << "\" kind=clothing"
                    << " form=" << ESM::RefId(clothing->mId) << " editor=\"" << clothing->mEditorId << "\""
                    << " model=\"" << model << "\" female=" << isFemale
                    << " slots=0x" << std::hex << clothing->mClothingFlags << std::dec
                    << " reusedModel=" << !firstAttachment
                    << " required=1 attached=" << (attached != nullptr) << " renderable=" << renderable
                    << makeActorVisualAuditDetails(attached.get());
            logWorldViewerActorLedger(mPtr, "equipment-part", details.str());
        }

        if (const ESM4::Weapon* weapon = MWClass::ESM4Npc::getEquippedWeapon(mPtr))
        {
            mFalloutActionWeapon = weapon;
            ++requiredWeaponParts;
            const MWMechanics::DrawState drawState = mPtr.getClass().getCreatureStats(mPtr).getDrawState();
            const bool weaponDrawn = drawState == MWMechanics::DrawState::Weapon;
            mFalloutWeaponsShown = weaponDrawn;
            mFalloutWeaponHolsterBone.clear();
            const std::string_view preferredBone = "Weapon";
            if (proofTargetActor && std::getenv("OPENMW_FNV_PROOF_HIDE_EQUIPPED_WEAPON") != nullptr)
            {
                weaponIntentionallyHidden = true;
                Log(Debug::Info) << "FNV/ESM4 proof: skipped equipped weapon for clean dialogue proof on "
                                 << traits.mEditorId;
            }
            osg::ref_ptr<osg::Node> attached;
            const std::string weaponModel = resolveFalloutWeaponViewModel(*weapon);
            mFalloutWeaponUsesWorldModelFallback = mFirstPersonView && weaponModel == weapon->mModel;
            if (!weaponIntentionallyHidden)
            {
                std::string authoredParent;
                // Resolve the model's Prn when present, otherwise the canonical
                // authored skeleton Weapon target. Never invent a hand, hip,
                // back, or actor-root fallback.
                attached = insertAttachedPart(weaponModel, {}, &authoredParent);
                if (!authoredParent.empty())
                    mFalloutWeaponDrawBone = authoredParent;
                mFalloutWeaponPart = attached;
                if (attached != nullptr)
                {
                    if (weaponDrawn)
                        showWeapons(true);
                    else if (mFirstPersonView)
                        attached->setNodeMask(0);
                    else if (!applyRetailWeaponHolsterContract(*weapon))
                        attached->setNodeMask(0);
                }
            }
            const bool renderable = actorPartHasRenderableGeometry(attached.get());
            if (renderable)
                ++attachedWeaponParts;
            {
                std::ostringstream details;
                details << "game=FONV npc=\"" << traits.mEditorId << "\" kind=weapon"
                        << " form=" << ESM::RefId(weapon->mId) << " editor=\"" << weapon->mEditorId << "\""
                        << " model=\"" << weaponModel << "\" worldModel=\"" << weapon->mModel
                        << "\" firstPersonModel=\"" << weapon->mFirstPersonModel
                        << "\" firstPerson=" << mFirstPersonView << " preferredBone=\"" << preferredBone << "\""
                        << " animationType=" << static_cast<unsigned int>(weapon->mData.animationType)
                        << " handGrip=" << static_cast<unsigned int>(weapon->mData.handGrip)
                        << " reloadAnim=" << static_cast<unsigned int>(weapon->mData.reloadAnim)
                        << " drawState=" << static_cast<unsigned int>(drawState)
                        << " weaponDrawn=" << weaponDrawn
                        << " holsterBone=\"" << mFalloutWeaponHolsterBone << "\""
                        << " hiddenByProof=" << weaponIntentionallyHidden
                        << " required=1 attached=" << (attached != nullptr) << " renderable=" << renderable
                        << makeActorVisualAuditDetails(attached.get());
                logWorldViewerActorLedger(mPtr, "equipment-part", details.str());
            }
            if (!weaponIntentionallyHidden)
            {
                const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
                Log(Debug::Verbose) << "FNV/ESM4 diag: equipped NPC weapon " << weapon->mEditorId << " model="
                                 << weapon->mModel << " damage=" << weapon->mData.damage << " for "
                                 << traits.mEditorId << " attached=" << (attached != nullptr);
                Log(Debug::Verbose) << "FNV/ESM4 diag: weapon metadata " << weapon->mEditorId
                                 << " ammo=" << ESM::RefId(weapon->mAmmo)
                                 << " repairList=" << ESM::RefId(weapon->mRepairList)
                                 << " equipType=" << ESM::RefId(weapon->mEquipType)
                                 << " impactDataSet=" << ESM::RefId(weapon->mImpactDataSet)
                                 << " worldModel=" << ESM::RefId(weapon->mWorldModel)
                                 << " clipSize=" << static_cast<unsigned int>(weapon->mData.clipSize)
                                 << " modItems=[" << ESM::RefId(weapon->mModItem[0]) << ","
                                 << ESM::RefId(weapon->mModItem[1]) << "," << ESM::RefId(weapon->mModItem[2])
                                 << "] sounds=[" << formatFalloutWeaponSoundRefs(*weapon) << "]";
                Log(Debug::Verbose) << "FNV/ESM4 diag: weapon sound files " << weapon->mEditorId << " ["
                                 << formatFalloutWeaponSoundFiles(*weapon, *store) << "]";
            }
        }

        ActorVisualAuditVisitor finalActorAudit;
        if (mObjectRoot != nullptr)
            mObjectRoot->accept(finalActorAudit);
        const bool bodyComplete = attachedRaceBodyParts + coveredRaceBodyParts == requiredRaceBodyParts;
        const bool raceFaceComplete = attachedRaceFaceParts == requiredRaceFaceParts;
        const bool npcHeadComplete = attachedRequestedHeadParts == requiredNpcHeadPartRecords;
        const bool hairComplete = traits.mHair.isZeroOrUnset()
            || attachedHeadPartTypes.count(ESM4::HeadPart::Type_Hair) != 0 || fallbackHairAttached;
        const bool armorComplete = attachedArmorParts == requiredArmorParts;
        const bool clothingComplete = attachedClothingParts == requiredClothingParts;
        const bool weaponComplete = attachedWeaponParts == requiredWeaponParts && !weaponIntentionallyHidden;
        const bool goreComplete = finalActorAudit.mRenderableGoreGeometry == 0;
        const bool assemblyComplete = bodyComplete && raceFaceComplete && npcHeadComplete && hairComplete
            && armorComplete && clothingComplete && weaponComplete && goreComplete;
        {
            std::ostringstream details;
            details << "game=FONV npc=\"" << traits.mEditorId << "\""
                    << " bodyRequired=" << requiredRaceBodyParts << " bodyAttached=" << attachedRaceBodyParts
                    << " bodyCovered=" << coveredRaceBodyParts << " bodyComplete=" << bodyComplete
                    << " raceFaceRequired=" << requiredRaceFaceParts
                    << " raceFaceAttached=" << attachedRaceFaceParts << " raceFaceComplete=" << raceFaceComplete
                    << " npcHeadRequired=" << requiredNpcHeadPartRecords
                    << " npcHeadAttached=" << attachedRequestedHeadParts
                    << " npcHeadTotalWithExtras=" << insertedHeadParts << " npcHeadComplete=" << npcHeadComplete
                    << " hairRequired=" << !traits.mHair.isZeroOrUnset() << " hairComplete=" << hairComplete
                    << " armorRequired=" << requiredArmorParts << " armorAttached=" << attachedArmorParts
                    << " clothingRequired=" << requiredClothingParts
                    << " clothingAttached=" << attachedClothingParts
                    << " weaponRequired=" << requiredWeaponParts << " weaponAttached=" << attachedWeaponParts
                    << " weaponHiddenByProof=" << weaponIntentionallyHidden
                    << " activeGoreGeometry=" << finalActorAudit.mRenderableGoreGeometry
                    << " status=" << (assemblyComplete ? "passed" : "failing");
            logWorldViewerActorLedger(mPtr, "actor-assembly-gate", details.str());
        }

        if (proofActor)
            Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: END parts assembled";
    }

    unsigned int ESM4NpcAnimation::insertHeadParts(
        const ESM4::Npc& traits, const std::vector<ESM::FormId>& partIds, std::set<uint32_t>& usedHeadPartTypes,
        std::set<uint32_t>* attachedHeadPartTypes, unsigned int* attachedRequestedPartCount,
        const ESM4::Race* faceGenRace, bool faceGenFemale)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        const auto equippedArmor = MWClass::ESM4Npc::getEquippedArmor(mPtr);
        const bool wearingHat = std::any_of(equippedArmor.begin(), equippedArmor.end(), [](const ESM4::Armor* armor) {
                constexpr std::uint32_t headgearSlots = ESM4::Armor::FO3_Head | ESM4::Armor::FO3_Hair
                    | ESM4::Armor::FO3_Headband | ESM4::Armor::FO3_Hat;
                return armor != nullptr && (armor->mArmorFlags & headgearSlots) != 0;
            });
        const auto selectHairVariant = [&](osg::Node* attached, const ESM4::HeadPart& headPart) {
            if (attached == nullptr || (headPart.mType != ESM4::HeadPart::Type_Hair
                    && !isFalloutScalpHairModel(headPart.mModel)))
                return;
            SelectFalloutHairVariantVisitor visitor(wearingHat);
            attached->accept(visitor);
            logFalloutFaceDrawableAudit(
                attached, headPart.mModel, mPtr, "post-headpart-hair-variant");
            Log(Debug::Info) << "FNV/ESM4 actor completeness: selected "
                             << (wearingHat ? "Hat" : "NoHat") << " hair variant actor=" << traits.mEditorId
                             << " model=" << headPart.mModel << " selected=" << visitor.mSelected
                             << " hidden=" << visitor.mHidden;
        };
        unsigned int inserted = 0;
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
            const bool miscPart = isFallout3OrNewVegas(traits) && isFonvMiscHeadPart(*part);
            // Do not reserve a type until it produced renderable geometry. A bad or
            // absent first hair entry must not suppress the NPC's HAIR fallback (or
            // a later valid head-part entry of the same type).
            if (miscPart || usedHeadPartTypes.count(part->mType) == 0)
            {
                const osg::Vec4f hairTint = getHairTint(traits);
                const osg::Vec4f* tint = miscPart || part->mType == ESM4::HeadPart::Type_Hair
                    || part->mType == ESM4::HeadPart::Type_FacialHair
                    || part->mType == ESM4::HeadPart::Type_Eyebrows
                    ? &hairTint
                    : nullptr;
                Log(Debug::Verbose) << "FNV/ESM4 diag: inserting NPC head part " << part->mEditorId << " type="
                                 << part->mType << " model=" << part->mModel << " for "
                                 << mPtr.getCellRef().getRefId();
                osg::ref_ptr<osg::Node> attached = insertPart(part->mModel, tint);
                if (part->mType == ESM4::HeadPart::Type_Hair || isFalloutScalpHairModel(part->mModel))
                    applyFaceGenHairEgmMorph(
                        mResourceSystem, attached.get(), part->mModel, traits, wearingHat, faceGenRace, faceGenFemale);
                else
                    applyFaceGenEgmMorph(
                        mResourceSystem, attached.get(), part->mModel, traits, {}, faceGenRace, faceGenFemale);
                applyFalloutProofTriStaticMorph(mResourceSystem, attached.get(), part->mModel, traits);
                const bool renderable = actorPartHasRenderableGeometry(attached.get());
                if (renderable)
                {
                    applyFalloutDialogueMorph(mResourceSystem, mPtr, this, attached.get(), part->mModel, traits);
                    selectHairVariant(attached.get(), *part);
                    if (!miscPart)
                        usedHeadPartTypes.insert(part->mType);
                    if (attachedHeadPartTypes != nullptr)
                        attachedHeadPartTypes->insert(part->mType);
                    if (attachedRequestedPartCount != nullptr)
                        ++*attachedRequestedPartCount;
                    ++inserted;
                }
                {
                    std::ostringstream details;
                    details << "game=" << worldViewerNpcGameTag(traits) << " npc=\"" << traits.mEditorId << "\""
                            << " kind=npc-head-part form=" << ESM::RefId(partId)
                            << " editor=\"" << part->mEditorId << "\" type=" << part->mType
                            << " model=\"" << part->mModel << "\" extra=0 selected=1"
                            << " attached=" << (attached != nullptr) << " renderable=" << renderable
                            << makeActorVisualAuditDetails(attached.get());
                    logWorldViewerActorLedger(mPtr, "actor-part-manifest", details.str());
                }
                if (renderable && isFonvFacialHairHeadPart(*part))
                {
                    usedHeadPartTypes.insert(ESM4::HeadPart::Type_FacialHair);
                    if (attached != nullptr && attachedHeadPartTypes != nullptr)
                        attachedHeadPartTypes->insert(ESM4::HeadPart::Type_FacialHair);
                }
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
                    const osg::Vec4f* extraTint = isFonvMiscHeadPart(*extraPart)
                            || extraPart->mType == ESM4::HeadPart::Type_Hair
                            || extraPart->mType == ESM4::HeadPart::Type_FacialHair
                            || extraPart->mType == ESM4::HeadPart::Type_Eyebrows
                        ? &hairTint
                        : nullptr;
                    osg::ref_ptr<osg::Node> extraAttached = insertPart(extraPart->mModel, extraTint);
                    if (extraPart->mType == ESM4::HeadPart::Type_Hair
                        || isFalloutScalpHairModel(extraPart->mModel))
                        applyFaceGenHairEgmMorph(
                            mResourceSystem, extraAttached.get(), extraPart->mModel, traits, wearingHat, faceGenRace,
                            faceGenFemale);
                    else
                        applyFaceGenEgmMorph(mResourceSystem, extraAttached.get(), extraPart->mModel, traits, {},
                            faceGenRace, faceGenFemale);
                    applyFalloutProofTriStaticMorph(mResourceSystem, extraAttached.get(), extraPart->mModel, traits);
                    const bool extraRenderable = actorPartHasRenderableGeometry(extraAttached.get());
                    if (extraRenderable)
                    {
                        applyFalloutDialogueMorph(
                            mResourceSystem, mPtr, this, extraAttached.get(), extraPart->mModel, traits);
                        selectHairVariant(extraAttached.get(), *extraPart);
                        if (attachedHeadPartTypes != nullptr)
                            attachedHeadPartTypes->insert(extraPart->mType);
                        ++inserted;
                    }
                    {
                        std::ostringstream details;
                        details << "game=" << worldViewerNpcGameTag(traits) << " npc=\"" << traits.mEditorId
                                << "\" kind=npc-head-part form=" << ESM::RefId(extraPartId)
                                << " editor=\"" << extraPart->mEditorId << "\" type=" << extraPart->mType
                                << " model=\"" << extraPart->mModel << "\" extra=1 selected=1"
                                << " attached=" << (extraAttached != nullptr) << " renderable=" << extraRenderable
                                << makeActorVisualAuditDetails(extraAttached.get());
                        logWorldViewerActorLedger(mPtr, "actor-part-manifest", details.str());
                    }
                    if (extraAttached != nullptr && std::getenv("OPENMW_FNV_PROOF_MOUTH_DRIVER") != nullptr
                        && isFalloutMouthDriverPart(extraPart->mModel))
                        applyFalloutProofTriOpenMorph(
                            mResourceSystem, mPtr, extraAttached.get(), extraPart->mModel, traits);
                    if (extraRenderable && isFonvFacialHairHeadPart(*extraPart))
                    {
                        usedHeadPartTypes.insert(ESM4::HeadPart::Type_FacialHair);
                        if (extraAttached != nullptr && attachedHeadPartTypes != nullptr)
                            attachedHeadPartTypes->insert(ESM4::HeadPart::Type_FacialHair);
                    }
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
        const bool insertAllDistinctAddonModels
            = worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_INSERT_ALL_ESM4_ARMOR_ADDONS");
        const bool skipSkinWhenClothed = insertAllDistinctAddonModels
            && worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_SKIP_ESM4_SKIN_WHEN_CLOTHED");
        auto isSkinNakedArmor = [](const ESM4::Armor* armor) {
            return armor != nullptr && Misc::StringUtils::ciEqual(armor->mEditorId, "Skin_Naked");
        };
        bool hasNonSkinArmor = false;
        for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(mPtr))
        {
            if (!isSkinNakedArmor(armor))
            {
                hasNonSkinArmor = true;
                break;
            }
        }
        if (!hasNonSkinArmor && !traits.mWornArmor.isZeroOrUnset())
        {
            if (const ESM4::Armor* wornArmor
                = searchEsm4ViewerRecordWithLocalFallback<ESM4::Armor>(*store, traits.mWornArmor))
                hasNonSkinArmor = !isSkinNakedArmor(wornArmor);
        }

        auto findArmorAddons = [&](const ESM4::Armor* armor) {
            {
                std::ostringstream details;
                details << "game=" << worldViewerNpcGameTag(traits)
                        << " armor=\"" << armor->mEditorId << "\""
                        << " armorName=\"" << armor->mFullName << "\""
                        << " addonCount=" << armor->mAddOns.size()
                        << " race=" << ESM::RefId(traits.mRace);
                logWorldViewerActorLedger(mPtr, "tes5-armor-candidate", details.str());
            }
            for (ESM::FormId armaId : armor->mAddOns)
            {
                if (armaId.isZeroOrUnset())
                    continue;
                ESM::FormId resolvedArmaId = armaId;
                const ESM4::ArmorAddon* arma
                    = searchEsm4ViewerRecordWithLocalFallback<ESM4::ArmorAddon>(*store, armaId, &resolvedArmaId);
                if (!arma)
                {
                    Log(Debug::Error) << "ArmorAddon not found: " << ESM::RefId(armaId);
                    std::ostringstream details;
                    details << "armor=\"" << armor->mEditorId << "\""
                            << " addon=" << ESM::RefId(armaId)
                            << " result=\"missing\"";
                    logWorldViewerActorLedger(mPtr, "tes5-armor-addon", details.str());
                    continue;
                }
                bool compatibleRace = arma->mRacePrimary == traits.mRace;
                for (auto r : arma->mRaces)
                    if (r == traits.mRace)
                        compatibleRace = true;

                const std::string_view addonModel = isFemale ? arma->mModelFemale : arma->mModelMale;
                std::string loweredAddonModel(addonModel);
                Misc::StringUtils::lowerCaseInPlace(loweredAddonModel);
                const bool isStandaloneStarfieldHandModel
                    = loweredAddonModel.find("nakedhands") != std::string::npos
                    || loweredAddonModel.find("hands_3rd") != std::string::npos;
                if (skipSkinWhenClothed && hasNonSkinArmor && isSkinNakedArmor(armor)
                    && !isStandaloneStarfieldHandModel)
                {
                    std::ostringstream details;
                    details << "armor=\"" << armor->mEditorId << "\""
                            << " armorName=\"" << armor->mFullName << "\""
                            << " addon=\"" << arma->mEditorId << "\""
                            << " model=\"" << addonModel << "\""
                            << " result=\"skip-clothed-skin-body-preserve-hands\""
                            << " nonSkinArmor=1";
                    logWorldViewerActorLedger(mPtr, "tes5-armor-skip", details.str());
                    continue;
                }

                {
                    std::ostringstream details;
                    details << "armor=\"" << armor->mEditorId << "\""
                            << " addon=" << ESM::RefId(armaId)
                            << " resolvedAddon=" << ESM::RefId(resolvedArmaId)
                            << " addonEditor=\"" << arma->mEditorId << "\""
                            << " compatibleRace=" << compatibleRace
                            << " primaryRace=" << ESM::RefId(arma->mRacePrimary)
                            << " model=\"" << addonModel << "\""
                            << " covers=0x" << std::hex << arma->mBodyTemplate.bodyPart << std::dec
                            << " malePriority=" << arma->mMalePriority
                            << " femalePriority=" << arma->mFemalePriority;
                    logWorldViewerActorLedger(mPtr, "tes5-armor-addon", details.str());
                }

                if (compatibleRace)
                    armorAddons.push_back(arma);
            }
        };

        {
            std::ostringstream details;
            details << "game=" << worldViewerNpcGameTag(traits)
                    << " race=" << ESM::RefId(traits.mRace)
                    << " raceEditor=\"" << (race ? race->mEditorId : std::string()) << "\""
                    << " female=" << isFemale
                    << " equippedArmor=" << MWClass::ESM4Npc::getEquippedArmor(mPtr).size()
                    << " equippedClothing=" << MWClass::ESM4Npc::getEquippedClothing(mPtr).size()
                    << " wornArmor=" << ESM::RefId(traits.mWornArmor)
                    << " raceSkin=" << (race ? ESM::RefId(race->mSkin).toDebugString() : std::string());
            logWorldViewerActorLedger(mPtr, "tes5-armor-begin", details.str());
        }

        for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(mPtr))
            findArmorAddons(armor);
        if (!traits.mWornArmor.isZeroOrUnset())
        {
            if (const ESM4::Armor* armor
                = searchEsm4ViewerRecordWithLocalFallback<ESM4::Armor>(*store, traits.mWornArmor))
                findArmorAddons(armor);
            else
                Log(Debug::Error) << "Worn armor not found: " << ESM::RefId(traits.mWornArmor);
        }
        if (!race->mSkin.isZeroOrUnset())
        {
            if (const ESM4::Armor* armor
                = searchEsm4ViewerRecordWithLocalFallback<ESM4::Armor>(*store, race->mSkin))
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
        std::set<std::string> insertedProofAddonModels;
        for (const ESM4::ArmorAddon* arma : armorAddons)
        {
            const uint32_t covers = arma->mBodyTemplate.bodyPart;
            const std::string_view addonModel = isFemale ? arma->mModelFemale : arma->mModelMale;
            if (insertAllDistinctAddonModels)
            {
                std::string loweredModel(addonModel);
                Misc::StringUtils::lowerCaseInPlace(loweredModel);
                if (loweredModel.empty())
                {
                    std::ostringstream details;
                    details << "addonEditor=\"" << arma->mEditorId << "\""
                            << " model=\"" << addonModel << "\""
                            << " covers=0x" << std::hex << covers
                            << " usedParts=0x" << usedParts << std::dec
                            << " result=\"empty-model-proof-all\"";
                    logWorldViewerActorLedger(mPtr, "tes5-armor-insert", details.str());
                    continue;
                }

                if (!insertedProofAddonModels.insert(loweredModel).second)
                {
                    std::ostringstream details;
                    details << "addonEditor=\"" << arma->mEditorId << "\""
                            << " model=\"" << addonModel << "\""
                            << " covers=0x" << std::hex << covers
                            << " usedParts=0x" << usedParts << std::dec
                            << " result=\"duplicate-model-proof-all\"";
                    logWorldViewerActorLedger(mPtr, "tes5-armor-insert", details.str());
                    continue;
                }

                usedParts |= covers;
                std::ostringstream details;
                details << "addonEditor=\"" << arma->mEditorId << "\""
                        << " model=\"" << addonModel << "\""
                        << " covers=0x" << std::hex << covers
                        << " usedParts=0x" << usedParts << std::dec
                        << " result=\"insert-proof-all\"";
                logWorldViewerActorLedger(mPtr, "tes5-armor-insert", details.str());
                insertPart(addonModel, nullptr, {},
                    (covers & ESM4::Armor::TES5_Shield) != 0 ? std::string_view("SHIELD") : std::string_view());
                for (const std::string& siblingModel : getStarfieldProofClothingSiblingModels(addonModel, isFemale))
                {
                    std::string loweredSiblingModel(siblingModel);
                    Misc::StringUtils::lowerCaseInPlace(loweredSiblingModel);
                    if (!insertedProofAddonModels.insert(loweredSiblingModel).second)
                        continue;

                    std::ostringstream siblingDetails;
                    siblingDetails << "addonEditor=\"" << arma->mEditorId << "\""
                                   << " sourceModel=\"" << addonModel << "\""
                                   << " model=\"" << siblingModel << "\""
                                   << " covers=0x" << std::hex << covers
                                   << " usedParts=0x" << usedParts << std::dec
                                   << " result=\"insert-proof-sibling\"";
                    logWorldViewerActorLedger(mPtr, "tes5-armor-insert", siblingDetails.str());
                    insertPart(siblingModel);
                }
                continue;
            }

            // if body is already covered, skip to avoid clipping
            if (covers & usedParts & ESM4::Armor::TES5_Body)
            {
                std::ostringstream details;
                details << "addonEditor=\"" << arma->mEditorId << "\""
                        << " model=\"" << addonModel << "\""
                        << " covers=0x" << std::hex << covers
                        << " usedParts=0x" << usedParts << std::dec
                        << " result=\"covered\"";
                logWorldViewerActorLedger(mPtr, "tes5-armor-insert", details.str());
                continue;
            }
            // if covers at least something that wasn't covered before - add model
            if (covers & ~usedParts)
            {
                usedParts |= covers;
                std::ostringstream details;
                details << "addonEditor=\"" << arma->mEditorId << "\""
                        << " model=\"" << addonModel << "\""
                        << " covers=0x" << std::hex << covers
                        << " usedParts=0x" << usedParts << std::dec
                        << " result=\"insert\"";
                logWorldViewerActorLedger(mPtr, "tes5-armor-insert", details.str());
                insertPart(addonModel, nullptr, {},
                    (covers & ESM4::Armor::TES5_Shield) != 0 ? std::string_view("SHIELD") : std::string_view());
            }
            else
            {
                std::ostringstream details;
                details << "addonEditor=\"" << arma->mEditorId << "\""
                        << " model=\"" << addonModel << "\""
                        << " covers=0x" << std::hex << covers
                        << " usedParts=0x" << usedParts << std::dec
                        << " result=\"empty-cover\"";
                logWorldViewerActorLedger(mPtr, "tes5-armor-insert", details.str());
            }
        }

        {
            std::ostringstream details;
            details << "addonCount=" << armorAddons.size()
                    << " usedParts=0x" << std::hex << usedParts << std::dec;
            logWorldViewerActorLedger(mPtr, "tes5-armor-end", details.str());
        }

        std::set<uint32_t> usedHeadPartTypes;
        std::set<uint32_t> attachedHeadPartTypes;
        if (usedParts & ESM4::Armor::TES5_Hair)
            usedHeadPartTypes.insert(ESM4::HeadPart::Type_Hair);
        bool starfieldGeneratedFaceAttached = false;
        if (traits.mIsStarfield)
        {
            const std::string faceModel = getStarfieldGeneratedFaceModel(traits);
            const VFS::Manager* vfs = mResourceSystem != nullptr ? mResourceSystem->getVFS() : nullptr;
            const VFS::Path::Normalized correctedFaceModel
                = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(faceModel));
            const bool faceModelExists = vfs != nullptr && vfs->exists(correctedFaceModel);
            osg::ref_ptr<osg::Node> generatedFace;
            if (faceModelExists)
                generatedFace = insertPart(faceModel);
            starfieldGeneratedFaceAttached = generatedFace != nullptr;
            if (generatedFace != nullptr)
                generatedFace->setName("Starfield Generated Face " + correctedFaceModel.value());
            Log(starfieldGeneratedFaceAttached ? Debug::Info : Debug::Verbose)
                << "Starfield generated face composition: actor=" << mPtr.getCellRef().getRefId()
                << " npc=\"" << traits.mEditorId << "\""
                << " model=\"" << correctedFaceModel.value() << "\""
                << " vfsExists=" << faceModelExists
                << " attached=" << starfieldGeneratedFaceAttached
                << " source="
                << (Misc::StringUtils::ciEqual(traits.mEditorId, "Player")
                        ? "player-face-form-override"
                        : "npc-base-form");
        }
        if (!starfieldGeneratedFaceAttached)
        {
            insertHeadParts(traits, traits.mHeadParts, usedHeadPartTypes, &attachedHeadPartTypes);
            insertHeadParts(traits, isFemale ? race->mHeadPartIdsFemale : race->mHeadPartIdsMale,
                usedHeadPartTypes, &attachedHeadPartTypes);
        }

        if (traits.mIsFO4)
        {
            const bool faceAttached = attachedHeadPartTypes.count(ESM4::HeadPart::Type_Face) != 0;
            const bool eyesAttached = attachedHeadPartTypes.count(ESM4::HeadPart::Type_Eyes) != 0;
            const bool mouthTeethAttached = attachedHeadPartTypes.count(ESM4::HeadPart::Type_Teeth) != 0;
            const bool pass = faceAttached && eyesAttached && mouthTeethAttached;
            Log(pass ? Debug::Info : Debug::Warning)
                << "FO4 face composition telemetry: actor=" << mPtr.getCellRef().getRefId()
                << " npc=\"" << traits.mEditorId << "\""
                << " face=" << faceAttached
                << " eyes=" << eyesAttached
                << " mouthTeeth=" << mouthTeethAttached
                << " attachedTypes=" << attachedHeadPartTypes.size()
                 << " result=" << (pass ? "pass" : "fail");
        }
        else if (race != nullptr && Misc::StringUtils::ciEqual(race->mEditorId, "NordRace"))
        {
            const bool faceAttached = attachedHeadPartTypes.count(ESM4::HeadPart::Type_Face) != 0;
            const bool eyesAttached = attachedHeadPartTypes.count(ESM4::HeadPart::Type_Eyes) != 0;
            const bool mouthAttached = attachedHeadPartTypes.count(ESM4::HeadPart::Type_Misc) != 0;
            const bool hairAttached = attachedHeadPartTypes.count(ESM4::HeadPart::Type_Hair) != 0;
            const bool pass = faceAttached && eyesAttached && mouthAttached && hairAttached;
            Log(pass ? Debug::Info : Debug::Warning)
                << "Skyrim face composition telemetry: actor=" << mPtr.getCellRef().getRefId()
                << " npc=\"" << traits.mEditorId << "\""
                << " face=" << faceAttached
                << " eyes=" << eyesAttached
                << " mouth=" << mouthAttached
                << " hair=" << hairAttached
                << " attachedTypes=" << attachedHeadPartTypes.size()
                << " result=" << (pass ? "pass" : "fail");
        }
    }
}
