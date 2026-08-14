#include <gtest/gtest.h>

#include "apps/openmw/mwrender/objectpaging.hpp"

namespace MWRender
{
    namespace
    {
        TEST(ObjectPagingTest, activeGridRejectsStaleCachedChunkCenters)
        {
            const osg::Vec4i activeGrid(-2, -2, 3, 3);

            EXPECT_TRUE(isObjectPagingChunkInsideActiveGrid(osg::Vec2f(0.5f, 0.5f), activeGrid));
            EXPECT_TRUE(isObjectPagingChunkInsideActiveGrid(osg::Vec2f(2.5f, -1.5f), activeGrid));

            EXPECT_FALSE(isObjectPagingChunkInsideActiveGrid(osg::Vec2f(-3.5f, 0.5f), activeGrid));
            EXPECT_FALSE(isObjectPagingChunkInsideActiveGrid(osg::Vec2f(0.5f, 3.5f), activeGrid));
            EXPECT_FALSE(isObjectPagingChunkInsideActiveGrid(osg::Vec2f(-2.f, 0.5f), activeGrid));
            EXPECT_FALSE(isObjectPagingChunkInsideActiveGrid(osg::Vec2f(0.5f, 3.f), activeGrid));
        }
    }
}
