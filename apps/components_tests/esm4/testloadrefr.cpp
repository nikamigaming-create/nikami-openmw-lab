#include <components/esm4/loadrefr.hpp>

#include <gtest/gtest.h>

#include <array>
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

    TEST(Esm4ReferenceTest, loadsFalloutTriggerPrimitiveMetadata)
    {
        const std::array<float, 3> bounds{ 80.414f, 104.688f, 140.f };
        const std::array<float, 4> color{ 0.25f, 0.5f, 0.75f, 0.4f };
        const std::uint32_t type = ESM4::Primitive::Box;

        std::string primitive;
        ESM4Test::appendPod(primitive, bounds);
        ESM4Test::appendPod(primitive, color);
        ESM4Test::appendPod(primitive, type);

        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "XPRM", primitive);

        const ESM4::Reference reference = loadReference(std::move(recordData));

        ASSERT_TRUE(reference.mHasPrimitive);
        EXPECT_EQ(reference.mPrimitive.mBounds, bounds);
        EXPECT_EQ(reference.mPrimitive.mColor, color);
        EXPECT_EQ(reference.mPrimitive.mType, type);
    }

    TEST(Esm4ReferenceTest, rejectsMalformedTriggerPrimitiveWithoutLosingAlignment)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "XPRM", std::string(sizeof(float), '\0'));

        ESM::Position position{};
        position.pos[0] = 101.f;
        std::string positionData;
        ESM4Test::appendPod(positionData, position);
        ESM4Test::appendSubRecord(recordData, "DATA", positionData);

        const ESM4::Reference reference = loadReference(std::move(recordData));

        EXPECT_FALSE(reference.mHasPrimitive);
        EXPECT_FLOAT_EQ(reference.mPos.pos[0], position.pos[0]);
    }
}
