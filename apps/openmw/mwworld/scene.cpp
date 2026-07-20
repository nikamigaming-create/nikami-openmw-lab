#include "scene.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <string_view>

#include <BulletCollision/CollisionDispatch/btCollisionObject.h>

#include <components/debug/debuglog.hpp>
#include <components/detournavigator/agentbounds.hpp>
#include <components/detournavigator/debug.hpp>
#include <components/detournavigator/heightfieldshape.hpp>
#include <components/detournavigator/navigator.hpp>
#include <components/detournavigator/updateguard.hpp>
#include <components/esm/esmterrain.hpp>
#include <components/esm/records.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadpack.hpp>
#include <components/esm4/script.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/node.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/terraingrid.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwclass/esm4npc.hpp"
#include "../mwclass/fnvfurniturelifecycle.hpp"

#include "../mwrender/landmanager.hpp"
#include "../mwrender/postprocessor.hpp"
#include "../mwrender/renderingmanager.hpp"

#include "../mwphysics/actor.hpp"
#include "../mwphysics/heightfield.hpp"
#include "../mwphysics/object.hpp"
#include "../mwphysics/physicssystem.hpp"

#include "../mwworld/actionteleport.hpp"

#include "actorfacing.hpp"
#include "cellpreloader.hpp"
#include "cellstore.hpp"
#include "cellvisitors.hpp"
#include "class.hpp"
#include "esmstore.hpp"
#include "localscripts.hpp"
#include "player.hpp"
#include "worldimp.hpp"
#include "worldmodel.hpp"

namespace
{
    using MWWorld::RotationOrder;

    osg::Quat makeActorOsgQuat(const MWWorld::Ptr& ptr)
    {
        const ESM::Position& position = ptr.getRefData().getPosition();
        bool tes4Npc = false;
        if (ptr.getType() == ESM::REC_NPC_4)
        {
            const MWWorld::LiveCellRef<ESM4::Npc>* npc = ptr.get<ESM4::Npc>();
            tes4Npc = npc != nullptr && npc->mBase != nullptr && npc->mBase->mIsTES4;
        }
        // TES4 NPC meshes retain the legacy quarter-turn conversion. FO3/FNV NPCs and CREA4 rigs author their
        // visual front on the same local +Y axis used by CharacterController and the movement solver.
        return osg::Quat(MWWorld::getActorModelYaw(position.rot[2], tes4Npc), osg::Vec3(0, 0, -1));
    }

    osg::Quat makeInversedOrderObjectOsgQuat(const ESM::Position& position)
    {
        const float xr = position.rot[0];
        const float yr = position.rot[1];
        const float zr = position.rot[2];

        return osg::Quat(xr, osg::Vec3(-1, 0, 0)) * osg::Quat(yr, osg::Vec3(0, -1, 0))
            * osg::Quat(zr, osg::Vec3(0, 0, -1));
    }

    osg::Quat makeInverseNodeRotation(const MWWorld::Ptr& ptr)
    {
        const auto& pos = ptr.getRefData().getPosition();
        return ptr.getClass().isActor() ? makeActorOsgQuat(ptr) : makeInversedOrderObjectOsgQuat(pos);
    }

    osg::Quat makeDirectNodeRotation(const MWWorld::Ptr& ptr)
    {
        const auto& pos = ptr.getRefData().getPosition();
        return ptr.getClass().isActor() ? makeActorOsgQuat(ptr) : Misc::Convert::makeOsgQuat(pos);
    }

    osg::Quat makeNodeRotation(const MWWorld::Ptr& ptr, RotationOrder order)
    {
        if (order == RotationOrder::inverse)
            return makeInverseNodeRotation(ptr);
        return makeDirectNodeRotation(ptr);
    }

    void setNodeRotation(const MWWorld::Ptr& ptr, MWRender::RenderingManager& rendering, const osg::Quat& rotation)
    {
        if (ptr.getRefData().getBaseNode())
            rendering.rotateObject(ptr, rotation);
    }

    VFS::Path::Normalized getModel(const MWWorld::Ptr& ptr)
    {
        if (Misc::ResourceHelpers::isHiddenMarker(ptr.getCellRef().getRefId()))
            return {};
        return ptr.getClass().getCorrectedModel(ptr);
    }

    bool fnvPackageHasExplicitTime(const ESM4::AIPackage& package)
    {
        return package.mSchedule.time != 0xff && package.mSchedule.duration != 0;
    }

