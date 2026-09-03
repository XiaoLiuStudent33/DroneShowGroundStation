// One-click joint package supervisor. Native backend stays visible.
// No drone protocol, command forwarding, configuration rewriting or forced kills.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <objbase.h>

#include "AppConfig.h"
#include "DSPGCSTransportConfigLoader.h"
#include "ManagedBackendSession.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class Handle
{
public:
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool Valid() const { return value && value != INVALID_HANDLE_VALUE; }
};

std::ofstream logFile;
fs::path logDirectory;
bool checkOnly = false;
bool noDialogs = false;
bool readOnlySession = true;
fs::path backendDirectory;
fs::path runtimeConfigDirectory;
fs::path gameExecutable;
fs::path gameDirectory;
fs::path artifactWorkspace;
fs::path userDataRoot;
fs::path ueUserDirectory;
bool installedLayout = false;

std::wstring Wide(const std::string& s)
{
    if (s.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.data(),
        static_cast<int>(s.size()), nullptr, 0);
    std::wstring result(size, L' ');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
        result.data(), size);
    return result;
}

std::string Utf8(const std::wstring& s)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, s.data(),
        static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, ' ');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

void Log(const std::wstring& message)
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    logFile << t.wHour << ':' << t.wMinute << ':' << t.wSecond << ' '
        << Utf8(message) << std::endl;
}

[[noreturn]] void Fail(const std::wstring& message)
{
    throw std::runtime_error(Utf8(message));
}

std::wstring WinError(DWORD code = GetLastError())
{
    wchar_t text[1024]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, text, 1024, nullptr);
    return L"Windows " + std::to_wstring(code) + L": " + text;
}

void Report(const std::wstring& message)
{
    Log(L"ERROR: " + message);
    if (!checkOnly && !noDialogs)
        MessageBoxW(nullptr, (message + L"\n\n日志目录：\n" +
            logDirectory.wstring()).c_str(), L"DroneGCS 联合启动器",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

fs::path ExecutableDirectory()
{
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(nullptr, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) Fail(L"无法确定启动器所在目录。");
    return fs::path(std::wstring(buffer.data(), length)).parent_path();
}

fs::path EnvironmentDirectory(const wchar_t* name)
{
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetEnvironmentVariableW(name, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size())
        Fail(L"无法取得 Windows 用户数据目录。");
    return fs::path(std::wstring(buffer.data(), length));
}

void OpenLogs(const fs::path& root)
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t run[100]{};
    swprintf_s(run, L"%04u%02u%02u-%02u%02u%02u-%lu", t.wYear, t.wMonth,
        t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    const fs::path primary = userDataRoot / L"Logs";
    const fs::path fallback = root / L"Logs";
    for (const auto& parent : {primary, fallback})
    {
        std::error_code ec;
        const fs::path candidate = parent / run;
        // Never reuse/truncate a prior run's logs (including PID reuse).
        if (fs::exists(candidate, ec)) continue;
        fs::create_directories(candidate, ec);
        if (ec) continue;
        logFile.open(candidate / L"launcher.log", std::ios::out);
        if (logFile)
        {
            logDirectory = candidate;
            Log(L"DroneGCS joint launcher v2; root=" + root.wstring());
            return;
        }
        logFile.clear();
    }
    Fail(L"无法创建日志目录，请检查当前用户 AppData 写入权限。");
}

void CheckOtherProcesses()
{
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.Valid()) Fail(L"无法检查现有进程：" + WinError());
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD ourSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &ourSession))
        Fail(L"无法读取当前会话。");
    if (!Process32FirstW(snapshot.value, &entry))
        Fail(L"无法读取进程列表：" + WinError());
    do
    {
        if (_wcsicmp(entry.szExeFile, L"DroneGCS.exe") != 0 &&
            _wcsicmp(entry.szExeFile, L"DroneShowPlatform1.exe") != 0 &&
            _wcsicmp(entry.szExeFile, L"DroneShowPlatform1-Win64-Shipping.exe") != 0)
            continue;
        DWORD session = 0;
        if (ProcessIdToSessionId(entry.th32ProcessID, &session) && session == ourSession)
            Fail(L"已有 UE 或 Console 正在运行（" + std::wstring(entry.szExeFile) +
                L"，PID " + std::to_wstring(entry.th32ProcessID) +
                L"）。请先正常关闭旧窗口，再双击启动器；不会接管或终止旧进程。");
    } while (Process32NextW(snapshot.value, &entry));
}

