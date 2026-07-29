#include <components/esm4/script.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace
{
    using DecodeError = ESM4::ScriptBytecodeDecodeError;

    TEST(Esm4ScdaDecoderTest, shouldDecodeVcg02StageFiveRetailFramesByteExactly)
    {
        // FalloutNV.esm 0010A214 VCG02, stage 5 entry 0 SCDA. SHA-256:
        // e192cce569c9ebe50d63d9bc68bbec5d2d273826da30510a912e958721cc7ee9
        const std::array<std::uint8_t, 28> bytecode{ 0xa3, 0x11, 0x0f, 0x00, 0x03, 0x00, 0x72, 0x01,
            0x00, 0x6e, 0x03, 0x00, 0x00, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00, 0xdd, 0x11, 0x05, 0x00,
            0x01, 0x00, 0x72, 0x01, 0x00 };
        std::vector<ESM4::ScriptBytecodeInstruction> instructions;

        const ESM4::ScriptBytecodeDecodeResult result = ESM4::decodeFalloutScriptBytecode(bytecode, instructions);

        ASSERT_TRUE(result.succeeded());
        EXPECT_EQ(result.bytesConsumed, bytecode.size());
        EXPECT_EQ(result.instructionCount, 2);
        ASSERT_EQ(instructions.size(), 2);

        EXPECT_EQ(instructions[0].offset, 0);
        EXPECT_EQ(instructions[0].opcode, 0x11a3); // SetObjectiveDisplayed
        EXPECT_FALSE(instructions[0].isReferenceFunction());
        ASSERT_EQ(instructions[0].arguments.size(), 15);
        EXPECT_EQ(instructions[0].arguments.data(), bytecode.data() + 4);
        EXPECT_EQ(std::vector<std::uint8_t>(instructions[0].arguments.begin(), instructions[0].arguments.end()),
            (std::vector<std::uint8_t>{
                0x03, 0x00, 0x72, 0x01, 0x00, 0x6e, 0x03, 0x00, 0x00, 0x00, 0x6e, 0x01, 0x00, 0x00, 0x00 }));

        EXPECT_EQ(instructions[1].offset, 19);
        EXPECT_EQ(instructions[1].opcode, 0x11dd); // ForceActiveQuest
        EXPECT_FALSE(instructions[1].isReferenceFunction());
        EXPECT_EQ(instructions[1].arguments.data(), bytecode.data() + 23);
        EXPECT_EQ(std::vector<std::uint8_t>(instructions[1].arguments.begin(), instructions[1].arguments.end()),
            (std::vector<std::uint8_t>{ 0x01, 0x00, 0x72, 0x01, 0x00 }));
    }

    TEST(Esm4ScdaDecoderTest, shouldExposeReferenceFunctionsAndUnknownOpcodesWithoutSkippingThem)
    {
        const std::array<std::uint8_t, 15> bytecode{
            0x1c, 0x00, 0x01, 0x00, 0x5e, 0x10, 0x00, 0x00, // retail reference-function framing
            0xef, 0xbe, 0x03, 0x00, 0xaa, 0xbb, 0xcc // deliberately unknown opcode
        };
        std::vector<ESM4::ScriptBytecodeInstruction> instructions;

        const ESM4::ScriptBytecodeDecodeResult result = ESM4::decodeFalloutScriptBytecode(bytecode, instructions);

        ASSERT_TRUE(result.succeeded());
        ASSERT_EQ(instructions.size(), 2);
        EXPECT_EQ(instructions[0].opcode, 0x105e);
        ASSERT_TRUE(instructions[0].callingReferenceIndex.has_value());
        EXPECT_EQ(*instructions[0].callingReferenceIndex, 1);
        EXPECT_TRUE(instructions[0].arguments.empty());
        EXPECT_EQ(instructions[1].opcode, 0xbeef);
        EXPECT_FALSE(instructions[1].isReferenceFunction());
        EXPECT_EQ(std::vector<std::uint8_t>(instructions[1].arguments.begin(), instructions[1].arguments.end()),
            (std::vector<std::uint8_t>{ 0xaa, 0xbb, 0xcc }));
    }

    TEST(Esm4ScdaDecoderTest, shouldRejectEveryMalformedFrameBoundaryWithoutReturningAPrefix)
    {
        const auto expectFailure = [](std::span<const std::uint8_t> bytecode, DecodeError expectedError,
                                       std::size_t expectedOffset, std::size_t expectedInstructionCount) {
            std::vector<ESM4::ScriptBytecodeInstruction> instructions{ ESM4::ScriptBytecodeInstruction{} };
            const ESM4::ScriptBytecodeDecodeResult result
                = ESM4::decodeFalloutScriptBytecode(bytecode, instructions);
            EXPECT_FALSE(result.succeeded());
            EXPECT_EQ(result.error, expectedError);
            EXPECT_EQ(result.bytesConsumed, expectedOffset);
            EXPECT_EQ(result.instructionCount, expectedInstructionCount);
            EXPECT_TRUE(instructions.empty());
        };

        const std::array<std::uint8_t, 3> truncatedHeader{ 0xa3, 0x11, 0x00 };
        expectFailure(truncatedHeader, DecodeError::TruncatedInstructionHeader, 0, 0);

        const std::array<std::uint8_t, 6> truncatedReferenceHeader{ 0x1c, 0x00, 0x01, 0x00, 0xa3, 0x11 };
        expectFailure(truncatedReferenceHeader, DecodeError::TruncatedReferenceInstructionHeader, 0, 0);

        const std::array<std::uint8_t, 5> truncatedPayload{ 0xa3, 0x11, 0x0f, 0x00, 0x03 };
        expectFailure(truncatedPayload, DecodeError::ArgumentPayloadOverrun, 0, 0);

        const std::array<std::uint8_t, 8> truncatedReferencePayload{
            0x1c, 0x00, 0x01, 0x00, 0xa3, 0x11, 0xff, 0xff };
        expectFailure(truncatedReferencePayload, DecodeError::ArgumentPayloadOverrun, 0, 0);

        const std::array<std::uint8_t, 5> malformedTrailingByte{ 0xdd, 0x11, 0x00, 0x00, 0xff };
        expectFailure(malformedTrailingByte, DecodeError::TruncatedInstructionHeader, 4, 1);
    }

    TEST(Esm4ScdaDecoderTest, shouldAcceptEmptyAndExactBoundaryPayloads)
    {
        std::vector<ESM4::ScriptBytecodeInstruction> instructions;
        const std::array<std::uint8_t, 0> empty{};
        const ESM4::ScriptBytecodeDecodeResult emptyResult = ESM4::decodeFalloutScriptBytecode(empty, instructions);
        EXPECT_TRUE(emptyResult.succeeded());
        EXPECT_EQ(emptyResult.bytesConsumed, 0);
        EXPECT_EQ(emptyResult.instructionCount, 0);
        EXPECT_TRUE(instructions.empty());

        const std::array<std::uint8_t, 6> exactPayload{ 0x34, 0x12, 0x02, 0x00, 0xaa, 0xbb };
        const ESM4::ScriptBytecodeDecodeResult exactResult
            = ESM4::decodeFalloutScriptBytecode(exactPayload, instructions);
        ASSERT_TRUE(exactResult.succeeded());
        ASSERT_EQ(instructions.size(), 1);
        EXPECT_EQ(instructions[0].arguments.size(), 2);
        EXPECT_EQ(instructions[0].arguments[0], 0xaa);
        EXPECT_EQ(instructions[0].arguments[1], 0xbb);
    }
}
