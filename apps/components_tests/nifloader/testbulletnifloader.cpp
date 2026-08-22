#include "../nif/node.hpp"

#include <components/bullethelpers/processtrianglecallback.hpp>
#include <components/misc/convert.hpp>
#include <components/nif/data.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/node.hpp>
#include <components/nif/physics.hpp>
#include <components/nifbullet/bulletnifloader.hpp>

#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>
#include <BulletCollision/CollisionShapes/btBoxShape.h>
#include <BulletCollision/CollisionShapes/btCapsuleShape.h>
#include <BulletCollision/CollisionShapes/btCompoundShape.h>
#include <BulletCollision/CollisionShapes/btConvexHullShape.h>
#include <BulletCollision/CollisionShapes/btSphereShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <type_traits>

namespace
{
    template <class T>
    bool compareObjects(const T* lhs, const T* rhs)
    {
        return (!lhs && !rhs) || (lhs && rhs && *lhs == *rhs);
    }

    std::vector<btVector3> getTriangles(const btBvhTriangleMeshShape& shape)
    {
        std::vector<btVector3> result;
        auto callback = BulletHelpers::makeProcessTriangleCallback([&](btVector3* triangle, int, int) {
            for (std::size_t i = 0; i < 3; ++i)
                result.push_back(triangle[i]);
        });
        btVector3 aabbMin;
        btVector3 aabbMax;
        shape.getAabb(btTransform::getIdentity(), aabbMin, aabbMax);
        shape.processAllTriangles(&callback, aabbMin, aabbMax);
        return result;
    }

    std::vector<std::pair<int, int>> getTriangleIdentities(const btBvhTriangleMeshShape& shape)
    {
        std::vector<std::pair<int, int>> result;
        auto callback = BulletHelpers::makeProcessTriangleCallback(
            [&](btVector3*, int shapePart, int triangleIndex) { result.emplace_back(shapePart, triangleIndex); });
        btVector3 aabbMin;
        btVector3 aabbMax;
        shape.getAabb(btTransform::getIdentity(), aabbMin, aabbMax);
        shape.processAllTriangles(&callback, aabbMin, aabbMax);
        return result;
    }

    struct ClosestRayResultWithIdentity final : btCollisionWorld::ClosestRayResultCallback
    {
        using btCollisionWorld::ClosestRayResultCallback::ClosestRayResultCallback;

        btScalar addSingleResult(btCollisionWorld::LocalRayResult& result, bool normalInWorldSpace) override
        {
            const btScalar fraction = ClosestRayResultCallback::addSingleResult(result, normalInWorldSpace);
            if (result.m_localShapeInfo != nullptr)
            {
                mShapePart = result.m_localShapeInfo->m_shapePart;
                mTriangleIndex = result.m_localShapeInfo->m_triangleIndex;
            }
            return fraction;
        }

        int mShapePart = -1;
        int mTriangleIndex = -1;
    };

    bool isNear(btScalar lhs, btScalar rhs)
    {
        return std::abs(lhs - rhs) <= 1e-5;
    }

    bool isNear(const btVector3& lhs, const btVector3& rhs)
    {
        return std::equal(static_cast<const btScalar*>(lhs), static_cast<const btScalar*>(lhs) + 3,
            static_cast<const btScalar*>(rhs), [](btScalar l, btScalar r) { return isNear(l, r); });
    }

    bool isNear(const btMatrix3x3& lhs, const btMatrix3x3& rhs)
    {
        for (int i = 0; i < 3; ++i)
            if (!isNear(lhs[i], rhs[i]))
                return false;
        return true;
    }

    bool isNear(const btTransform& lhs, const btTransform& rhs)
    {
        return isNear(lhs.getOrigin(), rhs.getOrigin()) && isNear(lhs.getBasis(), rhs.getBasis());
    }

    bool isNear(std::span<const btVector3> lhs, std::span<const btVector3> rhs)
    {
        if (lhs.size() != rhs.size())
            return false;
        return std::equal(
            lhs.begin(), lhs.end(), rhs.begin(), [](const btVector3& l, const btVector3& r) { return isNear(l, r); });
    }

    struct WriteVec3f
    {
        osg::Vec3f mValue;

        friend std::ostream& operator<<(std::ostream& stream, const WriteVec3f& value)
        {
            return stream << "osg::Vec3f {" << std::setprecision(std::numeric_limits<float>::max_exponent10)
                          << value.mValue.x() << ", " << std::setprecision(std::numeric_limits<float>::max_exponent10)
                          << value.mValue.y() << ", " << std::setprecision(std::numeric_limits<float>::max_exponent10)
                          << value.mValue.z() << "}";
        }
    };
}

static std::ostream& operator<<(std::ostream& stream, const btVector3& value)
{
    return stream << "btVector3 {" << std::setprecision(std::numeric_limits<float>::max_exponent10) << value.getX()
                  << ", " << std::setprecision(std::numeric_limits<float>::max_exponent10) << value.getY() << ", "
                  << std::setprecision(std::numeric_limits<float>::max_exponent10) << value.getZ() << "}";
}

static std::ostream& operator<<(std::ostream& stream, const btMatrix3x3& value)
{
    stream << "btMatrix3x3 {";
    for (int i = 0; i < 3; ++i)
        stream << value.getRow(i) << ", ";
    return stream << "}";
}

static std::ostream& operator<<(std::ostream& stream, const btTransform& value)
{
    return stream << "btTransform {" << value.getBasis() << ", " << value.getOrigin() << "}";
}

static std::ostream& operator<<(std::ostream& stream, const btCollisionShape* value);

static std::ostream& operator<<(std::ostream& stream, const btCompoundShape& value)
{
    stream << "btCompoundShape {" << value.getLocalScaling() << ", ";
    stream << "{";
    for (int i = 0; i < value.getNumChildShapes(); ++i)
        stream << value.getChildShape(i) << ", ";
    stream << "},";
    stream << "{";
    for (int i = 0; i < value.getNumChildShapes(); ++i)
        stream << value.getChildTransform(i) << ", ";
    stream << "}";
    return stream << "}";
}

static std::ostream& operator<<(std::ostream& stream, const btBoxShape& value)
{
    return stream << "btBoxShape {" << value.getLocalScaling() << ", " << value.getHalfExtentsWithoutMargin() << "}";
}

namespace Resource
{

    static std::ostream& operator<<(std::ostream& stream, const TriangleMeshShape& value)
    {
        stream << "Resource::TriangleMeshShape {" << value.getLocalScaling() << ", "
               << value.usesQuantizedAabbCompression() << ", " << value.getOwnsBvh() << ", {";
        auto callback = BulletHelpers::makeProcessTriangleCallback([&](btVector3* triangle, int, int) {
            for (std::size_t i = 0; i < 3; ++i)
                stream << triangle[i] << ", ";
        });
        btVector3 aabbMin;
        btVector3 aabbMax;
        value.getAabb(btTransform::getIdentity(), aabbMin, aabbMax);
        value.processAllTriangles(&callback, aabbMin, aabbMax);
        return stream << "}}";
    }

    static std::ostream& operator<<(std::ostream& stream, const ScaledTriangleMeshShape& value)
    {
        return stream << "Resource::ScaledTriangleMeshShape {" << value.getLocalScaling() << ", "
                      << value.getChildShape() << "}";
    }

    static bool operator==(const CollisionBox& l, const CollisionBox& r)
    {
        const auto tie = [](const CollisionBox& v) { return std::tie(v.mExtents, v.mCenter); };
        return tie(l) == tie(r);
    }

    static std::ostream& operator<<(std::ostream& stream, const CollisionBox& value)
    {
        return stream << "CollisionBox {" << WriteVec3f{ value.mExtents } << ", " << WriteVec3f{ value.mCenter } << "}";
    }

}

static std::ostream& operator<<(std::ostream& stream, const btCollisionShape& value)
{
    switch (value.getShapeType())
    {
        case COMPOUND_SHAPE_PROXYTYPE:
            return stream << static_cast<const btCompoundShape&>(value);
        case BOX_SHAPE_PROXYTYPE:
            return stream << static_cast<const btBoxShape&>(value);
        case TRIANGLE_MESH_SHAPE_PROXYTYPE:
            if (const auto casted = dynamic_cast<const Resource::TriangleMeshShape*>(&value))
                return stream << *casted;
            break;
        case SCALED_TRIANGLE_MESH_SHAPE_PROXYTYPE:
            if (const auto casted = dynamic_cast<const Resource::ScaledTriangleMeshShape*>(&value))
                return stream << *casted;
            break;
    }
    return stream << "btCollisionShape {" << value.getShapeType() << "}";
}

static std::ostream& operator<<(std::ostream& stream, const btCollisionShape* value)
{
    return value ? stream << "&" << *value : stream << "nullptr";
}

namespace std
{
    static std::ostream& operator<<(std::ostream& stream, const map<int, int>& value)
    {
        stream << "std::map<int, int> {";
        for (const auto& v : value)
            stream << "{" << v.first << ", " << v.second << "},";
        return stream << "}";
    }
}

namespace Resource
{
    static bool operator==(const Resource::BulletShape& lhs, const Resource::BulletShape& rhs)
    {
        return compareObjects(lhs.mCollisionShape.get(), rhs.mCollisionShape.get())
            && compareObjects(lhs.mAvoidCollisionShape.get(), rhs.mAvoidCollisionShape.get())
            && lhs.mCollisionBox == rhs.mCollisionBox && lhs.mVisualCollisionType == rhs.mVisualCollisionType
            && lhs.mAnimatedShapes == rhs.mAnimatedShapes
            && lhs.mCollisionShapeMaterials == rhs.mCollisionShapeMaterials;
    }

    static std::ostream& operator<<(std::ostream& stream, Resource::VisualCollisionType value)
    {
        switch (value)
        {
            case Resource::VisualCollisionType::None:
                return stream << "Resource::VisualCollisionType::None";
            case Resource::VisualCollisionType::Default:
                return stream << "Resource::VisualCollisionType::Default";
            case Resource::VisualCollisionType::Camera:
                return stream << "Resource::VisualCollisionType::Camera";
        }
        return stream << static_cast<std::underlying_type_t<Resource::VisualCollisionType>>(value);
    }

    static std::ostream& operator<<(std::ostream& stream, const Resource::BulletShape& value)
    {
        return stream << "Resource::BulletShape {" << value.mCollisionShape.get() << ", "
                      << value.mAvoidCollisionShape.get() << ", " << value.mCollisionBox << ", "
                      << value.mAnimatedShapes << ", " << value.mVisualCollisionType
                      << ", havokMaterials=" << value.mCollisionShapeMaterials.size() << "}";
    }
}

