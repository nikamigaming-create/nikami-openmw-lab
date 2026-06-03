#include "creatureanimation.hpp"

#include <osg/ComputeBoundsVisitor>
#include <osg/MatrixTransform>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <components/debug/debuglog.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm4/loadbptd.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadlvlc.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

#include <vector>

#include "../mwmechanics/weapontype.hpp"

#include "../mwbase/environment.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"

namespace MWRender
{
    namespace
    {
        bool hasSuffix(std::string_view value, std::string_view suffix)
        {
            return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
        }

        std::string toLowerAscii(std::string_view value)
        {
            std::string result(value);
            std::transform(result.begin(), result.end(), result.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return result;
        }

        bool isLikelySkeletonNif(std::string_view value)
        {
            const std::string lowered = toLowerAscii(value);
            return lowered.ends_with("skeleton.nif") || lowered.find("/skeleton") != std::string::npos
                || lowered.find("\\skeleton") != std::string::npos;
        }

        bool findCreatureKf(const VFS::Manager& vfs, const std::string& path, std::string& normalizedPath)
        {
            VFS::Path::Normalized normalized(path);
            if (!vfs.exists(normalized))
                return false;

            normalizedPath = normalized.value();
            return true;
        }

        std::vector<std::string> collectDiscoveredCreatureKfs(
            const VFS::Manager& vfs, std::string_view directory, std::string_view probeToken, const std::string& editorId)
        {
            std::vector<std::string> paths;
            unsigned int logged = 0;
            const bool logCandidates = std::getenv("OPENMW_FNV_CREATURE_KF_DIAG") != nullptr;
            for (const VFS::Path::Normalized& name : vfs.getRecursiveDirectoryIterator(directory))
            {
                const std::string_view value = name.view();
                if (!hasSuffix(value, ".kf"))
                    continue;

                paths.push_back(name.value());

                if (logCandidates && logged < 24)
                {
                    Log(Debug::Info) << "FNV/ESM4 diag: creature KF candidate " << editorId << " path=" << name;
                    ++logged;
                }
            }

            if (paths.empty() && !probeToken.empty())
            {
                for (const VFS::Path::Normalized& name : vfs.getRecursiveDirectoryIterator())
                {
                    const std::string_view value = name.view();
                    if (value.find(probeToken) == std::string_view::npos || !hasSuffix(value, ".kf"))
                        continue;

                    paths.push_back(name.value());

                    if (logCandidates && logged < 24)
                    {
                        Log(Debug::Info) << "FNV/ESM4 diag: creature KF global candidate " << editorId
                                         << " path=" << name;
                        ++logged;
                    }
                }
            }

            return paths;
        }

        const ESM4::Creature* searchCreatureTemplate(ESM::FormId id, int depth = 0)
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr || id.isZeroOrUnset() || depth > 8)
                return nullptr;

            const ESM::RecNameInts foundType = static_cast<ESM::RecNameInts>(store->find(id));
            if (foundType == ESM::RecNameInts::REC_CREA4)
                return store->get<ESM4::Creature>().search(id);

            if (foundType != ESM::RecNameInts::REC_LVLC4)
                return nullptr;

            const ESM4::LevelledCreature* list = store->get<ESM4::LevelledCreature>().search(id);
            if (list == nullptr || list->mLvlObject.empty())
                return nullptr;

            const ESM4::LVLO* selected = nullptr;
            for (const ESM4::LVLO& entry : list->mLvlObject)
            {
                if (entry.item == 0)
                    continue;
                if (selected == nullptr || entry.level <= 1)
                    selected = &entry;
                if (entry.level <= 1)
                    break;
            }

            return selected == nullptr ? nullptr : searchCreatureTemplate(ESM::FormId::fromUint32(selected->item), depth + 1);
        }

        const ESM4::Creature& getEffectiveCreatureForRendering(const ESM4::Creature& creature)
        {
            const ESM4::Creature* current = &creature;
            for (int depth = 0; depth < 8; ++depth)
            {
                if (!current->mModel.empty() || !current->mNif.empty() || !current->mBodyParts.empty())
                    return *current;
                if (current->mBaseTemplate.isZeroOrUnset())
                    return *current;

                const ESM4::Creature* templated = searchCreatureTemplate(current->mBaseTemplate);
                if (templated == nullptr || templated == current)
                    return *current;

                current = templated;
            }

            return *current;
        }

        bool appendUniquePath(std::vector<std::string>& paths, const std::string& path)
        {
            if (path.empty() || std::find(paths.begin(), paths.end(), path) != paths.end())
                return false;

            paths.push_back(path);
            return true;
        }

