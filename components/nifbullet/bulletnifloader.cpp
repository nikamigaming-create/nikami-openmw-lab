#include "bulletnifloader.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <tuple>
#include <variant>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/nifstream.hpp>
#include <components/nif/node.hpp>
#include <components/nif/parent.hpp>
#include <components/nif/physics.hpp>

#include <BulletCollision/CollisionShapes/btTriangleIndexVertexArray.h>

namespace
{

    // Fallout 3 / New Vegas bhk positions use Havok units at a 1:7 ratio to scene-graph units.
    constexpr float sHavokToGameUnits = 7.f;

    struct TriangleMeshPart
    {
        std::vector<osg::Vec3f> mVertices;
        std::vector<std::array<std::uint16_t, 3>> mTriangles;
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
    };

    struct SubshapeRange
    {
        std::size_t mBegin = 0;
        std::size_t mEnd = 0;
        std::optional<std::uint32_t> mMaterial;
    };

    std::optional<PackedCollision> makePackedCollision(
        const Nif::bhkPackedNiTriStripsShape& packed, const Nif::bhkRigidBody& body, const osg::Matrixf& nodeTransform)
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
        Resource::CollisionShapeMaterialTable materials;
        std::optional<std::uint32_t> uniformMaterial;
        bool hasUniformMaterial = true;
        if (sourceParts.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return std::nullopt;
        for (std::size_t sourcePart = 0; sourcePart < sourceParts.size(); ++sourcePart)
        {
            if (sourceParts[sourcePart].mTriangles.empty())
                continue;
            const int shapePart = static_cast<int>(parts.size());
            parts.push_back(std::move(sourceParts[sourcePart]));
            if (ranges[sourcePart].mMaterial)
            {
                if (!materials.addShapePartMaterial(shapePart, *ranges[sourcePart].mMaterial))
                    return std::nullopt;
                if (!uniformMaterial)
                    uniformMaterial = ranges[sourcePart].mMaterial;
                else if (*uniformMaterial != *ranges[sourcePart].mMaterial)
                    hasUniformMaterial = false;
            }
            else
                hasUniformMaterial = false;
        }
        if (parts.empty())
            return std::nullopt;

        if (uniformMaterial && hasUniformMaterial && !materials.addUniformMaterial(*uniformMaterial))
            return std::nullopt;

        auto mesh = std::make_unique<OwnedTriangleIndexVertexArray>(std::move(parts));
        Resource::CollisionShapePtr shape(new Resource::TriangleMeshShape(mesh.release(), true));
        return PackedCollision{ std::move(shape), std::move(materials) };
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

    std::optional<PackedCollision> loadPackedCollision(const Nif::NiAVObject& node, const Nif::Parent* parent)
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
        return makePackedCollision(*packed, *body, getNodeTransform(node, parent));
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
        std::optional<PackedCollision> mCollision;
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
            // Animated and avoid bodies need distinct runtime ownership. Multiple bodies also remain distinct until
            // their motion and filter semantics can be represented without flattening them by guesswork.
            if (result.mCollision || animated || avoid)
            {
                result.mRejected = true;
                return;
            }
            auto collision = loadPackedCollision(node, parent);
            if (!collision)
            {
                result.mRejected = true;
                return;
            }
            result.mCollision = std::move(collision);
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
            if (foundCollisionRoot && search.mCollision && !search.mRejected)
            {
                mShape->mCollisionShape = std::move(search.mCollision->mShape);
                mShape->mCollisionShapeMaterials = std::move(search.mCollision->mMaterials);
                return mShape;
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
