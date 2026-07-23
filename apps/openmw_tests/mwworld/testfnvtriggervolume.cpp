#include <gtest/gtest.h>

#include <apps/openmw/mwlua/triggervolume.hpp>

namespace
{
    ESM4::Primitive makeBox(float x, float y, float z)
    {
        ESM4::Primitive result;
        result.mType = ESM4::Primitive::Box;
        result.mBounds = { x, y, z };
        return result;
    }

    TEST(FnvTriggerVolumeTest, UsesFullAuthoredBoundsAndActorCollisionExtents)
    {
        const ESM4::Primitive box = makeBox(80.f, 100.f, 140.f);
        ESM::Position trigger{};
        trigger.pos[0] = 100.f;
        trigger.pos[1] = 200.f;
        trigger.pos[2] = 300.f;

        EXPECT_TRUE(MWLua::intersectsTriggerBox(
            box, 1.f, trigger, osg::Vec3f(100.f, 200.f, 370.f), osg::Vec3f(20.f, 20.f, 70.f)));
        EXPECT_TRUE(MWLua::intersectsTriggerBox(
            box, 1.f, trigger, osg::Vec3f(155.f, 200.f, 370.f), osg::Vec3f(20.f, 20.f, 70.f)));
        EXPECT_FALSE(MWLua::intersectsTriggerBox(
            box, 1.f, trigger, osg::Vec3f(161.f, 200.f, 370.f), osg::Vec3f(20.f, 20.f, 70.f)));
    }

    TEST(FnvTriggerVolumeTest, HonorsAuthoredRotation)
    {
        const ESM4::Primitive box = makeBox(200.f, 40.f, 100.f);
        ESM::Position trigger{};
        trigger.rot[2] = 1.57079632679f;
        const osg::Vec3f pointHalf(1.f, 1.f, 1.f);

        EXPECT_TRUE(
            MWLua::intersectsTriggerBox(box, 1.f, trigger, osg::Vec3f(0.f, 90.f, 0.f), pointHalf));
        EXPECT_FALSE(
            MWLua::intersectsTriggerBox(box, 1.f, trigger, osg::Vec3f(90.f, 0.f, 0.f), pointHalf));
    }

    TEST(FnvTriggerVolumeTest, RejectsNonBoxAndDegeneratePrimitives)
    {
        ESM::Position trigger{};
        ESM4::Primitive sphere = makeBox(100.f, 100.f, 100.f);
        sphere.mType = ESM4::Primitive::Sphere;

        EXPECT_FALSE(MWLua::intersectsTriggerBox(
            sphere, 1.f, trigger, osg::Vec3f(), osg::Vec3f(1.f, 1.f, 1.f)));
        EXPECT_FALSE(MWLua::intersectsTriggerBox(
            makeBox(0.f, 100.f, 100.f), 1.f, trigger, osg::Vec3f(), osg::Vec3f(1.f, 1.f, 1.f)));
    }
}