    bool fnvPackageCoversHour(const ESM4::AIPackage& package, float hour)
    {
        if (!fnvPackageHasExplicitTime(package))
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

    const ESM4::Reference* resolveFnvPackageReference(
        const MWWorld::ESMStore& store, const ESM4::AIPackage::PLDT& location)
    {
        if (location.type != 0 && location.type != 4)
            return nullptr;
        return store.get<ESM4::Reference>().search(ESM::FormId::fromUint32(location.location));
    }

    float getFnvPackageHour(const MWWorld::World& world, bool& usedHourOverride)
    {
        usedHourOverride = false;
        float hour = world.getTimeStamp().getHour();
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

    enum class FnvPackagePrePlacement
    {
        None,
        SameCell,
        MovedToPackageCell
    };

    struct FnvFurnitureMarkerPlacement
    {
        osg::Vec3f mOffset;
        float mHeading = 0.f;
        std::uint16_t mType = 0;
        std::uint16_t mEntryPoint = 0;
        std::uint8_t mPositionRef = 0;
        std::uint8_t mMarkerIndex = 0xff;
        bool mLegacy = false;
        bool mFound = false;
    };

    bool parseFnvIntEnv(const char* name, int& value)
    {
        const char* env = std::getenv(name);
        if (env == nullptr || env[0] == '\0')
            return false;

        char* end = nullptr;
        const long parsed = std::strtol(env, &end, 10);
        if (end == env)
            return false;

        value = static_cast<int>(parsed);
        return true;
    }

    int getViewerEsm4CellGridRadius()
    {
        int radius = Constants::ESM4CellGridRadius;
        int requested = radius;
        if (parseFnvIntEnv("OPENMW_WORLD_VIEWER_ESM4_GRID_RADIUS", requested))
        {
            if (requested < 0)
                requested = 0;
            if (requested > 8)
                requested = 8;
            radius = requested;
        }

        static int loggedRadius = -1;
        if (loggedRadius != radius)
        {
            loggedRadius = radius;
            Log(Debug::Info) << "World viewer: ESM4 exterior grid radius=" << radius
                             << " source="
                             << (std::getenv("OPENMW_WORLD_VIEWER_ESM4_GRID_RADIUS") != nullptr ? "env" : "default");
        }

        return radius;
    }

    bool envEnabled(const char* name)
    {
        const char* value = std::getenv(name);
        return value != nullptr && *value != '\0' && value[0] != '0';
    }

    bool worldViewerDiagnosticFilterEnabled()
    {
        return envEnabled("OPENMW_WORLD_VIEWER_HIDE_DIAGNOSTIC_MODELS")
            || envEnabled("OPENMW_WORLD_VIEWER_TELEMETRY");
    }

    bool worldViewerFreezeEsm4ActorMechanics()
    {
        return envEnabled("OPENMW_WORLD_VIEWER_FREEZE_ESM4_ACTOR_MECHANICS");
    }

    bool isEsm4Actor(const MWWorld::Ptr& ptr)
    {
        return ptr.getType() == ESM::REC_NPC_4 || ptr.getType() == ESM::REC_CREA4;
    }

    bool contains(std::string_view value, std::string_view needle)
    {
        return value.find(needle) != std::string_view::npos;
    }

    bool isWorldViewerDiagnosticModel(std::string_view value)
    {
        return contains(value, "meshes/markers/")
            || contains(value, "/editormarkers/")
            || contains(value, "staticcollectionpivotdummy")
            || contains(value, "fill_planes/")
            || contains(value, "fillplane_")
            || contains(value, "occlusion")
            || contains(value, "occluder")
            || contains(value, "portalmarker")
            || contains(value, "roommarker")
            || contains(value, "loadmarker")
            || contains(value, "xmarker")
            || contains(value, "headingmarker")
            || contains(value, "triggerbox")
            || contains(value, "collisionmarker")
            || contains(value, "water/water");
    }

    bool skipWorldViewerDiagnosticObject(const MWWorld::Ptr& ptr, const VFS::Path::Normalized& model)
    {
        if (model.empty() || !worldViewerDiagnosticFilterEnabled() || !isWorldViewerDiagnosticModel(model.value()))
            return false;

        static std::atomic<int> skipLogCount{ 0 };
        const int logIndex = skipLogCount.fetch_add(1);
        if (logIndex < 240)
        {
            Log(Debug::Info) << "World viewer: skipped diagnostic scene object base="
                             << ptr.getCellRef().getRefId().toDebugString()
                             << " type=" << ptr.getTypeDescription()
                             << " model=" << model.value()
                             << " ptr=" << ptr.toString();
        }
        else if (logIndex == 240)
            Log(Debug::Info) << "World viewer: further diagnostic scene object skip logs suppressed";

        return true;
    }

    osg::Vec3f transformFnvFurnitureMarkerOffsetForProof(osg::Vec3f offset)
    {
        const char* mode = std::getenv("OPENMW_FNV_FURNITURE_MARKER_OFFSET_MODE");
        if (mode == nullptr || mode[0] == '\0')
            return offset;

        const std::string_view value(mode);
        if (value == "negx")
            return osg::Vec3f(-offset.x(), offset.y(), offset.z());
        if (value == "negy")
            return osg::Vec3f(offset.x(), -offset.y(), offset.z());
        if (value == "negxy")
            return osg::Vec3f(-offset.x(), -offset.y(), offset.z());
        if (value == "swapxy")
            return osg::Vec3f(offset.y(), offset.x(), offset.z());
        if (value == "swapxynegx")
            return osg::Vec3f(-offset.y(), offset.x(), offset.z());
        if (value == "swapxynegy")
            return osg::Vec3f(offset.y(), -offset.x(), offset.z());
        if (value == "swapxynegxy")
            return osg::Vec3f(-offset.y(), -offset.x(), offset.z());
        return offset;
    }

    void collectFnvFurnitureMarkers(const Nif::NiObjectNET& object, std::vector<FnvFurnitureMarkerPlacement>& result)
    {
        for (const Nif::ExtraPtr& extra : object.getExtraList())
        {
            if (extra.empty() || extra->recType != Nif::RC_BSFurnitureMarker)
                continue;

            const auto* marker = static_cast<const Nif::BSFurnitureMarker*>(extra.getPtr());
            for (const Nif::BSFurnitureMarker::LegacyFurniturePosition& legacy : marker->mLegacyMarkers)
            {
                FnvFurnitureMarkerPlacement placement;
                placement.mOffset = legacy.mOffset;
                placement.mHeading = static_cast<float>(legacy.mOrientation) / 1000.f;
                placement.mPositionRef = legacy.mPositionRef;
                placement.mLegacy = true;
                placement.mFound = true;
                result.push_back(placement);
            }
            for (const Nif::BSFurnitureMarker::FurniturePosition& current : marker->mMarkers)
            {
                FnvFurnitureMarkerPlacement placement;
                placement.mOffset = current.mOffset;
                placement.mHeading = current.mHeading;
                placement.mType = current.mType;
                placement.mEntryPoint = current.mEntryPoint;
                placement.mFound = true;
                result.push_back(placement);
            }
        }

        if (const auto* node = dynamic_cast<const Nif::NiNode*>(&object))
        {
            for (const Nif::NiAVObjectPtr& child : node->mChildren)
            {
                if (child.empty())
                    continue;
                if (const auto* childObject = dynamic_cast<const Nif::NiObjectNET*>(child.getPtr()))
                    collectFnvFurnitureMarkers(*childObject, result);
            }
        }
    }

    FnvFurnitureMarkerPlacement getFnvFurnitureMarkerPlacement(
        const MWWorld::ESMStore& store, const ESM4::Reference& target)
    {
        FnvFurnitureMarkerPlacement fallback;
        const ESM4::Furniture* furniture = store.get<ESM4::Furniture>().search(target.mBaseObj);
        if (furniture == nullptr || furniture->mModel.empty())
            return fallback;

        try
        {
            VFS::Path::Normalized model = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(furniture->mModel));
            Resource::ResourceSystem* resourceSystem = MWBase::Environment::get().getResourceSystem();
            Nif::NIFFilePtr nif = resourceSystem->getNifFileManager()->get(model);
            Nif::FileView file(*nif);

            std::vector<FnvFurnitureMarkerPlacement> markers;
            for (std::size_t i = 0; i < file.numRoots(); ++i)
            {
                if (const auto* root = dynamic_cast<const Nif::NiObjectNET*>(file.getRoot(i)))
                    collectFnvFurnitureMarkers(*root, markers);
            }

            Log(Debug::Verbose) << "FNV/ESM4 diag: furniture marker scan targetRef=" << target.mEditorId
                             << " base=" << target.mBaseObj << " model=" << model.value()
                             << " activeMarkers=0x" << std::hex << furniture->mActiveMarkerFlags << std::dec
                             << " markers=" << markers.size();

            for (std::size_t i = 0; i < markers.size(); ++i)
            {
                markers[i].mMarkerIndex = static_cast<std::uint8_t>(std::min<std::size_t>(i, 0xff));
                const FnvFurnitureMarkerPlacement& marker = markers[i];
                Log(Debug::Verbose) << "FNV/ESM4 diag: furniture marker candidate index=" << i
                                 << " offset=(" << marker.mOffset.x() << "," << marker.mOffset.y() << ","
                                 << marker.mOffset.z() << ") heading=" << marker.mHeading
                                 << " type=" << marker.mType << " entryPoint=" << marker.mEntryPoint
                                 << " positionRef=" << static_cast<int>(marker.mPositionRef)
                                 << " legacy=" << marker.mLegacy;
            }

            auto markerLess = [](const FnvFurnitureMarkerPlacement& left, const FnvFurnitureMarkerPlacement& right) {
                return std::abs(left.mOffset.x()) < std::abs(right.mOffset.x());
            };
            if (!markers.empty())
            {
                int markerIndex = -1;
                if (parseFnvIntEnv("OPENMW_FNV_FURNITURE_MARKER_INDEX", markerIndex) && markerIndex >= 0
                    && markerIndex < static_cast<int>(markers.size()))
                {
                    Log(Debug::Verbose) << "FNV/ESM4 diag: selected furniture marker by proof index " << markerIndex;
                    return markers[static_cast<std::size_t>(markerIndex)];
                }

                int positionRef = -1;
                if (parseFnvIntEnv("OPENMW_FNV_FURNITURE_MARKER_POSITION_REF", positionRef))
                {
                    for (const FnvFurnitureMarkerPlacement& marker : markers)
                    {
                        if (static_cast<int>(marker.mPositionRef) == positionRef)
                        {
                            Log(Debug::Verbose) << "FNV/ESM4 diag: selected furniture marker by proof positionRef "
                                             << positionRef;
                            return marker;
                        }
                    }
                }

                // FO3/FNV MNAM is a marker-index bitfield. Retail Easy Pete telemetry, for
                // example, reports markerIndex=2 for activeMarkers=0x40000004. High bits may
                // carry furniture capabilities, so only compare indices that exist in the NIF.
                for (std::size_t i = 0; i < markers.size() && i < 32; ++i)
                {
                    if ((furniture->mActiveMarkerFlags & (std::uint32_t{ 1 } << i)) != 0)
                    {
                        Log(Debug::Verbose) << "FNV/ESM4 diag: selected active furniture marker index " << i;
                        return markers[i];
                    }
                }

                const auto centered = std::min_element(markers.begin(), markers.end(), markerLess);
                return centered != markers.end() ? *centered : markers.front();
            }
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: furniture marker scan failed targetRef=" << target.mEditorId
                                << " error=" << e.what();
        }

        return fallback;
    }

    osg::Vec3f rotateFnvPackageOffset(const osg::Vec3f& offset, float rotZ)
    {
        const char* sign = std::getenv("OPENMW_FNV_FURNITURE_MARKER_ROTATION_SIGN");
        const float zSign = (sign != nullptr && std::string_view(sign) == "positive") ? 1.f : -1.f;
        const osg::Matrix rotation = osg::Matrix::rotate(osg::Quat(rotZ, osg::Vec3f(0.f, 0.f, zSign)));
        const osg::Vec3d transformed = osg::Vec3d(offset) * rotation;
        return osg::Vec3f(transformed.x(), transformed.y(), transformed.z());
    }

    float applyFnvFurnitureMarkerHeading(float targetRotZ, float markerHeading)
    {
        const char* mode = std::getenv("OPENMW_FNV_FURNITURE_MARKER_HEADING_MODE");
        if (mode == nullptr || mode[0] == '\0' || std::string_view(mode) == "add")
            return targetRotZ + markerHeading;
        if (std::string_view(mode) == "subtract")
            return targetRotZ - markerHeading;
        if (std::string_view(mode) == "none")
            return targetRotZ;
        if (std::string_view(mode) == "marker")
            return markerHeading;
        return targetRotZ + markerHeading;
    }

    bool fnvPackageConditionsPass(const ESM4::AIPackage& package)
    {
        if (package.mConditions.empty())
            return true;
        return MWBase::Environment::get().getWorld()->getESM4QuestRuntime().evaluateConditions(package.mConditions);
    }

    FnvPackagePrePlacement applyFnvPackagePrePlacement(const MWWorld::Ptr& ptr, const MWWorld::World& world)
    {
        if (std::getenv("OPENMW_FNV_DISABLE_PACKAGE_PREPLACEMENT") != nullptr
            || std::getenv("OPENMW_FNV_DISABLE_AI_PACKAGES") != nullptr)
            return FnvPackagePrePlacement::None;

        if (ptr.getType() != ESM::REC_NPC_4)
            return FnvPackagePrePlacement::None;

        const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(ptr);
        if (traits == nullptr || !traits->mIsFONV)
            return FnvPackagePrePlacement::None;

        const ESM4::Npc* packageRecord = MWClass::ESM4Npc::getAIPackageRecord(ptr);
        if (packageRecord == nullptr || packageRecord->mAIPackages.empty())
            return FnvPackagePrePlacement::None;

        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        if (store == nullptr || ptr.getCell() == nullptr || ptr.getCell()->getCell() == nullptr)
            return FnvPackagePrePlacement::None;

        bool usedHourOverride = false;
        const float hour = getFnvPackageHour(world, usedHourOverride);
        const auto& packageStore = store->get<ESM4::AIPackage>();

        const ESM4::AIPackage* selected = nullptr;
        for (ESM::FormId packageId : packageRecord->mAIPackages)
        {
            const ESM4::AIPackage* package = packageStore.search(packageId);
            if (package != nullptr && fnvPackageConditionsPass(*package) && fnvPackageCoversHour(*package, hour))
            {
                selected = package;
                break;
            }
        }

        if (selected == nullptr)
            return FnvPackagePrePlacement::None;

        const ESM4::Reference* target = resolveFnvPackageReference(*store, selected->mLocation);
        if (target == nullptr)
            return FnvPackagePrePlacement::None;

        const bool furnitureTarget = store->get<ESM4::Furniture>().search(target->mBaseObj) != nullptr;
        const MWClass::FalloutFurnitureState furnitureState = MWClass::ESM4Npc::getFurnitureState(ptr);
        const MWClass::FalloutFurniturePlacement furniturePlacement = MWClass::ESM4Npc::getFurniturePlacement(ptr);
        const bool retainFurnitureClaim = furnitureTarget && MWClass::shouldRetainFalloutFurnitureClaim(furnitureState,
            furniturePlacement.mValid, furniturePlacement.mFurnitureRef == target->mId);
        if (!retainFurnitureClaim)
        {
            MWClass::ESM4Npc::setFurnitureState(ptr, MWClass::FalloutFurnitureState::None);
            MWClass::ESM4Npc::setFurniturePlacement(ptr, {});
        }

        const ESM::RefId& currentCellId = ptr.getCell()->getCell()->getId();
        const bool sameCell = target->mParent == currentCellId;
        Log(Debug::Verbose) << "FNV/ESM4 diag: package pre-placement " << selected->mEditorId
                         << " hour=" << hour << " override=" << usedHourOverride
                         << " targetRef=" << target->mEditorId << " targetParent=" << target->mParent
                         << " currentCell=" << currentCellId << " sameCell=" << sameCell << " for "
                         << traits->mEditorId;

        if (retainFurnitureClaim)
        {
            Log(Debug::Verbose) << "FNV/ESM4 diag: retained active furniture claim package="
                                << selected->mEditorId << " targetRef=" << target->mEditorId << " state="
                                << static_cast<int>(furnitureState) << " for " << traits->mEditorId;
            return FnvPackagePrePlacement::None;
        }

        // A scheduled package describes an AI goal, not a license to teleport a
        // persistent actor into an unloaded interior while its exterior cell is
        // being inserted.  Keep the authored reference in its current cell and
        // let the runtime package/pathing code perform an actual transition.
        // The legacy behavior remains available only to focused compatibility
        // captures that explicitly request it.
        if (!sameCell && !envEnabled("OPENMW_FNV_ENABLE_CROSS_CELL_PACKAGE_PREPLACEMENT"))
        {
            Log(Debug::Verbose) << "FNV/ESM4: deferred cross-cell package goal " << selected->mEditorId
                                << " actor=" << traits->mEditorId << " targetCell=" << target->mParent
                                << " currentCell=" << currentCellId;
            return FnvPackagePrePlacement::None;
        }

        ESM::Position position = ptr.getRefData().getPosition();
        position.pos[0] = target->mPos.pos[0];
        position.pos[1] = target->mPos.pos[1];
        position.pos[2] = target->mPos.pos[2];
        position.rot[0] = 0.f;
        position.rot[1] = 0.f;
        position.rot[2] = target->mPos.rot[2];

        const FnvFurnitureMarkerPlacement marker = getFnvFurnitureMarkerPlacement(*store, *target);
        if (marker.mFound && std::getenv("OPENMW_FNV_DISABLE_FURNITURE_MARKER_PLACEMENT") == nullptr)
        {
            const osg::Vec3f proofOffset = transformFnvFurnitureMarkerOffsetForProof(marker.mOffset);
            const osg::Vec3f worldOffset = rotateFnvPackageOffset(proofOffset, target->mPos.rot[2]);
            const bool entryMarkerPlacement = envEnabled("OPENMW_FNV_FURNITURE_ENTRY_MARKER_PLACEMENT");
            if (furnitureTarget)
            {
                MWClass::FalloutFurniturePlacement placement;
                placement.mEntryPosition = osg::Vec3f(target->mPos.pos[0] + worldOffset.x(),
                    target->mPos.pos[1] + worldOffset.y(), target->mPos.pos[2] + worldOffset.z());
                placement.mSettledPosition = osg::Vec3f(
                    target->mPos.pos[0], target->mPos.pos[1], target->mPos.pos[2] + marker.mOffset.z());
                placement.mEntryYaw = applyFnvFurnitureMarkerHeading(target->mPos.rot[2], marker.mHeading);
                placement.mSettledYaw = target->mPos.rot[2];
                placement.mFurnitureRef = target->mId;
                placement.mMarkerIndex = marker.mMarkerIndex;
                placement.mPositionRef = marker.mPositionRef;
                placement.mValid = true;

                const bool alongY = std::abs(marker.mOffset.y()) >= std::abs(marker.mOffset.x());
                const std::string_view direction = alongY ? (marker.mOffset.y() >= 0.f ? "forward" : "back")
                                                          : (marker.mOffset.x() < 0.f ? "left" : "right");
                placement.mEnterGroup = "chair" + std::string(direction) + "enter";
                placement.mExitGroup = "chair" + std::string(direction) + "exit";
                MWClass::ESM4Npc::setFurniturePlacement(ptr, placement);
                MWClass::ESM4Npc::setFurnitureState(ptr, MWClass::FalloutFurnitureState::Approaching);

                position.pos[0] = placement.mEntryPosition.x();
                position.pos[1] = placement.mEntryPosition.y();
                position.pos[2] = placement.mEntryPosition.z();
                position.rot[2] = placement.mEntryYaw;

                if (sameCell && !entryMarkerPlacement)
                {
                    Log(Debug::Verbose) << "FNV/ESM4 diag: deferred same-cell furniture placement to runtime package "
                                     << selected->mEditorId << " targetRef=" << target->mEditorId
                                     << " markerIndex=" << static_cast<unsigned int>(placement.mMarkerIndex)
                                     << " enterGroup=" << placement.mEnterGroup
                                     << " exitGroup=" << placement.mExitGroup << " for " << traits->mEditorId;
                    return FnvPackagePrePlacement::None;
                }
            }
            else
            {
                position.pos[0] += worldOffset.x();
                position.pos[1] += worldOffset.y();
                position.pos[2] += worldOffset.z();
                position.rot[2] = applyFnvFurnitureMarkerHeading(target->mPos.rot[2], marker.mHeading);
            }
            Log(Debug::Verbose) << "FNV/ESM4 diag: applied furniture marker package placement "
                             << selected->mEditorId << " targetRef=" << target->mEditorId
                             << " markerOffset=(" << marker.mOffset.x() << "," << marker.mOffset.y() << ","
                             << marker.mOffset.z() << ") proofOffset=(" << proofOffset.x() << ","
                             << proofOffset.y() << "," << proofOffset.z() << ") worldOffset=(" << worldOffset.x()
                             << "," << worldOffset.y() << "," << worldOffset.z() << ") heading=" << marker.mHeading
                             << " type=" << marker.mType << " entryPoint=" << marker.mEntryPoint
                             << " positionRef=" << static_cast<int>(marker.mPositionRef)
                             << " legacy=" << marker.mLegacy
                             << " state=" << (furnitureTarget ? "entry" : "target")
                             << " finalPos=(" << position.pos[0] << "," << position.pos[1] << ","
                             << position.pos[2] << ") finalRotZ=" << position.rot[2] << " for "
                             << traits->mEditorId;
        }

        if (!sameCell)
        {
            MWWorld::CellStore* targetCell
                = MWBase::Environment::get().getWorldModel()->findCell(target->mParent, true);
            if (targetCell == nullptr)
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: package pre-placement target cell not found "
                                    << target->mParent << " for " << selected->mEditorId << " actor "
                                    << traits->mEditorId;
                return FnvPackagePrePlacement::None;
            }

            ptr.getRefData().setPosition(position);
            MWWorld::Ptr movedPtr = ptr.getCell()->moveTo(ptr, targetCell);
            Log(Debug::Verbose) << "FNV/ESM4 diag: applied cross-cell package pre-placement "
                             << selected->mEditorId << " actor=" << traits->mEditorId
                             << " targetRef=" << target->mEditorId << " fromCell=" << currentCellId
                             << " toCell=" << targetCell->getCell()->getId()
                             << " toName='" << targetCell->getCell()->getNameId()
                             << "' toDesc='" << targetCell->getCell()->getDescription() << "' pos=("
                             << position.pos[0] << "," << position.pos[1] << "," << position.pos[2]
                             << ") rotZ=" << position.rot[2]
                             << " movedPtrCell=" << movedPtr.getCell()->getCell()->getId();
            return FnvPackagePrePlacement::MovedToPackageCell;
        }

        ptr.getRefData().setPosition(position);
        Log(Debug::Verbose) << "FNV/ESM4 diag: applied same-cell package pre-placement " << selected->mEditorId
                         << " targetRef=" << target->mEditorId << " pos=(" << position.pos[0] << ","
                         << position.pos[1] << "," << position.pos[2] << ") rotZ=" << position.rot[2] << " for "
                         << traits->mEditorId;
        return FnvPackagePrePlacement::SameCell;
    }

