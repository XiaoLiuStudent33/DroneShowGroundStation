#define WIN32_LEAN_AND_MEAN
#include "ConsoleArtifactConsumer.h"

#include <windows.h>

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
    using dronegcs::route_artifact::Digest256;
    using dronegcs::route_artifact::Manifest;

    constexpr std::uint64_t MaxManifestBytes = 64ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t MaxReadyBytes = 512;
    constexpr DWORD StreamingBufferBytes = 64 * 1024;

    void SetError(
        ConsoleArtifactDiagnostic& diagnostic,
        const ConsoleArtifactErrorCode code,
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
        WinHandle(WinHandle&& other) noexcept : value_(other.value_) { other.value_ = INVALID_HANDLE_VALUE; }
        WinHandle& operator=(WinHandle&& other) noexcept
        {
            if (this != &other) { Reset(); value_ = other.value_; other.value_ = INVALID_HANDLE_VALUE; }
            return *this;
        }
        bool Valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
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
        Lower(root); Lower(candidate);
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        if (candidate == root) return true;
        root.push_back(L'\\');
        return candidate.size() > root.size() && candidate.compare(0, root.size(), root) == 0;
    }

    bool FinalPath(HANDLE handle, std::wstring& out)
    {
        const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
        if (required == 0) return false;
        std::wstring buffer(required, L'\0');
        const DWORD written = GetFinalPathNameByHandleW(handle, buffer.data(), required, FILE_NAME_NORMALIZED);
        if (written == 0 || written >= required) return false;
        buffer.resize(written);
        out = std::move(buffer);
        return true;
    }

    bool ValidateEntryAttributes(
        const fs::path& path,
        const bool expectDirectory,
        ConsoleArtifactDiagnostic& diagnostic)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            SetError(diagnostic,
                expectDirectory ? ConsoleArtifactErrorCode::CurrentMissing : ConsoleArtifactErrorCode::RouteMissing,
                expectDirectory ? "Required Artifact directory is missing." : "Required Artifact file is missing.");
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::ReparsePointDetected,
                "Artifact path contains a reparse point.");
            return false;
        }
        const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDirectory != expectDirectory)
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::IoFailure,
                "Artifact entry has the wrong filesystem type.");
            return false;
        }
        return true;
    }

    bool ValidateWorkspace(
        const std::string& configuredRoot,
        fs::path& root,
        fs::path& current,
        std::wstring& currentFinal,
        ConsoleArtifactDiagnostic& diagnostic)
    {
        if (configuredRoot.empty())
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::InvalidWorkspaceRoot,
                "Artifact Workspace root is not configured.");
            return false;
        }
        root = fs::u8path(configuredRoot).lexically_normal();
        if (!root.is_absolute() || root == root.root_path())
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::InvalidWorkspaceRoot,
                "Artifact Workspace root must be an absolute non-volume path.");
            return false;
        }
        current = root / L"Current";
        if (!ValidateEntryAttributes(root, true, diagnostic)
            || !ValidateEntryAttributes(current, true, diagnostic)) return false;

        WinHandle rootHandle(CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        WinHandle currentHandle(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        std::wstring rootFinal;
        if (!rootHandle.Valid() || !currentHandle.Valid()
            || !FinalPath(rootHandle.Get(), rootFinal) || !FinalPath(currentHandle.Get(), currentFinal))
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::IoFailure,
                "Artifact Workspace could not be opened safely.");
            return false;
        }
        if (!IsContainedFinalPath(rootFinal, currentFinal))
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::PathEscapesRoot,
                "Current does not resolve beneath the configured Workspace root.");
            return false;
        }
        return true;
    }

    bool ValidatePathChain(
        const fs::path& current,
        const fs::path& relative,
        const bool finalDirectory,
        ConsoleArtifactDiagnostic& diagnostic)
    {
        fs::path cursor = current;
        std::size_t index = 0;
        const std::size_t count = static_cast<std::size_t>(std::distance(relative.begin(), relative.end()));
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
        ConsoleArtifactDiagnostic& diagnostic)
    {
        if (!ValidatePathChain(current, relative, false, diagnostic)) return false;
        const fs::path full = current / relative;
        out = WinHandle(CreateFileW(full.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!out.Valid())
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::IoFailure,
                "Artifact file could not be opened.");
            return false;
        }
        FILE_ATTRIBUTE_TAG_INFO tag{};
        std::wstring fileFinal;
        if (!GetFileInformationByHandleEx(out.Get(), FileAttributeTagInfo, &tag, sizeof(tag))
            || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::ReparsePointDetected,
                "Opened Artifact file is a reparse point.");
            return false;
        }
        if (!FinalPath(out.Get(), fileFinal) || !IsContainedFinalPath(currentFinal, fileFinal))
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::PathEscapesRoot,
                "Artifact file resolves outside Current.");
            return false;
        }
        return true;
    }

    bool ReadSmallFile(
        const fs::path& current,
        const std::wstring& currentFinal,
        const fs::path& relative,
        const std::uint64_t maximumBytes,
        std::string& out,
        ConsoleArtifactDiagnostic& diagnostic)
    {
        out.clear();
        WinHandle handle;
        if (!OpenSafeFile(current, currentFinal, relative, handle, diagnostic)) return false;
        BY_HANDLE_FILE_INFORMATION before{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(handle.Get(), &before)
            || !GetFileSizeEx(handle.Get(), &size) || size.QuadPart < 0
            || static_cast<std::uint64_t>(size.QuadPart) > maximumBytes)
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::IoFailure,
                "Artifact metadata file size is invalid.");
            return false;
        }
        out.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < out.size())
        {
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(out.size() - offset, StreamingBufferBytes));
            DWORD read = 0;
            if (!ReadFile(handle.Get(), out.data() + offset, request, &read, nullptr) || read == 0)
            {
                SetError(diagnostic, ConsoleArtifactErrorCode::IoFailure,
                    "Artifact metadata file read failed.");
                return false;
            }
            offset += read;
        }
        BY_HANDLE_FILE_INFORMATION after{};
        if (!GetFileInformationByHandle(handle.Get(), &after)
            || before.dwVolumeSerialNumber != after.dwVolumeSerialNumber
            || before.nFileIndexHigh != after.nFileIndexHigh
            || before.nFileIndexLow != after.nFileIndexLow
            || before.nFileSizeHigh != after.nFileSizeHigh
            || before.nFileSizeLow != after.nFileSizeLow
            || before.ftLastWriteTime.dwHighDateTime != after.ftLastWriteTime.dwHighDateTime
            || before.ftLastWriteTime.dwLowDateTime != after.ftLastWriteTime.dwLowDateTime)
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::FileChangedDuringValidation,
                "Artifact metadata changed during validation.");
            return false;
        }
        return true;
    }

    bool SameBinding(
        const dronegcs::route_artifact::AssignmentBinding& left,
        const dronegcs::route_artifact::AssignmentBinding& right)
    {
        return left.FleetSessionId == right.FleetSessionId
            && left.AssignmentAuthoritySessionId == right.AssignmentAuthoritySessionId
            && left.AssignmentRevision == right.AssignmentRevision
            && left.AssignmentDigest == right.AssignmentDigest;
    }

    bool ValidateExpected(
        const Manifest& manifest,
        const ExpectedArtifactIdentity& expected,
        ConsoleArtifactDiagnostic& diagnostic)
    {
        if (manifest.ArtifactSetId != expected.artifactSetId
            || manifest.ArtifactSetDigest != expected.artifactSetDigest
            || !SameBinding(manifest.Assignment, expected.assignmentBinding))
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::ArtifactIdentityMismatch,
                "Current Artifact does not match the expected identity.");
            return false;
        }
        return true;
    }

    Digest256 FrozenDigest(const dspgcs::FrozenAssignmentDigest& source)
    {
        Digest256 result;
        std::copy(source.begin(), source.end(), result.Bytes.begin());
        result.IsSet = true;
        return result;
    }

    bool ValidateFrozen(
        const Manifest& manifest,
        const std::shared_ptr<const FrozenAssignmentAcceptedSnapshot>& frozen,
        ConsoleArtifactDiagnostic& diagnostic)
    {
        if (!frozen)
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::FrozenAssignmentMissing,
                "An accepted Frozen Assignment is required.");
            return false;
        }
        const auto RevisionFits = frozen->identity.assignmentRevision <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const auto FleetRevisionFits = frozen->fleetRevisionAtCapture <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const auto DatasetGenerationFits = frozen->routeDatasetGeneration <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const auto CountFits = [](const std::uint32_t value)
        {
            return value <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
        };
        const bool completionMatches =
            (manifest.Completion == dronegcs::route_artifact::CompletionMode::Full
                && frozen->completionMode == dspgcs::FrozenAssignmentCompletionMode::Full)
            || (manifest.Completion == dronegcs::route_artifact::CompletionMode::Degraded
                && frozen->completionMode == dspgcs::FrozenAssignmentCompletionMode::Degraded);
        const bool freezeMatches =
            (manifest.Freeze == dronegcs::route_artifact::FreezePolicy::RequireFull
                && frozen->freezePolicy == dspgcs::FrozenAssignmentFreezePolicy::RequireFull)
            || (manifest.Freeze == dronegcs::route_artifact::FreezePolicy::AllowDegraded
                && frozen->freezePolicy == dspgcs::FrozenAssignmentFreezePolicy::AllowDegraded);
        if (!RevisionFits || !FleetRevisionFits || !DatasetGenerationFits
            || !CountFits(frozen->routeSlotCount) || !CountFits(frozen->matchedCount)
            || !CountFits(frozen->spareCount) || !CountFits(frozen->excludedCount)
            || !CountFits(frozen->vacantCount)
            || manifest.Assignment.FleetSessionId != frozen->identity.fleetSessionId
            || manifest.Assignment.AssignmentAuthoritySessionId != frozen->identity.assignmentAuthoritySessionId
            || manifest.Assignment.AssignmentRevision != static_cast<std::int64_t>(frozen->identity.assignmentRevision)
            || manifest.Assignment.AssignmentDigest != FrozenDigest(frozen->canonicalPayloadDigest)
            || manifest.FleetRevisionAtCapture != static_cast<std::int64_t>(frozen->fleetRevisionAtCapture)
            || manifest.RouteDatasetGeneration != static_cast<std::int64_t>(frozen->routeDatasetGeneration)
            || !completionMatches || !freezeMatches
            || manifest.RouteSlotCount != static_cast<std::int32_t>(frozen->routeSlotCount)
            || manifest.MatchedCount != static_cast<std::int32_t>(frozen->matchedCount)
            || manifest.SpareCount != static_cast<std::int32_t>(frozen->spareCount)
            || manifest.ExcludedCount != static_cast<std::int32_t>(frozen->excludedCount)
            || manifest.VacantCount != static_cast<std::int32_t>(frozen->vacantCount))
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::FrozenAssignmentMismatch,
                "Manifest audit data differs from the accepted Frozen Assignment.");
            return false;
        }
        std::vector<dronegcs::route_artifact::MatchedMapping> mapping;
        mapping.reserve(frozen->matchedMappings.size());
        for (const auto& entry : frozen->matchedMappings)
            mapping.push_back({ entry.droneId, entry.routeSlotIndex });
        std::sort(mapping.begin(), mapping.end(), dronegcs::route_artifact::MappingLess);
        if (mapping.size() != manifest.Mapping.size())
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::FrozenAssignmentMismatch,
                "Manifest mapping count differs from the accepted Frozen Assignment.");
            return false;
        }
        for (std::size_t index = 0; index < mapping.size(); ++index)
        {
            if (mapping[index].DroneId != manifest.Mapping[index].DroneId
                || mapping[index].RouteSlotIndex != manifest.Mapping[index].RouteSlotIndex)
            {
                SetError(diagnostic, ConsoleArtifactErrorCode::FrozenAssignmentMismatch,
                    "Manifest mapping differs from the accepted Frozen Assignment.");
                return false;
            }
        }
        return true;
    }

    bool ValidateAllowlist(
        const fs::path& current,
        const Manifest& manifest,
        ConsoleArtifactDiagnostic& diagnostic)
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
        fs::recursive_directory_iterator iterator(current, fs::directory_options::none, error);
        const fs::recursive_directory_iterator end;
        while (!error && iterator != end)
        {
            const fs::directory_entry entry = *iterator;
            const DWORD attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                SetError(diagnostic, ConsoleArtifactErrorCode::IoFailure,
                    "Current enumeration encountered an unreadable entry.");
                return false;
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                iterator.disable_recursion_pending();
                SetError(diagnostic, ConsoleArtifactErrorCode::ReparsePointDetected,
                    "Current contains a reparse point.");
                return false;
            }
            const std::string relative = entry.path().lexically_relative(current).generic_u8string();
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) actualDirectories.insert(relative);
            else actualFiles.insert(relative);
            iterator.increment(error);
        }
        if (error || actualFiles != expectedFiles || actualDirectories != expectedDirectories)
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::UnexpectedCurrentEntry,
                "Current contains missing or unexpected files/directories.");
            return false;
        }
        return true;
    }

    struct LoadedMetadata
    {
        fs::path root;
        fs::path current;
        std::wstring currentFinal;
        DWORD currentVolumeSerial = 0;
        DWORD currentFileIndexHigh = 0;
        DWORD currentFileIndexLow = 0;
        Manifest manifest;
    };

    bool LoadMetadata(
        const std::string& workspaceRoot,
        const ExpectedArtifactIdentity& expected,
        LoadedMetadata& out,
        ConsoleArtifactDiagnostic& diagnostic,
        const bool validateAllowlist)
    {
        if (!ValidateWorkspace(workspaceRoot, out.root, out.current, out.currentFinal, diagnostic)) return false;
        WinHandle currentIdentityHandle(CreateFileW(out.current.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION currentIdentity{};
        if (!currentIdentityHandle.Valid()
            || !GetFileInformationByHandle(currentIdentityHandle.Get(), &currentIdentity))
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::IoFailure,
                "Current identity could not be captured.");
            return false;
        }
        out.currentVolumeSerial = currentIdentity.dwVolumeSerialNumber;
        out.currentFileIndexHigh = currentIdentity.nFileIndexHigh;
        out.currentFileIndexLow = currentIdentity.nFileIndexLow;
        std::string readyBytes;
        std::string manifestBytes;
        if (!ReadSmallFile(out.current, out.currentFinal, L"READY", MaxReadyBytes, readyBytes, diagnostic))
        {
            diagnostic.errorCode = ConsoleArtifactErrorCode::ReadyMissingOrInvalid;
            return false;
        }
        if (!ReadSmallFile(out.current, out.currentFinal, L"manifest.json", MaxManifestBytes, manifestBytes, diagnostic))
        {
            diagnostic.errorCode = ConsoleArtifactErrorCode::ManifestMissingOrInvalid;
            return false;
        }
        dronegcs::route_artifact::Error contractError;
        std::string readySetId;
        Digest256 readyDigest;
        if (!dronegcs::route_artifact::ParseReadyMarker(readyBytes, readySetId, readyDigest, contractError))
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::ReadyMissingOrInvalid,
                "READY bytes are malformed or noncanonical.");
            return false;
        }
        if (!dronegcs::route_artifact::ParseCanonicalManifest(manifestBytes, out.manifest, contractError))
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::ManifestMissingOrInvalid,
                "Manifest bytes are malformed, invalid, or noncanonical.");
            return false;
        }
        if (readySetId != out.manifest.ArtifactSetId || readyDigest != out.manifest.ArtifactSetDigest
            || !ValidateExpected(out.manifest, expected, diagnostic))
        {
            if (diagnostic.errorCode == ConsoleArtifactErrorCode::None)
                SetError(diagnostic, ConsoleArtifactErrorCode::ArtifactIdentityMismatch,
                    "READY, Manifest, and expected identity differ.");
            return false;
        }
        return !validateAllowlist || ValidateAllowlist(out.current, out.manifest, diagnostic);
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

    void SampleHandleCount(ConsoleArtifactValidationMetrics& metrics)
    {
        DWORD handles = 0;
        if (GetProcessHandleCount(GetCurrentProcess(), &handles))
            metrics.peakProcessHandles = std::max(
                metrics.peakProcessHandles,
                static_cast<std::uint32_t>(handles));
    }

    bool ValidateRoute(
        const LoadedMetadata& metadata,
        const dronegcs::route_artifact::RouteRecord& route,
        ConsoleArtifactValidationMetrics& metrics,
        ConsoleArtifactDiagnostic& diagnostic,
        const std::function<bool()>& cancelled)
    {
        const fs::path relative = fs::u8path(route.RelativePath);
        WinHandle handle;
        if (!OpenSafeFile(metadata.current, metadata.currentFinal, relative, handle, diagnostic))
        {
            diagnostic.routeSlotIndex = route.RouteSlotIndex;
            return false;
        }
        ++metrics.physicalRouteOpenCount;
        SampleHandleCount(metrics);
        BY_HANDLE_FILE_INFORMATION before{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(handle.Get(), &before) || !GetFileSizeEx(handle.Get(), &size) || size.QuadPart < 0)
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::IoFailure,
                "Route file metadata could not be read.", route.RouteSlotIndex);
            return false;
        }
        if (size.QuadPart != route.FileByteSize)
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::RouteSizeMismatch,
                "Route file size differs from Manifest.", route.RouteSlotIndex);
            return false;
        }
        dronegcs::route_artifact::Sha256 sha;
        RowCounter rowCounter;
        dronegcs::wpl110::StreamingParser parser(metadata.manifest.FrameCount, &CountRow, &rowCounter);
        std::vector<std::uint8_t> buffer(StreamingBufferBytes);
        std::uint64_t total = 0;
        for (;;)
        {
            if (cancelled())
            {
                SetError(diagnostic, ConsoleArtifactErrorCode::Cancelled,
                    "Artifact validation was cancelled.", route.RouteSlotIndex);
                return false;
            }
            DWORD read = 0;
            if (!ReadFile(handle.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            {
                SetError(diagnostic, ConsoleArtifactErrorCode::IoFailure,
                    "Route streaming read failed.", route.RouteSlotIndex);
                return false;
            }
            if (read == 0) break;
            if (!sha.Update(buffer.data(), read) || !parser.Consume(buffer.data(), read))
            {
                const auto& parserError = parser.GetError();
                SetError(diagnostic, ConsoleArtifactErrorCode::WaypointSemanticInvalid,
                    "Route WPL grammar or semantics are invalid.", route.RouteSlotIndex);
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
            SetError(diagnostic, ConsoleArtifactErrorCode::WaypointSemanticInvalid,
                "Route WPL finalization failed.", route.RouteSlotIndex);
            diagnostic.rowIndex = parserError.RowIndex;
            diagnostic.fieldIndex = parserError.FieldIndex;
            diagnostic.waypointErrorCode = parserError.Code;
            return false;
        }
        Digest256 digest;
        BY_HANDLE_FILE_INFORMATION after{};
        if (!sha.Finalize(digest) || !GetFileInformationByHandle(handle.Get(), &after)
            || before.dwVolumeSerialNumber != after.dwVolumeSerialNumber
            || before.nFileIndexHigh != after.nFileIndexHigh
            || before.nFileIndexLow != after.nFileIndexLow
            || before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow
            || before.ftLastWriteTime.dwHighDateTime != after.ftLastWriteTime.dwHighDateTime
            || before.ftLastWriteTime.dwLowDateTime != after.ftLastWriteTime.dwLowDateTime)
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::FileChangedDuringValidation,
                "Route file changed during validation.", route.RouteSlotIndex);
            return false;
        }
        if (total != static_cast<std::uint64_t>(route.FileByteSize))
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::RouteSizeMismatch,
                "Route bytes read differ from Manifest size.", route.RouteSlotIndex);
            return false;
        }
        if (digest != route.RouteArtifactDigest)
        {
            SetError(diagnostic, ConsoleArtifactErrorCode::RouteDigestMismatch,
                "Route SHA-256 differs from Manifest.", route.RouteSlotIndex);
            return false;
        }
        ++metrics.routeFilesRead;
        metrics.rowsParsed += rowCounter.rows;
        metrics.bytesRead += total;
        return true;
    }

    ConsoleArtifactValidationResult ValidateCurrentImpl(
        const std::string& workspaceRoot,
        const ExpectedArtifactIdentity& expected,
        const std::shared_ptr<const FrozenAssignmentAcceptedSnapshot>& frozen,
        const std::function<bool()>& cancelled,
        const std::uint64_t generation)
    {
        ConsoleArtifactValidationResult result;
        result.generation = generation;
        const auto started = std::chrono::steady_clock::now();
        LoadedMetadata metadata;
        if (cancelled())
        {
            SetError(result.diagnostic, ConsoleArtifactErrorCode::Cancelled,
                "Artifact validation was cancelled.");
            result.metrics.elapsedSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            return result;
        }
        else if (!LoadMetadata(workspaceRoot, expected, metadata, result.diagnostic, true)
            || !ValidateFrozen(metadata.manifest, frozen, result.diagnostic))
        {
            result.metrics.elapsedSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            return result;
        }
        for (const auto& route : metadata.manifest.Routes)
        {
            if (!ValidateRoute(metadata, route, result.metrics, result.diagnostic, cancelled))
            {
                result.metrics.elapsedSeconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started).count();
                return result;
            }
        }
        LoadedMetadata recheck;
        if (!LoadMetadata(workspaceRoot, expected, recheck, result.diagnostic, true))
        {
            result.metrics.elapsedSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            return result;
        }
        if (metadata.currentVolumeSerial != recheck.currentVolumeSerial
            || metadata.currentFileIndexHigh != recheck.currentFileIndexHigh
            || metadata.currentFileIndexLow != recheck.currentFileIndexLow)
        {
            SetError(result.diagnostic, ConsoleArtifactErrorCode::FileChangedDuringValidation,
                "Current was replaced during Artifact validation.");
            result.metrics.elapsedSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            return result;
        }
        auto snapshot = std::make_shared<ValidatedArtifactSnapshot>();
        snapshot->artifactSetId = metadata.manifest.ArtifactSetId;
        snapshot->artifactSetDigest = metadata.manifest.ArtifactSetDigest;
        snapshot->assignmentBinding = metadata.manifest.Assignment;
        snapshot->fleetRevisionAtCapture = metadata.manifest.FleetRevisionAtCapture;
        snapshot->routeDatasetGeneration = metadata.manifest.RouteDatasetGeneration;
        snapshot->completionMode = metadata.manifest.Completion;
        snapshot->freezePolicy = metadata.manifest.Freeze;
        snapshot->matchedCount = metadata.manifest.MatchedCount;
        snapshot->spareCount = metadata.manifest.SpareCount;
        snapshot->excludedCount = metadata.manifest.ExcludedCount;
        snapshot->vacantCount = metadata.manifest.VacantCount;
        snapshot->sourceIdentity = metadata.manifest.Source;
        snapshot->waypointGeneratorVersion = metadata.manifest.WaypointGeneratorVersion;
        snapshot->generatorInputDigest = metadata.manifest.GeneratorInputDigest;
        snapshot->frameCount = metadata.manifest.FrameCount;
        snapshot->routeCount = metadata.manifest.RouteSlotCount;
        snapshot->matchedMappings = metadata.manifest.Mapping;
        snapshot->routes.reserve(metadata.manifest.Routes.size());
        for (const auto& route : metadata.manifest.Routes)
            snapshot->routes.push_back({ route.RouteSlotIndex, route.RelativePath, route.FileByteSize, route.RouteArtifactDigest });
        snapshot->validatedAtUtcMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        snapshot->validationGeneration = generation;
        result.success = true;
        result.snapshot = std::move(snapshot);
        result.metrics.elapsedSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        return result;
    }
}