void CheckUdpPort(const std::string& address, unsigned short port)
{
    IN_ADDR target{};
    if (InetPtonA(AF_INET, address.c_str(), &target) != 1)
        Fail(L"无效 IPv4 地址：" + Wide(address));
    ULONG bytes = 0;
    DWORD status = GetExtendedUdpTable(nullptr, &bytes, FALSE, AF_INET,
        UDP_TABLE_OWNER_PID, 0);
    std::vector<unsigned char> table;
    for (int retry = 0; retry < 4 && status == ERROR_INSUFFICIENT_BUFFER; ++retry)
    {
        table.resize(bytes);
        status = GetExtendedUdpTable(table.data(), &bytes, FALSE, AF_INET,
            UDP_TABLE_OWNER_PID, 0);
    }
    if (status != NO_ERROR || table.empty())
        Fail(L"无法检查 UDP 端口：" + WinError(status));
    const auto* rows = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(table.data());
    for (DWORD i = 0; i < rows->dwNumEntries; ++i)
    {
        const auto& row = rows->table[i];
        if (ntohs(static_cast<u_short>(row.dwLocalPort)) == port &&
            (!row.dwLocalAddr || !target.S_un.S_addr || row.dwLocalAddr == target.S_un.S_addr))
            Fail(L"UDP " + Wide(address) + L":" + std::to_wstring(port) +
                L" 已被 PID " + std::to_wstring(row.dwOwningPid) +
                L" 占用。请先核对并正常关闭占用程序。");
    }
}

void RequireFile(const fs::path& path)
{
    if (!fs::is_regular_file(path)) Fail(L"缺少文件：\n" + path.wstring());
}

void CopyInitialConfig(const fs::path& source, const fs::path& destination)
{
    if (fs::is_regular_file(destination)) return;
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) Fail(L"无法创建用户配置目录：\n" + destination.parent_path().wstring());
    if (!fs::copy_file(source, destination, fs::copy_options::none, ec) &&
        !fs::is_regular_file(destination))
        Fail(L"无法初始化用户配置：\n" + destination.wstring());
}

void ResolveRuntimeConfig()
{
    const fs::path bundledConfig = backendDirectory / L"gcs-config.ini";
    const fs::path bundledTransport = backendDirectory / L"config/DSPGCSTransport.ini";
    RequireFile(bundledConfig);
    RequireFile(bundledTransport);
    if (!installedLayout)
    {
        runtimeConfigDirectory = backendDirectory;
        return;
    }
    runtimeConfigDirectory = userDataRoot / L"Config";
    CopyInitialConfig(bundledConfig, runtimeConfigDirectory / L"gcs-config.ini");
    CopyInitialConfig(bundledTransport,
        runtimeConfigDirectory / L"config/DSPGCSTransport.ini");
}

void RequireLocalAddress(const std::string& address)
{
    IN_ADDR target{};
    if (InetPtonA(AF_INET, address.c_str(), &target) != 1)
        Fail(L"无效 IPv4 地址：" + Wide(address));
    ULONG bytes = 15 * 1024;
    std::vector<unsigned char> storage(bytes);
    DWORD status = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(storage.data()), &bytes);
    if (status == ERROR_BUFFER_OVERFLOW)
    {
        storage.resize(bytes);
        status = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(storage.data()), &bytes);
    }
    if (status != NO_ERROR) Fail(L"无法检查本机 IPv4 地址：" + WinError(status));
    for (auto* adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(storage.data());
        adapter; adapter = adapter->Next)
    {
        for (auto* item = adapter->FirstUnicastAddress; item; item = item->Next)
        {
            if (item->Address.lpSockaddr && item->Address.lpSockaddr->sa_family == AF_INET &&
                reinterpret_cast<sockaddr_in*>(item->Address.lpSockaddr)->sin_addr.S_un.S_addr ==
                    target.S_un.S_addr)
                return;
        }
    }
    Fail(L"本机没有配置 " + Wide(address) +
        L"。请先把连接无人机的网卡 IPv4 设置为该地址，再重新启动；启动器不会自动修改网卡。");
}

