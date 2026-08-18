#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <string>

#if defined(_WIN32)
#if __has_include("Windows/WindowsHWrapper.h")
#include "Windows/WindowsHWrapper.h"
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#endif

namespace dronegcs::route_artifact
{
    enum class ArtifactReadLeaseAcquireResult : std::uint8_t
    {
        Acquired,
        Busy,
        Error
    };

    /**
     * Cross-process Current swap gate. A Console Upload Job owns the gate for
     * its entire read lifetime; UE promotion takes the same gate briefly.
     * Windows releases an owned mutex when its process/thread terminates, so no
     * filesystem tombstone or historical Artifact archive is required.
     */
    class ArtifactReadLease final
    {
    public:
        ArtifactReadLease() = default;
        ~ArtifactReadLease() { Release(); }
        ArtifactReadLease(const ArtifactReadLease&) = delete;
        ArtifactReadLease& operator=(const ArtifactReadLease&) = delete;

        ArtifactReadLeaseAcquireResult TryAcquire(
            const std::wstring& workspaceRoot,
            bool& recoveredAbandonedOwner,
            std::string& error)
        {
            Release();
            recoveredAbandonedOwner = false;
            error.clear();
            if (workspaceRoot.empty())
            {
                error = "Artifact Workspace root is empty.";
                return ArtifactReadLeaseAcquireResult::Error;
            }
#if defined(_WIN32)
            const std::wstring name = BuildMutexName(workspaceRoot);
            HANDLE candidate = CreateMutexW(nullptr, 0, name.c_str());
            if (!candidate)
            {
                error = "Artifact read lease mutex could not be opened.";
                return ArtifactReadLeaseAcquireResult::Error;
            }
            const DWORD wait = WaitForSingleObject(candidate, 0);
            if (wait == WAIT_TIMEOUT)
            {
                CloseHandle(candidate);
                return ArtifactReadLeaseAcquireResult::Busy;
            }
            if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
            {
                CloseHandle(candidate);
                error = "Artifact read lease mutex wait failed.";
                return ArtifactReadLeaseAcquireResult::Error;
            }
            handle_ = candidate;
            recoveredAbandonedOwner = wait == WAIT_ABANDONED;
            return ArtifactReadLeaseAcquireResult::Acquired;
#else
            (void)workspaceRoot;
            error = "Artifact read lease is supported only on Windows.";
            return ArtifactReadLeaseAcquireResult::Error;
#endif
        }

        void Release()
        {
#if defined(_WIN32)
            if (handle_)
            {
                ReleaseMutex(handle_);
                CloseHandle(handle_);
                handle_ = nullptr;
            }
#endif
        }

        bool IsHeld() const
        {
#if defined(_WIN32)
            return handle_ != nullptr;
#else
            return false;
#endif
        }

        static std::wstring BuildMutexName(std::wstring workspaceRoot)
        {
            std::replace(workspaceRoot.begin(), workspaceRoot.end(), L'/', L'\\');
            while (workspaceRoot.size() > 3 && workspaceRoot.back() == L'\\')
                workspaceRoot.pop_back();
            std::transform(workspaceRoot.begin(), workspaceRoot.end(), workspaceRoot.begin(),
                [](const wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
            std::uint64_t hash = 1469598103934665603ULL;
            for (const wchar_t value : workspaceRoot)
            {
                const std::uint16_t code = static_cast<std::uint16_t>(value);
                hash ^= static_cast<std::uint8_t>(code & 0xffU);
                hash *= 1099511628211ULL;
                hash ^= static_cast<std::uint8_t>((code >> 8U) & 0xffU);
                hash *= 1099511628211ULL;
            }
            std::wostringstream stream;
            stream << L"Local\\DroneGCS-ArtifactCurrentGate-v1-"
                << std::hex << std::setfill(L'0') << std::setw(16) << hash;
            return stream.str();
        }

    private:
#if defined(_WIN32)
        HANDLE handle_ = nullptr;
#endif
    };
}
