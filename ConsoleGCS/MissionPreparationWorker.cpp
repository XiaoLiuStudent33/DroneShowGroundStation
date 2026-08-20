#include "MissionPreparationWorker.h"

#include "RouteArtifactContract.h"
#include "Wpl110Core.h"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <utility>

namespace
{
    namespace fs = std::filesystem;
    namespace artifact = dronegcs::route_artifact;

    struct Handle
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };

    bool IsWithin(const fs::path& root, const fs::path& child)
    {
        auto rootIt = root.begin();
        auto childIt = child.begin();
        for (; rootIt != root.end(); ++rootIt, ++childIt)
        {
            if (childIt == child.end() ||
                _wcsicmp(rootIt->c_str(), childIt->c_str()) != 0)
                return false;
        }
        return true;
    }

    bool SameFile(const BY_HANDLE_FILE_INFORMATION& left,
        const BY_HANDLE_FILE_INFORMATION& right)
    {
        return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
            left.nFileIndexHigh == right.nFileIndexHigh &&
            left.nFileIndexLow == right.nFileIndexLow &&
            left.nFileSizeHigh == right.nFileSizeHigh &&
            left.nFileSizeLow == right.nFileSizeLow &&
            left.ftLastWriteTime.dwHighDateTime == right.ftLastWriteTime.dwHighDateTime &&
            left.ftLastWriteTime.dwLowDateTime == right.ftLastWriteTime.dwLowDateTime;
    }

    struct RowContext
    {
        std::vector<dronegcs::route_mission::MissionItemIntV1>* mission = nullptr;
        const std::atomic_bool* cancelled = nullptr;
        dronegcs::route_mission::Error conversionError;
        bool conversionFailed = false;
    };

    bool ConsumeRow(const dronegcs::wpl110::Row& row, void* opaque)
    {
        auto& context = *static_cast<RowContext*>(opaque);
        if ((context.cancelled && context.cancelled->load()) ||
            !context.mission)
            return false;
        dronegcs::route_mission::MissionItemIntV1 item;
        if (!dronegcs::route_mission::BuildMissionItemIntV1(
            row, item, context.conversionError))
        {
            context.conversionFailed = true;
            return false;
        }
        context.mission->push_back(item);
        return true;
    }

    MissionPreparationResult FailResult(
        MissionPreparationResult result,
        const MissionPreparationError error,
        const char* diagnostic)
    {
        result.error = error;
        result.safeDiagnostic = diagnostic ? diagnostic : "Mission preparation failed.";
        result.mission.clear();
        return result;
    }
}

MissionPreparationWorker::MissionPreparationWorker()
    : worker_(&MissionPreparationWorker::ThreadMain, this)
{
}

MissionPreparationWorker::~MissionPreparationWorker()
{
    Shutdown();
}

bool MissionPreparationWorker::Submit(MissionPreparationRequest request)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_.load() || pending_ || completed_ || inFlight_)
        return false;
    cancel_.store(false);
    pending_ = std::move(request);
    wake_.notify_one();
    return true;
}

bool MissionPreparationWorker::Poll(MissionPreparationResult& result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!completed_) return false;
    result = std::move(*completed_);
    completed_.reset();
    return true;
}

void MissionPreparationWorker::CancelActive()
{
    cancel_.store(true);
}

bool MissionPreparationWorker::IsBusy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.has_value() || inFlight_ || completed_.has_value();
}

void MissionPreparationWorker::Shutdown()
{
    if (stop_.exchange(true)) return;
    cancel_.store(true);
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void MissionPreparationWorker::ThreadMain()
{
    for (;;)
    {
        MissionPreparationRequest request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_.load() || pending_.has_value(); });
            if (stop_.load() && !pending_) return;
            request = std::move(*pending_);
            pending_.reset();
            inFlight_ = true;
        }
        MissionPreparationResult result = PrepareForTest(request, &cancel_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inFlight_ = false;
            completed_ = std::move(result);
        }
    }
}

