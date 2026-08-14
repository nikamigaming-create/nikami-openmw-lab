#include "fnvsidecaripc.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <system_error>

#include <components/debug/debuglog.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace OMW::FNVSidecar
{
    namespace
    {
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

        std::string boundedString(const char* value, std::size_t capacity)
        {
            const char* end = static_cast<const char*>(std::memchr(value, '\0', capacity));
            return std::string(value, end != nullptr ? end : value + capacity);
        }

        std::optional<std::string_view> jsonObject(std::string_view json, std::string_view key)
        {
            const std::string marker = "\"" + std::string(key) + "\"";
            std::size_t position = json.find(marker);
            if (position == std::string_view::npos)
                return std::nullopt;
            position = json.find(':', position + marker.size());
            if (position == std::string_view::npos)
                return std::nullopt;
            position = json.find_first_not_of(" \t\r\n", position + 1);
            if (position == std::string_view::npos || json[position] != '{')
                return std::nullopt;

            const std::size_t begin = position;
            unsigned int depth = 0;
            bool inString = false;
            bool escaped = false;
            for (; position < json.size(); ++position)
            {
                const char ch = json[position];
                if (inString)
                {
                    if (escaped)
                        escaped = false;
                    else if (ch == '\\')
                        escaped = true;
                    else if (ch == '"')
                        inString = false;
                    continue;
                }
                if (ch == '"')
                    inString = true;
                else if (ch == '{')
                    ++depth;
                else if (ch == '}' && --depth == 0)
                    return json.substr(begin, position - begin + 1);
            }
            return std::nullopt;
        }

        std::optional<std::string> jsonString(std::string_view json, std::string_view key)
        {
            const std::string marker = "\"" + std::string(key) + "\"";
            std::size_t position = json.find(marker);
            if (position == std::string_view::npos)
                return std::nullopt;
            position = json.find(':', position + marker.size());
            if (position == std::string_view::npos)
                return std::nullopt;
            position = json.find_first_not_of(" \t\r\n", position + 1);
            if (position == std::string_view::npos || json[position] != '"')
                return std::nullopt;
            ++position;
            std::string result;
            bool escaped = false;
            for (; position < json.size(); ++position)
            {
                const char ch = json[position];
                if (escaped)
                {
                    switch (ch)
                    {
                        case '"': result.push_back('"'); break;
                        case '\\': result.push_back('\\'); break;
                        case '/': result.push_back('/'); break;
                        case 'b': result.push_back('\b'); break;
                        case 'f': result.push_back('\f'); break;
                        case 'n': result.push_back('\n'); break;
                        case 'r': result.push_back('\r'); break;
                        case 't': result.push_back('\t'); break;
                        default: return std::nullopt;
                    }
                    escaped = false;
                }
                else if (ch == '\\')
                    escaped = true;
                else if (ch == '"')
                    return result;
                else
                    result.push_back(ch);
            }
            return std::nullopt;
        }

        std::optional<std::uint64_t> jsonUnsigned(std::string_view json, std::string_view key)
        {
            const std::string marker = "\"" + std::string(key) + "\"";
            std::size_t position = json.find(marker);
            if (position == std::string_view::npos)
                return std::nullopt;
            position = json.find(':', position + marker.size());
            if (position == std::string_view::npos)
                return std::nullopt;
            position = json.find_first_not_of(" \t\r\n", position + 1);
            if (position == std::string_view::npos)
                return std::nullopt;
            const char* first = json.data() + position;
            const char* last = json.data() + json.size();
            std::uint64_t value = 0;
            const auto parsed = std::from_chars(first, last, value, 10);
            if (parsed.ec != std::errc() || parsed.ptr == first)
                return std::nullopt;
            return value;
        }

        std::optional<bool> jsonBoolean(std::string_view json, std::string_view key)
        {
            const std::string marker = "\"" + std::string(key) + "\"";
            std::size_t position = json.find(marker);
            if (position == std::string_view::npos)
                return std::nullopt;
            position = json.find(':', position + marker.size());
            if (position == std::string_view::npos)
                return std::nullopt;
            position = json.find_first_not_of(" \t\r\n", position + 1);
            if (position == std::string_view::npos)
                return std::nullopt;
            const auto terminated = [&](std::size_t end) {
                return end == json.size() || json.find_first_of(" \t\r\n,}", end) == end;
            };
            if (json.substr(position, 4) == "true" && terminated(position + 4))
                return true;
            if (json.substr(position, 5) == "false" && terminated(position + 5))
                return false;
            return std::nullopt;
        }

        template <std::size_t Size>
        std::optional<std::array<std::uint32_t, Size>> jsonUnsignedArray(
            std::string_view json, std::string_view key)
        {
            const std::string marker = "\"" + std::string(key) + "\"";
            std::size_t position = json.find(marker);
            if (position == std::string_view::npos)
                return std::nullopt;
            position = json.find(':', position + marker.size());
            if (position == std::string_view::npos)
                return std::nullopt;
            position = json.find_first_not_of(" \t\r\n", position + 1);
            if (position == std::string_view::npos || json[position] != '[')
                return std::nullopt;
            ++position;

            std::array<std::uint32_t, Size> result{};
            for (std::size_t index = 0; index < Size; ++index)
            {
                position = json.find_first_not_of(" \t\r\n", position);
                if (position == std::string_view::npos)
                    return std::nullopt;
                const char* first = json.data() + position;
                const char* last = json.data() + json.size();
                std::uint64_t value = 0;
                const auto parsed = std::from_chars(first, last, value, 10);
                if (parsed.ec != std::errc() || parsed.ptr == first
                    || value > std::numeric_limits<std::uint32_t>::max())
                    return std::nullopt;
                result[index] = static_cast<std::uint32_t>(value);
                position = static_cast<std::size_t>(parsed.ptr - json.data());
                position = json.find_first_not_of(" \t\r\n", position);
                if (position == std::string_view::npos)
                    return std::nullopt;
                const char expected = index + 1 == Size ? ']' : ',';
                if (json[position] != expected)
                    return std::nullopt;
                ++position;
            }
            return result;
        }

        float floatFromBits(std::uint32_t bits)
        {
            float value = 0.f;
            static_assert(sizeof(value) == sizeof(bits));
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

#ifdef _WIN32
        std::wstring utf8ToWide(std::string_view text)
        {
            if (text.empty())
                return {};
            const int required = MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (required <= 0)
                return {};
            std::wstring result(static_cast<std::size_t>(required), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                    result.data(), required)
                != required)
                return {};
            return result;
        }
#endif
    }

    struct Client::Impl
    {
        std::string mMappingName;
#ifdef _WIN32
        HANDLE mMapping = nullptr;
        HANDLE mRetailReadyEvent = nullptr;
        HANDLE mOpenMwReadyEvent = nullptr;
        HANDLE mCaptureAckEvent = nullptr;
        HANDLE mErrorEvent = nullptr;
        SharedBlock* mBlock = nullptr;

        bool lock()
        {
            if (mBlock == nullptr)
                return false;
            constexpr unsigned int attempts = 2000;
            for (unsigned int attempt = 0; attempt < attempts; ++attempt)
            {
                auto* mutex = reinterpret_cast<volatile LONG*>(&mBlock->mHeader.mMutex);
                if (InterlockedCompareExchange(mutex, 1, 0) == 0)
                    return true;
                if ((attempt & 31u) == 31u)
                    SwitchToThread();
            }
            return false;
        }

        void unlock()
        {
            auto* mutex = reinterpret_cast<volatile LONG*>(&mBlock->mHeader.mMutex);
            InterlockedExchange(mutex, 0);
        }

        bool validHeader() const
        {
            return mBlock != nullptr && mBlock->mHeader.mMagic == Magic && mBlock->mHeader.mVersion == Version
                && mBlock->mHeader.mHeaderBytes == SharedHeaderBytes
                && mBlock->mHeader.mTotalBytes == SharedBlockBytes;
        }

        void close()
        {
            if (mBlock != nullptr)
                UnmapViewOfFile(mBlock);
            if (mMapping != nullptr)
                CloseHandle(mMapping);
            if (mRetailReadyEvent != nullptr)
                CloseHandle(mRetailReadyEvent);
            if (mOpenMwReadyEvent != nullptr)
                CloseHandle(mOpenMwReadyEvent);
            if (mCaptureAckEvent != nullptr)
                CloseHandle(mCaptureAckEvent);
            if (mErrorEvent != nullptr)
                CloseHandle(mErrorEvent);
            mBlock = nullptr;
            mMapping = nullptr;
            mRetailReadyEvent = nullptr;
            mOpenMwReadyEvent = nullptr;
            mCaptureAckEvent = nullptr;
            mErrorEvent = nullptr;
        }
#endif
    };

    Client::Client()
        : mImpl(std::make_unique<Impl>())
    {
        const char* configured = std::getenv("OPENMW_FNV_SIDECAR_SHARED_MEMORY_NAME");
        if (configured == nullptr || *configured == '\0')
            configured = std::getenv("NIKAMI_ORACLE_SHARED_MEMORY_NAME");
        if (configured == nullptr || *configured == '\0')
            return;
        mImpl->mMappingName = configured;

#ifdef _WIN32
        const std::wstring mappingName = utf8ToWide(mImpl->mMappingName);
        if (mappingName.empty())
        {
            Log(Debug::Error) << "FNV sidecar: invalid shared-memory mapping name";
            return;
        }

        // The coordinator owns creation and initialization. Opening only keeps a
        // mistyped/stale channel from becoming a second, apparently valid session.
        mImpl->mMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str());
        if (mImpl->mMapping == nullptr)
        {
            Log(Debug::Error) << "FNV sidecar: OpenFileMapping failed error=" << GetLastError();
            return;
        }

        mImpl->mBlock = static_cast<SharedBlock*>(
            MapViewOfFile(mImpl->mMapping, FILE_MAP_ALL_ACCESS, 0, 0, SharedBlockBytes));
        if (mImpl->mBlock == nullptr)
        {
            Log(Debug::Error) << "FNV sidecar: MapViewOfFile failed error=" << GetLastError();
            mImpl->close();
            return;
        }
        if (!mImpl->validHeader())
        {
            Log(Debug::Error) << "FNV sidecar: protocol header mismatch name=" << mImpl->mMappingName;
            mImpl->close();
            return;
        }

        const auto openEvent = [&](std::string_view suffix) {
            return OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                utf8ToWide(mImpl->mMappingName + std::string(suffix)).c_str());
        };
        mImpl->mOpenMwReadyEvent = openEvent(".openmw-ready");
        mImpl->mRetailReadyEvent = openEvent(".retail-ready");
        mImpl->mCaptureAckEvent = openEvent(".capture-ack");
        mImpl->mErrorEvent = openEvent(".error");
        if (mImpl->mRetailReadyEvent == nullptr || mImpl->mOpenMwReadyEvent == nullptr || mImpl->mCaptureAckEvent == nullptr
            || mImpl->mErrorEvent == nullptr)
        {
            Log(Debug::Error) << "FNV sidecar: required event open failed error=" << GetLastError();
            mImpl->close();
            return;
        }
        Log(Debug::Info) << "FNV sidecar: OpenMW endpoint ready mapping=\"" << mImpl->mMappingName
                         << "\" bytes=" << SharedBlockBytes << " version=" << Version;
