#ifndef OPENMW_APPS_OPENMW_FNVSIDECARIPC_H
#define OPENMW_APPS_OPENMW_FNVSIDECARIPC_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace OMW::FNVSidecar
{
    constexpr std::uint32_t Magic = 0x43534B4Eu; // "NKSC" in little-endian memory.
    constexpr std::uint16_t Version = 1;
    constexpr std::size_t SharedBlockBytes = 65536;
    constexpr std::size_t SharedHeaderBytes = 512;
    constexpr std::size_t PayloadBytes = (SharedBlockBytes - SharedHeaderBytes) / 2;

    enum class State : std::uint32_t
    {
        Empty = 0,
        PlanLoaded = 1,
        RetailPreparing = 2,
        RetailReady = 3,
        BothReady = 4,
        CaptureIssued = 5,
        WaitingCaptureAck = 6,
        Advancing = 7,
        Complete = 8,
        Error = 0xFFFFFFFFu,
    };

    enum Flag : std::uint32_t
    {
        RetailReadyFlag = 1u << 0,
        OpenMwReadyFlag = 1u << 1,
        RetailCapturedFlag = 1u << 2,
        OpenMwCapturedFlag = 1u << 3,
        CaptureAckFlag = 1u << 4,
        RetailCompleteFlag = 1u << 5,
        OpenMwCompleteFlag = 1u << 6,
        ErrorFlag = 1u << 31,
    };

    enum class ErrorCode : std::uint32_t
    {
        None = 0,
        InvalidPlan = 1,
        ActorUnavailable = 2,
        ActorSpawnFailed = 3,
        WeaponPolicyFailed = 4,
        ActionRejected = 5,
        RetailReadyTimeout = 6,
        OpenMwReadyTimeout = 7,
        ScreenshotTimeout = 8,
        CaptureAckTimeout = 9,
        SharedMemoryFault = 10,
        InternalFault = 11,
    };

#pragma pack(push, 8)
    struct alignas(8) SharedHeader
    {
        std::uint32_t mMagic;
        std::uint16_t mVersion;
        std::uint16_t mHeaderBytes;
        std::uint32_t mTotalBytes;
        volatile std::int32_t mMutex;
        volatile std::uint32_t mState;
        volatile std::uint32_t mFlags;
        volatile std::uint32_t mErrorCode;
        volatile std::uint32_t mActorIndex;
        volatile std::uint32_t mActionIndex;
        volatile std::uint32_t mActionCount;
        std::uint32_t mReserved0;
        std::uint32_t mReserved1;
        volatile std::uint64_t mGeneration;
        volatile std::uint64_t mRetailFrame;
        volatile std::uint64_t mOpenMwFrame;
        volatile std::uint64_t mCaptureOrdinal;
        volatile std::uint64_t mDeadlineTickMs;
        volatile std::uint32_t mRetailPayloadLength;
        volatile std::uint32_t mRetailPayloadCrc32;
        volatile std::uint32_t mOpenMwPayloadLength;
        volatile std::uint32_t mOpenMwPayloadCrc32;
        char mSequenceId[128];
        char mErrorMessage[256];
        std::uint8_t mReserved[24];
    };

    struct alignas(8) SharedBlock
    {
        SharedHeader mHeader;
        char mRetailPayload[PayloadBytes];
        char mOpenMwPayload[PayloadBytes];
    };
#pragma pack(pop)

    static_assert(sizeof(SharedHeader) == SharedHeaderBytes);
    static_assert(offsetof(SharedHeader, mMagic) == 0);
    static_assert(offsetof(SharedHeader, mMutex) == 12);
    static_assert(offsetof(SharedHeader, mState) == 16);
    static_assert(offsetof(SharedHeader, mGeneration) == 48);
    static_assert(offsetof(SharedHeader, mRetailPayloadLength) == 88);
    static_assert(offsetof(SharedHeader, mSequenceId) == 104);
    static_assert(offsetof(SharedHeader, mErrorMessage) == 232);
    static_assert(offsetof(SharedBlock, mRetailPayload) == SharedHeaderBytes);
    static_assert(offsetof(SharedBlock, mOpenMwPayload) == SharedHeaderBytes + PayloadBytes);
    static_assert(sizeof(SharedBlock) == SharedBlockBytes);

    struct Snapshot
    {
        bool mValid = false;
        State mState = State::Empty;
        std::uint32_t mFlags = 0;
        ErrorCode mErrorCode = ErrorCode::None;
        std::uint32_t mActorIndex = 0;
        std::uint32_t mActionIndex = 0;
        std::uint32_t mActionCount = 0;
        std::uint64_t mGeneration = 0;
        std::uint64_t mRetailFrame = 0;
        std::uint64_t mOpenMwFrame = 0;
        std::uint64_t mCaptureOrdinal = 0;
        std::uint64_t mDeadlineTickMs = 0;
        std::string mSequenceId;
        std::string mRetailPayload;
        std::string mErrorMessage;
    };

    struct RetailAction
    {
        std::string mSchema;
        std::string mSequenceId;
        std::uint32_t mActorIndex = 0;
        std::uint32_t mActionIndex = 0;
        std::uint64_t mGeneration = 0;
        std::uint32_t mRetailBaseForm = 0;
        std::string mActionId;
        std::uint32_t mRequestedFrames = 0;
        std::uint32_t mRequestedWeaponForm = 0;
    };

    // Strictly decode the identity and requested state that OpenMW must consume.
    // Unknown telemetry members are intentionally ignored, while every member in
    // RetailAction is required and cross-checked against the shared header.
    std::optional<RetailAction> parseRetailAction(const Snapshot& snapshot, std::string& error);

    // Same epoch as the shared header's deadlineTickMs on Windows.
    std::uint64_t monotonicTickMilliseconds();

    // Non-blocking OpenMW endpoint for the retail/OpenMW lockstep proof protocol.
    // The class is a no-op on non-Windows platforms and when no mapping name is configured.
    class Client
    {
    public:
        Client();
        ~Client();

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        bool enabled() const;
        const std::string& mappingName() const;
        Snapshot snapshot();

        bool publishReady(const Snapshot& retail, std::uint64_t openMwFrame, const std::string& payload);
        bool markCaptured(std::uint64_t generation, std::uint32_t actorIndex, std::uint32_t actionIndex,
            std::uint64_t openMwFrame, const std::string& payload);
        bool acknowledgeCapture(
            std::uint64_t generation, std::uint32_t actorIndex, std::uint32_t actionIndex);
        bool markComplete(std::uint64_t openMwFrame);
        bool setError(ErrorCode code, const std::string& message);

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
