#ifndef OPENMW_COMPONENTS_NIFBULLET_FALLOUTPHYSICSCONSTANTS_H
#define OPENMW_COMPONENTS_NIFBULLET_FALLOUTPHYSICSCONSTANTS_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace NifBullet::FalloutPhysics
{
    // Immutable Fallout NIF/Havok schema and algorithm boundaries. These are
    // deliberately named instead of being repeated in shape conversion code.
    inline constexpr float kHavokToGameUnits = 7.f;
    inline constexpr std::uint32_t kHavokMaterialMask = 0x1fU;
    inline constexpr std::size_t kMaximumShapeTraversalDepth = 64;
    inline constexpr float kShapeComparisonTolerance = 1e-5f;
    inline constexpr std::array<float, 4> kIdentityPackedScale{ 1.f, 1.f, 1.f, 0.f };
    inline constexpr std::size_t kMinimumTriangleVertexCount = 3;
    inline constexpr std::size_t kTriangleStripInitialIndex = 2;
    inline constexpr std::size_t kTriangleParityPeriod = 2;
    inline constexpr std::size_t kSingleCollisionCount = 1;
    inline constexpr std::size_t kMultipleCollisionCount = 2;
    inline constexpr std::size_t kMinimumConvexVertexCount = 4;
    inline constexpr std::size_t kVectorComponentCount = 3;
    inline constexpr std::size_t kHomogeneousComponentCount = 4;
    inline constexpr float kHalf = 0.5f;
    inline constexpr std::uint32_t kBethesdaCollisionFlag = 1U << 1;
    inline constexpr std::uint32_t kBethesdaMarkerFlag = 1U << 5;
    inline constexpr std::size_t kCollisionMarkerPrefixLength = 2;
}

#endif
