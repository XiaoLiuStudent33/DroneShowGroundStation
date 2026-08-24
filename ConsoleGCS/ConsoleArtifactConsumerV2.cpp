#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ConsoleArtifactConsumerV2.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <set>
#include <system_error>

namespace
{
    namespace fs = std::filesystem;
    namespace artifact = dronegcs::route_artifact;
    namespace artifact_v2 = dronegcs::route_artifact::v2;

    constexpr std::uint64_t MaxManifestBytes = 64ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t MaxReadyBytes = 512;
    constexpr DWORD StreamingBufferBytes = 64 * 1024;

    void SetError(
        ConsoleArtifactV2Diagnostic& diagnostic,
        const ConsoleArtifactV2ErrorCode code,
        const char* detail,
        const std::int32_t routeSlot = -1)
    {
        diagnostic = {};
        diagnostic.errorCode = code;
        diagnostic.routeSlotIndex = routeSlot;
        diagnostic.safeDetail = detail ? detail : "";
    }

    class WinHandle final
    {
    public:
        WinHandle() = default;
        explicit WinHandle(HANDLE value) : value_(value) {}
        ~WinHandle() { Reset(); }
        WinHandle(const WinHandle&) = delete;
        WinHandle& operator=(const WinHandle&) = delete;
        WinHandle(WinHandle&& other) noexcept : value_(other.value_)
        {
            other.value_ = INVALID_HANDLE_VALUE;
        }
        WinHandle& operator=(WinHandle&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                value_ = other.value_;
                other.value_ = INVALID_HANDLE_VALUE;
            }
            return *this;
        }
        bool Valid() const
        {
            return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
        }
        HANDLE Get() const { return value_; }
        void Reset()
        {
            if (Valid()) CloseHandle(value_);
            value_ = INVALID_HANDLE_VALUE;
        }
    private:
        HANDLE value_ = INVALID_HANDLE_VALUE;
    };

    bool IsContainedFinalPath(std::wstring root, std::wstring candidate)
    {
        auto Lower = [](std::wstring& value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                [](const wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
        };
        Lower(root);
        Lower(candidate);
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
            root.pop_back();
        if (candidate == root) return true;
        root.push_back(L'\\');
        return candidate.size() > root.size()
            && candidate.compare(0, root.size(), root) == 0;
    }

    bool FinalPath(HANDLE handle, std::wstring& out)
    {
        const DWORD required = GetFinalPathNameByHandleW(
            handle, nullptr, 0, FILE_NAME_NORMALIZED);
        if (required == 0) return false;
        std::wstring buffer(required, L'\0');
        const DWORD written = GetFinalPathNameByHandleW(
            handle, buffer.data(), required, FILE_NAME_NORMALIZED);
        if (written == 0 || written >= required) return false;
        buffer.resize(written);
        out = std::move(buffer);
        return true;
    }

    bool ValidateEntryAttributes(
        const fs::path& path,
        const bool expectDirectory,
        ConsoleArtifactV2Diagnostic& diagnostic)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            SetError(diagnostic,
                expectDirectory
                    ? ConsoleArtifactV2ErrorCode::CurrentMissing
                    : ConsoleArtifactV2ErrorCode::RouteMissing,
                expectDirectory
                    ? "Required Artifact directory is missing."
                    : "Required Artifact file is missing.");
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::ReparsePointDetected,
                "Artifact path contains a reparse point.");
            return false;
        }
        const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDirectory != expectDirectory)
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::IoFailure,
                "Artifact entry has the wrong filesystem type.");
            return false;
        }
        return true;
    }

    bool CaptureDirectoryIdentity(
        const fs::path& path,
        RouteArtifactV2CurrentDirectoryIdentity& out,
        ConsoleArtifactV2Diagnostic& diagnostic)
    {
        WinHandle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION information{};
        if (!handle.Valid() || !GetFileInformationByHandle(handle.Get(), &information))
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::IoFailure,
                "Current identity could not be captured.");
            return false;
        }
        out.volumeSerialNumber = information.dwVolumeSerialNumber;
        out.fileIndexHigh = information.nFileIndexHigh;
        out.fileIndexLow = information.nFileIndexLow;
        return true;
    }

    bool ValidateWorkspace(
        const std::string& configuredRoot,
        fs::path& root,
        fs::path& current,
        std::string& canonicalRoot,
        std::wstring& currentFinal,
        std::string& resolvedCurrent,
        ConsoleArtifactV2Diagnostic& diagnostic)
    {
        if (configuredRoot.empty())
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::InvalidWorkspaceRoot,
                "Artifact Workspace root is not configured.");
            return false;
        }
        root = fs::u8path(configuredRoot).lexically_normal();
        if (!root.is_absolute() || root == root.root_path())
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::InvalidWorkspaceRoot,
                "Artifact Workspace root must be an absolute non-volume path.");
            return false;
        }
        canonicalRoot = root.u8string();
        current = root / L"Current";
        if (!ValidateEntryAttributes(root, true, diagnostic)
            || !ValidateEntryAttributes(current, true, diagnostic))
            return false;

        WinHandle rootHandle(CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        WinHandle currentHandle(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        std::wstring rootFinal;
        if (!rootHandle.Valid() || !currentHandle.Valid()
            || !FinalPath(rootHandle.Get(), rootFinal)
            || !FinalPath(currentHandle.Get(), currentFinal))
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::IoFailure,
                "Artifact Workspace could not be opened safely.");
            return false;
        }
        if (!IsContainedFinalPath(rootFinal, currentFinal))
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::PathEscapesRoot,
                "Current does not resolve beneath the configured Workspace root.");
            return false;
        }
        resolvedCurrent = fs::path(currentFinal).u8string();
        return true;
    }

    bool ValidatePathChain(
        const fs::path& current,
        const fs::path& relative,
        const bool finalDirectory,
        ConsoleArtifactV2Diagnostic& diagnostic)
    {
        fs::path cursor = current;
        std::size_t index = 0;
        const std::size_t count = static_cast<std::size_t>(
            std::distance(relative.begin(), relative.end()));
        for (const fs::path& part : relative)
        {
            cursor /= part;
            ++index;
            const bool isDirectory = index < count || finalDirectory;
            if (!ValidateEntryAttributes(cursor, isDirectory, diagnostic)) return false;
        }
        return true;
    }

    bool OpenSafeFile(
        const fs::path& current,
        const std::wstring& currentFinal,
        const fs::path& relative,
        WinHandle& out,
        ConsoleArtifactV2Diagnostic& diagnostic)
    {
        if (!ValidatePathChain(current, relative, false, diagnostic)) return false;
        out = WinHandle(CreateFileW((current / relative).c_str(),
            GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!out.Valid())
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::IoFailure,
                "Artifact file could not be opened.");
            return false;
        }
        FILE_ATTRIBUTE_TAG_INFO tag{};
        std::wstring fileFinal;
        if (!GetFileInformationByHandleEx(
                out.Get(), FileAttributeTagInfo, &tag, sizeof(tag))
            || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::ReparsePointDetected,
                "Opened Artifact file is a reparse point.");
            return false;
        }
        if (!FinalPath(out.Get(), fileFinal)
            || !IsContainedFinalPath(currentFinal, fileFinal))
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::PathEscapesRoot,
                "Artifact file resolves outside Current.");
            return false;
        }
        return true;
    }

    bool SameFileInformation(
        const BY_HANDLE_FILE_INFORMATION& left,
        const BY_HANDLE_FILE_INFORMATION& right)
    {
        return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber
            && left.nFileIndexHigh == right.nFileIndexHigh
            && left.nFileIndexLow == right.nFileIndexLow
            && left.nFileSizeHigh == right.nFileSizeHigh
            && left.nFileSizeLow == right.nFileSizeLow
            && left.ftLastWriteTime.dwHighDateTime == right.ftLastWriteTime.dwHighDateTime
            && left.ftLastWriteTime.dwLowDateTime == right.ftLastWriteTime.dwLowDateTime;
    }

    RouteArtifactV2FileIdentity ToRouteFileIdentity(
        const BY_HANDLE_FILE_INFORMATION& information)
    {
        RouteArtifactV2FileIdentity identity;
        identity.volumeSerialNumber = information.dwVolumeSerialNumber;
        identity.fileIndexHigh = information.nFileIndexHigh;
        identity.fileIndexLow = information.nFileIndexLow;
        identity.fileSizeHigh = information.nFileSizeHigh;
        identity.fileSizeLow = information.nFileSizeLow;
        identity.lastWriteTimeHigh = information.ftLastWriteTime.dwHighDateTime;
        identity.lastWriteTimeLow = information.ftLastWriteTime.dwLowDateTime;
        return identity;
    }

    std::uint64_t MillisecondsSince(
        const std::chrono::steady_clock::time_point started)
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
    }

    bool ReadSmallFile(
        const fs::path& current,
        const std::wstring& currentFinal,
        const fs::path& relative,
        const std::uint64_t maximumBytes,
        std::string& out,
        ConsoleArtifactV2Diagnostic& diagnostic)
    {
        out.clear();
        WinHandle handle;
        if (!OpenSafeFile(current, currentFinal, relative, handle, diagnostic))
            return false;
        BY_HANDLE_FILE_INFORMATION before{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(handle.Get(), &before)
            || !GetFileSizeEx(handle.Get(), &size) || size.QuadPart < 0
            || static_cast<std::uint64_t>(size.QuadPart) > maximumBytes)
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::IoFailure,
                "Artifact metadata file size is invalid.");
            return false;
        }
        out.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < out.size())
        {
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
                out.size() - offset, StreamingBufferBytes));
            DWORD read = 0;
            if (!ReadFile(handle.Get(), out.data() + offset, request, &read, nullptr)
                || read == 0)
            {
                SetError(diagnostic, ConsoleArtifactV2ErrorCode::IoFailure,
                    "Artifact metadata file read failed.");
                return false;
            }
            offset += read;
        }
        BY_HANDLE_FILE_INFORMATION after{};
        if (!GetFileInformationByHandle(handle.Get(), &after)
            || !SameFileInformation(before, after))
        {
            SetError(diagnostic,
                ConsoleArtifactV2ErrorCode::FileChangedDuringValidation,
                "Artifact metadata changed during validation.");
            return false;
        }
        return true;
    }

    bool ValidateExpected(
        const artifact_v2::Manifest& manifest,
        const ExpectedRouteArtifactV2Identity& expected,
        ConsoleArtifactV2Diagnostic& diagnostic)
    {
        if (manifest.ArtifactSetId != expected.artifactSetId
            || manifest.ArtifactSetDigest != expected.artifactSetDigest)
        {
            SetError(diagnostic,
                ConsoleArtifactV2ErrorCode::ArtifactIdentityMismatch,
                "Current Artifact does not match the expected v2 identity.");
            return false;
        }
        return true;
    }

    bool ValidateAllowlist(
        const fs::path& current,
        const artifact_v2::Manifest& manifest,
        ConsoleArtifactV2Diagnostic& diagnostic)
    {
        std::set<std::string> expectedFiles{ "manifest.json", "READY" };
        std::set<std::string> expectedDirectories;
        for (const auto& route : manifest.Routes)
        {
            expectedFiles.insert(route.RelativePath);
            fs::path parent = fs::u8path(route.RelativePath).parent_path();
            while (!parent.empty() && parent != L".")
            {
                expectedDirectories.insert(parent.generic_u8string());
                parent = parent.parent_path();
            }
        }

        std::set<std::string> actualFiles;
        std::set<std::string> actualDirectories;
        std::error_code error;
        fs::recursive_directory_iterator iterator(
            current, fs::directory_options::none, error);
        const fs::recursive_directory_iterator end;
        while (!error && iterator != end)
        {
            const fs::directory_entry entry = *iterator;
            const DWORD attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                SetError(diagnostic, ConsoleArtifactV2ErrorCode::IoFailure,
                    "Current enumeration encountered an unreadable entry.");
                return false;
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                iterator.disable_recursion_pending();
                SetError(diagnostic,
                    ConsoleArtifactV2ErrorCode::ReparsePointDetected,
                    "Current contains a reparse point.");
                return false;
            }
            const std::string relative =
                entry.path().lexically_relative(current).generic_u8string();
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                actualDirectories.insert(relative);
            else
                actualFiles.insert(relative);
            iterator.increment(error);
        }
        if (error || actualFiles != expectedFiles
            || actualDirectories != expectedDirectories)
        {
            SetError(diagnostic,
                ConsoleArtifactV2ErrorCode::UnexpectedCurrentEntry,
                "Current contains missing or unexpected files/directories.");
            return false;
        }
        return true;
    }

    struct LoadedMetadata
    {
        fs::path root;
        fs::path current;
        std::string canonicalRoot;
        std::wstring currentFinal;
        std::string resolvedCurrent;
        RouteArtifactV2CurrentDirectoryIdentity currentIdentity;
        artifact_v2::Manifest manifest;
    };

    bool LoadMetadata(
        const std::string& workspaceRoot,
        const ExpectedRouteArtifactV2Identity& expected,
        LoadedMetadata& out,
        ConsoleArtifactV2Diagnostic& diagnostic,
        const bool validateAllowlist)
    {
        if (!ValidateWorkspace(workspaceRoot, out.root, out.current,
                out.canonicalRoot, out.currentFinal, out.resolvedCurrent,
                diagnostic)
            || !CaptureDirectoryIdentity(
                out.current, out.currentIdentity, diagnostic))
            return false;

        std::string readyBytes;
        std::string manifestBytes;
        if (!ReadSmallFile(out.current, out.currentFinal, L"READY",
                MaxReadyBytes, readyBytes, diagnostic))
        {
            diagnostic.errorCode = ConsoleArtifactV2ErrorCode::ReadyMissingOrInvalid;
            return false;
        }
        if (!ReadSmallFile(out.current, out.currentFinal, L"manifest.json",
                MaxManifestBytes, manifestBytes, diagnostic))
        {
            diagnostic.errorCode = ConsoleArtifactV2ErrorCode::ManifestMissingOrInvalid;
            return false;
        }

        artifact::Error contractError;
        std::string readySetId;
        artifact::Digest256 readyDigest;
        if (!artifact::ParseReadyMarker(
                readyBytes, readySetId, readyDigest, contractError))
        {
            SetError(diagnostic,
                ConsoleArtifactV2ErrorCode::ReadyMissingOrInvalid,
                "READY bytes are malformed or noncanonical.");
            return false;
        }

        // A valid canonical v1 manifest always begins with this exact token.
        // It is classified for diagnostics only and is never parsed or accepted.
        if (manifestBytes.rfind("{\"artifactSchemaVersion\":1,", 0) == 0)
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::LegacyUnsupported,
                "Legacy Route Artifact schema v1 is unsupported by the pure v2 consumer.");
            return false;
        }
        if (!artifact_v2::ParseCanonicalManifest(
                manifestBytes, out.manifest, contractError))
        {
            SetError(diagnostic,
                ConsoleArtifactV2ErrorCode::ManifestMissingOrInvalid,
                "Manifest bytes are not a valid canonical Route Artifact schema v2 manifest.");
            return false;
        }
        if (readySetId != out.manifest.ArtifactSetId
            || readyDigest != out.manifest.ArtifactSetDigest
            || !ValidateExpected(out.manifest, expected, diagnostic))
        {
            if (diagnostic.errorCode == ConsoleArtifactV2ErrorCode::None)
            {
                SetError(diagnostic,
                    ConsoleArtifactV2ErrorCode::ArtifactIdentityMismatch,
                    "READY, Manifest, and expected v2 identity differ.");
            }
            return false;
        }
        return !validateAllowlist
            || ValidateAllowlist(out.current, out.manifest, diagnostic);
    }

    struct RowCounter
    {
        std::uint64_t rows = 0;
    };

    bool CountRow(const dronegcs::wpl110::Row&, void* context)
    {
        ++static_cast<RowCounter*>(context)->rows;
        return true;
    }

    void SampleProcessMetrics(ConsoleArtifactV2ValidationMetrics& metrics)
    {
        DWORD handles = 0;
        if (GetProcessHandleCount(GetCurrentProcess(), &handles))
        {
            metrics.peakProcessHandles = std::max(
                metrics.peakProcessHandles,
                static_cast<std::uint32_t>(handles));
        }
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        {
            metrics.peakWorkingSetBytes = std::max(
                metrics.peakWorkingSetBytes,
                static_cast<std::uint64_t>(counters.PeakWorkingSetSize));
        }
    }

    enum class AbortReason { None, Cancelled, Deadline };

    AbortReason CheckAbort(
        const std::function<bool()>& cancelled,
        const std::chrono::steady_clock::time_point deadline)
    {
        if (cancelled()) return AbortReason::Cancelled;
        if (std::chrono::steady_clock::now() >= deadline)
            return AbortReason::Deadline;
        return AbortReason::None;
    }

    bool SetAbortError(
        const AbortReason reason,
        ConsoleArtifactV2Diagnostic& diagnostic,
        const std::int32_t routeSlot = -1)
    {
        if (reason == AbortReason::None) return false;
        if (reason == AbortReason::Cancelled)
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::Cancelled,
                "Artifact validation was cancelled.", routeSlot);
        }
        else
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::DeadlineExceeded,
                "Artifact validation deadline was exceeded.", routeSlot);
        }
        return true;
    }

    bool ValidateRoute(
        const LoadedMetadata& metadata,
        const artifact::RouteRecord& route,
        RouteArtifactV2FileIdentity& validatedIdentity,
        ConsoleArtifactV2ValidationMetrics& metrics,
        ConsoleArtifactV2Diagnostic& diagnostic,
        const std::function<bool()>& cancelled,
        const std::chrono::steady_clock::time_point deadline)
    {
        if (SetAbortError(CheckAbort(cancelled, deadline), diagnostic,
                route.RouteSlotIndex))
            return false;

        WinHandle handle;
        if (!OpenSafeFile(metadata.current, metadata.currentFinal,
                fs::u8path(route.RelativePath), handle, diagnostic))
        {
            diagnostic.routeSlotIndex = route.RouteSlotIndex;
            return false;
        }
        ++metrics.physicalRouteOpenCount;
        SampleProcessMetrics(metrics);

        BY_HANDLE_FILE_INFORMATION before{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(handle.Get(), &before)
            || !GetFileSizeEx(handle.Get(), &size) || size.QuadPart < 0)
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::IoFailure,
                "Route file metadata could not be read.", route.RouteSlotIndex);
            return false;
        }
        if (size.QuadPart != route.FileByteSize)
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::RouteSizeMismatch,
                "Route file size differs from Manifest.", route.RouteSlotIndex);
            return false;
        }

        artifact::Sha256 sha;
        RowCounter rowCounter;
        dronegcs::wpl110::StreamingParser parser(
            metadata.manifest.FrameCount, &CountRow, &rowCounter);
        std::vector<std::uint8_t> buffer(StreamingBufferBytes);
        std::uint64_t total = 0;
        for (;;)
        {
            if (SetAbortError(CheckAbort(cancelled, deadline), diagnostic,
                    route.RouteSlotIndex))
                return false;
            DWORD read = 0;
            if (!ReadFile(handle.Get(), buffer.data(),
                    static_cast<DWORD>(buffer.size()), &read, nullptr))
            {
                SetError(diagnostic, ConsoleArtifactV2ErrorCode::IoFailure,
                    "Route streaming read failed.", route.RouteSlotIndex);
                return false;
            }
            if (read == 0) break;
            if (!sha.Update(buffer.data(), read)
                || !parser.Consume(buffer.data(), read))
            {
                const auto& parserError = parser.GetError();
                SetError(diagnostic,
                    ConsoleArtifactV2ErrorCode::WaypointSemanticInvalid,
                    "Route WPL grammar or semantics are invalid.",
                    route.RouteSlotIndex);
                diagnostic.rowIndex = parserError.RowIndex;
                diagnostic.fieldIndex = parserError.FieldIndex;
                diagnostic.waypointErrorCode = parserError.Code;
                return false;
            }
            total += read;
        }
        if (!parser.Finish())
        {
            const auto& parserError = parser.GetError();
            SetError(diagnostic,
                ConsoleArtifactV2ErrorCode::WaypointSemanticInvalid,
                "Route WPL finalization failed.", route.RouteSlotIndex);
            diagnostic.rowIndex = parserError.RowIndex;
            diagnostic.fieldIndex = parserError.FieldIndex;
            diagnostic.waypointErrorCode = parserError.Code;
            return false;
        }

        artifact::Digest256 digest;
        BY_HANDLE_FILE_INFORMATION after{};
        if (!sha.Finalize(digest)
            || !GetFileInformationByHandle(handle.Get(), &after)
            || !SameFileInformation(before, after))
        {
            SetError(diagnostic,
                ConsoleArtifactV2ErrorCode::FileChangedDuringValidation,
                "Route file changed during validation.", route.RouteSlotIndex);
            return false;
        }
        if (total != static_cast<std::uint64_t>(route.FileByteSize))
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::RouteSizeMismatch,
                "Route bytes read differ from Manifest size.", route.RouteSlotIndex);
            return false;
        }
        if (digest != route.RouteArtifactDigest)
        {
            SetError(diagnostic, ConsoleArtifactV2ErrorCode::RouteDigestMismatch,
                "Route SHA-256 differs from Manifest.", route.RouteSlotIndex);
            return false;
        }
        ++metrics.routeFilesRead;
        metrics.rowsParsed += rowCounter.rows;
        metrics.bytesRead += total;
        validatedIdentity = ToRouteFileIdentity(after);
        return true;
    }

    ConsoleArtifactV2ValidationResult ValidateCurrentImpl(
        const std::string& workspaceRoot,
        const ExpectedRouteArtifactV2Identity& expected,
        const std::function<bool()>& cancelled,
        const std::uint64_t generation,
        const std::chrono::steady_clock::time_point deadline,
        const ConsoleArtifactConsumerV2::RouteValidatedHook& routeValidatedHook,
        const ConsoleArtifactV2ValidationScope& validationScope,
        const std::shared_ptr<const AcceptedRouteArtifactV2>& cacheCandidate)
    {
        ConsoleArtifactV2ValidationResult result;
        result.generation = generation;
        result.metrics.fullArtifactValidation =
            validationScope.fullArtifactValidation;
        const auto started = std::chrono::steady_clock::now();
        const auto Finish = [&result, started]()
        {
            result.metrics.elapsedSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
        };

        if (SetAbortError(CheckAbort(cancelled, deadline), result.diagnostic))
        {
            Finish();
            return result;
        }
        LoadedMetadata metadata;
        const auto metadataStarted = std::chrono::steady_clock::now();
        if (!LoadMetadata(workspaceRoot, expected, metadata,
                result.diagnostic, result.metrics.fullArtifactValidation))
        {
            result.metrics.metadataDurationMs = MillisecondsSince(metadataStarted);
            Finish();
            return result;
        }
        result.metrics.metadataDurationMs = MillisecondsSince(metadataStarted);
        result.metrics.manifestRouteCount = metadata.manifest.Routes.size();

        std::vector<std::size_t> selectedRouteIndices;
        if (validationScope.fullArtifactValidation)
        {
            selectedRouteIndices.reserve(metadata.manifest.Routes.size());
            for (std::size_t index = 0; index < metadata.manifest.Routes.size(); ++index)
                selectedRouteIndices.push_back(index);
        }
        else
        {
            if (validationScope.matchedRoutes.empty())
            {
                SetError(result.diagnostic,
                    ConsoleArtifactV2ErrorCode::ArtifactIdentityMismatch,
                    "Selected Artifact validation requires at least one matched Route.");
                Finish();
                return result;
            }
            std::set<std::string> droneIds;
            std::set<std::int32_t> routeSlots;
            selectedRouteIndices.reserve(validationScope.matchedRoutes.size());
            for (const auto& binding : validationScope.matchedRoutes)
            {
                if (binding.droneId.empty() || binding.routeSlotIndex < 0
                    || !droneIds.insert(binding.droneId).second
                    || !routeSlots.insert(binding.routeSlotIndex).second)
                {
                    SetError(result.diagnostic,
                        ConsoleArtifactV2ErrorCode::ArtifactIdentityMismatch,
                        "Artifact validation scope contains an invalid or duplicate binding.");
                    Finish();
                    return result;
                }
                const auto route = std::lower_bound(
                    metadata.manifest.Routes.begin(), metadata.manifest.Routes.end(),
                    binding.routeSlotIndex,
                    [](const artifact::RouteRecord& record, const std::int32_t slot)
                    { return record.RouteSlotIndex < slot; });
                if (route == metadata.manifest.Routes.end()
                    || route->RouteSlotIndex != binding.routeSlotIndex)
                {
                    SetError(result.diagnostic,
                        ConsoleArtifactV2ErrorCode::RouteMissing,
                        "A selected RouteSlot has no Manifest record.",
                        binding.routeSlotIndex);
                    Finish();
                    return result;
                }
                selectedRouteIndices.push_back(static_cast<std::size_t>(
                    route - metadata.manifest.Routes.begin()));
            }
        }
        result.metrics.selectedRouteCount = selectedRouteIndices.size();

        if (!validationScope.fullArtifactValidation && cacheCandidate
            && cacheCandidate->artifactSetId == expected.artifactSetId
            && cacheCandidate->artifactSetDigest == expected.artifactSetDigest
            && cacheCandidate->validationScope == validationScope
            && cacheCandidate->currentDirectoryIdentity == metadata.currentIdentity
            && cacheCandidate->routes.size() == metadata.manifest.Routes.size())
        {
            const auto cacheStarted = std::chrono::steady_clock::now();
            bool cacheValid = true;
            for (const std::size_t index : selectedRouteIndices)
            {
                const auto& manifestRoute = metadata.manifest.Routes[index];
                const auto& cachedRoute = cacheCandidate->routes[index];
                WinHandle handle;
                BY_HANDLE_FILE_INFORMATION information{};
                if (!cachedRoute.validationConfirmed
                    || cachedRoute.routeSlotIndex != manifestRoute.RouteSlotIndex
                    || cachedRoute.relativePath != manifestRoute.RelativePath
                    || cachedRoute.fileByteSize != manifestRoute.FileByteSize
                    || cachedRoute.routeArtifactDigest != manifestRoute.RouteArtifactDigest
                    || !OpenSafeFile(metadata.current, metadata.currentFinal,
                        fs::u8path(manifestRoute.RelativePath), handle,
                        result.diagnostic)
                    || !GetFileInformationByHandle(handle.Get(), &information)
                    || !(cachedRoute.validatedFileIdentity ==
                        ToRouteFileIdentity(information)))
                {
                    cacheValid = false;
                    result.diagnostic = {};
                    break;
                }
                ++result.metrics.physicalRouteOpenCount;
            }
            result.metrics.cacheIdentityCheckDurationMs =
                MillisecondsSince(cacheStarted);
            if (cacheValid)
            {
                LoadedMetadata recheck;
                const auto recheckStarted = std::chrono::steady_clock::now();
                cacheValid = LoadMetadata(workspaceRoot, expected, recheck,
                        result.diagnostic, false)
                    && metadata.currentIdentity == recheck.currentIdentity;
                result.metrics.metadataRecheckDurationMs =
                    MillisecondsSince(recheckStarted);
            }
            if (cacheValid)
            {
                auto snapshot = std::make_shared<AcceptedRouteArtifactV2>(
                    *cacheCandidate);
                snapshot->validatedAtUtcMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                snapshot->validationGeneration = generation;
                result.metrics.cacheHit = true;
                result.success = true;
                result.snapshot = std::move(snapshot);
                SampleProcessMetrics(result.metrics);
                Finish();
                return result;
            }
            result.diagnostic = {};
        }

        std::vector<RouteArtifactV2FileIdentity> validatedIdentities(
            metadata.manifest.Routes.size());
        std::vector<bool> validationConfirmed(metadata.manifest.Routes.size(), false);
        const auto routeValidationStarted = std::chrono::steady_clock::now();
        for (const std::size_t index : selectedRouteIndices)
        {
            const auto& route = metadata.manifest.Routes[index];
            if (!ValidateRoute(metadata, route, validatedIdentities[index],
                    result.metrics, result.diagnostic, cancelled, deadline))
            {
                result.metrics.routeValidationDurationMs =
                    MillisecondsSince(routeValidationStarted);
                Finish();
                return result;
            }
            validationConfirmed[index] = true;
            if (routeValidatedHook) routeValidatedHook(route.RouteSlotIndex);
        }
        result.metrics.routeValidationDurationMs =
            MillisecondsSince(routeValidationStarted);

        LoadedMetadata recheck;
        const auto recheckStarted = std::chrono::steady_clock::now();
        if (!LoadMetadata(workspaceRoot, expected, recheck,
                result.diagnostic, result.metrics.fullArtifactValidation))
        {
            result.metrics.metadataRecheckDurationMs =
                MillisecondsSince(recheckStarted);
            Finish();
            return result;
        }
        result.metrics.metadataRecheckDurationMs = MillisecondsSince(recheckStarted);
        if (!(metadata.currentIdentity == recheck.currentIdentity))
        {
            SetError(result.diagnostic,
                ConsoleArtifactV2ErrorCode::FileChangedDuringValidation,
                "Current was replaced during Artifact validation.");
            Finish();
            return result;
        }

        auto snapshot = std::make_shared<AcceptedRouteArtifactV2>();
        snapshot->artifactSchemaVersion = metadata.manifest.SchemaVersion;
        snapshot->artifactSetId = metadata.manifest.ArtifactSetId;
        snapshot->artifactSetDigest = metadata.manifest.ArtifactSetDigest;
        snapshot->createdAtUtcMs = metadata.manifest.CreatedAtUtc;
        snapshot->stableSource.sourceDatasetDigest =
            metadata.manifest.Source.SourceDatasetDigest;
        snapshot->stableSource.normalizedShowOriginLatitudeE7 =
            metadata.manifest.ShowOriginLatitudeE7;
        snapshot->stableSource.normalizedShowOriginLongitudeE7 =
            metadata.manifest.ShowOriginLongitudeE7;
        snapshot->stableSource.routeSlotCount = metadata.manifest.RouteSlotCount;
        snapshot->sourceByteSize = metadata.manifest.Source.SourceByteSize;
        snapshot->sourceDisplayFileName =
            metadata.manifest.Source.SourceDisplayFileName;
        snapshot->frameCount = metadata.manifest.FrameCount;
        snapshot->hasRange = metadata.manifest.HasRange;
        snapshot->rangeStart = metadata.manifest.RangeStart;
        snapshot->rangeEnd = metadata.manifest.RangeEnd;
        snapshot->waypointGeneratorVersion =
            metadata.manifest.WaypointGeneratorVersion;
        snapshot->generatorInputDigest = metadata.manifest.GeneratorInputDigest;
        snapshot->routes.reserve(metadata.manifest.Routes.size());
        for (std::size_t index = 0; index < metadata.manifest.Routes.size(); ++index)
        {
            const auto& route = metadata.manifest.Routes[index];
            snapshot->routes.push_back({ route.RouteSlotIndex, route.RelativePath,
                route.FileByteSize, route.RouteArtifactDigest,
                validationConfirmed[index], validatedIdentities[index] });
        }
        snapshot->canonicalWorkspaceRoot = metadata.canonicalRoot;
        snapshot->resolvedCurrentRoot = metadata.resolvedCurrent;
        snapshot->currentDirectoryIdentity = metadata.currentIdentity;
        snapshot->validatedAtUtcMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        snapshot->validationGeneration = generation;
        snapshot->validationScope = validationScope;

        result.success = true;
        result.snapshot = std::move(snapshot);
        SampleProcessMetrics(result.metrics);
        Finish();
        return result;
    }
}