        VFS::Path::Normalized correctCreatureBodyPath(const std::string& path)
        {
            VFS::Path::Normalized normalized(path);
            if (normalized.value().starts_with("meshes/"))
                return normalized;
            return Misc::ResourceHelpers::correctMeshPath(normalized);
        }

        void appendDirectoryBodyNifs(const VFS::Manager& vfs, std::string_view directory, std::string_view editorId,
            std::vector<std::string>& paths)
        {
            std::vector<std::string> candidates;
            for (const VFS::Path::Normalized& name : vfs.getRecursiveDirectoryIterator(directory))
            {
                const std::string_view value = name.view();
                if (!hasSuffix(value, ".nif") || isLikelySkeletonNif(value))
                    continue;

                candidates.push_back(name.value());
            }

            const std::string loweredEditor = toLowerAscii(editorId);
            auto appendMatching = [&](std::string_view token) {
                for (const std::string& candidate : candidates)
                {
                    const std::string loweredCandidate = toLowerAscii(candidate);
                    if (loweredCandidate.find(token) != std::string::npos)
                        appendUniquePath(paths, candidate);
                }
            };

            if (loweredEditor.find("coyote") != std::string::npos)
                appendMatching("coyote.nif");
            else if (loweredEditor.find("mongrel") != std::string::npos)
                appendMatching("nv_mongrel.nif");
            else if (loweredEditor.find("dog") != std::string::npos)
                appendMatching("dogskin.nif");

            if (!paths.empty())
                return;

            for (const std::string& candidate : candidates)
            {
                const std::string loweredCandidate = toLowerAscii(candidate);
                if (loweredCandidate.find("skullcap") != std::string::npos
                    || loweredCandidate.find("eyes") != std::string::npos
                    || loweredCandidate.find("rex") != std::string::npos
                    || loweredCandidate.find("cyberdog") != std::string::npos
                    || loweredCandidate.find("static") != std::string::npos)
                    continue;

                appendUniquePath(paths, candidate);
                return;
            }
        }

        std::vector<std::string> collectCreatureBodyNifs(
            const ESM4::Creature& creature, const VFS::Manager& vfs, std::string_view animationDirectory)
        {
            std::vector<std::string> paths;
            for (const std::string& bodyNif : creature.mNif)
            {
                if (!isLikelySkeletonNif(bodyNif))
                    appendUniquePath(paths, bodyNif);
            }

            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr)
                return paths;

            const auto& bodyPartStore = store->get<ESM4::BodyPartData>();
            for (ESM::FormId bodyPartId : creature.mBodyParts)
            {
                const ESM4::BodyPartData* bodyPartData = bodyPartStore.search(bodyPartId);
                if (bodyPartData == nullptr)
                    continue;

                if (!isLikelySkeletonNif(bodyPartData->mModel))
                    appendUniquePath(paths, bodyPartData->mModel);
            }

            if (paths.empty())
                appendDirectoryBodyNifs(vfs, animationDirectory, creature.mEditorId, paths);

