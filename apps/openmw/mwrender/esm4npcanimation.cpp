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

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/attach.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwmechanics/character.hpp"

#include <osg/ComputeBoundsVisitor>
#include <osg/FrontFace>
#include <osg/Geode>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/NodeCallback>
#include <osg/NodeVisitor>
#include <osg/PositionAttitudeTransform>
#include <osg/Texture2D>
#include <osgAnimation/Bone>
#include <osgAnimation/UpdateBone>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <istream>
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

namespace MWRender
{
    namespace
    {
        class TintMaterialVisitor : public osg::NodeVisitor
        {
        public:
            TintMaterialVisitor(const osg::Vec4f& tint, float emissionStrength = 0.f)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mTint(tint)
                , mEmissionStrength(emissionStrength)
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
                    for (osg::Vec4f& color : *colors)
                    {
                        color.x() *= mTint.x();
                        color.y() *= mTint.y();
                        color.z() *= mTint.z();
                    }
                    colors->dirty();
                    return true;
                }

                if (osg::Vec4ubArray* colors = dynamic_cast<osg::Vec4ubArray*>(existingColors))
                {
                    for (osg::Vec4ub& color : *colors)
                    {
                        color.r() = static_cast<unsigned char>(std::clamp(color.r() * mTint.x(), 0.f, 255.f));
                        color.g() = static_cast<unsigned char>(std::clamp(color.g() * mTint.y(), 0.f, 255.f));
                        color.b() = static_cast<unsigned char>(std::clamp(color.b() * mTint.z(), 0.f, 255.f));
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
            if (Misc::StringUtils::ciEqual(traits.mEditorId, "GSEasyPete"))
                return osg::Vec4f(0.96f, 0.96f, 0.92f, 1.f);
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

            Log(Debug::Info) << "FNV/ESM4 diag: using exported NPC FaceGen diffuse " << texture << " for "
                             << traits.mEditorId << " "
                             << (image != nullptr ? std::to_string(image->s()) + "x" + std::to_string(image->t())
                                                  : std::string("<unloaded>"));
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

        bool applyFalloutDialogueMorph(
            Resource::ResourceSystem* resourceSystem, Animation* animation, osg::Node* attached,
            std::string_view model, const ESM4::Npc& traits)
        {
            if (attached == nullptr)
                return false;

            const std::shared_ptr<const FaceGenTri> tri = loadFaceGenTri(resourceSystem, model);
            if (!tri)
                return false;

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
                return osg::Vec3f(readFalloutProofFloat("OPENMW_FNV_BROW_OFFSET_X", -0.12f),
                    readFalloutProofFloat("OPENMW_FNV_BROW_OFFSET_Y", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_BROW_OFFSET_Z", 0.f));
            if (lowered.find("eye") != std::string::npos)
                return osg::Vec3f(readFalloutProofFloat("OPENMW_FNV_EYE_OFFSET_X", -0.18f),
                    readFalloutProofFloat("OPENMW_FNV_EYE_OFFSET_Y", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_EYE_OFFSET_Z", 0.f));
            if (lowered.find("beard") != std::string::npos)
                return osg::Vec3f(readFalloutProofFloat("OPENMW_FNV_BEARD_OFFSET_X", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_BEARD_OFFSET_Y", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_BEARD_OFFSET_Z", 0.f));
            if (lowered.find("mouth") != std::string::npos || lowered.find("teeth") != std::string::npos
                || lowered.find("tongue") != std::string::npos)
                return osg::Vec3f(readFalloutProofFloat("OPENMW_FNV_MOUTH_OFFSET_X", -0.35f),
                    readFalloutProofFloat("OPENMW_FNV_MOUTH_OFFSET_Y", 0.f),
                    readFalloutProofFloat("OPENMW_FNV_MOUTH_OFFSET_Z", 0.f));
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

        osg::Quat getFalloutHeadFrameSurfaceAttitude(std::string_view model, bool headgearStaticPart)
        {
            const std::string prefix = getFalloutHeadFrameSurfacePrefix(model, headgearStaticPart);
            if (prefix.empty())
                return osg::Quat();

            constexpr float degreesToRadians = 0.017453292519943295f;
            const float xDegrees = readFalloutProofFloat((prefix + "_ROTATION_X").c_str(), 0.f);
            const float yDegrees = readFalloutProofFloat((prefix + "_ROTATION_Y").c_str(), 0.f);
            const bool faceInternal = prefix == "OPENMW_FNV_EYE" || prefix == "OPENMW_FNV_MOUTH"
                || prefix == "OPENMW_FNV_BEARD" || prefix == "OPENMW_FNV_BROW";
            const float zFallback = (faceInternal || prefix == "OPENMW_FNV_HAIR") ? -90.f : 0.f;
            const float zDegrees = readFalloutProofFloat((prefix + "_ROTATION_Z").c_str(), zFallback);
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

    void ESM4NpcAnimation::updateParts()
    {
        if (mObjectRoot == nullptr)
            return;
        const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(mPtr);
        if (traits == nullptr)
            return;
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
            && std::getenv("OPENMW_FNV_STATICIZE_RIGGED_HAND_PARTS") != nullptr
            && std::getenv("OPENMW_FNV_KEEP_RIGGED_HAND_PARTS") == nullptr;
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
            attached = attachStaticizedFalloutPart(attachTemplateNode, attachNode, mResourceSystem->getSceneManager(),
                !staticizedBareHandPartRig);
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
        if (attached != nullptr && headAttachedStaticPart)
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
            const float emissionStrength = isFalloutHairTintModel(correctedModel.value()) ? 0.65f : 0.f;
            TintMaterialVisitor visitor(*tint, emissionStrength);
            attached->accept(visitor);
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
        const std::string npcFaceTexture = findFonvNpcFaceTexture(mResourceSystem, traits);
        const std::string npcFaceDetailTexture
            = npcFaceTexture.empty() ? findFonvNpcFaceDetailTexture(mResourceSystem, traits) : std::string();
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
                neutralizeFalloutSkinMaterial(attached.get(), bodyPart.mesh, traits);
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
                if (std::getenv("OPENMW_FNV_APPLY_FACEGEN_DETAIL") != nullptr && !npcFaceTexture.empty())
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: applying NPC face detail overlay " << npcFaceTexture
                                     << " on " << headPart.mesh << " for " << traits.mEditorId;
                    overrideFalloutPartDetailTexture(npcFaceTexture, mResourceSystem, *attached);
                }
                if (!npcFaceNormalTexture.empty())
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: overriding NPC part normal texture " << npcFaceNormalTexture
                                     << " on " << headPart.mesh << " for " << traits.mEditorId;
                    overrideFalloutPartNormalTexture(npcFaceNormalTexture, mResourceSystem, *attached);
                }
                neutralizeFalloutSkinMaterial(attached.get(), headPart.mesh, traits);
            }
            if (attached != nullptr)
                applyFalloutProofTriStaticMorph(mResourceSystem, attached.get(), headPart.mesh, traits);
            if (attached != nullptr)
                applyFalloutDialogueMorph(mResourceSystem, this, attached.get(), headPart.mesh, traits);
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
                    applyFalloutDialogueMorph(mResourceSystem, this, attached.get(), hair->mModel, traits);
                if (attached != nullptr)
                {
                    TintMaterialVisitor visitor(hairTint, 0.65f);
                    attached->accept(visitor);
                }
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
                         << (!npcFaceTexture.empty() ? "OK" : "RACE")
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
            const bool miscPart = traits.mIsFONV && isFonvMiscHeadPart(*part);
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
                    applyFalloutDialogueMorph(mResourceSystem, this, attached.get(), part->mModel, traits);
                if (attached != nullptr && tint != nullptr)
                {
                    const float emissionStrength = isFalloutHairTintModel(part->mModel) ? 0.65f : 0.f;
                    TintMaterialVisitor visitor(*tint, emissionStrength);
                    attached->accept(visitor);
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
                    osg::ref_ptr<osg::Node> extraAttached = insertPart(extraPart->mModel, tint);
                    applyFaceGenEgmMorph(mResourceSystem, extraAttached.get(), extraPart->mModel, traits);
                    applyFalloutProofTriStaticMorph(mResourceSystem, extraAttached.get(), extraPart->mModel, traits);
                    if (extraAttached != nullptr)
                        applyFalloutDialogueMorph(mResourceSystem, this, extraAttached.get(), extraPart->mModel, traits);
                    if (extraAttached != nullptr && std::getenv("OPENMW_FNV_PROOF_MOUTH_DRIVER") != nullptr
                        && isFalloutMouthDriverPart(extraPart->mModel))
                        applyFalloutProofTriOpenMorph(
                            mResourceSystem, mPtr, extraAttached.get(), extraPart->mModel, traits);
                    if (extraAttached != nullptr && tint != nullptr)
                    {
                        const float emissionStrength = isFalloutHairTintModel(extraPart->mModel) ? 0.65f : 0.f;
                        TintMaterialVisitor visitor(*tint, emissionStrength);
                        extraAttached->accept(visitor);
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
