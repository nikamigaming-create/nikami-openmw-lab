#include <apps/openmw/mwsound/saysequence.hpp>

#include <array>

#include <gtest/gtest.h>

namespace
{
    using VFS::Path::Normalized;

    TEST(SaySequenceTest, advancesRetailResponsesWithoutOverlap)
    {
        const std::array voices{
            Normalized("sound/voice/falloutnv.esm/maleold02/vfreeformg_vfreeformgoodsp_00106635_1.ogg"),
            Normalized("sound/voice/falloutnv.esm/maleold02/vfreeformg_vfreeformgoodsp_00106635_2.ogg"),
        };

        MWSound::SaySequence sequence;
        sequence.replace(voices);

        ASSERT_NE(sequence.beginNext(), nullptr);
        EXPECT_EQ(*sequence.getCurrent(), voices[0]);
        EXPECT_EQ(sequence.getPendingCount(), 1);
        EXPECT_EQ(sequence.beginNext(), nullptr) << "an active response must not overlap the next response";

        sequence.finishCurrent();
        ASSERT_NE(sequence.beginNext(), nullptr);
        EXPECT_EQ(*sequence.getCurrent(), voices[1]);
        EXPECT_EQ(sequence.getPendingCount(), 0);

        sequence.finishCurrent();
        EXPECT_EQ(sequence.beginNext(), nullptr);
        EXPECT_EQ(sequence.getCurrent(), nullptr);
    }

    TEST(SaySequenceTest, replacementDropsEveryStaleResponse)
    {
        const std::array oldVoices{ Normalized("old_1.ogg"), Normalized("old_2.ogg") };
        const std::array newVoices{ Normalized("new_1.ogg"), Normalized("new_2.ogg") };

        MWSound::SaySequence sequence;
        sequence.replace(oldVoices);
        ASSERT_NE(sequence.beginNext(), nullptr);

        sequence.replace(newVoices);
        EXPECT_EQ(sequence.getCurrent(), nullptr);
        EXPECT_EQ(sequence.getPendingCount(), 2);

        ASSERT_NE(sequence.beginNext(), nullptr);
        EXPECT_EQ(*sequence.getCurrent(), newVoices[0]);
        sequence.finishCurrent();
        ASSERT_NE(sequence.beginNext(), nullptr);
        EXPECT_EQ(*sequence.getCurrent(), newVoices[1]);
    }

    TEST(SaySequenceTest, clearPreventsDeferredPlayback)
    {
        const std::array voices{ Normalized("first.ogg"), Normalized("second.ogg") };

        MWSound::SaySequence sequence;
        sequence.replace(voices);
        ASSERT_NE(sequence.beginNext(), nullptr);
        sequence.clear();

        EXPECT_EQ(sequence.getCurrent(), nullptr);
        EXPECT_EQ(sequence.getPendingCount(), 0);
        EXPECT_EQ(sequence.beginNext(), nullptr);
    }
}