static bool operator==(const btCollisionShape& lhs, const btCollisionShape& rhs);

static bool operator==(const btCompoundShape& lhs, const btCompoundShape& rhs)
{
    if (lhs.getNumChildShapes() != rhs.getNumChildShapes() || lhs.getLocalScaling() != rhs.getLocalScaling())
        return false;
    for (int i = 0; i < lhs.getNumChildShapes(); ++i)
    {
        if (!compareObjects(lhs.getChildShape(i), rhs.getChildShape(i))
            || !isNear(lhs.getChildTransform(i), rhs.getChildTransform(i)))
            return false;
    }
    return true;
}

static bool operator==(const btBoxShape& lhs, const btBoxShape& rhs)
{
    return isNear(lhs.getLocalScaling(), rhs.getLocalScaling())
        && lhs.getHalfExtentsWithoutMargin() == rhs.getHalfExtentsWithoutMargin();
}

static bool operator==(const btBvhTriangleMeshShape& lhs, const btBvhTriangleMeshShape& rhs)
{
    return isNear(lhs.getLocalScaling(), rhs.getLocalScaling())
        && lhs.usesQuantizedAabbCompression() == rhs.usesQuantizedAabbCompression()
        && lhs.getOwnsBvh() == rhs.getOwnsBvh() && isNear(getTriangles(lhs), getTriangles(rhs));
}

static bool operator==(const btScaledBvhTriangleMeshShape& lhs, const btScaledBvhTriangleMeshShape& rhs)
{
    return isNear(lhs.getLocalScaling(), rhs.getLocalScaling())
        && compareObjects(lhs.getChildShape(), rhs.getChildShape());
}

static bool operator==(const btCollisionShape& lhs, const btCollisionShape& rhs)
{
    if (lhs.getShapeType() != rhs.getShapeType())
        return false;
    switch (lhs.getShapeType())
    {
        case COMPOUND_SHAPE_PROXYTYPE:
            return static_cast<const btCompoundShape&>(lhs) == static_cast<const btCompoundShape&>(rhs);
        case BOX_SHAPE_PROXYTYPE:
            return static_cast<const btBoxShape&>(lhs) == static_cast<const btBoxShape&>(rhs);
        case TRIANGLE_MESH_SHAPE_PROXYTYPE:
            if (const auto lhsCasted = dynamic_cast<const Resource::TriangleMeshShape*>(&lhs))
                if (const auto rhsCasted = dynamic_cast<const Resource::TriangleMeshShape*>(&rhs))
                    return *lhsCasted == *rhsCasted;
            return false;
        case SCALED_TRIANGLE_MESH_SHAPE_PROXYTYPE:
            if (const auto lhsCasted = dynamic_cast<const Resource::ScaledTriangleMeshShape*>(&lhs))
                if (const auto rhsCasted = dynamic_cast<const Resource::ScaledTriangleMeshShape*>(&rhs))
                    return *lhsCasted == *rhsCasted;
            return false;
    }
    return false;
}

namespace
{
    using namespace testing;
    using namespace Nif::Testing;
    using NifBullet::BulletNifLoader;

    constexpr VFS::Path::NormalizedView testNif("test.nif");
    constexpr VFS::Path::NormalizedView xtestNif("xtest.nif");

    struct PackedCollisionRecords
    {
        Nif::bhkCollisionObject mCollision;
        Nif::bhkRigidBody mBody;
        Nif::bhkMoppBvTreeShape mMopp;
        Nif::bhkPackedNiTriStripsShape mPacked;
        Nif::hkPackedNiTriStripsData mData;

        explicit PackedCollisionRecords(Nif::RecordType bodyType = Nif::RC_bhkRigidBody)
        {
            mCollision.mRecordType = Nif::RC_bhkCollisionObject;
            mCollision.mFlags = 1;
            mBody.mRecordType = bodyType;
            mMopp.mRecordType = Nif::RC_bhkMoppBvTreeShape;
            mPacked.mRecordType = Nif::RC_bhkPackedNiTriStripsShape;
            mData.mRecordType = Nif::RC_hkPackedNiTriStripsData;
            mBody.mInfo.mRotation = osg::Quat();
            mBody.mInfo.mTranslation = osg::Vec4f();
            mBody.mHavokFilter = { 1, 0, 0 };
            mBody.mWorldObjectInfo.mPhaseType = Nif::BroadPhaseType::BroadPhase_Entity;
            mBody.mWorldObjectInfo.mProperty = { 0, 0, 0 };
            static_cast<Nif::bhkEntity&>(mBody).mInfo.mResponseType = Nif::HkResponseType::Response_SimpleContact;
            static_cast<Nif::bhkEntity&>(mBody).mInfo.mProcessContactDelay = 0;
            mBody.mInfo.mHavokFilter = { 1, 0, 0 };
            mBody.mInfo.mResponseType = Nif::HkResponseType::Response_SimpleContact;
            mBody.mInfo.mProcessContactDelay = 0;
            mBody.mInfo.mFriction = 0.5f;
            mBody.mInfo.mRollingFrictionMult = 0.f;
            mBody.mInfo.mRestitution = 0.f;
            mBody.mInfo.mPenetrationDepth = 0.f;
            mBody.mInfo.mMotionType = Nif::HkMotionType::Motion_Fixed;
            mBody.mInfo.mDeactivatorType = Nif::HkDeactivatorType::Deactivator_Never;
            mBody.mInfo.mEnableDeactivation = false;
            mBody.mInfo.mSolverDeactivation = Nif::HkSolverDeactivation::SolverDeactivation_Off;
            mBody.mInfo.mQualityType = Nif::HkQualityType::Quality_Fixed;
            mBody.mInfo.mAutoRemoveLevel = 0;
            mBody.mInfo.mResponseModifierFlags = 0;
            mBody.mInfo.mNumContactPointShapeKeys = 0;
            mBody.mInfo.mForceCollidedOntoPPU = false;
            mBody.mBodyFlags = 0;
            mPacked.mScale = osg::Vec4f(1.f, 1.f, 1.f, 0.f);
            mData.mVertices = { osg::Vec3f(0.f, 0.f, 0.f), osg::Vec3f(1.f, 0.f, 0.f), osg::Vec3f(0.f, 1.f, 0.f) };
            mData.mTriangles.resize(1);
            mData.mTriangles.front().mTriangle = { 0, 1, 2 };
            mData.mSubshapes.resize(1);
            mData.mSubshapes.front().mNumVertices = 3;
            mData.mSubshapes.front().mHavokMaterial.mMaterial = 5;
            mData.mSubshapes.front().mHavokFilter = { 1, 0, 0 };
            mPacked.mData = Nif::hkPackedNiTriStripsDataPtr(&mData);
            mMopp.mShape = Nif::bhkShapePtr(&mPacked);
            mBody.mShape = Nif::bhkShapePtr(&mMopp);
            mCollision.mBody = Nif::bhkWorldObjectPtr(&mBody);
        }

        void attach(Nif::NiAVObject& node) { node.mCollision = Nif::NiCollisionObjectPtr(&mCollision); }
    };

    template <class Shape>
    struct PrimitiveCollisionRecords
    {
        Nif::bhkCollisionObject mCollision;
        Nif::bhkRigidBody mBody;
        Shape mShape;

        PrimitiveCollisionRecords(Nif::RecordType shapeType, Nif::RecordType bodyType = Nif::RC_bhkRigidBody)
        {
            mCollision.mRecordType = Nif::RC_bhkCollisionObject;
            mCollision.mFlags = 1;
            mBody.mRecordType = bodyType;
            mBody.mInfo.mRotation = osg::Quat();
            mBody.mInfo.mTranslation = osg::Vec4f();
            mBody.mHavokFilter = { 1, 0, 0 };
            mBody.mWorldObjectInfo.mPhaseType = Nif::BroadPhaseType::BroadPhase_Entity;
            mBody.mWorldObjectInfo.mProperty = { 0, 0, 0 };
            static_cast<Nif::bhkEntity&>(mBody).mInfo.mResponseType = Nif::HkResponseType::Response_SimpleContact;
            static_cast<Nif::bhkEntity&>(mBody).mInfo.mProcessContactDelay = 0;
            mBody.mInfo.mHavokFilter = { 1, 0, 0 };
            mBody.mInfo.mResponseType = Nif::HkResponseType::Response_SimpleContact;
            mBody.mInfo.mProcessContactDelay = 0;
            mBody.mInfo.mFriction = 0.5f;
            mBody.mInfo.mRollingFrictionMult = 0.f;
            mBody.mInfo.mRestitution = 0.f;
            mBody.mInfo.mPenetrationDepth = 0.f;
            mBody.mInfo.mMotionType = Nif::HkMotionType::Motion_Fixed;
            mBody.mInfo.mDeactivatorType = Nif::HkDeactivatorType::Deactivator_Never;
            mBody.mInfo.mEnableDeactivation = false;
            mBody.mInfo.mSolverDeactivation = Nif::HkSolverDeactivation::SolverDeactivation_Off;
            mBody.mInfo.mQualityType = Nif::HkQualityType::Quality_Fixed;
            mBody.mInfo.mAutoRemoveLevel = 0;
            mBody.mInfo.mResponseModifierFlags = 0;
            mBody.mInfo.mNumContactPointShapeKeys = 0;
            mBody.mInfo.mForceCollidedOntoPPU = false;
            mBody.mBodyFlags = 0;
            mShape.mRecordType = shapeType;
            mBody.mShape = Nif::bhkShapePtr(&mShape);
            mCollision.mBody = Nif::bhkWorldObjectPtr(&mBody);
        }

        void attach(Nif::NiAVObject& node) { node.mCollision = Nif::NiCollisionObjectPtr(&mCollision); }
    };

    void copy(const btTransform& src, Nif::NiTransform& dst)
    {
        dst.mTranslation = Misc::Convert::makeOsgVec3f(src.getOrigin());
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                dst.mRotation.mValues[row][column] = static_cast<float>(src.getBasis().getRow(row)[column]);
    }

