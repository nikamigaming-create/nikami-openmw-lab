#include "../../openmw/fnvsidecaripc.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace
{
    OMW::FNVSidecar::Snapshot makeSnapshot()
    {
        OMW::FNVSidecar::Snapshot snapshot;
        snapshot.mValid = true;
        snapshot.mState = OMW::FNVSidecar::State::RetailReady;
        snapshot.mFlags = OMW::FNVSidecar::RetailReadyFlag;
        snapshot.mActorIndex = 3;
        snapshot.mActionIndex = 5;
        snapshot.mActionCount = 7;
        snapshot.mGeneration = 27;
        snapshot.mSequenceId = "all-actors-v1";
        snapshot.mRetailPayload = R"({
            "schema":"nikami-fnv-sidecar-retail/v1",
            "sequenceId":"all-actors-v1",
            "key":{"sequenceId":"all-actors-v1","actorIndex":3,"actionIndex":5},
            "generation":27,
            "actor":{"refForm":123,"baseForm":1060484,"spawned":false},
            "action":{"id":"shoot","retailPlayGroup":"AttackRight","requestedFrames":24,"elapsedFrames":24,"accepted":true},
            "animation":{"weaponOut":false,"aiming":false},
            "weaponPolicy":{"requestedForm":518692,"equippedForm":518692,"exact":true}
        })";
        return snapshot;
    }

    std::uint32_t crc32(std::string_view bytes)
    {
        std::uint32_t value = 0xFFFFFFFFu;
        for (const unsigned char byte : bytes)
        {
            value ^= byte;
            for (unsigned int bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ (0xEDB88320u & (0u - (value & 1u)));
        }
        return ~value;
    }
}

TEST(FNVSidecarIpc, SharedLayoutMatchesNKSCv1)
{
    EXPECT_EQ(sizeof(OMW::FNVSidecar::SharedHeader), 512u);
    EXPECT_EQ(sizeof(OMW::FNVSidecar::SharedBlock), 65536u);
    EXPECT_EQ(offsetof(OMW::FNVSidecar::SharedBlock, mRetailPayload), 512u);
    EXPECT_EQ(offsetof(OMW::FNVSidecar::SharedBlock, mOpenMwPayload), 33024u);
}

TEST(FNVSidecarIpc, ParsesAndCrossChecksRetailAction)
{
    const OMW::FNVSidecar::Snapshot snapshot = makeSnapshot();
    std::string error;
    const auto action = OMW::FNVSidecar::parseRetailAction(snapshot, error);
    ASSERT_TRUE(action.has_value()) << error;
    EXPECT_EQ(action->mSequenceId, "all-actors-v1");
    EXPECT_EQ(action->mActorIndex, 3u);
    EXPECT_EQ(action->mActionIndex, 5u);
    EXPECT_EQ(action->mGeneration, 27u);
    EXPECT_EQ(action->mRetailBaseForm, 1060484u);
    EXPECT_EQ(action->mActionId, "shoot");
    EXPECT_EQ(action->mRequestedFrames, 24u);
    EXPECT_EQ(action->mRequestedWeaponForm, 518692u);
    EXPECT_FALSE(action->mWeaponDrawn);
}

TEST(FNVSidecarIpc, ParsesDrawnWeaponState)
{
    OMW::FNVSidecar::Snapshot snapshot = makeSnapshot();
    const std::string holstered = "\"weaponOut\":false";
    const std::size_t offset = snapshot.mRetailPayload.find(holstered);
    ASSERT_NE(offset, std::string::npos);
    snapshot.mRetailPayload.replace(offset, holstered.size(), "\"weaponOut\":true");
    std::string error;
    const auto action = OMW::FNVSidecar::parseRetailAction(snapshot, error);
    ASSERT_TRUE(action.has_value()) << error;
    EXPECT_TRUE(action->mWeaponDrawn);
}

TEST(FNVSidecarIpc, RejectsMissingWeaponDrawState)
{
    OMW::FNVSidecar::Snapshot snapshot = makeSnapshot();
    const std::string animation = "\"animation\":{\"weaponOut\":false,\"aiming\":false}";
    const std::size_t offset = snapshot.mRetailPayload.find(animation);
    ASSERT_NE(offset, std::string::npos);
    snapshot.mRetailPayload.replace(offset, animation.size(), "\"animation\":{}");
    std::string error;
    EXPECT_FALSE(OMW::FNVSidecar::parseRetailAction(snapshot, error).has_value());
    EXPECT_EQ(error, "retail-payload-required-value-invalid");
}