#else
        Log(Debug::Warning) << "FNV sidecar: shared-memory lockstep is only available on Windows";
#endif
    }

    Client::~Client()
    {
#ifdef _WIN32
        if (mImpl != nullptr)
            mImpl->close();
#endif
    }

    bool Client::enabled() const
    {
#ifdef _WIN32
        return mImpl != nullptr && mImpl->validHeader();
#else
        return false;
#endif
    }

    const std::string& Client::mappingName() const
    {
        return mImpl->mMappingName;
    }

    Snapshot Client::snapshot()
    {
        Snapshot result;
#ifdef _WIN32
        if (!enabled() || !mImpl->lock())
            return result;

        const SharedHeader& header = mImpl->mBlock->mHeader;
        result.mState = static_cast<State>(header.mState);
        result.mFlags = header.mFlags;
        result.mErrorCode = static_cast<ErrorCode>(header.mErrorCode);
        result.mActorIndex = header.mActorIndex;
        result.mActionIndex = header.mActionIndex;
        result.mActionCount = header.mActionCount;
        result.mGeneration = header.mGeneration;
        result.mRetailFrame = header.mRetailFrame;
        result.mOpenMwFrame = header.mOpenMwFrame;
        result.mCaptureOrdinal = header.mCaptureOrdinal;
        result.mDeadlineTickMs = header.mDeadlineTickMs;
        result.mSequenceId = boundedString(header.mSequenceId, sizeof(header.mSequenceId));
        result.mErrorMessage = boundedString(header.mErrorMessage, sizeof(header.mErrorMessage));
        const std::uint32_t requestedLength = header.mRetailPayloadLength;
        const std::uint32_t length
            = std::min<std::uint32_t>(requestedLength, static_cast<std::uint32_t>(PayloadBytes));
        result.mRetailPayload.assign(mImpl->mBlock->mRetailPayload, mImpl->mBlock->mRetailPayload + length);
        result.mValid = length == header.mRetailPayloadLength
            && crc32(result.mRetailPayload) == header.mRetailPayloadCrc32;
        if (length == 0 && header.mRetailPayloadCrc32 == crc32(std::string_view{}))
            result.mValid = true;
        mImpl->unlock();
#endif
        return result;
    }

    bool Client::publishReady(const Snapshot& retail, std::uint64_t openMwFrame, const std::string& payload)
    {
#ifdef _WIN32
        if (!enabled() || !retail.mValid || payload.size() > PayloadBytes || !mImpl->lock())
            return false;
        SharedHeader& header = mImpl->mBlock->mHeader;
        const bool matches = header.mGeneration == retail.mGeneration && header.mActorIndex == retail.mActorIndex
            && header.mActionIndex == retail.mActionIndex && (header.mFlags & RetailReadyFlag) != 0
            && header.mState == static_cast<std::uint32_t>(State::RetailReady)
            && (header.mFlags & ErrorFlag) == 0;
        if (!matches)
        {
            mImpl->unlock();
            return false;
        }

        std::memcpy(mImpl->mBlock->mOpenMwPayload, payload.data(), payload.size());
        if (payload.size() < PayloadBytes)
            mImpl->mBlock->mOpenMwPayload[payload.size()] = '\0';
        header.mOpenMwPayloadLength = static_cast<std::uint32_t>(payload.size());
        header.mOpenMwPayloadCrc32 = crc32(payload);
        header.mOpenMwFrame = openMwFrame;
        header.mFlags |= OpenMwReadyFlag;
        if ((header.mFlags & RetailReadyFlag) != 0)
            header.mState = static_cast<std::uint32_t>(State::BothReady);
        mImpl->unlock();
        SetEvent(mImpl->mOpenMwReadyEvent);
        return true;
#else
        (void)retail;
        (void)openMwFrame;
        (void)payload;
        return false;
#endif
    }

    bool Client::markCaptured(std::uint64_t generation, std::uint32_t actorIndex, std::uint32_t actionIndex,
        std::uint64_t openMwFrame, const std::string& payload)
    {
#ifdef _WIN32
        if (!enabled() || payload.size() > PayloadBytes || !mImpl->lock())
            return false;
        SharedHeader& header = mImpl->mBlock->mHeader;
        if (header.mGeneration != generation || header.mActorIndex != actorIndex
            || header.mActionIndex != actionIndex || (header.mFlags & OpenMwReadyFlag) == 0
            || (header.mState != static_cast<std::uint32_t>(State::CaptureIssued)
                && header.mState != static_cast<std::uint32_t>(State::WaitingCaptureAck))
            || (header.mFlags & ErrorFlag) != 0)
        {
            mImpl->unlock();
            return false;
        }
        std::memcpy(mImpl->mBlock->mOpenMwPayload, payload.data(), payload.size());
        if (payload.size() < PayloadBytes)
            mImpl->mBlock->mOpenMwPayload[payload.size()] = '\0';
        header.mOpenMwPayloadLength = static_cast<std::uint32_t>(payload.size());
        header.mOpenMwPayloadCrc32 = crc32(payload);
        header.mOpenMwFrame = openMwFrame;
        header.mFlags |= OpenMwCapturedFlag;
        header.mState = static_cast<std::uint32_t>(State::WaitingCaptureAck);
        const bool acknowledge = (header.mFlags & RetailCapturedFlag) != 0;
        if (acknowledge)
        {
            header.mFlags |= CaptureAckFlag;
            header.mState = static_cast<std::uint32_t>(State::Advancing);
        }
        mImpl->unlock();
        SetEvent(mImpl->mOpenMwReadyEvent);
        if (acknowledge)
            SetEvent(mImpl->mCaptureAckEvent);
        return true;
#else
        (void)generation;
        (void)actorIndex;
        (void)actionIndex;
        (void)openMwFrame;
        (void)payload;
        return false;
#endif
    }

    bool Client::acknowledgeCapture(
        std::uint64_t generation, std::uint32_t actorIndex, std::uint32_t actionIndex)
    {
#ifdef _WIN32
        if (!enabled() || !mImpl->lock())
            return false;
        SharedHeader& header = mImpl->mBlock->mHeader;
        const std::uint32_t captures = RetailCapturedFlag | OpenMwCapturedFlag;
        const bool matches = header.mGeneration == generation && header.mActorIndex == actorIndex
            && header.mActionIndex == actionIndex && (header.mFlags & captures) == captures
            && (header.mFlags & ErrorFlag) == 0;
        if (matches)
        {
            header.mFlags |= CaptureAckFlag;
            header.mState = static_cast<std::uint32_t>(State::Advancing);
        }
        mImpl->unlock();
        if (matches)
            SetEvent(mImpl->mCaptureAckEvent);
        return matches;
#else
        (void)generation;
        (void)actorIndex;
        (void)actionIndex;
        return false;
#endif
    }

    bool Client::markComplete(std::uint64_t openMwFrame)
    {
#ifdef _WIN32
        if (!enabled() || !mImpl->lock())
            return false;
        SharedHeader& header = mImpl->mBlock->mHeader;
        if ((header.mFlags & RetailCompleteFlag) == 0 || (header.mFlags & ErrorFlag) != 0)
        {
            mImpl->unlock();
            return false;
        }
        header.mOpenMwFrame = openMwFrame;
        header.mFlags |= OpenMwCompleteFlag;
        if ((header.mFlags & RetailCompleteFlag) != 0)
            header.mState = static_cast<std::uint32_t>(State::Complete);
        mImpl->unlock();
        SetEvent(mImpl->mOpenMwReadyEvent);
        return true;
#else
        (void)openMwFrame;
        return false;
#endif
    }

    bool Client::setError(ErrorCode code, const std::string& message)
    {
#ifdef _WIN32
        if (!enabled() || !mImpl->lock())
            return false;
        SharedHeader& header = mImpl->mBlock->mHeader;
        const bool errorAlreadyActive = (header.mFlags & ErrorFlag) != 0
            || header.mState == static_cast<std::uint32_t>(State::Error)
            || header.mErrorCode != static_cast<std::uint32_t>(ErrorCode::None);
        if (!errorAlreadyActive)
        {
            header.mErrorCode = static_cast<std::uint32_t>(code);
            header.mFlags |= ErrorFlag;
            header.mState = static_cast<std::uint32_t>(State::Error);
            const std::size_t length = std::min(message.size(), sizeof(header.mErrorMessage) - 1);
            std::memcpy(header.mErrorMessage, message.data(), length);
            header.mErrorMessage[length] = '\0';
        }
        mImpl->unlock();
        SetEvent(mImpl->mErrorEvent);
        return true;
#else
        (void)code;
        (void)message;
        return false;
#endif
    }

    std::optional<RetailAction> parseRetailAction(const Snapshot& snapshot, std::string& error)
    {
        error.clear();
        if (!snapshot.mValid)
        {
            error = "retail-payload-crc-or-length-invalid";
            return std::nullopt;
        }
        const auto schema = jsonString(snapshot.mRetailPayload, "schema");
        const auto sequence = jsonString(snapshot.mRetailPayload, "sequenceId");
        const auto key = jsonObject(snapshot.mRetailPayload, "key");
        const auto actor = jsonObject(snapshot.mRetailPayload, "actor");
        const auto action = jsonObject(snapshot.mRetailPayload, "action");
        const auto animation = jsonObject(snapshot.mRetailPayload, "animation");
        const auto weapon = jsonObject(snapshot.mRetailPayload, "weaponPolicy");
        if (!schema || !sequence || !key || !actor || !action || !animation || !weapon)
        {
            error = "retail-payload-required-object-missing";
            return std::nullopt;
        }
        const auto keySequence = jsonString(*key, "sequenceId");
        const auto actorIndex = jsonUnsigned(*key, "actorIndex");
        const auto actionIndex = jsonUnsigned(*key, "actionIndex");
        const auto generation = jsonUnsigned(snapshot.mRetailPayload, "generation");
        const auto baseForm = jsonUnsigned(*actor, "baseForm");
        const auto actionId = jsonString(*action, "id");
        const auto requestedFrames = jsonUnsigned(*action, "requestedFrames");
        const auto weaponDrawn = jsonBoolean(*animation, "weaponOut");
        const auto requestedWeapon = jsonUnsigned(*weapon, "requestedForm");
        if (!keySequence || !actorIndex || !actionIndex || !generation || !baseForm || !actionId
            || !requestedFrames || !weaponDrawn || !requestedWeapon
            || *actorIndex > std::numeric_limits<std::uint32_t>::max()
            || *actionIndex > std::numeric_limits<std::uint32_t>::max()
            || *baseForm > std::numeric_limits<std::uint32_t>::max()
            || *requestedFrames > std::numeric_limits<std::uint32_t>::max()
            || *requestedWeapon > std::numeric_limits<std::uint32_t>::max())
        {
            error = "retail-payload-required-value-invalid";
            return std::nullopt;
        }
        if (*schema != "nikami-fnv-sidecar-retail/v1" || *sequence != snapshot.mSequenceId
            || *keySequence != snapshot.mSequenceId || *actorIndex != snapshot.mActorIndex
            || *actionIndex != snapshot.mActionIndex || *generation != snapshot.mGeneration)
        {
            error = "retail-payload-shared-header-identity-mismatch";
            return std::nullopt;
        }
        RetailAction result;
        result.mSchema = *schema;
        result.mSequenceId = *sequence;
        result.mActorIndex = static_cast<std::uint32_t>(*actorIndex);
        result.mActionIndex = static_cast<std::uint32_t>(*actionIndex);
        result.mGeneration = *generation;
        result.mRetailBaseForm = static_cast<std::uint32_t>(*baseForm);
        result.mActionId = *actionId;
        result.mRequestedFrames = static_cast<std::uint32_t>(*requestedFrames);
        result.mRequestedWeaponForm = static_cast<std::uint32_t>(*requestedWeapon);
        result.mWeaponDrawn = *weaponDrawn;

        const auto attachmentObject = jsonObject(*weapon, "attachment");
        const auto attachmentAvailable = attachmentObject
            ? jsonBoolean(*attachmentObject, "available") : std::optional<bool>{};
        if (result.mRequestedWeaponForm != 0 && !result.mWeaponDrawn
            && (!attachmentObject || !attachmentAvailable || !*attachmentAvailable))
        {
            error = "retail-payload-holster-attachment-missing";
            return std::nullopt;
        }
        if (attachmentObject && attachmentAvailable && *attachmentAvailable)
        {
            const auto sourceForm = jsonUnsigned(*attachmentObject, "sourceForm");
            const auto evaluatedSlot = jsonUnsigned(*attachmentObject, "evaluatedSlot");
            const auto evaluatedState = jsonUnsigned(*attachmentObject, "evaluatedState");
            const auto modelRootName = jsonString(*attachmentObject, "modelRootName");
            const auto frameName = jsonString(*attachmentObject, "frameName");
            const auto parentName = jsonString(*attachmentObject, "parentName");
            if (!sourceForm || !evaluatedSlot || !evaluatedState || !modelRootName || !frameName
                || !parentName || *sourceForm > std::numeric_limits<std::uint32_t>::max()
                || *evaluatedSlot >= 20 || *evaluatedState > std::numeric_limits<std::uint32_t>::max()
                || static_cast<std::uint32_t>(*sourceForm) != result.mRequestedWeaponForm
                || modelRootName->empty() || frameName->empty() || parentName->empty()
                || modelRootName->size() > 255 || frameName->size() > 127 || parentName->size() > 127)
            {
                error = "retail-payload-holster-attachment-identity-invalid";
                return std::nullopt;
            }

            const auto rotationBits = jsonUnsignedArray<9>(*attachmentObject, "rotationBits");
            const auto translationBits = jsonUnsignedArray<3>(*attachmentObject, "translationBits");
            const auto scaleBits = jsonUnsigned(*attachmentObject, "scaleBits");
            if (!rotationBits || !translationBits || !scaleBits
                || *scaleBits > std::numeric_limits<std::uint32_t>::max())
            {
                error = "retail-payload-holster-attachment-transform-invalid";
                return std::nullopt;
            }

            RetailAction::WeaponAttachment attachment;
            attachment.mSourceForm = static_cast<std::uint32_t>(*sourceForm);
            attachment.mEvaluatedSlot = static_cast<std::uint32_t>(*evaluatedSlot);
            attachment.mEvaluatedState = static_cast<std::uint32_t>(*evaluatedState);
            attachment.mModelRootName = *modelRootName;
            attachment.mFrameName = *frameName;
            attachment.mParentName = *parentName;
            attachment.mRotationBits = *rotationBits;
            attachment.mTranslationBits = *translationBits;
            attachment.mScaleBits = static_cast<std::uint32_t>(*scaleBits);
            bool transformValid = true;
            for (std::size_t index = 0; index < attachment.mRotation.size(); ++index)
            {
                attachment.mRotation[index] = floatFromBits(attachment.mRotationBits[index]);
                transformValid = transformValid && std::isfinite(attachment.mRotation[index])
                    && std::abs(attachment.mRotation[index]) <= 2.f;
            }
            for (std::size_t index = 0; index < attachment.mTranslation.size(); ++index)
            {
                attachment.mTranslation[index]
                    = floatFromBits(attachment.mTranslationBits[index]);
                transformValid = transformValid && std::isfinite(attachment.mTranslation[index])
                    && std::abs(attachment.mTranslation[index]) <= 1000000.f;
            }
            attachment.mScale = floatFromBits(attachment.mScaleBits);
            transformValid = transformValid && std::isfinite(attachment.mScale)
                && attachment.mScale > 0.f && attachment.mScale < 1000.f;
            if (!transformValid)
            {
                error = "retail-payload-holster-attachment-transform-invalid";
                return std::nullopt;
            }
            result.mWeaponAttachment = std::move(attachment);
        }
        return result;
    }

    std::uint64_t monotonicTickMilliseconds()
    {
#ifdef _WIN32
        return GetTickCount64();
#else
        return 0;
#endif
    }
}
