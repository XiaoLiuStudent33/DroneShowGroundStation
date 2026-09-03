/** Launcher-only lifecycle. No network, command, or configuration authority. */
#pragma once

#include "AppConfig.h"
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <string>
#include <atomic>

class ManagedBackendSession
{
public:
    ~ManagedBackendSession()
    {
        if (handlerInstalled_) SetConsoleCtrlHandler(ConsoleControl, FALSE);
        if (ready_) CloseHandle(ready_);
        if (stop_) CloseHandle(stop_);
        if (owner_) CloseHandle(owner_);
    }
    ManagedBackendSession() = default;
    ManagedBackendSession(const ManagedBackendSession&) = delete;
    ManagedBackendSession& operator=(const ManagedBackendSession&) = delete;

    static bool IsReadOnly(const AppConfig& config)
    {
        return config.safeTestMode && config.mavlinkNetworkEnabled &&
            config.dspgcsTelemetryEnabled && !config.realCommandEnabled &&
            !config.dspgcsCommandEnabled && !config.routeUploadEnabled;
    }

    bool Open(const std::string& token, const AppConfig& config,
        bool hasProbeOrHardwareTest, std::string& error)
    {
        if (token.empty()) return true; // Manual invocation is unchanged.
        stopOnOwnerExit_ = IsReadOnly(config);
        if (!config.mavlinkNetworkEnabled || !config.dspgcsTelemetryEnabled || hasProbeOrHardwareTest)
        {
            error = "Launcher mode requires production network/telemetry; "
                "standalone hardware/probe tests cannot be launcher-managed.";
            return false;
        }
        const auto split = token.find('_');
        DWORD pid = 0;
        if (split == std::string::npos || split == 0 || token.size() > 96 ||
            token.size() < split + 9 || !std::all_of(token.begin(), token.end(),
                [](char c) { return (c >= '0' && c <= '9') ||
                    (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
                    c == '-' || c == '_'; }))
        {
            error = "Invalid launcher session token.";
            return false;
        }
        const auto parsed = std::from_chars(token.data(), token.data() + split, pid);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + split ||
            pid == 0 || pid == GetCurrentProcessId())
        {
            error = "Invalid launcher owner PID.";
            return false;
        }
        const std::wstring prefix = L"Local\\DroneGCS.Managed." +
            std::wstring(token.begin(), token.end());
        // Open existing events only. A manual/old executable cannot claim readiness.
        ready_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, (prefix + L".ready").c_str());
        stop_ = OpenEventW(SYNCHRONIZE, FALSE, (prefix + L".stop").c_str());
        owner_ = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!ready_ || !stop_ || !owner_ ||
            WaitForSingleObject(owner_, 0) != WAIT_TIMEOUT ||
            WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT)
        {
            error = "Launcher owner/events unavailable; startup refused.";
            return false;
        }
        operatorStop_.store(false);
        handlerInstalled_ = SetConsoleCtrlHandler(ConsoleControl, TRUE) != FALSE;
        if (!handlerInstalled_) { error = "Cannot install Console shutdown handler."; return false; }
        return true;
    }

    bool Enabled() const { return owner_ != nullptr; }
    bool ShouldStop() const
    {
        return owner_ && (operatorStop_.load() ||
            (stopOnOwnerExit_ && WaitForSingleObject(owner_, 0) != WAIT_TIMEOUT) ||
            WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT);
    }
    bool MarkReady()
    {
        if (!ready_ || readySent_) return true;
        if (!SetEvent(ready_)) return false;
        readySent_ = true;
        return true;
    }

private:
    static BOOL WINAPI ConsoleControl(DWORD type)
    {
        if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
        operatorStop_.store(true);
        return TRUE;
    }
    inline static std::atomic<bool> operatorStop_{false};
    bool stopOnOwnerExit_ = true;
    bool handlerInstalled_ = false;
    HANDLE ready_ = nullptr;
    HANDLE stop_ = nullptr;
    HANDLE owner_ = nullptr;
    bool readySent_ = false;
};