TEST(FNVSidecarIpc, RejectsHeaderPayloadIdentityMismatch)
{
    OMW::FNVSidecar::Snapshot snapshot = makeSnapshot();
    snapshot.mActionIndex = 4;
    std::string error;
    EXPECT_FALSE(OMW::FNVSidecar::parseRetailAction(snapshot, error).has_value());
    EXPECT_EQ(error, "retail-payload-shared-header-identity-mismatch");
}

TEST(FNVSidecarIpc, RejectsUnprovenPayloadIntegrity)
{
    OMW::FNVSidecar::Snapshot snapshot = makeSnapshot();
    snapshot.mValid = false;
    std::string error;
    EXPECT_FALSE(OMW::FNVSidecar::parseRetailAction(snapshot, error).has_value());
    EXPECT_EQ(error, "retail-payload-crc-or-length-invalid");
}

TEST(FNVSidecarIpc, RejectsMissingRequestedState)
{
    OMW::FNVSidecar::Snapshot snapshot = makeSnapshot();
    snapshot.mRetailPayload = R"({
        "schema":"nikami-fnv-sidecar-retail/v1",
        "sequenceId":"all-actors-v1",
        "key":{"sequenceId":"all-actors-v1","actorIndex":3,"actionIndex":5},
        "generation":27,
        "actor":{"baseForm":1060484},
        "action":{"id":"shoot","requestedFrames":24}
    })";
    std::string error;
    EXPECT_FALSE(OMW::FNVSidecar::parseRetailAction(snapshot, error).has_value());
    EXPECT_EQ(error, "retail-payload-required-object-missing");
}