ConsoleArtifactConsumerV2::ConsoleArtifactConsumerV2(
    std::string configuredWorkspaceRoot)
    : workspaceRoot_(fs::u8path(configuredWorkspaceRoot).lexically_normal().u8string())
    , worker_(&ConsoleArtifactConsumerV2::WorkerMain, this)
{
}

ConsoleArtifactConsumerV2::~ConsoleArtifactConsumerV2()
{
    stop_.store(true);
    cancellationVersion_.fetch_add(1);
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::uint64_t ConsoleArtifactConsumerV2::RequestValidation(
    ExpectedRouteArtifactV2Identity expectedIdentity,
    const std::chrono::milliseconds deadline)
{
    return RequestValidation(
        std::move(expectedIdentity), ConsoleArtifactV2ValidationScope{}, deadline);
}

std::uint64_t ConsoleArtifactConsumerV2::RequestValidation(
    ExpectedRouteArtifactV2Identity expectedIdentity,
    ConsoleArtifactV2ValidationScope validationScope,
    const std::chrono::milliseconds deadline)
{
    std::sort(validationScope.matchedRoutes.begin(),
        validationScope.matchedRoutes.end(),
        [](const ConsoleArtifactV2MatchedRoute& left,
            const ConsoleArtifactV2MatchedRoute& right)
        {
            if (left.routeSlotIndex != right.routeSlotIndex)
                return left.routeSlotIndex < right.routeSlotIndex;
            return left.droneId < right.droneId;
        });
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t version = cancellationVersion_.fetch_add(1) + 1;
    const std::uint64_t generation = ++nextGeneration_;
    pending_ = Request{ generation, version, std::move(expectedIdentity),
        std::move(validationScope),
        deadline.count() > 0
            ? std::chrono::steady_clock::now() + deadline
            : std::chrono::steady_clock::now() };
    completed_.reset();
    wake_.notify_one();
    return generation;
}

bool ConsoleArtifactConsumerV2::PollCompleted(
    ConsoleArtifactV2ValidationResult& result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!completed_) return false;
    result = std::move(*completed_);
    completed_.reset();
    return true;
}