    // Null node meant to distinguish objects that aren't in the scene from paged objects
    // TODO: find a more clever way to make paging exclusion more reliable?
    static osg::ref_ptr<SceneUtil::PositionAttitudeTransform> pagedNode = new SceneUtil::PositionAttitudeTransform;

    void addObject(const MWWorld::Ptr& ptr, const MWWorld::World& world, const std::vector<ESM::RefNum>& pagedRefs,
        MWPhysics::PhysicsSystem& physics, MWRender::RenderingManager& rendering)
    {
        if (ptr.getRefData().getBaseNode() || physics.getActor(ptr))
        {
            Log(Debug::Warning) << "Warning: Tried to add " << ptr.getCellRef().getRefId() << " to the scene twice";
            return;
        }

        const FnvPackagePrePlacement fnvPackagePrePlacement = applyFnvPackagePrePlacement(ptr, world);
        if (fnvPackagePrePlacement == FnvPackagePrePlacement::MovedToPackageCell)
            return;

        const VFS::Path::Normalized model = getModel(ptr);
        const auto rotation = makeDirectNodeRotation(ptr);

        if (skipWorldViewerDiagnosticObject(ptr, model))
            return;

        ESM::RefNum refnum = ptr.getCellRef().getRefNum();
        const bool pagedRef = refnum.hasContentFile() && std::binary_search(pagedRefs.begin(), pagedRefs.end(), refnum);
        const bool forceEsm4ActorRender = pagedRef && isEsm4Actor(ptr) && ptr.getClass().isActor();
        if (!pagedRef || forceEsm4ActorRender)
        {
            if (forceEsm4ActorRender && envEnabled("OPENMW_WORLD_VIEWER_ACTOR_TELEMETRY"))
                Log(Debug::Info) << "World viewer actor ledger: phase=paged-ref-actor-promote"
                                 << " ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                                 << " base=" << ptr.getCellRef().getRefId().toDebugString()
                                 << " type=\"" << ptr.getTypeDescription() << "\"";
            ptr.getClass().insertObjectRendering(ptr, model, rendering);
        }
        else
            ptr.getRefData().setBaseNode(pagedNode);
        setNodeRotation(ptr, rendering, rotation);
        if (fnvPackagePrePlacement == FnvPackagePrePlacement::SameCell && ptr.getRefData().getBaseNode())
        {
            const osg::Vec3f scenePos = ptr.getRefData().getBaseNode()->getPosition();
            const ESM::Position& refPos = ptr.getRefData().getPosition();
            Log(Debug::Verbose) << "FNV/ESM4 diag: finalized package scene placement "
                             << ptr.getCellRef().getRefId() << " refPos=(" << refPos.pos[0] << ","
                             << refPos.pos[1] << "," << refPos.pos[2] << ") scenePos=(" << scenePos.x() << ","
                             << scenePos.y() << "," << scenePos.z() << ") rotZ=" << refPos.rot[2];
        }

        const bool freezeEsm4Actor = worldViewerFreezeEsm4ActorMechanics() && isEsm4Actor(ptr) && ptr.getClass().isActor();
        if (ptr.getClass().useAnim() && !freezeEsm4Actor)
            MWBase::Environment::get().getMechanicsManager()->add(ptr);
        else if (freezeEsm4Actor && envEnabled("OPENMW_WORLD_VIEWER_ACTOR_TELEMETRY"))
        {
            Log(Debug::Info) << "World viewer actor ledger: phase=mechanics-skip ref=\"" << ptr.getCellRef().getRefId()
                             << "\" type=" << ptr.getType() << " reason=\"proof static ESM4 actor\"";
        }

        if (ptr.getClass().isActor())
            rendering.addWaterRippleEmitter(ptr);

        // Restore effect particles
        world.applyLoopingParticles(ptr);

        if (!model.empty())
            ptr.getClass().insertObject(ptr, model, rotation, physics);

        MWBase::Environment::get().getLuaManager()->objectAddedToScene(ptr);
    }