ConsoleArtifactConsumer::ConsoleArtifactConsumer(std::string configuredWorkspaceRoot)
    : workspaceRoot_(std::move(configuredWorkspaceRoot))
    , worker_(&ConsoleArtifactConsumer::WorkerMain, this)
{
}

ConsoleArtifactConsumer::~ConsoleArtifactConsumer()
{
    stop_.store(true);
    cancellationVersion_.fetch_add(1);
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::uint64_t ConsoleArtifactConsumer::RequestValidation(
    ExpectedArtifactIdentity expectedIdentity,
    std::shared_ptr<const FrozenAssignmentAcceptedSnapshot> frozenAssignment)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t version = cancellationVersion_.fetch_add(1) + 1;
    const std::uint64_t generation = ++nextGeneration_;
    pending_ = Request{ generation, version, std::move(expectedIdentity), std::move(frozenAssignment) };
    completed_.reset();
    wake_.notify_one();
    return generation;
}

bool ConsoleArtifactConsumer::PollCompleted(ConsoleArtifactValidationResult& result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!completed_) return false;
    result = std::move(*completed_);
    completed_.reset();
    return true;
}

void ConsoleArtifactConsumer::CancelValidation()
{
    cancellationVersion_.fetch_add(1);
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.reset();
}

bool ConsoleArtifactConsumer::IsCurrentIdentityStill(
    const ExpectedArtifactIdentity& expectedIdentity,
    ConsoleArtifactDiagnostic& diagnostic) const
{
    LoadedMetadata metadata;
    return LoadMetadata(workspaceRoot_, expectedIdentity, metadata, diagnostic, false);
}

ConsoleArtifactValidationResult ConsoleArtifactConsumer::ValidateCurrentForTest(
    const std::string& configuredWorkspaceRoot,
    const ExpectedArtifactIdentity& expectedIdentity,
    const std::shared_ptr<const FrozenAssignmentAcceptedSnapshot>& frozenAssignment,
    const std::atomic_bool* cancellation,
    const std::uint64_t generation)
{
    return ValidateCurrentImpl(configuredWorkspaceRoot, expectedIdentity, frozenAssignment,
        [cancellation]() { return cancellation && cancellation->load(); }, generation);
}

void ConsoleArtifactConsumer::WorkerMain()
{
    while (!stop_.load())
    {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this]() { return stop_.load() || pending_.has_value(); });
            if (stop_.load()) break;
            request = std::move(*pending_);
            pending_.reset();
        }
        ConsoleArtifactValidationResult result = ValidateCurrentImpl(
            workspaceRoot_, request.expectedIdentity, request.frozenAssignment,
            [this, version = request.cancellationVersion]()
            {
                return stop_.load() || cancellationVersion_.load() != version;
            }, request.generation);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stop_.load() && cancellationVersion_.load() == request.cancellationVersion)
            completed_ = std::move(result);
    }
}
