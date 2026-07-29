#include "fnvfurnitureplacement.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <string_view>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/node.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/vfs/pathutil.hpp>

#include <osg/Matrix>
#include <osg/Quat>

#include "../mwbase/environment.hpp"
#include "../mwworld/esmstore.hpp"

namespace
{
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

    osg::Vec3f transformFnvFurnitureMarkerOffset(osg::Vec3f offset)
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

    FnvFurnitureMarkerPlacement findFnvFurnitureMarker(const MWWorld::ESMStore& store, const ESM4::Reference& target)
    {
        FnvFurnitureMarkerPlacement fallback;
        const ESM4::Furniture* furniture = store.get<ESM4::Furniture>().search(target.mBaseObj);
        if (furniture == nullptr || furniture->mModel.empty())
            return fallback;

        try
        {
            const VFS::Path::Normalized model
                = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(furniture->mModel));
            Resource::ResourceSystem* const resourceSystem = MWBase::Environment::get().getResourceSystem();
            if (resourceSystem == nullptr)
                return fallback;
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

            if (markers.empty())
                return fallback;

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

            for (std::size_t i = 0; i < markers.size() && i < 32; ++i)
            {
                if ((furniture->mActiveMarkerFlags & (std::uint32_t{ 1 } << i)) != 0)
                {
                    Log(Debug::Verbose) << "FNV/ESM4 diag: selected active furniture marker index " << i;
                    return markers[i];
                }
            }

            const auto markerLess = [](const FnvFurnitureMarkerPlacement& left, const FnvFurnitureMarkerPlacement& right) {
                return std::abs(left.mOffset.x()) < std::abs(right.mOffset.x());
            };
            return *std::min_element(markers.begin(), markers.end(), markerLess);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: furniture marker scan failed targetRef=" << target.mEditorId
                                << " error=" << e.what();
        }

        return fallback;
    }

    osg::Vec3f rotateFnvFurnitureMarkerOffset(const osg::Vec3f& offset, float rotZ)
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
}

namespace MWClass
{
    FalloutFurniturePlacement makeFalloutFurniturePlacement(
        const MWWorld::ESMStore& store, const ESM4::Reference& furniture)
    {
        FalloutFurniturePlacement placement;
        const FnvFurnitureMarkerPlacement marker = findFnvFurnitureMarker(store, furniture);
        if (!marker.mFound || std::getenv("OPENMW_FNV_DISABLE_FURNITURE_MARKER_PLACEMENT") != nullptr)
            return placement;

        const osg::Vec3f proofOffset = transformFnvFurnitureMarkerOffset(marker.mOffset);
        const osg::Vec3f worldOffset = rotateFnvFurnitureMarkerOffset(proofOffset, furniture.mPos.rot[2]);
        placement.mEntryPosition = osg::Vec3f(furniture.mPos.pos[0] + worldOffset.x(),
            furniture.mPos.pos[1] + worldOffset.y(), furniture.mPos.pos[2] + worldOffset.z());
        placement.mSettledPosition = osg::Vec3f(
            furniture.mPos.pos[0], furniture.mPos.pos[1], furniture.mPos.pos[2] + marker.mOffset.z());
        placement.mEntryYaw = applyFnvFurnitureMarkerHeading(furniture.mPos.rot[2], marker.mHeading);
        placement.mSettledYaw = furniture.mPos.rot[2];
        placement.mFurnitureRef = furniture.mId;
        placement.mMarkerIndex = marker.mMarkerIndex;
        placement.mPositionRef = marker.mPositionRef;
        placement.mValid = true;

        const bool alongY = std::abs(marker.mOffset.y()) >= std::abs(marker.mOffset.x());
        const std::string_view direction = alongY ? (marker.mOffset.y() >= 0.f ? "forward" : "back")
                                                  : (marker.mOffset.x() < 0.f ? "left" : "right");
        placement.mEnterGroup = "chair" + std::string(direction) + "enter";
        placement.mExitGroup = "chair" + std::string(direction) + "exit";

        Log(Debug::Verbose) << "FNV/ESM4 diag: resolved furniture marker placement targetRef=" << furniture.mEditorId
                            << " markerIndex=" << static_cast<unsigned int>(placement.mMarkerIndex)
                            << " entryGroup=" << placement.mEnterGroup << " exitGroup=" << placement.mExitGroup
                            << " entry=(" << placement.mEntryPosition.x() << "," << placement.mEntryPosition.y()
                            << "," << placement.mEntryPosition.z() << ")";
        return placement;
    }
}