    struct TestBulletNifLoader : Test
    {
        BulletNifLoader mLoader;
        Nif::NiAVObject mNode;
        Nif::NiAVObject mNode2;
        Nif::NiNode mNiNode;
        Nif::NiNode mNiNode2;
        Nif::NiNode mNiNode3;
        Nif::NiTriShapeData mNiTriShapeData;
        Nif::NiTriShape mNiTriShape;
        Nif::NiTriShapeData mNiTriShapeData2;
        Nif::NiTriShape mNiTriShape2;
        Nif::NiTriStripsData mNiTriStripsData;
        Nif::NiTriStrips mNiTriStrips;
        Nif::NiSkinInstance mNiSkinInstance;
        Nif::NiStringExtraData mNiStringExtraData;
        Nif::NiStringExtraData mNiStringExtraData2;
        Nif::NiIntegerExtraData mNiIntegerExtraData;
        Nif::NiTimeController mController;
        btTransform mTransform{ btMatrix3x3(btQuaternion(btVector3(1, 0, 0), 0.5f)), btVector3(1, 2, 3) };
        btTransform mTransformScale2{ btMatrix3x3(btQuaternion(btVector3(1, 0, 0), 0.5f)), btVector3(2, 4, 6) };
        btTransform mTransformScale3{ btMatrix3x3(btQuaternion(btVector3(1, 0, 0), 0.5f)), btVector3(3, 6, 9) };
        btTransform mTransformScale4{ btMatrix3x3(btQuaternion(btVector3(1, 0, 0), 0.5f)), btVector3(4, 8, 12) };
        const std::string mHash = "hash";

        TestBulletNifLoader()
        {
            init(mNode);
            init(mNode2);
            init(mNiNode);
            init(mNiNode2);
            init(mNiNode3);
            init(mNiTriShape);
            init(mNiTriShape2);
            init(mNiTriStrips);
            init(mNiSkinInstance);
            init(mNiStringExtraData);
            init(mNiStringExtraData2);
            init(static_cast<Nif::Extra&>(mNiIntegerExtraData));
            init(mController);

            mNiTriShapeData.mRecordType = Nif::RC_NiTriShapeData;
            mNiTriShapeData.mVertices = { osg::Vec3f(0, 0, 0), osg::Vec3f(1, 0, 0), osg::Vec3f(1, 1, 0) };
            mNiTriShapeData.mNumTriangles = 1;
            mNiTriShapeData.mTriangles = { 0, 1, 2 };
            mNiTriShape.mData = Nif::NiGeometryDataPtr(&mNiTriShapeData);

            mNiTriShapeData2.mRecordType = Nif::RC_NiTriShapeData;
            mNiTriShapeData2.mVertices = { osg::Vec3f(0, 0, 1), osg::Vec3f(1, 0, 1), osg::Vec3f(1, 1, 1) };
            mNiTriShapeData2.mNumTriangles = 1;
            mNiTriShapeData2.mTriangles = { 0, 1, 2 };
            mNiTriShape2.mData = Nif::NiGeometryDataPtr(&mNiTriShapeData2);

            mNiTriStripsData.mRecordType = Nif::RC_NiTriStripsData;
            mNiTriStripsData.mVertices
                = { osg::Vec3f(0, 0, 0), osg::Vec3f(1, 0, 0), osg::Vec3f(1, 1, 0), osg::Vec3f(0, 1, 0) };
            mNiTriStripsData.mNumTriangles = 2;
            mNiTriStripsData.mStrips = { { 0, 1, 2, 3 } };
            mNiTriStrips.mData = Nif::NiGeometryDataPtr(&mNiTriStripsData);
        }

        void enableBethesdaCollision(Nif::NiAVObject& root)
        {
            mNiIntegerExtraData.mData = 2;
            mNiIntegerExtraData.mRecordType = Nif::RC_BSXFlags;
            root.mExtraList.push_back(Nif::ExtraPtr(&mNiIntegerExtraData));
        }

        osg::ref_ptr<Resource::BulletShape> loadFallout(Nif::NiAVObject& root)
        {
            Nif::NIFFile file(testNif);
            file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
            file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
            file.mRoots.push_back(&root);
            return mLoader.load(file);
        }
    };

    TEST_F(TestBulletNifLoader, for_zero_num_roots_should_return_default)
    {
        Nif::NIFFile file(testNif);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
        EXPECT_EQ(result->mFileName, "test.nif");
        EXPECT_EQ(result->mFileHash, mHash);
    }

    TEST(BulletShapeMaterialTest, resolvesExactPartAndUniformMaterialsInPrecedenceOrder)
    {
        Resource::BulletShape shape;
        ASSERT_TRUE(shape.mCollisionShapeMaterials.addUniformMaterial(1));
        ASSERT_TRUE(shape.mCollisionShapeMaterials.addShapePartMaterial(2, 3));
        ASSERT_TRUE(shape.mCollisionShapeMaterials.addTriangleMaterial(2, 4, 5));

        EXPECT_EQ(shape.getHavokMaterial(2, 4), 5u);
        EXPECT_EQ(shape.getHavokMaterial(2, 8), 3u);
        EXPECT_EQ(shape.getHavokMaterial(7, 8), 1u);
        EXPECT_EQ(shape.getHavokMaterial(-1, -1), 1u);
    }

    TEST(BulletShapeMaterialTest, rejectsInvalidAndConflictingMaterialKeys)
    {
        Resource::CollisionShapeMaterialTable materials;

        EXPECT_FALSE(materials.addShapePartMaterial(-1, 1));
        EXPECT_FALSE(materials.addTriangleMaterial(2, -1, 1));
        EXPECT_FALSE(materials.addCompoundChildMaterial(-1, 1));
        EXPECT_TRUE(materials.addShapePartMaterial(2, 3));
        EXPECT_TRUE(materials.addShapePartMaterial(2, 3));
        EXPECT_FALSE(materials.addShapePartMaterial(2, 4));
        EXPECT_TRUE(materials.addCompoundChildMaterial(5, 7));
        EXPECT_EQ(materials.getMaterial(-1, 5), 7u);
    }

    TEST(BulletShapeMaterialTest, leavesUnmappedHitsUnresolved)
    {
        const Resource::BulletShape shape;

        EXPECT_FALSE(shape.getHavokMaterial(0, 0).has_value());
    }

    TEST(BulletShapeMaterialTest, reportsCompoundConvexChildIdentity)
    {
        btCompoundShape compound;
        btBoxShape first(btVector3(1.f, 1.f, 1.f));
        btBoxShape second(btVector3(1.f, 1.f, 1.f));
        btTransform firstTransform = btTransform::getIdentity();
        btTransform secondTransform = btTransform::getIdentity();
        secondTransform.setOrigin(btVector3(10.f, 0.f, 0.f));
        compound.addChildShape(firstTransform, &first);
        compound.addChildShape(secondTransform, &second);

        btCollisionObject object;
        object.setCollisionShape(&compound);
        const btVector3 rayFrom(10.f, 0.f, 10.f);
        const btVector3 rayTo(10.f, 0.f, -10.f);
        ClosestRayResultWithIdentity callback(rayFrom, rayTo);
        btTransform fromTransform = btTransform::getIdentity();
        btTransform toTransform = btTransform::getIdentity();
        fromTransform.setOrigin(rayFrom);
        toTransform.setOrigin(rayTo);
        btCollisionWorld::rayTestSingle(
            fromTransform, toTransform, &object, &compound, btTransform::getIdentity(), callback);

        ASSERT_TRUE(callback.hasHit());
        EXPECT_EQ(callback.mShapePart, -1);
        EXPECT_EQ(callback.mTriangleIndex, 1);

        Resource::CollisionShapeMaterialTable materials;
        ASSERT_TRUE(materials.addCompoundChildMaterial(1, 9));
        EXPECT_EQ(materials.getMaterial(callback.mShapePart, callback.mTriangleIndex), 9u);
    }

    TEST_F(TestBulletNifLoader, loadsFalloutBoxCollisionWithBodyAndNodeTransforms)
    {
        PrimitiveCollisionRecords<Nif::bhkBoxShape> collision(Nif::RC_bhkBoxShape, Nif::RC_bhkRigidBodyT);
        collision.mShape.mHavokMaterial.mMaterial = 0x45;
        collision.mShape.mRadius = 0.1f;
        collision.mShape.mExtents = osg::Vec3f(1.f, 2.f, 3.f);
        collision.mBody.mInfo.mTranslation = osg::Vec4f(1.f, 0.f, 0.f, 0.f);
        collision.attach(mNode);
        mNode.mTransform.mTranslation = osg::Vec3f(2.f, 0.f, 0.f);
        mNode.mTransform.mScale = 2.f;
        enableBethesdaCollision(mNode);

        const auto result = loadFallout(mNode);

        ASSERT_NE(result->mCollisionShape, nullptr);
        ASSERT_TRUE(result->mCollisionShape->isCompound());
        const auto& compound = static_cast<const btCompoundShape&>(*result->mCollisionShape);
        ASSERT_EQ(compound.getNumChildShapes(), 1);
        ASSERT_EQ(compound.getChildShape(0)->getShapeType(), BOX_SHAPE_PROXYTYPE);
        EXPECT_TRUE(isNear(compound.getChildTransform(0).getOrigin(), btVector3(16.f, 0.f, 0.f)));
        const auto& box = static_cast<const btBoxShape&>(*compound.getChildShape(0));
        EXPECT_TRUE(isNear(box.getHalfExtentsWithMargin(), btVector3(15.4f, 29.4f, 43.4f)));
        EXPECT_EQ(result->getHavokMaterial(-1, -1), 5u);

        btCollisionObject object;
        object.setCollisionShape(result->mCollisionShape.get());
        const btVector3 rayFrom(16.f, 0.f, 100.f);
        const btVector3 rayTo(16.f, 0.f, -100.f);
        ClosestRayResultWithIdentity callback(rayFrom, rayTo);
        btTransform fromTransform = btTransform::getIdentity();
        btTransform toTransform = btTransform::getIdentity();
        fromTransform.setOrigin(rayFrom);
        toTransform.setOrigin(rayTo);
        btCollisionWorld::rayTestSingle(
            fromTransform, toTransform, &object, result->mCollisionShape.get(), btTransform::getIdentity(), callback);
        ASSERT_TRUE(callback.hasHit());
        EXPECT_EQ(result->getHavokMaterial(callback.mShapePart, callback.mTriangleIndex), 5u);
        EXPECT_NO_THROW(Resource::makeInstance(result));
    }

    TEST_F(TestBulletNifLoader, loadsFalloutSphereCollision)
    {
        PrimitiveCollisionRecords<Nif::bhkSphereShape> collision(Nif::RC_bhkSphereShape);
        collision.mShape.mHavokMaterial.mMaterial = 9;
        collision.mShape.mRadius = 2.f;
        collision.attach(mNode);
        enableBethesdaCollision(mNode);

        const auto result = loadFallout(mNode);

        ASSERT_NE(result->mCollisionShape, nullptr);
        const auto& compound = static_cast<const btCompoundShape&>(*result->mCollisionShape);
        ASSERT_EQ(compound.getNumChildShapes(), 1);
        ASSERT_EQ(compound.getChildShape(0)->getShapeType(), SPHERE_SHAPE_PROXYTYPE);
        EXPECT_EQ(static_cast<const btSphereShape&>(*compound.getChildShape(0)).getRadius(), btScalar(14.f));
        EXPECT_EQ(result->getHavokMaterial(-1, -1), 9u);
        EXPECT_NO_THROW(Resource::makeInstance(result));
    }

