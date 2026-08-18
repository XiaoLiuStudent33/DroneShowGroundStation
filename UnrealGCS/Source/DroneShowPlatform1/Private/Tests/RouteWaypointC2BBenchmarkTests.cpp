#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RouteArtifactWorkspaceManager.h"
#include "RouteMissionCodec.h"
#include "RoutePreviewNpzLoader.h"
#include "RouteWaypointBatchGenerator.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace RouteWaypointC2BBenchmarkPrivate
{
	struct FProcessSample
	{
		uint64 Cpu100ns = 0;
		uint64 WorkingSetBytes = 0;
		uint64 PrivateBytes = 0;
		uint32 HandleCount = 0;
		double WallSeconds = 0.0;
	};

	uint64 FileTimeValue(const FILETIME& Value)
	{
		ULARGE_INTEGER Result;
		Result.LowPart = Value.dwLowDateTime;
		Result.HighPart = Value.dwHighDateTime;
		return Result.QuadPart;
	}

	FProcessSample CaptureProcessSample()
	{
		FProcessSample Result;
		Result.WallSeconds = FPlatformTime::Seconds();
		const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
		Result.WorkingSetBytes = Memory.UsedPhysical;
		SIZE_T PrivateBytes = 0;
		if (FPlatformProcess::GetApplicationMemoryUsage(
			FPlatformProcess::GetCurrentProcessId(), &PrivateBytes))
		{
			Result.PrivateBytes = static_cast<uint64>(PrivateBytes);
		}
#if PLATFORM_WINDOWS
		FILETIME Creation;
		FILETIME Exit;
		FILETIME Kernel;
		FILETIME User;
		if (::GetProcessTimes(::GetCurrentProcess(), &Creation, &Exit, &Kernel, &User))
		{
			Result.Cpu100ns = FileTimeValue(Kernel) + FileTimeValue(User);
		}
		DWORD Handles = 0;
		if (::GetProcessHandleCount(::GetCurrentProcess(), &Handles))
		{
			Result.HandleCount = Handles;
		}
#endif
		return Result;
	}

	double AverageCpuPercent(const FProcessSample& Begin, const FProcessSample& End)
	{
		const double Wall = End.WallSeconds - Begin.WallSeconds;
		if (Wall <= 0.0 || End.Cpu100ns < Begin.Cpu100ns)
		{
			return 0.0;
		}
		const double CpuSeconds = static_cast<double>(End.Cpu100ns - Begin.Cpu100ns) / 1.0e7;
		const int32 LogicalCores = FMath::Max(
			1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
		return 100.0 * CpuSeconds / (Wall * LogicalCores);
	}

	class FProcessMonitor final
	{
	public:
		void Start()
		{
			bStop.Store(false);
			Future = Async(EAsyncExecution::Thread, [this]()
			{
				FProcessSample Previous = CaptureProcessSample();
				{
					FScopeLock Lock(&Mutex);
					PeakWorkingSetBytes = Previous.WorkingSetBytes;
					PeakPrivateBytes = Previous.PrivateBytes;
					PeakHandleCount = Previous.HandleCount;
				}
				for (;;)
				{
					FPlatformProcess::SleepNoStats(0.05f);
					const FProcessSample Current = CaptureProcessSample();
					const double Cpu = AverageCpuPercent(Previous, Current);
					{
						FScopeLock Lock(&Mutex);
						PeakCpuPercent = FMath::Max(PeakCpuPercent, Cpu);
						PeakWorkingSetBytes = FMath::Max(
							PeakWorkingSetBytes, Current.WorkingSetBytes);
						PeakPrivateBytes = FMath::Max(
							PeakPrivateBytes, Current.PrivateBytes);
						PeakHandleCount = FMath::Max(
							PeakHandleCount, Current.HandleCount);
					}
					Previous = Current;
					if (bStop.Load())
					{
						break;
					}
				}
			});
		}

		void Stop()
		{
			bStop.Store(true);
			if (Future.IsValid())
			{
				Future.Wait();
			}
		}

		void Read(double& OutPeakCpu, uint64& OutPeakWorkingSet,
			uint64& OutPeakPrivate, uint32& OutPeakHandles) const
		{
			FScopeLock Lock(&Mutex);
			OutPeakCpu = PeakCpuPercent;
			OutPeakWorkingSet = PeakWorkingSetBytes;
			OutPeakPrivate = PeakPrivateBytes;
			OutPeakHandles = PeakHandleCount;
		}

	private:
		mutable FCriticalSection Mutex;
		TAtomic<bool> bStop{false};
		TFuture<void> Future;
		double PeakCpuPercent = 0.0;
		uint64 PeakWorkingSetBytes = 0;
		uint64 PeakPrivateBytes = 0;
		uint32 PeakHandleCount = 0;
	};

	class FBackgroundSentinel final
	{
	public:
		void Start()
		{
			bStop.Store(false);
			Future = Async(EAsyncExecution::Thread, [this]()
			{
				while (!bStop.Load())
				{
					const double StartSeconds = FPlatformTime::Seconds();
					TFuture<void> Sentinel = Async(EAsyncExecution::ThreadPool, []() {});
					Sentinel.Wait();
					const double Latency = FPlatformTime::Seconds() - StartSeconds;
					{
						FScopeLock Lock(&Mutex);
						Latencies.Add(Latency);
					}
					FPlatformProcess::SleepNoStats(0.05f);
				}
			});
		}

		void Stop(double& OutAverageMs, double& OutP95Ms, double& OutMaxMs)
		{
			bStop.Store(true);
			if (Future.IsValid())
			{
				Future.Wait();
			}
			TArray<double> Copy;
			{
				FScopeLock Lock(&Mutex);
				Copy = Latencies;
			}
			Copy.Sort();
			double Sum = 0.0;
			for (const double Value : Copy) Sum += Value;
			OutAverageMs = Copy.IsEmpty() ? 0.0 : 1000.0 * Sum / Copy.Num();
			OutP95Ms = Copy.IsEmpty() ? 0.0
				: 1000.0 * Copy[FMath::Clamp(
					FMath::CeilToInt(Copy.Num() * 0.95) - 1, 0, Copy.Num() - 1)];
			OutMaxMs = Copy.IsEmpty() ? 0.0 : 1000.0 * Copy.Last();
		}

	private:
		FCriticalSection Mutex;
		TAtomic<bool> bStop{false};
		TFuture<void> Future;
		TArray<double> Latencies;
	};

	class FDiscardSink final : public IRouteWaypointByteSink
	{
	public:
		bool Write(const uint8*, int32, FString& OutError) override
		{
			OutError.Reset();
			return true;
		}
		bool Finalize(FString& OutError) override
		{
			OutError.Reset();
			return true;
		}
	};

	FRouteArtifactDigest256 HashLabel(const FString& Label)
	{
		FTCHARToUTF8 Utf8(*Label);
		FRouteArtifactDigest256 Digest;
		FString Error;
		FRouteArtifactSha256::HashBytes(
			Utf8.Get(), static_cast<uint64>(Utf8.Length()), Digest, Error);
		return Digest;
	}

	FRouteWaypointGenerationSnapshot MakeSnapshot(
		const TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe>& Data,
		FString& OutError)
	{
		FRouteWaypointGenerationSnapshot Snapshot;
		Snapshot.RouteData = Data;
		Snapshot.SourceIdentity = Data->SourceIdentity;
		Snapshot.RouteDatasetGeneration = 1;
		FRouteWaypointGeneratorError GeneratorError;
		if (!RouteWaypointGenerator::BuildContext(
			31.932471, 118.638531, Snapshot.GeneratorContext, GeneratorError))
		{
			OutError = GeneratorError.Diagnostic;
			return FRouteWaypointGenerationSnapshot{};
		}
		Snapshot.WaypointGeneratorVersion = Snapshot.GeneratorContext.WaypointGeneratorVersion;
		Snapshot.GeneratorInputDigest = Snapshot.GeneratorContext.GeneratorInputDigest;
		Snapshot.FrameCount = Data->FrameCount;
		Snapshot.RouteCount = Data->DroneCount;
		return Snapshot;
	}

	FRouteArtifactBuildIdentity MakeBuild(
		const FRouteWaypointGenerationSnapshot& Snapshot)
	{
		FRouteArtifactBuildIdentity Build;
		Build.AssignmentBinding.FleetSessionID = TEXT("c2b-benchmark-fleet");
		Build.AssignmentBinding.AssignmentAuthoritySessionID = TEXT("c2b-benchmark-authority");
		Build.AssignmentBinding.AssignmentRevision = 1;
		Build.AssignmentBinding.AssignmentDigest = HashLabel(TEXT("c2b-benchmark-assignment"));
		Build.SourceDatasetDigest = Snapshot.SourceIdentity.SourceDatasetDigest;
		Build.WaypointGeneratorVersion = Snapshot.WaypointGeneratorVersion;
		Build.GeneratorInputDigest = Snapshot.GeneratorInputDigest;
		return Build;
	}

	FRouteArtifactWorkspaceAuthorityGate MakeGate(
		const FRouteArtifactBuildIdentity& Build,
		const FRouteWaypointGenerationSnapshot& Snapshot)
	{
		FRouteArtifactWorkspaceAuthorityGate Gate;
		Gate.bHasFrozenSnapshot = true;
		Gate.FrozenFleetSessionID = Build.AssignmentBinding.FleetSessionID;
		Gate.FrozenAssignmentAuthoritySessionID =
			Build.AssignmentBinding.AssignmentAuthoritySessionID;
		Gate.FrozenAssignmentRevision = Build.AssignmentBinding.AssignmentRevision;
		Gate.bConsoleAccepted = true;
		Gate.ConsoleAcceptedBinding = Build.AssignmentBinding;
		Gate.bSourceExists = true;
		Gate.ActiveSourceIdentity = Snapshot.SourceIdentity;
		Gate.RouteSlotCount = Snapshot.RouteCount;
		return Gate;
	}

	FRouteArtifactManifest MakeDraft(
		const FRouteArtifactBuildIdentity& Build,
		const FRouteWaypointGenerationSnapshot& Snapshot,
		const FString& ArtifactSetID)
	{
		FRouteArtifactManifest Draft;
		Draft.ArtifactSetID = ArtifactSetID;
		Draft.CreatedAtUtc = 1700000000123;
		Draft.AssignmentBinding = Build.AssignmentBinding;
		Draft.FleetRevisionAtCapture = 1;
		Draft.RouteDatasetGeneration = Snapshot.RouteDatasetGeneration;
		Draft.CompletionMode = ERouteAssignmentCompletionMode::Full;
		Draft.FreezePolicy = ERouteAssignmentFreezePolicy::RequireFull;
		Draft.MatchedCount = Snapshot.RouteCount;
		Draft.SourceIdentity = Snapshot.SourceIdentity;
		Draft.FrameCount = Snapshot.FrameCount;
		Draft.RouteSlotCount = Snapshot.RouteCount;
		Draft.WaypointGeneratorVersion = Snapshot.WaypointGeneratorVersion;
		Draft.GeneratorInputDigest = Snapshot.GeneratorInputDigest;
		Draft.MatchedMapping.Reserve(Snapshot.RouteCount);
		for (int32 Slot = 0; Slot < Snapshot.RouteCount; ++Slot)
		{
			FRouteArtifactMatchedMapping& Mapping = Draft.MatchedMapping.AddDefaulted_GetRef();
			Mapping.DroneID = FString::Printf(TEXT("benchmark-drone-%06d"), Slot);
			Mapping.RouteSlotIndex = Slot;
		}
		return Draft;
	}

	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> MakeRoutePrefix(
		const TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe>& Source,
		const int32 RouteCount)
	{
		TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> Result =
			MakeShared<FRoutePreviewData, ESPMode::ThreadSafe>();
		Result->FrameCount = Source->FrameCount;
		Result->DroneCount = RouteCount;
		Result->SampleRateHz = Source->SampleRateHz;
		Result->ShowOriginLatitude = 31.932471;
		Result->ShowOriginLongitude = 118.638531;
		Result->bShowOriginSet = true;
		Result->SourceIdentity = Source->SourceIdentity;
		const int64 SampleCount = static_cast<int64>(Result->FrameCount) * RouteCount;
		Result->PositionsENU.SetNumUninitialized(static_cast<int32>(SampleCount));
		Result->Colors.SetNumUninitialized(static_cast<int32>(SampleCount));
		for (int32 Frame = 0; Frame < Result->FrameCount; ++Frame)
		{
			FMemory::Memcpy(
				Result->PositionsENU.GetData() + static_cast<int64>(Frame) * RouteCount,
				Source->PositionsENU.GetData() + static_cast<int64>(Frame) * Source->DroneCount,
				static_cast<SIZE_T>(RouteCount) * sizeof(FVector3f));
			FMemory::Memcpy(
				Result->Colors.GetData() + static_cast<int64>(Frame) * RouteCount,
				Source->Colors.GetData() + static_cast<int64>(Frame) * Source->DroneCount,
				static_cast<SIZE_T>(RouteCount) * sizeof(FFloat16Color));
		}
		return Result;
	}

	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> MakeSyntheticData(
		const int32 FrameCount,
		const int32 RouteCount)
	{
		TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> Data =
			MakeShared<FRoutePreviewData, ESPMode::ThreadSafe>();
		Data->FrameCount = FrameCount;
		Data->DroneCount = RouteCount;
		Data->ShowOriginLatitude = 31.932471;
		Data->ShowOriginLongitude = 118.638531;
		Data->bShowOriginSet = true;
		FString Error;
		FRouteArtifactSourceIdentity::Create(
			HashLabel(TEXT("c2b-synthetic-source")),
			static_cast<int64>(FrameCount) * RouteCount,
			TEXT("c2b-synthetic.npz"), Data->SourceIdentity, Error);
		const int64 Samples = static_cast<int64>(FrameCount) * RouteCount;
		Data->PositionsENU.SetNumUninitialized(static_cast<int32>(Samples));
		Data->Colors.SetNumUninitialized(static_cast<int32>(Samples));
		for (int32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			for (int32 Route = 0; Route < RouteCount; ++Route)
			{
				const int32 Index = Frame * RouteCount + Route;
				Data->PositionsENU[Index] = FVector3f(
					Route * 0.01f, Frame * 0.02f, Frame * 0.001f);
				Data->Colors[Index] = FFloat16Color(
					FLinearColor(0.25f, 0.5f, 0.75f, 1.0f));
			}
		}
		return Data;
	}

	struct FSweepResult
	{
		int32 Workers = 0;
		double Seconds = 0.0;
		double CpuPercent = 0.0;
		int64 Rows = 0;
		int64 Bytes = 0;
		int32 PeakOpenFiles = 0;
		int64 WorkingSetDelta = 0;
	};

	FSweepResult RunNoDiskSweep(
		const FRouteWaypointGenerationSnapshot& Snapshot,
		const int32 WorkerCount)
	{
		FSweepResult Output;
		Output.Workers = WorkerCount;
		FCriticalSection ClaimMutex;
		FCriticalSection AggregateMutex;
		int32 NextRoute = 0;
		const FProcessSample Before = CaptureProcessSample();
		TArray<TFuture<void>> Workers;
		Workers.Reserve(WorkerCount);
		for (int32 WorkerIndex = 0; WorkerIndex < WorkerCount; ++WorkerIndex)
		{
			Workers.Add(Async(EAsyncExecution::ThreadPool, [&]()
			{
				int64 LocalRows = 0;
				int64 LocalBytes = 0;
				for (;;)
				{
					int32 RouteSlot = INDEX_NONE;
					{
						FScopeLock Lock(&ClaimMutex);
						if (NextRoute < Snapshot.RouteCount) RouteSlot = NextRoute++;
					}
					if (RouteSlot == INDEX_NONE) break;
					FRouteWaypointGeneratorInput Input;
					Input.RouteData = Snapshot.RouteData.Get();
					Input.RouteSlotIndex = RouteSlot;
					Input.SourceIdentity = Snapshot.SourceIdentity;
					Input.Context = Snapshot.GeneratorContext;
					FDiscardSink Sink;
					const FRouteWaypointGenerationResult Result =
						RouteWaypointGenerator::GenerateRoute(Input, Sink);
					if (Result.bSuccess)
					{
						LocalRows += Result.RowCount;
						LocalBytes += Result.BytesWritten;
					}
				}
				FScopeLock Lock(&AggregateMutex);
				Output.Rows += LocalRows;
				Output.Bytes += LocalBytes;
			}));
		}
		for (TFuture<void>& Worker : Workers) Worker.Wait();
		const FProcessSample After = CaptureProcessSample();
		Output.Seconds = After.WallSeconds - Before.WallSeconds;
		Output.CpuPercent = AverageCpuPercent(Before, After);
		Output.WorkingSetDelta = static_cast<int64>(After.WorkingSetBytes)
			- static_cast<int64>(Before.WorkingSetBytes);
		return Output;
	}

	FString BenchmarkRoot(const FString& Label)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectIntermediateDir(), TEXT("RouteWaypointC2BBenchmark"),
			Label + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	FSweepResult RunFilesystemSweep(
		FAutomationTestBase& Test,
		const FRouteWaypointGenerationSnapshot& Snapshot,
		const int32 WorkerCount)
	{
		FSweepResult Output;
		Output.Workers = WorkerCount;
		const FString Root = BenchmarkRoot(TEXT("calibration"));
		FRouteArtifactWorkspaceManager Manager;
		FString Error;
		const FRouteArtifactBuildIdentity Build = MakeBuild(Snapshot);
		const FRouteArtifactWorkspaceAuthorityGate Gate = MakeGate(Build, Snapshot);
		FRouteArtifactGenerationHandle Handle;
		if (!Manager.SetConfiguredWorkspaceRoot(Root, Error)
			|| !Manager.BeginGeneration(Build, Gate, Handle, Error))
		{
			Test.AddError(Error);
			return Output;
		}
		FRouteWaypointBatchRequest Request;
		Request.Snapshot = Snapshot;
		Request.WorkspaceManager = &Manager;
		Request.AttemptID = Handle.AttemptID;
		Request.ArtifactSetID = Handle.ArtifactSetID;
		Request.WorkerCountOverride = WorkerCount;
		FRouteWaypointBatchGenerator Batch;
		const FProcessSample Before = CaptureProcessSample();
		if (!Batch.Start(Request, Error))
		{
			Test.AddError(Error);
			return Output;
		}
		Batch.Wait();
		const FProcessSample After = CaptureProcessSample();
		const FRouteWaypointBatchProgress Progress = Batch.GetProgress();
		Output.Seconds = After.WallSeconds - Before.WallSeconds;
		Output.CpuPercent = AverageCpuPercent(Before, After);
		Output.Rows = Progress.RowsWritten;
		Output.Bytes = Progress.BytesWritten;
		Output.PeakOpenFiles = Progress.PeakOpenRouteFiles;
		Output.WorkingSetDelta = static_cast<int64>(After.WorkingSetBytes)
			- static_cast<int64>(Before.WorkingSetBytes);
		Test.TestEqual(TEXT("Calibration filesystem routes complete"),
			Progress.CompletedRoutes, Snapshot.RouteCount);
		Manager.AbortGeneration(Error);
		Manager.Shutdown();
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return Output;
	}

	void LogSweep(FAutomationTestBase& Test, const TCHAR* Kind, const FSweepResult& Value)
	{
		const double RowsPerSecond = Value.Seconds > 0.0 ? Value.Rows / Value.Seconds : 0.0;
		const double MbPerSecond = Value.Seconds > 0.0
			? Value.Bytes / (1024.0 * 1024.0 * Value.Seconds) : 0.0;
		Test.AddInfo(FString::Printf(
			TEXT("C2B_SWEEP Kind=%s Workers=%d Elapsed=%.6f RowsPerSec=%.3f MBPerSec=%.3f CPU=%.3f PeakOpenFiles=%d WorkingSetDelta=%lld"),
			Kind, Value.Workers, Value.Seconds, RowsPerSecond, MbPerSecond,
			Value.CpuPercent, Value.PeakOpenFiles, Value.WorkingSetDelta));
	}

	void LogHotspotMicrobench(
		FAutomationTestBase& Test,
		const FRouteWaypointGenerationSnapshot& Snapshot)
	{
		constexpr int32 Iterations = 1000000;
		volatile double GeoAccumulator = 0.0;
		FString GeoError;
		const double GeoStart = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < Iterations; ++Index)
		{
			const FVector3f& Position = Snapshot.RouteData->PositionsENU[
				Index % Snapshot.RouteData->PositionsENU.Num()];
			RouteAssignmentGeo::FGeodeticPosition Geodetic;
			RouteAssignmentGeo::LocalEnuToGeodetic(
				Snapshot.GeneratorContext.GeoOrigin,
				Position.X, Position.Y, 0.0, Geodetic, GeoError);
			GeoAccumulator += Geodetic.LatitudeDegrees;
		}
		const double GeoSeconds = FPlatformTime::Seconds() - GeoStart;

		TArray<uint8> Row;
		FRouteWaypointGeneratorError RowError;
		int64 FormatAccumulator = 0;
		const double FormatStart = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < Iterations; ++Index)
		{
			RouteWaypointGenerator::FormatWaypointRow(
				Index, 148, 26, 216, 319324710, 1186385310,
				-222241935, Row, RowError);
			FormatAccumulator += Row.Num();
		}
		const double FormatSeconds = FPlatformTime::Seconds() - FormatStart;
		Test.AddInfo(FString::Printf(
			TEXT("C2B_HOTSPOT Iterations=%d GeoSeconds=%.6f FormatSeconds=%.6f GeoAccumulator=%.3f FormatBytes=%lld"),
			Iterations, GeoSeconds, FormatSeconds,
			static_cast<double>(GeoAccumulator), FormatAccumulator));
	}

	bool RunFilesystemDigestSet(
		FAutomationTestBase& Test,
		const FRouteWaypointGenerationSnapshot& Snapshot,
		const int32 WorkerCount,
		TArray<FRouteArtifactDigest256>& OutDigests)
	{
		const FString Root = BenchmarkRoot(TEXT("determinism"));
		FRouteArtifactWorkspaceManager Manager;
		FString Error;
		const FRouteArtifactBuildIdentity Build = MakeBuild(Snapshot);
		const FRouteArtifactWorkspaceAuthorityGate Gate = MakeGate(Build, Snapshot);
		FRouteArtifactGenerationHandle Handle;
		if (!Manager.SetConfiguredWorkspaceRoot(Root, Error)
			|| !Manager.BeginGeneration(Build, Gate, Handle, Error))
		{
			Test.AddError(Error);
			return false;
		}
		FRouteWaypointBatchRequest Request;
		Request.Snapshot = Snapshot;
		Request.WorkspaceManager = &Manager;
		Request.AttemptID = Handle.AttemptID;
		Request.ArtifactSetID = Handle.ArtifactSetID;
		Request.WorkerCountOverride = WorkerCount;
		FRouteWaypointBatchGenerator Batch;
		if (!Batch.Start(Request, Error))
		{
			Test.AddError(Error);
			return false;
		}
		Batch.Wait();
		if (Batch.GetProgress().CompletedRoutes != Snapshot.RouteCount)
		{
			Test.AddError(TEXT("Real subset determinism generation failed."));
			return false;
		}
		OutDigests.SetNum(Snapshot.RouteCount);
		for (int32 Slot = 0; Slot < Snapshot.RouteCount; ++Slot)
		{
			FString Path;
			int64 FileBytes = 0;
			if (!Manager.ResolveSafeStagingRoutePath(Slot, Path, Error)
				|| !FRouteArtifactSha256::HashFileStreaming(
					Path, OutDigests[Slot], FileBytes, Error))
			{
				Test.AddError(Error);
				return false;
			}
		}
		Manager.AbortGeneration(Error);
		Manager.Shutdown();
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return true;
	}

	bool CountMissionRows(const FString& Path, int64& OutRows)
	{
		OutRows = 0;
		TUniquePtr<IFileHandle> Handle(
			FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
		if (!Handle) return false;
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(1024 * 1024);
		int64 Remaining = Handle->Size();
		while (Remaining > 0)
		{
			const int64 ReadCount = FMath::Min<int64>(Remaining, Buffer.Num());
			if (!Handle->Read(Buffer.GetData(), ReadCount)) return false;
			for (int64 Index = 0; Index < ReadCount; ++Index)
			{
				if (Buffer[Index] == '\n') ++OutRows;
			}
			Remaining -= ReadCount;
		}
		return true;
	}

	bool CollectMissionRow(const dronegcs::wpl110::Row& Row, void* Context)
	{
		static_cast<std::vector<dronegcs::wpl110::Row>*>(Context)->push_back(Row);
		return true;
	}

	bool LoadTypedMissionRows(
		const FString& Path,
		std::vector<dronegcs::wpl110::Row>& OutRows)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path)) return false;
		OutRows.clear();
		OutRows.reserve(9100);
		dronegcs::wpl110::StreamingParser Parser(9100, &CollectMissionRow, &OutRows);
		return Parser.Consume(Bytes.GetData(), static_cast<std::size_t>(Bytes.Num()))
			&& Parser.Finish() && OutRows.size() == 9100;
	}

	bool BenchmarkAllRealMissionRoutes(
		FAutomationTestBase& Test,
		const FString& CurrentRoot,
		const FRouteArtifactManifest& Manifest)
	{
		using namespace dronegcs::route_mission;
		std::vector<dronegcs::wpl110::Row> Rows;
		WholeRouteEncoderV1 Encoder;
		dronegcs::route_mission::Error MissionError;
		double EncodeSeconds = 0.0;
		uint64 EncodeCpu100ns = 0;
		uint64 Checksum = 0;
		for (const FRouteArtifactRecord& Record : Manifest.Routes)
		{
			const FString RoutePath = FPaths::Combine(CurrentRoot, Record.RelativePath);
			if (!LoadTypedMissionRows(RoutePath, Rows))
			{
				Test.AddError(FString::Printf(
					TEXT("2F-C real route %d did not parse."), Record.RouteSlotIndex));
				return false;
			}
			const FProcessSample CpuBefore = CaptureProcessSample();
			const double EncodeStart = FPlatformTime::Seconds();
			if (!Encoder.Begin(9100, MissionError)) return false;
			for (const dronegcs::wpl110::Row& Row : Rows)
			{
				if (!Encoder.Consume(Row, MissionError)) return false;
			}
			if (!Encoder.Finish(MissionError)) return false;
			EncodeSeconds += FPlatformTime::Seconds() - EncodeStart;
			const FProcessSample CpuAfter = CaptureProcessSample();
			EncodeCpu100ns += CpuAfter.Cpu100ns - CpuBefore.Cpu100ns;
			const std::vector<Byte>& Encoded = Encoder.GetBytes();
			Checksum += Encoded.front() + Encoded.back() + Encoded.size();
		}
		constexpr uint64 TotalRows = 9100ULL * 1000;
		constexpr uint64 TotalBytes = TotalRows * MissionItemRecordBytesV1;
		Test.AddInfo(FString::Printf(
			TEXT("C2B_2FC_MISSION_1000 Routes=1000 DistinctRoutes=1000 Rows=%llu Bytes=%llu EncodeSeconds=%.6f CpuSeconds=%.6f RowsPerSec=%.3f MiBPerSec=%.3f ActiveEncodedRoutes=1 ActiveBufferBytes=345800 AllocationProxy=1 Checksum=%llu"),
			TotalRows, TotalBytes, EncodeSeconds,
			static_cast<double>(EncodeCpu100ns) / 1.0e7,
			TotalRows / EncodeSeconds,
			(TotalBytes / (1024.0 * 1024.0)) / EncodeSeconds,
			Checksum));
		return true;
	}

	FString Utf8Line(const TArray<uint8>& Bytes, const int32 Begin, const int32 End)
	{
		const int32 Count = FMath::Max(0, End - Begin);
		FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Begin), Count);
		return FString(Converted.Length(), Converted.Get());
	}

	bool ReadFirstLastRows(
		const FString& Path, int64& OutBytes, FString& OutFirst, FString& OutLast)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path)) return false;
		OutBytes = Bytes.Num();
		int32 FirstBegin = INDEX_NONE;
		int32 FirstEnd = INDEX_NONE;
		int32 LastBegin = INDEX_NONE;
		for (int32 Index = 0; Index + 1 < Bytes.Num(); ++Index)
		{
			if (Bytes[Index] == '\r' && Bytes[Index + 1] == '\n')
			{
				if (FirstBegin == INDEX_NONE)
				{
					FirstBegin = Index + 2;
				}
				else if (FirstEnd == INDEX_NONE)
				{
					FirstEnd = Index;
				}
				LastBegin = Index + 2;
			}
		}
		if (FirstBegin == INDEX_NONE || FirstEnd == INDEX_NONE || LastBegin == INDEX_NONE)
		{
			return false;
		}
		OutFirst = Utf8Line(Bytes, FirstBegin, FirstEnd);
		OutLast = Utf8Line(Bytes, LastBegin, Bytes.Num());
		return true;
	}

	struct FBenchmarkFinalizeResult
	{
		bool bSuccess = false;
		FRouteArtifactFinalizeMetrics Metrics;
		FString Error;
	};

	struct FRealBenchmarkState
	{
		FAutomationTestBase* Test = nullptr;
		FString Root;
		TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> Data;
		FRouteWaypointGenerationSnapshot Snapshot;
		FRouteArtifactBuildIdentity Build;
		FRouteArtifactWorkspaceAuthorityGate Gate;
		FRouteArtifactGenerationHandle Handle;
		TUniquePtr<FRouteArtifactWorkspaceManager> Manager;
		TUniquePtr<FRouteWaypointBatchGenerator> Batch;
		FRouteArtifactManifest Draft;
		FProcessMonitor Monitor;
		FBackgroundSentinel Sentinel;
		FProcessSample BeforeLoad;
		FProcessSample BeforeGeneration;
		FProcessSample AfterSnapshot;
		double EndToEndStartSeconds = 0.0;
		double LoadSeconds = 0.0;
		double SnapshotSeconds = 0.0;
		double BeginPreflightSeconds = 0.0;
		double GenerationStartSeconds = 0.0;
		double GenerationSeconds = 0.0;
		double LastHeartbeatSeconds = 0.0;
		TArray<double> HeartbeatIntervals;
		FRouteWaypointBatchProgress FinalProgress;
		TFuture<FBenchmarkFinalizeResult> FinalizeFuture;
		bool bFinalizeStarted = false;
	};

	class FWaitForRealBenchmarkCommand final : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForRealBenchmarkCommand(
			const TSharedPtr<FRealBenchmarkState, ESPMode::ThreadSafe>& InState)
			: State(InState)
		{
		}

		bool Update() override
		{
			const double Now = FPlatformTime::Seconds();
			if (State->LastHeartbeatSeconds > 0.0)
			{
				State->HeartbeatIntervals.Add(Now - State->LastHeartbeatSeconds);
			}
			State->LastHeartbeatSeconds = Now;
			if (!State->bFinalizeStarted)
			{
				if (!State->Batch->IsFinished()) return false;
				State->Batch->Wait();
				State->FinalProgress = State->Batch->GetProgress();
				State->GenerationSeconds =
					FPlatformTime::Seconds() - State->GenerationStartSeconds;
				const FRouteWaypointBatchProgress& Progress = State->FinalProgress;
				State->Test->TestEqual(TEXT("Real benchmark worker state"),
					Progress.GenerationState, ERouteWaypointBatchState::WorkersSucceeded);
				State->Test->TestEqual(TEXT("Real benchmark completed routes"),
					Progress.CompletedRoutes, 1000);
				State->Test->TestEqual(TEXT("Real benchmark rows"),
					Progress.RowsWritten, static_cast<int64>(9100000));
				if (Progress.GenerationState != ERouteWaypointBatchState::WorkersSucceeded)
				{
					State->Test->AddError(FString::Printf(
						TEXT("Real generation failed. Slot=%d Code=%d Reason=%s Completed=%d Failed=%d Cancelled=%d"),
						Progress.FirstFailureRouteSlotIndex,
						static_cast<int32>(Progress.FirstFailureErrorCode),
						*Progress.FirstFailureDiagnostic, Progress.CompletedRoutes,
						Progress.FailedRoutes, Progress.CancellationCount));
					State->Monitor.Stop();
					double A = 0.0, B = 0.0, C = 0.0;
					State->Sentinel.Stop(A, B, C);
					FString AbortError;
					State->Manager->AbortGeneration(AbortError);
					State->Manager->Shutdown();
					IFileManager::Get().DeleteDirectory(*State->Root, false, true);
					return true;
				}
				State->bFinalizeStarted = true;
				TSharedPtr<FRealBenchmarkState, ESPMode::ThreadSafe> SharedState = State;
				State->FinalizeFuture = Async(EAsyncExecution::ThreadPool,
					[SharedState]()
					{
						FBenchmarkFinalizeResult Result;
						Result.bSuccess = SharedState->Manager->FinalizeGeneratedRoutes(
							SharedState->Draft, SharedState->Gate,
							SharedState->Snapshot.MakeSourceValidationContext(),
							Result.Error, &Result.Metrics);
						return Result;
					});
				return false;
			}
			if (!State->FinalizeFuture.IsReady()) return false;

			FBenchmarkFinalizeResult FinalizeResult = State->FinalizeFuture.Get();
			FString Error = FinalizeResult.Error;
			const FRouteArtifactFinalizeMetrics& FinalizeMetrics = FinalizeResult.Metrics;
			State->Test->TestTrue(TEXT("Real benchmark finalize"), FinalizeResult.bSuccess);
			if (!FinalizeResult.bSuccess)
			{
				State->Test->AddError(Error);
				State->Monitor.Stop();
				double A = 0.0, B = 0.0, C = 0.0;
				State->Sentinel.Stop(A, B, C);
				State->Manager->Shutdown();
				IFileManager::Get().DeleteDirectory(*State->Root, false, true);
				return true;
			}
			const double EndToEndSeconds = State->LoadSeconds
				+ State->SnapshotSeconds + State->BeginPreflightSeconds
				+ State->GenerationSeconds
				+ FinalizeMetrics.AuthoritativeRouteValidationSeconds
				+ FinalizeMetrics.ManifestConstructionSerializationSeconds
				+ FinalizeMetrics.ReadyPromotionSeconds;

			const FProcessSample AfterFinalize = CaptureProcessSample();
			State->Monitor.Stop();
			double SentinelAverageMs = 0.0;
			double SentinelP95Ms = 0.0;
			double SentinelMaxMs = 0.0;
			State->Sentinel.Stop(SentinelAverageMs, SentinelP95Ms, SentinelMaxMs);
			double PeakCpu = 0.0;
			uint64 PeakWorkingSet = 0;
			uint64 PeakPrivate = 0;
			uint32 PeakHandles = 0;
			State->Monitor.Read(PeakCpu, PeakWorkingSet, PeakPrivate, PeakHandles);

			State->HeartbeatIntervals.Sort();
			double HeartbeatSum = 0.0;
			for (const double Interval : State->HeartbeatIntervals) HeartbeatSum += Interval;
			const double HeartbeatAverageMs = State->HeartbeatIntervals.IsEmpty() ? 0.0
				: 1000.0 * HeartbeatSum / State->HeartbeatIntervals.Num();
			const double HeartbeatP95Ms = State->HeartbeatIntervals.IsEmpty() ? 0.0
				: 1000.0 * State->HeartbeatIntervals[FMath::Clamp(
					FMath::CeilToInt(State->HeartbeatIntervals.Num() * 0.95) - 1,
					0, State->HeartbeatIntervals.Num() - 1)];
			const double HeartbeatMaxMs = State->HeartbeatIntervals.IsEmpty() ? 0.0
				: 1000.0 * State->HeartbeatIntervals.Last();

			const double ValidationStart = FPlatformTime::Seconds();
			FRouteArtifactManifest Validated;
			const bool bValidated = State->Manager->LoadAndValidateCurrentArtifact(
				Validated, Error);
			const double ValidationSeconds = FPlatformTime::Seconds() - ValidationStart;
			State->Test->TestTrue(TEXT("Post-finalize Current validation"), bValidated);
			State->Test->TestEqual(TEXT("Manifest route count"),
				State->Draft.Routes.Num(), 1000);
			State->Test->TestEqual(TEXT("Production finalize hash reads"),
				State->Manager->GetTestFinalizeRouteValidationReadCount(), static_cast<int64>(1000));

			int32 WaypointFileCount = 0;
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(
				Files, *State->Manager->GetCurrentInfo().CurrentRoot,
				TEXT("*.waypoints"), true, false, false);
			WaypointFileCount = Files.Num();
			State->Test->TestEqual(TEXT("Real benchmark physical route files"),
				WaypointFileCount, 1000);
			for (const FRouteArtifactRecord& Record : State->Draft.Routes)
			{
				int64 Rows = 0;
				const FString RoutePath = FPaths::Combine(
					State->Manager->GetCurrentInfo().CurrentRoot, Record.RelativePath);
				if (!CountMissionRows(RoutePath, Rows) || Rows != 9100)
				{
					State->Test->AddError(FString::Printf(
						TEXT("Route %d row count mismatch."), Record.RouteSlotIndex));
					break;
				}
			}

			for (const int32 RouteSlot : {0, 500, 999})
			{
				const FRouteArtifactRecord& Record = State->Draft.Routes[RouteSlot];
				const FString RoutePath = FPaths::Combine(
					State->Manager->GetCurrentInfo().CurrentRoot, Record.RelativePath);
				int64 FileBytes = 0;
				FString FirstRow;
				FString LastRow;
				State->Test->TestTrue(TEXT("Representative route read"),
					ReadFirstLastRows(RoutePath, FileBytes, FirstRow, LastRow));
				State->Test->AddInfo(FString::Printf(
					TEXT("C2B_ROUTE Slot=%d Bytes=%lld Digest=%s First=%s Last=%s"),
					RouteSlot, FileBytes, *Record.RouteArtifactDigest.ToLowerHex(),
					*FirstRow, *LastRow));
			}

			const FString MissionBenchmarkExport =
				FPlatformMisc::GetEnvironmentVariable(TEXT("DRONEGCS_2FC_EXPORT_ROUTES"));
			if (!MissionBenchmarkExport.IsEmpty())
			{
				State->Test->TestTrue(TEXT("2F-C export directory created"),
					IFileManager::Get().MakeDirectory(*MissionBenchmarkExport, true));
				for (const int32 RouteSlot : {0, 500, 999})
				{
					const FRouteArtifactRecord& Record = State->Draft.Routes[RouteSlot];
					const FString SourcePath = FPaths::Combine(
						State->Manager->GetCurrentInfo().CurrentRoot, Record.RelativePath);
					const FString DestinationPath = FPaths::Combine(
						MissionBenchmarkExport,
						FString::Printf(TEXT("%d.waypoints"), RouteSlot));
					State->Test->TestEqual(TEXT("2F-C representative route exported"),
						IFileManager::Get().Copy(
							*DestinationPath, *SourcePath, true, true), COPY_OK);
				}
				State->Test->AddInfo(FString::Printf(
					TEXT("C2B_2FC_EXPORT Directory=%s Routes=0,500,999"),
					*MissionBenchmarkExport));
			}
			State->Test->TestTrue(TEXT("2F-C all distinct real routes encode"),
				BenchmarkAllRealMissionRoutes(
					*State->Test,
					State->Manager->GetCurrentInfo().CurrentRoot,
					State->Draft));

			const FRouteWaypointBatchProgress& Progress = State->FinalProgress;
			const double GenerationSeconds = State->GenerationSeconds;
			const double RowsPerSecond = GenerationSeconds > 0.0
				? Progress.RowsWritten / GenerationSeconds : 0.0;
			const double GenerationMbPerSecond = GenerationSeconds > 0.0
				? Progress.BytesWritten / (1024.0 * 1024.0 * GenerationSeconds) : 0.0;
			const double ValidationMbPerSecond = FinalizeMetrics.AuthoritativeRouteValidationSeconds > 0.0
				? Progress.BytesWritten / (1024.0 * 1024.0
					* FinalizeMetrics.AuthoritativeRouteValidationSeconds) : 0.0;
			State->Test->AddInfo(FString::Printf(
				TEXT("C2B_FULL SourceDigest=%s GeneratorInputDigest=%s Version=%s Workers=%d PeakWorkers=%d PeakOpenFiles=%d Load=%.6f Snapshot=%.6f BeginPreflight=%.6f Generation=%.6f Validation=%.6f Manifest=%.6f Promotion=%.6f EndToEnd=%.6f Rows=%lld Bytes=%lld RowsPerSec=%.3f GenerationMBPerSec=%.3f ValidationMBPerSec=%.3f RoutesPerSec=%.3f AvgCPU=%.3f PeakCPU=%.3f BeforeWS=%llu SnapshotWS=%llu PeakWS=%llu BeforePrivate=%llu SnapshotPrivate=%llu PeakPrivate=%llu HandlesBefore=%u HandlesPeak=%u HandlesAfter=%u HeartbeatAvgMs=%.3f HeartbeatP95Ms=%.3f HeartbeatMaxMs=%.3f SentinelAvgMs=%.3f SentinelP95Ms=%.3f SentinelMaxMs=%.3f ArtifactSetID=%s ArtifactSetDigest=%s ValidationReads=%lld OfflineCurrentValidation=%.6f"),
				*State->Snapshot.SourceIdentity.SourceDatasetDigest.ToLowerHex(),
				*State->Snapshot.GeneratorInputDigest.ToLowerHex(),
				*State->Snapshot.WaypointGeneratorVersion,
				Progress.WorkerCount, Progress.PeakActiveWorkers,
				Progress.PeakOpenRouteFiles, State->LoadSeconds,
				State->SnapshotSeconds, State->BeginPreflightSeconds,
				GenerationSeconds, FinalizeMetrics.AuthoritativeRouteValidationSeconds,
				FinalizeMetrics.ManifestConstructionSerializationSeconds,
				FinalizeMetrics.ReadyPromotionSeconds, EndToEndSeconds,
				Progress.RowsWritten, Progress.BytesWritten, RowsPerSecond,
				GenerationMbPerSecond, ValidationMbPerSecond,
				GenerationSeconds > 0.0 ? 1000.0 / GenerationSeconds : 0.0,
				AverageCpuPercent(State->BeforeGeneration, AfterFinalize), PeakCpu,
				State->BeforeGeneration.WorkingSetBytes,
				State->AfterSnapshot.WorkingSetBytes, PeakWorkingSet,
				State->BeforeGeneration.PrivateBytes,
				State->AfterSnapshot.PrivateBytes, PeakPrivate,
				State->BeforeGeneration.HandleCount, PeakHandles,
				AfterFinalize.HandleCount, HeartbeatAverageMs, HeartbeatP95Ms,
				HeartbeatMaxMs, SentinelAverageMs, SentinelP95Ms, SentinelMaxMs,
				*State->Draft.ArtifactSetID,
				*State->Draft.ArtifactSetDigest.ToLowerHex(),
				State->Manager->GetTestFinalizeRouteValidationReadCount(), ValidationSeconds));

			const double CleanupStart = FPlatformTime::Seconds();
			State->Manager->Shutdown();
			State->Batch.Reset();
			State->Snapshot = FRouteWaypointGenerationSnapshot{};
			State->Data.Reset();
			State->Manager.Reset();
			IFileManager::Get().DeleteDirectory(*State->Root, false, true);
			const double CleanupSeconds = FPlatformTime::Seconds() - CleanupStart;
			const FProcessSample AfterCleanup = CaptureProcessSample();
			State->Test->TestFalse(TEXT("Real benchmark temp workspace cleaned"),
				IFileManager::Get().DirectoryExists(*State->Root));
			State->Test->AddInfo(FString::Printf(
				TEXT("C2B_CLEANUP Seconds=%.6f WorkingSet=%llu Private=%llu Handles=%u ActiveWorkers=0 OpenRouteFiles=0 SnapshotReleased=1"),
				CleanupSeconds, AfterCleanup.WorkingSetBytes,
				AfterCleanup.PrivateBytes, AfterCleanup.HandleCount));
			return true;
		}

	private:
		TSharedPtr<FRealBenchmarkState, ESPMode::ThreadSafe> State;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointC2BContractTests,
	"DroneGCS.RouteWaypointBatch.2DC2B.StrictBoundsAndShutdown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointC2BContractTests::RunTest(const FString& Parameters)
{
	TArray<uint8> Row;
	FRouteWaypointGeneratorError RowError;
	TestTrue(TEXT("Maximum-width row formats"), RouteWaypointGenerator::FormatWaypointRow(
		MAX_int32, 255, 255, 255, -899999999, -1799999999, MIN_int64,
		Row, RowError));
	TestEqual(TEXT("Maximum row derivation"), static_cast<int64>(Row.Num()),
		FRouteWaypointBatchGenerator::GetMaxWaypointRowBytesV1());
	TestEqual(TEXT("Maximum v1 row is 80 bytes"),
		FRouteWaypointBatchGenerator::GetMaxWaypointRowBytesV1(), static_cast<int64>(80));
	int64 RouteBytes = 0;
	FString Error;
	TestTrue(TEXT("Max route formula"),
		FRouteWaypointBatchGenerator::ComputeMaxRouteFileBytesV1(9100, RouteBytes, Error));
	TestEqual(TEXT("9100-frame max route"), RouteBytes, static_cast<int64>(746211));
	int64 ThousandBytes = 0;
	int64 TenThousandBytes = 0;
	TestTrue(TEXT("1000 route bound"),
		FRouteWaypointBatchGenerator::ComputeOutputUpperBound(
			9100, 1000, ThousandBytes, Error));
	TestTrue(TEXT("10000 route bound"),
		FRouteWaypointBatchGenerator::ComputeOutputUpperBound(
			9100, 10000, TenThousandBytes, Error));
	AddInfo(FString::Printf(
		TEXT("C2B_PREFLIGHT MaxRow=%lld MaxRoute9100=%lld Artifact1000=%lld Artifact10000=%lld OldRoute1000=%lld OldRoute10000=%lld"),
		FRouteWaypointBatchGenerator::GetMaxWaypointRowBytesV1(), RouteBytes,
		ThousandBytes, TenThousandBytes,
		static_cast<int64>(1000) * (11 + static_cast<int64>(9100) * 130),
		static_cast<int64>(10000) * (11 + static_cast<int64>(9100) * 130)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointC2BShutdownTests,
	"DroneGCS.RouteWaypointBatch.2DC2B.BoundedCancelShutdown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointC2BShutdownTests::RunTest(const FString& Parameters)
{
	using namespace RouteWaypointC2BBenchmarkPrivate;
	const TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> Data =
		MakeSyntheticData(1024, 1000);
	FString Error;
	const FRouteWaypointGenerationSnapshot Snapshot = MakeSnapshot(Data, Error);
	const FRouteArtifactBuildIdentity Build = MakeBuild(Snapshot);
	const FRouteArtifactWorkspaceAuthorityGate Gate = MakeGate(Build, Snapshot);
	const FString Root = BenchmarkRoot(TEXT("shutdown"));
	FRouteArtifactWorkspaceManager Manager;
	FRouteArtifactGenerationHandle Handle;
	TestTrue(TEXT("Shutdown fixture root"),
		Manager.SetConfiguredWorkspaceRoot(Root, Error));
	TestTrue(TEXT("Shutdown fixture begin"),
		Manager.BeginGeneration(Build, Gate, Handle, Error));
	FRouteWaypointBatchRequest Request;
	Request.Snapshot = Snapshot;
	Request.WorkspaceManager = &Manager;
	Request.AttemptID = Handle.AttemptID;
	Request.ArtifactSetID = Handle.ArtifactSetID;
	Request.WorkerCountOverride = 0;
	Request.TestDelayPerRouteMilliseconds = 1;
	FRouteWaypointBatchGenerator Batch;
	TestTrue(TEXT("Shutdown fixture start"), Batch.Start(Request, Error));
	FPlatformProcess::SleepNoStats(0.01f);
	const double ShutdownStart = FPlatformTime::Seconds();
	Batch.Cancel();
	Batch.Wait();
	const double ShutdownSeconds = FPlatformTime::Seconds() - ShutdownStart;
	const FRouteWaypointBatchProgress Progress = Batch.GetProgress();
	TestEqual(TEXT("Cancel reaches terminal state"), Progress.GenerationState,
		ERouteWaypointBatchState::Cancelled);
	TestEqual(TEXT("Cancel leaves no active route"), Progress.ActiveRoutes, 0);
	TestEqual(TEXT("Cancel closes all route files"), Progress.ActiveRouteFiles, 0);
	TestTrue(TEXT("Cancel shutdown is bounded"), ShutdownSeconds < 5.0);
	TestTrue(TEXT("Cancel staging cleanup"), Manager.AbortGeneration(Error));
	Manager.Shutdown();
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	AddInfo(FString::Printf(
		TEXT("C2B_SHUTDOWN Seconds=%.6f Workers=%d PeakOpenFiles=%d Completed=%d Cancelled=%d"),
		ShutdownSeconds, Progress.WorkerCount, Progress.PeakOpenRouteFiles,
		Progress.CompletedRoutes, Progress.CancellationCount));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointC2B10KBenchmarkTest,
	"DroneGCSBenchmark.RouteWaypointBatch.2DC2B.Synthetic10Kx16",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointC2B10KBenchmarkTest::RunTest(const FString& Parameters)
{
	using namespace RouteWaypointC2BBenchmarkPrivate;
	const TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> Data =
		MakeSyntheticData(16, 10000);
	FString Error;
	const FRouteWaypointGenerationSnapshot Snapshot = MakeSnapshot(Data, Error);
	const FRouteArtifactBuildIdentity Build = MakeBuild(Snapshot);
	const FRouteArtifactWorkspaceAuthorityGate Gate = MakeGate(Build, Snapshot);
	const FString Root = BenchmarkRoot(TEXT("10k-x16"));
	FRouteArtifactWorkspaceManager Manager;
	FRouteArtifactGenerationHandle Handle;
	TestTrue(TEXT("10K benchmark root"), Manager.SetConfiguredWorkspaceRoot(Root, Error));
	TestTrue(TEXT("10K benchmark begin"),
		Manager.BeginGeneration(Build, Gate, Handle, Error));
	FRouteWaypointBatchRequest Request;
	Request.Snapshot = Snapshot;
	Request.WorkspaceManager = &Manager;
	Request.AttemptID = Handle.AttemptID;
	Request.ArtifactSetID = Handle.ArtifactSetID;
	Request.WorkerCountOverride = 0;
	FRouteWaypointBatchGenerator Batch;
	const FProcessSample Before = CaptureProcessSample();
	FProcessMonitor Monitor;
	Monitor.Start();
	const double Start = FPlatformTime::Seconds();
	TestTrue(TEXT("10K benchmark start"), Batch.Start(Request, Error));
	Batch.Wait();
	const double Elapsed = FPlatformTime::Seconds() - Start;
	Monitor.Stop();
	double PeakCpu = 0.0;
	uint64 PeakWorkingSet = 0;
	uint64 PeakPrivate = 0;
	uint32 PeakHandles = 0;
	Monitor.Read(PeakCpu, PeakWorkingSet, PeakPrivate, PeakHandles);
	const FProcessSample After = CaptureProcessSample();
	const FRouteWaypointBatchProgress Progress = Batch.GetProgress();
	TestEqual(TEXT("10K completed files"), Progress.CompletedRoutes, 10000);
	TestEqual(TEXT("10K rows"), Progress.RowsWritten, static_cast<int64>(160000));
	TestTrue(TEXT("10K bounded workers"), Progress.WorkerCount <= 8);
	TestTrue(TEXT("10K bounded open files"),
		Progress.PeakOpenRouteFiles <= Progress.WorkerCount);
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(
		Files, *Handle.StagingRoot, TEXT("*.waypoints"), true, false, false);
	TestEqual(TEXT("10K physical files"), Files.Num(), 10000);
	const double CleanupStart = FPlatformTime::Seconds();
	TestTrue(TEXT("10K abort cleanup"), Manager.AbortGeneration(Error));
	Manager.Shutdown();
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const double CleanupSeconds = FPlatformTime::Seconds() - CleanupStart;
	TestFalse(TEXT("10K workspace removed"),
		IFileManager::Get().DirectoryExists(*Root));
	AddInfo(FString::Printf(
		TEXT("C2B_10K Routes=10000 Frames=16 Files=%d Rows=%lld Bytes=%lld Elapsed=%.6f RoutesPerSec=%.3f RowsPerSec=%.3f Workers=%d PeakWorkers=%d PeakOpenFiles=%d HandlesBefore=%u HandlesPeak=%u HandlesAfter=%u WorkingSetDelta=%lld PrivateDelta=%lld PeakWorkingSet=%llu PeakPrivate=%llu PeakCPU=%.3f Cleanup=%.6f"),
		Files.Num(), Progress.RowsWritten, Progress.BytesWritten, Elapsed,
		Elapsed > 0.0 ? 10000.0 / Elapsed : 0.0,
		Elapsed > 0.0 ? Progress.RowsWritten / Elapsed : 0.0,
		Progress.WorkerCount, Progress.PeakActiveWorkers,
		Progress.PeakOpenRouteFiles, Before.HandleCount, PeakHandles,
		After.HandleCount,
		static_cast<int64>(After.WorkingSetBytes) - static_cast<int64>(Before.WorkingSetBytes),
		static_cast<int64>(After.PrivateBytes) - static_cast<int64>(Before.PrivateBytes),
		PeakWorkingSet, PeakPrivate, PeakCpu, CleanupSeconds));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointC2BRealBenchmarkTest,
	"DroneGCSBenchmark.RouteWaypointBatch.2DC2B.Real1000x9100",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointC2BRealBenchmarkTest::RunTest(const FString& Parameters)
{
	using namespace RouteWaypointC2BBenchmarkPrivate;
	const FString NpzPath = FPlatformMisc::GetEnvironmentVariable(TEXT("DRONEGCS_C2B_NPZ"));
	const FString Mode = FPlatformMisc::GetEnvironmentVariable(TEXT("DRONEGCS_C2B_MODE"));
	if (NpzPath.IsEmpty() || !IFileManager::Get().FileExists(*NpzPath))
	{
		AddError(TEXT("DRONEGCS_C2B_NPZ must name an existing benchmark NPZ."));
		return false;
	}

	TSharedPtr<FRealBenchmarkState, ESPMode::ThreadSafe> State =
		MakeShared<FRealBenchmarkState, ESPMode::ThreadSafe>();
	State->Test = this;
	State->EndToEndStartSeconds = FPlatformTime::Seconds();
	State->BeforeLoad = CaptureProcessSample();
	FRoutePreviewData Loaded;
	FString Error;
	const double LoadStart = FPlatformTime::Seconds();
	if (!FRoutePreviewNpzLoader::Load(NpzPath, Loaded, Error))
	{
		AddError(Error);
		return false;
	}
	State->LoadSeconds = FPlatformTime::Seconds() - LoadStart;
	TestEqual(TEXT("Real NPZ RouteCount"), Loaded.DroneCount, 1000);
	TestEqual(TEXT("Real NPZ FrameCount"), Loaded.FrameCount, 9100);
	State->Data = MakeShared<FRoutePreviewData, ESPMode::ThreadSafe>();
	*State->Data = MoveTemp(Loaded);

	const double SnapshotStart = FPlatformTime::Seconds();
	State->Snapshot = MakeSnapshot(State->Data, Error);
	State->SnapshotSeconds = FPlatformTime::Seconds() - SnapshotStart;
	if (!State->Snapshot.IsValid())
	{
		AddError(Error.IsEmpty() ? TEXT("Benchmark snapshot is invalid.") : Error);
		return false;
	}
	State->AfterSnapshot = CaptureProcessSample();
	TestTrue(TEXT("Real snapshot shares Positions allocation"),
		State->Snapshot.RouteData->PositionsENU.GetData()
			== State->Data->PositionsENU.GetData());
	TestTrue(TEXT("Real snapshot shares Colors allocation"),
		State->Snapshot.RouteData->Colors.GetData() == State->Data->Colors.GetData());

	const TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> CalibrationData =
		MakeRoutePrefix(State->Data, 128);
	const FRouteWaypointGenerationSnapshot CalibrationSnapshot =
		MakeSnapshot(CalibrationData, Error);
	for (const int32 Workers : {1, 2, 4, 8, 12, 16, 20})
	{
		LogSweep(*this, TEXT("NoDisk"), RunNoDiskSweep(CalibrationSnapshot, Workers));
		LogSweep(*this, TEXT("Filesystem"),
			RunFilesystemSweep(*this, CalibrationSnapshot, Workers));
	}
	LogHotspotMicrobench(*this, CalibrationSnapshot);

	if (Mode.Equals(TEXT("calibration"), ESearchCase::IgnoreCase))
	{
		State->Snapshot = FRouteWaypointGenerationSnapshot{};
		State->Data.Reset();
		AddInfo(TEXT("C2B_CALIBRATION_COMPLETE Routes=128 Frames=9100"));
		return true;
	}
	const TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> DeterminismData =
		MakeRoutePrefix(State->Data, 32);
	const FRouteWaypointGenerationSnapshot DeterminismSnapshot =
		MakeSnapshot(DeterminismData, Error);
	TArray<FRouteArtifactDigest256> SingleWorkerDigests;
	TArray<FRouteArtifactDigest256> DefaultWorkerDigests;
	TestTrue(TEXT("Real subset worker=1 determinism run"),
		RunFilesystemDigestSet(
			*this, DeterminismSnapshot, 1, SingleWorkerDigests));
	TestTrue(TEXT("Real subset default-worker determinism run"),
		RunFilesystemDigestSet(*this, DeterminismSnapshot,
			FRouteWaypointBatchGenerator::GetDefaultWorkerCount(
				DeterminismSnapshot.RouteCount), DefaultWorkerDigests));
	TestTrue(TEXT("Real subset exact route digest determinism"),
		DefaultWorkerDigests == SingleWorkerDigests);
	AddInfo(FString::Printf(
		TEXT("C2B_REAL_SUBSET_DETERMINISM Routes=%d Frames=%d WorkerA=1 WorkerB=%d ExactDigestMatch=%d"),
		DeterminismSnapshot.RouteCount, DeterminismSnapshot.FrameCount,
		FRouteWaypointBatchGenerator::GetDefaultWorkerCount(
			DeterminismSnapshot.RouteCount),
		DefaultWorkerDigests == SingleWorkerDigests ? 1 : 0));

	State->Root = BenchmarkRoot(TEXT("real-1000x9100"));
	State->Manager = MakeUnique<FRouteArtifactWorkspaceManager>();
	State->Build = MakeBuild(State->Snapshot);
	State->Gate = MakeGate(State->Build, State->Snapshot);
	const double BeginStart = FPlatformTime::Seconds();
	int64 FreeBytes = 0;
	int64 RequiredBytes = 0;
	if (!State->Manager->SetConfiguredWorkspaceRoot(State->Root, Error)
		|| !State->Manager->GetWorkspaceVolumeFreeBytes(FreeBytes, Error)
		|| !FRouteWaypointBatchGenerator::CheckDiskCapacity(
			9100, 1000, FreeBytes, RequiredBytes, Error)
		|| !State->Manager->BeginGeneration(
			State->Build, State->Gate, State->Handle, Error))
	{
		AddError(Error);
		return false;
	}
	State->BeginPreflightSeconds = FPlatformTime::Seconds() - BeginStart;
	State->Draft = MakeDraft(State->Build, State->Snapshot, State->Handle.ArtifactSetID);
	State->Batch = MakeUnique<FRouteWaypointBatchGenerator>();
	FRouteWaypointBatchRequest Request;
	Request.Snapshot = State->Snapshot;
	Request.WorkspaceManager = State->Manager.Get();
	Request.AttemptID = State->Handle.AttemptID;
	Request.ArtifactSetID = State->Handle.ArtifactSetID;
	Request.WorkerCountOverride = 0;
	State->BeforeGeneration = CaptureProcessSample();
	State->Monitor.Start();
	State->Sentinel.Start();
	State->GenerationStartSeconds = FPlatformTime::Seconds();
	State->LastHeartbeatSeconds = State->GenerationStartSeconds;
	if (!State->Batch->Start(Request, Error))
	{
		State->Monitor.Stop();
		double A = 0.0, B = 0.0, C = 0.0;
		State->Sentinel.Stop(A, B, C);
		AddError(Error);
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForRealBenchmarkCommand(State));
	return true;
}

#endif
