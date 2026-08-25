#include "ConsoleUI.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <iostream>

namespace
{
    using namespace std::chrono_literals;

    void Expect(const bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    class FakeConsoleTarget final : public IConsoleRenderTarget
    {
    public:
        explicit FakeConsoleTarget(
            const ConsoleOutputHandleKind kind = ConsoleOutputHandleKind::Console,
            const short width = 80,
            const short height = 30)
            : state_{ kind, kind == ConsoleOutputHandleKind::Console,
                width, height }
        {
        }

        ConsoleTargetState Inspect() override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return state_;
        }

        bool WriteDashboard(const ConsoleDashboardFrame& lines) override
        {
            RecordWriter();
            Delay();
            std::lock_guard<std::mutex> lock(mutex_);
            frames_.push_back(lines);
            return true;
        }

        bool WriteLinear(const std::string& line) override
        {
            RecordWriter();
            Delay();
            std::lock_guard<std::mutex> lock(mutex_);
            linear_.push_back(line);
            return true;
        }

        void Restore() override
        {
            RecordWriter();
            restored_.store(true);
        }

        void Resize(const short width, const short height)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.width = width;
            state_.height = height;
            state_.geometryValid = width > 0 && height > 0;
        }

        void SetWriteDelay(const std::chrono::milliseconds delay)
        {
            delayMs_.store(static_cast<int>(delay.count()));
        }