    TEST_F(TestBulletNifLoader, loadsFalloutCapsuleCollision)
    {
        PrimitiveCollisionRecords<Nif::bhkCapsuleShape> collision(Nif::RC_bhkCapsuleShape);
        collision.mShape.mHavokMaterial.mMaterial = 7;
        collision.mShape.mRadius = 0.5f;
        collision.mShape.mRadius1 = 0.5f;
        collision.mShape.mRadius2 = 0.5f;
        collision.mShape.mPoint1 = osg::Vec3f(0.f, -1.f, 0.f);
        collision.mShape.mPoint2 = osg::Vec3f(0.f, 1.f, 0.f);
        collision.attach(mNode);
        enableBethesdaCollision(mNode);

        const auto result = loadFallout(mNode);

        ASSERT_NE(result->mCollisionShape, nullptr);
        const auto& compound = static_cast<const btCompoundShape&>(*result->mCollisionShape);
        ASSERT_EQ(compound.getNumChildShapes(), 1);
        ASSERT_EQ(compound.getChildShape(0)->getShapeType(), CAPSULE_SHAPE_PROXYTYPE);
        const auto& capsule = static_cast<const btCapsuleShape&>(*compound.getChildShape(0));
        EXPECT_EQ(capsule.getRadius(), btScalar(3.5f));
        EXPECT_EQ(capsule.getHalfHeight(), btScalar(7.f));
        EXPECT_EQ(result->getHavokMaterial(-1, -1), 7u);
        EXPECT_NO_THROW(Resource::makeInstance(result));
    }

    TEST_F(TestBulletNifLoader, loadsAnimatedFalloutConvexHullCollision)
    {
        PrimitiveCollisionRecords<Nif::bhkConvexVerticesShape> collision(Nif::RC_bhkConvexVerticesShape);
        collision.mShape.mHavokMaterial.mMaterial = 3;
        collision.mShape.mRadius = 0.05f;
        collision.mShape.mVertices = { osg::Vec4f(0.f, 0.f, 0.f, 0.f), osg::Vec4f(1.f, 0.f, 0.f, 0.f),
            osg::Vec4f(0.f, 1.f, 0.f, 0.f), osg::Vec4f(0.f, 0.f, 1.f, 0.f) };
        collision.attach(mNiNode);
        mNiNode.mRecordIndex = 44;
        mController.mRecordType = Nif::RC_NiKeyframeController;
        mController.mFlags = Nif::NiTimeController::Flag_Active;
        mNiNode.mController = Nif::NiTimeControllerPtr(&mController);
        enableBethesdaCollision(mNiNode);

        const auto result = loadFallout(mNiNode);

        ASSERT_NE(result->mCollisionShape, nullptr);
        const auto& compound = static_cast<const btCompoundShape&>(*result->mCollisionShape);
        ASSERT_EQ(compound.getNumChildShapes(), 1);
        ASSERT_TRUE(compound.getChildShape(0)->isCompound());
        const auto& local = static_cast<const btCompoundShape&>(*compound.getChildShape(0));
        ASSERT_EQ(local.getNumChildShapes(), 1);
        ASSERT_EQ(local.getChildShape(0)->getShapeType(), CONVEX_HULL_SHAPE_PROXYTYPE);
        EXPECT_EQ(static_cast<const btConvexHullShape&>(*local.getChildShape(0)).getNumPoints(), 4);
        EXPECT_EQ(result->mAnimatedShapes, (std::map<int, int>{ { 44, 0 } }));
        EXPECT_EQ(result->getHavokMaterial(-1, -1), 3u);
        EXPECT_NO_THROW(Resource::makeInstance(result));
    }

    TEST_F(TestBulletNifLoader, loadsFalloutListCollisionWithChildMaterials)
    {
        PrimitiveCollisionRecords<Nif::bhkListShape> collision(Nif::RC_bhkListShape);
        Nif::bhkBoxShape box;
        Nif::bhkConvexTransformShape transformedBox;
        Nif::bhkConvexVerticesShape convex;
        box.mRecordType = Nif::RC_bhkBoxShape;
        box.mHavokMaterial.mMaterial = 5;
        box.mRadius = 0.1f;
        box.mExtents = osg::Vec3f(1.f, 1.f, 1.f);
        transformedBox.mRecordType = Nif::RC_bhkConvexTransformShape;
        transformedBox.mShape = Nif::bhkShapePtr(&box);
        transformedBox.mHavokMaterial.mMaterial = 5;
        transformedBox.mTransform = osg::Matrixf::identity();
        transformedBox.mTransform.setTrans(osg::Vec3f(1.f, 0.f, 0.f));
        convex.mRecordType = Nif::RC_bhkConvexVerticesShape;
        convex.mHavokMaterial.mMaterial = 9;
        convex.mRadius = 0.05f;
        convex.mVertices = { osg::Vec4f(10.f, 0.f, 0.f, 0.f), osg::Vec4f(11.f, 0.f, 0.f, 0.f),
            osg::Vec4f(10.f, 1.f, 0.f, 0.f), osg::Vec4f(10.f, 0.f, 1.f, 0.f) };
        collision.mShape.mSubshapes = { Nif::bhkShapePtr(&transformedBox), Nif::bhkShapePtr(&convex) };
        collision.attach(mNode);
        mNode.mTransform.mTranslation = osg::Vec3f(2.f, 0.f, 0.f);
        enableBethesdaCollision(mNode);

        const auto result = loadFallout(mNode);

        ASSERT_NE(result->mCollisionShape, nullptr);
        ASSERT_TRUE(result->mCollisionShape->isCompound());
        const auto& compound = static_cast<const btCompoundShape&>(*result->mCollisionShape);
        ASSERT_EQ(compound.getNumChildShapes(), 2);
        EXPECT_EQ(compound.getChildShape(0)->getShapeType(), BOX_SHAPE_PROXYTYPE);
        EXPECT_EQ(compound.getChildShape(1)->getShapeType(), CONVEX_HULL_SHAPE_PROXYTYPE);
        EXPECT_TRUE(isNear(compound.getChildTransform(0).getOrigin(), btVector3(9.f, 0.f, 0.f)));
        EXPECT_EQ(result->getHavokMaterial(-1, 0), 5u);
        EXPECT_EQ(result->getHavokMaterial(-1, 1), 9u);
        EXPECT_FALSE(result->getHavokMaterial(-1, -1).has_value());

        btCollisionObject object;
        object.setCollisionShape(result->mCollisionShape.get());
        const btVector3 rayFrom(73.f, 1.f, 10.f);
        const btVector3 rayTo(73.f, 1.f, -10.f);
        ClosestRayResultWithIdentity callback(rayFrom, rayTo);
        btTransform fromTransform = btTransform::getIdentity();
        btTransform toTransform = btTransform::getIdentity();
        fromTransform.setOrigin(rayFrom);
        toTransform.setOrigin(rayTo);
        btCollisionWorld::rayTestSingle(
            fromTransform, toTransform, &object, result->mCollisionShape.get(), btTransform::getIdentity(), callback);
        ASSERT_TRUE(callback.hasHit());
        EXPECT_EQ(callback.mShapePart, -1);
        EXPECT_EQ(callback.mTriangleIndex, 1);
        EXPECT_EQ(result->getHavokMaterial(callback.mShapePart, callback.mTriangleIndex), 9u);
        EXPECT_NO_THROW(Resource::makeInstance(result));
    }

    TEST_F(TestBulletNifLoader, mergesEquivalentFixedPrimitiveCollisionBodies)
    {
        Nif::NiNode firstNode;
        Nif::NiNode secondNode;
        init(firstNode);
        init(secondNode);
        PrimitiveCollisionRecords<Nif::bhkBoxShape> first(Nif::RC_bhkBoxShape);
        PrimitiveCollisionRecords<Nif::bhkBoxShape> second(Nif::RC_bhkBoxShape);
        first.mShape.mHavokMaterial.mMaterial = 5;
        first.mShape.mRadius = 0.1f;
        first.mShape.mExtents = osg::Vec3f(1.f, 1.f, 1.f);
        second.mShape.mHavokMaterial.mMaterial = 9;
        second.mShape.mRadius = 0.1f;
        second.mShape.mExtents = osg::Vec3f(1.f, 1.f, 1.f);
        secondNode.mTransform.mTranslation = osg::Vec3f(10.f, 0.f, 0.f);
        first.attach(firstNode);
        second.attach(secondNode);

        enableBethesdaCollision(mNiNode);
        firstNode.mParents.push_back(&mNiNode);
        secondNode.mParents.push_back(&mNiNode);
        mNiNode.mChildren = { Nif::NiAVObjectPtr(&firstNode), Nif::NiAVObjectPtr(&secondNode) };

        const auto result = loadFallout(mNiNode);

        ASSERT_NE(result->mCollisionShape, nullptr);
        const auto& compound = static_cast<const btCompoundShape&>(*result->mCollisionShape);
        ASSERT_EQ(compound.getNumChildShapes(), 2);
        EXPECT_EQ(result->getHavokMaterial(-1, 0), 5u);
        EXPECT_EQ(result->getHavokMaterial(-1, 1), 9u);
        EXPECT_FALSE(result->getHavokMaterial(-1, -1).has_value());

        btCollisionObject object;
        object.setCollisionShape(result->mCollisionShape.get());
        const btVector3 rayFrom(10.f, 0.f, 10.f);
        const btVector3 rayTo(10.f, 0.f, -10.f);
        ClosestRayResultWithIdentity callback(rayFrom, rayTo);
        btTransform fromTransform = btTransform::getIdentity();
        btTransform toTransform = btTransform::getIdentity();
        fromTransform.setOrigin(rayFrom);
        toTransform.setOrigin(rayTo);
        btCollisionWorld::rayTestSingle(
            fromTransform, toTransform, &object, result->mCollisionShape.get(), btTransform::getIdentity(), callback);
        ASSERT_TRUE(callback.hasHit());
        EXPECT_EQ(callback.mShapePart, -1);
        EXPECT_EQ(callback.mTriangleIndex, 1);
        EXPECT_EQ(result->getHavokMaterial(callback.mShapePart, callback.mTriangleIndex), 9u);
    }