void ConsoleArtifactConsumerV2::CancelValidation()
{
    cancellationVersion_.fetch_add(1);
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.reset();
}

std::shared_ptr<const AcceptedRouteArtifactV2>
ConsoleArtifactConsumerV2::GetAcceptedSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return accepted_;
}

std::optional<AcceptedRouteArtifactV2RouteRecord>
ConsoleArtifactConsumerV2::FindRouteRecord(const std::int32_t routeSlotIndex) const
{
    const auto snapshot = GetAcceptedSnapshot();
    if (!snapshot || routeSlotIndex < 0
        || static_cast<std::size_t>(routeSlotIndex) >= snapshot->routes.size())
        return std::nullopt;
    const auto& route = snapshot->routes[static_cast<std::size_t>(routeSlotIndex)];
    if (route.routeSlotIndex != routeSlotIndex) return std::nullopt;
    return route;
}

bool ConsoleArtifactConsumerV2::IsCurrentIdentityStill(
    const ExpectedRouteArtifactV2Identity& expectedIdentity,
    ConsoleArtifactV2Diagnostic& diagnostic) const
{
    const auto accepted = GetAcceptedSnapshot();
    if (!accepted)
    {
        SetError(diagnostic,
            ConsoleArtifactV2ErrorCode::CurrentMissing,
            "No accepted v2 Artifact snapshot is available.");
        return false;
    }
    return IsCurrentIdentityStill(
        expectedIdentity, accepted->currentDirectoryIdentity, diagnostic);
}