        std::vector<ConsoleDashboardFrame> Frames() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return frames_;
        }

        std::vector<std::string> LinearLines() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return linear_;
        }

        std::set<std::thread::id> WriterThreads() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return writerThreads_;
        }

        bool Restored() const { return restored_.load(); }

    private:
        void RecordWriter()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            writerThreads_.insert(std::this_thread::get_id());
        }

        void Delay() const
        {
            const int delay = delayMs_.load();
            if (delay > 0) std::this_thread::sleep_for(
                std::chrono::milliseconds(delay));
        }

        mutable std::mutex mutex_;
        ConsoleTargetState state_;
        std::vector<ConsoleDashboardFrame> frames_;
        std::vector<std::string> linear_;
        std::set<std::thread::id> writerThreads_;
        std::atomic<int> delayMs_{ 0 };
        std::atomic<bool> restored_{ false };
    };

    void Start(
        const std::shared_ptr<FakeConsoleTarget>& target,
        const ConsoleOutputMode mode,
        const std::size_t capacity = 1024)
    {
        ConsoleUI::Shutdown();
        ConsoleUI::ResetTestingConfiguration();
        Expect(ConsoleUI::ConfigureForTesting(
            target, mode, capacity, 2ms), "test target configuration failed");
        ConsoleUI::Init();
    }

    bool Contains(
        const std::vector<std::string>& lines,
        const std::string& needle)
    {
        return std::any_of(lines.begin(), lines.end(),
            [&needle](const std::string& line)
            {
                return line.find(needle) != std::string::npos;
            });
    }

    bool Contains(
        const ConsoleDashboardFrame& lines,
        const std::string& needle)
    {
        return std::any_of(lines.begin(), lines.end(),
            [&needle](const ConsoleDashboardLine& line)
            {
                return line.text.find(needle) != std::string::npos;
            });
    }

    std::size_t CountContains(
        const std::vector<std::string>& lines,
        const std::string& needle)
    {
        return static_cast<std::size_t>(std::count_if(
            lines.begin(), lines.end(), [&needle](const std::string& line)
            {
                return line.find(needle) != std::string::npos;
            }));
    }

    std::size_t CountContains(
        const ConsoleDashboardFrame& lines,
        const std::string& needle)
    {
        return static_cast<std::size_t>(std::count_if(
            lines.begin(), lines.end(), [&needle](const ConsoleDashboardLine& line)
            {
                return line.text.find(needle) != std::string::npos;
            }));
    }

    void VerifyModeDetection()
    {
        for (const auto expected : {
                 std::pair{ ConsoleOutputHandleKind::Console,
                     ConsoleOutputMode::Dashboard },
                 std::pair{ ConsoleOutputHandleKind::Pipe,
                     ConsoleOutputMode::Linear },
                 std::pair{ ConsoleOutputHandleKind::Disk,
                     ConsoleOutputMode::Linear },
                 std::pair{ ConsoleOutputHandleKind::Invalid,
                     ConsoleOutputMode::Linear } })
        {
            auto target = std::make_shared<FakeConsoleTarget>(expected.first);
            Start(target, ConsoleOutputMode::Auto);
            Expect(ConsoleUI::GetStats().activeMode == expected.second,
                "auto mode did not follow handle type and Console geometry");
            ConsoleUI::Shutdown();
        }

        auto pipe = std::make_shared<FakeConsoleTarget>(
            ConsoleOutputHandleKind::Pipe);
        Start(pipe, ConsoleOutputMode::Dashboard);
        Expect(ConsoleUI::GetStats().activeMode == ConsoleOutputMode::Linear,
            "Dashboard override attempted cursor output on a pipe");
        ConsoleUI::Shutdown();
    }

    void VerifyLinearIntegrity()
    {
        auto target = std::make_shared<FakeConsoleTarget>(
            ConsoleOutputHandleKind::Pipe);
        Start(target, ConsoleOutputMode::Auto);
        const std::string longMessage =
            "ArtifactDigest=" + std::string(1000, 'a');
        ConsoleUI::AddLog("Artifact", longMessage);
        ConsoleUI::AddTerminalLog(
            "JobState=Succeeded LogicalBytes=345800\nVerification=PASS");
        Expect(ConsoleUI::WaitUntilIdle(2s), "linear renderer did not drain");
        ConsoleUI::Shutdown();

        const auto lines = target->LinearLines();
        Expect(Contains(lines, longMessage),
            "Linear sink truncated the original long message");
        Expect(Contains(lines, "JobState=Succeeded LogicalBytes=345800"),
            "Linear sink lost terminal summary");
        Expect(Contains(lines, "Verification=PASS"),
            "multiline record did not remain complete linear lines");
        for (const std::string& line : lines)
        {
            Expect(line.find("\x1b") == std::string::npos,
                "Linear sink emitted an ANSI control sequence");
        }
    }

    void VerifyDashboardLayoutAndReplacement()
    {
        auto target = std::make_shared<FakeConsoleTarget>(
            ConsoleOutputHandleKind::Console, 72, 30);
        Start(target, ConsoleOutputMode::Dashboard);
        ConsoleUI::UpdateFleet({ ConsoleDroneStatus{ "Drone001-LONG-TAIL", true } });
        ConsoleUI::AddLog("RouteUpload", "RoutePath=" + std::string(300, 'R'));
        ConsoleUI::AddTerminalLog(
            "Job terminal summary Items=9100 Segments=3 State=Succeeded");
        Expect(ConsoleUI::WaitUntilIdle(2s), "Dashboard renderer did not drain");

        ConsoleUI::UpdateFleet({ ConsoleDroneStatus{ "Drone001", true } });
        Expect(ConsoleUI::WaitUntilIdle(2s), "short replacement did not render");
        ConsoleUI::Shutdown();

        const auto frames = target->Frames();
        Expect(!frames.empty(), "Dashboard produced no frame");
        const auto& finalFrame = frames.back();
        Expect(finalFrame.size() == 30, "Dashboard height did not match viewport");
        for (const ConsoleDashboardLine& line : finalFrame)
        {
            Expect(line.text.size() == 72,
                "Dashboard wrote beyond row boundary");
            Expect(line.attributes.size() == 72,
                "Dashboard attributes exceeded the row boundary");
        }
        Expect(Contains(finalFrame, "Total Drone : 1"),
            "Fleet total was not on its owned row");
        Expect(CountContains(finalFrame, "Drone001") == 1,
            "Drone row was duplicated outside Drone List");
        Expect(!Contains(finalFrame, "LONG-TAIL"),
            "short row left stale characters from the old long row");
        Expect(Contains(finalFrame, "Job terminal summary"),
            "terminal summary was not readable in Runtime Log");
    }

    void VerifyDashboardStatusColors()
    {
        auto target = std::make_shared<FakeConsoleTarget>(
            ConsoleOutputHandleKind::Console, 80, 30);
        Start(target, ConsoleOutputMode::Dashboard);
        ConsoleUI::UpdateFleet({
            ConsoleDroneStatus{ "Drone001", true },
            ConsoleDroneStatus{ "Drone002", false } });
        Expect(ConsoleUI::WaitUntilIdle(2s), "colored Dashboard did not render");

        auto frames = target->Frames();
        Expect(!frames.empty(), "colored Dashboard produced no frame");
        const ConsoleDashboardFrame& initial = frames.back();
        Expect(initial[4].attributes[14] == ConsoleBrightBlueAttribute,
            "Total count was not bright blue");
        Expect(initial[5].attributes[14] == ConsoleBrightGreenAttribute,
            "Online count was not bright green");
        Expect(initial[6].attributes[14] == ConsoleBrightRedAttribute,
            "Offline count was not bright red");
        for (std::size_t index = 0; index < 8; ++index)
        {
            Expect(initial[10].attributes[index] == ConsoleDefaultAttribute &&
                   initial[11].attributes[index] == ConsoleDefaultAttribute,
                "DroneID inherited a status color");
        }
        for (std::size_t index = 10; index < 16; ++index)
            Expect(initial[10].attributes[index] == ConsoleBrightGreenAttribute,
                "ONLINE token was not entirely green");
        for (std::size_t index = 10; index < 17; ++index)
            Expect(initial[11].attributes[index] == ConsoleBrightRedAttribute,
                "OFFLINE token was not entirely red");
        Expect(initial[10].attributes[16] == ConsoleDefaultAttribute &&
               initial[11].attributes[17] == ConsoleDefaultAttribute,
            "status trailing whitespace retained a color");

        ConsoleUI::UpdateFleet({
            ConsoleDroneStatus{ "Drone001", false },
            ConsoleDroneStatus{ "Drone002", true } });
        Expect(ConsoleUI::WaitUntilIdle(2s), "status color transition did not render");
        ConsoleUI::Shutdown();
        frames = target->Frames();
        const ConsoleDashboardFrame& transitioned = frames.back();
        for (std::size_t index = 10; index < 17; ++index)
            Expect(transitioned[10].attributes[index] ==
                    ConsoleBrightRedAttribute,
                "Online to Offline left the old green attribute");
        for (std::size_t index = 10; index < 16; ++index)
            Expect(transitioned[11].attributes[index] ==
                    ConsoleBrightGreenAttribute,
                "Offline to Online left the old red attribute");
        Expect(transitioned[10].attributes[17] == ConsoleDefaultAttribute &&
               transitioned[11].attributes[16] == ConsoleDefaultAttribute,
            "status colors leaked into another cell or row");
    }

    void VerifyResize()
    {
        auto target = std::make_shared<FakeConsoleTarget>(
            ConsoleOutputHandleKind::Console, 90, 36);
        Start(target, ConsoleOutputMode::Dashboard);
        ConsoleUI::UpdateFleet({ ConsoleDroneStatus{ "Drone001", true } });
        ConsoleUI::AddLog("Runtime", "wide frame");
        Expect(ConsoleUI::WaitUntilIdle(2s), "initial resize fixture failed");

        target->Resize(48, 18);
        ConsoleUI::AddLog("Runtime", "narrow frame");
        Expect(ConsoleUI::WaitUntilIdle(2s), "narrow resize failed");
        std::this_thread::sleep_for(10ms);
        auto frames = target->Frames();
        Expect(!frames.empty() && frames.back().size() == 18 &&
               frames.back().front().text.size() == 48 &&
               frames.back().front().attributes.size() == 48,
            "renderer retained stale wide geometry");

        target->Resize(20, 8);
        ConsoleUI::AddLog("Runtime", "compact frame");
        Expect(ConsoleUI::WaitUntilIdle(2s), "compact resize failed");
        std::this_thread::sleep_for(10ms);
        frames = target->Frames();
        Expect(frames.back().size() == 8 &&
               frames.back().front().text.size() == 20 &&
               frames.back().front().attributes.size() == 20,
            "small viewport did not use bounded compact frame");
        Expect(Contains(frames.back(), "COMPACT"),
            "small viewport did not identify compact dashboard");

        target->Resize(100, 40);
        ConsoleUI::AddLog("Runtime", "restored frame");
        Expect(ConsoleUI::WaitUntilIdle(2s), "expanded resize failed");
        std::this_thread::sleep_for(10ms);
        frames = target->Frames();
        Expect(frames.back().size() == 40 &&
               frames.back().front().text.size() == 100 &&
               frames.back().front().attributes.size() == 100,
            "renderer did not recover expanded geometry");
        ConsoleUI::Shutdown();
    }

    void VerifySingleWriterAndProducerOrder()
    {
        auto target = std::make_shared<FakeConsoleTarget>(
            ConsoleOutputHandleKind::Pipe);
        Start(target, ConsoleOutputMode::Linear, 4096);

        std::vector<std::thread> producers;
        for (int producer = 0; producer < 5; ++producer)
        {
            producers.emplace_back([producer]()
            {
                for (int index = 0; index < 100; ++index)
                {
                    ConsoleUI::AddLog(
                        "P" + std::to_string(producer),
                        "record=" + std::to_string(index));
                    if (index % 20 == 0)
                        ConsoleUI::UpdateFleet({ ConsoleDroneStatus{
                            "Drone" + std::to_string(producer), true } });
                }
            });
        }
        for (std::thread& producer : producers) producer.join();
        Expect(ConsoleUI::WaitUntilIdle(5s), "concurrent queue did not drain");
        const ConsoleUIStats stats = ConsoleUI::GetStats();
        ConsoleUI::Shutdown();

        Expect(stats.renderedLogCount == 500 && stats.droppedLogCount == 0,
            "concurrent records were lost without queue pressure");
        Expect(target->WriterThreads().size() == 1,
            "physical target had more than one writer thread");
        const auto lines = target->LinearLines();
        for (int producer = 0; producer < 5; ++producer)
        {
            int next = 0;
            const std::string category = "[P" + std::to_string(producer) + "]";
            for (const std::string& line : lines)
            {
                if (line.find(category) == std::string::npos) continue;
                Expect(line.find("record=" + std::to_string(next)) !=
                        std::string::npos,
                    "same-producer log order changed");
                ++next;
            }
            Expect(next == 100, "producer records were incomplete");
        }
    }

    void VerifyBoundedQueueAndCriticalRetention()
    {
        auto target = std::make_shared<FakeConsoleTarget>(
            ConsoleOutputHandleKind::Pipe);
        target->SetWriteDelay(3ms);
        Start(target, ConsoleOutputMode::Linear, 8);
        for (int index = 0; index < 1000; ++index)
            ConsoleUI::AddLog("Stress", "noise=" + std::to_string(index));
        ConsoleUI::AddTerminalLog("CRITICAL_JOB_TERMINAL State=Failed");
        Expect(ConsoleUI::WaitUntilIdle(10s), "bounded queue did not drain");
        const ConsoleUIStats stats = ConsoleUI::GetStats();
        ConsoleUI::Shutdown();

        Expect(stats.queueCapacity == 8 && stats.pendingLogCount == 0,
            "bounded queue capacity contract changed");
        Expect(stats.droppedLogCount > 0,
            "queue pressure did not expose DroppedLogCount");
        const auto lines = target->LinearLines();
        Expect(Contains(lines, "DroppedLogCount="),
            "drop telemetry was not observable in physical output");
        Expect(Contains(lines, "CRITICAL_JOB_TERMINAL"),
            "terminal record was silently lost behind noncritical logs");
    }

    void VerifyShutdownWithActiveProducer()
    {
        auto target = std::make_shared<FakeConsoleTarget>(
            ConsoleOutputHandleKind::Pipe);
        Start(target, ConsoleOutputMode::Linear, 128);
        std::atomic<bool> produce{ true };
        std::thread producer([&produce]()
        {
            while (produce.load()) ConsoleUI::AddLog("Shutdown", "active");
        });
        std::this_thread::sleep_for(10ms);
        const auto started = std::chrono::steady_clock::now();
        ConsoleUI::Shutdown();
        produce.store(false);
        producer.join();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        Expect(elapsed < 2s, "shutdown flush was not bounded");
        Expect(target->Restored(), "renderer did not restore target on shutdown");
        Expect(target->WriterThreads().size() == 1,
            "shutdown introduced a second physical writer");
    }

    void VerifyProductionStyleFrame()
    {
        auto target = std::make_shared<FakeConsoleTarget>(
            ConsoleOutputHandleKind::Console, 110, 36);
        Start(target, ConsoleOutputMode::Dashboard);
        for (int refresh = 0; refresh < 20; ++refresh)
            ConsoleUI::UpdateFleet({ ConsoleDroneStatus{ "Drone001", true } });
        ConsoleUI::AddLog("RouteUpload", "Items=9100 Segment=1/3 LogicalBytes=115200");
        ConsoleUI::AddLog("MAVFTP", "Segment=2/3 WriteWindow=16 ACK=committed");
        ConsoleUI::AddLog("MAVFTP", "Segment=3/3 Verification Count/Home/First/Last=PASS");
        ConsoleUI::AddTerminalLog(
            "Job terminal summary Targets=1 Succeeded=1 Failed=0 Cancelled=0 LogicalBytes=345800");
        Expect(ConsoleUI::WaitUntilIdle(2s), "production frame did not render");
        ConsoleUI::Shutdown();

        const auto frames = target->Frames();
        Expect(!frames.empty(), "production fixture produced no frame");
        const auto& frame = frames.back();
        Expect(Contains(frame, "Total Drone : 1") &&
               Contains(frame, "Online      : 1") &&
               CountContains(frame, "Drone001") == 1,
            "production Fleet layout was inconsistent");
        Expect(Contains(frame, "Job terminal summary"),
            "production terminal summary was obscured");
    }
}

int RunConsoleRenderTests()
{
    try
    {
        VerifyModeDetection();
        VerifyLinearIntegrity();
        VerifyDashboardLayoutAndReplacement();
        VerifyDashboardStatusColors();
        VerifyResize();
        VerifySingleWriterAndProducerOrder();
        VerifyBoundedQueueAndCriticalRetention();
        VerifyShutdownWithActiveProducer();
        VerifyProductionStyleFrame();
        ConsoleUI::Shutdown();
        ConsoleUI::ResetTestingConfiguration();
        std::cout << "Console render integrity tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        ConsoleUI::Shutdown();
        ConsoleUI::ResetTestingConfiguration();
        std::cerr << "Console render integrity tests failed: "
            << error.what() << '\n';
        return 1;
    }
}