    TEST_F(TestBulletNifLoader, fallsBackForHeterogeneousPrimitiveCollisionBodies)
    {
        Nif::NiNode firstNode;
        Nif::NiNode secondNode;
        init(firstNode);
        init(secondNode);
        PrimitiveCollisionRecords<Nif::bhkBoxShape> first(Nif::RC_bhkBoxShape);
        PrimitiveCollisionRecords<Nif::bhkBoxShape> second(Nif::RC_bhkBoxShape);
        first.mShape.mRadius = 0.1f;
        first.mShape.mExtents = osg::Vec3f(1.f, 1.f, 1.f);
        second.mShape.mRadius = 0.1f;
        second.mShape.mExtents = osg::Vec3f(1.f, 1.f, 1.f);
        second.mBody.mHavokFilter.mLayer = 3;
        second.mBody.mInfo.mHavokFilter.mLayer = 3;
        first.attach(firstNode);
        second.attach(secondNode);

        enableBethesdaCollision(mNiNode);
        firstNode.mParents.push_back(&mNiNode);
        secondNode.mParents.push_back(&mNiNode);
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren
            = { Nif::NiAVObjectPtr(&firstNode), Nif::NiAVObjectPtr(&secondNode), Nif::NiAVObjectPtr(&mNiTriShape) };

        const auto result = loadFallout(mNiNode);

        ASSERT_NE(result->mCollisionShape, nullptr);
        EXPECT_TRUE(result->mCollisionShape->isCompound());
        EXPECT_TRUE(result->mCollisionShapeMaterials.empty());
    }

    TEST_F(TestBulletNifLoader, loadsFalloutPackedCollisionWithSubshapeMaterials)
    {
        PackedCollisionRecords collision(Nif::RC_bhkRigidBodyT);
        collision.mBody.mInfo.mTranslation = osg::Vec4f(1.f, 0.f, 0.f, 0.f);
        collision.mData.mVertices = { osg::Vec3f(0.f, 0.f, 0.f), osg::Vec3f(1.f, 0.f, 0.f), osg::Vec3f(0.f, 1.f, 0.f),
            osg::Vec3f(2.f, 0.f, 0.f), osg::Vec3f(3.f, 0.f, 0.f), osg::Vec3f(2.f, 1.f, 0.f) };
        collision.mData.mTriangles.resize(2);
        collision.mData.mTriangles[0].mTriangle = { 0, 1, 2 };
        collision.mData.mTriangles[1].mTriangle = { 3, 4, 5 };
        collision.mData.mSubshapes.resize(2);
        collision.mData.mSubshapes[0].mNumVertices = 3;
        collision.mData.mSubshapes[0].mHavokMaterial.mMaterial = 0x45;
        collision.mData.mSubshapes[0].mHavokFilter = { 1, 0, 0 };
        collision.mData.mSubshapes[1].mNumVertices = 3;
        collision.mData.mSubshapes[1].mHavokMaterial.mMaterial = 0x69;
        collision.mData.mSubshapes[1].mHavokFilter = { 1, 0, 0 };
        collision.attach(mNode);
        mNode.mTransform.mTranslation = osg::Vec3f(2.f, 0.f, 0.f);
        enableBethesdaCollision(mNode);

        Nif::NIFFile file(testNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&mNode);

        const auto result = mLoader.load(file);

        ASSERT_NE(result->mCollisionShape, nullptr);
        ASSERT_EQ(result->mCollisionShape->getShapeType(), TRIANGLE_MESH_SHAPE_PROXYTYPE);
        const auto& shape = static_cast<const btBvhTriangleMeshShape&>(*result->mCollisionShape);
        EXPECT_EQ(shape.getMeshInterface()->getNumSubParts(), 2);
        EXPECT_THAT(getTriangleIdentities(shape), UnorderedElementsAre(Pair(0, 0), Pair(1, 0)));

        const std::vector<btVector3> triangles = getTriangles(shape);
        ASSERT_EQ(triangles.size(), 6u);
        EXPECT_THAT(triangles, Contains(btVector3(9.f, 0.f, 0.f)));
        EXPECT_THAT(triangles, Contains(btVector3(16.f, 0.f, 0.f)));
        EXPECT_THAT(triangles, Contains(btVector3(23.f, 0.f, 0.f)));
        EXPECT_THAT(triangles, Contains(btVector3(30.f, 0.f, 0.f)));
        EXPECT_EQ(result->getHavokMaterial(0, 0), 5u);
        EXPECT_EQ(result->getHavokMaterial(1, 0), 9u);
        EXPECT_FALSE(result->getHavokMaterial(-1, -1).has_value());

        btCollisionObject object;
        object.setCollisionShape(result->mCollisionShape.get());
        const btVector3 rayFrom(24.f, 1.f, 10.f);
        const btVector3 rayTo(24.f, 1.f, -10.f);
        ClosestRayResultWithIdentity callback(rayFrom, rayTo);
        btTransform fromTransform = btTransform::getIdentity();
        btTransform toTransform = btTransform::getIdentity();
        fromTransform.setOrigin(rayFrom);
        toTransform.setOrigin(rayTo);
        btCollisionWorld::rayTestSingle(
            fromTransform, toTransform, &object, result->mCollisionShape.get(), btTransform::getIdentity(), callback);

        ASSERT_TRUE(callback.hasHit());
        EXPECT_EQ(callback.mShapePart, 1);
        EXPECT_EQ(callback.mTriangleIndex, 0);
        EXPECT_EQ(result->getHavokMaterial(callback.mShapePart, callback.mTriangleIndex), 9u);
    }

    TEST_F(TestBulletNifLoader, appliesMirroredPackedScaleExactlyOnce)
    {
        Nif::NiNode collisionNode;
        init(collisionNode);
        PackedCollisionRecords collision(Nif::RC_bhkRigidBodyT);
        collision.mBody.mInfo.mTranslation = osg::Vec4f(1.f, 0.f, 0.f, 0.f);
        collision.mPacked.mScale = osg::Vec4f(2.f, 2.f, 2.f, 0.f);
        collision.attach(collisionNode);

        enableBethesdaCollision(mNiNode);
        mNiNode.mTransform.mTranslation = osg::Vec3f(100.f, 0.f, 0.f);
        collisionNode.mTransform.mTranslation = osg::Vec3f(10.f, 0.f, 0.f);
        collisionNode.mTransform.mScale = 2.f;
        collisionNode.mParents.push_back(&mNiNode);
        mNiNode.mChildren = { Nif::NiAVObjectPtr(&collisionNode) };

        Nif::NIFFile file(testNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&mNiNode);

        const auto result = mLoader.load(file);

        ASSERT_NE(result->mCollisionShape, nullptr);
        ASSERT_EQ(result->mCollisionShape->getShapeType(), TRIANGLE_MESH_SHAPE_PROXYTYPE);
        const auto& shape = static_cast<const btBvhTriangleMeshShape&>(*result->mCollisionShape);
        EXPECT_THAT(getTriangles(shape), Contains(btVector3(124.f, 0.f, 0.f)));
        EXPECT_EQ(result->getHavokMaterial(0, 0), 5u);
    }

    TEST_F(TestBulletNifLoader, ignoresFalloutPackedCollisionWithoutBSXCollisionFlag)
    {
        PackedCollisionRecords collision;
        collision.attach(mNiNode);

        Nif::NIFFile file(testNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&mNiNode);

        const auto result = mLoader.load(file);

        EXPECT_EQ(result->mCollisionShape, nullptr);
        EXPECT_TRUE(result->mCollisionShapeMaterials.empty());
    }

    TEST_F(TestBulletNifLoader, fallsBackWhenPackedScaleDoesNotMirrorOwnerNode)
    {
        PackedCollisionRecords collision;
        collision.mPacked.mScale = osg::Vec4f(2.f, 2.f, 2.f, 0.f);
        collision.attach(mNiNode);

        enableBethesdaCollision(mNiNode);
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = { Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&mNiNode);

        const auto result = mLoader.load(file);

        ASSERT_NE(result->mCollisionShape, nullptr);
        EXPECT_TRUE(result->mCollisionShape->isCompound());
        EXPECT_TRUE(result->mCollisionShapeMaterials.empty());
    }

    TEST_F(TestBulletNifLoader, mergesEquivalentFixedRootAndDescendantCollisionObjects)
    {
        Nif::NiNode collisionNode;
        init(collisionNode);
        PackedCollisionRecords rootCollision;
        PackedCollisionRecords descendantCollision;
        rootCollision.attach(mNiNode);
        descendantCollision.attach(collisionNode);
        collisionNode.mTransform.mTranslation = osg::Vec3f(10.f, 0.f, 0.f);

        enableBethesdaCollision(mNiNode);
        collisionNode.mParents.push_back(&mNiNode);
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = { Nif::NiAVObjectPtr(&collisionNode), Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&mNiNode);

        const auto result = mLoader.load(file);

        ASSERT_NE(result->mCollisionShape, nullptr);
        ASSERT_EQ(result->mCollisionShape->getShapeType(), TRIANGLE_MESH_SHAPE_PROXYTYPE);
        const auto& shape = static_cast<const btBvhTriangleMeshShape&>(*result->mCollisionShape);
        EXPECT_THAT(getTriangleIdentities(shape), UnorderedElementsAre(Pair(0, 0), Pair(1, 0)));
        EXPECT_THAT(getTriangles(shape), Contains(btVector3(10.f, 0.f, 0.f)));
        EXPECT_EQ(result->getHavokMaterial(0, 0), 5u);
        EXPECT_EQ(result->getHavokMaterial(1, 0), 5u);
    }

    TEST_F(TestBulletNifLoader, fallsBackAtomicallyForHeterogeneousFixedCollisionObjects)
    {
        Nif::NiNode collisionNode;
        init(collisionNode);
        PackedCollisionRecords rootCollision;
        PackedCollisionRecords descendantCollision;
        descendantCollision.mBody.mHavokFilter.mLayer = 3;
        descendantCollision.mBody.mInfo.mHavokFilter.mLayer = 3;
        descendantCollision.mData.mSubshapes.front().mHavokFilter.mLayer = 3;
        rootCollision.attach(mNiNode);
        descendantCollision.attach(collisionNode);

        enableBethesdaCollision(mNiNode);
        collisionNode.mParents.push_back(&mNiNode);
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = { Nif::NiAVObjectPtr(&collisionNode), Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&mNiNode);

        const auto result = mLoader.load(file);

        ASSERT_NE(result->mCollisionShape, nullptr);
        EXPECT_TRUE(result->mCollisionShape->isCompound());
        EXPECT_TRUE(result->mCollisionShapeMaterials.empty());
    }