void Preflight(const fs::path& root)
{
    // UAT stages ProjectDir files under Windows/DroneShowPlatform1. The old
    // sibling Windows/Backend layout remains supported for isolated fixtures.
    const bool staged = fs::is_directory(root / L"Binaries/Win64");
    const fs::path project = staged ? root : root / L"Windows/DroneShowPlatform1";
    gameDirectory = staged ? root.parent_path() : root / L"Windows";
    backendDirectory = staged ? root / L"Binaries/Win64/Backend" : root / L"Backend";
    RequireFile(backendDirectory / L"DroneGCS.exe");
    ResolveRuntimeConfig();
    const fs::path devExe = project / L"Binaries/Win64/DroneShowPlatform1.exe";
    const fs::path shippingExe = project / L"Binaries/Win64/DroneShowPlatform1-Win64-Shipping.exe";
    if (fs::is_regular_file(devExe) == fs::is_regular_file(shippingExe))
        Fail(L"需要且只能有一套 UE Game EXE（Development 或 Shipping）。请打包到新的空目录。");
    gameExecutable = fs::is_regular_file(shippingExe) ? shippingExe : devExe;
    if (!fs::is_directory(project / L"Content/Paks") ||
        !fs::is_directory(gameDirectory / L"Engine"))
        Fail(L"UE 打包目录不完整。请保留整个 Windows 文件夹，而非只复制 EXE。");
    CheckOtherProcesses();
    if (!SetCurrentDirectoryW(runtimeConfigDirectory.c_str()))
        Fail(L"无法设置配置工作目录：" + WinError());
    // Reuse the production parsers; do not invent a second INI authority.
    AppConfig config;
    dspgcs::DSPGCSTransportConfig transport;
    std::string error;
    if (!AppConfig::Load("gcs-config.ini", config, error) ||
        !LoadDSPGCSTransportConfig(config.dspgcsTransportConfigPath, transport, error))
        Fail(L"后端配置无效：\n" + Wide(error));
    if (!config.mavlinkNetworkEnabled || !config.dspgcsTelemetryEnabled)
        Fail(L"联合启动需要启用 MAVLink 网络和 DSP 遥测。请核对配置。");
    readOnlySession = ManagedBackendSession::IsReadOnly(config);
    artifactWorkspace = config.artifactWorkspaceRoot.empty()
        ? userDataRoot / L"ArtifactWorkspace"
        : fs::u8path(config.artifactWorkspaceRoot);
    if (!artifactWorkspace.is_absolute() || artifactWorkspace == artifactWorkspace.root_path())
        Fail(L"无效航线工作目录。");
    // This deployment is paired with the preserved UE package's endpoints.
    if (transport.type != dspgcs::TransportType::Loopback ||
        transport.bindAddress != "127.0.0.1" || transport.bindPort != 12001 ||
        transport.remoteAddress != "127.0.0.1" || transport.remotePort != 12002)
        Fail(L"当前 UE 包使用 127.0.0.1:12002，Console 使用 127.0.0.1:12001。"
            L"\nDSP-GCS 配置与此包不匹配，请核对配置；启动器不会自动改端口。");
    RequireLocalAddress(config.mavlinkBindIP);
    CheckUdpPort(config.mavlinkBindIP, static_cast<unsigned short>(config.mavlinkBindPort));
    CheckUdpPort(transport.bindAddress, transport.bindPort);
    CheckUdpPort(transport.remoteAddress, transport.remotePort);
    Log(std::wstring(L"Preflight OK: ") + (readOnlySession ? L"read-only" : L"control-enabled") +
        L"; MAVLink=" + Wide(config.mavlinkBindIP) + L":" +
        std::to_wstring(config.mavlinkBindPort) + L"; DSP=127.0.0.1:12001 -> 127.0.0.1:12002");
    Log(L"ArtifactWorkspace=" + artifactWorkspace.wstring());
    Log(L"RuntimeConfig=" + (runtimeConfigDirectory / L"gcs-config.ini").wstring());
}