    void addObject(const MWWorld::Ptr& ptr, const MWWorld::World& world, const MWPhysics::PhysicsSystem& physics,
        float& lowestPoint, bool isInterior, DetourNavigator::Navigator& navigator,
        const DetourNavigator::UpdateGuard* navigatorUpdateGuard = nullptr)
    {
        if (const auto object = physics.getObject(ptr))
        {
            // Find the lowest point of this collision object in world space from its AABB if interior
            // this point is used to determine the infinite fall cutoff from lowest point in the cell
            if (isInterior)
            {
                btVector3 aabbMin;
                btVector3 aabbMax;
                const auto transform = object->getTransform();
                object->getShapeInstance()->mCollisionShape->getAabb(transform, aabbMin, aabbMax);
                lowestPoint = std::min(lowestPoint, static_cast<float>(aabbMin.z()));
            }

            const DetourNavigator::ObjectTransform objectTransform{ ptr.getRefData().getPosition(),
                ptr.getCellRef().getScale() };

            if (ptr.getClass().isDoor() && !ptr.getCellRef().getTeleport())
            {
                btVector3 aabbMin;
                btVector3 aabbMax;
                object->getShapeInstance()->mCollisionShape->getAabb(btTransform::getIdentity(), aabbMin, aabbMax);

                const auto center = (aabbMax + aabbMin) * 0.5f;

                const auto distanceFromDoor = world.getMaxActivationDistance() * 0.5f;
                const auto toPoint = aabbMax.x() - aabbMin.x() < aabbMax.y() - aabbMin.y()
                    ? btVector3(distanceFromDoor, 0, 0)
                    : btVector3(0, distanceFromDoor, 0);

                const auto transform = object->getTransform();
                const btTransform closedDoorTransform(
                    Misc::Convert::makeBulletQuaternion(ptr.getCellRef().getPosition()), transform.getOrigin());

                const auto start = Misc::Convert::toOsg(closedDoorTransform(center + toPoint));
                const auto startPoint = physics.castRay(start, start - osg::Vec3f(0, 0, 1000), { ptr }, {},
                    MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap
                        | MWPhysics::CollisionType_Water);
                const auto connectionStart = startPoint.mHit ? startPoint.mHitPos : start;

                const auto end = Misc::Convert::toOsg(closedDoorTransform(center - toPoint));
                const auto endPoint = physics.castRay(end, end - osg::Vec3f(0, 0, 1000), { ptr }, {},
                    MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap
                        | MWPhysics::CollisionType_Water);
                const auto connectionEnd = endPoint.mHit ? endPoint.mHitPos : end;

                navigator.addObject(DetourNavigator::ObjectId(object),
                    DetourNavigator::DoorShapes(
                        object->getShapeInstance(), objectTransform, connectionStart, connectionEnd),
                    transform, navigatorUpdateGuard);
            }
            else if (object->getShapeInstance()->mVisualCollisionType == Resource::VisualCollisionType::None)
            {
                navigator.addObject(DetourNavigator::ObjectId(object),
                    DetourNavigator::ObjectShapes(object->getShapeInstance(), objectTransform), object->getTransform(),
                    navigatorUpdateGuard);
            }
        }
        else if (physics.getActor(ptr))
        {
            const DetourNavigator::AgentBounds agentBounds = world.getPathfindingAgentBounds(ptr);
            if (!navigator.addAgent(agentBounds))
                Log(Debug::Warning) << "Agent bounds are not supported by navigator for " << ptr.toString() << ": "
                                    << agentBounds;
        }
    }

    struct InsertVisitor
    {
        MWWorld::CellStore& mCell;
        Loading::Listener* mLoadingListener;

        std::vector<MWWorld::Ptr> mToInsert;

        InsertVisitor(MWWorld::CellStore& cell, Loading::Listener* loadingListener);

        bool operator()(const MWWorld::Ptr& ptr);

        template <class AddObject>
        void insert(AddObject&& addObject);
    };

    InsertVisitor::InsertVisitor(MWWorld::CellStore& cell, Loading::Listener* loadingListener)
        : mCell(cell)
        , mLoadingListener(loadingListener)
    {
    }

    bool InsertVisitor::operator()(const MWWorld::Ptr& ptr)
    {
        // do not insert directly as we can't modify the cell from within the visitation
        // CreatureLevList::insertObjectRendering may spawn a new creature
        mToInsert.push_back(ptr);
        return true;
    }

    template <class AddObject>
    void InsertVisitor::insert(AddObject&& addObject)
    {
        for (MWWorld::Ptr& ptr : mToInsert)
        {
            if (!ptr.mRef->isDeleted() && ptr.getRefData().isEnabled())
            {
                try
                {
                    addObject(ptr);
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Error) << "failed to render '" << ptr.getCellRef().getRefId() << "': " << e.what();
                }
            }

            if (mLoadingListener != nullptr)
                mLoadingListener->increaseProgress(1);
        }
    }

    int getCellPositionDistanceToOrigin(const std::pair<int, int>& cellPosition)
    {
        return std::abs(cellPosition.first) + std::abs(cellPosition.second);
    }

    bool isCellInCollection(ESM::ExteriorCellLocation cellIndex, MWWorld::Scene::CellStoreCollection& collection)
    {
        for (auto* cell : collection)
        {
            assert(cell->getCell()->isExterior());
            if (cellIndex == cell->getCell()->getExteriorCellLocation())
                return true;
        }
        return false;
    }

    bool removeFromSorted(ESM::RefNum refNum, std::vector<ESM::RefNum>& pagedRefs)
    {
        const auto it = std::lower_bound(pagedRefs.begin(), pagedRefs.end(), refNum);
        if (it == pagedRefs.end() || *it != refNum)
            return false;
        pagedRefs.erase(it);
        return true;
    }

    template <class Function>
    void iterateOverCellsAround(int cellX, int cellY, int range, Function&& f)
    {
        for (int x = cellX - range, lastX = cellX + range; x <= lastX; ++x)
            for (int y = cellY - range, lastY = cellY + range; y <= lastY; ++y)
                f(x, y);
    }

    void sortCellsToLoad(int centerX, int centerY, std::vector<std::pair<int, int>>& cells)
    {
        const auto getDistanceToPlayerCell = [&](const std::pair<int, int>& cellPosition) {
            return std::abs(cellPosition.first - centerX) + std::abs(cellPosition.second - centerY);
        };

        const auto getCellPositionPriority = [&](const std::pair<int, int>& cellPosition) {
            return std::make_pair(getDistanceToPlayerCell(cellPosition), getCellPositionDistanceToOrigin(cellPosition));
        };

        std::sort(cells.begin(), cells.end(), [&](const std::pair<int, int>& lhs, const std::pair<int, int>& rhs) {
            return getCellPositionPriority(lhs) < getCellPositionPriority(rhs);
        });
    }
}

namespace MWWorld
{
    void Scene::removeFromPagedRefs(const Ptr& ptr)
    {
        ESM::RefNum refnum = ptr.getCellRef().getRefNum();
        if (refnum.hasContentFile() && removeFromSorted(refnum, mPagedRefs))
        {
            if (!ptr.getRefData().getBaseNode())
                return;
            ptr.getClass().insertObjectRendering(ptr, getModel(ptr), mRendering);
            setNodeRotation(ptr, mRendering, makeNodeRotation(ptr, RotationOrder::direct));
            reloadTerrain();
        }
    }

    bool Scene::isPagedRef(const Ptr& ptr) const
    {
        return ptr.getRefData().getBaseNode() == pagedNode.get();
    }

    void Scene::updateObjectRotation(const Ptr& ptr, RotationOrder order)
    {
        const auto rot = makeNodeRotation(ptr, order);
        setNodeRotation(ptr, mRendering, rot);
        mPhysics->updateRotation(ptr, rot);
    }

    void Scene::updateObjectScale(const Ptr& ptr)
    {
        float scale = ptr.getCellRef().getScale();
        osg::Vec3f scaleVec(scale, scale, scale);
        ptr.getClass().adjustScale(ptr, scaleVec, true);
        mRendering.scaleObject(ptr, scaleVec);
        mPhysics->updateScale(ptr);
    }

    void Scene::update(float duration)
    {
        if (mChangeCellGridRequest.has_value())
        {
            changeCellGrid(mChangeCellGridRequest->mPosition, mChangeCellGridRequest->mCellIndex,
                mChangeCellGridRequest->mChangeEvent);
            mChangeCellGridRequest.reset();
        }

        mPreloader->updateCache(mRendering.getReferenceTime());
        preloadCells(duration);
    }

    void Scene::unloadCell(CellStore* cell, const DetourNavigator::UpdateGuard* navigatorUpdateGuard)
    {
        if (mActiveCells.find(cell) == mActiveCells.end())
            return;
        Log(Debug::Info) << "Unloading cell " << cell->getCell()->getDescription();

        ListAndResetObjectsVisitor visitor;

        cell->forEach(visitor, true); // Include objects being teleported by Lua
        for (const auto& ptr : visitor.mObjects)
        {
            if (const auto object = mPhysics->getObject(ptr))
            {
                if (object->getShapeInstance()->mVisualCollisionType == Resource::VisualCollisionType::None)
                    mNavigator.removeObject(DetourNavigator::ObjectId(object), navigatorUpdateGuard);
                mPhysics->remove(ptr);
            }
            else if (mPhysics->getActor(ptr))
            {
                mNavigator.removeAgent(mWorld.getPathfindingAgentBounds(ptr));
                mRendering.removeActorPath(ptr);
                mPhysics->remove(ptr);
            }
            else
                ptr.mRef->mData.mPhysicsPostponed = false;
            MWBase::Environment::get().getLuaManager()->objectRemovedFromScene(ptr);
        }

        const auto cellX = cell->getCell()->getGridX();
        const auto cellY = cell->getCell()->getGridY();

        if (cell->getCell()->isExterior())
        {
            mNavigator.removeHeightfield(osg::Vec2i(cellX, cellY), navigatorUpdateGuard);
            mPhysics->removeHeightField(cellX, cellY);
        }

        if (cell->getCell()->hasWater())
            mNavigator.removeWater(osg::Vec2i(cellX, cellY), navigatorUpdateGuard);

        ESM::visit(ESM::VisitOverload{
                       [&](const ESM::Cell& c) {
                           if (const auto pathgrid = mWorld.getStore().get<ESM::Pathgrid>().search(c))
                               mNavigator.removePathgrid(*pathgrid);
                       },
                       [&](const ESM4::Cell& /*c*/) {},
                   },
            *cell->getCell());

        MWBase::Environment::get().getMechanicsManager()->drop(cell);

        mRendering.removeCell(cell);
        MWBase::Environment::get().getWindowManager()->removeCell(cell);

        mWorld.getLocalScripts().clearCell(cell);

        MWBase::Environment::get().getSoundManager()->stopSound(cell);
        mActiveCells.erase(cell);
        // Clean up any effects that may have been spawned while unloading all cells
        if (mActiveCells.empty())
            mRendering.notifyWorldSpaceChanged();
    }