    TEST_F(TestBulletNifLoader, fallsBackAtomicallyForNonFixedMultiBodyCollision)
    {
        Nif::NiNode collisionNode;
        init(collisionNode);
        PackedCollisionRecords rootCollision;
        PackedCollisionRecords descendantCollision;
        descendantCollision.mBody.mInfo.mMotionType = Nif::HkMotionType::Motion_Keyframed;
        rootCollision.attach(mNiNode);
        descendantCollision.attach(collisionNode);

        enableBethesdaCollision(mNiNode);
        collisionNode.mParents.push_back(&mNiNode);
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = { Nif::NiAVObjectPtr(&collisionNode), Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&mNiNode);

        const auto result = mLoader.load(file);

        ASSERT_NE(result->mCollisionShape, nullptr);
        EXPECT_TRUE(result->mCollisionShape->isCompound());
        EXPECT_TRUE(result->mCollisionShapeMaterials.empty());
    }

    TEST_F(TestBulletNifLoader, loadsSingleAnimatedDescendantPackedCollision)
    {
        Nif::NiNode collisionNode;
        init(collisionNode);
        collisionNode.mRecordIndex = 42;
        collisionNode.mTransform.mTranslation = osg::Vec3f(10.f, 0.f, 0.f);
        collisionNode.mTransform.mScale = 2.f;
        PackedCollisionRecords collision;
        collision.mPacked.mScale = osg::Vec4f(2.f, 2.f, 2.f, 0.f);
        collision.attach(collisionNode);
        mController.mRecordType = Nif::RC_NiKeyframeController;
        mController.mFlags = Nif::NiTimeController::Flag_Active;
        collisionNode.mController = Nif::NiTimeControllerPtr(&mController);

        enableBethesdaCollision(mNiNode);
        collisionNode.mParents.push_back(&mNiNode);
        mNiNode.mChildren = { Nif::NiAVObjectPtr(&collisionNode) };

        Nif::NIFFile file(testNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&mNiNode);

        const auto result = mLoader.load(file);

        ASSERT_NE(result->mCollisionShape, nullptr);
        ASSERT_TRUE(result->mCollisionShape->isCompound());
        const auto& compound = static_cast<const btCompoundShape&>(*result->mCollisionShape);
        ASSERT_EQ(compound.getNumChildShapes(), 1);
        EXPECT_TRUE(isNear(compound.getChildTransform(0).getOrigin(), btVector3(10.f, 0.f, 0.f)));
        EXPECT_TRUE(isNear(compound.getChildShape(0)->getLocalScaling(), btVector3(2.f, 2.f, 2.f)));
        EXPECT_EQ(result->mAnimatedShapes, (std::map<int, int>{ { 42, 0 } }));

        btCollisionObject object;
        object.setCollisionShape(result->mCollisionShape.get());
        const btVector3 rayFrom(12.f, 2.f, 10.f);
        const btVector3 rayTo(12.f, 2.f, -10.f);
        ClosestRayResultWithIdentity callback(rayFrom, rayTo);
        btTransform fromTransform = btTransform::getIdentity();
        btTransform toTransform = btTransform::getIdentity();
        fromTransform.setOrigin(rayFrom);
        toTransform.setOrigin(rayTo);
        btCollisionWorld::rayTestSingle(
            fromTransform, toTransform, &object, result->mCollisionShape.get(), btTransform::getIdentity(), callback);

        ASSERT_TRUE(callback.hasHit());
        EXPECT_EQ(callback.mShapePart, 0);
        EXPECT_EQ(callback.mTriangleIndex, 0);
        EXPECT_EQ(result->getHavokMaterial(callback.mShapePart, callback.mTriangleIndex), 5u);
    }

    TEST_F(TestBulletNifLoader, loadsSingleExternalKfAnimatedPackedCollision)
    {
        Nif::NiNode collisionNode;
        init(collisionNode);
        collisionNode.mRecordIndex = 43;
        PackedCollisionRecords collision;
        collision.attach(collisionNode);

        enableBethesdaCollision(mNiNode);
        collisionNode.mParents.push_back(&mNiNode);
        mNiNode.mChildren = { Nif::NiAVObjectPtr(&collisionNode) };

        Nif::NIFFile file(xtestNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&mNiNode);

        const auto result = mLoader.load(file);

        ASSERT_NE(result->mCollisionShape, nullptr);
        EXPECT_TRUE(result->mCollisionShape->isCompound());
        EXPECT_EQ(result->mAnimatedShapes, (std::map<int, int>{ { 43, 0 } }));
        EXPECT_EQ(result->getHavokMaterial(0, 0), 5u);
    }

    TEST_F(TestBulletNifLoader, fallsBackAtomicallyForAnimatedMultiBodyCollision)
    {
        Nif::NiNode animatedNode;
        Nif::NiNode staticNode;
        init(animatedNode);
        init(staticNode);
        PackedCollisionRecords animatedCollision;
        PackedCollisionRecords staticCollision;
        animatedCollision.attach(animatedNode);
        staticCollision.attach(staticNode);
        mController.mRecordType = Nif::RC_NiKeyframeController;
        mController.mFlags = Nif::NiTimeController::Flag_Active;
        animatedNode.mController = Nif::NiTimeControllerPtr(&mController);

        enableBethesdaCollision(mNiNode);
        animatedNode.mParents.push_back(&mNiNode);
        staticNode.mParents.push_back(&mNiNode);
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren
            = { Nif::NiAVObjectPtr(&animatedNode), Nif::NiAVObjectPtr(&staticNode), Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mVersion = Nif::NIFFile::NIFVersion::VER_BGS;
        file.mBethVersion = Nif::NIFFile::BethVersion::BETHVER_FO3;
        file.mRoots.push_back(&mNiNode);

        const auto result = mLoader.load(file);

        ASSERT_NE(result->mCollisionShape, nullptr);
        EXPECT_TRUE(result->mCollisionShape->isCompound());
        EXPECT_TRUE(result->mCollisionShapeMaterials.empty());
    }

    TEST_F(TestBulletNifLoader, should_ignore_nullptr_root)
    {
        Nif::NIFFile file(testNif);
        file.mRoots.push_back(nullptr);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_default_root_nif_node_should_return_default)
    {
        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_default_root_collision_node_nif_node_should_return_default)
    {
        mNode.mRecordType = Nif::RC_RootCollisionNode;

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_default_root_nif_node_and_filename_starting_with_x_should_return_default)
    {
        Nif::NIFFile file(xtestNif);
        file.mRoots.push_back(&mNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_root_bounding_box_should_return_shape_with_bounding_box_data)
    {
        mNode.mName = "Bounding Box";
        mNode.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNode.mBounds.mBox.mExtents = osg::Vec3f(1, 2, 3);
        mNode.mBounds.mBox.mCenter = osg::Vec3f(-1, -2, -3);

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;
        expected.mCollisionBox.mExtents = osg::Vec3f(1, 2, 3);
        expected.mCollisionBox.mCenter = osg::Vec3f(-1, -2, -3);

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_child_bounding_box_should_return_shape_with_bounding_box_data)
    {
        mNode.mName = "Bounding Box";
        mNode.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNode.mBounds.mBox.mExtents = osg::Vec3f(1, 2, 3);
        mNode.mBounds.mBox.mCenter = osg::Vec3f(-1, -2, -3);
        mNode.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNode) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;
        expected.mCollisionBox.mExtents = osg::Vec3f(1, 2, 3);
        expected.mCollisionBox.mCenter = osg::Vec3f(-1, -2, -3);

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_root_with_bounds_and_child_bounding_box_should_use_bounding_box)
    {
        mNode.mName = "Bounding Box";
        mNode.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNode.mBounds.mBox.mExtents = osg::Vec3f(1, 2, 3);
        mNode.mBounds.mBox.mCenter = osg::Vec3f(-1, -2, -3);
        mNode.mParents.push_back(&mNiNode);

        mNiNode.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNiNode.mBounds.mBox.mExtents = osg::Vec3f(4, 5, 6);
        mNiNode.mBounds.mBox.mCenter = osg::Vec3f(-4, -5, -6);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNode) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;
        expected.mCollisionBox.mExtents = osg::Vec3f(1, 2, 3);
        expected.mCollisionBox.mCenter = osg::Vec3f(-1, -2, -3);

        EXPECT_EQ(*result, expected);
    }