            return paths;
        }

        osg::Vec3f boundingBoxExtent(const osg::BoundingBox& box)
        {
            return osg::Vec3f(box.xMax() - box.xMin(), box.yMax() - box.yMin(), box.zMax() - box.zMin());
        }

        void logCreatureBounds(std::string_view label, const std::string& editorId, const std::string& path,
            osg::Node& node)
        {
            osg::ComputeBoundsVisitor boundsVisitor;
            node.accept(boundsVisitor);
            const osg::BoundingBox box = boundsVisitor.getBoundingBox();
            if (!box.valid())
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: creature " << label << " bounds invalid for "
                                    << editorId << " path=" << path;
                return;
            }

            const osg::Vec3f center = box.center();
            const osg::Vec3f extent = boundingBoxExtent(box);
            Log(Debug::Info) << "FNV/ESM4 diag: creature " << label << " bounds for " << editorId
                             << " path=" << path << " center=(" << center.x() << "," << center.y() << ","
                             << center.z() << ") extent=(" << extent.x() << "," << extent.y() << ","
                             << extent.z() << ")";
        }

        class ForceFalloutCreatureBodyVisibleVisitor : public osg::NodeVisitor
        {
        public:
            ForceFalloutCreatureBodyVisibleVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                node.setNodeMask(~0u);
                traverse(node);
            }

            void apply(osg::Drawable& drawable) override
            {
                drawable.setNodeMask(~0u);
                drawable.setCullingActive(false);

                if (dynamic_cast<SceneUtil::RigGeometry*>(&drawable) != nullptr)
                    ++mRigGeometryCount;
                else if (dynamic_cast<osg::Geometry*>(&drawable) != nullptr)
                    ++mStaticGeometryCount;
                else
                    ++mOtherDrawableCount;

                for (osg::Node* node : getNodePath())
                {
                    if (node != nullptr)
                        node->setNodeMask(~0u);
                }
            }

            unsigned int mRigGeometryCount = 0;
            unsigned int mStaticGeometryCount = 0;
            unsigned int mOtherDrawableCount = 0;
        };

        void forceFalloutCreatureBodyVisible(
            osg::Node* bodyNode, const std::string& editorId, const VFS::Path::Normalized& bodyPath)
        {
            if (bodyNode == nullptr)
                return;

            ForceFalloutCreatureBodyVisibleVisitor visitor;
            bodyNode->accept(visitor);
            Log(Debug::Info) << "FNV/ESM4 diag: forced creature body render mask for " << editorId
                             << " path=" << bodyPath
                             << " rigged=" << visitor.mRigGeometryCount
                             << " static=" << visitor.mStaticGeometryCount
                             << " other=" << visitor.mOtherDrawableCount;
        }
    }

    CreatureAnimation::CreatureAnimation(
        const MWWorld::Ptr& ptr, const std::string& model, Resource::ResourceSystem* resourceSystem, bool animated)
        : ActorAnimation(ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), resourceSystem)
    {
        if (!model.empty())
        {
            setObjectRoot(model, false, false, true);

            if (mPtr.getType() == ESM::Creature::sRecordId)
            {
                MWWorld::LiveCellRef<ESM::Creature>* ref = mPtr.get<ESM::Creature>();
                if ((ref->mBase->mFlags & ESM::Creature::Bipedal))
                    addAnimSource(Settings::models().mXbaseanim.get(), model);

                if (animated)
                    addAnimSource(model, model);
            }
            else if (mPtr.getType() == ESM4::Creature::sRecordId)
            {
                MWWorld::LiveCellRef<ESM4::Creature>* ref = mPtr.get<ESM4::Creature>();
                const ESM4::Creature& effective = getEffectiveCreatureForRendering(*ref->mBase);
                addAnimSource(model, model);

                std::string animationDirectory = model;
                const std::size_t slash = animationDirectory.find_last_of("/\\");
                animationDirectory = slash == std::string::npos ? std::string() : animationDirectory.substr(0, slash + 1);
                unsigned int attachedBodyNifs = 0;
                const VFS::Manager* vfs = resourceSystem->getVFS();
                const std::vector<std::string> bodyNifs = collectCreatureBodyNifs(effective, *vfs, animationDirectory);
                for (const std::string& bodyNif : bodyNifs)
                {
                    if (bodyNif.empty())
                        continue;
                    const VFS::Path::Normalized bodyPath = correctCreatureBodyPath(bodyNif);
                    osg::ref_ptr<osg::Node> bodyNode
                        = resourceSystem->getSceneManager()->getInstance(bodyPath, mObjectRoot);
                    forceFalloutCreatureBodyVisible(bodyNode, ref->mBase->mEditorId, bodyPath);
                    mObjectRoot->addChild(bodyNode);
                    if (std::getenv("OPENMW_FNV_CREATURE_BODY_DIAG") != nullptr)
                    {
                        Log(Debug::Info) << "FNV/ESM4 diag: attached creature body nif "
                                         << ref->mBase->mEditorId << " effective=" << effective.mEditorId
                                         << " path=" << bodyPath;
                        logCreatureBounds("body", ref->mBase->mEditorId, bodyPath.value(), *bodyNode);
                    }
                    ++attachedBodyNifs;
                }
                if (std::getenv("OPENMW_FNV_CREATURE_BODY_DIAG") != nullptr && mObjectRoot != nullptr)
                    logCreatureBounds("root", ref->mBase->mEditorId, model, *mObjectRoot);
                unsigned int fallbackKfs = 0;
                for (const std::string& kf : effective.mKf)
                {
                    if (!kf.empty())
                        addAnimSource(animationDirectory + kf, model);
                }

                static constexpr std::string_view fallbackNames[] = {
                    "skeleton.kf",
                    "idle.kf",
                    "forward.kf",
                    "backward.kf",
                    "left.kf",
                    "right.kf",
                    "walkforward.kf",
                    "runforward.kf",
                    "attackleft.kf",
                    "attackright.kf",
                    "attack1.kf",
                };
                for (std::string_view fallback : fallbackNames)
                {
                    std::string path = animationDirectory + std::string(fallback);
                    std::string normalizedPath;
                    if (findCreatureKf(*vfs, path, normalizedPath))
                    {
                        addAnimSource(normalizedPath, model);
                        ++fallbackKfs;
                    }
                }

                std::string normalizedDirectory = animationDirectory;
                VFS::Path::normalizeFilenameInPlace(normalizedDirectory);
                std::string probeToken = normalizedDirectory;
                if (probeToken.ends_with('/'))
                    probeToken.pop_back();
                const std::vector<std::string> discoveredKfPaths
                    = collectDiscoveredCreatureKfs(*vfs, normalizedDirectory, probeToken, effective.mEditorId);
                for (const std::string& path : discoveredKfPaths)
                    addAnimSource(path, model);

                Log(Debug::Info) << "FNV/ESM4 diag: inserted creature animation for "
                                 << ref->mBase->mEditorId << " model=" << model
                                 << " animated=" << animated << " effective=" << effective.mEditorId
                                 << " kfCount=" << effective.mKf.size()
                                 << " bodyPartCount=" << effective.mBodyParts.size()
                                 << " attachedBodyNifs=" << attachedBodyNifs
                                 << " fallbackKfs=" << fallbackKfs
                                 << " discoveredKfs=" << discoveredKfPaths.size();
            }
        }
    }

    CreatureWeaponAnimation::CreatureWeaponAnimation(
        const MWWorld::Ptr& ptr, const std::string& model, Resource::ResourceSystem* resourceSystem, bool animated)
        : ActorAnimation(ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), resourceSystem)
        , mShowWeapons(false)
        , mShowCarriedLeft(false)
    {
        MWWorld::LiveCellRef<ESM::Creature>* ref = mPtr.get<ESM::Creature>();

        if (!model.empty())
        {
            setObjectRoot(model, true, false, true);

            if ((ref->mBase->mFlags & ESM::Creature::Bipedal))
                addAnimSource(Settings::models().mXbaseanim.get(), model);

            if (animated)
                addAnimSource(model, model);

            mPtr.getClass().getInventoryStore(mPtr).setInvListener(this);

            updateParts();
        }

        mWeaponAnimationTime = std::make_shared<WeaponAnimationTime>(this);
    }

    void CreatureWeaponAnimation::showWeapons(bool showWeapon)
    {
        if (showWeapon != mShowWeapons)
        {
            mShowWeapons = showWeapon;
            updateParts();
        }
    }

    void CreatureWeaponAnimation::showCarriedLeft(bool show)
    {
        if (show != mShowCarriedLeft)
        {
            mShowCarriedLeft = show;
            updateParts();
        }
    }

    void CreatureWeaponAnimation::updateParts()
    {
        mAmmunition.reset();
        mWeapon.reset();
        mShield.reset();

        updateHolsteredWeapon(!mShowWeapons);
        updateQuiver();
        updateHolsteredShield(mShowCarriedLeft);

        if (mShowWeapons)
            updatePart(mWeapon, MWWorld::InventoryStore::Slot_CarriedRight);
        if (mShowCarriedLeft)
            updatePart(mShield, MWWorld::InventoryStore::Slot_CarriedLeft);
    }

    void CreatureWeaponAnimation::updatePart(PartHolderPtr& scene, int slot)
    {
        if (!mObjectRoot)
            return;

        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        MWWorld::ConstContainerStoreIterator it = inv.getSlot(slot);

        if (it == inv.end())
        {
            scene.reset();
            return;
        }
        MWWorld::ConstPtr item = *it;

        std::string_view bonename;
        VFS::Path::Normalized itemModel = item.getClass().getCorrectedModel(item);
        if (slot == MWWorld::InventoryStore::Slot_CarriedRight)
        {
            if (item.getType() == ESM::Weapon::sRecordId)
            {
                int type = item.get<ESM::Weapon>()->mBase->mData.mType;
                bonename = MWMechanics::getWeaponType(type)->mAttachBone;
                if (bonename != "Weapon Bone")
                {
                    const NodeMap& nodeMap = getNodeMap();
                    NodeMap::const_iterator found = nodeMap.find(bonename);
                    if (found == nodeMap.end())
                        bonename = "Weapon Bone";
                }
            }
            else
                bonename = "Weapon Bone";
        }
        else
        {
            bonename = "Shield Bone";
            if (item.getType() == ESM::Armor::sRecordId)
            {
                itemModel = getShieldMesh(item, false);
            }
        }

        try
        {
            osg::ref_ptr<osg::Node> attached
                = attach(itemModel, bonename, bonename, item.getType() == ESM::Light::sRecordId);

            scene = std::make_unique<PartHolder>(attached);

            if (!item.getClass().getEnchantment(item).empty())
                mGlowUpdater
                    = SceneUtil::addEnchantedGlow(attached, mResourceSystem, item.getClass().getEnchantmentColor(item));

            // Crossbows start out with a bolt attached
            // FIXME: code duplicated from NpcAnimation
            if (slot == MWWorld::InventoryStore::Slot_CarriedRight && item.getType() == ESM::Weapon::sRecordId
                && item.get<ESM::Weapon>()->mBase->mData.mType == ESM::Weapon::MarksmanCrossbow)
            {
                const ESM::WeaponType* weaponInfo = MWMechanics::getWeaponType(ESM::Weapon::MarksmanCrossbow);
                MWWorld::ConstContainerStoreIterator ammo = inv.getSlot(MWWorld::InventoryStore::Slot_Ammunition);
                if (ammo != inv.end() && ammo->get<ESM::Weapon>()->mBase->mData.mType == weaponInfo->mAmmoType)
                    attachArrow();
                else
                    mAmmunition.reset();
            }
            else
                mAmmunition.reset();

            std::shared_ptr<SceneUtil::ControllerSource> source;

            if (slot == MWWorld::InventoryStore::Slot_CarriedRight)
                source = mWeaponAnimationTime;
            else
                source = mAnimationTimePtr[0];

            SceneUtil::AssignControllerSourcesVisitor assignVisitor(std::move(source));
            attached->accept(assignVisitor);

            if (item.getType() == ESM::Light::sRecordId)
                addExtraLight(scene->getNode()->asGroup(), SceneUtil::LightCommon(*item.get<ESM::Light>()->mBase));
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "Can not add creature part: " << e.what();
        }
    }

    bool CreatureWeaponAnimation::isArrowAttached() const
    {
        return mAmmunition != nullptr;
    }

    void CreatureWeaponAnimation::detachArrow()
    {
        WeaponAnimation::detachArrow(mPtr);
        updateQuiver();
    }

    void CreatureWeaponAnimation::attachArrow()
    {
        WeaponAnimation::attachArrow(mPtr);

        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        MWWorld::ConstContainerStoreIterator ammo = inv.getSlot(MWWorld::InventoryStore::Slot_Ammunition);
        if (ammo != inv.end() && !ammo->getClass().getEnchantment(*ammo).empty())
        {
            osg::Group* bone = getArrowBone();
            if (bone != nullptr && bone->getNumChildren())
                SceneUtil::addEnchantedGlow(
                    bone->getChild(0), mResourceSystem, ammo->getClass().getEnchantmentColor(*ammo));
        }

        updateQuiver();
    }

    void CreatureWeaponAnimation::releaseArrow(float attackStrength)
    {
        WeaponAnimation::releaseArrow(mPtr, attackStrength);
        updateQuiver();
    }

    osg::Group* CreatureWeaponAnimation::getArrowBone()
    {
        if (!mWeapon)
            return nullptr;

        if (!mPtr.getClass().hasInventoryStore(mPtr))
            return nullptr;

        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        MWWorld::ConstContainerStoreIterator weapon = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        if (weapon == inv.end() || weapon->getType() != ESM::Weapon::sRecordId)
            return nullptr;

        int type = weapon->get<ESM::Weapon>()->mBase->mData.mType;
        int ammoType = MWMechanics::getWeaponType(type)->mAmmoType;
        if (ammoType == ESM::Weapon::None)
            return nullptr;

        // Try to find and attachment bone in actor's skeleton, otherwise fall back to the ArrowBone in weapon's mesh
        osg::Group* bone = getBoneByName(MWMechanics::getWeaponType(ammoType)->mAttachBone);
        if (bone == nullptr)
        {
            SceneUtil::FindByNameVisitor findVisitor("ArrowBone");
            mWeapon->getNode()->accept(findVisitor);
            bone = findVisitor.mFoundNode;
        }
        return bone;
    }

    osg::Node* CreatureWeaponAnimation::getWeaponNode()
    {
        return mWeapon ? mWeapon->getNode().get() : nullptr;
    }

    Resource::ResourceSystem* CreatureWeaponAnimation::getResourceSystem()
    {
        return mResourceSystem;
    }

    void CreatureWeaponAnimation::addControllers()
    {
        Animation::addControllers();
        if (mObjectRoot)
            WeaponAnimation::addControllers(mNodeMap, mActiveControllers, mObjectRoot.get());
    }

    osg::Vec3f CreatureWeaponAnimation::runAnimation(float duration)
    {
        osg::Vec3f ret = Animation::runAnimation(duration);

        WeaponAnimation::configureControllers(mPtr.getRefData().getPosition().rot[0] + getBodyPitchRadians());

        return ret;
    }

}