    void Scene::loadCell(CellStore& cell, Loading::Listener* loadingListener, bool respawn, const osg::Vec3f& position,
        const DetourNavigator::UpdateGuard* navigatorUpdateGuard)
    {
        using DetourNavigator::HeightfieldShape;

        assert(mActiveCells.find(&cell) == mActiveCells.end());
        mActiveCells.insert(&cell);

        Log(Debug::Info) << "Loading cell " << cell.getCell()->getDescription();

        const int cellX = cell.getCell()->getGridX();
        const int cellY = cell.getCell()->getGridY();
        const MWWorld::Cell& cellVariant = *cell.getCell();
        ESM::RefId worldspace = cellVariant.getWorldSpace();
        ESM::ExteriorCellLocation cellIndex(cellX, cellY, worldspace);

        if (cellVariant.isExterior())
        {
            osg::ref_ptr<const ESMTerrain::LandObject> land = mRendering.getLandManager()->getLand(cellIndex);
            const ESM::LandData* data = land ? land->getData(ESM::Land::DATA_VHGT) : nullptr;
            const int verts = ESM::getLandSize(worldspace);
            const int worldsize = ESM::getCellSize(worldspace);

            if (data)
            {
                mPhysics->addHeightField(data->getHeights().data(), cellX, cellY, worldsize, verts,
                    data->getMinHeight(), data->getMaxHeight(), land.get());
            }
            else if (!ESM::isEsm4Ext(worldspace))
            {
                static const std::vector<float> defaultHeight(verts * verts, ESM::Land::DEFAULT_HEIGHT);
                mPhysics->addHeightField(defaultHeight.data(), cellX, cellY, worldsize, verts,
                    ESM::Land::DEFAULT_HEIGHT, ESM::Land::DEFAULT_HEIGHT, land.get());
            }
            if (const auto heightField = mPhysics->getHeightField(cellX, cellY))
            {
                const osg::Vec2i cellPosition(cellX, cellY);
                const btVector3& origin = heightField->getCollisionObject()->getWorldTransform().getOrigin();
                const osg::Vec3f shift(origin.x(), origin.y(), origin.z());
                const HeightfieldShape shape = [&]() -> HeightfieldShape {
                    if (data == nullptr)
                    {
                        return DetourNavigator::HeightfieldPlane{ static_cast<float>(ESM::Land::DEFAULT_HEIGHT) };
                    }
                    else
                    {
                        DetourNavigator::HeightfieldSurface heights;
                        heights.mHeights = data->getHeights().data();
                        heights.mSize = static_cast<std::size_t>(data->getLandSize());
                        heights.mMinHeight = data->getMinHeight();
                        heights.mMaxHeight = data->getMaxHeight();
                        return heights;
                    }
                }();
                mNavigator.addHeightfield(cellPosition, worldsize, shape, navigatorUpdateGuard);
            }
        }

        ESM::visit(ESM::VisitOverload{
                       [&](const ESM::Cell& c) {
                           if (const auto pathgrid = mWorld.getStore().get<ESM::Pathgrid>().search(c))
                               mNavigator.addPathgrid(c, *pathgrid);
                       },
                       [&](const ESM4::Cell& /*c*/) {},
                   },
            *cell.getCell());

        // register local scripts
        // do this before insertCell, to make sure we don't add scripts from levelled creature spawning twice
        mWorld.getLocalScripts().addCell(&cell);

        if (respawn)
            cell.respawn();

        insertCell(cell, loadingListener, navigatorUpdateGuard);

        mRendering.addCell(&cell);

        MWBase::Environment::get().getWindowManager()->addCell(&cell);
        bool waterEnabled = cellVariant.hasWater() || cell.isExterior();
        float waterLevel = cell.getWaterLevel();
        mRendering.setWaterEnabled(waterEnabled);
        if (waterEnabled)
        {
            mPhysics->enableWater(waterLevel);
            mRendering.setWaterHeight(waterLevel);

            if (cellVariant.isExterior())
            {
                if (mPhysics->getHeightField(cellX, cellY) != nullptr)
                    mNavigator.addWater(
                        osg::Vec2i(cellX, cellY), ESM::Land::REAL_SIZE, waterLevel, navigatorUpdateGuard);
            }
            else
            {
                mNavigator.addWater(
                    osg::Vec2i(cellX, cellY), std::numeric_limits<int>::max(), waterLevel, navigatorUpdateGuard);
            }
        }
        else
            mPhysics->disableWater();

        if (!cell.isExterior() && !cellVariant.isQuasiExterior())
            mRendering.configureAmbient(cellVariant);

        mPreloader->notifyLoaded(&cell);
    }

    void Scene::clear()
    {
        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();
        for (auto iter = mActiveCells.begin(); iter != mActiveCells.end();)
        {
            auto* cell = *iter++;
            unloadCell(cell, navigatorUpdateGuard.get());
        }
        navigatorUpdateGuard.reset();
        assert(mActiveCells.empty());
        mCurrentCell = nullptr;
        mLowestPoint = std::numeric_limits<float>::max();

        mPreloader->clear();
        mTeleportDoorPreloadRequestsLogged.clear();
        mTeleportDoorPreloadCompletionsLogged.clear();
    }

    osg::Vec4i Scene::gridCenterToBounds(const osg::Vec2i& centerCell) const
    {
        return osg::Vec4i(centerCell.x() - mHalfGridSize, centerCell.y() - mHalfGridSize,
            centerCell.x() + mHalfGridSize + 1, centerCell.y() + mHalfGridSize + 1);
    }

    osg::Vec2i Scene::getNewGridCenter(const osg::Vec3f& pos, const osg::Vec2i* currentGridCenter) const
    {
        ESM::RefId worldspace
            = mCurrentCell ? mCurrentCell->getCell()->getWorldSpace() : ESM::Cell::sDefaultWorldspaceId;
        if (currentGridCenter)
        {
            const osg::Vec2f center = ESM::indexToPosition(
                ESM::ExteriorCellLocation(currentGridCenter->x(), currentGridCenter->y(), worldspace), true);
            float distance = std::max(std::abs(center.x() - pos.x()), std::abs(center.y() - pos.y()));
            float cellSize = ESM::getCellSize(worldspace);
            const float maxDistance = cellSize / 2 + mCellLoadingThreshold; // 1/2 cell size + threshold
            if (distance <= maxDistance)
                return *currentGridCenter;
        }
        ESM::ExteriorCellLocation cellPos = ESM::positionToExteriorCellLocation(pos.x(), pos.y(), worldspace);
        return { cellPos.mX, cellPos.mY };
    }

    void Scene::playerMoved(const osg::Vec3f& pos)
    {
        if (!mCurrentCell)
            return;

        // The player is reset when z is 90 units below the lowest reference bound z.
        constexpr float lowestPointAdjustment = -90.0f;
        if (mCurrentCell->isExterior())
        {
            osg::Vec2i newCell = getNewGridCenter(pos, &mCurrentGridCenter);
            if (newCell != mCurrentGridCenter)
                requestChangeCellGrid(pos, newCell);
        }
        else if (pos.z() < mLowestPoint + lowestPointAdjustment)
        {
            // Player has fallen into the void, reset to interior marker/coc (#1415)
            const std::string_view cellNameId = mCurrentCell->getCell()->getNameId();
            MWBase::World* world = MWBase::Environment::get().getWorld();
            MWWorld::Ptr playerPtr = world->getPlayerPtr();

            // Check that collision is enabled, which is opposite to Vanilla
            // this change was decided in MR #4100 as the behaviour is preferable
            if (world->isActorCollisionEnabled(playerPtr))
            {
                ESM::Position newPos;
                const ESM::RefId refId = world->findInteriorPosition(cellNameId, newPos);

                // Only teleport if that teleport point is > the lowest point, rare edge case
                if (!refId.empty() && newPos.pos[2] >= mLowestPoint - lowestPointAdjustment)
                {
                    MWWorld::ActionTeleport(refId, newPos, false).execute(playerPtr);
                    Log(Debug::Warning) << "Player position has been reset due to falling into the void";
                }
            }
        }
    }

    void Scene::requestChangeCellGrid(const osg::Vec3f& position, const osg::Vec2i& cell, bool changeEvent)
    {
        mChangeCellGridRequest = ChangeCellGridRequest{ position,
            ESM::ExteriorCellLocation(cell.x(), cell.y(), mCurrentCell->getCell()->getWorldSpace()), changeEvent };
    }

    void Scene::changeCellGrid(const osg::Vec3f& pos, ESM::ExteriorCellLocation playerCellIndex, bool changeEvent)
    {
        const int halfGridSize
            = isEsm4Ext(playerCellIndex.mWorldspace) ? getViewerEsm4CellGridRadius() : Constants::CellGridRadius;
        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();
        const int playerCellX = playerCellIndex.mX;
        const int playerCellY = playerCellIndex.mY;

        for (auto iter = mActiveCells.begin(); iter != mActiveCells.end();)
        {
            auto* cell = *iter++;
            if (cell->getCell()->isExterior() && cell->getCell()->getWorldSpace() == playerCellIndex.mWorldspace)
            {
                const auto dx = std::abs(playerCellX - cell->getCell()->getGridX());
                const auto dy = std::abs(playerCellY - cell->getCell()->getGridY());
                if (dx > halfGridSize || dy > halfGridSize)
                    unloadCell(cell, navigatorUpdateGuard.get());
            }
            else
                unloadCell(cell, navigatorUpdateGuard.get());
        }

        const DetourNavigator::CellGridBounds cellGridBounds{
            .mCenter = osg::Vec2i(playerCellX, playerCellY),
            .mHalfSize = halfGridSize,
        };

        mNavigator.updateBounds(playerCellIndex.mWorldspace, cellGridBounds, pos, navigatorUpdateGuard.get());

        mHalfGridSize = halfGridSize;
        mCurrentGridCenter = osg::Vec2i(playerCellX, playerCellY);
        osg::Vec4i newGrid = gridCenterToBounds(mCurrentGridCenter);

        // NOTE: setActiveGrid must be after enableTerrain, otherwise we set the grid in the old exterior worldspace
        mRendering.enableTerrain(true, playerCellIndex.mWorldspace);
        mRendering.setActiveGrid(newGrid);

        mPreloader->setTerrain(mRendering.getTerrain());
        if (mRendering.pagingUnlockCache())
            mPreloader->abortTerrainPreloadExcept(nullptr);
        if (!mPreloader->isTerrainLoaded(PositionCellGrid{ pos, newGrid }, mRendering.getReferenceTime()))
            preloadTerrain(pos, playerCellIndex.mWorldspace, true);
        mPagedRefs.clear();
        mRendering.getPagedRefnums(newGrid, mPagedRefs);

        addPostponedPhysicsObjects();

        std::size_t refsToLoad = 0;
        std::vector<std::pair<int, int>> cellsPositionsToLoad;
        iterateOverCellsAround(playerCellX, playerCellY, mHalfGridSize, [&](int x, int y) {
            const ESM::ExteriorCellLocation location(x, y, playerCellIndex.mWorldspace);
            if (isCellInCollection(location, mActiveCells))
                return;
            refsToLoad += mWorld.getWorldModel().getExterior(location).count();
            cellsPositionsToLoad.emplace_back(x, y);
        });

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);
        loadingListener->setLabel("#{OMWEngine:LoadingExterior}");
        loadingListener->setProgressRange(refsToLoad);