    TEST_F(
        TestBulletNifLoader, for_root_and_two_children_where_both_with_bounds_but_one_is_bounding_box_use_bounding_box)
    {
        mNode.mName = "Bounding Box";
        mNode.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNode.mBounds.mBox.mExtents = osg::Vec3f(1, 2, 3);
        mNode.mBounds.mBox.mCenter = osg::Vec3f(-1, -2, -3);
        mNode.mParents.push_back(&mNiNode);

        mNode2.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNode2.mBounds.mBox.mExtents = osg::Vec3f(4, 5, 6);
        mNode2.mBounds.mBox.mCenter = osg::Vec3f(-4, -5, -6);
        mNode2.mParents.push_back(&mNiNode);

        mNiNode.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNiNode.mBounds.mBox.mExtents = osg::Vec3f(7, 8, 9);
        mNiNode.mBounds.mBox.mCenter = osg::Vec3f(-7, -8, -9);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNode), Nif::NiAVObjectPtr(&mNode2) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;
        expected.mCollisionBox.mExtents = osg::Vec3f(1, 2, 3);
        expected.mCollisionBox.mCenter = osg::Vec3f(-1, -2, -3);

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader,
        for_root_and_two_children_where_both_with_bounds_but_second_is_bounding_box_use_bounding_box)
    {
        mNode.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNode.mBounds.mBox.mExtents = osg::Vec3f(1, 2, 3);
        mNode.mBounds.mBox.mCenter = osg::Vec3f(-1, -2, -3);
        mNode.mParents.push_back(&mNiNode);

        mNode2.mName = "Bounding Box";
        mNode2.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNode2.mBounds.mBox.mExtents = osg::Vec3f(4, 5, 6);
        mNode2.mBounds.mBox.mCenter = osg::Vec3f(-4, -5, -6);
        mNode2.mParents.push_back(&mNiNode);

        mNiNode.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNiNode.mBounds.mBox.mExtents = osg::Vec3f(7, 8, 9);
        mNiNode.mBounds.mBox.mCenter = osg::Vec3f(-7, -8, -9);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNode), Nif::NiAVObjectPtr(&mNode2) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;
        expected.mCollisionBox.mExtents = osg::Vec3f(4, 5, 6);
        expected.mCollisionBox.mCenter = osg::Vec3f(-4, -5, -6);

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_root_nif_node_with_bounds_should_return_shape_with_null_collision_shape)
    {
        mNode.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNode.mBounds.mBox.mExtents = osg::Vec3f(1, 2, 3);
        mNode.mBounds.mBox.mCenter = osg::Vec3f(-1, -2, -3);

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_tri_shape_root_node_should_return_static_shape)
    {
        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiTriShape);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));

        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        auto triShape = std::make_unique<Resource::TriangleMeshShape>(triangles.release(), true);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(triShape.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_tri_shape_root_node_with_bounds_should_return_static_shape)
    {
        mNiTriShape.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNiTriShape.mBounds.mBox.mExtents = osg::Vec3f(1, 2, 3);
        mNiTriShape.mBounds.mBox.mCenter = osg::Vec3f(-1, -2, -3);

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiTriShape);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));

        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        auto triShape = std::make_unique<Resource::TriangleMeshShape>(triangles.release(), true);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(triShape.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_tri_shape_child_node_should_return_static_shape)
    {
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));

        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        auto triShape = std::make_unique<Resource::TriangleMeshShape>(triangles.release(), true);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(triShape.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_nested_tri_shape_child_should_return_static_shape)
    {
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiNode2) };
        mNiNode2.mParents.push_back(&mNiNode);
        mNiNode2.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };
        mNiTriShape.mParents.push_back(&mNiNode2);

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));

        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        auto triShape = std::make_unique<Resource::TriangleMeshShape>(triangles.release(), true);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(triShape.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_two_tri_shape_children_should_return_static_shape_with_all_meshes)
    {
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiTriShape2.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape), Nif::NiAVObjectPtr(&mNiTriShape2) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<btTriangleMesh> triangles2(new btTriangleMesh(false));
        triangles2->addTriangle(btVector3(0, 0, 1), btVector3(1, 0, 1), btVector3(1, 1, 1));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        auto triShape = std::make_unique<Resource::TriangleMeshShape>(triangles.release(), true);
        auto triShape2 = std::make_unique<Resource::TriangleMeshShape>(triangles2.release(), true);

        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(triShape.release(), btVector3(1, 1, 1)));
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(triShape2.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader,
        for_tri_shape_child_node_and_filename_starting_with_x_and_not_empty_skin_should_return_static_shape)
    {
        mNiTriShape.mSkin = Nif::NiSkinInstancePtr(&mNiSkinInstance);
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(xtestNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        auto triShape = std::make_unique<Resource::TriangleMeshShape>(triangles.release(), true);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(triShape.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_tri_shape_root_node_and_filename_starting_with_x_should_return_animated_shape)
    {
        copy(mTransform, mNiTriShape.mTransform);
        mNiTriShape.mTransform.mScale = 3;

        Nif::NIFFile file(xtestNif);
        file.mRoots.push_back(&mNiTriShape);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> shape(new btCompoundShape);
        shape->addChildShape(mTransform, new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(3, 3, 3)));
        Resource::BulletShape expected;
        expected.mCollisionShape.reset(shape.release());
        expected.mAnimatedShapes = { { -1, 0 } };

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_tri_shape_child_node_and_filename_starting_with_x_should_return_animated_shape)
    {
        copy(mTransform, mNiTriShape.mTransform);
        mNiTriShape.mTransform.mScale = 3;
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };
        mNiNode.mTransform.mScale = 4;

        Nif::NIFFile file(xtestNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> shape(new btCompoundShape);
        shape->addChildShape(
            mTransformScale4, new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(12, 12, 12)));
        Resource::BulletShape expected;
        expected.mCollisionShape.reset(shape.release());
        expected.mAnimatedShapes = { { -1, 0 } };

        EXPECT_EQ(*result, expected);
    }

    TEST_F(
        TestBulletNifLoader, for_two_tri_shape_children_nodes_and_filename_starting_with_x_should_return_animated_shape)
    {
        copy(mTransform, mNiTriShape.mTransform);
        mNiTriShape.mTransform.mScale = 3;
        mNiTriShape.mParents.push_back(&mNiNode);

        copy(mTransform, mNiTriShape2.mTransform);
        mNiTriShape2.mTransform.mScale = 3;
        mNiTriShape2.mParents.push_back(&mNiNode);

        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape), Nif::NiAVObjectPtr(&mNiTriShape2) };

        Nif::NIFFile file(xtestNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));

        std::unique_ptr<btTriangleMesh> triangles2(new btTriangleMesh(false));
        triangles2->addTriangle(btVector3(0, 0, 1), btVector3(1, 0, 1), btVector3(1, 1, 1));
        std::unique_ptr<Resource::TriangleMeshShape> mesh2(new Resource::TriangleMeshShape(triangles2.release(), true));

        std::unique_ptr<btCompoundShape> shape(new btCompoundShape);
        shape->addChildShape(mTransform, new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(3, 3, 3)));
        shape->addChildShape(mTransform, new Resource::ScaledTriangleMeshShape(mesh2.release(), btVector3(3, 3, 3)));
        Resource::BulletShape expected;
        expected.mCollisionShape.reset(shape.release());
        expected.mAnimatedShapes = { { -1, 0 } };

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_tri_shape_child_node_with_controller_should_return_animated_shape)
    {
        mController.mRecordType = Nif::RC_NiKeyframeController;
        mController.mFlags |= Nif::NiTimeController::Flag_Active;
        copy(mTransform, mNiTriShape.mTransform);
        mNiTriShape.mTransform.mScale = 3;
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiTriShape.mController = Nif::NiTimeControllerPtr(&mController);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };
        mNiNode.mTransform.mScale = 4;

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> shape(new btCompoundShape);
        shape->addChildShape(
            mTransformScale4, new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(12, 12, 12)));
        Resource::BulletShape expected;
        expected.mCollisionShape.reset(shape.release());
        expected.mAnimatedShapes = { { -1, 0 } };

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_two_tri_shape_children_nodes_where_one_with_controller_should_return_animated_shape)
    {
        mController.mRecordType = Nif::RC_NiKeyframeController;
        mController.mFlags |= Nif::NiTimeController::Flag_Active;
        copy(mTransform, mNiTriShape.mTransform);
        mNiTriShape.mTransform.mScale = 3;
        mNiTriShape.mParents.push_back(&mNiNode);
        copy(mTransform, mNiTriShape2.mTransform);
        mNiTriShape2.mTransform.mScale = 3;
        mNiTriShape2.mParents.push_back(&mNiNode);
        mNiTriShape2.mController = Nif::NiTimeControllerPtr(&mController);
        mNiNode.mChildren = Nif::NiAVObjectList{
            Nif::NiAVObjectPtr(&mNiTriShape),
            Nif::NiAVObjectPtr(&mNiTriShape2),
        };
        mNiNode.mTransform.mScale = 4;

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));

        std::unique_ptr<btTriangleMesh> triangles2(new btTriangleMesh(false));
        triangles2->addTriangle(btVector3(0, 0, 1), btVector3(1, 0, 1), btVector3(1, 1, 1));
        std::unique_ptr<Resource::TriangleMeshShape> mesh2(new Resource::TriangleMeshShape(triangles2.release(), true));

        std::unique_ptr<btCompoundShape> shape(new btCompoundShape);
        shape->addChildShape(
            mTransformScale4, new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(12, 12, 12)));
        shape->addChildShape(
            mTransformScale4, new Resource::ScaledTriangleMeshShape(mesh2.release(), btVector3(12, 12, 12)));
        Resource::BulletShape expected;
        expected.mCollisionShape.reset(shape.release());
        expected.mAnimatedShapes = { { -1, 1 } };

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, should_add_static_mesh_to_existing_compound_mesh)
    {
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(xtestNif);
        file.mRoots.push_back(&mNiNode);
        file.mRoots.push_back(&mNiTriShape2);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));

        std::unique_ptr<btTriangleMesh> triangles2(new btTriangleMesh(false));
        triangles2->addTriangle(btVector3(0, 0, 1), btVector3(1, 0, 1), btVector3(1, 1, 1));
        std::unique_ptr<Resource::TriangleMeshShape> mesh2(new Resource::TriangleMeshShape(triangles2.release(), true));

        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh2.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());
        expected.mAnimatedShapes = { { -1, 0 } };

        EXPECT_EQ(*result, expected);
    }

    TEST_F(
        TestBulletNifLoader, for_root_avoid_node_and_tri_shape_child_node_should_return_shape_with_null_collision_shape)
    {
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };
        mNiNode.mRecordType = Nif::RC_AvoidNode;

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));

        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));
        Resource::BulletShape expected;
        expected.mAvoidCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_tri_shape_child_node_with_empty_data_should_return_shape_with_null_collision_shape)
    {
        mNiTriShape.mData = Nif::NiGeometryDataPtr(nullptr);
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader,
        for_tri_shape_child_node_with_empty_data_triangles_should_return_shape_with_null_collision_shape)
    {
        auto data = static_cast<Nif::NiTriShapeData*>(mNiTriShape.mData.getPtr());
        data->mTriangles.clear();
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader,
        for_root_node_with_extra_data_string_equal_ncc_should_return_shape_with_cameraonly_collision)
    {
        mNiStringExtraData.mData = "NCC__";
        mNiStringExtraData.mRecordType = Nif::RC_NiStringExtraData;
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mExtra = Nif::ExtraPtr(&mNiStringExtraData);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        expected.mVisualCollisionType = Resource::VisualCollisionType::Camera;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader,
        for_root_node_with_not_first_extra_data_string_equal_ncc_should_return_shape_with_cameraonly_collision)
    {
        mNiStringExtraData.mNext = Nif::ExtraPtr(&mNiStringExtraData2);
        mNiStringExtraData2.mData = "NCC__";
        mNiStringExtraData2.mRecordType = Nif::RC_NiStringExtraData;
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mExtra = Nif::ExtraPtr(&mNiStringExtraData);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());
        expected.mVisualCollisionType = Resource::VisualCollisionType::Camera;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(
        TestBulletNifLoader, for_root_node_with_extra_data_string_starting_with_nc_should_return_shape_with_nocollision)
    {
        mNiStringExtraData.mData = "NC___";
        mNiStringExtraData.mRecordType = Nif::RC_NiStringExtraData;
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mExtra = Nif::ExtraPtr(&mNiStringExtraData);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());
        expected.mVisualCollisionType = Resource::VisualCollisionType::Default;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader,
        for_root_node_with_not_first_extra_data_string_starting_with_nc_should_return_shape_with_nocollision)
    {
        mNiStringExtraData.mNext = Nif::ExtraPtr(&mNiStringExtraData2);
        mNiStringExtraData2.mData = "NC___";
        mNiStringExtraData2.mRecordType = Nif::RC_NiStringExtraData;
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mExtra = Nif::ExtraPtr(&mNiStringExtraData);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());
        expected.mVisualCollisionType = Resource::VisualCollisionType::Default;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_tri_shape_child_node_with_extra_data_string_should_ignore_extra_data)
    {
        mNiStringExtraData.mData = "NC___";
        mNiStringExtraData.mRecordType = Nif::RC_NiStringExtraData;
        mNiTriShape.mExtra = Nif::ExtraPtr(&mNiStringExtraData);
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_empty_root_collision_node_without_nc_should_return_shape_with_cameraonly_collision)
    {
        Nif::NiTriShape niTriShape;
        Nif::NiNode emptyCollisionNode;
        init(niTriShape);
        init(emptyCollisionNode);

        niTriShape.mData = Nif::NiGeometryDataPtr(&mNiTriShapeData);
        niTriShape.mParents.push_back(&mNiNode);

        emptyCollisionNode.mRecordType = Nif::RC_RootCollisionNode;
        emptyCollisionNode.mParents.push_back(&mNiNode);

        mNiNode.mChildren
            = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&niTriShape), Nif::NiAVObjectPtr(&emptyCollisionNode) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());
        expected.mVisualCollisionType = Resource::VisualCollisionType::Camera;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, bsx_editor_marker_flag_disables_collision_for_markers)
    {
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiTriShape.mName = "EditorMarker";
        mNiIntegerExtraData.mData = 34; // BSXFlags "has collision" | "editor marker"
        mNiIntegerExtraData.mRecordType = Nif::RC_BSXFlags;
        mNiNode.mExtraList.push_back(Nif::ExtraPtr(&mNiIntegerExtraData));
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;
        file.mVersion = Nif::NIFStream::generateVersion(10, 0, 1, 0);

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, mrk_editor_marker_flag_disables_collision_for_markers)
    {
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiTriShape.mName = "Tri EditorMarker";
        mNiStringExtraData.mData = "MRK";
        mNiStringExtraData.mRecordType = Nif::RC_NiStringExtraData;
        mNiNode.mExtra = Nif::ExtraPtr(&mNiStringExtraData);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_tri_strips_root_node_should_return_static_shape)
    {
        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiTriStrips);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        triangles->addTriangle(btVector3(1, 0, 0), btVector3(0, 1, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, should_ignore_tri_strips_data_with_empty_strips)
    {
        mNiTriStripsData.mStrips.clear();

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiTriStrips);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        const Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_static_mesh_should_ignore_tri_strips_data_with_less_than_3_strips)
    {
        mNiTriStripsData.mStrips.front() = { 0, 1 };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiTriStrips);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        const Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_avoid_collision_mesh_should_ignore_tri_strips_data_with_less_than_3_strips)
    {
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };
        mNiNode.mRecordType = Nif::RC_AvoidNode;
        mNiTriStripsData.mStrips.front() = { 0, 1 };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiTriStrips);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        const Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, for_animated_mesh_should_ignore_tri_strips_data_with_less_than_3_strips)
    {
        mNiTriStripsData.mStrips.front() = { 0, 1 };
        mNiTriStrips.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriStrips) };

        Nif::NIFFile file(xtestNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        const Resource::BulletShape expected;

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, should_not_add_static_mesh_with_no_triangles_to_compound_shape)
    {
        mNiTriStripsData.mStrips.front() = { 0, 1 };
        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };

        Nif::NIFFile file(xtestNif);
        file.mRoots.push_back(&mNiNode);
        file.mRoots.push_back(&mNiTriStrips);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());
        expected.mAnimatedShapes = { { -1, 0 } };

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, should_handle_node_with_multiple_parents)
    {
        copy(mTransform, mNiTriShape.mTransform);
        mNiTriShape.mTransform.mScale = 4;
        mNiTriShape.mParents = { &mNiNode, &mNiNode2 };
        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };
        mNiNode.mTransform.mScale = 2;
        mNiNode2.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };
        mNiNode2.mTransform.mScale = 3;

        Nif::NIFFile file(xtestNif);
        file.mRoots.push_back(&mNiNode);
        file.mRoots.push_back(&mNiNode2);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles1(new btTriangleMesh(false));
        triangles1->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh1(new Resource::TriangleMeshShape(triangles1.release(), true));
        std::unique_ptr<btTriangleMesh> triangles2(new btTriangleMesh(false));
        triangles2->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh2(new Resource::TriangleMeshShape(triangles2.release(), true));
        std::unique_ptr<btCompoundShape> shape(new btCompoundShape);
        shape->addChildShape(
            mTransformScale2, new Resource::ScaledTriangleMeshShape(mesh1.release(), btVector3(8, 8, 8)));
        shape->addChildShape(
            mTransformScale3, new Resource::ScaledTriangleMeshShape(mesh2.release(), btVector3(12, 12, 12)));
        Resource::BulletShape expected;
        expected.mCollisionShape.reset(shape.release());
        expected.mAnimatedShapes = { { -1, 0 } };

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, dont_assign_invalid_bounding_box_extents)
    {
        copy(mTransform, mNiTriShape.mTransform);
        mNiTriShape.mTransform.mScale = 10;
        mNiTriShape.mParents.push_back(&mNiNode);

        mNiTriShape2.mName = "Bounding Box";
        mNiTriShape2.mBounds.mType = Nif::BoundingVolume::Type::BOX_BV;
        mNiTriShape2.mBounds.mBox.mExtents = osg::Vec3f(-1, -2, -3);
        mNiTriShape2.mParents.push_back(&mNiNode);

        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape), Nif::NiAVObjectPtr(&mNiTriShape2) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);

        const auto result = mLoader.load(file);

        const bool extentsUnassigned
            = std::ranges::all_of(result->mCollisionBox.mExtents._v, [](float extent) { return extent == 0.f; });

        EXPECT_EQ(extentsUnassigned, true);
    }

    TEST_F(TestBulletNifLoader, no_rcn_flag_with_immediate_rcn_uses_rcn_for_collision)
    {
        Nif::NiNode rcn;
        init(rcn);
        rcn.mRecordType = Nif::RC_RootCollisionNode;

        mNiTriShape.mParents.push_back(&mNiNode);
        rcn.mParents.push_back(&mNiNode);
        mNiTriShape2.mParents.push_back(&rcn);

        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape), Nif::NiAVObjectPtr(&rcn) };
        rcn.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape2) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 1), btVector3(1, 0, 1), btVector3(1, 1, 1));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, no_rcn_flag_with_nested_rcn_ignores_rcn_and_uses_all_for_collision)
    {
        Nif::NiNode rcn;
        init(rcn);
        rcn.mRecordType = Nif::RC_RootCollisionNode;

        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode2.mParents.push_back(&mNiNode);
        rcn.mParents.push_back(&mNiNode2);
        mNiTriShape2.mParents.push_back(&rcn);

        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape), Nif::NiAVObjectPtr(&mNiNode2) };
        mNiNode2.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&rcn) };
        rcn.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape2) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles1(new btTriangleMesh(false));
        triangles1->addTriangle(btVector3(0, 0, 0), btVector3(1, 0, 0), btVector3(1, 1, 0));
        std::unique_ptr<Resource::TriangleMeshShape> mesh1(new Resource::TriangleMeshShape(triangles1.release(), true));

        std::unique_ptr<btTriangleMesh> triangles2(new btTriangleMesh(false));
        triangles2->addTriangle(btVector3(0, 0, 1), btVector3(1, 0, 1), btVector3(1, 1, 1));
        std::unique_ptr<Resource::TriangleMeshShape> mesh2(new Resource::TriangleMeshShape(triangles2.release(), true));

        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh1.release(), btVector3(1, 1, 1)));
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh2.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, rcn_flag_with_nested_rcn_uses_nested_rcn_for_collision)
    {
        Nif::NiNode rcn;
        init(rcn);
        rcn.mRecordType = Nif::RC_RootCollisionNode;

        mNiStringExtraData.mData = "RCN";
        mNiStringExtraData.mRecordType = Nif::RC_NiStringExtraData;
        mNiNode.mExtra = Nif::ExtraPtr(&mNiStringExtraData);

        mNiTriShape.mParents.push_back(&mNiNode);
        mNiNode2.mParents.push_back(&mNiNode);
        rcn.mParents.push_back(&mNiNode2);
        mNiTriShape2.mParents.push_back(&rcn);

        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape), Nif::NiAVObjectPtr(&mNiNode2) };
        mNiNode2.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&rcn) };
        rcn.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape2) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 1), btVector3(1, 0, 1), btVector3(1, 1, 1));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }

    TEST_F(TestBulletNifLoader, rcn_flag_with_multiple_nested_rcns_uses_last_rcn)
    {
        Nif::NiNode rcn1;
        init(rcn1);
        rcn1.mRecordType = Nif::RC_RootCollisionNode;

        Nif::NiNode rcn2;
        init(rcn2);
        rcn2.mRecordType = Nif::RC_RootCollisionNode;

        mNiStringExtraData.mData = "RCN";
        mNiStringExtraData.mRecordType = Nif::RC_NiStringExtraData;
        mNiNode.mExtra = Nif::ExtraPtr(&mNiStringExtraData);

        mNiNode2.mParents.push_back(&mNiNode);
        mNiNode3.mParents.push_back(&mNiNode);

        rcn1.mParents.push_back(&mNiNode2);
        mNiTriShape.mParents.push_back(&rcn1);

        rcn2.mParents.push_back(&mNiNode3);
        mNiTriShape2.mParents.push_back(&rcn2);

        mNiNode.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiNode2), Nif::NiAVObjectPtr(&mNiNode3) };
        mNiNode2.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&rcn1) };
        mNiNode3.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&rcn2) };
        rcn1.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape) };
        rcn2.mChildren = Nif::NiAVObjectList{ Nif::NiAVObjectPtr(&mNiTriShape2) };

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&mNiNode);
        file.mHash = mHash;

        const auto result = mLoader.load(file);

        std::unique_ptr<btTriangleMesh> triangles(new btTriangleMesh(false));
        triangles->addTriangle(btVector3(0, 0, 1), btVector3(1, 0, 1), btVector3(1, 1, 1));
        std::unique_ptr<Resource::TriangleMeshShape> mesh(new Resource::TriangleMeshShape(triangles.release(), true));
        std::unique_ptr<btCompoundShape> compound(new btCompoundShape);
        compound->addChildShape(
            btTransform::getIdentity(), new Resource::ScaledTriangleMeshShape(mesh.release(), btVector3(1, 1, 1)));

        Resource::BulletShape expected;
        expected.mCollisionShape.reset(compound.release());

        EXPECT_EQ(*result, expected);
    }
}
