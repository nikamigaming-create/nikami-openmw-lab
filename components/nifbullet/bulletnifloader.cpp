#include "bulletnifloader.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <variant>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/nif/data.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/nifstream.hpp>
#include <components/nif/node.hpp>
#include <components/nif/parent.hpp>
#include <components/nif/physics.hpp>

#include <BulletCollision/CollisionShapes/btBoxShape.h>
#include <BulletCollision/CollisionShapes/btCapsuleShape.h>
#include <BulletCollision/CollisionShapes/btConvexHullShape.h>
#include <BulletCollision/CollisionShapes/btSphereShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>
#include <BulletCollision/CollisionShapes/btTriangleIndexVertexArray.h>

namespace
{

    // Fallout 3 / New Vegas bhk positions use Havok units at a 1:7 ratio to scene-graph units.
    constexpr float sHavokToGameUnits = 7.f;

    struct TriangleMeshPart
    {
        std::vector<osg::Vec3f> mVertices;
        std::vector<std::array<std::uint16_t, 3>> mTriangles;
        std::optional<std::uint32_t> mMaterial;
    };

    class OwnedTriangleIndexVertexArray final : public btTriangleIndexVertexArray
    {
    public:
        explicit OwnedTriangleIndexVertexArray(std::vector<TriangleMeshPart> parts)
            : mParts(std::move(parts))
        {
            static_assert(sizeof(osg::Vec3f) == sizeof(float) * 3);
            static_assert(sizeof(std::array<std::uint16_t, 3>) == sizeof(std::uint16_t) * 3);

            for (const TriangleMeshPart& part : mParts)
            {
                btIndexedMesh indexedMesh;
                indexedMesh.m_numTriangles = static_cast<int>(part.mTriangles.size());
                indexedMesh.m_triangleIndexBase = reinterpret_cast<const unsigned char*>(part.mTriangles.data());
                indexedMesh.m_triangleIndexStride = static_cast<int>(sizeof(part.mTriangles.front()));
                indexedMesh.m_numVertices = static_cast<int>(part.mVertices.size());
                indexedMesh.m_vertexBase = reinterpret_cast<const unsigned char*>(part.mVertices.data());
                indexedMesh.m_vertexStride = static_cast<int>(sizeof(part.mVertices.front()));
                indexedMesh.m_vertexType = PHY_FLOAT;
                addIndexedMesh(indexedMesh, PHY_SHORT);
            }
        }

    private:
        std::vector<TriangleMeshPart> mParts;
    };

    struct PackedCollision
    {
        Resource::CollisionShapePtr mShape;
        Resource::CollisionShapeMaterialTable mMaterials;
        Resource::BethesdaCollisionFilter mFilter;
    };

    using HavokFilterKey = Resource::BethesdaHavokFilter;

    HavokFilterKey getFilterKey(const Nif::HavokFilter& filter)
    {
        return { .mLayer = filter.mLayer, .mFlags = filter.mFlags, .mGroup = filter.mGroup };
    }

    struct CollisionSemantics
    {
        Nif::HkMotionType mMotionType = Nif::HkMotionType::Motion_Invalid;
        std::uint16_t mCollisionFlags = 0;
        HavokFilterKey mWorldFilter;
        Nif::BroadPhaseType mBroadPhaseType = Nif::BroadPhaseType::BroadPhase_Invalid;
        std::uint32_t mWorldPropertyData = 0;
        std::uint32_t mWorldPropertySize = 0;
        std::uint32_t mWorldPropertyCapacityAndFlags = 0;
        Nif::HkResponseType mEntityResponseType = Nif::HkResponseType::Response_Invalid;
        std::uint16_t mEntityProcessContactDelay = 0;
        HavokFilterKey mBodyFilter;
        Nif::HkResponseType mBodyResponseType = Nif::HkResponseType::Response_Invalid;
        std::uint16_t mBodyProcessContactDelay = 0;
        float mFriction = 0.f;
        float mRollingFrictionMultiplier = 0.f;
        float mRestitution = 0.f;
        float mPenetrationDepth = 0.f;
        Nif::HkDeactivatorType mDeactivatorType = Nif::HkDeactivatorType::Deactivator_Invalid;
        bool mEnableDeactivation = false;
        Nif::HkSolverDeactivation mSolverDeactivation = Nif::HkSolverDeactivation::SolverDeactivation_Invalid;
        Nif::HkQualityType mQualityType = Nif::HkQualityType::Quality_Invalid;
        std::uint8_t mAutoRemoveLevel = 0;
        std::uint8_t mResponseModifierFlags = 0;
        std::uint8_t mNumContactPointShapeKeys = 0;
        bool mForceCollidedOntoPPU = false;
        std::uint32_t mBodyFlags = 0;
        std::set<HavokFilterKey> mSubshapeFilters;

        bool operator==(const CollisionSemantics&) const = default;
    };

    Resource::BethesdaCollisionFilter getCollisionFilter(const CollisionSemantics& semantics)
    {
        return {
            .mWorldObjectFilter = semantics.mWorldFilter,
            .mRigidBodyFilter = semantics.mBodyFilter,
            .mSubshapeFilters = { semantics.mSubshapeFilters.begin(), semantics.mSubshapeFilters.end() },
        };
    }

    struct PackedCollisionData
    {
        std::vector<TriangleMeshPart> mParts;
        CollisionSemantics mSemantics;
        bool mAnimated = false;
        int mRecordIndex = -1;
        osg::Matrixf mNodeTransform = osg::Matrixf::identity();
    };

    struct PrimitiveCollisionData
    {
        Resource::CollisionShapePtr mShape;
        osg::Matrixf mLocalTransform = osg::Matrixf::identity();
        osg::Matrixf mNodeTransform = osg::Matrixf::identity();
        Resource::CollisionShapeMaterialTable mMaterials;
        CollisionSemantics mSemantics;
        bool mAnimated = false;
        int mRecordIndex = -1;
    };