        sortCellsToLoad(playerCellX, playerCellY, cellsPositionsToLoad);

        for (const auto& [x, y] : cellsPositionsToLoad)
        {
            ESM::ExteriorCellLocation indexToLoad = { x, y, playerCellIndex.mWorldspace };
            if (!isCellInCollection(indexToLoad, mActiveCells))
            {
                CellStore& cell = mWorld.getWorldModel().getExterior(indexToLoad);
                loadCell(cell, loadingListener, changeEvent, pos, navigatorUpdateGuard.get());
            }
        }

        mNavigator.update(pos, navigatorUpdateGuard.get());

        navigatorUpdateGuard.reset();

        CellStore& current = mWorld.getWorldModel().getExterior(playerCellIndex);
        MWBase::Environment::get().getWindowManager()->changeCell(&current);

        if (changeEvent)
            mCellChanged = true;

        mCellLoaded = true;
    }

    void Scene::addPostponedPhysicsObjects()
    {
        for (const auto& cell : mActiveCells)
        {
            cell->forEach([&](const MWWorld::Ptr& ptr) {
                if (ptr.mRef->mData.mPhysicsPostponed)
                {
                    ptr.mRef->mData.mPhysicsPostponed = false;
                    if (ptr.mRef->mData.isEnabled() && ptr.mRef->mRef.getCount() > 0)
                    {
                        const VFS::Path::Normalized model = getModel(ptr);
                        if (!model.empty())
                        {
                            const auto rotation = makeNodeRotation(ptr, RotationOrder::direct);
                            ptr.getClass().insertObjectPhysics(ptr, model, rotation, *mPhysics);
                        }
                    }
                }
                return true;
            });
        }
    }

    void Scene::testExteriorCells()
    {
        // Note: temporary disable ICO to decrease memory usage
        mRendering.getResourceSystem()->getSceneManager()->setIncrementalCompileOperation(nullptr);

        mRendering.getResourceSystem()->setExpiryDelay(1.f);

        const MWWorld::Store<ESM::Cell>& cells = mWorld.getStore().get<ESM::Cell>();

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);
        loadingListener->setProgressRange(cells.getExtSize());

        MWWorld::Store<ESM::Cell>::iterator it = cells.extBegin();
        int i = 1;
        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();
        for (; it != cells.extEnd(); ++it)
        {
            loadingListener->setLabel("#{OMWEngine:TestingExteriorCells} (" + std::to_string(i) + "/"
                + std::to_string(cells.getExtSize()) + ")...");

            CellStore& cell = mWorld.getWorldModel().getExterior(
                ESM::ExteriorCellLocation(it->mData.mX, it->mData.mY, ESM::Cell::sDefaultWorldspaceId));
            const osg::Vec3f position
                = osg::Vec3f(it->mData.mX + 0.5f, it->mData.mY + 0.5f, 0) * Constants::CellSizeInUnits;
            const osg::Vec2i cellPosition(it->mData.mX, it->mData.mY);

            const DetourNavigator::CellGridBounds cellGridBounds{
                .mCenter = osg::Vec2i(it->mData.mX, it->mData.mY),
                .mHalfSize = Constants::CellGridRadius,
            };

            mNavigator.updateBounds(
                ESM::Cell::sDefaultWorldspaceId, cellGridBounds, position, navigatorUpdateGuard.get());

            loadCell(cell, nullptr, false, position, navigatorUpdateGuard.get());

            mNavigator.update(position, navigatorUpdateGuard.get());
            navigatorUpdateGuard.reset();
            mNavigator.wait(DetourNavigator::WaitConditionType::requiredTilesPresent, nullptr);
            navigatorUpdateGuard = mNavigator.makeUpdateGuard();

            auto iter = mActiveCells.begin();
            while (iter != mActiveCells.end())
            {
                if (it->isExterior() && it->mData.mX == (*iter)->getCell()->getGridX()
                    && it->mData.mY == (*iter)->getCell()->getGridY())
                {
                    unloadCell(*iter, navigatorUpdateGuard.get());
                    break;
                }

                ++iter;
            }

            mRendering.getResourceSystem()->updateCache(mRendering.getReferenceTime());

            loadingListener->increaseProgress(1);
            i++;
        }

        mRendering.getResourceSystem()->getSceneManager()->setIncrementalCompileOperation(
            mRendering.getIncrementalCompileOperation());
        mRendering.getResourceSystem()->setExpiryDelay(Settings::cells().mCacheExpiryDelay);
    }

    void Scene::testInteriorCells()
    {
        // Note: temporary disable ICO to decrease memory usage
        mRendering.getResourceSystem()->getSceneManager()->setIncrementalCompileOperation(nullptr);

        mRendering.getResourceSystem()->setExpiryDelay(1.f);

        const MWWorld::Store<ESM::Cell>& cells = mWorld.getStore().get<ESM::Cell>();

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);
        loadingListener->setProgressRange(cells.getIntSize());

        int i = 1;
        MWWorld::Store<ESM::Cell>::iterator it = cells.intBegin();
        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();
        for (; it != cells.intEnd(); ++it)
        {
            loadingListener->setLabel("#{OMWEngine:TestingInteriorCells} (" + std::to_string(i) + "/"
                + std::to_string(cells.getIntSize()) + ")...");

            CellStore& cell = mWorld.getWorldModel().getInterior(it->mName);
            ESM::Position position;
            mWorld.findInteriorPosition(it->mName, position);
            mNavigator.updateBounds(
                cell.getCell()->getWorldSpace(), std::nullopt, position.asVec3(), navigatorUpdateGuard.get());
            loadCell(cell, nullptr, false, position.asVec3(), navigatorUpdateGuard.get());

            mNavigator.update(position.asVec3(), navigatorUpdateGuard.get());
            navigatorUpdateGuard.reset();
            mNavigator.wait(DetourNavigator::WaitConditionType::requiredTilesPresent, nullptr);
            navigatorUpdateGuard = mNavigator.makeUpdateGuard();

            auto iter = mActiveCells.begin();
            while (iter != mActiveCells.end())
            {
                assert(!(*iter)->getCell()->isExterior());

                if (it->mName == (*iter)->getCell()->getNameId())
                {
                    unloadCell(*iter, navigatorUpdateGuard.get());
                    break;
                }

                ++iter;
            }

            mRendering.getResourceSystem()->updateCache(mRendering.getReferenceTime());

            loadingListener->increaseProgress(1);
            i++;
        }

        mRendering.getResourceSystem()->getSceneManager()->setIncrementalCompileOperation(
            mRendering.getIncrementalCompileOperation());
        mRendering.getResourceSystem()->setExpiryDelay(Settings::cells().mCacheExpiryDelay);
    }

    void Scene::changePlayerCell(CellStore& cell, const ESM::Position& pos, bool adjustPlayerPos)
    {
        mHalfGridSize = cell.getCell()->isEsm4() ? getViewerEsm4CellGridRadius() : Constants::CellGridRadius;
        mCurrentCell = &cell;

        mRendering.enableTerrain(cell.isExterior(), cell.getCell()->getWorldSpace());

        MWWorld::Ptr old = mWorld.getPlayerPtr();
        mWorld.getPlayer().setCell(&cell);

        MWWorld::Ptr player = mWorld.getPlayerPtr();
        mRendering.updatePlayerPtr(player);

        // The player is loaded before the scene and by default it is grounded, with the scene fully loaded,
        // we validate and correct this. Only run once, during initial cell load.
        if (old.mCell == &cell)
            mPhysics->traceDown(player, player.getRefData().getPosition().asVec3(), 10.f);

        if (adjustPlayerPos)
        {
            mWorld.moveObject(player, pos.asVec3());
            mWorld.rotateObject(player, pos.asRotationVec3());

            player.getClass().adjustPosition(player, true);
        }

        MWBase::Environment::get().getMechanicsManager()->updateCell(old, player);
        MWBase::Environment::get().getWindowManager()->watchActor(player);

        mPhysics->updatePtr(old, player);

        mWorld.adjustSky();

        mLastPlayerPos = player.getRefData().getPosition().asVec3();
    }

    Scene::Scene(MWWorld::World& world, MWRender::RenderingManager& rendering, MWPhysics::PhysicsSystem* physics,
        DetourNavigator::Navigator& navigator)
        : mCurrentCell(nullptr)
        , mCellChanged(false)
        , mWorld(world)
        , mPhysics(physics)
        , mRendering(rendering)
        , mNavigator(navigator)
        , mCellLoadingThreshold(1024.f)
        , mPreloadDistance(Settings::cells().mPreloadDistance)
        , mPreloadEnabled(Settings::cells().mPreloadEnabled)
        , mPreloadExteriorGrid(Settings::cells().mPreloadExteriorGrid)
        , mPreloadDoors(Settings::cells().mPreloadDoors)
        , mPreloadFastTravel(Settings::cells().mPreloadFastTravel)
        , mPredictionTime(Settings::cells().mPredictionTime)
        , mLowestPoint(std::numeric_limits<float>::max())
    {
        mPreloader = std::make_unique<CellPreloader>(rendering.getResourceSystem(), physics->getShapeManager(),
            rendering.getTerrain(), rendering.getLandManager());
        mPreloader->setWorkQueue(mRendering.getWorkQueue());
        mPreloader->setExpiryDelay(Settings::cells().mPreloadCellExpiryDelay);
        mPreloader->setMinCacheSize(Settings::cells().mPreloadCellCacheMin);
        mPreloader->setMaxCacheSize(Settings::cells().mPreloadCellCacheMax);
        mPreloader->setPreloadInstances(Settings::cells().mPreloadInstances);
    }

    Scene::~Scene()
    {
        for (const osg::ref_ptr<SceneUtil::WorkItem>& v : mWorkItems)
            v->abort();

        for (const osg::ref_ptr<SceneUtil::WorkItem>& v : mWorkItems)
            v->waitTillDone();
    }

    bool Scene::hasCellChanged() const
    {
        return mCellChanged;
    }

    const Scene::CellStoreCollection& Scene::getActiveCells() const
    {
        return mActiveCells;
    }

    void Scene::changeToInteriorCell(
        std::string_view cellName, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {
        CellStore& cell = mWorld.getWorldModel().getInterior(cellName);
        bool useFading = (mCurrentCell != nullptr);
        if (useFading)
            MWBase::Environment::get().getWindowManager()->fadeScreenOut(0.5);

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        loadingListener->setLabel("#{OMWEngine:LoadingInterior}");
        Loading::ScopedLoad load(loadingListener);

        if (mCurrentCell == &cell)
        {
            mWorld.moveObject(mWorld.getPlayerPtr(), position.asVec3());
            mWorld.rotateObject(mWorld.getPlayerPtr(), position.asRotationVec3());

            if (adjustPlayerPos)
                mWorld.getPlayerPtr().getClass().adjustPosition(mWorld.getPlayerPtr(), true);
            MWBase::Environment::get().getWindowManager()->fadeScreenIn(0.5);
            return;
        }

        Log(Debug::Info) << "Changing to interior";

        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();

        // unload
        for (auto iter = mActiveCells.begin(); iter != mActiveCells.end();)
        {
            auto* cellToUnload = *iter++;
            unloadCell(cellToUnload, navigatorUpdateGuard.get());
        }
        assert(mActiveCells.empty());

        loadingListener->setProgressRange(cell.count());

        mNavigator.updateBounds(
            cell.getCell()->getWorldSpace(), std::nullopt, position.asVec3(), navigatorUpdateGuard.get());

        // Load cell.
        mPagedRefs.clear();
        loadCell(cell, loadingListener, changeEvent, position.asVec3(), navigatorUpdateGuard.get());

        navigatorUpdateGuard.reset();

        changePlayerCell(cell, position, adjustPlayerPos);

        // adjust fog
        mRendering.configureFog(*mCurrentCell->getCell());

        // Sky system
        mWorld.adjustSky();

        if (changeEvent)
            mCellChanged = true;

        mCellLoaded = true;

        if (useFading)
            MWBase::Environment::get().getWindowManager()->fadeScreenIn(0.5);

        MWBase::Environment::get().getWindowManager()->changeCell(mCurrentCell);

        MWBase::Environment::get().getWorld()->getPostProcessor()->setExteriorFlag(cell.getCell()->isQuasiExterior());
    }

    void Scene::changeToExteriorCell(
        const ESM::RefId& extCellId, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {

        if (changeEvent)
            MWBase::Environment::get().getWindowManager()->fadeScreenOut(0.5);
        CellStore& current = mWorld.getWorldModel().getCell(extCellId);

        const osg::Vec2i cellIndex(current.getCell()->getGridX(), current.getCell()->getGridY());

        changeCellGrid(position.asVec3(),
            ESM::ExteriorCellLocation(cellIndex.x(), cellIndex.y(), current.getCell()->getWorldSpace()), changeEvent);

        changePlayerCell(current, position, adjustPlayerPos);

        if (changeEvent)
            MWBase::Environment::get().getWindowManager()->fadeScreenIn(0.5);

        MWBase::Environment::get().getWorld()->getPostProcessor()->setExteriorFlag(true);
    }

    CellStore* Scene::getCurrentCell()
    {
        return mCurrentCell;
    }

    void Scene::markCellAsUnchanged()
    {
        mCellChanged = false;
    }

    void Scene::insertCell(
        CellStore& cell, Loading::Listener* loadingListener, const DetourNavigator::UpdateGuard* navigatorUpdateGuard)
    {
        const bool isInterior = !cell.isExterior();
        const bool skipDistantEsm4Actors = envEnabled("OPENMW_WORLD_VIEWER_SKIP_DISTANT_ESM4_ACTORS")
            && cell.isExterior() && cell.getCell()->isEsm4()
            && osg::Vec2i(cell.getCell()->getGridX(), cell.getCell()->getGridY()) != mCurrentGridCenter;
        std::size_t skippedDistantEsm4Actors = 0;
        InsertVisitor insertVisitor(cell, loadingListener);
        cell.forEach(insertVisitor);
        insertVisitor.insert([&](const MWWorld::Ptr& ptr) {
            if (skipDistantEsm4Actors && isEsm4Actor(ptr) && ptr.getClass().isActor())
            {
                ++skippedDistantEsm4Actors;
                return;
            }
            addObject(ptr, mWorld, mPagedRefs, *mPhysics, mRendering);
        });
        insertVisitor.insert([&](const MWWorld::Ptr& ptr) {
            if (skipDistantEsm4Actors && isEsm4Actor(ptr) && ptr.getClass().isActor())
                return;
            addObject(ptr, mWorld, *mPhysics, mLowestPoint, isInterior, mNavigator, navigatorUpdateGuard);
        });
        if (skippedDistantEsm4Actors != 0)
            Log(Debug::Info) << "World viewer: kept connected ESM4 exterior geometry while deferring "
                             << skippedDistantEsm4Actors << " actor(s) in distant cell "
                             << cell.getCell()->getDescription();
    }

    void Scene::addObjectToScene(const Ptr& ptr)
    {
        const bool isInterior = mCurrentCell && !mCurrentCell->isExterior();
        try
        {
            addObject(ptr, mWorld, mPagedRefs, *mPhysics, mRendering);
            addObject(ptr, mWorld, *mPhysics, mLowestPoint, isInterior, mNavigator);
            mWorld.scaleObject(ptr, ptr.getCellRef().getScale());
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "failed to render '" << ptr.getCellRef().getRefId() << "': " << e.what();
        }
    }

    void Scene::removeObjectFromScene(const Ptr& ptr, bool keepActive)
    {
        MWBase::Environment::get().getMechanicsManager()->remove(ptr, keepActive);
        // You'd expect the sounds attached to the object to be stopped here
        // because the object is nowhere to be heard, but in Morrowind, they're not.
        // They're still stopped when the cell is unloaded
        // or if the player moves away far from the object's position.
        // Todd Howard, Who art in Bethesda, hallowed be Thy name.
        MWBase::Environment::get().getLuaManager()->objectRemovedFromScene(ptr);
        if (const auto object = mPhysics->getObject(ptr))
        {
            if (object->getShapeInstance()->mVisualCollisionType == Resource::VisualCollisionType::None)
                mNavigator.removeObject(DetourNavigator::ObjectId(object), nullptr);
        }
        else if (mPhysics->getActor(ptr))
        {
            mNavigator.removeAgent(mWorld.getPathfindingAgentBounds(ptr));
        }
        mPhysics->remove(ptr);
        mRendering.removeObject(ptr);
        if (ptr.getClass().isActor())
            mRendering.removeWaterRippleEmitter(ptr);
        ptr.getRefData().setBaseNode(nullptr);
    }

    bool Scene::isCellActive(const CellStore& cell)
    {
        return mActiveCells.contains(&cell);
    }

    Ptr Scene::searchPtrViaActorId(int actorId)
    {
        for (CellStoreCollection::const_iterator iter(mActiveCells.begin()); iter != mActiveCells.end(); ++iter)
        {
            Ptr ptr = (*iter)->searchViaActorId(actorId);
            if (!ptr.isEmpty())
                return ptr;
        }
        return Ptr();
    }

    class PreloadMeshItem : public SceneUtil::WorkItem
    {
    public:
        explicit PreloadMeshItem(VFS::Path::NormalizedView mesh, Resource::SceneManager* sceneManager)
            : mMesh(mesh)
            , mSceneManager(sceneManager)
        {
        }

        void doWork() override
        {
            if (mAborted)
                return;

            try
            {
                mSceneManager->getTemplate(mMesh);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Failed to get mesh template \"" << mMesh << "\" to preload: " << e.what();
            }
        }

        void abort() override { mAborted = true; }

    private:
        VFS::Path::Normalized mMesh;
        Resource::SceneManager* mSceneManager;
        std::atomic_bool mAborted{ false };
    };

    void Scene::preload(const std::string& mesh, bool useAnim)
    {
        const VFS::Path::Normalized meshPath = useAnim
            ? Misc::ResourceHelpers::correctActorModelPath(
                VFS::Path::toNormalized(mesh), mRendering.getResourceSystem()->getVFS())
            : VFS::Path::toNormalized(mesh);

        if (mRendering.getResourceSystem()->getSceneManager()->checkLoaded(meshPath, mRendering.getReferenceTime()))
            return;

        osg::ref_ptr<PreloadMeshItem> item(
            new PreloadMeshItem(meshPath, mRendering.getResourceSystem()->getSceneManager()));
        mRendering.getWorkQueue()->addWorkItem(item);
        const auto isDone = [](const osg::ref_ptr<SceneUtil::WorkItem>& v) { return v->isDone(); };
        mWorkItems.erase(std::remove_if(mWorkItems.begin(), mWorkItems.end(), isDone), mWorkItems.end());
        mWorkItems.emplace_back(std::move(item));
    }

    void Scene::preloadCells(float dt)
    {
        if (dt <= 1e-06)
            return;
        std::vector<PositionCellGrid> exteriorPositions;

        const MWWorld::ConstPtr player = mWorld.getPlayerPtr();
        osg::Vec3f playerPos = player.getRefData().getPosition().asVec3();
        osg::Vec3f moved = playerPos - mLastPlayerPos;
        osg::Vec3f predictedPos = playerPos + moved / dt * mPredictionTime;

        if (mCurrentCell->isExterior())
            exteriorPositions.push_back(PositionCellGrid{
                predictedPos, gridCenterToBounds(getNewGridCenter(predictedPos, &mCurrentGridCenter)) });

        mLastPlayerPos = playerPos;

        if (mPreloadEnabled)
        {
            if (mPreloadDoors)
                preloadTeleportDoorDestinations(playerPos, predictedPos);
            if (mPreloadExteriorGrid)
                preloadExteriorGrid(playerPos, predictedPos);
            if (mPreloadFastTravel)
                preloadFastTravelDestinations(playerPos, exteriorPositions);
        }

        mPreloader->setTerrainPreloadPositions(exteriorPositions);
    }

    void Scene::preloadTeleportDoorDestinations(const osg::Vec3f& playerPos, const osg::Vec3f& predictedPos)
    {
        struct TeleportDoorCandidate
        {
            MWWorld::ConstPtr mDoor;
            std::string_view mFormat;
        };

        std::vector<TeleportDoorCandidate> teleportDoors;
        for (const MWWorld::CellStore* cellStore : mActiveCells)
        {
            const auto appendTeleportDoors = [&](const auto& doors, std::string_view format) {
                forEachTeleportDoor(doors.mList, [&](const auto& door) {
                    teleportDoors.push_back({ MWWorld::ConstPtr(&door, cellStore), format });
                });
            };
            appendTeleportDoors(cellStore->getReadOnlyDoors(), "esm3");
            appendTeleportDoors(cellStore->getReadOnlyEsm4Doors(), "esm4");
        }

        const bool telemetryEnabled = [] {
            const char* value = std::getenv("OPENMW_WORLD_VIEWER_DOOR_PRELOAD_TELEMETRY");
            return value != nullptr && *value != '\0' && std::string_view(value) != "0";
        }();
        const auto preloadStateName = [](CellPreloader::PreloadState state) {
            switch (state)
            {
                case CellPreloader::PreloadState::NotRequested:
                    return "not-requested";
                case CellPreloader::PreloadState::Pending:
                    return "pending";
                case CellPreloader::PreloadState::Complete:
                    return "complete";
            }
            return "unknown";
        };

        for (const TeleportDoorCandidate& candidate : teleportDoors)
        {
            const MWWorld::ConstPtr& door = candidate.mDoor;
            float sqrDistToPlayer = (playerPos - door.getRefData().getPosition().asVec3()).length2();
            sqrDistToPlayer
                = std::min(sqrDistToPlayer, (predictedPos - door.getRefData().getPosition().asVec3()).length2());

            if (sqrDistToPlayer < mPreloadDistance * mPreloadDistance)
            {
                try
                {
                    MWWorld::CellStore& destination = mWorld.getWorldModel().getCell(door.getCellRef().getDestCell());
                    const CellPreloader::PreloadState stateBefore = mPreloader->getPreloadState(destination);
                    preloadCellWithSurroundings(destination);
                    const CellPreloader::PreloadState stateAfter = mPreloader->getPreloadState(destination);
                    const ESM::RefNum doorId = door.getCellRef().getRefNum();

                    if (telemetryEnabled && mTeleportDoorPreloadRequestsLogged.insert(doorId).second)
                    {
                        Log(Debug::Info) << "Teleport door preload telemetry: phase=requested format="
                                         << candidate.mFormat << " door=" << ESM::RefId(doorId).toDebugString()
                                         << " destCell=" << door.getCellRef().getDestCell().toDebugString()
                                         << " distance=" << std::sqrt(sqrDistToPlayer)
                                         << " stateBefore=" << preloadStateName(stateBefore)
                                         << " stateAfter=" << preloadStateName(stateAfter);
                    }
                    if (telemetryEnabled && stateAfter == CellPreloader::PreloadState::Complete
                        && mTeleportDoorPreloadCompletionsLogged.insert(doorId).second)
                    {
                        Log(Debug::Info) << "Teleport door preload telemetry: phase=complete format="
                                         << candidate.mFormat << " door=" << ESM::RefId(doorId).toDebugString()
                                         << " destCell=" << door.getCellRef().getDestCell().toDebugString()
                                         << " distance=" << std::sqrt(sqrDistToPlayer);
                    }
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "Failed to schedule preload for door " << door.toString() << ": "
                                        << e.what();
                }
            }
        }
    }

    void Scene::preloadExteriorGrid(const osg::Vec3f& playerPos, const osg::Vec3f& predictedPos)
    {
        if (!mWorld.isCellExterior())
            return;

        int halfGridSizePlusOne = mHalfGridSize + 1;

        int cellX, cellY;
        cellX = mCurrentGridCenter.x();
        cellY = mCurrentGridCenter.y();
        ESM::RefId extWorldspace = mWorld.getCurrentWorldspace();

        float cellSize = ESM::getCellSize(extWorldspace);

        for (int dx = -halfGridSizePlusOne; dx <= halfGridSizePlusOne; ++dx)
        {
            for (int dy = -halfGridSizePlusOne; dy <= halfGridSizePlusOne; ++dy)
            {
                if (dy != halfGridSizePlusOne && dy != -halfGridSizePlusOne && dx != halfGridSizePlusOne
                    && dx != -halfGridSizePlusOne)
                    continue; // only care about the outer (not yet loaded) part of the grid
                ESM::ExteriorCellLocation cellIndex(cellX + dx, cellY + dy, extWorldspace);
                const osg::Vec2f thisCellCenter = ESM::indexToPosition(cellIndex, true);

                float dist = std::max(
                    std::abs(thisCellCenter.x() - playerPos.x()), std::abs(thisCellCenter.y() - playerPos.y()));
                dist = std::min(dist,
                    std::max(std::abs(thisCellCenter.x() - predictedPos.x()),
                        std::abs(thisCellCenter.y() - predictedPos.y())));
                float loadDist = cellSize / 2 + cellSize - mCellLoadingThreshold + mPreloadDistance;

                if (dist < loadDist)
                    preloadCell(mWorld.getWorldModel().getExterior(cellIndex));
            }
        }
    }

    void Scene::preloadCellWithSurroundings(CellStore& cell)
    {
        if (!cell.isExterior())
        {
            mPreloader->preload(cell, mRendering.getReferenceTime());
            return;
        }

        const int cellX = cell.getCell()->getGridX();
        const int cellY = cell.getCell()->getGridY();

        std::vector<std::pair<int, int>> cells;
        const std::size_t gridSize = static_cast<std::size_t>(2 * mHalfGridSize + 1);
        cells.reserve(gridSize * gridSize);

        iterateOverCellsAround(cellX, cellY, mHalfGridSize, [&](int x, int y) { cells.emplace_back(x, y); });

        sortCellsToLoad(cellX, cellY, cells);

        const std::size_t leftCapacity = mPreloader->getMaxCacheSize() - mPreloader->getCacheSize();
        if (cells.size() > leftCapacity)
        {
            [[maybe_unused]] static const bool logged = [&] {
                Log(Debug::Warning) << "Not enough cell preloader cache capacity to preload exterior cells, consider "
                                       "increasing \"preload cell cache max\" up to "
                                    << (mPreloader->getCacheSize() + cells.size());
                return true;
            }();
            cells.resize(leftCapacity);
        }

        const ESM::RefId worldspace = cell.getCell()->getWorldSpace();
        for (const auto& [x, y] : cells)
            mPreloader->preload(mWorld.getWorldModel().getExterior(ESM::ExteriorCellLocation(x, y, worldspace)),
                mRendering.getReferenceTime());
    }

    void Scene::preloadCell(CellStore& cell)
    {
        mPreloader->preload(cell, mRendering.getReferenceTime());
    }

    void Scene::preloadTerrain(const osg::Vec3f& pos, ESM::RefId worldspace, bool sync)
    {
        if (mRendering.getTerrain()->getWorldspace() != worldspace)
            throw std::runtime_error("preloadTerrain can only work with the current exterior worldspace");

        ESM::ExteriorCellLocation cellPos = ESM::positionToExteriorCellLocation(pos.x(), pos.y(), worldspace);
        const PositionCellGrid position{ pos, gridCenterToBounds({ cellPos.mX, cellPos.mY }) };
        mPreloader->abortTerrainPreloadExcept(&position);
        mPreloader->setTerrainPreloadPositions(std::span(&position, 1));
        if (!sync)
            return;

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);

        loadingListener->setLabel("#{OMWEngine:InitializingData}");

        mPreloader->syncTerrainLoad(*loadingListener);
    }

    void Scene::reloadTerrain()
    {
        mPreloader->setTerrainPreloadPositions({});
    }

    struct ListFastTravelDestinationsVisitor
    {
        ListFastTravelDestinationsVisitor(float preloadDist, const osg::Vec3f& playerPos)
            : mPreloadDist(preloadDist)
            , mPlayerPos(playerPos)
        {
        }

        bool operator()(const MWWorld::Ptr& ptr)
        {
            if ((ptr.getRefData().getPosition().asVec3() - mPlayerPos).length2() > mPreloadDist * mPreloadDist)
                return true;

            if (ptr.getClass().isNpc())
            {
                const std::vector<ESM::Transport::Dest>& transport = ptr.get<ESM::NPC>()->mBase->mTransport.mList;
                mList.insert(mList.begin(), transport.begin(), transport.end());
            }
            else
            {
                const std::vector<ESM::Transport::Dest>& transport = ptr.get<ESM::Creature>()->mBase->mTransport.mList;
                mList.insert(mList.begin(), transport.begin(), transport.end());
            }
            return true;
        }
        float mPreloadDist;
        osg::Vec3f mPlayerPos;
        std::vector<ESM::Transport::Dest> mList;
    };

    void Scene::preloadFastTravelDestinations(
        const osg::Vec3f& playerPos, std::vector<PositionCellGrid>& exteriorPositions)
    {
        ListFastTravelDestinationsVisitor listVisitor(mPreloadDistance, playerPos);
        ESM::RefId extWorldspace = mWorld.getCurrentWorldspace();
        for (MWWorld::CellStore* cellStore : mActiveCells)
        {
            cellStore->forEachType<ESM::NPC>(listVisitor);
            cellStore->forEachType<ESM::Creature>(listVisitor);
        }

        for (ESM::Transport::Dest& dest : listVisitor.mList)
        {
            if (!dest.mCellName.empty())
                preloadCell(mWorld.getWorldModel().getInterior(dest.mCellName));
            else
            {
                osg::Vec3f pos = dest.mPos.asVec3();
                const ESM::ExteriorCellLocation cellIndex
                    = ESM::positionToExteriorCellLocation(pos.x(), pos.y(), extWorldspace);
                preloadCellWithSurroundings(mWorld.getWorldModel().getExterior(cellIndex));
                exteriorPositions.push_back(PositionCellGrid{ pos, gridCenterToBounds(getNewGridCenter(pos)) });
            }
        }
    }

    void Scene::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        mPreloader->reportStats(frameNumber, stats);
    }
}