struct Process
{
    Handle process;
    DWORD pid = 0;
    DWORD ExitCode() const
    {
        DWORD code = 0;
        if (!GetExitCodeProcess(process.value, &code)) return GetLastError();
        return code;
    }
};

void Start(Process& child, const fs::path& exe, const std::wstring& arguments,
    const fs::path& cwd, bool visibleConsole = false)
{
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::wstring command = L"\"" + exe.wstring() + L"\" " + arguments;
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr,
            FALSE, visibleConsole ? CREATE_NEW_CONSOLE : 0, nullptr,
            cwd.c_str(), &startup, &info))
        Fail(L"无法启动 " + exe.filename().wstring() + L"：" + WinError());
    child.process.value = info.hProcess;
    child.pid = info.dwProcessId;
    CloseHandle(info.hThread);
    Log(L"Started " + exe.wstring() + L" PID=" + std::to_wstring(child.pid));
}

struct BackendSession
{
    Handle ready;
    Handle stop;
    Process backend;
    std::wstring token;
    bool cleanupAttempted = false;
    bool cleanExit = false;

    BackendSession()
    {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) Fail(L"无法创建启动会话。");
        wchar_t guidText[40]{};
        StringFromGUID2(guid, guidText, 40);
        token = std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::wstring(guidText + 1, 36);
        const auto prefix = L"Local\\DroneGCS.Managed." + token;
        ready.value = CreateEventW(nullptr, TRUE, FALSE, (prefix + L".ready").c_str());
        stop.value = CreateEventW(nullptr, TRUE, FALSE, (prefix + L".stop").c_str());
        if (!ready.Valid() || !stop.Valid()) Fail(L"无法创建启动握手：" + WinError());
    }

    ~BackendSession()
    {
        Stop();
    }

    bool Stop()
    {
        if (cleanupAttempted) return cleanExit;
        cleanupAttempted = true;
        if (!backend.process.Valid()) return cleanExit = true;
        if (WaitForSingleObject(backend.process.value, 0) == WAIT_TIMEOUT)
        {
            if (!readOnlySession)
            {
                Log(L"Control backend remains running, PID=" + std::to_wstring(backend.pid) +
                    L"; UE/launcher exit never stops an active control service.");
                return cleanExit = true;
            }
            Log(L"Requesting graceful backend stop (owned PID=" +
                std::to_wstring(backend.pid) + L")");
            SetEvent(stop.value);
            if (WaitForSingleObject(backend.process.value, 15000) != WAIT_OBJECT_0)
            {
                Report(L"后端未在 15 秒内退出，PID=" + std::to_wstring(backend.pid) +
                    L"。未强制结束进程，请保留日志并检查。请勿重复启动。");
                return false;
            }
        }
        Log(L"Backend exited, code=" + std::to_wstring(backend.ExitCode()));
        return cleanExit = backend.ExitCode() == 0;
    }
};

