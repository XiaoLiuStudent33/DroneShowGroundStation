/**
 * @brief Single-owner Console renderer with Dashboard and Linear sinks.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "ConsoleUI.h"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <mutex>
#include <utility>

namespace
{
    constexpr std::size_t DefaultQueueCapacity = 1024;
    constexpr std::size_t LogHistoryCapacity = 256;
    constexpr short MinimumDashboardWidth = 40;
    constexpr short MinimumDashboardHeight = 14;

    struct ConsoleLogRecord
    {
        std::uint64_t sequence = 0;
        std::string timestamp;
        std::string category;
        std::string text;
        ConsoleLogSeverity severity = ConsoleLogSeverity::Info;
    };

    std::string CurrentTimestamp()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char buffer[16]{};
        std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
        return buffer;
    }

    bool IsUtf8Continuation(const unsigned char value)
    {
        return (value & 0xC0U) == 0x80U;
    }

    std::string ClipUtf8(const std::string& value, const std::size_t width)
    {
        if (value.size() <= width) return value;
        if (width == 0) return {};
        if (width <= 3) return std::string(width, '.');

        std::size_t end = width - 3;
        while (end > 0 && end < value.size() &&
               IsUtf8Continuation(static_cast<unsigned char>(value[end])))
        {
            --end;
        }
        return value.substr(0, end) + "...";
    }

    std::string FitLine(const std::string& value, const std::size_t width)
    {
        std::string line = ClipUtf8(value, width);
        if (line.size() < width) line.append(width - line.size(), ' ');
        return line;
    }

    std::wstring ToConsoleWideLine(
        const std::string& utf8,
        const std::size_t width)
    {
        if (width == 0) return {};
        const int required = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
            static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring wide;
        if (required > 0)
        {
            wide.resize(static_cast<std::size_t>(required));
            MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                static_cast<int>(utf8.size()), wide.data(), required);
        }
        else
        {
            // Invalid external bytes remain observable without changing the
            // process code page or allowing an unbounded Console write.
            wide.assign(utf8.begin(), utf8.end());
        }
        if (wide.size() > width) wide.resize(width);
        if (wide.size() < width) wide.append(width - wide.size(), L' ');
        return wide;
    }

    std::vector<std::string> SplitLines(const std::string& text)
    {
        std::vector<std::string> lines;
        std::size_t begin = 0;
        while (begin <= text.size())
        {
            const std::size_t end = text.find('\n', begin);
            std::string line = text.substr(
                begin,
                end == std::string::npos ? std::string::npos : end - begin);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        return lines;
    }

    std::string FormatRecordPrefix(const ConsoleLogRecord& record)
    {
        const char* severity = "INFO";
        if (record.severity == ConsoleLogSeverity::Error) severity = "ERROR";
        if (record.severity == ConsoleLogSeverity::Terminal) severity = "TERMINAL";
        return "[" + record.timestamp + "] [" + record.category + "] [" +
            severity + "] ";
    }

    class Win32ConsoleRenderTarget final : public IConsoleRenderTarget
    {
    public:
        Win32ConsoleRenderTarget()
            : output_(GetStdHandle(STD_OUTPUT_HANDLE))
        {
        }

        ConsoleTargetState Inspect() override
        {
            ConsoleTargetState state;
            if (output_ == nullptr || output_ == INVALID_HANDLE_VALUE) return state;

            const DWORD fileType = GetFileType(output_);
            if (fileType == FILE_TYPE_PIPE)
            {
                state.handleKind = ConsoleOutputHandleKind::Pipe;
                return state;
            }
            if (fileType == FILE_TYPE_DISK)
            {
                state.handleKind = ConsoleOutputHandleKind::Disk;
                return state;
            }

            DWORD mode = 0;
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (fileType == FILE_TYPE_CHAR && GetConsoleMode(output_, &mode) &&
                GetConsoleScreenBufferInfo(output_, &info))
            {
                state.handleKind = ConsoleOutputHandleKind::Console;
                state.geometryValid = true;
                state.width = static_cast<short>(
                    info.srWindow.Right - info.srWindow.Left + 1);
                state.height = static_cast<short>(
                    info.srWindow.Bottom - info.srWindow.Top + 1);
                windowLeft_ = info.srWindow.Left;
                windowTop_ = info.srWindow.Top;
                defaultAttributes_ = info.wAttributes;
            }
            return state;
        }

        bool WriteDashboard(const ConsoleDashboardFrame& lines) override
        {
            const ConsoleTargetState state = Inspect();
            if (!state.geometryValid || state.width <= 0 || state.height <= 0)
                return false;

            if (!cursorHidden_)
            {
                CONSOLE_CURSOR_INFO cursor{};
                if (GetConsoleCursorInfo(output_, &cursor))
                {
                    originalCursor_ = cursor;
                    cursor.bVisible = FALSE;
                    SetConsoleCursorInfo(output_, &cursor);
                    cursorHidden_ = true;
                }
            }

            const short count = (std::min)(
                state.height, static_cast<short>(lines.size()));
            for (short row = 0; row < count; ++row)
            {
                const ConsoleDashboardLine& source =
                    lines[static_cast<std::size_t>(row)];
                const std::string fitted = FitLine(
                    source.text,
                    static_cast<std::size_t>(state.width));
                const std::wstring wide = ToConsoleWideLine(
                    fitted, static_cast<std::size_t>(state.width));
                DWORD written = 0;
                const COORD position = {
                    windowLeft_, static_cast<short>(windowTop_ + row) };
                if (!WriteConsoleOutputCharacterW(
                        output_, wide.data(), state.width, position, &written) ||
                    written != static_cast<DWORD>(state.width))
                {
                    return false;
                }

                const WORD foregroundMask = FOREGROUND_BLUE | FOREGROUND_GREEN |
                    FOREGROUND_RED | FOREGROUND_INTENSITY;
                std::vector<WORD> attributes(
                    static_cast<std::size_t>(state.width), defaultAttributes_);
                const std::size_t styledCount = (std::min)(
                    attributes.size(), source.attributes.size());
                for (std::size_t index = 0; index < styledCount; ++index)
                {
                    const std::uint16_t requested = source.attributes[index];
                    if (requested != ConsoleDefaultAttribute)
                    {
                        attributes[index] = static_cast<WORD>(
                            (defaultAttributes_ & ~foregroundMask) |
                            (requested & foregroundMask));
                    }
                }
                written = 0;
                // Text remains valid if attribute output is unavailable. No
                // producer or fallback writer is allowed to repair the frame.
                const bool attributesWritten = WriteConsoleOutputAttribute(
                    output_, attributes.data(), state.width, position, &written) != FALSE
                    && written == static_cast<DWORD>(state.width);
                if (!attributesWritten)
                {
                    // Best-effort fail-closed presentation: a partial styled
                    // write is overwritten with the captured default attribute.
                    // The same renderer thread remains the only console writer.
                    std::fill(
                        attributes.begin(), attributes.end(), defaultAttributes_);
                    written = 0;
                    WriteConsoleOutputAttribute(
                        output_, attributes.data(), state.width, position, &written);
                }
            }
            return true;
        }

        bool WriteLinear(const std::string& line) override
        {
            if (output_ == nullptr || output_ == INVALID_HANDLE_VALUE) return false;
            std::string complete = line;
            complete.push_back('\n');
            DWORD written = 0;
            return WriteFile(
                       output_, complete.data(),
                       static_cast<DWORD>(complete.size()), &written, nullptr) != FALSE &&
                written == static_cast<DWORD>(complete.size());
        }

        void Restore() override
        {
            if (cursorHidden_ && output_ != nullptr &&
                output_ != INVALID_HANDLE_VALUE)
            {
                SetConsoleCursorInfo(output_, &originalCursor_);
            }
            cursorHidden_ = false;
        }

    private:
        HANDLE output_ = INVALID_HANDLE_VALUE;
        short windowLeft_ = 0;
        short windowTop_ = 0;
        bool cursorHidden_ = false;
        CONSOLE_CURSOR_INFO originalCursor_{ 20, TRUE };
        WORD defaultAttributes_ =
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    };

    class ConsolePresentationController
    {
    public:
        bool ConfigureForTesting(
            std::shared_ptr<IConsoleRenderTarget> target,
            const ConsoleOutputMode mode,
            const std::size_t capacity,
            const std::chrono::milliseconds interval)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_ || !target) return false;
            configuredTarget_ = std::move(target);
            configuredMode_ = mode;
            queueCapacity_ = (std::max)(std::size_t{ 1 }, capacity);
            renderInterval_ = (std::max)(std::chrono::milliseconds(1), interval);
            return true;
        }

        void ResetTestingConfiguration()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_) return;
            configuredTarget_.reset();
            configuredMode_ = ConsoleOutputMode::Auto;
            queueCapacity_ = DefaultQueueCapacity;
            renderInterval_ = std::chrono::milliseconds(50);
            ResetStateLocked();
        }

        void Init()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_) return;

            ResetStateLocked();
            target_ = configuredTarget_ != nullptr
                ? configuredTarget_
                : std::make_shared<Win32ConsoleRenderTarget>();
            const ConsoleTargetState targetState = target_->Inspect();
            activeMode_ = configuredMode_;
            if (activeMode_ == ConsoleOutputMode::Auto)
            {
                activeMode_ = targetState.handleKind ==
                        ConsoleOutputHandleKind::Console && targetState.geometryValid
                    ? ConsoleOutputMode::Dashboard
                    : ConsoleOutputMode::Linear;
            }
            if (activeMode_ == ConsoleOutputMode::Dashboard &&
                (!targetState.geometryValid ||
                 targetState.handleKind != ConsoleOutputHandleKind::Console))
            {
                activeMode_ = ConsoleOutputMode::Linear;
            }

            accepting_ = true;
            running_ = true;
            stopRequested_ = false;
            renderer_ = std::thread([this]() { RenderLoop(); });
        }

        void Shutdown()
        {
            std::thread renderer;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!running_) return;
                accepting_ = false;
                stopRequested_ = true;
                renderer = std::move(renderer_);
            }
            wake_.notify_all();
            if (renderer.joinable()) renderer.join();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
                target_.reset();
            }
            idle_.notify_all();
        }

        void UpdateFleet(const std::vector<ConsoleDroneStatus>& drones)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!accepting_) return;
                fleet_ = drones;
                ++fleetVersion_;
            }
            wake_.notify_one();
        }

        void AddLog(
            std::string category,
            std::string text,
            const ConsoleLogSeverity severity)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!accepting_)
                {
                    ++rejectedAfterShutdownCount_;
                    return;
                }

                ConsoleLogRecord record;
                record.sequence = ++submittedLogCount_;
                record.timestamp = CurrentTimestamp();
                record.category = category.empty() ? "Runtime" : std::move(category);
                record.text = std::move(text);
                record.severity = severity;

                if (pending_.size() >= queueCapacity_)
                {
                    const auto nonCritical = std::find_if(
                        pending_.begin(), pending_.end(),
                        [](const ConsoleLogRecord& queued)
                        {
                            return queued.severity == ConsoleLogSeverity::Info;
                        });
                    if (nonCritical != pending_.end())
                    {
                        pending_.erase(nonCritical);
                        ++droppedLogCount_;
                    }
                    else if (severity == ConsoleLogSeverity::Info)
                    {
                        ++droppedLogCount_;
                        wake_.notify_one();
                        return;
                    }
                    else
                    {
                        pending_.pop_front();
                        ++droppedLogCount_;
                        ++droppedCriticalLogCount_;
                    }
                }
                pending_.push_back(std::move(record));
            }
            wake_.notify_one();
        }

        bool WaitUntilIdle(const std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return idle_.wait_for(lock, timeout, [this]()
            {
                return pending_.empty() &&
                    renderedLogCount_ + droppedLogCount_ >= submittedLogCount_ &&
                    renderedFleetVersion_ >= fleetVersion_;
            });
        }

        ConsoleUIStats GetStats() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ConsoleUIStats stats;
            stats.submittedLogCount = submittedLogCount_;
            stats.renderedLogCount = renderedLogCount_;
            stats.droppedLogCount = droppedLogCount_;
            stats.droppedCriticalLogCount = droppedCriticalLogCount_;
            stats.rejectedAfterShutdownCount = rejectedAfterShutdownCount_;
            stats.pendingLogCount = pending_.size();
            stats.queueCapacity = queueCapacity_;
            stats.activeMode = activeMode_;
            stats.rendererThreadId = rendererThreadId_;
            return stats;
        }

    private:
        void ResetStateLocked()
        {
            pending_.clear();
            history_.clear();
            fleet_.clear();
            submittedLogCount_ = 0;
            renderedLogCount_ = 0;
            droppedLogCount_ = 0;
            droppedCriticalLogCount_ = 0;
            rejectedAfterShutdownCount_ = 0;
            reportedDroppedLogCount_ = 0;
            fleetVersion_ = 0;
            renderedFleetVersion_ = 0;
            rendererThreadId_ = {};
            lastFrame_.clear();
            lastWidth_ = 0;
            lastHeight_ = 0;
        }

        void RenderLoop()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                rendererThreadId_ = std::this_thread::get_id();
            }

            for (;;)
            {
                std::deque<ConsoleLogRecord> records;
                std::vector<ConsoleDroneStatus> fleet;
                std::uint64_t fleetVersion = 0;
                std::uint64_t droppedCount = 0;
                std::uint64_t droppedCriticalCount = 0;
                bool stopping = false;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    wake_.wait_for(lock, renderInterval_, [this]()
                    {
                        return stopRequested_ || !pending_.empty() ||
                            renderedFleetVersion_ < fleetVersion_;
                    });
                    records.swap(pending_);
                    fleet = fleet_;
                    fleetVersion = fleetVersion_;
                    droppedCount = droppedLogCount_;
                    droppedCriticalCount = droppedCriticalLogCount_;
                    stopping = stopRequested_;
                }

                if (droppedCount > reportedDroppedLogCount_)
                {
                    ConsoleLogRecord warning;
                    warning.timestamp = CurrentTimestamp();
                    warning.category = "Console";
                    warning.severity = ConsoleLogSeverity::Error;
                    warning.text = "DroppedLogCount=" + std::to_string(droppedCount) +
                        " (bounded queue overflow; critical drops=" +
                        std::to_string(droppedCriticalCount) + ")";
                    records.push_front(std::move(warning));
                    reportedDroppedLogCount_ = droppedCount;
                }

                if (activeMode_ == ConsoleOutputMode::Linear)
                    RenderLinear(records);
                else
                {
                    AppendHistory(records);
                    RenderDashboard(fleet);
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    renderedLogCount_ += static_cast<std::uint64_t>(
                        std::count_if(records.begin(), records.end(),
                            [](const ConsoleLogRecord& record)
                            {
                                return record.sequence != 0;
                            }));
                    renderedFleetVersion_ = fleetVersion;
                }
                idle_.notify_all();

                if (stopping)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (pending_.empty()) break;
                }
            }

            if (target_) target_->Restore();
        }

        void RenderLinear(const std::deque<ConsoleLogRecord>& records)
        {
            for (const ConsoleLogRecord& record : records)
            {
                const std::string prefix = FormatRecordPrefix(record);
                for (const std::string& line : SplitLines(record.text))
                    target_->WriteLinear(prefix + line);
            }
        }

        void AppendHistory(const std::deque<ConsoleLogRecord>& records)
        {
            for (const ConsoleLogRecord& record : records)
            {
                const std::string prefix = FormatRecordPrefix(record);
                for (const std::string& line : SplitLines(record.text))
                {
                    history_.push_back(prefix + line);
                    if (history_.size() > LogHistoryCapacity) history_.pop_front();
                }
            }
        }

        void RenderDashboard(const std::vector<ConsoleDroneStatus>& fleet)
        {
            const ConsoleTargetState state = target_->Inspect();
            if (!state.geometryValid || state.width <= 0 || state.height <= 0) return;

            const short width = state.width;
            const short height = state.height;
            ConsoleDashboardFrame frame(static_cast<std::size_t>(height));
            for (ConsoleDashboardLine& line : frame)
            {
                line.text.assign(static_cast<std::size_t>(width), ' ');
                line.attributes.assign(
                    static_cast<std::size_t>(width), ConsoleDefaultAttribute);
            }
            const auto put = [&frame, width, height](
                const short row, const std::string& text)
            {
                if (row < 0 || row >= height) return;
                ConsoleDashboardLine& line = frame[static_cast<std::size_t>(row)];
                line.text = FitLine(
                    text, static_cast<std::size_t>(width));
                std::fill(
                    line.attributes.begin(), line.attributes.end(),
                    ConsoleDefaultAttribute);
            };
            const auto style = [&frame, width, height](
                const short row,
                const std::size_t begin,
                const std::size_t count,
                const std::uint16_t attribute)
            {
                if (row < 0 || row >= height || begin >=
                    static_cast<std::size_t>(width)) return;
                ConsoleDashboardLine& line = frame[static_cast<std::size_t>(row)];
                const std::size_t end = (std::min)(
                    line.attributes.size(), begin + count);
                std::fill(
                    line.attributes.begin() + begin,
                    line.attributes.begin() + end,
                    attribute);
            };

            const std::size_t online = static_cast<std::size_t>(std::count_if(
                fleet.begin(), fleet.end(), [](const ConsoleDroneStatus& drone)
                {
                    return drone.online;
                }));

            if (width < MinimumDashboardWidth || height < MinimumDashboardHeight)
            {
                put(0, "COMPACT | Drone GCS dashboard");
                put(1, "Total=" + std::to_string(fleet.size()) +
                    " Online=" + std::to_string(online) +
                    " Offline=" + std::to_string(fleet.size() - online));
                const std::string totalValue = std::to_string(fleet.size());
                const std::string onlineValue = std::to_string(online);
                const std::string offlineValue =
                    std::to_string(fleet.size() - online);
                style(1, 6, totalValue.size(), ConsoleBrightBlueAttribute);
                const std::size_t onlineBegin = 6 + totalValue.size() + 8;
                style(1, onlineBegin, onlineValue.size(),
                    ConsoleBrightGreenAttribute);
                const std::size_t offlineBegin =
                    onlineBegin + onlineValue.size() + 9;
                style(1, offlineBegin, offlineValue.size(),
                    ConsoleBrightRedAttribute);
                const short logRows = (std::max)(short{ 0 },
                    static_cast<short>(height - 3));
                const std::size_t first = history_.size() >
                        static_cast<std::size_t>(logRows)
                    ? history_.size() - static_cast<std::size_t>(logRows)
                    : 0;
                short row = 3;
                for (std::size_t index = first;
                     index < history_.size() && row < height; ++index, ++row)
                    put(row, history_[index]);
            }
            else
            {
                put(0, "Drone GCS - Backend Server");
                put(1, std::string(static_cast<std::size_t>(width), '='));
                put(3, "Fleet Status:");
                put(4, "Total Drone : " + std::to_string(fleet.size()));
                put(5, "Online      : " + std::to_string(online));
                put(6, "Offline     : " + std::to_string(fleet.size() - online));
                style(4, 14, std::to_string(fleet.size()).size(),
                    ConsoleBrightBlueAttribute);
                style(5, 14, std::to_string(online).size(),
                    ConsoleBrightGreenAttribute);
                style(6, 14, std::to_string(fleet.size() - online).size(),
                    ConsoleBrightRedAttribute);
                put(8, "Drone List:");
                put(9, std::string(static_cast<std::size_t>(width), '-'));

                const short logTitle = (std::max)(short{ 12 },
                    static_cast<short>((height * 2) / 3));
                const short droneStart = 10;
                const short droneEnd = static_cast<short>(logTitle - 2);
                const std::size_t droneCapacity = droneEnd >= droneStart
                    ? static_cast<std::size_t>(droneEnd - droneStart + 1)
                    : 0;
                const bool overflow = fleet.size() > droneCapacity;
                const std::size_t visible = overflow && droneCapacity > 0
                    ? droneCapacity - 1
                    : (std::min)(fleet.size(), droneCapacity);
                for (std::size_t index = 0; index < visible; ++index)
                {
                    const std::string status =
                        fleet[index].online ? "ONLINE" : "OFFLINE";
                    const short targetRow =
                        static_cast<short>(droneStart + index);
                    put(targetRow, fleet[index].droneId + "  " + status);
                    style(targetRow, fleet[index].droneId.size() + 2,
                        status.size(), fleet[index].online
                            ? ConsoleBrightGreenAttribute
                            : ConsoleBrightRedAttribute);
                }
                if (overflow && droneCapacity > 0)
                {
                    put(droneEnd, "... " +
                        std::to_string(fleet.size() - visible) + " more drones");
                }

                put(logTitle, std::string(static_cast<std::size_t>(width), '='));
                put(static_cast<short>(logTitle + 1), "Runtime Log:");
                const short separator = static_cast<short>(logTitle + 2);
                put(separator, std::string(static_cast<std::size_t>(width), '-'));
                const short logStart = static_cast<short>(separator + 1);
                const short logRows = (std::max)(short{ 0 },
                    static_cast<short>(height - logStart));
                const std::size_t first = history_.size() >
                        static_cast<std::size_t>(logRows)
                    ? history_.size() - static_cast<std::size_t>(logRows)
                    : 0;
                short row = logStart;
                for (std::size_t index = first;
                     index < history_.size() && row < height; ++index, ++row)
                    put(row, history_[index]);
            }

            if (frame != lastFrame_ || width != lastWidth_ || height != lastHeight_)
            {
                target_->WriteDashboard(frame);
                lastFrame_ = std::move(frame);
                lastWidth_ = width;
                lastHeight_ = height;
            }
        }

        mutable std::mutex mutex_;
        std::condition_variable wake_;
        std::condition_variable idle_;
        std::deque<ConsoleLogRecord> pending_;
        std::deque<std::string> history_;
        std::vector<ConsoleDroneStatus> fleet_;
        std::shared_ptr<IConsoleRenderTarget> configuredTarget_;
        std::shared_ptr<IConsoleRenderTarget> target_;
        ConsoleOutputMode configuredMode_ = ConsoleOutputMode::Auto;
        ConsoleOutputMode activeMode_ = ConsoleOutputMode::Linear;
        std::size_t queueCapacity_ = DefaultQueueCapacity;
        std::chrono::milliseconds renderInterval_{ 50 };
        std::thread renderer_;
        bool running_ = false;
        bool accepting_ = false;
        bool stopRequested_ = false;
        std::uint64_t submittedLogCount_ = 0;
        std::uint64_t renderedLogCount_ = 0;
        std::uint64_t droppedLogCount_ = 0;
        std::uint64_t droppedCriticalLogCount_ = 0;
        std::uint64_t rejectedAfterShutdownCount_ = 0;
        std::uint64_t reportedDroppedLogCount_ = 0;
        std::uint64_t fleetVersion_ = 0;
        std::uint64_t renderedFleetVersion_ = 0;
        std::thread::id rendererThreadId_;
        ConsoleDashboardFrame lastFrame_;
        short lastWidth_ = 0;
        short lastHeight_ = 0;
    };

    ConsolePresentationController& Controller()
    {
        static ConsolePresentationController controller;
        return controller;
    }
}

void ConsoleUI::Init() { Controller().Init(); }
void ConsoleUI::Shutdown() { Controller().Shutdown(); }

void ConsoleUI::UpdateFleet(const std::vector<ConsoleDroneStatus>& drones)
{
    Controller().UpdateFleet(drones);
}

void ConsoleUI::Update(const FlightData& data, const HoverTest& test)
{
    Update("Drone", data, test);
}

void ConsoleUI::Update(
    const std::string& droneId,
    const FlightData& data,
    const HoverTest& /*test*/)
{
    UpdateFleet({ ConsoleDroneStatus{
        droneId.empty() ? "Drone" : droneId, data.online } });
}