    CollisionSemantics getCollisionSemantics(const Nif::bhkRigidBody& body, const Nif::bhkCollisionObject& collision,
        std::set<HavokFilterKey> subshapeFilters)
    {
        const Nif::bhkEntity& entity = body;
        return {
            .mMotionType = body.mInfo.mMotionType,
            .mCollisionFlags = collision.mFlags,
            .mWorldFilter = getFilterKey(body.mHavokFilter),
            .mBroadPhaseType = body.mWorldObjectInfo.mPhaseType,
            .mWorldPropertyData = body.mWorldObjectInfo.mProperty.mData,
            .mWorldPropertySize = body.mWorldObjectInfo.mProperty.mSize,
            .mWorldPropertyCapacityAndFlags = body.mWorldObjectInfo.mProperty.mCapacityAndFlags,
            .mEntityResponseType = entity.mInfo.mResponseType,
            .mEntityProcessContactDelay = entity.mInfo.mProcessContactDelay,
            .mBodyFilter = getFilterKey(body.mInfo.mHavokFilter),
            .mBodyResponseType = body.mInfo.mResponseType,
            .mBodyProcessContactDelay = body.mInfo.mProcessContactDelay,
            .mFriction = body.mInfo.mFriction,
            .mRollingFrictionMultiplier = body.mInfo.mRollingFrictionMult,
            .mRestitution = body.mInfo.mRestitution,
            .mPenetrationDepth = body.mInfo.mPenetrationDepth,
            .mDeactivatorType = body.mInfo.mDeactivatorType,
            .mEnableDeactivation = body.mInfo.mEnableDeactivation,
            .mSolverDeactivation = body.mInfo.mSolverDeactivation,
            .mQualityType = body.mInfo.mQualityType,
            .mAutoRemoveLevel = body.mInfo.mAutoRemoveLevel,
            .mResponseModifierFlags = body.mInfo.mResponseModifierFlags,
            .mNumContactPointShapeKeys = body.mInfo.mNumContactPointShapeKeys,
            .mForceCollidedOntoPPU = body.mInfo.mForceCollidedOntoPPU,
            .mBodyFlags = body.mBodyFlags,
            .mSubshapeFilters = std::move(subshapeFilters),
        };
    }

    struct SubshapeRange
    {
        std::size_t mBegin = 0;
        std::size_t mEnd = 0;
        std::optional<std::uint32_t> mMaterial;
    };

    std::optional<PackedCollisionData> makePackedCollisionData(const Nif::bhkPackedNiTriStripsShape& packed,
        const Nif::bhkRigidBody& body, const Nif::bhkCollisionObject& collision, const osg::Matrixf& nodeTransform)
    {
        if (packed.mData.empty())
            return std::nullopt;

        const Nif::hkPackedNiTriStripsData& data = packed.mData.get();
        if (data.mVertices.empty() || data.mTriangles.empty()
            || data.mVertices.size() > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1)
            return std::nullopt;

        const std::vector<Nif::hkSubPartData>& subshapes
            = data.mSubshapes.empty() ? packed.mSubshapes : data.mSubshapes;
        std::vector<SubshapeRange> ranges;
        if (subshapes.empty())
            ranges.push_back({ 0, data.mVertices.size(), std::nullopt });
        else
        {
            ranges.reserve(subshapes.size());
            std::size_t begin = 0;
            for (const Nif::hkSubPartData& subshape : subshapes)
            {
                const std::size_t end = begin + subshape.mNumVertices;
                if (end > data.mVertices.size())
                    return std::nullopt;
                ranges.push_back({ begin, end, subshape.mHavokMaterial.mMaterial & 0x1f });
                begin = end;
            }
            if (begin != data.mVertices.size())
                return std::nullopt;
        }

        const auto transformVertex = [&](const osg::Vec3f& source) {
            osg::Vec3f result = source * sHavokToGameUnits;
            if (body.mRecordType == Nif::RC_bhkRigidBodyT)
            {
                result = body.mInfo.mRotation * result;
                result
                    += osg::Vec3f(body.mInfo.mTranslation.x(), body.mInfo.mTranslation.y(), body.mInfo.mTranslation.z())
                    * sHavokToGameUnits;
            }
            return nodeTransform.preMult(result);
        };

        std::vector<TriangleMeshPart> sourceParts(ranges.size());
        std::vector<std::size_t> vertexParts(data.mVertices.size());
        for (std::size_t part = 0; part < ranges.size(); ++part)
        {
            const SubshapeRange& range = ranges[part];
            sourceParts[part].mVertices.reserve(range.mEnd - range.mBegin);
            for (std::size_t vertex = range.mBegin; vertex < range.mEnd; ++vertex)
            {
                vertexParts[vertex] = part;
                sourceParts[part].mVertices.push_back(transformVertex(data.mVertices[vertex]));
            }
        }

        for (const Nif::TriangleData& sourceTriangle : data.mTriangles)
        {
            if (std::ranges::any_of(
                    sourceTriangle.mTriangle, [&](std::uint16_t vertex) { return vertex >= data.mVertices.size(); }))
                return std::nullopt;

            const std::size_t firstPart = vertexParts[sourceTriangle.mTriangle[0]];
            if (vertexParts[sourceTriangle.mTriangle[1]] != firstPart
                || vertexParts[sourceTriangle.mTriangle[2]] != firstPart)
                return std::nullopt;

            const std::size_t begin = ranges[firstPart].mBegin;
            sourceParts[firstPart].mTriangles.push_back(
                { static_cast<std::uint16_t>(sourceTriangle.mTriangle[0] - begin),
                    static_cast<std::uint16_t>(sourceTriangle.mTriangle[1] - begin),
                    static_cast<std::uint16_t>(sourceTriangle.mTriangle[2] - begin) });
        }

        std::vector<TriangleMeshPart> parts;
        for (std::size_t sourcePart = 0; sourcePart < sourceParts.size(); ++sourcePart)
        {
            if (sourceParts[sourcePart].mTriangles.empty())
                continue;
            sourceParts[sourcePart].mMaterial = ranges[sourcePart].mMaterial;
            parts.push_back(std::move(sourceParts[sourcePart]));
        }
        if (parts.empty())
            return std::nullopt;

        std::set<HavokFilterKey> subshapeFilters;
        for (const Nif::hkSubPartData& subshape : subshapes)
            subshapeFilters.emplace(getFilterKey(subshape.mHavokFilter));

        CollisionSemantics semantics = getCollisionSemantics(body, collision, std::move(subshapeFilters));
        return PackedCollisionData{ std::move(parts), std::move(semantics) };
    }

    bool canMergePackedCollisions(const std::vector<PackedCollisionData>& collisions)
    {
        if (collisions.size() < 2)
            return false;
        if (collisions.front().mAnimated || collisions.front().mSemantics.mMotionType != Nif::HkMotionType::Motion_Fixed
            || collisions.front().mSemantics.mSubshapeFilters.size() > 1)
            return false;
        return std::ranges::all_of(collisions.begin() + 1, collisions.end(), [&](const PackedCollisionData& collision) {
            return !collision.mAnimated && collision.mSemantics == collisions.front().mSemantics;
        });
    }