#ifdef _WIN32
TEST(FNVSidecarIpc, DrivesReadyCaptureAckAndCompleteTransitions)
{
    const std::string mappingName = "Local\\NikamiFNVSidecarTest."
        + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetTickCount64());
    const std::wstring wideName(mappingName.begin(), mappingName.end());
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(OMW::FNVSidecar::SharedBlockBytes), wideName.c_str());
    ASSERT_NE(mapping, nullptr);
    const std::wstring retailReadyName = wideName + L".retail-ready";
    const std::wstring openMwReadyName = wideName + L".openmw-ready";
    const std::wstring captureAckName = wideName + L".capture-ack";
    const std::wstring errorName = wideName + L".error";
    HANDLE retailReady = CreateEventW(nullptr, TRUE, FALSE, retailReadyName.c_str());
    HANDLE openMwReady = CreateEventW(nullptr, TRUE, FALSE, openMwReadyName.c_str());
    HANDLE captureAck = CreateEventW(nullptr, TRUE, FALSE, captureAckName.c_str());
    HANDLE error = CreateEventW(nullptr, TRUE, FALSE, errorName.c_str());
    ASSERT_NE(retailReady, nullptr);
    ASSERT_NE(openMwReady, nullptr);
    ASSERT_NE(captureAck, nullptr);
    ASSERT_NE(error, nullptr);
    auto* block = static_cast<OMW::FNVSidecar::SharedBlock*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, OMW::FNVSidecar::SharedBlockBytes));
    ASSERT_NE(block, nullptr);
    std::memset(block, 0, OMW::FNVSidecar::SharedBlockBytes);
    block->mHeader.mMagic = OMW::FNVSidecar::Magic;
    block->mHeader.mVersion = OMW::FNVSidecar::Version;
    block->mHeader.mHeaderBytes = static_cast<std::uint16_t>(OMW::FNVSidecar::SharedHeaderBytes);
    block->mHeader.mTotalBytes = static_cast<std::uint32_t>(OMW::FNVSidecar::SharedBlockBytes);
    block->mHeader.mState = static_cast<std::uint32_t>(OMW::FNVSidecar::State::RetailReady);
    block->mHeader.mFlags = OMW::FNVSidecar::RetailReadyFlag;
    block->mHeader.mActorIndex = 3;
    block->mHeader.mActionIndex = 5;
    block->mHeader.mActionCount = 7;
    block->mHeader.mGeneration = 27;
    strcpy_s(block->mHeader.mSequenceId, "all-actors-v1");
    const OMW::FNVSidecar::Snapshot source = makeSnapshot();
    std::memcpy(block->mRetailPayload, source.mRetailPayload.data(), source.mRetailPayload.size());
    block->mHeader.mRetailPayloadLength = static_cast<std::uint32_t>(source.mRetailPayload.size());
    block->mHeader.mRetailPayloadCrc32 = crc32(source.mRetailPayload);

    ASSERT_EQ(_putenv_s("OPENMW_FNV_SIDECAR_SHARED_MEMORY_NAME", mappingName.c_str()), 0);
    {
        OMW::FNVSidecar::Client client;
        ASSERT_TRUE(client.enabled());
        const OMW::FNVSidecar::Snapshot snapshot = client.snapshot();
        ASSERT_TRUE(snapshot.mValid);
        ASSERT_TRUE(client.publishReady(snapshot, 101, "{\"phase\":\"ready\"}"));
        EXPECT_NE(block->mHeader.mFlags & OMW::FNVSidecar::OpenMwReadyFlag, 0u);
        EXPECT_EQ(block->mHeader.mState,
            static_cast<std::uint32_t>(OMW::FNVSidecar::State::BothReady));
        EXPECT_EQ(WaitForSingleObject(openMwReady, 0), WAIT_OBJECT_0);

        block->mHeader.mState = static_cast<std::uint32_t>(OMW::FNVSidecar::State::CaptureIssued);
        ASSERT_TRUE(client.markCaptured(27, 3, 5, 102, "{\"phase\":\"captured\"}"));
        EXPECT_NE(block->mHeader.mFlags & OMW::FNVSidecar::OpenMwCapturedFlag, 0u);
        EXPECT_EQ(block->mHeader.mFlags & OMW::FNVSidecar::CaptureAckFlag, 0u);
        block->mHeader.mFlags |= OMW::FNVSidecar::RetailCapturedFlag;
        ASSERT_TRUE(client.acknowledgeCapture(27, 3, 5));
        EXPECT_NE(block->mHeader.mFlags & OMW::FNVSidecar::CaptureAckFlag, 0u);
        EXPECT_EQ(block->mHeader.mState,
            static_cast<std::uint32_t>(OMW::FNVSidecar::State::Advancing));
        EXPECT_EQ(WaitForSingleObject(captureAck, 0), WAIT_OBJECT_0);

        block->mHeader.mFlags |= OMW::FNVSidecar::RetailCompleteFlag;
        ASSERT_TRUE(client.markComplete(103));
        EXPECT_NE(block->mHeader.mFlags & OMW::FNVSidecar::OpenMwCompleteFlag, 0u);
        EXPECT_EQ(block->mHeader.mState,
            static_cast<std::uint32_t>(OMW::FNVSidecar::State::Complete));

        ASSERT_TRUE(client.setError(OMW::FNVSidecar::ErrorCode::InvalidPlan, "first-error"));
        EXPECT_EQ(block->mHeader.mErrorCode,
            static_cast<std::uint32_t>(OMW::FNVSidecar::ErrorCode::InvalidPlan));
        EXPECT_STREQ(block->mHeader.mErrorMessage, "first-error");
        EXPECT_EQ(block->mHeader.mState,
            static_cast<std::uint32_t>(OMW::FNVSidecar::State::Error));
        EXPECT_EQ(WaitForSingleObject(error, 0), WAIT_OBJECT_0);

        ASSERT_TRUE(client.setError(OMW::FNVSidecar::ErrorCode::InternalFault, "later-error"));
        EXPECT_EQ(block->mHeader.mErrorCode,
            static_cast<std::uint32_t>(OMW::FNVSidecar::ErrorCode::InvalidPlan));
        EXPECT_STREQ(block->mHeader.mErrorMessage, "first-error");
    }
    EXPECT_EQ(_putenv_s("OPENMW_FNV_SIDECAR_SHARED_MEMORY_NAME", ""), 0);
    UnmapViewOfFile(block);
    CloseHandle(mapping);
    CloseHandle(retailReady);
    CloseHandle(openMwReady);
    CloseHandle(captureAck);
    CloseHandle(error);
}
#endif
