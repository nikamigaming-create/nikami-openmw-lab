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

#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/resource/imagemanager.hpp>
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
#include <osg/TexEnv>
#include <osg/Texture2D>
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
#include "../mwworld/cell.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/timestamp.hpp"
#include "util.hpp"
#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
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
            TintMaterialVisitor(
                const osg::Vec4f& tint, float emissionStrength = 0.f, bool replaceVertexRgb = false)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mTint(tint)
                , mEmissionStrength(emissionStrength)
                , mReplaceVertexRgb(replaceVertexRgb)
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
                        if (mReplaceVertexRgb)
                            color.set(1.f, 1.f, 1.f, color.w());
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
                        if (mReplaceVertexRgb)
                            color.set(255, 255, 255, color.a());
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
            bool mReplaceVertexRgb = false;

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
                traverse(geode);
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

        private:
            void auditDrawable(osg::Drawable& drawable)
            {
                ++mDrawables;
                if (drawable.asGeometry() != nullptr)
                    ++mGeometry;
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    ++mRigGeometry;
                    if (rig->getSourceGeometry() != nullptr)
                        ++mGeometry;
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
                    = Misc::ResourceHelpers::correctTexturePath(texture, vfs);
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
                = Misc::ResourceHelpers::correctTexturePath(texture, resourceSystem->getVFS());
            if (!resourceSystem->getVFS()->exists(correctedTexture))
                return nullptr;

            return resourceSystem->getImageManager()->getImage(correctedTexture);
        }

        bool isFullSizeSkinDiffuse(Resource::ResourceSystem* resourceSystem, std::string_view texture)
        {
            osg::ref_ptr<osg::Image> image = getExistingTextureImage(resourceSystem, texture);
            return image != nullptr && image->s() >= 64 && image->t() >= 64;
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
            tint = osg::Vec4f(std::clamp(average.x() * 2.f, 0.f, 1.f),
                std::clamp(average.y() * 2.f, 0.f, 1.f), std::clamp(average.z() * 2.f, 0.f, 1.f), 1.f);
            return true;
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
                = Misc::ResourceHelpers::correctTexturePath(texture, resourceSystem->getVFS());
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
        }

        void overrideFalloutPartDetailTexture(
            std::string_view texture, Resource::ResourceSystem* resourceSystem, osg::Node& node)
        {
            overrideFalloutPartTexture(texture, "detailMap", 4, resourceSystem, node);
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
                    const float loudness = MWBase::Environment::get().getSoundManager()->getSaySoundLoudness(mActor);
                    open = std::clamp(loudness * 5.0f, 0.f, 0.65f);
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
                        mFirstRigBoneNames.reserve(mFirstRigBoneCount);
                        const std::size_t sampleCount = std::min<std::size_t>(mFirstRigBoneCount, 4);
                        for (std::size_t i = 0; i < mFirstRigBoneCount; ++i)
                        {
                            const std::string boneName(rig->getBoneName(i));
                            mFirstRigBoneNames.push_back(boneName);
                            if (i < sampleCount)
                            {
                                if (i != 0)
                                    mFirstRigBoneSample << ",";
                                mFirstRigBoneSample << boneName;
                            }
                        }
                    }
                }
                else if (SceneUtil::RigGeometryHolder* holder = dynamic_cast<SceneUtil::RigGeometryHolder*>(&drawable))
                {
                    ++mRigGeometryCount;
                    if (holder->getSourceRigGeometry() != nullptr)
                        ++mStaticGeometryCount;
                    drawable.setCullingActive(false);
                }
                else if (SceneUtil::MorphGeometry* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
                {
                    ++mMorphGeometryCount;
                    if (morph->getSourceGeometry() != nullptr)
                        ++mStaticGeometryCount;
                    drawable.setCullingActive(false);
                }
                else if (dynamic_cast<osg::Geometry*>(&drawable) != nullptr)
                    ++mStaticGeometryCount;
            }

            unsigned int mRigGeometryCount = 0;
            unsigned int mStaticGeometryCount = 0;
            unsigned int mMorphGeometryCount = 0;
            std::string mFirstRigRootBone;
            std::size_t mFirstRigBoneCount = 0;
            std::vector<std::string> mFirstRigBoneNames;
            std::ostringstream mFirstRigBoneSample;
        };

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
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        mark(*drawable);

                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { mark(drawable); }

            unsigned int mMarked = 0;

        private:
            void mark(osg::Drawable& drawable)
            {
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    rig->setFalloutCharacterSkinning(true);
                    ++mMarked;
                }
            }
        };

        class ForceFalloutRigGeometryUpdateVisitor : public osg::NodeVisitor
        {
        public:
            ForceFalloutRigGeometryUpdateVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                {
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        applyDrawable(*drawable);
                }
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override { applyDrawable(drawable); }

            unsigned int mForcedRigGeometry = 0;
            unsigned int mForcedRigGeometryHolder = 0;
            unsigned int mRefreshedRigGeometry = 0;

        private:
            void applyDrawable(osg::Drawable& drawable)
            {
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (rig->isFalloutCharacterRig())
                    {
                        rig->forceNextUpdate();
                        if (rig->refreshFalloutSkinningForCurrentPose())
                            ++mRefreshedRigGeometry;
                        ++mForcedRigGeometry;
                    }
                    return;
                }

                if (SceneUtil::RigGeometryHolder* holder = dynamic_cast<SceneUtil::RigGeometryHolder*>(&drawable))
                {
                    holder->forceNextUpdate();
                    ++mForcedRigGeometryHolder;
                }
            }
        };

        unsigned int forceFalloutRigGeometryUpdate(osg::Node* root, unsigned int& holderCount, unsigned int& refreshCount)
        {
            holderCount = 0;
            refreshCount = 0;
            if (root == nullptr)
                return 0;

            ForceFalloutRigGeometryUpdateVisitor visitor;
            root->accept(visitor);
            holderCount = visitor.mForcedRigGeometryHolder;
            refreshCount = visitor.mRefreshedRigGeometry;
            return visitor.mForcedRigGeometry;
        }

        bool isFallout3OrNewVegas(const ESM4::Npc& npc)
        {
            return npc.mIsFO3 || npc.mIsFONV;
        }

        class StaticizeFalloutRiggedGeometryVisitor : public osg::NodeVisitor
        {
        public:
            StaticizeFalloutRiggedGeometryVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
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
                auto vertexCount = [](const osg::Geometry* geometry) -> unsigned int {
                    if (geometry == nullptr || geometry->getVertexArray() == nullptr)
                        return 0;
                    return geometry->getVertexArray()->getNumElements();
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

        bool applyFaceGenEgmMorph(Resource::ResourceSystem* resourceSystem, osg::Node* attached, std::string_view model,
            const ESM4::Npc& traits)
        {
            if (attached == nullptr)
                return false;

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
            FalloutDialogueMorphDriver(const MWWorld::ConstPtr& actor, Animation* animation, const FaceGenTri& tri,
                const std::string& model)
                : mActor(actor)
                , mAnimation(animation)
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
                float dominantLipValue = 0.f;
                std::string_view dominantLipTarget;

                for (const Morph& morph : mMorphs)
                {
                    float val = mAnimation->getFalloutHeadAnimTrackValue(morph.name);
                    const float lipValue
                        = MWBase::Environment::get().getSoundManager()->getSaySoundFacialTrackValue(mActor, morph.name);
                    val += lipValue;
                    if (std::abs(lipValue) > std::abs(dominantLipValue))
                    {
                        dominantLipValue = lipValue;
                        dominantLipTarget = morph.name;
                    }
                    values.push_back(val);
                }

                if (std::getenv("OPENMW_FNV_PROOF_LIP_TELEMETRY") != nullptr
                    && nv != nullptr && nv->getFrameStamp() != nullptr)
                {
                    const std::uint64_t frame = nv->getFrameStamp()->getFrameNumber();
                    if (frame != mLastTelemetryFrame && frame % 5 == 0)
                    {
                        mLastTelemetryFrame = frame;
                        Log(Debug::Info) << "FNV/ESM4 LIP SAMPLE actor=" << mActor.getCellRef().getRefId()
                                         << " frame=" << frame << " target="
                                         << (dominantLipTarget.empty() ? std::string_view("<neutral>")
                                                                      : dominantLipTarget)
                                         << " value=" << dominantLipValue;
                    }
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
                osg::ref_ptr<osg::Geometry> sourceGeometry = rig.getSourceGeometry();
                if (sourceGeometry == nullptr)
                    return;

                const osg::Vec3Array* baseVertices
                    = dynamic_cast<const osg::Vec3Array*>(sourceGeometry->getVertexArray());
                if (baseVertices == nullptr)
                    return;

                osg::ref_ptr<osg::Vec3Array> baseVertexCopy = new osg::Vec3Array(*baseVertices);
                osg::ref_ptr<osg::Geometry> morphed
                    = new osg::Geometry(*sourceGeometry, osg::CopyOp::SHALLOW_COPY);
                osg::ref_ptr<osg::Vec3Array> activeVertices = new osg::Vec3Array(*baseVertices);
                morphed->setVertexArray(activeVertices);

                morphed->setDataVariance(osg::Object::DYNAMIC);
                morphed->setUseDisplayList(false);
                morphed->setUseVertexBufferObjects(true);
                activeVertices->setDataVariance(osg::Object::DYNAMIC);

                rig.setSourceGeometry(morphed);

                Target t;
                t.mRig = &rig;
                t.mBaseVertices = std::move(baseVertexCopy);
                t.mActiveVertices = activeVertices;
                mTargets.push_back(t);
            }

            void addGeometry(osg::Geometry& geometry)
            {
                // Replacing the drawable can release the last parent-owned reference to the source geometry. Keep
                // it and the immutable vertex baseline alive before touching the parent list.
                osg::ref_ptr<osg::Geometry> sourceGeometry = &geometry;
                const osg::Vec3Array* baseVertices
                    = dynamic_cast<const osg::Vec3Array*>(sourceGeometry->getVertexArray());
                if (baseVertices == nullptr)
                    return;

                osg::ref_ptr<osg::Vec3Array> baseVertexCopy = new osg::Vec3Array(*baseVertices);
                osg::ref_ptr<osg::Geometry> morphed
                    = new osg::Geometry(*sourceGeometry, osg::CopyOp::SHALLOW_COPY);
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
                t.mBaseVertices = std::move(baseVertexCopy);
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

            MWWorld::ConstPtr mActor;
            Animation* mAnimation;
            std::string mModel;
            std::vector<Morph> mMorphs;
            std::vector<Target> mTargets;
            std::vector<float> mLastValues;
            std::uint64_t mLastTelemetryFrame = std::numeric_limits<std::uint64_t>::max();
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

        bool applyFalloutDialogueMorph(
            Resource::ResourceSystem* resourceSystem, const MWWorld::ConstPtr& actor, Animation* animation,
            osg::Node* attached,
            std::string_view model, const ESM4::Npc& traits)
        {
            if (attached == nullptr)
                return false;

            const std::shared_ptr<const FaceGenTri> tri = loadFaceGenTri(resourceSystem, model);
            if (!tri)
                return false;

            osg::ref_ptr<FalloutDialogueMorphDriver> driver
                = new FalloutDialogueMorphDriver(actor, animation, *tri, std::string(model));
            FalloutDialogueMorphTargetVisitor targetVisitor(*driver);
            attached->accept(targetVisitor);

            if (driver->empty())
                return false;

            attached->addUpdateCallback(driver);
            if (std::getenv("OPENMW_FNV_PROOF_LIP_TELEMETRY") != nullptr)
                Log(Debug::Info) << "FNV/ESM4 LIP DRIVER actor=" << actor.getCellRef().getRefId()
                                 << " model=\"" << model << "\" attached=1";
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

        bool worldViewerNodeMapTelemetryEnabled()
        {
            return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_TELEMETRY_HAMMER")
                || worldViewerEnvEnabled("OPENMW_FNV_NODEMAP_AUDIT");
        }

        const std::string& printableNodeName(const std::string& name)
        {
            static const std::string emptyName = "<empty>";
            return name.empty() ? emptyName : name;
        }

        void logWorldViewerNodeMapSnapshot(
            const MWWorld::Ptr& ptr, std::string_view phase, const Animation::NodeMap& nodeMap, std::string_view context)
        {
            if (!worldViewerNodeMapTelemetryEnabled())
                return;

            std::ostringstream details;
            details << context << " nodeMap=" << nodeMap.size() << " nodes=[";
            std::size_t emitted = 0;
            for (const auto& [nodeName, node] : nodeMap)
            {
                if (emitted != 0)
                    details << "; ";
                details << "{key=\"" << printableNodeName(nodeName) << "\"";
                if (node != nullptr)
                {
                    details << " osgName=\"" << printableNodeName(node->getName()) << "\""
                            << " class=\"" << node->className() << "\""
                            << " children=" << node->getNumChildren()
                            << " mask=0x" << std::hex << node->getNodeMask() << std::dec;
                }
                else
                    details << " osgName=\"<null>\"";
                details << "}";
                ++emitted;
            }
            details << "]";
            logWorldViewerActorLedger(ptr, phase, details.str());
        }

        std::size_t countRigBoneMatches(const Animation::NodeMap& nodeMap, const std::vector<std::string>& boneNames)
        {
            std::size_t matches = 0;
            for (const std::string& boneName : boneNames)
            {
                if (boneName.empty())
                    continue;
                if (nodeMap.find(boneName) != nodeMap.end())
                {
                    ++matches;
                    continue;
                }
                for (const auto& [nodeName, node] : nodeMap)
                {
                    if (node != nullptr && Misc::StringUtils::ciEqual(nodeName, boneName))
                    {
                        ++matches;
                        break;
                    }
                }
            }
            return matches;
        }

        bool isStarfieldHumanHeadSurfaceModelLowered(std::string_view lowered)
        {
            return containsAny(lowered,
                { "actors/human/characterassets/male/malehead",
                    "actors\\human\\characterassets\\male\\malehead",
                    "actors/human/characterassets/female/femalehead",
                    "actors\\human\\characterassets\\female\\femalehead" });
        }

        bool isStarfieldHumanFaceAttachmentModelLowered(std::string_view lowered)
        {
            return containsAny(lowered,
                { "actors/human/characterassets/male/lefteye",
                    "actors\\human\\characterassets\\male\\lefteye",
                    "actors/human/characterassets/male/righteye",
                    "actors\\human\\characterassets\\male\\righteye",
                    "actors/human/characterassets/female/lefteye",
                    "actors\\human\\characterassets\\female\\lefteye",
                    "actors/human/characterassets/female/righteye",
                    "actors\\human\\characterassets\\female\\righteye",
                    "actors/human/characterassets/male/eyebrow",
                    "actors\\human\\characterassets\\male\\eyebrow",
                    "actors/human/characterassets/female/eyebrow",
                    "actors\\human\\characterassets\\female\\eyebrow",
                    "actors/human/mesh/hairs/",
                    "actors\\human\\mesh\\hairs\\",
                    "actors/human/mesh/beards/",
                    "actors\\human\\mesh\\beards\\" });
        }

        bool isStarfieldHumanHandSurfaceModelLowered(std::string_view lowered)
        {
            return containsAny(lowered,
                { "actors/human/mesh/nakedhands/",
                    "actors\\human\\mesh\\nakedhands\\",
                    "actors/human/characterassets/male/hands",
                    "actors\\human\\characterassets\\male\\hands",
                    "actors/human/characterassets/female/hands",
                    "actors\\human\\characterassets\\female\\hands" });
        }

        std::vector<std::string> getStarfieldProofClothingSiblingModels(std::string_view model, bool isFemale)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);

            const char* sex = isFemale ? "F" : "M";
            std::vector<std::string> result;
            auto path = [&](std::string_view folder, std::string_view base, std::string_view suffix = ".nif") {
                return std::string("Clothes\\") + std::string(folder) + "\\" + std::string(base) + "_" + sex
                    + std::string(suffix);
            };
            auto add = [&](std::string modelPath) { result.push_back(std::move(modelPath)); };

            if (containsAny(lowered, { "clothes/outfit_utilityoveralls_01/",
                                      "clothes\\outfit_utilityoveralls_01\\" }))
            {
                if (lowered.find("sso_jacket_cooling_01") != std::string::npos)
                {
                    add(path("Outfit_UtilityOveralls_01", "Outfit_UtilityOveralls_SSO_Jacket_Cooling_01_UpperBody"));
                    add(path("Outfit_UtilityOveralls_01", "Outfit_UtilityOveralls_Mechanic_01_LowerBody"));
                }
                else if (lowered.find("sso_jacket_01") != std::string::npos)
                {
                    add(path("Outfit_UtilityOveralls_01", "Outfit_UtilityOveralls_SSO_Jacket_01_UpperBody"));
                    add(path("Outfit_UtilityOveralls_01", "Outfit_UtilityOveralls_Mechanic_01_LowerBody"));
                }
                else if (lowered.find("weldingsleeves_01") != std::string::npos)
                {
                    add(path("Outfit_UtilityOveralls_01", "Outfit_UtilityOveralls_Mechanic_WeldingSleeves_01_UpperBody"));
                    add(path("Outfit_UtilityOveralls_01", "Outfit_UtilityOveralls_Mechanic_01_LowerBody"));
                }
                else if (lowered.find("mechanic_01") != std::string::npos)
                {
                    add(path("Outfit_UtilityOveralls_01", "Outfit_UtilityOveralls_Mechanic_01_UpperBody"));
                    add(path("Outfit_UtilityOveralls_01", "Outfit_UtilityOveralls_Mechanic_01_LowerBody"));
                }
                return result;
            }

            if (containsAny(lowered, { "clothes/outfit_service_uniform_01/",
                                      "clothes\\outfit_service_uniform_01\\" }))
            {
                add(path("Outfit_Service_Uniform_01", "Outfit_Service_Uniform_UpperBody_01"));
                add(path("Outfit_Service_Uniform_01", "Outfit_Service_Uniform_LowerBody_01"));
                add(path("Outfit_Service_Uniform_01", "Outfit_Service_Uniform_Sleeves_01"));
                return result;
            }

            if (containsAny(lowered, { "clothes/outfit_employee_uniform_formal_01/",
                                      "clothes\\outfit_employee_uniform_formal_01\\" }))
            {
                add(path("Outfit_Employee_Uniform_Formal_01", "Outfit_Employee_Uniform_Formal_UpperBody_01"));
                add(path("Outfit_Employee_Uniform_Formal_01", "Outfit_Employee_Uniform_Formal_LowerBody_01"));
                add(path("Outfit_Employee_Uniform_Formal_01", "Outfit_Employee_Uniform_Formal_Sleeves_01"));
                return result;
            }

            if (containsAny(lowered, { "clothes/outfit_colonist_quarterpaddedvest_01/",
                                      "clothes\\outfit_colonist_quarterpaddedvest_01\\" }))
            {
                add(path("Outfit_Colonist_QuarterPaddedVest_01", "Outfit_Colonist_QuarterPaddedVest_01"));
                add(path("Outfit_Colonist_QuarterPaddedVest_01", "Outfit_Colonist_QuarterPaddedVest_01_Pants"));
                add(path("Outfit_Colonist_QuarterPaddedVest_01", "Outfit_Colonist_QuarterPaddedVest_01_Sleeves",
                    "_3rd.nif"));
                return result;
            }

            return result;
        }

        bool shouldAttachFalloutStaticPartToHead(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return isStarfieldHumanHeadSurfaceModelLowered(lowered)
                || isStarfieldHumanFaceAttachmentModelLowered(lowered)
                || lowered.find("characters/head/head") != std::string::npos
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
            return isStarfieldHumanHeadSurfaceModelLowered(lowered)
                || lowered.find("characters/head/head") != std::string::npos
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
            return lowered.find("actors/human/characterassets/male/lefteye") != std::string::npos
                || lowered.find("actors\\human\\characterassets\\male\\lefteye") != std::string::npos
                || lowered.find("actors/human/characterassets/male/righteye") != std::string::npos
                || lowered.find("actors\\human\\characterassets\\male\\righteye") != std::string::npos
                || lowered.find("actors/human/characterassets/female/lefteye") != std::string::npos
                || lowered.find("actors\\human\\characterassets\\female\\lefteye") != std::string::npos
                || lowered.find("actors/human/characterassets/female/righteye") != std::string::npos
                || lowered.find("actors\\human\\characterassets\\female\\righteye") != std::string::npos
                || lowered.find("characters/head/eye") != std::string::npos
                || lowered.find("characters\\head\\eye") != std::string::npos;
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

        float readFalloutProofFloatWithLegacy(const std::string& name, const std::string& legacyName, float fallback)
        {
            if (!name.empty() && std::getenv(name.c_str()) != nullptr)
                return readFalloutProofFloat(name.c_str(), fallback);
            if (!legacyName.empty() && legacyName != name)
                return readFalloutProofFloat(legacyName.c_str(), fallback);
            if (!name.empty())
                return readFalloutProofFloat(name.c_str(), fallback);
            return fallback;
        }

        std::string getFalloutHeadFrameSurfaceLegacyPrefix(std::string_view model, bool headgearStaticPart)
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

        bool useTes5HeadFrameSurfacePrefix(std::string_view model, bool headgearStaticPart)
        {
            if (headgearStaticPart)
                return false;

            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            std::replace(lowered.begin(), lowered.end(), '\\', '/');
            return isStarfieldHumanHeadSurfaceModelLowered(lowered)
                || isStarfieldHumanFaceAttachmentModelLowered(lowered)
                || lowered.find("meshes/actors/human/") != std::string::npos
                || lowered.find("actors/human/") != std::string::npos;
        }

        std::string getFalloutHeadFrameSurfacePrefix(std::string_view model, bool headgearStaticPart)
        {
            const std::string legacyPrefix = getFalloutHeadFrameSurfaceLegacyPrefix(model, headgearStaticPart);
            constexpr std::string_view legacyRoot = "OPENMW_FNV_";
            if (legacyPrefix.empty() || !useTes5HeadFrameSurfacePrefix(model, headgearStaticPart)
                || !Misc::StringUtils::ciStartsWith(legacyPrefix, legacyRoot))
                return legacyPrefix;

            return "OPENMW_WORLD_VIEWER_TES5_" + legacyPrefix.substr(legacyRoot.size());
        }

        osg::Vec3f getFalloutHeadFrameSurfaceOffset(std::string_view model, bool headgearStaticPart)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            const std::string prefix = getFalloutHeadFrameSurfacePrefix(model, headgearStaticPart);
            const std::string legacyPrefix = getFalloutHeadFrameSurfaceLegacyPrefix(model, headgearStaticPart);
            const bool tes5Surface = prefix.find("OPENMW_WORLD_VIEWER_TES5_") == 0;
            const auto readAxis = [&](std::string_view suffix, float fallback) {
                return readFalloutProofFloatWithLegacy(prefix + std::string(suffix), legacyPrefix + std::string(suffix), fallback);
            };

            if (headgearStaticPart)
                return osg::Vec3f(readAxis("_OFFSET_X", 0.f), readAxis("_OFFSET_Y", 0.f), readAxis("_OFFSET_Z", 0.f));
            if (lowered.find("brow") != std::string::npos)
                return osg::Vec3f(readAxis("_OFFSET_X", -0.12f), readAxis("_OFFSET_Y", 0.f), readAxis("_OFFSET_Z", 0.f));
            if (lowered.find("eye") != std::string::npos)
                return osg::Vec3f(readAxis("_OFFSET_X", -0.18f), readAxis("_OFFSET_Y", 0.f), readAxis("_OFFSET_Z", 0.f));
            if (lowered.find("beard") != std::string::npos)
                return osg::Vec3f(readAxis("_OFFSET_X", 0.f), readAxis("_OFFSET_Y", 0.f), readAxis("_OFFSET_Z", 0.f));
            if (isFalloutScalpHairModel(lowered))
                return osg::Vec3f(readAxis("_OFFSET_X", 0.f), readAxis("_OFFSET_Y", 0.f), readAxis("_OFFSET_Z", 0.f));
            if (lowered.find("mouth") != std::string::npos || lowered.find("teeth") != std::string::npos
                || lowered.find("tongue") != std::string::npos)
            {
                const float xFallback = tes5Surface ? 0.f : -0.35f;
                return osg::Vec3f(readAxis("_OFFSET_X", xFallback), readAxis("_OFFSET_Y", 0.f), readAxis("_OFFSET_Z", 0.f));
            }
            return osg::Vec3f();
        }

        osg::Quat getFalloutHeadFrameSurfaceAttitude(std::string_view model, bool headgearStaticPart)
        {
            const std::string prefix = getFalloutHeadFrameSurfacePrefix(model, headgearStaticPart);
            if (prefix.empty())
                return osg::Quat();

            const std::string legacyPrefix = getFalloutHeadFrameSurfaceLegacyPrefix(model, headgearStaticPart);
            constexpr float degreesToRadians = 0.017453292519943295f;
            const float xDegrees
                = readFalloutProofFloatWithLegacy(prefix + "_ROTATION_X", legacyPrefix + "_ROTATION_X", 0.f);
            const float yDegrees
                = readFalloutProofFloatWithLegacy(prefix + "_ROTATION_Y", legacyPrefix + "_ROTATION_Y", 0.f);
            const bool faceInternal = legacyPrefix == "OPENMW_FNV_EYE" || legacyPrefix == "OPENMW_FNV_MOUTH"
                || legacyPrefix == "OPENMW_FNV_BEARD" || legacyPrefix == "OPENMW_FNV_BROW";
            const float zFallback = (faceInternal || legacyPrefix == "OPENMW_FNV_HAIR") ? -90.f : 0.f;
            const float zDegrees
                = readFalloutProofFloatWithLegacy(prefix + "_ROTATION_Z", legacyPrefix + "_ROTATION_Z", zFallback);
            const osg::Quat x(xDegrees * degreesToRadians, osg::Vec3f(1.f, 0.f, 0.f));
            const osg::Quat y(yDegrees * degreesToRadians, osg::Vec3f(0.f, 1.f, 0.f));
            const osg::Quat z(zDegrees * degreesToRadians, osg::Vec3f(0.f, 0.f, 1.f));
            return z * y * x;
        }

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
            return osg::Vec3f(readFalloutProofFloat("OPENMW_FNV_FACE_OFFSET_X", 0.9f),
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

        bool isStarfieldActorSpaceFacePart(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return lowered.find("actors/human/") != std::string::npos
                || lowered.find("actors\\human\\") != std::string::npos;
        }

        bool tes5StaticFaceSurfaceFallbackEnabled(std::string_view model)
        {
            if (worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_FORCE_TES5_STATIC_FACE_SURFACE_ANCHOR"))
                return true;
            if (worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_DISABLE_TES5_STATIC_FACE_SURFACE_ANCHOR"))
                return false;

            return !isStarfieldActorSpaceFacePart(model);
        }

        bool tes5StaticFaceOffsetsDisabled(std::string_view model)
        {
            return worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_DISABLE_TES5_FALLOUT_FACE_OFFSETS")
                || isStarfieldActorSpaceFacePart(model);
        }

        bool tes5UnstableFaceSurfaceQuarantineEnabled()
        {
            return !worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_DISABLE_TES5_UNSTABLE_FACE_SURFACE_QUARANTINE");
        }

        bool isTes5UnstableStaticFaceSurfaceModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            if (lowered.find("lefteye") != std::string::npos || lowered.find("righteye") != std::string::npos)
                return false;
            return isFalloutEyeModel(lowered) || lowered.find("mouth") != std::string::npos
                || lowered.find("teeth") != std::string::npos || lowered.find("tongue") != std::string::npos;
        }

        osg::Vec3f getTes5StaticFaceSurfacePosition(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            const bool child = lowered.find("child") != std::string::npos;
            return osg::Vec3f(readFalloutProofFloat("OPENMW_TES5_STATIC_FACE_SURFACE_X", 0.f),
                readFalloutProofFloat("OPENMW_TES5_STATIC_FACE_SURFACE_Y", child ? 0.25f : 1.3f),
                readFalloutProofFloat("OPENMW_TES5_STATIC_FACE_SURFACE_Z", 120.7f));
        }

        osg::Group* makeTes5StaticFaceSurfaceFrameHelper(osg::Group& parent, std::string_view model)
        {
            constexpr std::string_view helperName = "TES5 Static Face Surface Frame";
            const osg::Vec3f position = getTes5StaticFaceSurfacePosition(model);
            for (unsigned int i = 0; i < parent.getNumChildren(); ++i)
            {
                osg::PositionAttitudeTransform* existing
                    = dynamic_cast<osg::PositionAttitudeTransform*>(parent.getChild(i));
                if (existing != nullptr && existing->getName() == std::string(helperName))
                {
                    existing->setPosition(position);
                    existing->setAttitude(osg::Quat());
                    return existing;
                }
            }

            osg::ref_ptr<osg::PositionAttitudeTransform> helper = new osg::PositionAttitudeTransform;
            helper->setName(std::string(helperName));
            helper->setPosition(position);
            parent.addChild(helper);
            Log(Debug::Info) << "World viewer: inserted TES5 static face surface frame under " << parent.getName()
                             << " offset=(" << position.x() << "," << position.y() << "," << position.z()
                             << ") seedModel=" << model;
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
                return "animatedheadframe";
            return mode;
        }

        osg::Quat getFalloutHeadgearAttitude()
        {
            const char* mode = std::getenv("OPENMW_FNV_HEADGEAR_ROTATION_MODE");
            if (mode == nullptr || mode[0] == '\0')
                return osg::Quat();

            constexpr double halfPi = 1.57079632679489661923;
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
                             << " morphGeometry=" << visitor.mMorphGeometryCount
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
            return isStarfieldHumanHeadSurfaceModelLowered(lowered)
                || isStarfieldHumanFaceAttachmentModelLowered(lowered)
                || lowered.find("characters/head/head") != std::string::npos
                || lowered.find("characters\\head\\head") != std::string::npos
                || containsAny(
                    lowered, { "headgear", "hat", "hair", "beard", "brow", "eye", "mouth", "teeth", "tongue" });
        }

        bool isFalloutFaceTightModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return isStarfieldHumanHeadSurfaceModelLowered(lowered)
                || isStarfieldHumanFaceAttachmentModelLowered(lowered)
                || lowered.find("characters/head/head") != std::string::npos
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

        osg::ref_ptr<osg::Node> applyWorldViewerStaticActorPartProofScale(
            osg::ref_ptr<osg::Node> attached, std::string_view model, const MWWorld::Ptr& ptr)
        {
            if (attached == nullptr)
                return nullptr;

            const float fallback = readFalloutProofFloat("OPENMW_WORLD_VIEWER_SCALE_STATIC_ACTOR_PARTS", 1.f);
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            float scale = fallback;
            std::string_view scaleSource = "OPENMW_WORLD_VIEWER_SCALE_STATIC_ACTOR_PARTS";
            auto applyScaleOverride = [&](const char* name) {
                scale = readFalloutProofFloat(name, scale);
                scaleSource = name;
            };
            if (isFalloutHeadSurfaceModel(lowered))
                applyScaleOverride("OPENMW_WORLD_VIEWER_SCALE_STATIC_ACTOR_HEAD_PARTS");
            else if (isFalloutEyeModel(lowered))
                applyScaleOverride("OPENMW_WORLD_VIEWER_SCALE_STATIC_ACTOR_EYE_PARTS");
            else if (isFalloutBrowModel(lowered))
                applyScaleOverride("OPENMW_WORLD_VIEWER_SCALE_STATIC_ACTOR_BROW_PARTS");
            else if (isFalloutFaceHairModel(lowered))
            {
                scale = readFalloutProofFloat("OPENMW_WORLD_VIEWER_SCALE_STATIC_ACTOR_HAIR_PARTS", scale);
                applyScaleOverride("OPENMW_WORLD_VIEWER_SCALE_STATIC_ACTOR_FACE_HAIR_PARTS");
            }
            else if (isFalloutScalpHairModel(lowered))
                applyScaleOverride("OPENMW_WORLD_VIEWER_SCALE_STATIC_ACTOR_HAIR_PARTS");
            else if (isFalloutMouthSurfaceModel(lowered))
                applyScaleOverride("OPENMW_WORLD_VIEWER_SCALE_STATIC_ACTOR_MOUTH_PARTS");
            else if (isFalloutBareHandSurfaceModel(lowered) || isStarfieldHumanHandSurfaceModelLowered(lowered))
                applyScaleOverride("OPENMW_WORLD_VIEWER_SCALE_STATIC_ACTOR_HAND_PARTS");
            const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(ptr);
            const bool tes5ActorSpaceStaticPart
                = traits != nullptr && !traits->mIsTES4 && !isFallout3OrNewVegas(*traits);
            const bool scaleAroundBoundsCenter = shouldAttachFalloutStaticPartToHead(lowered);
            const bool actorSpacePositionScale = tes5ActorSpaceStaticPart && scaleAroundBoundsCenter;
            const bool tes5ScalpHairPart = isFalloutScalpHairModel(lowered);
            const bool rotateTes5HairPartAxes = actorSpacePositionScale && tes5ScalpHairPart
                && worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_ROTATE_TES5_HAIR_PART_AXES");
            const bool rotateTes5FacePartAxes = actorSpacePositionScale && !tes5ScalpHairPart
                && isFalloutStaticFaceChildPart(lowered)
                && worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_ROTATE_TES5_FACE_PART_AXES");
            const bool rotateTes5StaticActorAxes = rotateTes5HairPartAxes || rotateTes5FacePartAxes;
            if (scale <= 0.f || (std::abs(scale - 1.f) < 0.001f && !rotateTes5StaticActorAxes))
                return attached;

            osg::ref_ptr<osg::MatrixTransform> scaleNode = new osg::MatrixTransform;
            scaleNode->setName("World Viewer Scaled Native Actor Part " + std::string(model));
            osg::Vec3f pivot;
            if (scaleAroundBoundsCenter)
            {
                osg::ComputeBoundsVisitor boundsVisitor;
                attached->accept(boundsVisitor);
                const osg::BoundingBox box = boundsVisitor.getBoundingBox();
                if (box.valid())
                    pivot = box.center();
            }
            const osg::Matrix scaleMatrix = osg::Matrix::scale(scale, scale, scale);
            const float positionScale = actorSpacePositionScale
                ? readFalloutProofFloat("OPENMW_WORLD_VIEWER_POSITION_SCALE_STATIC_ACTOR_PARTS", fallback)
                : scale;
            osg::Matrix axisRotationMatrix;
            axisRotationMatrix.makeIdentity();
            float axisRotationDegrees = 0.f;
            if (rotateTes5HairPartAxes)
            {
                axisRotationDegrees = readFalloutProofFloat("OPENMW_WORLD_VIEWER_TES5_HAIR_PART_ROTATION_Z",
                    readFalloutProofFloat("OPENMW_WORLD_VIEWER_TES5_FACE_PART_ROTATION_Z", 90.f));
                axisRotationMatrix = osg::Matrix::rotate(
                    axisRotationDegrees * static_cast<float>(osg::PI) / 180.f, osg::Vec3f(0.f, 0.f, 1.f));
            }
            else if (rotateTes5FacePartAxes)
            {
                axisRotationDegrees
                    = readFalloutProofFloat("OPENMW_WORLD_VIEWER_TES5_FACE_PART_ROTATION_Z", 90.f);
                axisRotationMatrix = osg::Matrix::rotate(
                    axisRotationDegrees * static_cast<float>(osg::PI) / 180.f, osg::Vec3f(0.f, 0.f, 1.f));
            }
            const osg::Vec3f orientedPivot = rotateTes5StaticActorAxes ? transformPoint(pivot, axisRotationMatrix) : pivot;
            const osg::Vec3f positionPivot(
                orientedPivot.x() * positionScale, orientedPivot.y() * positionScale, orientedPivot.z() * positionScale);
            const osg::Matrix transformMatrix = actorSpacePositionScale
                ? osg::Matrix::translate(-pivot) * axisRotationMatrix * scaleMatrix * osg::Matrix::translate(positionPivot)
                : (scaleAroundBoundsCenter ? osg::Matrix::translate(-pivot) * scaleMatrix * osg::Matrix::translate(pivot)
                                           : scaleMatrix);
            scaleNode->setMatrix(transformMatrix);
            scaleNode->setNodeMask(attached->getNodeMask());

            if (attached->getNumParents() > 0)
            {
                osg::Group* parent = attached->getParent(0);
                if (parent != nullptr && parent->replaceChild(attached.get(), scaleNode.get()))
                    scaleNode->addChild(attached);
            }
            if (scaleNode->getNumChildren() == 0)
                scaleNode->addChild(attached);

            Log(Debug::Info) << "World viewer: scaled native static actor part model=" << model
                             << " scale=" << scale
                             << " source=" << scaleSource
                             << " positionScale=" << positionScale
                             << " actorSpacePositionScale=" << actorSpacePositionScale
                             << " axisRotation=" << axisRotationDegrees
                             << " rotateTes5FacePartAxes=" << rotateTes5FacePartAxes
                             << " rotateTes5HairPartAxes=" << rotateTes5HairPartAxes
                             << " pivot=(" << pivot.x() << "," << pivot.y() << "," << pivot.z() << ")"
                             << " orientedPivot=(" << orientedPivot.x() << "," << orientedPivot.y() << ","
                             << orientedPivot.z() << ")"
                             << " positionPivot=(" << positionPivot.x() << "," << positionPivot.y() << ","
                             << positionPivot.z() << ")"
                             << " pivotMode=" << (actorSpacePositionScale ? 2 : (scaleAroundBoundsCenter ? 1 : 0))
                             << " actor=" << ptr.getCellRef().getRefId();
            return scaleNode;
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

        bool isFalloutLongGunWeapon(const ESM4::Weapon& weapon)
        {
            std::string text = weapon.mEditorId + " " + weapon.mFullName + " " + weapon.mModel;
            Misc::StringUtils::lowerCaseInPlace(text);
            return (weapon.mData.animationType >= 5 && weapon.mData.animationType <= 9)
                || text.find("2hr") != std::string::npos
                || text.find("rifle") != std::string::npos || text.find("shotgun") != std::string::npos
                || text.find("sniper") != std::string::npos || text.find("launcher") != std::string::npos
                || text.find("minigun") != std::string::npos;
        }

        osg::ref_ptr<osg::MatrixTransform> makeFalloutAttachmentHelper(
            osg::Group& parent, std::string_view name, Animation::NodeMap& nodeMap)
        {
            // These helpers are animation targets. NifOsg keyframe callbacks require a NifOsg::MatrixTransform;
            // using a plain osg::MatrixTransform here makes the callback reinterpret the object as the larger NIF
            // transform type and corrupt its matrix when a retail Weapon controller is active.
            Nif::NiTransform bindTransform = Nif::NiTransform::getIdentity();
            if (Misc::StringUtils::ciEqual(name, "Weapon"))
            {
                // FO3 and FNV ship the same empty Weapon node under Bip01 R Hand. The scene optimizer removes that
                // empty node, so retain its authored local bind on the replacement animation target.
                bindTransform.mTranslation = osg::Vec3f(6.171f, 1.989f, 0.759f);
                bindTransform.mRotation.mValues[0][0] = 0.991f;
                bindTransform.mRotation.mValues[0][1] = -0.051f;
                bindTransform.mRotation.mValues[0][2] = -0.123f;
                bindTransform.mRotation.mValues[1][0] = 0.125f;
                bindTransform.mRotation.mValues[1][1] = 0.035f;
                bindTransform.mRotation.mValues[1][2] = 0.992f;
                bindTransform.mRotation.mValues[2][0] = -0.046f;
                bindTransform.mRotation.mValues[2][1] = -0.998f;
                bindTransform.mRotation.mValues[2][2] = 0.041f;
            }

            osg::ref_ptr<NifOsg::MatrixTransform> helper = new NifOsg::MatrixTransform(bindTransform);
            helper->setName(std::string(name));
            helper->setDataVariance(osg::Object::DYNAMIC);
            helper->setUserValue("esm4SyntheticAttachmentHelper", 1);
            if (Misc::StringUtils::ciEqual(name, "Weapon"))
            {
                const bool overrideBind = std::getenv("OPENMW_FNV_WEAPON_OFFSET_X") != nullptr
                    || std::getenv("OPENMW_FNV_WEAPON_OFFSET_Y") != nullptr
                    || std::getenv("OPENMW_FNV_WEAPON_OFFSET_Z") != nullptr
                    || std::getenv("OPENMW_FNV_WEAPON_ROTATION_X") != nullptr
                    || std::getenv("OPENMW_FNV_WEAPON_ROTATION_Y") != nullptr
                    || std::getenv("OPENMW_FNV_WEAPON_ROTATION_Z") != nullptr;
                if (overrideBind)
                {
                    const osg::Vec3f offset(readFalloutProofFloat("OPENMW_FNV_WEAPON_OFFSET_X", 6.171f),
                        readFalloutProofFloat("OPENMW_FNV_WEAPON_OFFSET_Y", 1.989f),
                        readFalloutProofFloat("OPENMW_FNV_WEAPON_OFFSET_Z", 0.759f));
                    constexpr float degreesToRadians = 0.017453292519943295f;
                    const float xDegrees = readFalloutProofFloat("OPENMW_FNV_WEAPON_ROTATION_X", -87.648f);
                    const float yDegrees = readFalloutProofFloat("OPENMW_FNV_WEAPON_ROTATION_Y", 2.636f);
                    const float zDegrees = readFalloutProofFloat("OPENMW_FNV_WEAPON_ROTATION_Z", 7.187f);
                    const osg::Quat x(xDegrees * degreesToRadians, osg::Vec3f(1.f, 0.f, 0.f));
                    const osg::Quat y(yDegrees * degreesToRadians, osg::Vec3f(0.f, 1.f, 0.f));
                    const osg::Quat z(zDegrees * degreesToRadians, osg::Vec3f(0.f, 0.f, 1.f));
                    const osg::Quat rotation = z * y * x;
                    helper->setRotation(rotation);
                    helper->setTranslation(offset);
                    Log(Debug::Info) << "FNV/ESM4 diag: weapon helper override offset=(" << offset.x() << ","
                                     << offset.y() << "," << offset.z() << ") rotation=(" << xDegrees << ","
                                     << yDegrees << "," << zDegrees << ")";
                }
                else
                    Log(Debug::Info) << "FNV/ESM4 diag: weapon helper uses retail FO3/FNV bind transform";
            }
            parent.addChild(helper);
            nodeMap.emplace(std::string(name), helper);
            return helper;
        }

        struct WorldViewerWeaponIkSolve
        {
            osg::Vec3f mMid;
            osg::Vec3f mEnd;
            float mError = -1.f;
            bool mReachable = false;
            bool mSolved = false;
        };

        struct WorldViewerWeaponIkRotationProbe
        {
            bool mSolved = false;
            float mError = -1.f;
            const char* mOrder = "none";
        };

        bool worldViewerFONVWeaponIkEnabled()
        {
            if (const char* value = std::getenv("OPENMW_FNV_WEAPON_IK"))
                return value[0] != '0';
            if (const char* value = std::getenv("OPENMW_ESM4_WEAPON_IK"))
                return value[0] != '0';
            return false;
        }

        bool worldViewerFONVWeaponFrameStabilizerEnabled()
        {
            // Diagnostic fallback only. Retail drives the Weapon node from the selected animation family.
            return worldViewerEnvEnabled("OPENMW_FNV_WEAPON_FRAME_STABILIZER")
                || worldViewerEnvEnabled("OPENMW_ESM4_WEAPON_FRAME_STABILIZER");
        }

        bool worldViewerFONVLongGunOffhandIkEnabled()
        {
            // Diagnostic fallback only. Authored 2hr/2ha/etc. sequences contain the retail offhand pose.
            return worldViewerEnvEnabled("OPENMW_FNV_LONG_GUN_OFFHAND_IK")
                || worldViewerEnvEnabled("OPENMW_ESM4_LONG_GUN_OFFHAND_IK");
        }

        osg::Vec3f normalizeFalloutIkVector(osg::Vec3f value, const osg::Vec3f& fallback)
        {
            if (value.normalize() <= 0.0001f)
                return fallback;
            return value;
        }

        float falloutIkDirectionAngleDegrees(osg::Vec3f left, osg::Vec3f right)
        {
            if (left.normalize() <= 0.0001f || right.normalize() <= 0.0001f)
                return -1.f;
            return std::acos(std::clamp(left * right, -1.f, 1.f)) * 57.29577951308232f;
        }

        void setFalloutIkTransformWorldMatrix(osg::MatrixTransform& transform, const osg::Matrix& desiredWorld)
        {
            osg::Group* parent = transform.getNumParents() > 0 ? transform.getParent(0) : nullptr;
            osg::Matrixf local = desiredWorld * osg::Matrix::inverse(getNodeWorldMatrix(parent));
            transform.setMatrix(local);
            transform.dirtyBound();

            if (osgAnimation::Bone* bone = dynamic_cast<osgAnimation::Bone*>(&transform))
            {
                if (osgAnimation::Bone* boneParent = bone->getBoneParent())
                    bone->setMatrixInSkeletonSpace(local * boneParent->getMatrixInSkeletonSpace());
                else
                    bone->setMatrixInSkeletonSpace(local);
            }
        }

        WorldViewerWeaponIkSolve solveFalloutWeaponIkTwoBone(
            const osg::Vec3f& root, const osg::Vec3f& mid, const osg::Vec3f& end,
            const osg::Vec3f& requestedTarget, const osg::Vec3f& poleHint)
        {
            WorldViewerWeaponIkSolve result;
            const float upperLength = std::clamp((mid - root).length(), 4.f, 36.f);
            const float lowerLength = std::clamp((end - mid).length(), 4.f, 36.f);

            osg::Vec3f rootToTarget = requestedTarget - root;
            float targetDistance = rootToTarget.normalize();
            if (targetDistance <= 0.0001f)
                return result;

            const float maxReach = std::max(0.001f, upperLength + lowerLength - 0.01f);
            const osg::Vec3f target = root + rootToTarget * std::min(targetDistance, maxReach);
            targetDistance = std::min(targetDistance, maxReach);
            result.mReachable = targetDistance < maxReach - 0.05f;

            const float along = std::clamp(
                (upperLength * upperLength - lowerLength * lowerLength + targetDistance * targetDistance)
                    / (2.f * targetDistance),
                0.f, upperLength);
            const float height = std::sqrt(std::max(0.f, upperLength * upperLength - along * along));
            osg::Vec3f pole = poleHint - rootToTarget * (poleHint * rootToTarget);
            if (pole.normalize() <= 0.0001f)
                pole = normalizeFalloutIkVector(
                    mid - (root + rootToTarget * ((mid - root) * rootToTarget)), osg::Vec3f(0.f, 0.f, -1.f));

            result.mMid = root + rootToTarget * along + pole * height;
            result.mEnd = target;
            result.mError = (result.mEnd - requestedTarget).length();
            result.mSolved = true;
            return result;
        }

        WorldViewerWeaponIkRotationProbe rotateFalloutWeaponIkSegmentToBest(
            osg::MatrixTransform& bone, osg::Node& endpoint, const osg::Vec3f& desiredSegmentEnd, float strength)
        {
            WorldViewerWeaponIkRotationProbe result;
            const osg::Matrix originalWorld = getNodeWorldMatrix(&bone);
            const osg::Vec3f boneOrigin = originalWorld.getTrans();
            osg::Vec3f from = getNodeWorldMatrix(&endpoint).getTrans() - boneOrigin;
            osg::Vec3f to = desiredSegmentEnd - boneOrigin;
            if (from.normalize() <= 0.0001f || to.normalize() <= 0.0001f)
                return result;

            osg::Quat delta;
            delta.makeRotate(from, to);
            osg::Quat limitedDelta;
            limitedDelta.slerp(std::clamp(strength, 0.f, 1.f), osg::Quat(), delta);

            const auto testCandidate = [&](const osg::Quat& rotation) {
                setFalloutIkTransformWorldMatrix(
                    bone, osg::Matrix::rotate(rotation) * osg::Matrix::translate(boneOrigin));
                return (getNodeWorldMatrix(&endpoint).getTrans() - desiredSegmentEnd).length();
            };

            const osg::Quat preRotation = limitedDelta * originalWorld.getRotate();
            const float preError = testCandidate(preRotation);
            setFalloutIkTransformWorldMatrix(bone, originalWorld);

            const osg::Quat postRotation = originalWorld.getRotate() * limitedDelta;
            const float postError = testCandidate(postRotation);
            setFalloutIkTransformWorldMatrix(bone, originalWorld);

            if (preError <= postError)
            {
                setFalloutIkTransformWorldMatrix(
                    bone, osg::Matrix::rotate(preRotation) * osg::Matrix::translate(boneOrigin));
                result.mError = preError;
                result.mOrder = "pre";
            }
            else
            {
                setFalloutIkTransformWorldMatrix(
                    bone, osg::Matrix::rotate(postRotation) * osg::Matrix::translate(boneOrigin));
                result.mError = postError;
                result.mOrder = "post";
            }
            result.mSolved = true;
            return result;
        }

        WorldViewerWeaponIkRotationProbe rotateFalloutWeaponIkAxisToBest(
            osg::MatrixTransform& transform, const osg::Vec3f& localAxis,
            const osg::Vec3f& desiredWorldAxis, float strength)
        {
            WorldViewerWeaponIkRotationProbe result;
            const osg::Matrix originalWorld = getNodeWorldMatrix(&transform);
            osg::Vec3f current = originalWorld.getRotate() * localAxis;
            osg::Vec3f desired = desiredWorldAxis;
            if (current.normalize() <= 0.0001f || desired.normalize() <= 0.0001f)
                return result;

            osg::Quat delta;
            delta.makeRotate(current, desired);
            osg::Quat limitedDelta;
            limitedDelta.slerp(std::clamp(strength, 0.f, 1.f), osg::Quat(), delta);

            const osg::Vec3f origin = originalWorld.getTrans();
            const auto testCandidate = [&](const osg::Quat& rotation) {
                setFalloutIkTransformWorldMatrix(
                    transform, osg::Matrix::rotate(rotation) * osg::Matrix::translate(origin));
                return falloutIkDirectionAngleDegrees(getNodeWorldMatrix(&transform).getRotate() * localAxis, desired);
            };

            const osg::Quat preRotation = limitedDelta * originalWorld.getRotate();
            const float preAngle = testCandidate(preRotation);
            setFalloutIkTransformWorldMatrix(transform, originalWorld);

            const osg::Quat postRotation = originalWorld.getRotate() * limitedDelta;
            const float postAngle = testCandidate(postRotation);
            setFalloutIkTransformWorldMatrix(transform, originalWorld);

            const float beforeAngle = falloutIkDirectionAngleDegrees(current, desired);
            if (beforeAngle >= 0.f && beforeAngle <= preAngle + 0.25f && beforeAngle <= postAngle + 0.25f)
            {
                result.mSolved = true;
                result.mError = beforeAngle;
                result.mOrder = "keep";
                return result;
            }

            if (preAngle >= 0.f && (postAngle < 0.f || preAngle <= postAngle))
            {
                setFalloutIkTransformWorldMatrix(
                    transform, osg::Matrix::rotate(preRotation) * osg::Matrix::translate(origin));
                result.mError = preAngle;
                result.mOrder = "pre";
            }
            else
            {
                setFalloutIkTransformWorldMatrix(
                    transform, osg::Matrix::rotate(postRotation) * osg::Matrix::translate(origin));
                result.mError = postAngle;
                result.mOrder = "post";
            }
            result.mSolved = true;
            return result;
        }

        struct WorldViewerHandOrientationProbe
        {
            bool mSolved = false;
            float mForwardError = -1.f;
            float mPalmError = -1.f;
            float mScore = -1.f;
            std::string mCandidate = "none";
        };

        osg::Vec3f projectFalloutIkDirectionOnPlane(
            osg::Vec3f direction, const osg::Vec3f& planeNormal, const osg::Vec3f& fallback)
        {
            const osg::Vec3f normal = normalizeFalloutIkVector(planeNormal, osg::Vec3f(0.f, 0.f, 1.f));
            direction -= normal * (direction * normal);
            return normalizeFalloutIkVector(direction, fallback);
        }

        float signedFalloutIkAngleAroundAxis(osg::Vec3f from, osg::Vec3f to, osg::Vec3f axis)
        {
            axis = normalizeFalloutIkVector(axis, osg::Vec3f(0.f, 0.f, 1.f));
            from = projectFalloutIkDirectionOnPlane(from, axis, osg::Vec3f(1.f, 0.f, 0.f));
            to = projectFalloutIkDirectionOnPlane(to, axis, osg::Vec3f(1.f, 0.f, 0.f));
            const float angle = std::acos(std::clamp(from * to, -1.f, 1.f));
            const osg::Vec3f cross = from ^ to;
            return cross * axis < 0.f ? -angle : angle;
        }

        WorldViewerHandOrientationProbe orientFalloutWeaponIkHandGripToBest(osg::MatrixTransform& hand,
            const osg::Vec3f& desiredWorldForward, const osg::Vec3f& desiredWorldPalm,
            float strength, std::string_view side, bool longGun)
        {
            struct AxisCandidate
            {
                osg::Vec3f mAxis;
                const char* mName;
            };

            const std::array<AxisCandidate, 6> axes = { {
                { osg::Vec3f(0.f, 1.f, 0.f), "+Y" },
                { osg::Vec3f(1.f, 0.f, 0.f), "+X" },
                { osg::Vec3f(-1.f, 0.f, 0.f), "-X" },
                { osg::Vec3f(0.f, 0.f, 1.f), "+Z" },
                { osg::Vec3f(0.f, 0.f, -1.f), "-Z" },
                { osg::Vec3f(0.f, -1.f, 0.f), "-Y" },
            } };

            const osg::Matrix originalWorld = getNodeWorldMatrix(&hand);
            const osg::Quat originalRotation = originalWorld.getRotate();
            const osg::Vec3f origin = originalWorld.getTrans();
            const osg::Vec3f targetForward
                = normalizeFalloutIkVector(desiredWorldForward, osg::Vec3f(0.f, 1.f, 0.f));
            const osg::Vec3f targetPalm = projectFalloutIkDirectionOnPlane(
                desiredWorldPalm, targetForward, osg::Vec3f(1.f, 0.f, 0.f));
            const std::string forced = [] {
                const char* value = std::getenv("OPENMW_FNV_WEAPON_IK_HAND_ORIENTATION");
                return value != nullptr ? std::string(value) : std::string();
            }();
            const std::string forcedSide = [&] {
                const char* value = side == "right" ? std::getenv("OPENMW_FNV_WEAPON_IK_RIGHT_HAND_ORIENTATION")
                                                    : std::getenv("OPENMW_FNV_WEAPON_IK_LEFT_HAND_ORIENTATION");
                return value != nullptr ? std::string(value) : std::string();
            }();
            const std::string forcedClass = [&] {
                const char* value = longGun ? std::getenv("OPENMW_FNV_WEAPON_IK_LONG_GUN_HAND_ORIENTATION")
                                            : std::getenv("OPENMW_FNV_WEAPON_IK_SIDEARM_HAND_ORIENTATION");
                return value != nullptr ? std::string(value) : std::string();
            }();
            const std::string forcedClassSide = [&] {
                const char* value = nullptr;
                if (longGun)
                    value = side == "right" ? std::getenv("OPENMW_FNV_WEAPON_IK_LONG_GUN_RIGHT_HAND_ORIENTATION")
                                            : std::getenv("OPENMW_FNV_WEAPON_IK_LONG_GUN_LEFT_HAND_ORIENTATION");
                else
                    value = side == "right" ? std::getenv("OPENMW_FNV_WEAPON_IK_SIDEARM_RIGHT_HAND_ORIENTATION")
                                            : std::getenv("OPENMW_FNV_WEAPON_IK_SIDEARM_LEFT_HAND_ORIENTATION");
                return value != nullptr ? std::string(value) : std::string();
            }();
            const auto matchesForced = [&](const std::string& value, const std::string& candidate,
                                           unsigned int candidateIndex, const char* forwardName,
                                           const char* palmName) {
                return value == candidate || value == std::to_string(candidateIndex)
                    || value == std::string(forwardName) + "/" + palmName;
            };

            WorldViewerHandOrientationProbe best;
            unsigned int index = 0;
            for (const AxisCandidate& forwardAxis : axes)
            {
                for (const AxisCandidate& palmAxis : axes)
                {
                    if (std::abs(forwardAxis.mAxis * palmAxis.mAxis) > 0.01f)
                        continue;

                    std::ostringstream candidateName;
                    candidateName << side << ":" << index << ":" << forwardAxis.mName << "/" << palmAxis.mName;
                    const std::string candidate = candidateName.str();
                    const bool hasForced = !forced.empty() || !forcedSide.empty() || !forcedClass.empty()
                        || !forcedClassSide.empty();
                    const bool forcedMatch = !hasForced
                        || matchesForced(forcedClassSide, candidate, index, forwardAxis.mName, palmAxis.mName)
                        || matchesForced(forcedSide, candidate, index, forwardAxis.mName, palmAxis.mName)
                        || matchesForced(forcedClass, candidate, index, forwardAxis.mName, palmAxis.mName)
                        || matchesForced(forced, candidate, index, forwardAxis.mName, palmAxis.mName);
                    ++index;
                    if (!forcedMatch)
                        continue;

                    osg::Vec3f currentForward = originalRotation * forwardAxis.mAxis;
                    if (currentForward.normalize() <= 0.0001f)
                        continue;

                    osg::Quat forwardDelta;
                    forwardDelta.makeRotate(currentForward, targetForward);
                    osg::Quat targetRotation = forwardDelta * originalRotation;
                    const osg::Vec3f palmAfterForward = projectFalloutIkDirectionOnPlane(
                        targetRotation * palmAxis.mAxis, targetForward, targetPalm);
                    osg::Quat rollDelta(
                        signedFalloutIkAngleAroundAxis(palmAfterForward, targetPalm, targetForward), targetForward);
                    targetRotation = rollDelta * targetRotation;

                    osg::Quat limitedRotation;
                    limitedRotation.slerp(std::clamp(strength, 0.f, 1.f), originalRotation, targetRotation);
                    setFalloutIkTransformWorldMatrix(
                        hand, osg::Matrix::rotate(limitedRotation) * osg::Matrix::translate(origin));
                    const osg::Quat appliedRotation = getNodeWorldMatrix(&hand).getRotate();
                    const float forwardError
                        = falloutIkDirectionAngleDegrees(appliedRotation * forwardAxis.mAxis, targetForward);
                    const float palmError = falloutIkDirectionAngleDegrees(
                        projectFalloutIkDirectionOnPlane(appliedRotation * palmAxis.mAxis, targetForward, targetPalm),
                        targetPalm);
                    if (forwardError < 0.f || palmError < 0.f)
                    {
                        setFalloutIkTransformWorldMatrix(hand, originalWorld);
                        continue;
                    }

                    const float preference = (std::string(forwardAxis.mName) == "+Y" ? 0.f : 0.25f)
                        + (side == "right" && std::string(palmAxis.mName) == "-X" ? 0.f : 0.05f)
                        + (side == "left" && std::string(palmAxis.mName) == "+X" ? 0.f : 0.05f);
                    const float score = forwardError * 4.f + palmError + preference;
                    if (!best.mSolved || score < best.mScore)
                    {
                        best.mSolved = true;
                        best.mForwardError = forwardError;
                        best.mPalmError = palmError;
                        best.mScore = score;
                        best.mCandidate = candidate;
                    }
                    setFalloutIkTransformWorldMatrix(hand, originalWorld);
                }
            }

            if (best.mSolved)
            {
                const std::string selected = best.mCandidate;
                const std::size_t slash = selected.rfind('/');
                const std::size_t colon = selected.rfind(':', slash == std::string::npos ? selected.size() : slash);
                const std::string forwardName = colon == std::string::npos || slash == std::string::npos
                    ? "+Y"
                    : selected.substr(colon + 1, slash - colon - 1);
                const std::string palmName = slash == std::string::npos ? "-X" : selected.substr(slash + 1);
                const auto axisByName = [&](const std::string& name) {
                    for (const AxisCandidate& axis : axes)
                    {
                        if (name == axis.mName)
                            return axis.mAxis;
                    }
                    return osg::Vec3f(0.f, 1.f, 0.f);
                };
                const osg::Vec3f localForward = axisByName(forwardName);
                const osg::Vec3f localPalm = axisByName(palmName);
                osg::Vec3f currentForward = originalRotation * localForward;
                currentForward.normalize();
                osg::Quat forwardDelta;
                forwardDelta.makeRotate(currentForward, targetForward);
                osg::Quat targetRotation = forwardDelta * originalRotation;
                const osg::Vec3f palmAfterForward = projectFalloutIkDirectionOnPlane(
                    targetRotation * localPalm, targetForward, targetPalm);
                osg::Quat rollDelta(
                    signedFalloutIkAngleAroundAxis(palmAfterForward, targetPalm, targetForward), targetForward);
                targetRotation = rollDelta * targetRotation;
                osg::Quat limitedRotation;
                limitedRotation.slerp(std::clamp(strength, 0.f, 1.f), originalRotation, targetRotation);
                setFalloutIkTransformWorldMatrix(
                    hand, osg::Matrix::rotate(limitedRotation) * osg::Matrix::translate(origin));
            }
            else
                setFalloutIkTransformWorldMatrix(hand, originalWorld);

            return best;
        }

        osg::Vec3f falloutWeaponIkAxisFromName(std::string_view name)
        {
            if (Misc::StringUtils::ciEqual(name, "-X"))
                return osg::Vec3f(-1.f, 0.f, 0.f);
            if (Misc::StringUtils::ciEqual(name, "+Y") || Misc::StringUtils::ciEqual(name, "Y"))
                return osg::Vec3f(0.f, 1.f, 0.f);
            if (Misc::StringUtils::ciEqual(name, "-Y"))
                return osg::Vec3f(0.f, -1.f, 0.f);
            if (Misc::StringUtils::ciEqual(name, "+Z") || Misc::StringUtils::ciEqual(name, "Z"))
                return osg::Vec3f(0.f, 0.f, 1.f);
            if (Misc::StringUtils::ciEqual(name, "-Z"))
                return osg::Vec3f(0.f, 0.f, -1.f);
            return osg::Vec3f(1.f, 0.f, 0.f);
        }

        const char* falloutWeaponIkAxisName(const osg::Vec3f& axis)
        {
            if (axis.x() < -0.5f)
                return "-X";
            if (axis.y() > 0.5f)
                return "+Y";
            if (axis.y() < -0.5f)
                return "-Y";
            if (axis.z() > 0.5f)
                return "+Z";
            if (axis.z() < -0.5f)
                return "-Z";
            return "+X";
        }

        osg::Vec3f chooseFalloutWeaponIkAimAxis(osg::MatrixTransform& weaponFrame, const osg::Vec3f& forward)
        {
            if (const char* env = std::getenv("OPENMW_FNV_WEAPON_IK_AIM_AXIS"))
                return falloutWeaponIkAxisFromName(env);

            const osg::Quat rotation = getNodeWorldMatrix(&weaponFrame).getRotate();
            const osg::Vec3f candidates[] = {
                osg::Vec3f(1.f, 0.f, 0.f), osg::Vec3f(-1.f, 0.f, 0.f),
                osg::Vec3f(0.f, 1.f, 0.f), osg::Vec3f(0.f, -1.f, 0.f),
                osg::Vec3f(0.f, 0.f, 1.f), osg::Vec3f(0.f, 0.f, -1.f)
            };

            float bestDot = -1.f;
            osg::Vec3f bestAxis(1.f, 0.f, 0.f);
            for (const osg::Vec3f& axis : candidates)
            {
                osg::Vec3f world = rotation * axis;
                if (world.normalize() <= 0.0001f)
                    continue;
                const float dot = world * forward;
                if (dot > bestDot)
                {
                    bestDot = dot;
                    bestAxis = axis;
                }
            }
            return bestAxis;
        }

        bool isFalloutWeaponIkLongGun(const ESM4::Weapon& weapon)
        {
            if (const char* env = std::getenv("OPENMW_FNV_WEAPON_IK_LONG_GUN"))
                return env[0] != '0';

            return isFalloutLongGunWeapon(weapon);
        }

        float readFalloutWeaponIkClassFloat(
            bool longGun, const char* side, const char* component, float fallback)
        {
            const std::string classPrefix = longGun ? "OPENMW_FNV_WEAPON_IK_LONG_GUN_"
                                                    : "OPENMW_FNV_WEAPON_IK_SIDEARM_";
            const std::string sideText(side);
            const std::string componentText(component);
            const std::string className = classPrefix + sideText + "_" + componentText;
            if (std::getenv(className.c_str()) != nullptr)
                return readFalloutProofFloat(className.c_str(), fallback);

            const std::string genericName = std::string("OPENMW_FNV_WEAPON_IK_") + sideText + "_" + componentText;
            return readFalloutProofFloat(genericName.c_str(), fallback);
        }

        bool applyFalloutWeaponGripIk(const Animation::NodeMap& nodeMap, osg::Group* objectRoot,
            SceneUtil::Skeleton* skeleton, const MWWorld::Ptr& ptr, const ESM4::Npc& traits)
        {
            static std::map<std::string, unsigned int> sFrameLogs;
            static std::map<std::string, bool> sProofLogged;
            const std::string actorKey
                = traits.mEditorId + "|" + ptr.getCellRef().getRefId().serializeText();
            unsigned int& frameLogs = sFrameLogs[actorKey];

            const auto logBlocked = [&](std::string_view reason) {
                if (!worldViewerFONVWeaponIkEnabled() && std::getenv("OPENMW_FNV_DISABLE_WEAPON_IK") == nullptr)
                    return;
                if (frameLogs >= 4)
                    return;
                ++frameLogs;
                Log(Debug::Warning) << "FNV/ESM4 proof: weapon IK blocked actor=" << traits.mEditorId
                                    << " ref=" << ptr.getCellRef().getRefId()
                                    << " reason=" << reason
                                    << " targetMatch=" << isFonvProofTargetActor(ptr, traits)
                                    << " disableWeaponIk="
                                    << (std::getenv("OPENMW_FNV_DISABLE_WEAPON_IK") != nullptr)
                                    << " runtime=loaded-pending-runtime gate=runtime-fnv-weapon-ik";
            };

            if (std::getenv("OPENMW_FNV_DISABLE_WEAPON_IK") != nullptr)
            {
                logBlocked("disabled-by-env");
                return false;
            }
            if (!worldViewerFONVWeaponIkEnabled())
                return false;
            if (!isFonvProofTargetActor(ptr, traits))
            {
                logBlocked("not-proof-target");
                return false;
            }

            const ESM4::Weapon* weapon = MWClass::ESM4Npc::getEquippedWeapon(ptr);
            if (weapon == nullptr)
            {
                logBlocked("no-equipped-weapon");
                return false;
            }

            osg::MatrixTransform* weaponFrame = dynamic_cast<osg::MatrixTransform*>(
                findBestAttachmentNode(nodeMap, { "Weapon", "weapon", "Bip01 Weapon", "Bip01 R Hand" }));
            if (weaponFrame == nullptr)
            {
                logBlocked("missing-weapon-frame");
                return false;
            }

            const auto findMatrix = [&](std::initializer_list<std::string_view> names) {
                return dynamic_cast<osg::MatrixTransform*>(findBestAttachmentNode(nodeMap, names));
            };
            osg::MatrixTransform* rightClavicle = findMatrix({ "Bip01 R Clavicle", "bip01 r clavicle" });
            osg::MatrixTransform* rightUpper = findMatrix({ "Bip01 R UpperArm", "bip01 r upperarm" });
            osg::MatrixTransform* rightForearm = findMatrix({ "Bip01 R Forearm", "bip01 r forearm" });
            osg::MatrixTransform* rightHand = findMatrix({ "Bip01 R Hand", "bip01 r hand" });
            osg::MatrixTransform* leftClavicle = findMatrix({ "Bip01 L Clavicle", "bip01 l clavicle" });
            osg::MatrixTransform* leftUpper = findMatrix({ "Bip01 L UpperArm", "bip01 l upperarm" });
            osg::MatrixTransform* leftForearm = findMatrix({ "Bip01 L Forearm", "bip01 l forearm" });
            osg::MatrixTransform* leftHand = findMatrix({ "Bip01 L Hand", "bip01 l hand" });
            if (rightUpper == nullptr || rightForearm == nullptr || rightHand == nullptr || leftUpper == nullptr
                || leftForearm == nullptr || leftHand == nullptr)
            {
                logBlocked("missing-arm-bone");
                return false;
            }

            osg::Group* spine = findBestAttachmentNode(
                nodeMap, { "Bip01 Spine2", "bip01 spine2", "Bip01 Spine1", "bip01 spine1" });
            const osg::Vec3f chest = getNodeWorldMatrix(spine != nullptr ? spine : objectRoot).getTrans();
            osg::Group* headNode = findBestAttachmentNode(nodeMap, { "Bip01 Head", "bip01 head", "Head", "head" });
            osg::Vec3f head = chest + osg::Vec3f(0.f, 0.f, 16.f);
            if (headNode != nullptr)
                head = getNodeWorldMatrix(headNode).getTrans();

            const ESM::Position& position = ptr.getRefData().getPosition();
            const float yaw = position.rot[2];
            osg::Vec3f forward(std::sin(yaw), std::cos(yaw), 0.f);
            forward = normalizeFalloutIkVector(forward, osg::Vec3f(0.f, 1.f, 0.f));
            osg::Vec3f right(forward.y(), -forward.x(), 0.f);
            right = normalizeFalloutIkVector(right, osg::Vec3f(1.f, 0.f, 0.f));
            const osg::Vec3f up(0.f, 0.f, 1.f);

            const bool longGun = isFalloutWeaponIkLongGun(*weapon);
            const float shoulderLift = std::clamp((head.z() - chest.z()) * 0.62f, 8.f, 14.f);
            const osg::Vec3f aimAnchor = chest + up * shoulderLift;
            const char* targetStyle = longGun ? "head-shoulder-long-gun" : "chest-sidearm";
            const osg::Vec3f rightTarget = longGun
                ? aimAnchor + forward * readFalloutWeaponIkClassFloat(longGun, "RIGHT", "FORWARD", 22.f)
                    + right * readFalloutWeaponIkClassFloat(longGun, "RIGHT", "SIDE", 10.f)
                    - up * readFalloutWeaponIkClassFloat(longGun, "RIGHT", "DROP", 1.f)
                : chest + forward * readFalloutWeaponIkClassFloat(longGun, "RIGHT", "FORWARD", 34.f)
                    + right * readFalloutWeaponIkClassFloat(longGun, "RIGHT", "SIDE", 10.f)
                    - up * readFalloutWeaponIkClassFloat(longGun, "RIGHT", "DROP", 3.f);
            const osg::Vec3f leftTarget = longGun
                ? aimAnchor + forward * readFalloutWeaponIkClassFloat(longGun, "LEFT", "FORWARD", 18.f)
                    - right * readFalloutWeaponIkClassFloat(longGun, "LEFT", "SIDE", 7.f)
                    - up * readFalloutWeaponIkClassFloat(longGun, "LEFT", "DROP", 0.5f)
                : chest + forward * readFalloutWeaponIkClassFloat(longGun, "LEFT", "FORWARD", 28.f)
                    - right * readFalloutWeaponIkClassFloat(longGun, "LEFT", "SIDE", 8.f)
                    - up * readFalloutWeaponIkClassFloat(longGun, "LEFT", "DROP", 5.f);
            const float strength = std::clamp(readFalloutProofFloat("OPENMW_FNV_WEAPON_IK_STRENGTH", 1.f), 0.f, 1.f);
            if (strength <= 0.f)
            {
                logBlocked("zero-strength");
                return false;
            }

            const osg::Vec3f rightHandBefore = getNodeWorldMatrix(rightHand).getTrans();
            const osg::Vec3f leftHandBefore = getNodeWorldMatrix(leftHand).getTrans();
            const osg::Vec3f weaponBefore = getNodeWorldMatrix(weaponFrame).getTrans();
            const osg::Vec3f weaponAimAxis = chooseFalloutWeaponIkAimAxis(*weaponFrame, forward);
            const osg::Vec3f weaponForwardBefore
                = normalizeFalloutIkVector(getNodeWorldMatrix(weaponFrame).getRotate() * weaponAimAxis, forward);

            unsigned int solved = 0;
            float rightError = -1.f;
            float leftError = -1.f;
            bool rightReachable = false;
            bool leftReachable = false;
            std::string rightOrders;
            std::string leftOrders;
            float rightHandAimError = -1.f;
            float leftHandAimError = -1.f;
            float rightHandPalmError = -1.f;
            float leftHandPalmError = -1.f;
            std::string rightHandOrientationCandidate = "none";
            std::string leftHandOrientationCandidate = "none";

            const auto solveArm = [&](osg::MatrixTransform* clavicle, osg::MatrixTransform& upper, osg::MatrixTransform& forearm,
                                      osg::MatrixTransform& hand, const osg::Vec3f& target,
                                      const osg::Vec3f& poleHint, const osg::Vec3f& desiredHandForward,
                                      const osg::Vec3f& desiredHandPalm, std::string_view sideName,
                                      float& error, bool& reachable, std::string& orders,
                                      float& handAimError, float& handPalmError,
                                      std::string& handOrientationCandidate) {
                unsigned int solvedForArm = 0;
                WorldViewerWeaponIkSolve solution;
                for (unsigned int i = 0; i < 6; ++i)
                {
                    solution = solveFalloutWeaponIkTwoBone(getNodeWorldMatrix(&upper).getTrans(),
                        getNodeWorldMatrix(&forearm).getTrans(), getNodeWorldMatrix(&hand).getTrans(), target, poleHint);
                    error = solution.mError;
                    reachable = solution.mReachable;
                    if (!solution.mSolved)
                        break;

                    const WorldViewerWeaponIkRotationProbe upperProbe
                        = rotateFalloutWeaponIkSegmentToBest(upper, forearm, solution.mMid, strength);
                    if (upperProbe.mSolved)
                    {
                        ++solvedForArm;
                        orders += orders.empty() ? std::string("upper=") : std::string(",upper=");
                        orders += upperProbe.mOrder;
                    }
                    const WorldViewerWeaponIkRotationProbe forearmProbe
                        = rotateFalloutWeaponIkSegmentToBest(forearm, hand, solution.mEnd, strength);
                    if (forearmProbe.mSolved)
                    {
                        ++solvedForArm;
                        orders += orders.empty() ? std::string("forearm=") : std::string(",forearm=");
                        orders += forearmProbe.mOrder;
                    }

                    error = (getNodeWorldMatrix(&hand).getTrans() - target).length();
                    if (error <= 2.f)
                        break;
                }

                if (solution.mSolved && clavicle != nullptr
                    && std::getenv("OPENMW_FNV_WEAPON_IK_DISABLE_CLAVICLE") == nullptr)
                {
                    const float clavicleStrength
                        = std::clamp(readFalloutProofFloat("OPENMW_FNV_WEAPON_IK_CLAVICLE_STRENGTH", 0.15f), 0.f, 1.f);
                    const osg::Vec3f shoulderAfter = getNodeWorldMatrix(&upper).getTrans();
                    const osg::Vec3f clavicleHint = getNodeWorldMatrix(clavicle).getTrans()
                        + normalizeFalloutIkVector(solution.mMid - shoulderAfter, poleHint) * 8.f;
                    const WorldViewerWeaponIkRotationProbe clavicleProbe
                        = rotateFalloutWeaponIkSegmentToBest(*clavicle, upper, clavicleHint, strength * clavicleStrength);
                    if (clavicleProbe.mSolved)
                    {
                        ++solvedForArm;
                        orders += orders.empty() ? std::string("clavicle=") : std::string(",clavicle=");
                        orders += clavicleProbe.mOrder;
                    }
                }

                const bool explicitHandOrientation
                    = std::getenv("OPENMW_FNV_ENABLE_WEAPON_IK_HAND_ORIENTATION") != nullptr
                    || std::getenv("OPENMW_FNV_WEAPON_IK_HAND_ORIENTATION") != nullptr
                    || (longGun ? std::getenv("OPENMW_FNV_WEAPON_IK_LONG_GUN_HAND_ORIENTATION") != nullptr
                                : std::getenv("OPENMW_FNV_WEAPON_IK_SIDEARM_HAND_ORIENTATION") != nullptr)
                    || (sideName == "right"
                            ? std::getenv("OPENMW_FNV_WEAPON_IK_RIGHT_HAND_ORIENTATION") != nullptr
                            : std::getenv("OPENMW_FNV_WEAPON_IK_LEFT_HAND_ORIENTATION") != nullptr)
                    || (longGun
                            ? (sideName == "right"
                                      ? std::getenv("OPENMW_FNV_WEAPON_IK_LONG_GUN_RIGHT_HAND_ORIENTATION") != nullptr
                                      : std::getenv("OPENMW_FNV_WEAPON_IK_LONG_GUN_LEFT_HAND_ORIENTATION") != nullptr)
                            : (sideName == "right"
                                      ? std::getenv("OPENMW_FNV_WEAPON_IK_SIDEARM_RIGHT_HAND_ORIENTATION") != nullptr
                                      : std::getenv("OPENMW_FNV_WEAPON_IK_SIDEARM_LEFT_HAND_ORIENTATION") != nullptr));
                if (explicitHandOrientation)
                {
                    const WorldViewerHandOrientationProbe handProbe = orientFalloutWeaponIkHandGripToBest(
                        hand, desiredHandForward, desiredHandPalm, strength, sideName, longGun);
                    if (handProbe.mSolved)
                    {
                        handAimError = handProbe.mForwardError;
                        handPalmError = handProbe.mPalmError;
                        handOrientationCandidate = handProbe.mCandidate;
                        orders += orders.empty() ? std::string("hand=") : std::string(",hand=");
                        orders += handProbe.mCandidate;
                        ++solvedForArm;
                    }
                }
                else
                {
                    handOrientationCandidate = std::string(sideName) + ":preserve-bind-roll";
                    handAimError = falloutIkDirectionAngleDegrees(
                        getNodeWorldMatrix(&hand).getRotate() * osg::Vec3f(0.f, 1.f, 0.f),
                        desiredHandForward);
                    handPalmError = 0.f;
                    orders += orders.empty() ? std::string("hand=") : std::string(",hand=");
                    orders += handOrientationCandidate;
                }
                return solvedForArm;
            };

            solved += solveArm(rightClavicle, *rightUpper, *rightForearm, *rightHand, rightTarget,
                right * 0.75f - up * 0.85f + forward * 0.15f, forward + up * 0.08f, -right, "right",
                rightError, rightReachable, rightOrders, rightHandAimError, rightHandPalmError,
                rightHandOrientationCandidate);
            solved += solveArm(leftClavicle, *leftUpper, *leftForearm, *leftHand, leftTarget,
                -right * 0.75f - up * 0.85f + forward * 0.15f, forward + up * 0.08f, right, "left",
                leftError, leftReachable, leftOrders, leftHandAimError, leftHandPalmError,
                leftHandOrientationCandidate);

            float weaponAimAngleBefore = falloutIkDirectionAngleDegrees(weaponForwardBefore, forward);
            WorldViewerWeaponIkRotationProbe weaponAimProbe
                = rotateFalloutWeaponIkAxisToBest(*weaponFrame, weaponAimAxis, forward, strength);
            const osg::Vec3f weaponForwardAfter
                = normalizeFalloutIkVector(getNodeWorldMatrix(weaponFrame).getRotate() * weaponAimAxis, forward);
            float weaponAimAngleAfter = falloutIkDirectionAngleDegrees(weaponForwardAfter, forward);

            const bool snapHandsToIkTargets = std::getenv("OPENMW_FNV_WEAPON_IK_SNAP_HANDS_TO_TARGETS") != nullptr;
            if (snapHandsToIkTargets)
            {
                osg::Matrix rightWorld = getNodeWorldMatrix(rightHand);
                rightWorld.setTrans(rightTarget);
                setFalloutIkTransformWorldMatrix(*rightHand, rightWorld);
                osg::Matrix leftWorld = getNodeWorldMatrix(leftHand);
                leftWorld.setTrans(leftTarget);
                setFalloutIkTransformWorldMatrix(*leftHand, leftWorld);
            }

            if (skeleton != nullptr)
            {
                skeleton->markBoneMatriceDirty();
                skeleton->updateBoneMatrices(0);
            }

            unsigned int forcedRigGeometryHolder = 0;
            unsigned int refreshedRigGeometry = 0;
            const unsigned int forcedRigGeometry
                = forceFalloutRigGeometryUpdate(objectRoot, forcedRigGeometryHolder, refreshedRigGeometry);

            const osg::Vec3f rightHandAfter = getNodeWorldMatrix(rightHand).getTrans();
            const osg::Vec3f leftHandAfter = getNodeWorldMatrix(leftHand).getTrans();
            const osg::Vec3f weaponAfter = getNodeWorldMatrix(weaponFrame).getTrans();
            const osg::Vec3f rightHandAxisYAfter = normalizeFalloutIkVector(
                getNodeWorldMatrix(rightHand).getRotate() * osg::Vec3f(0.f, 1.f, 0.f), osg::Vec3f(0.f, 1.f, 0.f));
            const osg::Vec3f leftHandAxisYAfter = normalizeFalloutIkVector(
                getNodeWorldMatrix(leftHand).getRotate() * osg::Vec3f(0.f, 1.f, 0.f), osg::Vec3f(0.f, 1.f, 0.f));
            const float rightHandForwardAngleAfter = falloutIkDirectionAngleDegrees(rightHandAxisYAfter, forward);
            const float leftHandForwardAngleAfter = falloutIkDirectionAngleDegrees(leftHandAxisYAfter, forward);
            const float rightTargetBefore = (rightHandBefore - rightTarget).length();
            const float rightTargetAfter = (rightHandAfter - rightTarget).length();
            const float leftTargetBefore = (leftHandBefore - leftTarget).length();
            const float leftTargetAfter = (leftHandAfter - leftTarget).length();
            const float rightWeaponGripDistanceAfter = (rightHandAfter - weaponAfter).length();
            const float leftWeaponGripDistanceAfter = (leftHandAfter - weaponAfter).length();
            const float weaponGripSpanAfter = (rightHandAfter - leftHandAfter).length();
            const float rightHandSideAfter = (rightHandAfter - chest) * right;
            const float leftHandSideAfter = (leftHandAfter - chest) * right;
            const bool handsUncrossed = rightHandSideAfter > leftHandSideAfter + 2.f;
            const bool gripOk = rightWeaponGripDistanceAfter <= 2.5f && leftWeaponGripDistanceAfter >= 8.f
                && leftWeaponGripDistanceAfter <= 32.f && weaponGripSpanAfter >= 10.f && weaponGripSpanAfter <= 32.f;
            const bool targetImproved = rightTargetAfter + 0.5f < rightTargetBefore
                || leftTargetAfter + 0.5f < leftTargetBefore;
            const bool aimOk = weaponAimAngleAfter >= 0.f
                && (weaponAimAngleAfter <= 12.f || weaponAimAngleAfter + 0.5f < weaponAimAngleBefore);
            const bool runtimeSupported = solved >= 4 && rightError >= 0.f && leftError >= 0.f
                && rightError <= 16.f && leftError <= 16.f && handsUncrossed && gripOk && targetImproved && aimOk;

            if (frameLogs < 12)
            {
                ++frameLogs;
                Log(runtimeSupported ? Debug::Info : Debug::Warning)
                    << "FNV/ESM4 telemetry: weapon IK frame actor=" << traits.mEditorId
                    << " ref=" << ptr.getCellRef().getRefId()
                    << " sample=" << frameLogs
                    << " weapon=\"" << weapon->mEditorId << "\""
                    << " weaponModel=\"" << weapon->mModel << "\""
                    << " solver=fabrik-two-bone-pole"
                    << " targetStyle=" << targetStyle
                    << " actorForward=(" << forward.x() << "," << forward.y() << "," << forward.z() << ")"
                    << " actorRight=(" << right.x() << "," << right.y() << "," << right.z() << ")"
                    << " chest=(" << chest.x() << "," << chest.y() << "," << chest.z() << ")"
                    << " rightTarget=(" << rightTarget.x() << "," << rightTarget.y() << "," << rightTarget.z() << ")"
                    << " leftTarget=(" << leftTarget.x() << "," << leftTarget.y() << "," << leftTarget.z() << ")"
                    << " weaponAimAxisName=" << falloutWeaponIkAxisName(weaponAimAxis)
                    << " weaponAimAngleBefore=" << weaponAimAngleBefore
                    << " weaponAimAngleAfter=" << weaponAimAngleAfter
                    << " weaponAimSolved=" << weaponAimProbe.mSolved
                    << " weaponAimOrder=" << weaponAimProbe.mOrder
                    << " snapHandsToIkTargets=" << snapHandsToIkTargets
                    << " forcedRigReskin=(" << forcedRigGeometry << "," << forcedRigGeometryHolder << ","
                    << refreshedRigGeometry << ")"
                    << " targetDistancesBefore=(" << rightTargetBefore << "," << leftTargetBefore << ")"
                    << " targetDistancesAfter=(" << rightTargetAfter << "," << leftTargetAfter << ")"
                    << " weaponGripDistancesAfter=(" << rightWeaponGripDistanceAfter << ","
                    << leftWeaponGripDistanceAfter << ")"
                    << " weaponGripSpanAfter=" << weaponGripSpanAfter
                    << " fabrikErrors=(" << rightError << "," << leftError << ")"
                    << " reachable=(" << rightReachable << "," << leftReachable << ")"
                    << " handsSide=(" << rightHandSideAfter << "," << leftHandSideAfter << ")"
                    << " handsUncrossed=" << handsUncrossed
                    << " rightHandAxisY=(" << rightHandAxisYAfter.x() << "," << rightHandAxisYAfter.y()
                    << "," << rightHandAxisYAfter.z() << ")"
                    << " leftHandAxisY=(" << leftHandAxisYAfter.x() << "," << leftHandAxisYAfter.y()
                    << "," << leftHandAxisYAfter.z() << ")"
                    << " handForwardAngles=(" << rightHandForwardAngleAfter << ","
                    << leftHandForwardAngleAfter << ")"
                    << " handOrientationCandidates=(" << rightHandOrientationCandidate << ","
                    << leftHandOrientationCandidate << ")"
                    << " handOrientationErrors=(" << rightHandAimError << "," << rightHandPalmError
                    << "," << leftHandAimError << "," << leftHandPalmError << ")"
                    << " rotationOrders=(" << rightOrders << ";" << leftOrders << ")"
                    << " runtime=" << (runtimeSupported ? "runtime-supported" : "loaded-pending-runtime")
                    << " gate=runtime-fnv-weapon-ik-telemetry";
            }

            if (!sProofLogged[actorKey])
            {
                sProofLogged[actorKey] = true;
                Log(runtimeSupported ? Debug::Info : Debug::Warning)
                    << "FNV/ESM4 proof: weapon IK solver active actor=" << traits.mEditorId
                    << " ref=" << ptr.getCellRef().getRefId()
                    << " solver=fabrik-two-bone-pole"
                    << " reference=FABRIK"
                    << " solvedBones=" << solved
                    << " rightFabrikError=" << rightError
                    << " leftFabrikError=" << leftError
                    << " rightFabrikReachable=" << rightReachable
                    << " leftFabrikReachable=" << leftReachable
                    << " strength=" << strength
                    << " targetStyle=" << targetStyle
                    << " rightTargetDistanceBefore=" << rightTargetBefore
                    << " rightTargetDistanceAfter=" << rightTargetAfter
                    << " leftTargetDistanceBefore=" << leftTargetBefore
                    << " leftTargetDistanceAfter=" << leftTargetAfter
                    << " rightWeaponGripDistanceAfter=" << rightWeaponGripDistanceAfter
                    << " leftWeaponGripDistanceAfter=" << leftWeaponGripDistanceAfter
                    << " weaponGripSpanAfter=" << weaponGripSpanAfter
                    << " handsUncrossed=" << handsUncrossed
                    << " weaponAimAxis=(" << weaponAimAxis.x() << "," << weaponAimAxis.y() << ","
                    << weaponAimAxis.z() << ")"
                    << " weaponAimAngleBefore=" << weaponAimAngleBefore
                    << " weaponAimAngleAfter=" << weaponAimAngleAfter
                    << " weaponAimSolved=" << weaponAimProbe.mSolved
                    << " weaponAimOrder=" << weaponAimProbe.mOrder
                    << " rightHandAxisY=(" << rightHandAxisYAfter.x() << "," << rightHandAxisYAfter.y()
                    << "," << rightHandAxisYAfter.z() << ")"
                    << " leftHandAxisY=(" << leftHandAxisYAfter.x() << "," << leftHandAxisYAfter.y()
                    << "," << leftHandAxisYAfter.z() << ")"
                    << " handForwardAngles=(" << rightHandForwardAngleAfter << ","
                    << leftHandForwardAngleAfter << ")"
                    << " handOrientationCandidates=(" << rightHandOrientationCandidate << ","
                    << leftHandOrientationCandidate << ")"
                    << " handOrientationErrors=(" << rightHandAimError << "," << rightHandPalmError
                    << "," << leftHandAimError << "," << leftHandPalmError << ")"
                    << " forcedRigReskin=(" << forcedRigGeometry << "," << forcedRigGeometryHolder << ","
                    << refreshedRigGeometry << ")"
                    << " runtime=" << (runtimeSupported ? "runtime-supported" : "loaded-pending-runtime")
                    << " gate=runtime-fnv-weapon-ik";
            }

            return runtimeSupported;
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
            return stream.str();
        }

        unsigned int getFalloutGeometryVertexCount(const osg::Geometry* geometry)
        {
            if (geometry == nullptr || geometry->getVertexArray() == nullptr)
                return 0;

            return geometry->getVertexArray()->getNumElements();
        }

        void expandBoundingBoxByTransformedCorners(
            osg::BoundingBox& target, const osg::BoundingBox& source, const osg::Matrix& matrix)
        {
            if (!source.valid())
                return;

            const float xs[] = { source.xMin(), source.xMax() };
            const float ys[] = { source.yMin(), source.yMax() };
            const float zs[] = { source.zMin(), source.zMax() };
            for (const float x : xs)
                for (const float y : ys)
                    for (const float z : zs)
                        target.expandBy(transformPoint(osg::Vec3f(x, y, z), matrix));
        }

        std::string falloutAuditVec3(const osg::Vec3f& value)
        {
            std::ostringstream stream;
            stream << "(" << value.x() << "," << value.y() << "," << value.z() << ")";
            return stream.str();
        }

        osg::BoundingBox computeFalloutGeometryBounds(const osg::Geometry* geometry)
        {
            osg::BoundingBox box;
            if (geometry == nullptr)
                return box;

            box = geometry->getBoundingBox();
            if (box.valid())
                return box;

            const osg::Vec3Array* vertices = dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray());
            if (vertices == nullptr)
                return box;
            for (const osg::Vec3f& vertex : *vertices)
                box.expandBy(vertex);
            return box;
        }

        class FalloutWeaponMeshAuditVisitor : public osg::NodeVisitor
        {
        public:
            FalloutWeaponMeshAuditVisitor(bool skipRootTransform = false)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mSkipRootTransform(skipRootTransform)
            {
            }

            void apply(osg::Geode& geode) override
            {
                const osg::Matrix localToAttached = computeLocalToAttached();
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (osg::Drawable* drawable = geode.getDrawable(i))
                        auditDrawable(*drawable, localToAttached);
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override
            {
                auditDrawable(drawable, computeLocalToAttached());
            }

            unsigned int mDrawableCount = 0;
            unsigned int mGeometryCount = 0;
            unsigned int mRigGeometryCount = 0;
            unsigned int mVertexCount = 0;
            osg::BoundingBox mAttachedLocalBox;

        private:
            osg::Matrix computeLocalToAttached() const
            {
                osg::NodePath path = getNodePath();
                if (mSkipRootTransform && !path.empty())
                    path.erase(path.begin());
                return osg::computeLocalToWorld(path);
            }

            void auditDrawable(osg::Drawable& drawable, const osg::Matrix& localToAttached)
            {
                ++mDrawableCount;

                osg::Geometry* geometry = dynamic_cast<osg::Geometry*>(&drawable);
                SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable);
                osg::Geometry* sourceGeometry = rig == nullptr ? nullptr : rig->getSourceGeometry();
                osg::Geometry* renderGeometry = geometry;
                if (rig != nullptr)
                {
                    ++mRigGeometryCount;
                    renderGeometry = rig->getLastFrameGeometry();
                    if (renderGeometry == nullptr)
                        renderGeometry = rig->getRenderGeometry(0);
                    if (renderGeometry == nullptr)
                        renderGeometry = sourceGeometry;
                }

                if (renderGeometry == nullptr)
                    renderGeometry = sourceGeometry;
                if (renderGeometry == nullptr)
                    return;

                ++mGeometryCount;
                mVertexCount += getFalloutGeometryVertexCount(renderGeometry);
                expandBoundingBoxByTransformedCorners(
                    mAttachedLocalBox, computeFalloutGeometryBounds(renderGeometry), localToAttached);
            }

            bool mSkipRootTransform = false;
        };

        osg::Vec3f chooseFalloutAuditMajorAxis(const osg::Vec3f& extent)
        {
            if (extent.y() >= extent.x() && extent.y() >= extent.z())
                return osg::Vec3f(0.f, 1.f, 0.f);
            if (extent.z() >= extent.x() && extent.z() >= extent.y())
                return osg::Vec3f(0.f, 0.f, 1.f);
            return osg::Vec3f(1.f, 0.f, 0.f);
        }

        bool looksLikeFalloutWeaponMeshModel(std::string_view model)
        {
            std::string lowered(model);
            Misc::StringUtils::lowerCaseInPlace(lowered);
            return containsAny(lowered, { "weapon", "weapons/", "weapons\\" });
        }

        void logFalloutWeaponMeshAudit(osg::Node* attached, osg::Group* attachNode,
            const Animation::NodeMap& nodeMap, std::string_view model, std::string_view preferredBone,
            const MWWorld::Ptr& ptr, std::string_view phase = "insert")
        {
            if (attached == nullptr || !worldViewerActorTelemetryEnabled())
                return;
            if (!Misc::StringUtils::ciEqual(preferredBone, "Weapon") && !looksLikeFalloutWeaponMeshModel(model))
                return;

            static std::map<std::string, unsigned int> sAuditRowsByActorModel;
            std::ostringstream keyStream;
            keyStream << ptr.getCellRef().getRefId() << "|" << phase << "|" << model;
            unsigned int& auditRows = sAuditRowsByActorModel[keyStream.str()];
            if (auditRows >= 4)
                return;
            ++auditRows;

            FalloutWeaponMeshAuditVisitor visitor(attached == attachNode);
            attached->accept(visitor);
            const osg::BoundingBox& localBox = visitor.mAttachedLocalBox;
            const bool valid = localBox.valid();
            const osg::Vec3f localCenter = valid ? localBox.center() : osg::Vec3f();
            const osg::Vec3f localExtent = boundingBoxExtent(localBox);
            const osg::Vec3f localMajorAxis = chooseFalloutAuditMajorAxis(localExtent);

            const ESM::Position& position = ptr.getRefData().getPosition();
            const float yaw = position.rot[2];
            osg::Vec3f actorForward(std::sin(yaw), std::cos(yaw), 0.f);
            actorForward = normalizeFalloutIkVector(actorForward, osg::Vec3f(0.f, 1.f, 0.f));
            osg::Vec3f actorRight(actorForward.y(), -actorForward.x(), 0.f);
            actorRight = normalizeFalloutIkVector(actorRight, osg::Vec3f(1.f, 0.f, 0.f));
            const osg::Vec3f actorUp(0.f, 0.f, 1.f);

            const osg::Matrix attachedWorld = getNodeWorldMatrix(attached);
            const osg::Matrix attachWorld = getNodeWorldMatrix(attachNode);
            const osg::Vec3f attachedOrigin = attachedWorld.getTrans();
            const osg::Vec3f attachOrigin = attachWorld.getTrans();
            const osg::Vec3f worldCenter = valid ? transformPoint(localCenter, attachWorld) : attachedOrigin;
            const osg::Vec3f centerFromAttach = worldCenter - attachOrigin;
            const osg::Vec3f originFromAttach = attachedOrigin - attachOrigin;
            const osg::Vec3f majorWorld = normalizeFalloutIkVector(
                attachWorld.getRotate() * localMajorAxis, actorForward);

            osg::Group* rightHand = findBestAttachmentNode(
                nodeMap, { "Bip01 R Hand", "bip01 r hand", "R Hand", "r hand" });
            osg::Group* leftHand = findBestAttachmentNode(
                nodeMap, { "Bip01 L Hand", "bip01 l hand", "L Hand", "l hand" });
            osg::Group* weaponFrame = findBestAttachmentNode(
                nodeMap, { "Weapon", "weapon", "Bip01 Weapon", "Bip01 R Hand", "bip01 r hand" });
            const osg::Vec3f rightHandOrigin
                = rightHand != nullptr ? osg::Vec3f(getNodeWorldMatrix(rightHand).getTrans()) : osg::Vec3f();
            const osg::Vec3f leftHandOrigin
                = leftHand != nullptr ? osg::Vec3f(getNodeWorldMatrix(leftHand).getTrans()) : osg::Vec3f();
            const osg::Vec3f weaponFrameOrigin = weaponFrame != nullptr
                ? osg::Vec3f(getNodeWorldMatrix(weaponFrame).getTrans())
                : osg::Vec3f();

            const float rightHandToCenter = rightHand != nullptr ? (rightHandOrigin - worldCenter).length() : -1.f;
            const float leftHandToCenter = leftHand != nullptr ? (leftHandOrigin - worldCenter).length() : -1.f;
            const float weaponFrameToCenter
                = weaponFrame != nullptr ? (weaponFrameOrigin - worldCenter).length() : -1.f;
            const float centerForward = centerFromAttach * actorForward;
            const float centerRight = centerFromAttach * actorRight;
            const float centerUp = centerFromAttach * actorUp;
            const float majorDotForward = majorWorld * actorForward;
            const float majorDotRight = majorWorld * actorRight;
            const float majorDotUp = majorWorld * actorUp;

            Log(Debug::Info) << "World viewer actor weapon mesh ledger:"
                             << " ref=" << ptr.getCellRef().getRefId()
                             << " phase=" << phase
                             << " model=\"" << model << "\""
                             << " preferredBone=\"" << preferredBone << "\""
                             << " attachNode=\"" << (attachNode != nullptr ? attachNode->getName() : std::string())
                             << "\""
                             << " attachedNode=\"" << attached->getName() << "\""
                             << " valid=" << valid
                             << " drawables=" << visitor.mDrawableCount
                             << " geometries=" << visitor.mGeometryCount
                             << " rigGeometries=" << visitor.mRigGeometryCount
                             << " vertices=" << visitor.mVertexCount
                             << " localCenter=" << falloutAuditVec3(localCenter)
                             << " localExtent=" << falloutAuditVec3(localExtent)
                             << " localMajorAxis=" << falloutWeaponIkAxisName(localMajorAxis)
                             << " worldCenter=" << falloutAuditVec3(worldCenter)
                             << " attachOrigin=" << falloutAuditVec3(attachOrigin)
                             << " attachedOrigin=" << falloutAuditVec3(attachedOrigin)
                             << " centerFromAttach=" << falloutAuditVec3(centerFromAttach)
                             << " originFromAttach=" << falloutAuditVec3(originFromAttach)
                             << " centerActorAxes=(" << centerForward << "," << centerRight << "," << centerUp
                             << ")"
                             << " majorWorld=" << falloutAuditVec3(majorWorld)
                             << " majorActorDots=(" << majorDotForward << "," << majorDotRight << "," << majorDotUp
                             << ")"
                             << " rightHand=" << falloutAuditVec3(rightHandOrigin)
                             << " leftHand=" << falloutAuditVec3(leftHandOrigin)
                             << " weaponFrame=" << falloutAuditVec3(weaponFrameOrigin)
                             << " handToVisibleCenter=(" << rightHandToCenter << "," << leftHandToCenter << ")"
                             << " weaponFrameToVisibleCenter=" << weaponFrameToCenter
                             << " gate=actor-weapon-mesh-telemetry";
        }

        bool stabilizeFalloutLongGunWeaponFrame(
            const Animation::NodeMap& nodeMap, const MWWorld::Ptr& ptr, const ESM4::Npc& traits,
            const ESM4::Weapon& weapon)
        {
            if (!worldViewerFONVWeaponFrameStabilizerEnabled()
                || (!traits.mIsFO3 && !traits.mIsFONV) || !isFalloutWeaponIkLongGun(weapon))
                return false;

            osg::MatrixTransform* weaponFrame = dynamic_cast<osg::MatrixTransform*>(
                findBestAttachmentNode(nodeMap, { "Weapon", "weapon", "Bip01 Weapon" }));
            if (weaponFrame == nullptr)
                return false;

            int syntheticHelper = 0;
            weaponFrame->getUserValue("esm4SyntheticAttachmentHelper", syntheticHelper);
            if (syntheticHelper == 0)
                return false;

            const ESM::Position& position = ptr.getRefData().getPosition();
            osg::Vec3f actorForward(std::sin(position.rot[2]), std::cos(position.rot[2]), 0.f);
            actorForward = normalizeFalloutIkVector(actorForward, osg::Vec3f(0.f, 1.f, 0.f));
            const osg::Vec3f desiredForward
                = normalizeFalloutIkVector(actorForward - osg::Vec3f(0.f, 0.f, 0.18f), actorForward);
            const osg::Vec3f localWeaponLongAxis(1.f, 0.f, 0.f);

            const float before = falloutIkDirectionAngleDegrees(
                getNodeWorldMatrix(weaponFrame).getRotate() * localWeaponLongAxis, desiredForward);
            const WorldViewerWeaponIkRotationProbe probe
                = rotateFalloutWeaponIkAxisToBest(*weaponFrame, localWeaponLongAxis, desiredForward, 1.f);
            const float after = falloutIkDirectionAngleDegrees(
                getNodeWorldMatrix(weaponFrame).getRotate() * localWeaponLongAxis, desiredForward);

            static std::map<std::string, unsigned int> sFrameStabilizerLogs;
            std::ostringstream keyStream;
            keyStream << ptr.getCellRef().getRefId() << "|" << weapon.mEditorId;
            unsigned int& logs = sFrameStabilizerLogs[keyStream.str()];
            if (logs < 8)
            {
                ++logs;
                Log(probe.mSolved ? Debug::Info : Debug::Warning)
                    << "FNV/ESM4 proof: stabilized long-gun weapon frame actor=" << traits.mEditorId
                    << " ref=" << ptr.getCellRef().getRefId()
                    << " weapon=\"" << weapon.mEditorId << "\""
                    << " weaponModel=\"" << weapon.mModel << "\""
                    << " localLongAxis=" << falloutWeaponIkAxisName(localWeaponLongAxis)
                    << " desiredForward=(" << desiredForward.x() << "," << desiredForward.y() << ","
                    << desiredForward.z() << ")"
                    << " angleBefore=" << before
                    << " angleAfter=" << after
                    << " solved=" << probe.mSolved
                    << " order=" << probe.mOrder
                    << " runtime=" << (probe.mSolved && after >= 0.f && after <= 12.f ? "runtime-supported"
                                                                                       : "loaded-pending-runtime")
                    << " gate=runtime-fnv-weapon-frame-stabilizer";
            }

            return probe.mSolved && after >= 0.f && after <= 12.f;
        }

        bool stabilizeFalloutSidearmWeaponFrame(
            const Animation::NodeMap& nodeMap, const MWWorld::Ptr& ptr, const ESM4::Npc& traits,
            const ESM4::Weapon& weapon)
        {
            if (!worldViewerFONVWeaponFrameStabilizerEnabled()
                || (!traits.mIsFO3 && !traits.mIsFONV) || isFalloutWeaponIkLongGun(weapon))
                return false;

            osg::MatrixTransform* weaponFrame = dynamic_cast<osg::MatrixTransform*>(
                findBestAttachmentNode(nodeMap, { "Weapon", "weapon", "Bip01 Weapon" }));
            if (weaponFrame == nullptr)
                return false;

            int syntheticHelper = 0;
            weaponFrame->getUserValue("esm4SyntheticAttachmentHelper", syntheticHelper);
            if (syntheticHelper == 0)
                return false;

            const ESM::Position& position = ptr.getRefData().getPosition();
            osg::Vec3f actorForward(std::sin(position.rot[2]), std::cos(position.rot[2]), 0.f);
            actorForward = normalizeFalloutIkVector(actorForward, osg::Vec3f(0.f, 1.f, 0.f));
            const osg::Vec3f desiredForward
                = normalizeFalloutIkVector(actorForward - osg::Vec3f(0.f, 0.f, 0.28f), actorForward);
            const osg::Vec3f localWeaponLongAxis(1.f, 0.f, 0.f);

            const float before = falloutIkDirectionAngleDegrees(
                getNodeWorldMatrix(weaponFrame).getRotate() * localWeaponLongAxis, desiredForward);
            const WorldViewerWeaponIkRotationProbe probe
                = rotateFalloutWeaponIkAxisToBest(*weaponFrame, localWeaponLongAxis, desiredForward, 1.f);
            const float after = falloutIkDirectionAngleDegrees(
                getNodeWorldMatrix(weaponFrame).getRotate() * localWeaponLongAxis, desiredForward);

            static std::map<std::string, unsigned int> sFrameStabilizerLogs;
            std::ostringstream keyStream;
            keyStream << ptr.getCellRef().getRefId() << "|" << weapon.mEditorId;
            unsigned int& logs = sFrameStabilizerLogs[keyStream.str()];
            if (logs < 8)
            {
                ++logs;
                Log(probe.mSolved ? Debug::Info : Debug::Warning)
                    << "FNV/ESM4 proof: stabilized sidearm weapon frame actor=" << traits.mEditorId
                    << " ref=" << ptr.getCellRef().getRefId()
                    << " weapon=\"" << weapon.mEditorId << "\""
                    << " weaponModel=\"" << weapon.mModel << "\""
                    << " localLongAxis=" << falloutWeaponIkAxisName(localWeaponLongAxis)
                    << " desiredForward=(" << desiredForward.x() << "," << desiredForward.y() << ","
                    << desiredForward.z() << ")"
                    << " angleBefore=" << before
                    << " angleAfter=" << after
                    << " solved=" << probe.mSolved
                    << " order=" << probe.mOrder
                    << " runtime=" << (probe.mSolved && after >= 0.f && after <= 12.f ? "runtime-supported"
                                                                                       : "loaded-pending-runtime")
                    << " gate=runtime-fnv-sidearm-frame-stabilizer";
            }

            return probe.mSolved && after >= 0.f && after <= 12.f;
        }

        bool applyFalloutIdleArmRelaxIk(const Animation::NodeMap& nodeMap, SceneUtil::Skeleton* skeleton,
            const MWWorld::Ptr& ptr, const ESM4::Npc& traits)
        {
            if (!traits.mIsFO3 && !traits.mIsFONV)
                return false;

            const auto findMatrix = [&](std::initializer_list<std::string_view> names) {
                return dynamic_cast<osg::MatrixTransform*>(findBestAttachmentNode(nodeMap, names));
            };

            osg::MatrixTransform* pelvis = findMatrix({ "Bip01 Pelvis", "bip01 pelvis" });
            osg::MatrixTransform* leftUpper = findMatrix({ "Bip01 L UpperArm", "bip01 l upperarm" });
            osg::MatrixTransform* leftForearm = findMatrix({ "Bip01 L Forearm", "bip01 l forearm" });
            osg::MatrixTransform* leftHand = findMatrix({ "Bip01 L Hand", "bip01 l hand" });
            osg::MatrixTransform* rightUpper = findMatrix({ "Bip01 R UpperArm", "bip01 r upperarm" });
            osg::MatrixTransform* rightForearm = findMatrix({ "Bip01 R Forearm", "bip01 r forearm" });
            osg::MatrixTransform* rightHand = findMatrix({ "Bip01 R Hand", "bip01 r hand" });
            if (pelvis == nullptr || leftUpper == nullptr || leftForearm == nullptr || leftHand == nullptr
                || rightUpper == nullptr || rightForearm == nullptr || rightHand == nullptr)
                return false;

            const osg::Matrix originalLeftUpperWorld = getNodeWorldMatrix(leftUpper);
            const osg::Matrix originalLeftForearmWorld = getNodeWorldMatrix(leftForearm);
            const osg::Matrix originalLeftHandWorld = getNodeWorldMatrix(leftHand);
            const osg::Matrix originalRightUpperWorld = getNodeWorldMatrix(rightUpper);
            const osg::Matrix originalRightForearmWorld = getNodeWorldMatrix(rightForearm);
            const osg::Matrix originalRightHandWorld = getNodeWorldMatrix(rightHand);
            const auto refreshSkeleton = [&]() {
                if (skeleton != nullptr)
                {
                    skeleton->markBoneMatriceDirty();
                    skeleton->updateBoneMatrices(0);
                }
            };
            const auto restoreOriginalArmPose = [&]() {
                setFalloutIkTransformWorldMatrix(*leftUpper, originalLeftUpperWorld);
                setFalloutIkTransformWorldMatrix(*leftForearm, originalLeftForearmWorld);
                setFalloutIkTransformWorldMatrix(*leftHand, originalLeftHandWorld);
                setFalloutIkTransformWorldMatrix(*rightUpper, originalRightUpperWorld);
                setFalloutIkTransformWorldMatrix(*rightForearm, originalRightForearmWorld);
                setFalloutIkTransformWorldMatrix(*rightHand, originalRightHandWorld);
                refreshSkeleton();
            };

            const osg::Vec3f pelvisOrigin = getNodeWorldMatrix(pelvis).getTrans();
            const osg::Vec3f leftShoulder = getNodeWorldMatrix(leftUpper).getTrans();
            const osg::Vec3f rightShoulder = getNodeWorldMatrix(rightUpper).getTrans();
            const osg::Vec3f leftElbow = getNodeWorldMatrix(leftForearm).getTrans();
            const osg::Vec3f rightElbow = getNodeWorldMatrix(rightForearm).getTrans();
            const osg::Vec3f leftHandOrigin = getNodeWorldMatrix(leftHand).getTrans();
            const osg::Vec3f rightHandOrigin = getNodeWorldMatrix(rightHand).getTrans();
            const osg::Vec3f shoulderMid = (leftShoulder + rightShoulder) * 0.5f;
            const osg::Vec3f handMid = (leftHandOrigin + rightHandOrigin) * 0.5f;
            const float shoulderSpan = (leftShoulder - rightShoulder).length();
            const float handSpan = (leftHandOrigin - rightHandOrigin).length();
            const float elbowSpan = (leftElbow - rightElbow).length();
            const float handSpreadRatio = handSpan / std::max(1.f, shoulderSpan);
            const float handMidDrop = shoulderMid.z() - handMid.z();
            const float handMidPelvisZ = handMid.z() - pelvisOrigin.z();
            const bool wideHighArmPose = handSpan > std::max(58.f, shoulderSpan * 2.1f)
                && elbowSpan > std::max(42.f, shoulderSpan * 1.55f) && handMidPelvisZ > 35.f && handMidDrop < 8.f;
            if (!wideHighArmPose)
                return false;

            const ESM::Position& position = ptr.getRefData().getPosition();
            osg::Vec3f actorForward(std::sin(position.rot[2]), std::cos(position.rot[2]), 0.f);
            actorForward = normalizeFalloutIkVector(actorForward, osg::Vec3f(0.f, 1.f, 0.f));
            const osg::Vec3f up(0.f, 0.f, 1.f);
            osg::Vec3f bodyRight = rightShoulder - leftShoulder;
            bodyRight.z() = 0.f;
            bodyRight = normalizeFalloutIkVector(bodyRight, osg::Vec3f(actorForward.y(), -actorForward.x(), 0.f));
            osg::Vec3f bodyForward = up ^ bodyRight;
            bodyForward = normalizeFalloutIkVector(bodyForward, actorForward);

            const float leftReach = (leftElbow - leftShoulder).length() + (leftHandOrigin - leftElbow).length();
            const float rightReach = (rightElbow - rightShoulder).length() + (rightHandOrigin - rightElbow).length();
            const float minReach = std::min(leftReach, rightReach);
            const float maxDrop = std::max(20.f, std::min(34.f, minReach - 2.f));
            const float armDrop = std::clamp(shoulderMid.z() - pelvisOrigin.z() - 4.f, 20.f, maxDrop);
            const float sideInset = std::clamp(shoulderSpan * 0.10f, 2.5f, 4.5f);
            const float forwardOffset = std::clamp(shoulderSpan * 0.12f, 2.5f, 4.5f);
            const osg::Vec3f leftTarget = leftShoulder + bodyRight * sideInset + bodyForward * forwardOffset
                - up * armDrop;
            const osg::Vec3f rightTarget = rightShoulder - bodyRight * sideInset + bodyForward * forwardOffset
                - up * armDrop;

            unsigned int solved = 0;
            float leftError = -1.f;
            float rightError = -1.f;
            bool leftReachable = false;
            bool rightReachable = false;
            std::string leftOrders;
            std::string rightOrders;

            const auto solveArm = [&](osg::MatrixTransform& upper, osg::MatrixTransform& forearm,
                                      osg::MatrixTransform& hand, const osg::Vec3f& target,
                                      const osg::Vec3f& poleHint, float& error, bool& reachable,
                                      std::string& orders) {
                unsigned int solvedForArm = 0;
                WorldViewerWeaponIkSolve solution;
                constexpr float strength = 0.98f;
                for (unsigned int i = 0; i < 8; ++i)
                {
                    solution = solveFalloutWeaponIkTwoBone(getNodeWorldMatrix(&upper).getTrans(),
                        getNodeWorldMatrix(&forearm).getTrans(), getNodeWorldMatrix(&hand).getTrans(), target, poleHint);
                    error = solution.mError;
                    reachable = solution.mReachable;
                    if (!solution.mSolved)
                        break;

                    const WorldViewerWeaponIkRotationProbe upperProbe
                        = rotateFalloutWeaponIkSegmentToBest(upper, forearm, solution.mMid, strength);
                    if (upperProbe.mSolved)
                    {
                        ++solvedForArm;
                        orders += orders.empty() ? std::string("upper=") : std::string(",upper=");
                        orders += upperProbe.mOrder;
                    }

                    const WorldViewerWeaponIkRotationProbe forearmProbe
                        = rotateFalloutWeaponIkSegmentToBest(forearm, hand, solution.mEnd, strength);
                    if (forearmProbe.mSolved)
                    {
                        ++solvedForArm;
                        orders += orders.empty() ? std::string("forearm=") : std::string(",forearm=");
                        orders += forearmProbe.mOrder;
                    }

                    error = (getNodeWorldMatrix(&hand).getTrans() - target).length();
                    if (error <= 3.f)
                        break;
                }
                return solvedForArm;
            };

            solved += solveArm(*leftUpper, *leftForearm, *leftHand, leftTarget,
                -bodyRight * 0.75f + bodyForward * 0.20f - up * 0.20f, leftError, leftReachable, leftOrders);
            solved += solveArm(*rightUpper, *rightForearm, *rightHand, rightTarget,
                bodyRight * 0.75f + bodyForward * 0.20f - up * 0.20f, rightError, rightReachable, rightOrders);
            refreshSkeleton();

            const osg::Vec3f leftShoulderAfter = getNodeWorldMatrix(leftUpper).getTrans();
            const osg::Vec3f rightShoulderAfter = getNodeWorldMatrix(rightUpper).getTrans();
            const osg::Vec3f leftElbowAfter = getNodeWorldMatrix(leftForearm).getTrans();
            const osg::Vec3f rightElbowAfter = getNodeWorldMatrix(rightForearm).getTrans();
            const osg::Vec3f leftHandAfter = getNodeWorldMatrix(leftHand).getTrans();
            const osg::Vec3f rightHandAfter = getNodeWorldMatrix(rightHand).getTrans();
            const osg::Vec3f shoulderMidAfter = (leftShoulderAfter + rightShoulderAfter) * 0.5f;
            const osg::Vec3f handMidAfter = (leftHandAfter + rightHandAfter) * 0.5f;
            const float shoulderSpanAfter = (leftShoulderAfter - rightShoulderAfter).length();
            const float handSpanAfter = (leftHandAfter - rightHandAfter).length();
            const float elbowSpanAfter = (leftElbowAfter - rightElbowAfter).length();
            const float handMidDropAfter = shoulderMidAfter.z() - handMidAfter.z();
            const float handMidPelvisZAfter = handMidAfter.z() - pelvisOrigin.z();
            const float leftTargetError = (leftHandAfter - leftTarget).length();
            const float rightTargetError = (rightHandAfter - rightTarget).length();
            const bool anatomyOk = handSpanAfter >= std::max(18.f, shoulderSpanAfter * 0.65f)
                && handSpanAfter <= std::max(42.f, shoulderSpanAfter * 1.55f)
                && elbowSpanAfter <= std::max(52.f, shoulderSpanAfter * 1.9f) && handMidDropAfter >= 8.f
                && handMidDropAfter <= 42.f && handMidPelvisZAfter >= -8.f && handMidPelvisZAfter <= 28.f;
            const bool supported = solved >= 4 && leftReachable && rightReachable && leftError >= 0.f
                && rightError >= 0.f && leftTargetError <= 6.f && rightTargetError <= 6.f && anatomyOk;
            if (!supported)
                restoreOriginalArmPose();

            static std::map<std::string, unsigned int> sIdleArmRelaxLogs;
            std::ostringstream keyStream;
            keyStream << ptr.getCellRef().getRefId();
            unsigned int& logs = sIdleArmRelaxLogs[keyStream.str()];
            if (logs < 10)
            {
                ++logs;
                Log(supported ? Debug::Info : Debug::Warning)
                    << "FNV/ESM4 proof: idle arm relax IK actor=" << traits.mEditorId
                    << " ref=" << ptr.getCellRef().getRefId()
                    << " game=" << worldViewerNpcGameTag(traits)
                    << " trigger=wide-high-arms"
                    << " shoulderSpanBefore=" << shoulderSpan
                    << " elbowSpanBefore=" << elbowSpan
                    << " handSpanBefore=" << handSpan
                    << " handSpreadRatioBefore=" << handSpreadRatio
                    << " handMidDropBefore=" << handMidDrop
                    << " handMidPelvisZBefore=" << handMidPelvisZ
                    << " leftTarget=(" << leftTarget.x() << "," << leftTarget.y() << "," << leftTarget.z() << ")"
                    << " rightTarget=(" << rightTarget.x() << "," << rightTarget.y() << "," << rightTarget.z() << ")"
                    << " armDrop=" << armDrop
                    << " sideInset=" << sideInset
                    << " forwardOffset=" << forwardOffset
                    << " solved=" << solved
                    << " reachable=(" << leftReachable << "," << rightReachable << ")"
                    << " errors=(" << leftError << "," << rightError << ")"
                    << " targetErrors=(" << leftTargetError << "," << rightTargetError << ")"
                    << " orders=(" << leftOrders << "|" << rightOrders << ")"
                    << " handSpanAfter=" << handSpanAfter
                    << " elbowSpanAfter=" << elbowSpanAfter
                    << " handMidDropAfter=" << handMidDropAfter
                    << " handMidPelvisZAfter=" << handMidPelvisZAfter
                    << " restored=" << !supported
                    << " runtime=" << (supported ? "runtime-supported" : "loaded-pending-runtime")
                    << " gate=runtime-fallout-idle-arm-relax-ik";
            }

            return supported;
        }

        bool applyFalloutLongGunOffhandIk(const Animation::NodeMap& nodeMap, SceneUtil::Skeleton* skeleton,
            const MWWorld::Ptr& ptr, const ESM4::Npc& traits, const ESM4::Weapon& weapon)
        {
            if (!worldViewerFONVLongGunOffhandIkEnabled()
                || (!traits.mIsFO3 && !traits.mIsFONV) || !isFalloutWeaponIkLongGun(weapon))
                return false;

            const auto findMatrix = [&](std::initializer_list<std::string_view> names) {
                return dynamic_cast<osg::MatrixTransform*>(findBestAttachmentNode(nodeMap, names));
            };

            osg::MatrixTransform* weaponFrame = findMatrix({ "Weapon", "weapon", "Bip01 Weapon" });
            osg::MatrixTransform* leftClavicle = findMatrix({ "Bip01 L Clavicle", "bip01 l clavicle" });
            osg::MatrixTransform* leftUpper = findMatrix({ "Bip01 L UpperArm", "bip01 l upperarm" });
            osg::MatrixTransform* leftForearm = findMatrix({ "Bip01 L Forearm", "bip01 l forearm" });
            osg::MatrixTransform* leftHand = findMatrix({ "Bip01 L Hand", "bip01 l hand" });
            if (weaponFrame == nullptr || leftUpper == nullptr || leftForearm == nullptr || leftHand == nullptr)
                return false;

            const osg::Matrix originalClavicleWorld = leftClavicle != nullptr ? getNodeWorldMatrix(leftClavicle) : osg::Matrix();
            const osg::Matrix originalUpperWorld = getNodeWorldMatrix(leftUpper);
            const osg::Matrix originalForearmWorld = getNodeWorldMatrix(leftForearm);
            const osg::Matrix originalHandWorld = getNodeWorldMatrix(leftHand);
            const auto restoreOriginalArmPose = [&]() {
                if (leftClavicle != nullptr)
                    setFalloutIkTransformWorldMatrix(*leftClavicle, originalClavicleWorld);
                setFalloutIkTransformWorldMatrix(*leftUpper, originalUpperWorld);
                setFalloutIkTransformWorldMatrix(*leftForearm, originalForearmWorld);
                setFalloutIkTransformWorldMatrix(*leftHand, originalHandWorld);
                if (skeleton != nullptr)
                {
                    skeleton->markBoneMatriceDirty();
                    skeleton->updateBoneMatrices(0);
                }
            };

            int syntheticHelper = 0;
            weaponFrame->getUserValue("esm4SyntheticAttachmentHelper", syntheticHelper);
            if (syntheticHelper == 0)
                return false;

            FalloutWeaponMeshAuditVisitor visitor(true);
            weaponFrame->accept(visitor);
            const osg::BoundingBox& localBox = visitor.mAttachedLocalBox;
            if (!localBox.valid())
                return false;

            const osg::Vec3f localExtent = boundingBoxExtent(localBox);
            const osg::Vec3f localLongAxis(1.f, 0.f, 0.f);
            const float foreEndOffset = std::clamp(localExtent.x() * 0.16f, 6.f, 12.f);
            const osg::Vec3f requestedLocalTarget = localBox.center() + localLongAxis * foreEndOffset;
            const osg::Matrix weaponWorld = getNodeWorldMatrix(weaponFrame);
            const osg::Vec3f requestedTarget = transformPoint(requestedLocalTarget, weaponWorld);

            const osg::Vec3f shoulder = getNodeWorldMatrix(leftUpper).getTrans();
            const osg::Vec3f rightHandOrigin = weaponWorld.getTrans();
            const float upperLength = std::clamp(
                static_cast<float>((getNodeWorldMatrix(leftForearm).getTrans() - shoulder).length()), 4.f, 36.f);
            const float lowerLength = std::clamp(
                static_cast<float>(
                    (getNodeWorldMatrix(leftHand).getTrans() - getNodeWorldMatrix(leftForearm).getTrans()).length()),
                4.f, 36.f);
            const float maxReach = std::max(0.001f, upperLength + lowerLength - 0.25f);
            const float minGripSpan = 18.f;
            const float maxGripSpan = 34.f;
            osg::Vec3f localTarget = requestedLocalTarget;
            osg::Vec3f target = requestedTarget;
            float targetScore = 1e30f;
            float targetGripSpan = (requestedTarget - rightHandOrigin).length();
            float targetShoulderDistance = (requestedTarget - shoulder).length();
            bool targetReachable = targetShoulderDistance <= maxReach;
            bool targetAdjusted = false;

            const float localMinX = localBox.xMin();
            const float localMaxX = localBox.xMax();
            const float desiredX = std::clamp(requestedLocalTarget.x(), localMinX, localMaxX);
            const osg::Vec3f localCenter = localBox.center();
            for (unsigned int i = 0; i <= 32; ++i)
            {
                const float t = static_cast<float>(i) / 32.f;
                const float candidateX = desiredX + (localMinX - desiredX) * t;
                const osg::Vec3f candidateLocal(candidateX, localCenter.y(), localCenter.z());
                const osg::Vec3f candidateWorld = transformPoint(candidateLocal, weaponWorld);
                const float shoulderDistance = (candidateWorld - shoulder).length();
                const float gripSpan = (candidateWorld - rightHandOrigin).length();
                if (shoulderDistance > maxReach || gripSpan < minGripSpan || gripSpan > maxGripSpan)
                    continue;

                const float score = (desiredX - candidateX) * 0.15f + std::abs(gripSpan - 23.f) * 0.35f
                    + shoulderDistance * 0.02f;
                if (score < targetScore)
                {
                    targetScore = score;
                    localTarget = candidateLocal;
                    target = candidateWorld;
                    targetGripSpan = gripSpan;
                    targetShoulderDistance = shoulderDistance;
                    targetReachable = true;
                    targetAdjusted = std::abs(candidateX - requestedLocalTarget.x()) > 0.001f;
                }
            }

            if (!targetReachable)
            {
                restoreOriginalArmPose();
                static std::map<std::string, unsigned int> sOffhandIkUnreachableLogs;
                std::ostringstream keyStream;
                keyStream << ptr.getCellRef().getRefId() << "|" << weapon.mEditorId << "|unreachable";
                unsigned int& logs = sOffhandIkUnreachableLogs[keyStream.str()];
                if (logs < 8)
                {
                    ++logs;
                    Log(Debug::Warning)
                        << "FNV/ESM4 proof: long-gun offhand IK actor=" << traits.mEditorId
                        << " ref=" << ptr.getCellRef().getRefId()
                        << " weapon=\"" << weapon.mEditorId << "\""
                        << " weaponModel=\"" << weapon.mModel << "\""
                        << " requestedTarget=(" << requestedTarget.x() << "," << requestedTarget.y() << ","
                        << requestedTarget.z() << ")"
                        << " requestedLocalTarget=(" << requestedLocalTarget.x() << "," << requestedLocalTarget.y()
                        << "," << requestedLocalTarget.z() << ")"
                        << " shoulderDistance=" << targetShoulderDistance
                        << " maxReach=" << maxReach
                        << " gripSpan=" << targetGripSpan
                        << " restored=1"
                        << " runtime=loaded-pending-runtime gate=runtime-fnv-long-gun-offhand-ik";
                }
                return false;
            }

            const ESM::Position& position = ptr.getRefData().getPosition();
            osg::Vec3f actorForward(std::sin(position.rot[2]), std::cos(position.rot[2]), 0.f);
            actorForward = normalizeFalloutIkVector(actorForward, osg::Vec3f(0.f, 1.f, 0.f));
            osg::Vec3f actorRight(actorForward.y(), -actorForward.x(), 0.f);
            actorRight = normalizeFalloutIkVector(actorRight, osg::Vec3f(1.f, 0.f, 0.f));
            const osg::Vec3f actorUp(0.f, 0.f, 1.f);
            const osg::Vec3f poleHint = -actorRight * 0.75f - actorUp * 0.85f + actorForward * 0.15f;
            const float strength = 0.92f;

            float error = -1.f;
            bool reachable = false;
            unsigned int solved = 0;
            std::string orders;
            WorldViewerWeaponIkSolve solution;
            for (unsigned int i = 0; i < 6; ++i)
            {
                solution = solveFalloutWeaponIkTwoBone(getNodeWorldMatrix(leftUpper).getTrans(),
                    getNodeWorldMatrix(leftForearm).getTrans(), getNodeWorldMatrix(leftHand).getTrans(), target, poleHint);
                error = solution.mError;
                reachable = solution.mReachable;
                if (!solution.mSolved)
                    break;

                const WorldViewerWeaponIkRotationProbe upperProbe
                    = rotateFalloutWeaponIkSegmentToBest(*leftUpper, *leftForearm, solution.mMid, strength);
                if (upperProbe.mSolved)
                {
                    ++solved;
                    orders += orders.empty() ? std::string("upper=") : std::string(",upper=");
                    orders += upperProbe.mOrder;
                }

                const WorldViewerWeaponIkRotationProbe forearmProbe
                    = rotateFalloutWeaponIkSegmentToBest(*leftForearm, *leftHand, solution.mEnd, strength);
                if (forearmProbe.mSolved)
                {
                    ++solved;
                    orders += orders.empty() ? std::string("forearm=") : std::string(",forearm=");
                    orders += forearmProbe.mOrder;
                }

                error = (getNodeWorldMatrix(leftHand).getTrans() - target).length();
                if (error <= 4.f)
                    break;
            }

            if (solution.mSolved && leftClavicle != nullptr)
            {
                const osg::Vec3f shoulderAfter = getNodeWorldMatrix(leftUpper).getTrans();
                const osg::Vec3f clavicleHint = getNodeWorldMatrix(leftClavicle).getTrans()
                    + normalizeFalloutIkVector(solution.mMid - shoulderAfter, poleHint) * 6.f;
                const WorldViewerWeaponIkRotationProbe clavicleProbe
                    = rotateFalloutWeaponIkSegmentToBest(*leftClavicle, *leftUpper, clavicleHint, strength * 0.08f);
                if (clavicleProbe.mSolved)
                {
                    ++solved;
                    orders += orders.empty() ? std::string("clavicle=") : std::string(",clavicle=");
                    orders += clavicleProbe.mOrder;
                }
            }

            if (skeleton != nullptr)
            {
                skeleton->markBoneMatriceDirty();
                skeleton->updateBoneMatrices(0);
            }

            const float handToWeaponTarget = (getNodeWorldMatrix(leftHand).getTrans() - target).length();
            const float finalHandSpan = (getNodeWorldMatrix(leftHand).getTrans() - rightHandOrigin).length();
            static std::map<std::string, unsigned int> sOffhandIkLogs;
            std::ostringstream keyStream;
            keyStream << ptr.getCellRef().getRefId() << "|" << weapon.mEditorId;
            unsigned int& logs = sOffhandIkLogs[keyStream.str()];
            const bool finalAnatomyOk
                = finalHandSpan >= minGripSpan && finalHandSpan <= maxGripSpan && targetShoulderDistance <= maxReach;
            const bool supported = solved >= 2 && handToWeaponTarget <= 8.f && targetReachable && finalAnatomyOk;
            if (!supported)
                restoreOriginalArmPose();
            if (logs < 8)
            {
                ++logs;
                Log(supported ? Debug::Info : Debug::Warning)
                    << "FNV/ESM4 proof: long-gun offhand IK actor=" << traits.mEditorId
                    << " ref=" << ptr.getCellRef().getRefId()
                    << " weapon=\"" << weapon.mEditorId << "\""
                    << " weaponModel=\"" << weapon.mModel << "\""
                    << " target=(" << target.x() << "," << target.y() << "," << target.z() << ")"
                    << " localTarget=(" << localTarget.x() << "," << localTarget.y() << "," << localTarget.z()
                    << ")"
                    << " requestedTarget=(" << requestedTarget.x() << "," << requestedTarget.y() << ","
                    << requestedTarget.z() << ")"
                    << " requestedLocalTarget=(" << requestedLocalTarget.x() << "," << requestedLocalTarget.y()
                    << "," << requestedLocalTarget.z() << ")"
                    << " targetAdjusted=" << targetAdjusted
                    << " shoulderDistance=" << targetShoulderDistance
                    << " maxReach=" << maxReach
                    << " gripSpan=" << targetGripSpan
                    << " finalHandSpan=" << finalHandSpan
                    << " solved=" << solved
                    << " reachable=" << reachable
                    << " error=" << error
                    << " handToWeaponTarget=" << handToWeaponTarget
                    << " orders=" << orders
                    << " restored=" << !supported
                    << " runtime=" << (supported ? "runtime-supported" : "loaded-pending-runtime")
                    << " gate=runtime-fnv-long-gun-offhand-ik";
            }

            return supported;
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
                osg::Geometry* renderGeometry = geometry;
                const char* renderSource = "drawable";
                if (rig != nullptr)
                {
                    renderGeometry = rig->getLastFrameGeometry();
                    renderSource = renderGeometry == nullptr ? "missing-last-frame" : "last-frame";
                    if (renderGeometry == nullptr)
                    {
                        renderGeometry = rig->getRenderGeometry(0);
                        renderSource = renderGeometry == nullptr ? "missing-render-buffer-0" : "render-buffer-0";
                    }
                }

                const osg::BoundingBox renderBox = computeFalloutGeometryBounds(renderGeometry);
                const osg::BoundingBox sourceBox = computeFalloutGeometryBounds(sourceGeometry);
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
                                 << " renderSource=" << renderSource
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

        std::string_view getFonvWeaponAnimationPrefix(std::uint8_t animationType)
        {
            // TESObjectWEAP::EWeaponType, verified against xNVSE's runtime layout and retail sequence telemetry.
            // In particular, TwoHandAutomatic is the 2ha family; TwoHandMelee is 2hm, not 2ha.
            switch (animationType)
            {
                case 0:
                    return "h2h";
                case 1:
                    return "1hm";
                case 2:
                    return "2hm";
                case 3:
                case 4:
                    return "1hp";
                case 5:
                case 7:
                    return "2hr";
                case 6:
                    return "2ha";
                case 8:
                    return "2hh";
                case 9:
                    return "2hl";
                case 10:
                case 13:
                    return "1gt";
                case 11:
                    return "1md";
                case 12:
                    return "1lm";
                default:
                    return {};
            }
        }

        std::string getFonvWeaponIdlePoseKf(const ESM4::Weapon* weapon)
        {
            if (weapon == nullptr)
                return "meshes/characters/_male/idleanims/talk_handsatside_still2.kf";

            const std::string_view prefix = getFonvWeaponAnimationPrefix(weapon->mData.animationType);
            if (!prefix.empty())
                return "meshes/characters/_male/" + std::string(prefix) + "aim.kf";

            std::string label = weapon->mEditorId + " " + weapon->mModel;
            Misc::StringUtils::lowerCaseInPlace(label);
            if (containsAny(label, { "rifle", "shotgun", "sniper", "launcher", "2hand", "2hr", "varmint" }))
                return "meshes/characters/_male/2hraim.kf";
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

        std::vector<std::string> collectWorldViewerNpcAnimationSources(
            Resource::ResourceSystem* resourceSystem, const ESM4::Npc& traits)
        {
            std::vector<std::string> result;
            const char* configuredSources = std::getenv("OPENMW_WORLD_VIEWER_NPC_ANIMATION_SOURCES");
            if (configuredSources == nullptr || configuredSources[0] == '\0')
                return result;

            const VFS::Manager* vfs = resourceSystem != nullptr ? resourceSystem->getVFS() : nullptr;
            std::stringstream stream(configuredSources);
            std::string path;
            while (std::getline(stream, path, ';'))
            {
                path = normalizeFonvAnimationPath(path);
                if (path.empty())
                    continue;
                if (std::find(result.begin(), result.end(), path) != result.end())
                    continue;
                if (vfs != nullptr && vfs->exists(VFS::Path::Normalized(path)))
                {
                    result.push_back(path);
                    continue;
                }

                Log(Debug::Warning) << "World viewer: configured NPC animation source missing kf=\"" << path
                                    << "\" actor=\"" << traits.mEditorId << "\" game=" << worldViewerNpcGameTag(traits);
            }

            return result;
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

        bool fonvPackageHasExplicitTime(const ESM4::AIPackage& package)
        {
            return package.mSchedule.time != 0xff && package.mSchedule.duration != 0;
        }

        bool fonvPackageCoversHour(const ESM4::AIPackage& package, float hour)
        {
            if (!fonvPackageHasExplicitTime(package))
                return false;

            const float start = static_cast<float>(package.mSchedule.time);
            const float duration = static_cast<float>(std::min<std::uint32_t>(package.mSchedule.duration, 24));
            const float end = std::fmod(start + duration, 24.f);
            if (duration >= 24.f)
                return true;
            if (start <= end)
                return hour >= start && hour < end;
            return hour >= start || hour < end;
        }

        const ESM4::Reference* resolvePackageReference(
            const MWWorld::ESMStore& store, const ESM4::AIPackage::PLDT& location)
        {
            if (location.type != 0 && location.type != 4)
                return nullptr;
            return store.get<ESM4::Reference>().search(formIdFromRaw(location.location));
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
            if (std::getenv("OPENMW_FNV_DISABLE_PACKAGE_PROCEDURE") != nullptr
                || std::getenv("OPENMW_FNV_DISABLE_AI_PACKAGES") != nullptr)
            {
                Log(Debug::Info) << "FNV/ESM4 diag: package procedure animation disabled by proof env for "
                                 << traits.mEditorId;
                return result;
            }

            const auto& packageStore = store.get<ESM4::AIPackage>();
            float hour = MWBase::Environment::get().getWorld()->getTimeStamp().getHour();
            bool usedHourOverride = false;
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

            const ESM4::AIPackage* selected = nullptr;
            for (ESM::FormId packageId : packageIds)
            {
                const ESM4::AIPackage* package = packageStore.search(packageId);
                if (package != nullptr && fonvPackageCoversHour(*package, hour))
                {
                    selected = package;
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
            const bool preferChairSitIdle = std::getenv("OPENMW_FNV_PREFER_CHAIRSIT_IDLE") != nullptr;

            switch (selected->mData.type)
            {
                case 3: // Eat
                    if (usesTableSeat)
                        addProcedureSourceIfPresent(
                            result, *vfs, "meshes/characters/_male/idleanims/sittablechaireata.kf");
                    else
                        addProcedureSourceIfPresent(
                            result, *vfs, "meshes/characters/_male/idleanims/sitchaireata.kf");
                    if (usesChairSeat && !preferChairSitIdle)
                        addProcedureSourceIfPresent(
                            result, *vfs, "meshes/characters/_male/idleanims/dynamicidle_chairsit.kf");
                    addProcedureSourceIfPresent(result, *vfs, "meshes/characters/_male/idleanims/dynamicidle_sit.kf");
                    if (usesChairSeat && preferChairSitIdle)
                        addProcedureSourceIfPresent(
                            result, *vfs, "meshes/characters/_male/idleanims/dynamicidle_chairsit.kf");
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
                        if (usesChairSeat && !preferChairSitIdle)
                            addProcedureSourceIfPresent(
                                result, *vfs, "meshes/characters/_male/idleanims/dynamicidle_chairsit.kf");
                        addProcedureSourceIfPresent(
                            result, *vfs, "meshes/characters/_male/idleanims/dynamicidle_sit.kf");
                        if (usesChairSeat && preferChairSitIdle)
                            addProcedureSourceIfPresent(
                                result, *vfs, "meshes/characters/_male/idleanims/dynamicidle_chairsit.kf");
                    }
                    break;
                default:
                    break;
            }

            for (const std::string& path : result)
                Log(Debug::Info) << "FNV/ESM4 diag: package procedure animation source " << path << " from "
                                 << selected->mEditorId << " type=" << getFonvPackageTypeName(selected->mData.type)
                                 << " furniture=" << furnitureModel << " for " << traits.mEditorId;

            if (const char* overridePath = std::getenv("OPENMW_FNV_PROCEDURE_KF_OVERRIDE"))
            {
                std::vector<std::string> overrideResult;
                std::stringstream stream(overridePath);
                std::string path;
                while (std::getline(stream, path, ';'))
                    addProcedureSourceIfPresent(overrideResult, *vfs, path);
                if (!overrideResult.empty())
                {
                    result = std::move(overrideResult);
                    for (const std::string& path : result)
                        Log(Debug::Info) << "FNV/ESM4 diag: package procedure animation override source "
                                         << path << " for " << traits.mEditorId;
                }
            }

            return result;
        }
    }

    ESM4NpcAnimation::ESM4NpcAnimation(
        const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode, Resource::ResourceSystem* resourceSystem)
        : Animation(ptr, std::move(parentNode), resourceSystem)
    {
        const std::string skeletonModel = mPtr.getClass().getCorrectedModel(mPtr);
        const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(mPtr);
        const ESM4::Npc* modelRecord = MWClass::ESM4Npc::getModelRecord(mPtr);
        const ESM4::Race* race = MWClass::ESM4Npc::getRace(mPtr);
        const VFS::Manager* vfs = mResourceSystem != nullptr ? mResourceSystem->getVFS() : nullptr;
        bool skeletonExists = false;
        if (vfs != nullptr && !skeletonModel.empty())
            skeletonExists = vfs->exists(VFS::Path::toNormalized(skeletonModel));

        if (traits != nullptr)
        {
            std::ostringstream details;
            details << "game=" << worldViewerNpcGameTag(*traits)
                    << " npc=\"" << traits->mEditorId << "\""
                    << " form=" << ESM::RefId(traits->mId)
                    << " race=" << ESM::RefId(traits->mRace)
                    << " raceEditor=\"" << (race != nullptr ? race->mEditorId : std::string()) << "\""
                    << " female=" << MWClass::ESM4Npc::isFemale(mPtr)
                    << " skeleton=\"" << skeletonModel << "\""
                    << " skeletonExists=" << skeletonExists
                    << " modelRecord=\""
                    << (modelRecord != nullptr ? modelRecord->mEditorId : std::string()) << "\""
                    << " modelKfCount=" << (modelRecord != nullptr ? modelRecord->mKf.size() : 0)
                    << " headParts=" << traits->mHeadParts.size()
                    << " armor=" << MWClass::ESM4Npc::getEquippedArmor(mPtr).size()
                    << " clothing=" << MWClass::ESM4Npc::getEquippedClothing(mPtr).size()
                    << " weapon=\""
                    << (MWClass::ESM4Npc::getEquippedWeapon(mPtr) != nullptr
                            ? MWClass::ESM4Npc::getEquippedWeapon(mPtr)->mEditorId
                            : std::string())
                    << "\"";
            logWorldViewerActorLedger(mPtr, "npc-root-begin", details.str());
        }
        else
            logWorldViewerActorLedger(mPtr, "npc-root-begin", "traits=0");

        try
        {
            setObjectRoot(skeletonModel, true, true, false);
        }
        catch (const std::exception& e)
        {
            std::ostringstream details;
            details << "skeleton=\"" << skeletonModel << "\" skeletonExists=" << skeletonExists
                    << " exception=\"" << e.what() << "\"";
            logWorldViewerActorLedger(mPtr, "npc-root-exception", details.str());
            if (!worldViewerSkipMissingActorParts())
                throw;
            return;
        }

        {
            std::ostringstream details;
            details << "skeleton=\"" << skeletonModel << "\" skeletonExists=" << skeletonExists
                    << " objectRoot=" << (mObjectRoot != nullptr)
                    << " skeletonNode=" << (mSkeleton != nullptr)
                    << " nodeMap=" << getNodeMap().size();
            logWorldViewerActorLedger(mPtr, "npc-root-end", details.str());
            logWorldViewerNodeMapSnapshot(mPtr, "npc-node-map", getNodeMap(), "afterRoot=1");
        }
        if (traits != nullptr && isFallout3OrNewVegas(*traits))
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

        if (traits != nullptr && !isFallout3OrNewVegas(*traits))
        {
            const std::vector<std::string> configuredSources
                = collectWorldViewerNpcAnimationSources(mResourceSystem, *traits);
            for (const std::string& kfPath : configuredSources)
            {
                auto source = addSingleAnimSource(kfPath, skeletonModel, false);
                if (worldViewerActorTelemetryEnabled())
                {
                    std::ostringstream details;
                    details << "game=" << worldViewerNpcGameTag(*traits)
                            << " npc=\"" << traits->mEditorId << "\""
                            << " kf=\"" << kfPath << "\""
                            << " reason=\"configured world-viewer\""
                            << " skeleton=\"" << skeletonModel << "\""
                            << " bound=" << (source != nullptr)
                            << " primary=1"
                            << " procedureIdle=0";
                    logWorldViewerActorLedger(mPtr, "animation-source", details.str());
                }
            }
        }

        if (traits != nullptr && isFallout3OrNewVegas(*traits))
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
                auto source = addSingleAnimSource(kfPath, skeletonModel, falloutProcedureIdle);
                if (worldViewerActorTelemetryEnabled())
                {
                    std::ostringstream details;
                    details << "game=" << worldViewerNpcGameTag(*traits)
                            << " npc=\"" << traits->mEditorId << "\""
                            << " kf=\"" << kfPath << "\""
                            << " reason=\"" << reason << "\""
                            << " skeleton=\"" << skeletonModel << "\""
                            << " bound=" << (source != nullptr)
                            << " primary=" << countsAsPrimary
                            << " procedureIdle=" << falloutProcedureIdle;
                    logWorldViewerActorLedger(mPtr, "animation-source", details.str());
                }
                if (source != nullptr)
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
                procedureIdleSources
                    = collectFonvPackageProcedureAnimationSources(mPtr, *store, mResourceSystem, *traits, packageIds);
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
                Log(Debug::Info) << "FNV/ESM4 diag: suppressing Fallout fast locomotion fallback KFs for "
                                 << traits->mEditorId
                                 << " because offline anatomy sweep rejects mtfast* under the current pose path; "
                                    "mt* walk sources synthesize run groups instead";
                addFonvAnimationSource("meshes/characters/_male/locomotion/mtturnleft.kf", "fallback");
                addFonvAnimationSource("meshes/characters/_male/locomotion/mtturnright.kf", "fallback");

                if (const ESM4::Weapon* weapon = MWClass::ESM4Npc::getEquippedWeapon(mPtr))
                {
                    const std::string_view prefix = getFonvWeaponAnimationPrefix(weapon->mData.animationType);
                    if (!prefix.empty())
                    {
                        // Retail FNV resolves locomotion through the equipped weapon family. Some families author
                        // only a subset (for example 2ha has a walk-forward plus four fast directions), so add the
                        // complete candidate set and let the VFS-backed source loader ignore absent files.
                        constexpr std::array<std::string_view, 10> suffixes{ "forward", "backward", "left", "right",
                            "fastforward", "fastbackward", "fastleft", "fastright", "turnleft", "turnright" };
                        for (std::string_view suffix : suffixes)
                        {
                            addFonvAnimationSource("meshes/characters/_male/locomotion/" + std::string(prefix)
                                    + std::string(suffix) + ".kf",
                                "retail weapon-family locomotion", false);
                        }
                    }

                    const char* esm4WeaponPose = std::getenv("OPENMW_ESM4_ENABLE_WEAPON_IDLE_POSE");
                    const char* fnvWeaponPose = std::getenv("OPENMW_FNV_ENABLE_WEAPON_IDLE_POSE");
                    const bool useWeaponIdlePose = esm4WeaponPose != nullptr
                        ? std::string_view(esm4WeaponPose) != "0"
                        : (fnvWeaponPose != nullptr ? std::string_view(fnvWeaponPose) != "0" : true);
                    if (useWeaponIdlePose)
                    {
                        const std::string weaponPose = getFonvWeaponIdlePoseKf(weapon);
                        addFonvAnimationSource(weaponPose, "weapon idle pose", false);
                    }
                    else
                        Log(Debug::Info) << "FNV/ESM4 diag: keeping ambient neutral idle for " << traits->mEditorId
                                         << " despite equipped weapon=" << weapon->mEditorId;
                }
            }

            // Add scheduled package procedure sources last because Animation::play resolves sources in reverse order.
            // These are narrow candidates such as Easy Pete's chair/eat package and should beat neutral mTIdle.
            for (const std::string& kfPath : procedureIdleSources)
                addFonvAnimationSource(kfPath, "scheduled package procedure", false, true);

            if (const char* forcedKfs = std::getenv("OPENMW_FNV_FORCED_KF_SOURCE"))
            {
                std::stringstream stream(forcedKfs);
                std::string kfPath;
                while (std::getline(stream, kfPath, ';'))
                {
                    kfPath = normalizeFonvAnimationPath(kfPath);
                    if (!kfPath.empty())
                        addFonvAnimationSource(kfPath, "forced diagnostic KF", false, true);
                }
            }

            addFalloutProofDialoguePose(getNodeMap(), mPtr, *traits);
        }
    }

    void ESM4NpcAnimation::applyPostManualFalloutActorPose()
    {
        const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(mPtr);
        if (traits == nullptr || (!traits->mIsFO3 && !traits->mIsFONV) || mObjectRoot == nullptr)
            return;

        applyFalloutIdleArmRelaxIk(getNodeMap(), mSkeleton, mPtr, *traits);
        applyFalloutWeaponGripIk(getNodeMap(), mObjectRoot.get(), mSkeleton, mPtr, *traits);
    }

    osg::Vec3f ESM4NpcAnimation::runAnimation(float duration)
    {
        osg::Vec3f movement = Animation::runAnimation(duration);
        const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(mPtr);
        if (traits == nullptr || (!traits->mIsFO3 && !traits->mIsFONV) || mObjectRoot == nullptr)
            return movement;

        const bool idleArmRelaxed = applyFalloutIdleArmRelaxIk(getNodeMap(), mSkeleton, mPtr, *traits);
        const bool weaponIkSupported
            = applyFalloutWeaponGripIk(getNodeMap(), mObjectRoot.get(), mSkeleton, mPtr, *traits);

        if (mSkeleton != nullptr)
        {
            mSkeleton->markBoneMatriceDirty();
            mSkeleton->updateBoneMatrices(0);
        }

        const ESM4::Weapon* equippedWeapon = MWClass::ESM4Npc::getEquippedWeapon(mPtr);
        const bool weaponFrameStabilized = equippedWeapon != nullptr
            && (stabilizeFalloutLongGunWeaponFrame(getNodeMap(), mPtr, *traits, *equippedWeapon)
                || stabilizeFalloutSidearmWeaponFrame(getNodeMap(), mPtr, *traits, *equippedWeapon));
        const bool longGunOffhandIkSupported = equippedWeapon != nullptr
            && applyFalloutLongGunOffhandIk(getNodeMap(), mSkeleton, mPtr, *traits, *equippedWeapon);

        unsigned int forcedRigGeometryHolder = 0;
        unsigned int refreshedRigGeometry = 0;
        const unsigned int forcedRigGeometry
            = forceFalloutRigGeometryUpdate(mObjectRoot.get(), forcedRigGeometryHolder, refreshedRigGeometry);

        if (const ESM4::Weapon* weapon = equippedWeapon)
        {
            osg::Group* weaponFrame = findBestAttachmentNode(
                getNodeMap(), { "Weapon", "weapon", "Bip01 Weapon", "Bip01 R Hand", "bip01 r hand" });
            if (weaponFrame != nullptr)
            {
                const VFS::Path::Normalized correctedModel
                    = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(weapon->mModel));
                logFalloutWeaponMeshAudit(
                    weaponFrame, weaponFrame, getNodeMap(), correctedModel.value(), "Weapon", mPtr, "runtime");
            }
        }

        static std::map<std::string, unsigned int> sFrameRefreshLogs;
        const std::string refId = mPtr.getCellRef().getRefId().serializeText();
        unsigned int& logs = sFrameRefreshLogs[refId];
        if (logs < (isProofPreviewAnimation() ? 12u : 2u))
        {
            ++logs;
            Log(refreshedRigGeometry > 0 ? Debug::Info : Debug::Warning)
                << "FNV/ESM4 proof: actor frame forced rig mesh refresh actor=" << traits->mEditorId
                << " ref=" << mPtr.getCellRef().getRefId()
                << " proofPreview=" << isProofPreviewAnimation()
                << " idleArmRelaxed=" << idleArmRelaxed
                << " weaponIkSupported=" << weaponIkSupported
                << " weaponFrameStabilized=" << weaponFrameStabilized
                << " longGunOffhandIkSupported=" << longGunOffhandIkSupported
                << " forcedRigGeometry=" << forcedRigGeometry
                << " forcedRigGeometryHolder=" << forcedRigGeometryHolder
                << " refreshedRigGeometry=" << refreshedRigGeometry
                << " runtime=" << (refreshedRigGeometry > 0 ? "runtime-supported" : "loaded-pending-runtime")
                << " gate=runtime-fnv-frame-rig-refresh";
        }

        return movement;
    }

    void ESM4NpcAnimation::updateParts()
    {
        if (mObjectRoot == nullptr)
        {
            logWorldViewerActorLedger(mPtr, "parts-skip", "reason=\"missing object root\"");
            return;
        }
        const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(mPtr);
        if (traits == nullptr)
        {
            logWorldViewerActorLedger(mPtr, "parts-skip", "reason=\"missing traits\"");
            return;
        }
        std::string_view branch = traits->mIsTES4 ? std::string_view("TES4")
            : isFallout3OrNewVegas(*traits)       ? std::string_view("FALLOUT")
                                                  : std::string_view("TES5");
        {
            std::ostringstream details;
            details << "branch=" << branch << " game=" << worldViewerNpcGameTag(*traits)
                    << " npc=\"" << traits->mEditorId << "\""
                    << " race=" << ESM::RefId(traits->mRace)
                    << " headParts=" << traits->mHeadParts.size()
                    << " armor=" << MWClass::ESM4Npc::getEquippedArmor(mPtr).size()
                    << " clothing=" << MWClass::ESM4Npc::getEquippedClothing(mPtr).size();
            logWorldViewerActorLedger(mPtr, "parts-begin", details.str());
        }
        if (traits->mIsTES4)
            updatePartsTES4(*traits);
        else if (isFallout3OrNewVegas(*traits))
            updatePartsFONV(*traits);
        else
        {
            // Skyrim and newer non-Fallout actors still use the TES5-style static part path.
            // Full HKX/behavior animation is not implemented here.
            updatePartsTES5(*traits);
        }
        applyWorldViewerFlatActorMaterials(mObjectRoot.get(), mPtr, "parts-end");
        applyWorldViewerFullbrightActorMaterials(mObjectRoot.get(), mPtr, "parts-end");
        {
            std::ostringstream details;
            details << "branch=" << branch << " game=" << worldViewerNpcGameTag(*traits)
                    << " npc=\"" << traits->mEditorId << "\""
                    << " objectRoot=" << (mObjectRoot != nullptr)
                    << " nodeMap=" << getNodeMap().size()
                    << makeActorVisualAuditDetails(mObjectRoot.get());
            logWorldViewerActorLedger(mPtr, "parts-end", details.str());
        }
    }

    osg::ref_ptr<osg::Node> ESM4NpcAnimation::insertPart(
        std::string_view model, const osg::Vec4f* tint, std::string_view diffuseTexture)
    {
        if (model.empty())
        {
            Log(Debug::Verbose) << "FNV/ESM4 diag: skipped empty NPC model part for "
                                << mPtr.getCellRef().getRefId();
            logWorldViewerActorLedger(mPtr, "part-skip", "reason=\"empty model\"");
            return nullptr;
        }
        if (Misc::StringUtils::ciEndsWith(model, ".egt") || Misc::StringUtils::ciEndsWith(model, ".egm")
            || Misc::StringUtils::ciEndsWith(model, ".tri"))
        {
            Log(Debug::Verbose) << "FNV/ESM4 diag: skipped non-render NPC data part " << model << " for "
                                << mPtr.getCellRef().getRefId();
            std::ostringstream details;
            details << "reason=\"non-render data\" model=\"" << model << "\"";
            logWorldViewerActorLedger(mPtr, "part-skip", details.str());
            return nullptr;
        }
        const VFS::Path::Normalized correctedModel = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(model));
        const VFS::Manager* vfs = mResourceSystem != nullptr ? mResourceSystem->getVFS() : nullptr;
        const bool modelExists = vfs != nullptr && vfs->exists(correctedModel);
        const ESM4::Npc* traitsRecord = MWClass::ESM4Npc::getTraitsRecord(mPtr);
        const bool falloutHumanPart = traitsRecord != nullptr && isFallout3OrNewVegas(*traitsRecord);
        const bool tes5StaticPart
            = traitsRecord != nullptr && !traitsRecord->mIsTES4 && !isFallout3OrNewVegas(*traitsRecord);
        {
            std::ostringstream details;
            details << "model=\"" << model << "\" corrected=\"" << correctedModel.value() << "\""
                    << " vfsExists=" << modelExists
                    << " tint=" << (tint != nullptr)
                    << " diffuse=\"" << diffuseTexture << "\"";
            logWorldViewerActorLedger(mPtr, "part-request", details.str());
        }
        if (!modelExists && worldViewerSkipMissingActorParts())
        {
            std::ostringstream details;
            details << "reason=\"missing vfs\" model=\"" << model << "\" corrected=\"" << correctedModel.value()
                    << "\"";
            logWorldViewerActorLedger(mPtr, "part-missing", details.str());
            return nullptr;
        }
        const std::string loweredModel = Misc::StringUtils::lowerCase(std::string(model));
        if (std::getenv("OPENMW_FNV_PROOF_HIDE_FACE_HAIR") != nullptr && isFalloutFaceHairModel(loweredModel))
        {
            Log(Debug::Info) << "FNV/ESM4 proof: skipped face hair diagnostic part " << correctedModel.value()
                             << " for " << mPtr.getCellRef().getRefId();
            return nullptr;
        }
        if (std::getenv("OPENMW_FNV_PROOF_HIDE_BROWS") != nullptr && isFalloutBrowModel(loweredModel))
        {
            Log(Debug::Info) << "FNV/ESM4 proof: skipped brow diagnostic part " << correctedModel.value()
                             << " for " << mPtr.getCellRef().getRefId();
            return nullptr;
        }
        if (std::getenv("OPENMW_FNV_PROOF_HIDE_HAIR") != nullptr && isFalloutScalpHairModel(loweredModel))
        {
            Log(Debug::Info) << "FNV/ESM4 proof: skipped scalp hair diagnostic part " << correctedModel.value()
                             << " for " << mPtr.getCellRef().getRefId();
            return nullptr;
        }
        if (std::getenv("OPENMW_FNV_PROOF_HIDE_ALL_HAIR") != nullptr && loweredModel.find("hair") != std::string::npos)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: skipped all-hair diagnostic part " << correctedModel.value()
                             << " for " << mPtr.getCellRef().getRefId();
            return nullptr;
        }
        if (std::getenv("OPENMW_FNV_PROOF_HIDE_HEADGEAR") != nullptr && isFalloutStaticHeadgearPart(model))
        {
            Log(Debug::Info) << "FNV/ESM4 proof: skipped headgear diagnostic part " << correctedModel.value()
                             << " for " << mPtr.getCellRef().getRefId();
            return nullptr;
        }
        if (std::getenv("OPENMW_FNV_PROOF_FACE_PARTS_ONLY") != nullptr
            && !shouldAttachFalloutStaticPartToHead(model))
        {
            Log(Debug::Info) << "FNV/ESM4 proof: skipped non-face diagnostic part " << correctedModel.value()
                             << " for " << mPtr.getCellRef().getRefId();
            return nullptr;
        }
        osg::ref_ptr<const osg::Node> templateNode;
        try
        {
            templateNode = mResourceSystem->getSceneManager()->getTemplate(correctedModel);
        }
        catch (const std::exception& e)
        {
            std::ostringstream details;
            details << "model=\"" << model << "\" corrected=\"" << correctedModel.value() << "\""
                    << " vfsExists=" << modelExists
                    << " exception=\"" << e.what() << "\"";
            logWorldViewerActorLedger(mPtr, "part-template-exception", details.str());
            if (!worldViewerSkipMissingActorParts())
                throw;
            return nullptr;
        }
        if (templateNode == nullptr)
        {
            std::ostringstream details;
            details << "model=\"" << model << "\" corrected=\"" << correctedModel.value() << "\""
                    << " vfsExists=" << modelExists;
            logWorldViewerActorLedger(mPtr, "part-template-null", details.str());
            return nullptr;
        }
        if (tes5StaticPart && tes5UnstableFaceSurfaceQuarantineEnabled()
            && isTes5UnstableStaticFaceSurfaceModel(model))
        {
            std::ostringstream details;
            details << "reason=\"tes5 unstable face surface\""
                    << " model=\"" << model << "\" corrected=\"" << correctedModel.value() << "\""
                    << " vfsExists=" << modelExists
                    << " template=1"
                    << " actor=\"" << (traitsRecord != nullptr ? traitsRecord->mEditorId : std::string()) << "\"";
            logWorldViewerActorLedger(mPtr, "part-quarantine", details.str());
            Log(Debug::Info) << "World viewer: quarantined TES5 unstable face surface model="
                             << correctedModel.value()
                             << " actor=" << (traitsRecord != nullptr ? traitsRecord->mEditorId : std::string())
                             << " reason=eye-mouth-surface";
            return nullptr;
        }

        osg::Group* attachNode = mObjectRoot.get();
        const NodeMap& nodeMap = getNodeMap();
        const bool headAttachedStaticPart = isFalloutStaticHeadAttachmentPart(model);
        const bool headgearStaticPart = isFalloutStaticHeadgearPart(model);
        const bool bareHandSurfacePart = isFalloutBareHandSurfaceModel(model);
        bool tes5StaticFaceSurfaceFallback = false;
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
                        if (headgearMode != nullptr && (Misc::StringUtils::ciEqual(headgearMode, "headframe")
                                                           || Misc::StringUtils::ciEqual(headgearMode, "headtranslation")))
                            attachNode = makeFalloutHeadFrameHelper(*bip01, *head);
                        else
                            attachNode = makeFalloutAnimatedHeadFrameHelper(*bip01, *head);
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
            if (worldViewerNodeMapTelemetryEnabled())
            {
                osg::Group* bip01 = findBestAttachmentNode(nodeMap, { "Bip01", "bip01" });
                osg::Group* head = findBestAttachmentNode(
                    nodeMap, { "Bip01 Head", "bip01 head", "Head", "head", "NPC Head [Head]", "head bone" });
                std::ostringstream details;
                details << "model=\"" << correctedModel.value() << "\""
                        << " mode=\"" << mode << "\""
                        << " attachNode=\"" << (attachNode != nullptr ? printableNodeName(attachNode->getName()) : "<null>")
                        << "\" bip01Found=" << (bip01 != nullptr)
                        << " bip01Name=\"" << (bip01 != nullptr ? printableNodeName(bip01->getName()) : "<null>")
                        << "\" headFound=" << (head != nullptr)
                        << " headName=\"" << (head != nullptr ? printableNodeName(head->getName()) : "<null>")
                        << "\" rootAttach=" << (attachNode == mObjectRoot.get())
                        << " nodeMap=" << nodeMap.size();
                logWorldViewerActorLedger(mPtr, "head-attach-probe", details.str());
            }
        }
        if (tes5StaticPart && isFalloutStaticFaceChildPart(model) && attachNode == mObjectRoot.get()
            && tes5StaticFaceSurfaceFallbackEnabled(model))
        {
            attachNode = makeTes5StaticFaceSurfaceFrameHelper(*mObjectRoot, model);
            tes5StaticFaceSurfaceFallback = true;
            Log(Debug::Info) << "World viewer: TES5 static face surface fallback model=" << correctedModel.value()
                             << " actor=" << (traitsRecord != nullptr ? traitsRecord->mEditorId : std::string())
                             << " attachNode=" << attachNode->getName()
                             << " nodeMap=" << nodeMap.size();
        }
        else if (!tes5StaticPart && isFalloutStaticFaceChildPart(model) && attachNode != nullptr)
            attachNode = makeFalloutFaceSurfaceFrameHelper(*attachNode);
        else if (tes5StaticPart && isFalloutStaticFaceChildPart(model) && attachNode == mObjectRoot.get()
            && worldViewerNodeMapTelemetryEnabled())
        {
            std::ostringstream details;
            details << "model=\"" << correctedModel.value() << "\""
                    << " reason=\"tes5-starfield-direct-actor-space\""
                    << " attachNode=\"" << printableNodeName(attachNode->getName()) << "\""
                    << " nodeMap=" << nodeMap.size();
            logWorldViewerActorLedger(mPtr, "head-attach-direct", details.str());
        }
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
        const std::size_t rigBoneMatches = countRigBoneMatches(nodeMap, rigProbe.mFirstRigBoneNames);
        {
            std::ostringstream details;
                    details << "model=\"" << model << "\" corrected=\"" << correctedModel.value() << "\""
                    << " vfsExists=" << modelExists
                    << " template=1"
                    << " rigGeometry=" << rigProbe.mRigGeometryCount
                    << " morphGeometry=" << rigProbe.mMorphGeometryCount
                    << " staticGeometry=" << rigProbe.mStaticGeometryCount
                    << " firstRigRoot=\"" << rigProbe.mFirstRigRootBone << "\""
                    << " firstRigBoneCount=" << rigProbe.mFirstRigBoneCount
                    << " rigBoneMatches=" << rigBoneMatches
                    << " nodeMap=" << nodeMap.size();
            logWorldViewerActorLedger(mPtr, "part-template", details.str());
        }
        if (worldViewerSkipUnmappedRiggedActorParts() && rigProbe.mRigGeometryCount > 0
            && rigProbe.mFirstRigBoneCount > 0 && rigBoneMatches == 0
            && !headAttachedStaticPart && !bareHandSurfacePart)
        {
            std::ostringstream details;
            details << "reason=\"unmapped rig bones\" model=\"" << model << "\" corrected=\""
                    << correctedModel.value() << "\" rigGeometry=" << rigProbe.mRigGeometryCount
                    << " morphGeometry=" << rigProbe.mMorphGeometryCount
                    << " firstRigRoot=\"" << rigProbe.mFirstRigRootBone << "\""
                    << " firstRigBoneCount=" << rigProbe.mFirstRigBoneCount
                    << " firstRigBoneSample=\"" << rigProbe.mFirstRigBoneSample.str() << "\""
                    << " nodeMap=" << nodeMap.size();
            logWorldViewerActorLedger(mPtr, "part-skip", details.str());
            Log(Debug::Warning) << "World viewer: skipped unmapped rigged NPC part " << correctedModel.value()
                                << " for " << mPtr.getCellRef().getRefId()
                                << " rigGeometry=" << rigProbe.mRigGeometryCount
                                << " firstRigRoot=" << rigProbe.mFirstRigRootBone
                                << " firstRigBones=" << rigProbe.mFirstRigBoneCount
                                << " sample=[" << rigProbe.mFirstRigBoneSample.str() << "]"
                                << " nodeMap=" << nodeMap.size();
            return nullptr;
        }
        osg::ref_ptr<const osg::Node> attachTemplateNode = templateNode;
        bool staticizedHeadPartRig = false;
        bool staticizedBareHandPartRig = false;
        const bool wantsStaticizedHeadPartRig = headAttachedStaticPart && rigProbe.mRigGeometryCount > 0
            && std::getenv("OPENMW_FNV_STATICIZE_RIGGED_HEAD_PARTS") != nullptr
            && std::getenv("OPENMW_FNV_KEEP_RIGGED_HEAD_PARTS") == nullptr;
        const bool wantsStaticizedBareHandPartRig = bareHandSurfacePart && rigProbe.mRigGeometryCount > 0
            && std::getenv("OPENMW_FNV_STATICIZE_RIGGED_HAND_PARTS") != nullptr
            && std::getenv("OPENMW_FNV_KEEP_RIGGED_HAND_PARTS") == nullptr;
        if (rigProbe.mRigGeometryCount > 0)
        {
            // Mark the cached template before SceneManager clones it.  RigGeometry's copy constructor preserves
            // this bit, so the very first update/cull traversal uses the Fallout skinning convention instead of
            // briefly treating an attached Bethesda body part as a generic OpenMW rig.
            MarkFalloutRigGeometryVisitor markFalloutRigs;
            const_cast<osg::Node*>(attachTemplateNode.get())->accept(markFalloutRigs);
            if (markFalloutRigs.mMarked > 0)
                Log(Debug::Info) << "FNV/ESM4 diag: pre-marked " << markFalloutRigs.mMarked
                                 << " Fallout rigged template drawable(s) on " << correctedModel.value() << " for "
                                 << mPtr.getCellRef().getRefId();
        }
        if (wantsStaticizedHeadPartRig)
        {
            osg::ref_ptr<osg::Node> staticTemplate = osg::clone(templateNode.get(), osg::CopyOp::DEEP_COPY_ALL);
            StaticizeFalloutRiggedGeometryVisitor staticizeVisitor;
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
            StaticizeFalloutRiggedGeometryVisitor staticizeVisitor;
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
            attached = attachStaticizedFalloutPart(
                attachTemplateNode, attachNode, mResourceSystem->getSceneManager(), true);
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

            StaticizeFalloutRiggedGeometryVisitor liveStaticizeVisitor;
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
        else if (worldViewerEnvEnabled("OPENMW_WORLD_VIEWER_ATTACH_STATIC_SKELETON_PARTS")
            && rigProbe.mRigGeometryCount == 0 && rigProbe.mStaticGeometryCount > 0
            && dynamic_cast<const SceneUtil::Skeleton*>(attachTemplateNode.get()) != nullptr)
        {
            attached = attachStaticizedFalloutPart(
                attachTemplateNode, attachNode, mResourceSystem->getSceneManager(), false);
            Log(Debug::Info) << "World viewer: static-skeleton attached NPC model part "
                             << correctedModel.value() << " to " << mPtr.getCellRef().getRefId()
                             << " attachNode=" << attachNode->getName()
                             << " staticGeometry=" << rigProbe.mStaticGeometryCount
                             << " nodeMap=" << nodeMap.size();
        }
        else
        {
            osg::Quat headgearAttitude;
            const osg::Quat* attitude = nullptr;
            if (headgearStaticPart)
            {
                headgearAttitude = getFalloutHeadgearAttitude();
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
        const bool skipFalloutHeadFrameSurfaceOffsetForTes5ActorSpace
            = tes5StaticPart && isFalloutStaticFaceChildPart(model) && !tes5StaticFaceSurfaceFallback
            && tes5StaticFaceOffsetsDisabled(model);
        if (attached != nullptr && headAttachedStaticPart
            && (tes5StaticFaceSurfaceFallback || skipFalloutHeadFrameSurfaceOffsetForTes5ActorSpace))
        {
            Log(Debug::Info) << "World viewer: skipped Fallout head-frame surface offset for TES5 static face part "
                             << correctedModel.value() << " on " << mPtr.getCellRef().getRefId()
                             << " fallback=" << tes5StaticFaceSurfaceFallback
                             << " actorSpace=" << skipFalloutHeadFrameSurfaceOffsetForTes5ActorSpace;
        }
        else if (attached != nullptr && headAttachedStaticPart)
        {
            const osg::Vec3f surfaceOffset = getFalloutHeadFrameSurfaceOffset(model, headgearStaticPart);
            const osg::Quat surfaceAttitude = getFalloutHeadFrameSurfaceAttitude(model, headgearStaticPart);
            if (surfaceOffset.length2() > 0.f || !surfaceAttitude.zeroRotation())
            {
                osg::ref_ptr<osg::Transform> offsetNode;
                const osg::Vec3f pivot = localBoundsCenter(*attached);
                if (!surfaceAttitude.zeroRotation() && std::getenv("OPENMW_FNV_HEAD_SURFACE_PIVOT_ROTATION") != nullptr)
                {
                    osg::ref_ptr<osg::MatrixTransform> matrixNode = new osg::MatrixTransform;
                    matrixNode->setMatrix(osg::Matrix::translate(-pivot) * osg::Matrix::rotate(surfaceAttitude)
                        * osg::Matrix::translate(pivot + surfaceOffset));
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
                                 << getFalloutHeadFrameSurfacePrefix(model, headgearStaticPart) << " pivot=("
                                 << pivot.x() << "," << pivot.y() << "," << pivot.z() << ") pivotMode="
                                 << (std::getenv("OPENMW_FNV_HEAD_SURFACE_PIVOT_ROTATION") != nullptr) << " for "
                                 << mPtr.getCellRef().getRefId();
            }
        }
        if (attached != nullptr && std::getenv("OPENMW_FNV_PROOF_MOUTH_DRIVER") != nullptr
            && isFalloutMouthDriverPart(model))
        {
            osg::ref_ptr<osg::PositionAttitudeTransform> mouthDriverNode = new osg::PositionAttitudeTransform;
            mouthDriverNode->setName("FNV Proof Mouth Driver " + correctedModel.value());
            if (attached->getNumParents() > 0)
            {
                osg::Group* parent = attached->getParent(0);
                if (parent != nullptr && parent->replaceChild(attached.get(), mouthDriverNode.get()))
                    mouthDriverNode->addChild(attached);
            }
            if (mouthDriverNode->getNumChildren() == 0)
                mouthDriverNode->addChild(attached);
            attached = mouthDriverNode;
            attached->setUpdateCallback(new FalloutProofMouthDriver(mPtr, correctedModel.value()));
                Log(Debug::Info) << "FNV/ESM4 proof: attached mouth driver to " << correctedModel.value() << " for "
                                 << mPtr.getCellRef().getRefId();
        }
        if (attached != nullptr && rigProbe.mRigGeometryCount == 0 && rigProbe.mStaticGeometryCount > 0)
            attached = applyWorldViewerStaticActorPartProofScale(attached, correctedModel.value(), mPtr);
        if (!diffuseTexture.empty() && attached != nullptr)
        {
            Log(Debug::Info) << "FNV/ESM4 diag: overriding NPC part texture " << diffuseTexture << " on "
                             << correctedModel.value() << " for " << mPtr.getCellRef().getRefId();
            overrideFalloutPartDiffuseTexture(diffuseTexture, mResourceSystem, *attached);
        }
        if (tint != nullptr && attached != nullptr)
        {
            const float emissionStrength = isFalloutHairTintModel(correctedModel.value()) ? 0.65f : 0.f;
            // FO3/FNV HairShaderProperty uses vertex RGB as shader data, not
            // as an extra diffuse colour multiplier.  Let HCLR drive the
            // material once and keep the authored alpha channel.
            TintMaterialVisitor visitor(*tint, emissionStrength, isFalloutHairTintModel(correctedModel.value()));
            attached->accept(visitor);
        }
        if (attached != nullptr
            && (isFalloutHairTintModel(correctedModel.value()) || isFalloutScalpHairModel(correctedModel.value())
                || isFalloutFaceHairModel(correctedModel.value()) || isFalloutBrowModel(correctedModel.value())))
        {
            FalloutCutoutAlphaVisitor cutoutAlpha;
            attached->accept(cutoutAlpha);
            Log(Debug::Info) << "FNV/ESM4 diag: enabled cutout alpha on " << cutoutAlpha.getAppliedCount()
                             << " hair/brow state(s) for " << correctedModel.value() << " on "
                             << mPtr.getCellRef().getRefId();
            if (worldViewerEnvEnabled("OPENMW_FNV_PROOF_DOUBLE_SIDED_HAIR")
                && isFalloutScalpHairModel(correctedModel.value()))
            {
                DisableCullVisitor visitor;
                attached->accept(visitor);
                Log(Debug::Info) << "FNV/ESM4 diag: made scalp hair double-sided "
                                 << correctedModel.value() << " for " << mPtr.getCellRef().getRefId();
            }
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
            std::string loweredMouthModel = Misc::StringUtils::lowerCase(correctedModel.value());
            const osg::Vec4f mouthColor = loweredMouthModel.find("tongue") != std::string::npos
                ? osg::Vec4f(0.48f, 0.13f, 0.11f, 1.f)
                : osg::Vec4f(1.f, 1.f, 1.f, 1.f);
            TintMaterialVisitor mouthMaterial(mouthColor);
            attached->accept(mouthMaterial);
            DisableCullVisitor visitor;
            attached->accept(visitor);
            Log(Debug::Info) << "FNV/ESM4 diag: made mouth interior surface double-sided "
                             << correctedModel.value() << " color=(" << mouthColor.r() << "," << mouthColor.g()
                             << "," << mouthColor.b() << "," << mouthColor.a() << ") for "
                             << mPtr.getCellRef().getRefId();
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
        forceWorldViewerActorPartMask(attached.get(), correctedModel.value(), mPtr);
        logFalloutPartShapeSummary(attached.get(), correctedModel.value(), mPtr);
        logFalloutAttachmentBounds(
            attached.get(), attachNode, findBestAttachmentNode(nodeMap, { "Bip01 Head", "bip01 head" }),
            correctedModel.value(), mPtr);
        logFalloutFaceDrawableAudit(attached.get(), correctedModel.value(), mPtr);
        {
            std::ostringstream details;
            details << "model=\"" << model << "\" corrected=\"" << correctedModel.value() << "\""
                    << " attached=" << (attached != nullptr)
                    << " attachNode=\"" << (attachNode != nullptr ? attachNode->getName() : std::string()) << "\""
                    << " headAttachment=" << headAttachedStaticPart
                    << " headgear=" << headgearStaticPart
                    << " handSurface=" << bareHandSurfacePart
                    << " rigGeometry=" << rigProbe.mRigGeometryCount
                    << " staticizedHead=" << staticizedHeadPartRig
                    << " staticizedHand=" << staticizedBareHandPartRig
                    << makeActorVisualAuditDetails(attached.get());
            logWorldViewerActorLedger(mPtr, "part-attached", details.str());
        }
        return attached;
    }

    osg::ref_ptr<osg::Node> ESM4NpcAnimation::insertAttachedPart(std::string_view model, std::string_view preferredBone)
    {
        if (model.empty())
        {
            logWorldViewerActorLedger(mPtr, "attached-part-skip", "reason=\"empty model\"");
            return nullptr;
        }

        const VFS::Path::Normalized correctedModel = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(model));
        const VFS::Manager* vfs = mResourceSystem != nullptr ? mResourceSystem->getVFS() : nullptr;
        const bool modelExists = vfs != nullptr && vfs->exists(correctedModel);
        {
            std::ostringstream details;
            details << "model=\"" << model << "\" corrected=\"" << correctedModel.value() << "\""
                    << " preferredBone=\"" << preferredBone << "\""
                    << " vfsExists=" << modelExists;
            logWorldViewerActorLedger(mPtr, "attached-part-request", details.str());
        }
        if (!modelExists && worldViewerSkipMissingActorParts())
        {
            std::ostringstream details;
            details << "reason=\"missing vfs\" model=\"" << model << "\" corrected=\"" << correctedModel.value()
                    << "\" preferredBone=\"" << preferredBone << "\"";
            logWorldViewerActorLedger(mPtr, "attached-part-missing", details.str());
            return nullptr;
        }

        osg::ref_ptr<const osg::Node> templateNode;
        try
        {
            templateNode = mResourceSystem->getSceneManager()->getTemplate(correctedModel);
        }
        catch (const std::exception& e)
        {
            std::ostringstream details;
            details << "model=\"" << model << "\" corrected=\"" << correctedModel.value() << "\""
                    << " preferredBone=\"" << preferredBone << "\""
                    << " exception=\"" << e.what() << "\"";
            logWorldViewerActorLedger(mPtr, "attached-part-template-exception", details.str());
            if (!worldViewerSkipMissingActorParts())
                throw;
            return nullptr;
        }
        if (templateNode == nullptr)
        {
            std::ostringstream details;
            details << "model=\"" << model << "\" corrected=\"" << correctedModel.value() << "\""
                    << " preferredBone=\"" << preferredBone << "\"";
            logWorldViewerActorLedger(mPtr, "attached-part-template-null", details.str());
            return nullptr;
        }

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
        forceWorldViewerActorPartMask(attached.get(), correctedModel.value(), mPtr);
        logFalloutPartShapeSummary(attached.get(), correctedModel.value(), mPtr);
        logFalloutAttachmentBounds(
            attached.get(), attachNode, findBestAttachmentNode(nodeMap, { "Bip01 Head", "bip01 head" }),
            correctedModel.value(), mPtr);
        logFalloutWeaponMeshAudit(attached.get(), attachNode, nodeMap, correctedModel.value(), preferredBone, mPtr);
        {
            std::ostringstream details;
            details << "model=\"" << model << "\" corrected=\"" << correctedModel.value() << "\""
                    << " preferredBone=\"" << preferredBone << "\""
                    << " attached=" << (attached != nullptr)
                    << " attachNode=\"" << (attachNode != nullptr ? attachNode->getName() : std::string()) << "\""
                    << makeActorVisualAuditDetails(attached.get());
            logWorldViewerActorLedger(mPtr, "attached-part-attached", details.str());
        }
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
            && Misc::StringUtils::ciEqual(mPtr.getCellRef().getRefId().serializeText(), "player"))
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

        const ESM4::Race* race = MWClass::ESM4Npc::getRace(mPtr);
        if (race == nullptr)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: race not found while assembling FONV NPC parts for "
                                << traits.mEditorId;
            return;
        }

        const bool isFemale = MWClass::ESM4Npc::isFemale(mPtr);
        const uint32_t coveredBodySlots = getFonvCoveredBodySlots(mPtr);
        // Fallout's exported *_0 face asset is a modulation/detail map over the
        // race diffuse, not a replacement diffuse.  Bodymods use the same
        // neutral-at-0.5 modulation convention, often as a uniform 8x8 map.
        const std::string npcFaceTexture;
        const std::string npcFaceDetailTexture = findFonvNpcFaceDetailTexture(mResourceSystem, traits);
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
            Log(Debug::Info) << "FNV/ESM4 diag: using baked NPC face texture " << npcFaceTexture << " for "
                             << traits.mEditorId;
        if (!npcFaceDetailTexture.empty())
            Log(Debug::Info) << "FNV/ESM4 diag: using baked NPC face detail overlay " << npcFaceDetailTexture
                             << " for " << traits.mEditorId;
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
                             << " as tiny FaceGen detail-only size=" << npcBodyTintWidth << "x" << npcBodyTintHeight
                             << " average=(" << npcBodyMaterialTint.x() << ", " << npcBodyMaterialTint.y() << ", "
                             << npcBodyMaterialTint.z() << "); preserving race body material for " << traits.mEditorId;
        if (!npcBodyNormalTexture.empty())
            Log(Debug::Info) << "FNV/ESM4 diag: using baked NPC body normal texture " << npcBodyNormalTexture
                             << " for " << traits.mEditorId;
        {
            std::ostringstream details;
            details << "game=" << worldViewerNpcGameTag(traits)
                    << " npc=\"" << traits.mEditorId << "\""
                    << " faceTexture=\"" << npcFaceTexture << "\""
                    << " faceDetail=\"" << npcFaceDetailTexture << "\""
                    << " faceNormal=\"" << npcFaceNormalTexture << "\""
                    << " bodyTexture=\"" << npcBodyTexture << "\""
                    << " bodyDetail=\"" << npcBodyDetailTexture << "\""
                    << " bodyNormal=\"" << npcBodyNormalTexture << "\""
                    << " bodyDetailTiny=" << npcBodyDetailIsTinyTint
                    << " bodyTintSize=" << npcBodyTintWidth << "x" << npcBodyTintHeight
                    << " tintLayers=" << traits.mTintLayers.size();
            logWorldViewerActorLedger(mPtr, "fallout-facegen-textures", details.str());
        }

        const bool proofTargetActor = isFonvProofTargetActor(mPtr, traits);
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

        for (const ESM4::Race::BodyPart& bodyPart : (isFemale ? race->mBodyPartsFemale : race->mBodyPartsMale))
        {
            if (fonvRaceBodyPartCovered(bodyPart.mesh, coveredBodySlots))
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: skipped covered race body skin " << bodyPart.mesh << " for "
                                    << traits.mEditorId << " slots=0x" << std::hex << coveredBodySlots << std::dec;
                continue;
            }
            const std::string_view texture
                = !npcBodyTexture.empty() && isFonvRaceSkinSurface(bodyPart.mesh) ? npcBodyTexture : bodyPart.texture;
            osg::ref_ptr<osg::Node> attached = insertPart(bodyPart.mesh, nullptr, texture);
            if (attached != nullptr && isFonvRaceSkinSurface(bodyPart.mesh))
            {
                forceFalloutActorPartVisible(attached.get(), bodyPart.mesh, traits);
                if (npcBodyDetailIsTinyTint)
                    tintFalloutSkinMaterial(attached.get(), bodyPart.mesh, traits, npcBodyMaterialTint);
                if (!npcBodyDetailTexture.empty() && !npcBodyDetailIsTinyTint)
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: applying NPC body tint/detail " << npcBodyDetailTexture
                                     << " on " << bodyPart.mesh << " for " << traits.mEditorId;
                    overrideFalloutPartDetailTexture(npcBodyDetailTexture, mResourceSystem, *attached);
                }
                if (!npcBodyNormalTexture.empty())
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: overriding NPC part normal texture " << npcBodyNormalTexture
                                     << " on " << bodyPart.mesh << " for " << traits.mEditorId;
                    overrideFalloutPartNormalTexture(npcBodyNormalTexture, mResourceSystem, *attached);
                }
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
        const std::vector<ESM4::Race::BodyPart>& raceHeadParts = isFemale ? race->mHeadPartsFemale : race->mHeadParts;
        for (std::size_t i = 0; i < raceHeadParts.size(); ++i)
        {
            const ESM4::Race::BodyPart& headPart = raceHeadParts[i];
            const bool eyePart = isFonvEyePart(i);
            const bool headSurface = isFonvHeadSurfacePart(i);
            const std::string_view texture = eyePart && !eyeTexture.empty() ? eyeTexture
                                                                            : headPart.texture;
            osg::ref_ptr<osg::Node> attached = insertPart(headPart.mesh, nullptr, texture);
            if (i < 8)
            {
                raceFacePartAttached[i] = attached != nullptr;
                raceFacePartHasMesh[i] = !headPart.mesh.empty();
            }
            Log(Debug::Info) << "FNV/ESM4 diag: race face part " << getFonvRaceHeadPartRole(i)
                             << " index=" << i << " mesh=" << headPart.mesh << " texture="
                             << (texture.empty() ? std::string("<none>") : std::string(texture))
                             << " attached=" << (attached != nullptr) << " status="
                             << getFonvFacePartStatus(attached != nullptr, !headPart.mesh.empty()) << " for "
                             << traits.mEditorId;
            if (headSurface && attached != nullptr)
            {
                forceFalloutActorPartVisible(attached.get(), headPart.mesh, traits);
                applyFaceGenEgmMorph(mResourceSystem, attached.get(), headPart.mesh, traits);
                if (!texture.empty())
                    overrideFalloutPartDiffuseTexture(texture, mResourceSystem, *attached);
                if (!npcFaceDetailTexture.empty())
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: applying NPC face detail overlay " << npcFaceDetailTexture
                                     << " on " << headPart.mesh << " for " << traits.mEditorId;
                    overrideFalloutPartDetailTexture(npcFaceDetailTexture, mResourceSystem, *attached);
                }
                if (!npcFaceNormalTexture.empty())
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: overriding NPC part normal texture " << npcFaceNormalTexture
                                     << " on " << headPart.mesh << " for " << traits.mEditorId;
                    overrideFalloutPartNormalTexture(npcFaceNormalTexture, mResourceSystem, *attached);
                }
                DisableCullVisitor visitor;
                attached->accept(visitor);
                Log(Debug::Info) << "FNV/ESM4 diag: made head skin surface double-sided " << headPart.mesh
                                 << " for " << traits.mEditorId;
            }
            if (attached != nullptr)
                applyFalloutProofTriStaticMorph(mResourceSystem, attached.get(), headPart.mesh, traits);
            if (attached != nullptr)
                applyFalloutDialogueMorph(mResourceSystem, mPtr, this, attached.get(), headPart.mesh, traits);
            logFalloutFaceDrawableAudit(attached.get(), headPart.mesh, mPtr, "final-race-head");
        }

        std::set<uint32_t> usedHeadPartTypes;
        unsigned int insertedHeadParts = insertHeadParts(traits, traits.mHeadParts, usedHeadPartTypes);
        bool fallbackHairAttached = false;
        if (!traits.mHair.isZeroOrUnset() && usedHeadPartTypes.count(ESM4::HeadPart::Type_Hair) == 0)
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (const ESM4::Hair* hair = store->get<ESM4::Hair>().search(traits.mHair))
            {
                usedHeadPartTypes.insert(ESM4::HeadPart::Type_Hair);
                const osg::Vec4f hairTint = getHairTint(traits);
                Log(Debug::Info) << "FNV/ESM4 diag: inserting FONV NPC hair " << hair->mEditorId << " model="
                                 << hair->mModel << " tint=(" << hairTint.x() << ", " << hairTint.y() << ", "
                                 << hairTint.z() << ") for " << traits.mEditorId;
                osg::ref_ptr<osg::Node> attached = insertPart(hair->mModel, &hairTint);
                fallbackHairAttached = attached != nullptr;
                applyFaceGenEgmMorph(mResourceSystem, attached.get(), hair->mModel, traits);
                applyFalloutProofTriStaticMorph(mResourceSystem, attached.get(), hair->mModel, traits);
                if (attached != nullptr)
                    applyFalloutDialogueMorph(mResourceSystem, mPtr, this, attached.get(), hair->mModel, traits);
                ++insertedHeadParts;
            }
            else
                Log(Debug::Error) << "Hair not found: " << ESM::RefId(traits.mHair);
        }

        if (insertedHeadParts > 0)
            Log(Debug::Info) << "FNV/ESM4 diag: using " << insertedHeadParts
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
                         << (usedHeadPartTypes.count(ESM4::HeadPart::Type_Hair) != 0 || fallbackHairAttached ? "OK"
                                                                                                              : "MISSING")
                         << " facialHairType="
                         << (usedHeadPartTypes.count(ESM4::HeadPart::Type_FacialHair) != 0 ? "OK" : "UNKNOWN")
                         << " npcSpecificHeadParts=" << insertedHeadParts
                         << " faceTexture="
                         << (!npcFaceDetailTexture.empty() ? "RACE+DETAIL" : "RACE")
                         << " faceNormal=" << (!npcFaceNormalTexture.empty() ? "OK" : "RACE")
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

        for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(mPtr))
        {
            const std::string_view model = MWClass::ESM4Npc::chooseEquipmentModel(armor, isFemale);
            if (proofActor)
                Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: armor " << armor->mEditorId
                                 << " form=" << ESM::RefId(armor->mId) << " model=" << model;
            osg::ref_ptr<osg::Node> attached = insertPart(model);
            forceFalloutActorPartVisible(attached.get(), model, traits);
            overrideFalloutEquipmentSkinTextures(
                attached.get(), model, traits, mResourceSystem, npcBodyTexture, npcFaceTexture);
            applyFaceGenEgmMorph(mResourceSystem, attached.get(), model, traits);
            overrideFalloutEquipmentSkinTextures(
                attached.get(), model, traits, mResourceSystem, npcBodyTexture, npcFaceTexture);
        }
        for (const ESM4::Clothing* clothing : MWClass::ESM4Npc::getEquippedClothing(mPtr))
        {
            const std::string_view model = MWClass::ESM4Npc::chooseEquipmentModel(clothing, isFemale);
            if (proofActor)
                Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: clothing " << clothing->mEditorId
                                 << " form=" << ESM::RefId(clothing->mId) << " model=" << model;
            osg::ref_ptr<osg::Node> attached = insertPart(model);
            forceFalloutActorPartVisible(attached.get(), model, traits);
            overrideFalloutEquipmentSkinTextures(
                attached.get(), model, traits, mResourceSystem, npcBodyTexture, npcFaceTexture);
            applyFaceGenEgmMorph(mResourceSystem, attached.get(), model, traits);
            overrideFalloutEquipmentSkinTextures(
                attached.get(), model, traits, mResourceSystem, npcBodyTexture, npcFaceTexture);
        }

        if (const ESM4::Weapon* weapon = MWClass::ESM4Npc::getEquippedWeapon(mPtr))
        {
            if (proofTargetActor && std::getenv("OPENMW_FNV_PROOF_HIDE_EQUIPPED_WEAPON") != nullptr)
            {
                Log(Debug::Info) << "FNV/ESM4 proof: skipped equipped weapon for clean dialogue proof on "
                                 << traits.mEditorId;
                if (proofActor)
                    Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: END parts assembled";
                return;
            }
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            osg::ref_ptr<osg::Node> attached = insertAttachedPart(weapon->mModel, "Weapon");
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

        if (proofActor)
            Log(Debug::Info) << "FNV/ESM4 ASSET PROOF GSEasyPete: END parts assembled";
    }

    unsigned int ESM4NpcAnimation::insertHeadParts(
        const ESM4::Npc& traits, const std::vector<ESM::FormId>& partIds, std::set<uint32_t>& usedHeadPartTypes)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
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
            if (miscPart || usedHeadPartTypes.emplace(part->mType).second)
            {
                const osg::Vec4f hairTint = getHairTint(traits);
                const osg::Vec4f* tint = miscPart || part->mType == ESM4::HeadPart::Type_Hair
                    || part->mType == ESM4::HeadPart::Type_FacialHair
                    || part->mType == ESM4::HeadPart::Type_Eyebrows
                    ? &hairTint
                    : nullptr;
                Log(Debug::Info) << "FNV/ESM4 diag: inserting NPC head part " << part->mEditorId << " type="
                                 << part->mType << " model=" << part->mModel << " for "
                                 << mPtr.getCellRef().getRefId();
                osg::ref_ptr<osg::Node> attached = insertPart(part->mModel, tint);
                applyFaceGenEgmMorph(mResourceSystem, attached.get(), part->mModel, traits);
                applyFalloutProofTriStaticMorph(mResourceSystem, attached.get(), part->mModel, traits);
                if (attached != nullptr)
                    applyFalloutDialogueMorph(mResourceSystem, mPtr, this, attached.get(), part->mModel, traits);
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
                    osg::ref_ptr<osg::Node> extraAttached = insertPart(extraPart->mModel, tint);
                    applyFaceGenEgmMorph(mResourceSystem, extraAttached.get(), extraPart->mModel, traits);
                    applyFalloutProofTriStaticMorph(mResourceSystem, extraAttached.get(), extraPart->mModel, traits);
                    if (extraAttached != nullptr)
                        applyFalloutDialogueMorph(
                            mResourceSystem, mPtr, this, extraAttached.get(), extraPart->mModel, traits);
                    if (extraAttached != nullptr && std::getenv("OPENMW_FNV_PROOF_MOUTH_DRIVER") != nullptr
                        && isFalloutMouthDriverPart(extraPart->mModel))
                        applyFalloutProofTriOpenMorph(
                            mResourceSystem, mPtr, extraAttached.get(), extraPart->mModel, traits);
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
            if (skipSkinWhenClothed && hasNonSkinArmor && isSkinNakedArmor(armor))
            {
                std::ostringstream details;
                details << "armor=\"" << armor->mEditorId << "\""
                        << " armorName=\"" << armor->mFullName << "\""
                        << " result=\"skip-skin-when-clothed\""
                        << " nonSkinArmor=1";
                logWorldViewerActorLedger(mPtr, "tes5-armor-skip", details.str());
                return;
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

                {
                    std::ostringstream details;
                    details << "armor=\"" << armor->mEditorId << "\""
                            << " addon=" << ESM::RefId(armaId)
                            << " resolvedAddon=" << ESM::RefId(resolvedArmaId)
                            << " addonEditor=\"" << arma->mEditorId << "\""
                            << " compatibleRace=" << compatibleRace
                            << " primaryRace=" << ESM::RefId(arma->mRacePrimary)
                            << " model=\"" << (isFemale ? arma->mModelFemale : arma->mModelMale) << "\""
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
                insertPart(addonModel);
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
                insertPart(addonModel);
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
        if (usedParts & ESM4::Armor::TES5_Hair)
            usedHeadPartTypes.insert(ESM4::HeadPart::Type_Hair);
        insertHeadParts(traits, traits.mHeadParts, usedHeadPartTypes);
        insertHeadParts(traits, isFemale ? race->mHeadPartIdsFemale : race->mHeadPartIdsMale, usedHeadPartTypes);
    }
}