    std::optional<PackedCollision> makePackedCollision(std::vector<PackedCollisionData> collisions)
    {
        std::size_t partCount = 0;
        for (const PackedCollisionData& collision : collisions)
        {
            if (collision.mParts.size() > std::numeric_limits<std::size_t>::max() - partCount)
                return std::nullopt;
            partCount += collision.mParts.size();
        }
        if (partCount == 0 || partCount > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return std::nullopt;

        const Resource::BethesdaCollisionFilter filter = getCollisionFilter(collisions.front().mSemantics);

        std::vector<TriangleMeshPart> parts;
        parts.reserve(partCount);
        Resource::CollisionShapeMaterialTable materials;
        std::optional<std::uint32_t> uniformMaterial;
        bool hasUniformMaterial = true;
        for (PackedCollisionData& collision : collisions)
        {
            for (TriangleMeshPart& part : collision.mParts)
            {
                const int shapePart = static_cast<int>(parts.size());
                if (part.mMaterial)
                {
                    if (!materials.addShapePartMaterial(shapePart, *part.mMaterial))
                        return std::nullopt;
                    if (!uniformMaterial)
                        uniformMaterial = part.mMaterial;
                    else if (*uniformMaterial != *part.mMaterial)
                        hasUniformMaterial = false;
                }
                else
                    hasUniformMaterial = false;
                parts.push_back(std::move(part));
            }
        }

        if (uniformMaterial && hasUniformMaterial && !materials.addUniformMaterial(*uniformMaterial))
            return std::nullopt;

        auto mesh = std::make_unique<OwnedTriangleIndexVertexArray>(std::move(parts));
        Resource::CollisionShapePtr shape(new Resource::TriangleMeshShape(mesh.release(), true));
        return PackedCollision{ std::move(shape), std::move(materials), filter };
    }

    bool packedScaleMirrorsNode(const Nif::bhkPackedNiTriStripsShape& packed, const Nif::NiAVObject& node)
    {
        constexpr float tolerance = 1e-5f;
        const float nodeScale = node.mTransform.mScale;
        if (!std::isfinite(nodeScale))
            return false;
        for (unsigned int component = 0; component < 4; ++component)
        {
            const float value = packed.mScale[component];
            const float expected = component == 3 ? 0.f : nodeScale;
            if (!std::isfinite(value) || std::abs(value - expected) > tolerance)
                return false;
        }
        return true;
    }

    osg::Matrixf getNodeTransform(const Nif::NiAVObject& node, const Nif::Parent* parent)
    {
        osg::Matrixf transform = node.mTransform.toMatrix();
        for (const Nif::Parent* current = parent; current != nullptr; current = current->mParent)
            transform *= current->mNiNode.mTransform.toMatrix();
        return transform;
    }

    struct BulletTransform
    {
        btTransform mRigidTransform;
        btVector3 mScale;
    };

    BulletTransform decomposeTransform(osg::Matrixf transform)
    {
        const btVector3 scale = Misc::Convert::toBullet(transform.getScale());
        transform.orthoNormalize(transform);

        btTransform rigidTransform;
        rigidTransform.setOrigin(Misc::Convert::toBullet(transform.getTrans()));
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                rigidTransform.getBasis()[row][column] = transform(column, row);
        return { rigidTransform, scale };
    }

    bool isFinite(const osg::Vec3f& value)
    {
        return std::ranges::all_of(value._v, [](float component) { return std::isfinite(component); });
    }

    bool isFinite(const osg::Matrixf& value)
    {
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                if (!std::isfinite(value(row, column)))
                    return false;
        return true;
    }

    osg::Matrixf getRigidBodyTransform(const Nif::bhkRigidBody& body)
    {
        if (body.mRecordType != Nif::RC_bhkRigidBodyT)
            return osg::Matrixf::identity();

        osg::Matrixf result;
        result.makeRotate(body.mInfo.mRotation);
        result.setTrans(
            osg::Vec3f(body.mInfo.mTranslation.x(), body.mInfo.mTranslation.y(), body.mInfo.mTranslation.z())
            * sHavokToGameUnits);
        return result;
    }

    struct PrimitiveShapeData
    {
        Resource::CollisionShapePtr mShape;
        osg::Matrixf mTransform = osg::Matrixf::identity();
        Resource::CollisionShapeMaterialTable mMaterials;
        std::set<HavokFilterKey> mChildFilters;
    };

    struct ShapeStackGuard
    {
        std::set<const Nif::bhkShape*>& mStack;
        const Nif::bhkShape* mShape;

        ~ShapeStackGuard() { mStack.erase(mShape); }
    };

    std::optional<PrimitiveShapeData> makePrimitiveShape(
        const Nif::bhkShape& source, std::set<const Nif::bhkShape*>& stack)
    {
        if (stack.size() >= 64 || !stack.insert(&source).second)
            return std::nullopt;
        const ShapeStackGuard guard{ stack, &source };

        if (const auto* mopp = dynamic_cast<const Nif::bhkMoppBvTreeShape*>(&source))
        {
            if (mopp->mShape.empty())
                return std::nullopt;
            return makePrimitiveShape(mopp->mShape.get(), stack);
        }

        if (const auto* transformed = dynamic_cast<const Nif::bhkConvexTransformShape*>(&source))
        {
            if (transformed->mShape.empty())
                return std::nullopt;
            auto child = makePrimitiveShape(transformed->mShape.get(), stack);
            if (!child)
                return std::nullopt;
            osg::Matrixf transform = transformed->mTransform;
            if (!isFinite(transform))
                return std::nullopt;
            transform.setTrans(transform.getTrans() * sHavokToGameUnits);
            child->mTransform *= transform;
            child->mMaterials = {};
            if (!child->mMaterials.addUniformMaterial(transformed->mHavokMaterial.mMaterial & 0x1f))
                return std::nullopt;
            return child;
        }

        if (const auto* list = dynamic_cast<const Nif::bhkListShape*>(&source))
        {
            if (list->mSubshapes.empty()
                || list->mSubshapes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                return std::nullopt;
            if (!list->mHavokFilters.empty() && list->mHavokFilters.size() != list->mSubshapes.size())
                return std::nullopt;

            auto compound = std::make_unique<btCompoundShape>();
            Resource::CollisionShapeMaterialTable materials;
            std::set<HavokFilterKey> childFilters;
            std::optional<std::uint32_t> uniformMaterial;
            bool hasUniformMaterial = true;
            for (const Nif::bhkShapePtr& subshape : list->mSubshapes)
            {
                if (subshape.empty())
                    return std::nullopt;
                auto child = makePrimitiveShape(subshape.get(), stack);
                if (!child)
                    return std::nullopt;
                childFilters.insert(child->mChildFilters.begin(), child->mChildFilters.end());
                const std::optional<std::uint32_t> material = child->mMaterials.getMaterial(-1, -1);
                if (!material)
                    return std::nullopt;

                const BulletTransform transform = decomposeTransform(child->mTransform);
                child->mShape->setLocalScaling(transform.mScale);
                const int childIndex = compound->getNumChildShapes();
                compound->addChildShape(transform.mRigidTransform, child->mShape.release());
                if (!materials.addCompoundChildMaterial(childIndex, *material))
                    return std::nullopt;
                if (!uniformMaterial)
                    uniformMaterial = material;
                else if (*uniformMaterial != *material)
                    hasUniformMaterial = false;
            }
            for (const Nif::HavokFilter& filter : list->mHavokFilters)
                childFilters.insert(getFilterKey(filter));
            if (childFilters.size() > 1)
                return std::nullopt;
            if (uniformMaterial && hasUniformMaterial && !materials.addUniformMaterial(*uniformMaterial))
                return std::nullopt;

            PrimitiveShapeData result;
            result.mShape.reset(compound.release());
            result.mMaterials = std::move(materials);
            result.mChildFilters = std::move(childFilters);
            return result;
        }

        if (const auto* stripsShape = dynamic_cast<const Nif::bhkNiTriStripsShape*>(&source))
        {
            if (stripsShape->mData.empty()
                || !std::ranges::equal(stripsShape->mScale._v, std::array{ 1.f, 1.f, 1.f, 0.f }))
                return std::nullopt;

            std::size_t totalVertices = 0;
            for (const auto& data : stripsShape->mData)
            {
                if (data.empty() || data->mVertices.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
                    || data->mVertices.size()
                        > static_cast<std::size_t>(std::numeric_limits<int>::max()) - totalVertices)
                    return std::nullopt;
                totalVertices += data->mVertices.size();
            }
            if (totalVertices == 0)
                return std::nullopt;
            auto mesh = std::make_unique<btTriangleMesh>();
            mesh->preallocateVertices(static_cast<int>(totalVertices));
            int baseVertex = 0;
            for (const auto& dataPtr : stripsShape->mData)
            {
                const Nif::NiTriStripsData& data = dataPtr.get();
                for (const osg::Vec3f& vertex : data.mVertices)
                {
                    if (!isFinite(vertex))
                        return std::nullopt;
                    mesh->findOrAddVertex(Misc::Convert::toBullet(vertex), false);
                }
                for (const std::vector<unsigned short>& strip : data.mStrips)
                {
                    if (strip.size() < 3 || std::ranges::any_of(strip, [&](unsigned short index) {
                            return index >= data.mVertices.size();
                        }))
                        continue;
                    unsigned short second = strip[0];
                    unsigned short third = strip[1];
                    for (std::size_t index = 2; index < strip.size(); ++index)
                    {
                        const unsigned short first = second;
                        second = third;
                        third = strip[index];
                        if (first == second || second == third || first == third)
                            continue;
                        if (index % 2 == 0)
                            mesh->addTriangleIndices(baseVertex + first, baseVertex + second, baseVertex + third);
                        else
                            mesh->addTriangleIndices(baseVertex + first, baseVertex + third, baseVertex + second);
                    }
                }
                baseVertex += static_cast<int>(data.mVertices.size());
            }
            if (mesh->getNumTriangles() == 0)
                return std::nullopt;

            PrimitiveShapeData result;
            result.mShape.reset(new Resource::TriangleMeshShape(mesh.release(), true));
            if (!result.mMaterials.addUniformMaterial(stripsShape->mHavokMaterial.mMaterial & 0x1f))
                return std::nullopt;
            return result;
        }

        Resource::CollisionShapePtr shape;
        osg::Matrixf localTransform = osg::Matrixf::identity();
        const auto* materialShape = dynamic_cast<const Nif::bhkSphereRepShape*>(&source);
        if (materialShape == nullptr)
            return std::nullopt;

        switch (source.mRecordType)
        {
            case Nif::RC_bhkBoxShape:
            {
                const auto& box = static_cast<const Nif::bhkBoxShape&>(source);
                if (!isFinite(box.mExtents)
                    || std::ranges::any_of(box.mExtents._v, [](float value) { return value <= 0.f; })
                    || !std::isfinite(box.mRadius) || box.mRadius < 0.f)
                    return std::nullopt;
                const btVector3 halfExtents = Misc::Convert::toBullet(box.mExtents * sHavokToGameUnits);
                const btScalar margin = box.mRadius * sHavokToGameUnits;
                auto result = std::make_unique<btBoxShape>(halfExtents + btVector3(margin, margin, margin));
                result->setMargin(margin);
                shape.reset(result.release());
                break;
            }
            case Nif::RC_bhkSphereShape:
            {
                const auto& sphere = static_cast<const Nif::bhkSphereShape&>(source);
                if (!std::isfinite(sphere.mRadius) || sphere.mRadius <= 0.f)
                    return std::nullopt;
                shape.reset(new btSphereShape(sphere.mRadius * sHavokToGameUnits));
                break;
            }
            case Nif::RC_bhkCapsuleShape:
            {
                const auto& capsule = static_cast<const Nif::bhkCapsuleShape&>(source);
                constexpr float tolerance = 1e-5f;
                if (!isFinite(capsule.mPoint1) || !isFinite(capsule.mPoint2) || !std::isfinite(capsule.mRadius)
                    || capsule.mRadius <= 0.f || std::abs(capsule.mRadius1 - capsule.mRadius) > tolerance
                    || std::abs(capsule.mRadius2 - capsule.mRadius) > tolerance)
                    return std::nullopt;
                const osg::Vec3f axis = capsule.mPoint2 - capsule.mPoint1;
                const float height = axis.length();
                const osg::Vec3f center = (capsule.mPoint1 + capsule.mPoint2) * (0.5f * sHavokToGameUnits);
                if (height <= tolerance)
                    shape.reset(new btSphereShape(capsule.mRadius * sHavokToGameUnits));
                else
                {
                    shape.reset(new btCapsuleShape(capsule.mRadius * sHavokToGameUnits, height * sHavokToGameUnits));
                    osg::Quat rotation;
                    rotation.makeRotate(osg::Vec3f(0.f, 1.f, 0.f), axis);
                    localTransform.makeRotate(rotation);
                }
                localTransform.setTrans(center);
                break;
            }
            case Nif::RC_bhkConvexVerticesShape:
            {
                const auto& convex = static_cast<const Nif::bhkConvexVerticesShape&>(source);
                if (convex.mVertices.size() < 4
                    || convex.mVertices.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
                    || !std::isfinite(convex.mRadius) || convex.mRadius < 0.f)
                    return std::nullopt;
                auto result = std::make_unique<btConvexHullShape>();
                for (const osg::Vec4f& vertex : convex.mVertices)
                {
                    const osg::Vec3f point(vertex.x(), vertex.y(), vertex.z());
                    if (!isFinite(point))
                        return std::nullopt;
                    result->addPoint(Misc::Convert::toBullet(point * sHavokToGameUnits), false);
                }
                result->setMargin(convex.mRadius * sHavokToGameUnits);
                result->recalcLocalAabb();
                shape.reset(result.release());
                break;
            }
            default:
                return std::nullopt;
        }

        PrimitiveShapeData result;
        result.mShape = std::move(shape);
        result.mTransform = localTransform;
        if (!result.mMaterials.addUniformMaterial(materialShape->mHavokMaterial.mMaterial & 0x1f))
            return std::nullopt;
        return result;
    }

    std::optional<PrimitiveShapeData> makePrimitiveShape(const Nif::bhkShape& source)
    {
        std::set<const Nif::bhkShape*> stack;
        return makePrimitiveShape(source, stack);
    }

    std::optional<PrimitiveCollisionData> loadPrimitiveCollisionData(
        const Nif::NiAVObject& node, const Nif::Parent* parent, bool animated)
    {
        if (node.mCollision.empty())
            return std::nullopt;
        const auto* collision = dynamic_cast<const Nif::bhkCollisionObject*>(&node.mCollision.get());
        if (collision == nullptr || collision->mBody.empty())
            return std::nullopt;
        const auto* body = dynamic_cast<const Nif::bhkRigidBody*>(&collision->mBody.get());
        if (body == nullptr || body->mShape.empty())
            return std::nullopt;

        auto primitive = makePrimitiveShape(body->mShape.get());
        if (!primitive)
            return std::nullopt;
        primitive->mTransform *= getRigidBodyTransform(*body);

        PrimitiveCollisionData result;
        result.mShape = std::move(primitive->mShape);
        result.mLocalTransform = primitive->mTransform;
        result.mNodeTransform = getNodeTransform(node, parent);
        result.mMaterials = std::move(primitive->mMaterials);
        result.mSemantics = getCollisionSemantics(*body, *collision, std::move(primitive->mChildFilters));
        result.mAnimated = animated;
        if (animated)
        {
            if (node.mRecordIndex > static_cast<unsigned int>(std::numeric_limits<int>::max()))
                return std::nullopt;
            result.mRecordIndex = static_cast<int>(node.mRecordIndex);
        }
        return result;
    }

    bool canMergePrimitiveCollisions(const std::vector<PrimitiveCollisionData>& collisions)
    {
        if (collisions.size() < 2 || collisions.front().mAnimated
            || collisions.front().mSemantics.mMotionType != Nif::HkMotionType::Motion_Fixed
            || collisions.front().mSemantics.mSubshapeFilters.size() > 1)
            return false;
        return std::ranges::all_of(collisions, [&](const PrimitiveCollisionData& collision) {
            return !collision.mAnimated && !collision.mShape->isCompound()
                && collision.mSemantics == collisions.front().mSemantics
                && collision.mMaterials.getMaterial(-1, -1).has_value();
        });
    }

    std::optional<PackedCollision> makeMergedPrimitiveCollision(std::vector<PrimitiveCollisionData> collisions)
    {
        if (!canMergePrimitiveCollisions(collisions)
            || collisions.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return std::nullopt;

        const Resource::BethesdaCollisionFilter filter = getCollisionFilter(collisions.front().mSemantics);

        auto compound = std::make_unique<btCompoundShape>();
        Resource::CollisionShapeMaterialTable materials;
        std::optional<std::uint32_t> uniformMaterial;
        bool hasUniformMaterial = true;
        for (PrimitiveCollisionData& collision : collisions)
        {
            collision.mLocalTransform *= collision.mNodeTransform;
            const BulletTransform transform = decomposeTransform(collision.mLocalTransform);
            collision.mShape->setLocalScaling(transform.mScale);
            const int childIndex = compound->getNumChildShapes();
            compound->addChildShape(transform.mRigidTransform, collision.mShape.release());

            const std::uint32_t material = *collision.mMaterials.getMaterial(-1, -1);
            if (!materials.addCompoundChildMaterial(childIndex, material))
                return std::nullopt;
            if (!uniformMaterial)
                uniformMaterial = material;
            else if (*uniformMaterial != material)
                hasUniformMaterial = false;
        }
        if (uniformMaterial && hasUniformMaterial && !materials.addUniformMaterial(*uniformMaterial))
            return std::nullopt;

        return PackedCollision{ Resource::CollisionShapePtr(compound.release()), std::move(materials), filter };
    }

    std::optional<PackedCollisionData> loadPackedCollisionData(
        const Nif::NiAVObject& node, const Nif::Parent* parent, bool animated)
    {
        if (node.mCollision.empty())
            return std::nullopt;
        const auto* collision = dynamic_cast<const Nif::bhkCollisionObject*>(&node.mCollision.get());
        if (collision == nullptr || collision->mBody.empty())
            return std::nullopt;
        const auto* body = dynamic_cast<const Nif::bhkRigidBody*>(&collision->mBody.get());
        if (body == nullptr || body->mShape.empty())
            return std::nullopt;

        const Nif::bhkShape* shape = body->mShape.getPtr();
        if (const auto* mopp = dynamic_cast<const Nif::bhkMoppBvTreeShape*>(shape))
        {
            if (mopp->mShape.empty())
                return std::nullopt;
            shape = mopp->mShape.getPtr();
        }
        const auto* packed = dynamic_cast<const Nif::bhkPackedNiTriStripsShape*>(shape);
        // Bethesda's supported packed-shape layout mirrors the owning node scale. The node transform below already
        // applies it; reject independent/non-uniform scale rather than double-scaling it.
        if (packed == nullptr || !packedScaleMirrorsNode(*packed, node))
            return std::nullopt;

        const osg::Matrixf nodeTransform = getNodeTransform(node, parent);
        auto result
            = makePackedCollisionData(*packed, *body, *collision, animated ? osg::Matrixf::identity() : nodeTransform);
        if (!result)
            return std::nullopt;
        if (animated)
        {
            if (node.mRecordIndex > static_cast<unsigned int>(std::numeric_limits<int>::max()))
                return std::nullopt;
            result->mAnimated = true;
            result->mRecordIndex = static_cast<int>(node.mRecordIndex);
            result->mNodeTransform = nodeTransform;
        }
        return result;
    }

    bool hasActiveTransformController(const Nif::NiAVObject& node)
    {
        for (Nif::NiTimeControllerPtr controller = node.mController; !controller.empty();
            controller = controller->mNext)
        {
            if (!controller->isActive())
                continue;
            if (controller->mRecordType == Nif::RC_NiKeyframeController
                || controller->mRecordType == Nif::RC_NiPathController
                || controller->mRecordType == Nif::RC_NiRollController)
                return true;
        }
        return false;
    }

    struct PackedCollisionSearch
    {
        std::vector<PackedCollisionData> mCollisions;
        std::vector<PrimitiveCollisionData> mPrimitives;
        bool mRejected = false;
    };

    void findPackedCollision(const Nif::NiAVObject& node, const Nif::Parent* parent, bool inheritedAnimated,
        bool inheritedAvoid, PackedCollisionSearch& result)
    {
        if (result.mRejected || (node.mRecordType == Nif::RC_NiCollisionSwitch && !node.collisionActive()))
            return;

        const bool animated = inheritedAnimated || hasActiveTransformController(node);
        const bool avoid = inheritedAvoid || node.mRecordType == Nif::RC_AvoidNode;
        if (!node.mCollision.empty())
        {
            // Avoid bodies need a separate collision channel.
            if (avoid)
            {
                result.mRejected = true;
                return;
            }
            if (auto collision = loadPackedCollisionData(node, parent, animated))
            {
                if (!result.mPrimitives.empty())
                {
                    result.mRejected = true;
                    return;
                }
                result.mCollisions.push_back(std::move(*collision));
            }
            else if (auto primitive = loadPrimitiveCollisionData(node, parent, animated))
            {
                if (!result.mCollisions.empty())
                {
                    result.mRejected = true;
                    return;
                }
                result.mPrimitives.push_back(std::move(*primitive));
            }
            else
            {
                result.mRejected = true;
                return;
            }
        }

        const auto* parentNode = dynamic_cast<const Nif::NiNode*>(&node);
        if (parentNode == nullptr)
            return;
        const Nif::Parent currentParent{ *parentNode, parent };
        for (const Nif::NiAVObjectPtr& child : parentNode->mChildren)
        {
            if (!child.empty())
                findPackedCollision(child.get(), &currentParent, animated, avoid, result);
            if (result.mRejected || node.mRecordType == Nif::RC_NiSwitchNode
                || node.mRecordType == Nif::RC_NiFltAnimationNode)
                break;
        }
    }

    bool pathFileNameStartsWithX(const std::string& path)
    {
        const std::size_t slashpos = path.find_last_of("/\\");
        const std::size_t letterPos = slashpos == std::string::npos ? 0 : slashpos + 1;
        return letterPos < path.size() && (path[letterPos] == 'x' || path[letterPos] == 'X');
    }

    bool hasBethesdaCollisionFlag(const Nif::NiAVObject& root)
    {
        for (const auto& extra : root.getExtraList())
            if (!extra.empty() && extra->mRecordType == Nif::RC_BSXFlags
                && (static_cast<const Nif::NiIntegerExtraData&>(extra.get()).mData & 2) != 0)
                return true;
        return false;
    }

}

namespace NifBullet
{

    osg::ref_ptr<Resource::BulletShape> BulletNifLoader::load(Nif::FileView nif)
    {
        mShape = new Resource::BulletShape;

        mCompoundShape.reset();
        mAvoidCompoundShape.reset();

        mShape->mFileHash = nif.getHash();

        const size_t numRoots = nif.numRoots();
        std::vector<const Nif::NiAVObject*> roots;
        for (size_t i = 0; i < numRoots; ++i)
        {
            const Nif::Record* r = nif.getRoot(i);
            if (!r)
                continue;
            const Nif::NiAVObject* node = dynamic_cast<const Nif::NiAVObject*>(r);
            if (node)
                roots.emplace_back(node);
        }
        mShape->mFileName = nif.getFilename();
        if (roots.empty())
        {
            warn("Found no root nodes in NIF file " + mShape->mFileName.value());
            return mShape;
        }

        for (const Nif::NiAVObject* node : roots)
            if (findBoundingBox(*node))
                break;

        if (nif.getVersion() == Nif::NIFFile::NIFVersion::VER_BGS
            && nif.getBethVersion() == Nif::NIFFile::BethVersion::BETHVER_FO3)
        {
            // Treat the active collision hierarchy as one atomic contract. A root body does not make additional
            // descendant bodies disappear, and a rejected authored tree must not be mixed with generated geometry.
            PackedCollisionSearch search;
            const bool fileAnimated = pathFileNameStartsWithX(mShape->mFileName);
            bool foundCollisionRoot = false;
            for (const Nif::NiAVObject* root : roots)
            {
                if (!hasBethesdaCollisionFlag(*root))
                    continue;
                foundCollisionRoot = true;
                findPackedCollision(*root, nullptr, fileAnimated, false, search);
                if (search.mRejected)
                    break;
            }
            if (foundCollisionRoot && !search.mRejected && search.mPrimitives.size() == 1
                && search.mCollisions.empty())
            {
                PrimitiveCollisionData primitive = std::move(search.mPrimitives.front());
                if (!primitive.mMaterials.empty())
                {
                    mShape->mBethesdaCollisionFilter = getCollisionFilter(primitive.mSemantics);
                    if (primitive.mAnimated)
                    {
                        auto compound = std::make_unique<btCompoundShape>();
                        const BulletTransform local = decomposeTransform(primitive.mLocalTransform);
                        primitive.mShape->setLocalScaling(local.mScale);
                        auto localShape = std::make_unique<btCompoundShape>();
                        localShape->addChildShape(local.mRigidTransform, primitive.mShape.release());

                        const BulletTransform node = decomposeTransform(primitive.mNodeTransform);
                        localShape->setLocalScaling(node.mScale);
                        compound->addChildShape(node.mRigidTransform, localShape.release());
                        mShape->mCollisionShape.reset(compound.release());
                        mShape->mAnimatedShapes.emplace(primitive.mRecordIndex, 0);
                    }
                    else if (primitive.mShape->isCompound())
                    {
                        primitive.mLocalTransform *= primitive.mNodeTransform;
                        const BulletTransform transform = decomposeTransform(primitive.mLocalTransform);
                        auto* compound = static_cast<btCompoundShape*>(primitive.mShape.get());
                        compound->setLocalScaling(transform.mScale);
                        for (int child = 0; child < compound->getNumChildShapes(); ++child)
                        {
                            const btTransform childTransform
                                = transform.mRigidTransform * compound->getChildTransform(child);
                            compound->updateChildTransform(child, childTransform, false);
                        }
                        compound->recalculateLocalAabb();
                        mShape->mCollisionShape = std::move(primitive.mShape);
                    }
                    else
                    {
                        auto compound = std::make_unique<btCompoundShape>();
                        primitive.mLocalTransform *= primitive.mNodeTransform;
                        const BulletTransform transform = decomposeTransform(primitive.mLocalTransform);
                        primitive.mShape->setLocalScaling(transform.mScale);
                        compound->addChildShape(transform.mRigidTransform, primitive.mShape.release());
                        mShape->mCollisionShape.reset(compound.release());
                    }
                    mShape->mCollisionShapeMaterials = std::move(primitive.mMaterials);
                    return mShape;
                }
            }
            if (foundCollisionRoot && !search.mRejected && search.mCollisions.empty()
                && canMergePrimitiveCollisions(search.mPrimitives))
            {
                if (auto collision = makeMergedPrimitiveCollision(std::move(search.mPrimitives)))
                {
                    mShape->mCollisionShape = std::move(collision->mShape);
                    mShape->mCollisionShapeMaterials = std::move(collision->mMaterials);
                    mShape->mBethesdaCollisionFilter = std::move(collision->mFilter);
                    return mShape;
                }
            }
            const bool supportedBodyCount
                = search.mCollisions.size() == 1 || canMergePackedCollisions(search.mCollisions);
            if (foundCollisionRoot && !search.mRejected && supportedBodyCount)
            {
                const bool animated = search.mCollisions.size() == 1 && search.mCollisions.front().mAnimated;
                const int animatedRecordIndex = animated ? search.mCollisions.front().mRecordIndex : -1;
                const osg::Matrixf animatedTransform
                    = animated ? search.mCollisions.front().mNodeTransform : osg::Matrixf::identity();
                if (auto collision = makePackedCollision(std::move(search.mCollisions)))
                {
                    mShape->mCollisionShapeMaterials = std::move(collision->mMaterials);
                    mShape->mBethesdaCollisionFilter = std::move(collision->mFilter);
                    if (animated)
                    {
                        const BulletTransform transform = decomposeTransform(animatedTransform);
                        auto child = std::make_unique<Resource::ScaledTriangleMeshShape>(
                            static_cast<btBvhTriangleMeshShape*>(collision->mShape.release()), transform.mScale);
                        auto compound = std::make_unique<btCompoundShape>();
                        compound->addChildShape(transform.mRigidTransform, child.release());
                        mShape->mCollisionShape.reset(compound.release());
                        mShape->mAnimatedShapes.emplace(animatedRecordIndex, 0);
                    }
                    else
                        mShape->mCollisionShape = std::move(collision->mShape);
                    return mShape;
                }
            }
        }

        HandleNodeArgs args;

        // files with the name convention xmodel.nif usually have keyframes stored in a separate file xmodel.kf (see
        // Animation::addAnimSource). assume all nodes in the file will be animated
        // TODO: investigate whether this should and could be optimized.
        args.mAnimated = pathFileNameStartsWithX(mShape->mFileName);

        for (const Nif::NiAVObject* node : roots)
            handleRoot(nif, *node, args);

        if (mCompoundShape)
            mShape->mCollisionShape = std::move(mCompoundShape);

        if (mAvoidCompoundShape)
            mShape->mAvoidCollisionShape = std::move(mAvoidCompoundShape);

        return mShape;
    }

    // Find a bounding box in the node hierarchy to use for actor collision
    bool BulletNifLoader::findBoundingBox(const Nif::NiAVObject& node)
    {
        if (Misc::StringUtils::ciEqual(node.mName, "Bounding Box"))
        {
            if (node.mBounds.mType == Nif::BoundingVolume::Type::BOX_BV
                && std::ranges::all_of(node.mBounds.mBox.mExtents._v, [](float extent) { return extent > 0.f; }))
            {
                mShape->mCollisionBox.mExtents = node.mBounds.mBox.mExtents;
                mShape->mCollisionBox.mCenter = node.mBounds.mBox.mCenter;
            }
            else
            {
                warn("Invalid Bounding Box node bounds in file " + mShape->mFileName.value());
            }
            return true;
        }

        if (auto ninode = dynamic_cast<const Nif::NiNode*>(&node))
            for (const auto& child : ninode->mChildren)
                if (!child.empty() && findBoundingBox(child.get()))
                    return true;

        return false;
    }

    void BulletNifLoader::handleRoot(Nif::FileView nif, const Nif::NiAVObject& node, HandleNodeArgs args)
    {
        // Gamebryo/Bethbryo meshes
        if (nif.getVersion() >= Nif::NIFStream::generateVersion(10, 0, 1, 0))
        {
            // Handle BSXFlags
            const Nif::NiIntegerExtraData* bsxFlags = nullptr;
            for (const auto& e : node.getExtraList())
            {
                if (!e.empty() && e->mRecordType == Nif::RC_BSXFlags)
                {
                    bsxFlags = static_cast<const Nif::NiIntegerExtraData*>(e.getPtr());
                    break;
                }
            }

            // Collision flag
            if (!bsxFlags || !(bsxFlags->mData & 2))
                return;

            // Editor marker flag
            if (bsxFlags->mData & 32)
                args.mHasMarkers = true;

            // FIXME: hack, using rendered geometry instead of Bethesda Havok data
            args.mGenerateCollision = true;
        }
        // Pre-Gamebryo meshes
        else
        {
            bool recursiveRcn = false;
            // Check for extra data
            for (const auto& e : node.getExtraList())
            {
                if (!e.empty() && e->mRecordType == Nif::RC_NiStringExtraData)
                {
                    // String markers may contain important information
                    // affecting the entire subtree of this node
                    auto sd = static_cast<const Nif::NiStringExtraData*>(e.getPtr());

                    // Editor marker flag
                    if (sd->mData == "MRK")
                        args.mHasTriMarkers = true;
                    else if (Misc::StringUtils::ciStartsWith(sd->mData, "NC"))
                    {
                        // NC prefix is case-insensitive but the second C in NCC flag needs be uppercase.

                        // Collide only with camera.
                        if (sd->mData.length() > 2 && sd->mData[2] == 'C')
                            mShape->mVisualCollisionType = Resource::VisualCollisionType::Camera;
                        // No collision.
                        else
                            mShape->mVisualCollisionType = Resource::VisualCollisionType::Default;
                    }
                    else if (sd->mData == "RCN")
                        recursiveRcn = true;
                }
            }

            const Nif::NiNode* ninode = dynamic_cast<const Nif::NiNode*>(&node);
            if (ninode)
                args.mCollisionNode = ninode->findRootCollisionNode(recursiveRcn);
            if (!args.mCollisionNode)
                args.mGenerateCollision = true;
            else if (args.mCollisionNode->mChildren.empty())
            {
                // FIXME: BulletNifLoader should never have to provide rendered geometry for camera collision
                args.mGenerateCollision = true;
                mShape->mVisualCollisionType = Resource::VisualCollisionType::Camera;
            }
        }

        handleNode(node, nullptr, args);
    }

    void BulletNifLoader::handleNode(const Nif::NiAVObject& node, const Nif::Parent* parent, HandleNodeArgs args)
    {
        // TODO: allow on-the fly collision switching via toggling this flag
        if (node.mRecordType == Nif::RC_NiCollisionSwitch && !node.collisionActive())
            return;

        for (Nif::NiTimeControllerPtr ctrl = node.mController; !ctrl.empty(); ctrl = ctrl->mNext)
        {
            if (args.mAnimated)
                break;
            if (!ctrl->isActive())
                continue;
            switch (ctrl->mRecordType)
            {
                case Nif::RC_NiKeyframeController:
                case Nif::RC_NiPathController:
                case Nif::RC_NiRollController:
                    args.mAnimated = true;
                    break;
                default:
                    continue;
            }
        }

        if (node.mRecordType == Nif::RC_RootCollisionNode)
        {
            // Encountered our RootCollisionNode inside an autogenerated mesh.
            // We treat empty RootCollisionNodes as NCC flag (set collisionType to `Camera`)
            // and generate the camera collision shape based on rendered geometry.
            if (args.mCollisionNode == &node && args.mGenerateCollision
                && mShape->mVisualCollisionType == Resource::VisualCollisionType::Camera)
                return;

            // Standard handling
            if (!args.mCollisionNode)
            {
                Log(Debug::Info) << "BulletNifLoader: Unexpected RootCollisionNode in " << mShape->mFileName
                                 << ". Treating as visible geometry.";
            }
            else if (args.mCollisionNode != &node)
            {
                Log(Debug::Info) << "BulletNifLoader: Extra RootCollisionNode in " << mShape->mFileName
                                 << ". Treating as visible geometry.";
            }
            else
            {
                args.mGenerateCollision = true;
            }
        }

        // Don't collide with AvoidNode shapes
        if (node.mRecordType == Nif::RC_AvoidNode)
            args.mAvoid = true;

        if (args.mGenerateCollision)
        {
            auto geometry = dynamic_cast<const Nif::NiGeometry*>(&node);
            if (geometry)
                handleGeometry(*geometry, parent, args);
        }

        // For NiNodes, loop through children
        if (const Nif::NiNode* ninode = dynamic_cast<const Nif::NiNode*>(&node))
        {
            const Nif::Parent currentParent{ *ninode, parent };
            for (const auto& child : ninode->mChildren)
            {
                if (!child.empty())
                {
                    assert(std::find(child->mParents.begin(), child->mParents.end(), ninode) != child->mParents.end());
                    handleNode(child.get(), &currentParent, args);
                }
                // For NiSwitchNodes and NiFltAnimationNodes, only use the first child
                // TODO: must synchronize with the rendering scene graph somehow
                // Doing this for NiLODNodes is unsafe (the first level might not be the closest)
                if (node.mRecordType == Nif::RC_NiSwitchNode || node.mRecordType == Nif::RC_NiFltAnimationNode)
                    break;
            }
        }
    }

    void BulletNifLoader::handleGeometry(
        const Nif::NiGeometry& niGeometry, const Nif::Parent* nodeParent, HandleNodeArgs args)
    {
        // This flag comes from BSXFlags
        if (args.mHasMarkers && Misc::StringUtils::ciStartsWith(niGeometry.mName, "EditorMarker"))
            return;

        // This flag comes from Morrowind
        if (args.mHasTriMarkers && Misc::StringUtils::ciStartsWith(niGeometry.mName, "Tri EditorMarker"))
            return;

        if (!niGeometry.mSkin.empty())
            args.mAnimated = false;

        std::unique_ptr<btCollisionShape> childShape = niGeometry.getCollisionShape();
        if (childShape == nullptr)
            return;

        osg::Matrixf transform = niGeometry.mTransform.toMatrix();
        for (const Nif::Parent* parent = nodeParent; parent != nullptr; parent = parent->mParent)
            transform *= parent->mNiNode.mTransform.toMatrix();

        if (childShape->getShapeType() == TRIANGLE_MESH_SHAPE_PROXYTYPE)
        {
            auto scaledShape = std::make_unique<Resource::ScaledTriangleMeshShape>(
                static_cast<btBvhTriangleMeshShape*>(childShape.get()), Misc::Convert::toBullet(transform.getScale()));
            std::ignore = childShape.release();

            childShape = std::move(scaledShape);
        }
        else
        {
            childShape->setLocalScaling(Misc::Convert::toBullet(transform.getScale()));
        }

        transform.orthoNormalize(transform);

        btTransform trans;
        trans.setOrigin(Misc::Convert::toBullet(transform.getTrans()));
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                trans.getBasis()[i][j] = transform(j, i);

        if (!args.mAvoid)
        {
            if (!mCompoundShape)
                mCompoundShape.reset(new btCompoundShape);

            if (args.mAnimated)
                mShape->mAnimatedShapes.emplace(niGeometry.mRecordIndex, mCompoundShape->getNumChildShapes());
            mCompoundShape->addChildShape(trans, childShape.get());
        }
        else
        {
            if (!mAvoidCompoundShape)
                mAvoidCompoundShape.reset(new btCompoundShape);
            mAvoidCompoundShape->addChildShape(trans, childShape.get());
        }

        std::ignore = childShape.release();
    }

} // namespace NifBullet
