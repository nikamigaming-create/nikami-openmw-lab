#include <gtest/gtest.h>

#include <cmath>

#include "apps/openmw/mwworld/timestamp.hpp"
#include "apps/openmw/mwworld/cellstore.hpp"

#include "components/esm4/common.hpp"

namespace MWWorld
{
    namespace
    {
        TEST(MWWorldTimeStampTest, operatorPlusShouldNotChangeTimeStampForZero)
        {
            TimeStamp timeStamp;
            timeStamp += 0;
            EXPECT_EQ(timeStamp.getDay(), 0);
            EXPECT_EQ(timeStamp.getHour(), 0);
        }

        TEST(MWWorldTimeStampTest, operatorPlusShouldProperlyHandleDoubleValuesCloseTo24)
        {
            TimeStamp timeStamp;
            timeStamp += std::nextafter(24.0, 0.0);
            EXPECT_EQ(timeStamp.getDay(), 0);
            EXPECT_LT(timeStamp.getHour(), 24);
            EXPECT_FLOAT_EQ(timeStamp.getHour(), 24);
        }

        TEST(MWWorldTimeStampTest, operatorPlusShouldProperlyHandleFloatValuesCloseTo24)
        {
            TimeStamp timeStamp;
            timeStamp += std::nextafter(24.0f, 0.0f);
            EXPECT_EQ(timeStamp.getDay(), 0);
            EXPECT_LT(timeStamp.getHour(), 24);
            EXPECT_FLOAT_EQ(timeStamp.getHour(), 24);
        }

        TEST(MWWorldTimeStampTest, operatorPlusShouldAddDaysForEach24Hours)
        {
            TimeStamp timeStamp;
            timeStamp += 24.0 * 42;
            EXPECT_EQ(timeStamp.getDay(), 42);
            EXPECT_EQ(timeStamp.getHour(), 0);
        }

        TEST(MWWorldTimeStampTest, operatorPlusShouldAddDaysForEach24HoursAndSetRemainderToHours)
        {
            TimeStamp timeStamp;
            timeStamp += 24.0 * 42 + 13.0;
            EXPECT_EQ(timeStamp.getDay(), 42);
            EXPECT_EQ(timeStamp.getHour(), 13);
        }

        TEST(MWWorldTimeStampTest, operatorPlusShouldAccumulateExistingValue)
        {
            TimeStamp timeStamp(13, 42);
            timeStamp += 24.0 * 2 + 17.0;
            EXPECT_EQ(timeStamp.getDay(), 45);
            EXPECT_EQ(timeStamp.getHour(), 6);
        }

        TEST(MWWorldTimeStampTest, operatorPlusShouldThrowExceptionForNegativeValue)
        {
            TimeStamp timeStamp(13, 42);
            EXPECT_THROW(timeStamp += -1, std::runtime_error);
        }

        TEST(MWWorldTimeStampTest, esm4CellResetRequiresMoreThanRetailDefaultHours)
        {
            const TimeStamp lastReset(12, 10);
            EXPECT_FALSE(isEsm4CellResetDue(lastReset + 72, lastReset, 72));
            EXPECT_TRUE(isEsm4CellResetDue(lastReset + 72.01, lastReset, 72));
            EXPECT_FALSE(isEsm4CellResetDue(lastReset + 1000, lastReset, -1));
        }

        TEST(MWWorldTimeStampTest, esm4NoRespawnReferenceNeverResets)
        {
            EXPECT_TRUE(isEsm4ReferenceResettable(0));
            EXPECT_FALSE(isEsm4ReferenceResettable(ESM4::Rec_NoRespawn));
            EXPECT_FALSE(isEsm4ReferenceResettable(ESM4::Rec_NoRespawn | ESM4::Rec_Persistent));
        }
    }
}