MissionPreparationResult MissionPreparationWorker::PrepareForTest(
    const MissionPreparationRequest& request,
    const std::atomic_bool* cancelled)
{
    const auto startedAt = std::chrono::steady_clock::now();
    MissionPreparationResult result;
    result.uploadJobId = request.uploadJobId;
    result.generation = request.generation;
    result.target = request.target;
    auto finish = [&result, startedAt]() -> MissionPreparationResult
    {
        result.prepareDurationMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt).count());
        return std::move(result);
    };
    auto fail = [&result, &finish](const MissionPreparationError error,
        const char* diagnostic) -> MissionPreparationResult
    {
        result = FailResult(std::move(result), error, diagnostic);
        return finish();
    };

    if (cancelled && cancelled->load())
        return fail(MissionPreparationError::Cancelled,
            "Mission preparation was cancelled.");
    if (!request.artifactSnapshot || request.target.routeSlotIndex < 0 ||
        request.target.relativePath.empty() || request.target.fileByteSize < 0 ||
        request.target.missionItemCount == 0 ||
        !request.target.routeArtifactDigest.IsSet)
        return fail(MissionPreparationError::InvalidSource,
            "Mission preparation source identity is invalid.");

    std::error_code pathError;
    const fs::path root = fs::weakly_canonical(
        fs::u8path(request.artifactSnapshot->resolvedCurrentRoot), pathError);
    const fs::path relative = fs::u8path(request.target.relativePath);
    if (pathError || !relative.is_relative() || relative.empty())
        return fail(MissionPreparationError::InvalidSource,
            "Route path is invalid for the held Artifact.");
    const fs::path routePath = fs::weakly_canonical(root / relative, pathError);
    if (pathError || !IsWithin(root, routePath))
        return fail(MissionPreparationError::InvalidSource,
            "Route path escapes the held Artifact root.");

    Handle handle;
    handle.value = CreateFileW(routePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle.value == INVALID_HANDLE_VALUE)
        return fail(MissionPreparationError::RouteMissing,
            "Route source file is unavailable.");

    BY_HANDLE_FILE_INFORMATION before{};
    LARGE_INTEGER fileSize{};
    if (!GetFileInformationByHandle(handle.value, &before) ||
        !GetFileSizeEx(handle.value, &fileSize) || fileSize.QuadPart < 0 ||
        (before.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return fail(MissionPreparationError::IoFailure,
            "Route source metadata could not be validated.");
    if (fileSize.QuadPart != request.target.fileByteSize)
        return fail(MissionPreparationError::RouteSizeMismatch,
            "Route source file size differs from the held Artifact.");

    result.mission.reserve(request.target.missionItemCount);
    RowContext rowContext{ &result.mission, cancelled };
    dronegcs::wpl110::StreamingParser parser(
        static_cast<std::int32_t>(request.target.missionItemCount),
        &ConsumeRow, &rowContext);
    artifact::Sha256 sha;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    for (;;)
    {
        if (cancelled && cancelled->load())
            return fail(MissionPreparationError::Cancelled,
                "Mission preparation was cancelled.");
        DWORD read = 0;
        if (!ReadFile(handle.value, buffer.data(),
            static_cast<DWORD>(buffer.size()), &read, nullptr))
            return fail(MissionPreparationError::IoFailure,
                "Route source read failed.");
        if (read == 0) break;
        result.bytesRead += read;
        if (!sha.Update(buffer.data(), read) || !parser.Consume(buffer.data(), read))
        {
            if (cancelled && cancelled->load())
                return fail(MissionPreparationError::Cancelled,
                    "Mission preparation was cancelled.");
            return fail(rowContext.conversionFailed
                ? MissionPreparationError::MissionConversionFailed
                : MissionPreparationError::WaypointValidationFailed,
                rowContext.conversionFailed
                    ? "Route row could not be converted to a Mission item."
                    : "Route WPL grammar or semantics are invalid.");
        }
    }
    if (!parser.Finish() || result.mission.size() != request.target.missionItemCount)
        return fail(MissionPreparationError::WaypointValidationFailed,
            "Route WPL row count differs from the held Artifact.");

    artifact::Digest256 digest;
    BY_HANDLE_FILE_INFORMATION after{};
    if (!sha.Finalize(digest) ||
        !GetFileInformationByHandle(handle.value, &after) ||
        !SameFile(before, after))
        return fail(MissionPreparationError::IoFailure,
            "Route source changed during Mission preparation.");
    if (result.bytesRead != static_cast<std::uint64_t>(request.target.fileByteSize))
        return fail(MissionPreparationError::RouteSizeMismatch,
            "Route bytes read differ from the held Artifact.");
    if (digest != request.target.routeArtifactDigest)
        return fail(MissionPreparationError::RouteDigestMismatch,
            "Route source digest differs from the held Artifact.");

    result.error = MissionPreparationError::None;
    result.safeDiagnostic.clear();
    return finish();
}