bool ConsoleArtifactConsumerV2::IsCurrentIdentityStill(
    const ExpectedRouteArtifactV2Identity& expectedIdentity,
    const RouteArtifactV2CurrentDirectoryIdentity& expectedCurrentDirectoryIdentity,
    ConsoleArtifactV2Diagnostic& diagnostic) const
{
    LoadedMetadata metadata;
    if (!LoadMetadata(workspaceRoot_, expectedIdentity, metadata, diagnostic, false))
        return false;
    if (!(expectedCurrentDirectoryIdentity == metadata.currentIdentity))
    {
        SetError(diagnostic,
            ConsoleArtifactV2ErrorCode::FileChangedDuringValidation,
            "Current directory identity differs from the expected v2 snapshot.");
        return false;
    }
    return true;
}

ConsoleArtifactV2ValidationResult
ConsoleArtifactConsumerV2::ValidateCurrentForTest(
    const std::string& configuredWorkspaceRoot,
    const ExpectedRouteArtifactV2Identity& expectedIdentity,
    const std::atomic_bool* cancellation,
    const std::uint64_t generation,
    const std::chrono::milliseconds deadline,
    RouteValidatedHook routeValidatedHook,
    ConsoleArtifactV2ValidationScope validationScope)
{
    std::sort(validationScope.matchedRoutes.begin(),
        validationScope.matchedRoutes.end(),
        [](const ConsoleArtifactV2MatchedRoute& left,
            const ConsoleArtifactV2MatchedRoute& right)
        {
            if (left.routeSlotIndex != right.routeSlotIndex)
                return left.routeSlotIndex < right.routeSlotIndex;
            return left.droneId < right.droneId;
        });
    return ValidateCurrentImpl(configuredWorkspaceRoot, expectedIdentity,
        [cancellation]() { return cancellation && cancellation->load(); },
        generation,
        deadline.count() > 0
            ? std::chrono::steady_clock::now() + deadline
            : std::chrono::steady_clock::now(),
        routeValidatedHook, validationScope, nullptr);
}

void ConsoleArtifactConsumerV2::WorkerMain()
{
    while (!stop_.load())
    {
        Request request;
        std::shared_ptr<const AcceptedRouteArtifactV2> cacheCandidate;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this]()
            {
                return stop_.load() || pending_.has_value();
            });
            if (stop_.load()) break;
            request = std::move(*pending_);
            pending_.reset();
            cacheCandidate = accepted_;
        }

        ConsoleArtifactV2ValidationResult result = ValidateCurrentImpl(
            workspaceRoot_, request.expectedIdentity,
            [this, version = request.cancellationVersion]()
            {
                return stop_.load() || cancellationVersion_.load() != version;
            },
            request.generation, request.deadline, {},
            request.validationScope, cacheCandidate);

        std::lock_guard<std::mutex> lock(mutex_);
        if (!stop_.load()
            && cancellationVersion_.load() == request.cancellationVersion)
        {
            if (result.success) accepted_ = result.snapshot;
            completed_ = std::move(result);
        }
    }
}