void ConsoleUI::AddLog(const std::string& text)
{
    AddLog("Runtime", text, ConsoleLogSeverity::Info);
}

void ConsoleUI::AddLog(
    const std::string& category,
    const std::string& text,
    const ConsoleLogSeverity severity)
{
    Controller().AddLog(category, text, severity);
}

void ConsoleUI::AddErrorLog(const std::string& text)
{
    AddLog("Runtime", text, ConsoleLogSeverity::Error);
}

void ConsoleUI::AddTerminalLog(const std::string& text)
{
    AddLog("Job", text, ConsoleLogSeverity::Terminal);
}

bool ConsoleUI::ConfigureForTesting(
    std::shared_ptr<IConsoleRenderTarget> target,
    const ConsoleOutputMode mode,
    const std::size_t queueCapacity,
    const std::chrono::milliseconds renderInterval)
{
    return Controller().ConfigureForTesting(
        std::move(target), mode, queueCapacity, renderInterval);
}

bool ConsoleUI::WaitUntilIdle(const std::chrono::milliseconds timeout)
{
    return Controller().WaitUntilIdle(timeout);
}

ConsoleUIStats ConsoleUI::GetStats() { return Controller().GetStats(); }
void ConsoleUI::ResetTestingConfiguration()
{
    Controller().ResetTestingConfiguration();
}
