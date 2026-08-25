#include <components/esm4/loadrefr.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "testutil.hpp"

namespace
{
    constexpr std::uint32_t kLinkedReferenceRaw = 0x00000456;
    constexpr std::uint32_t kLinkedReferenceAdjusted = 0x02000456;
    constexpr std::uint32_t kNullFormIdRaw = 0;
    constexpr float kPatrolIdleTime = 2.5f;

    ESM4::Reference loadReference(std::string recordData)
    {
        auto reader = ESM4Test::makeReader("REFR", std::move(recordData));
        ESM4::Reference reference;
        reference.load(*reader);
        return reference;
    }

    TEST(Esm4ReferenceTest, loadsFalloutPatrolMetadataAndAdjustsLinkedReference)
    {
        std::string recordData;
        std::string linkedReference;
        ESM4Test::appendPod(linkedReference, kLinkedReferenceRaw);
        ESM4Test::appendSubRecord(recordData, "XLKR", linkedReference);

        std::string patrolIdleTime;
        ESM4Test::appendPod(patrolIdleTime, kPatrolIdleTime);
        ESM4Test::appendSubRecord(recordData, "XPRD", patrolIdleTime);
        ESM4Test::appendSubRecord(recordData, "XPPA", std::string_view{});

        const ESM4::Reference reference = loadReference(std::move(recordData));

        EXPECT_EQ(reference.mLinkedReference, ESM::FormId::fromUint32(kLinkedReferenceAdjusted));
        EXPECT_FLOAT_EQ(reference.mPatrolIdleTime, kPatrolIdleTime);
        EXPECT_TRUE(reference.mHasPatrolIdleTime);
        EXPECT_TRUE(reference.mIsPatrolIdleScriptMarker);
    }

    TEST(Esm4ReferenceTest, rejectsMalformedPatrolPayloadsWithoutMutatingState)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "XLKR", std::string(sizeof(ESM::FormId32) - sizeof(std::uint8_t), '\0'));
        ESM4Test::appendSubRecord(recordData, "XPRD", std::string(sizeof(float) - sizeof(std::uint8_t), '\0'));
        ESM4Test::appendSubRecord(recordData, "XPPA", std::string(sizeof(std::uint8_t), '\0'));

        const ESM4::Reference reference = loadReference(std::move(recordData));

        EXPECT_TRUE(reference.mLinkedReference.isZeroOrUnset());
        EXPECT_FLOAT_EQ(reference.mPatrolIdleTime, ESM4::Reference::sDefaultPatrolIdleTime);
        EXPECT_FALSE(reference.mHasPatrolIdleTime);
        EXPECT_FALSE(reference.mIsPatrolIdleScriptMarker);
    }

    TEST(Esm4ReferenceTest, preservesNullPatrolLink)
    {
        std::string recordData;
        std::string nullReference;
        ESM4Test::appendPod(nullReference, kNullFormIdRaw);
        ESM4Test::appendSubRecord(recordData, "XLKR", nullReference);

        const ESM4::Reference reference = loadReference(std::move(recordData));

        EXPECT_TRUE(reference.mLinkedReference.isZeroOrUnset());
    }
}