int Run()
{
    const auto root = ExecutableDirectory();
    installedLayout = fs::is_regular_file(root / L"installed-layout.marker");
    userDataRoot = EnvironmentDirectory(L"LOCALAPPDATA") / L"DroneGCS";
    ueUserDirectory = userDataRoot / L"UE";
    OpenLogs(root);
    Handle mutex(CreateMutexW(nullptr, FALSE, L"Local\\DroneGCS.JointLauncher.v2"));
    const auto mutexError = GetLastError();
    if (!mutex.Valid()) Fail(L"无法创建单实例锁：" + WinError(mutexError));
    if (mutexError == ERROR_ALREADY_EXISTS)
        Fail(L"联合启动器已经运行。请使用已打开的 UE 窗口，不要重复启动后端。");
    Preflight(root);
    if (checkOnly)
    {
        Log(L"CHECK PASSED; no child processes started; no network packets sent.");
        return 0;
    }

    BackendSession session;
    fs::create_directories(artifactWorkspace);
    fs::create_directories(ueUserDirectory);
    if (!SetEnvironmentVariableW(L"DRONEGCS_ARTIFACT_WORKSPACE", artifactWorkspace.c_str()) ||
        !SetEnvironmentVariableW(L"DRONEGCS_BACKEND_LOG", (logDirectory / L"backend.log").c_str()) ||
        !SetEnvironmentVariableW(L"DRONEGCS_ROUTE_UPLOAD_LOG", (logDirectory / L"route-upload.log").c_str()) ||
        !SetEnvironmentVariableW(L"DRONEGCS_CONFIG_PATH",
            (runtimeConfigDirectory / L"gcs-config.ini").c_str()))
        Fail(L"无法设置本次子进程运行目录：" + WinError());
    Start(session.backend, backendDirectory / L"DroneGCS.exe",
        L"--launcher-session " + session.token, runtimeConfigDirectory, true);
    HANDLE startupWait[] = {session.backend.process.value, session.ready.value};
    const DWORD readyResult = WaitForMultipleObjects(2, startupWait, FALSE, 30000);
    if (readyResult == WAIT_OBJECT_0)
        Fail(L"后端启动失败，退出码=" + std::to_wstring(session.backend.ExitCode()) +
            L"。请查看 backend.log 中的配置/IP/端口错误。UE 尚未启动。");
    if (readyResult != WAIT_OBJECT_0 + 1)
        Fail(L"后端未在 30 秒内完成就绪握手。请确认使用配套后端版本并查看 backend.log。"
            L"\nUE 尚未启动。控制模式下后端不会被自动结束，请检查后端窗口。");
    Log(L"Backend ready: sockets initialized and first telemetry snapshot published.");

    Process ue;
    // Launch the real game, not the short-lived root bootstrap process.
    Start(ue, gameExecutable,
        L"-SaveToUserDir -UserDir=\"" + ueUserDirectory.wstring() + L"\"", gameDirectory);
    HANDLE wait[] = {ue.process.value, session.backend.process.value};
    const DWORD ended = WaitForMultipleObjects(2, wait, FALSE, INFINITE);
    bool backendFailed = false;
    if (ended == WAIT_OBJECT_0 + 1)
    {
        backendFailed = true;
        Report(L"后端在 UE 运行期间退出，退出码=" +
            std::to_wstring(session.backend.ExitCode()) +
            L"。遥测已中断，请正常关闭 UE 后查看日志。不会自动重启或终止 UE。");
        WaitForSingleObject(ue.process.value, INFINITE);
    }
    else if (ended != WAIT_OBJECT_0)
        Fail(L"无法监控程序状态：" + WinError());
    Log(L"UE exited, code=" + std::to_wstring(ue.ExitCode()));
    if (ue.ExitCode() != 0)
        Report(L"UE 异常退出，退出码=" + std::to_wstring(ue.ExitCode()) +
            L"。请同时查看 Windows\\DroneShowPlatform1\\Saved\\Logs 中的 UE 日志。");
    const bool backendStopped = session.Stop();
    return backendFailed || ue.ExitCode() != 0 || !backendStopped ? 1 : 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    const bool validArgs = args && (count == 1 || (count == 2 &&
        (std::wstring(args[1]) == L"--check" || std::wstring(args[1]) == L"--no-dialogs")));
    checkOnly = args && count == 2 && std::wstring(args[1]) == L"--check";
    noDialogs = args && count == 2 && std::wstring(args[1]) == L"--no-dialogs";
    if (args) LocalFree(args);
    try
    {
        if (!validArgs) Fail(L"支持双击运行、--check 只检查，或 --no-dialogs 自动化日志模式。");
        return Run();
    }
    catch (const std::exception& error)
    {
        Report(Wide(error.what()));
        return 1;
    }
}
