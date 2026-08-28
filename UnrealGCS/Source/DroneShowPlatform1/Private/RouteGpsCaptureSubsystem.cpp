#include "RouteGpsCaptureSubsystem.h"

#include "DroneData.h"
#include "FleetManagerSubsystem.h"
#include "GCSUIBridgeSubsystem.h"
#include "RouteArtifactTypes.h"
#include "RouteAssignmentGeo.h"

#include "Containers/StringConv.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"

#include <cmath>

namespace RouteGpsCapturePrivate
{
	double Median(TArray<double> Values)
	{
		if (Values.IsEmpty()) return 0.0;
		Values.Sort();
		const int32 Middle = Values.Num() / 2;
		return (Values.Num() & 1) != 0
			? Values[Middle]
			: (Values[Middle - 1] + Values[Middle]) * 0.5;
	}

	float MedianFloat(TArray<float> Values)
	{
		if (Values.IsEmpty()) return 0.0f;
		Values.Sort();
		const int32 Middle = Values.Num() / 2;
		return (Values.Num() & 1) != 0
			? Values[Middle]
			: (Values[Middle - 1] + Values[Middle]) * 0.5f;
	}

	bool IsCoordinateValid(const double Latitude, const double Longitude)
	{
		return FMath::IsFinite(Latitude) && Latitude >= -90.0 && Latitude <= 90.0
			&& FMath::IsFinite(Longitude) && Longitude >= -180.0 && Longitude <= 180.0
			&& !(Latitude == 0.0 && Longitude == 0.0);
	}

	bool ProjectHorizontal(
		const RouteAssignmentGeo::FLocalEnuOriginContext& Origin,
		const double Latitude,
		const double Longitude,
		double& OutEast,
		double& OutNorth)
	{
		const RouteAssignmentGeo::FEcefPosition Ecef =
			RouteAssignmentGeo::GeodeticToEcef(Latitude, Longitude, 0.0);
		const double DeltaX = Ecef.X - Origin.OriginEcef.X;
		const double DeltaY = Ecef.Y - Origin.OriginEcef.Y;
		const double DeltaZ = Ecef.Z - Origin.OriginEcef.Z;
		OutEast = -Origin.SinLongitude * DeltaX + Origin.CosLongitude * DeltaY;
		OutNorth = -Origin.SinLatitude * Origin.CosLongitude * DeltaX
			- Origin.SinLatitude * Origin.SinLongitude * DeltaY
			+ Origin.CosLatitude * DeltaZ;
		return FMath::IsFinite(OutEast) && FMath::IsFinite(OutNorth);
	}

	double HorizontalDistanceMeters(
		const double LatitudeA,
		const double LongitudeA,
		const double LatitudeB,
		const double LongitudeB)
	{
		RouteAssignmentGeo::FLocalEnuOriginContext Origin;
		FString Error;
		if (!RouteAssignmentGeo::BuildLocalEnuOriginContext(
			LatitudeA, LongitudeA, 0.0, Origin, Error))
		{
			return TNumericLimits<double>::Max();
		}
		double East = 0.0;
		double North = 0.0;
		return ProjectHorizontal(Origin, LatitudeB, LongitudeB, East, North)
			? std::hypot(East, North)
			: TNumericLimits<double>::Max();
	}

	void AppendBigEndianU32(TArray<uint8>& Bytes, const uint32 Value)
	{
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>(Value & 0xff));
	}
}

void URouteGpsCaptureSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UFleetManagerSubsystem>();
	Collection.InitializeDependency<UGCSUIBridgeSubsystem>();
	CaptureAuthoritySessionID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);

	if (UGCSUIBridgeSubsystem* Bridge = GetGameInstance()->GetSubsystem<UGCSUIBridgeSubsystem>())
	{
		SelectionChangedHandle = Bridge->OnCommandSelectionChanged.AddUObject(
			this, &URouteGpsCaptureSubsystem::HandleSelectionChanged);
	}
	if (UFleetManagerSubsystem* Fleet =
		GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>())
	{
		Fleet->OnDataBatchApplied.AddDynamic(
			this, &URouteGpsCaptureSubsystem::HandleFleetDataApplied);
	}
	FreshnessTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &URouteGpsCaptureSubsystem::TickFreshness),
		0.1f);
}

void URouteGpsCaptureSubsystem::Deinitialize()
{
	if (UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>() : nullptr)
	{
		Fleet->OnDataBatchApplied.RemoveDynamic(
			this, &URouteGpsCaptureSubsystem::HandleFleetDataApplied);
	}
	if (UGCSUIBridgeSubsystem* Bridge = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UGCSUIBridgeSubsystem>() : nullptr)
	{
		Bridge->OnCommandSelectionChanged.Remove(SelectionChangedHandle);
	}
	if (FreshnessTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(FreshnessTickerHandle);
		FreshnessTickerHandle.Reset();
	}
	WindowsByDroneID.Reset();
	CurrentSnapshot = FRouteGpsCaptureSnapshot{};
	LastCommandSelectionSnapshot = FRouteGpsCaptureSnapshot{};
	CaptureAuthoritySessionID.Reset();
	Super::Deinitialize();
}

bool URouteGpsCaptureSubsystem::ValidatePolicy(
	const FRouteGpsCapturePolicy& Policy,
	FString& OutError)
{
	OutError.Reset();
	if (Policy.ProfileName.TrimStartAndEnd().IsEmpty()
		|| Policy.RequiredFixType <= 0 || Policy.MaxSampleAgeMilliseconds <= 0
		|| Policy.MinWindowDurationMilliseconds <= 0 || Policy.MinUniqueSamples < 2
		|| !FMath::IsFinite(Policy.MaxMedianResidualMeters)
		|| Policy.MaxMedianResidualMeters <= 0.0
		|| !FMath::IsFinite(Policy.MaxRobustSpreadMeters)
		|| Policy.MaxRobustSpreadMeters <= 0.0
		|| !FMath::IsFinite(Policy.MaxSampleResidualMeters)
		|| Policy.MaxSampleResidualMeters <= 0.0
		|| !FMath::IsFinite(Policy.MaxJumpDistanceMeters)
		|| Policy.MaxJumpDistanceMeters <= 0.0
		|| Policy.CaptureTimeoutMilliseconds < Policy.MinWindowDurationMilliseconds
		|| Policy.CaptureTimeoutMilliseconds <= 0)
	{
		OutError = TEXT("GPS Capture Policy must provide positive, calibrated time, sample, stability, outlier, and jump gates.");
		return false;
	}
	return true;
}

bool URouteGpsCaptureSubsystem::LoadConfiguredPolicy(
	FRouteGpsCapturePolicy& OutPolicy,
	FString& OutError)
{
	static const TCHAR* Section = TEXT("DroneGCS.RouteGpsCapture");
	OutPolicy = FRouteGpsCapturePolicy{};
	OutError.Reset();
	if (!GConfig)
	{
		OutError = TEXT("GPS Capture configuration authority is unavailable.");
		return false;
	}

	int32 MaxSampleAge = 0;
	int32 MinWindowDuration = 0;
	int32 CaptureTimeout = 0;
	const bool bComplete =
		GConfig->GetString(Section, TEXT("ProfileName"), OutPolicy.ProfileName, GGameIni)
		&& GConfig->GetBool(Section, TEXT("HardwareValidated"), OutPolicy.bHardwareValidated, GGameIni)
		&& GConfig->GetInt(Section, TEXT("RequiredFixType"), OutPolicy.RequiredFixType, GGameIni)
		&& GConfig->GetInt(Section, TEXT("MaxSampleAgeMilliseconds"), MaxSampleAge, GGameIni)
		&& GConfig->GetInt(Section, TEXT("MinWindowDurationMilliseconds"), MinWindowDuration, GGameIni)
		&& GConfig->GetInt(Section, TEXT("MinUniqueSamples"), OutPolicy.MinUniqueSamples, GGameIni)
		&& GConfig->GetDouble(Section, TEXT("MaxMedianResidualMeters"), OutPolicy.MaxMedianResidualMeters, GGameIni)
		&& GConfig->GetDouble(Section, TEXT("MaxRobustSpreadMeters"), OutPolicy.MaxRobustSpreadMeters, GGameIni)
		&& GConfig->GetDouble(Section, TEXT("MaxSampleResidualMeters"), OutPolicy.MaxSampleResidualMeters, GGameIni)
		&& GConfig->GetDouble(Section, TEXT("MaxJumpDistanceMeters"), OutPolicy.MaxJumpDistanceMeters, GGameIni)
		&& GConfig->GetInt(Section, TEXT("CaptureTimeoutMilliseconds"), CaptureTimeout, GGameIni);
	if (!bComplete)
	{
		OutPolicy = FRouteGpsCapturePolicy{};
		OutError = TEXT("[DroneGCS.RouteGpsCapture] is incomplete; capture is fail-closed.");
		return false;
	}
	OutPolicy.MaxSampleAgeMilliseconds = MaxSampleAge;
	OutPolicy.MinWindowDurationMilliseconds = MinWindowDuration;
	OutPolicy.CaptureTimeoutMilliseconds = CaptureTimeout;
	return ValidatePolicy(OutPolicy, OutError);
}

bool URouteGpsCaptureSubsystem::ComputeSelectionDigest(
	const TArray<FString>& SelectedDroneIDs,
	FString& OutDigest,
	FString& OutError)
{
	TArray<uint8> Canonical;
	static constexpr ANSICHAR Domain[] = "DroneGCS.RouteGpsCapture.Selection.v1";
	Canonical.Append(reinterpret_cast<const uint8*>(Domain), sizeof(Domain) - 1);
	RouteGpsCapturePrivate::AppendBigEndianU32(Canonical, static_cast<uint32>(SelectedDroneIDs.Num()));
	for (const FString& DroneID : SelectedDroneIDs)
	{
		FTCHARToUTF8 Utf8(*DroneID);
		RouteGpsCapturePrivate::AppendBigEndianU32(Canonical, static_cast<uint32>(Utf8.Length()));
		Canonical.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
	FRouteArtifactDigest256 Digest;
	if (!FRouteArtifactSha256::HashBytes(Canonical, Digest, OutError))
	{
		OutDigest.Reset();
		return false;
	}
	OutDigest = Digest.ToLowerHex();
	return true;
}

bool URouteGpsCaptureSubsystem::BeginCapture(
	const FRouteGpsCapturePolicy& Policy,
	FString& OutError)
{
	const UGCSUIBridgeSubsystem* Bridge = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UGCSUIBridgeSubsystem>() : nullptr;
	if (!Bridge)
	{
		OutError = TEXT("Command Selection authority is unavailable.");
		return false;
	}
	return BeginCaptureInternal(
		Policy, Bridge->GetSelectedCommandDroneIDs(), true, OutError);
}

bool URouteGpsCaptureSubsystem::BeginCaptureForParticipants(
	const FRouteGpsCapturePolicy& Policy,
	const TArray<FString>& ParticipantDroneIDs,
	FString& OutError)
{
	return BeginCaptureInternal(Policy, ParticipantDroneIDs, false, OutError);
}

#if WITH_DEV_AUTOMATION_TESTS
bool URouteGpsCaptureSubsystem::BeginCaptureForSelectionForTest(
	const FRouteGpsCapturePolicy& Policy,
	const TArray<FString>& SelectedDroneIDs,
	FString& OutError)
{
	return BeginCaptureInternal(Policy, SelectedDroneIDs, true, OutError);
}
#endif

bool URouteGpsCaptureSubsystem::BeginCaptureInternal(
	const FRouteGpsCapturePolicy& Policy,
	TArray<FString> SelectedDroneIDs,
	const bool bCommandSelectionBound,
	FString& OutError)
{
	check(IsInGameThread());
	if (!ValidatePolicy(Policy, OutError)) return false;
	SelectedDroneIDs.Sort();
	for (int32 Index = SelectedDroneIDs.Num() - 1; Index > 0; --Index)
	{
		if (SelectedDroneIDs[Index].Equals(SelectedDroneIDs[Index - 1], ESearchCase::CaseSensitive))
		{
			OutError = TEXT("GPS Capture Selection contains a duplicate DroneID.");
			return false;
		}
	}
	if (SelectedDroneIDs.IsEmpty())
	{
		OutError = TEXT("GPS Capture requires at least one selected DroneID.");
		return false;
	}

	const UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>() : nullptr;
	if (!Fleet || Fleet->GetActiveFleetSessionID().IsEmpty())
	{
		OutError = TEXT("Fleet identity is unavailable for GPS Capture.");
		return false;
	}

	FRouteGpsCaptureSnapshot Candidate;
	Candidate.CaptureAuthoritySessionID = CaptureAuthoritySessionID;
	Candidate.CaptureRequestID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Candidate.CaptureRevision = ++LastIssuedCaptureRevision;
	Candidate.FleetSessionID = Fleet->GetActiveFleetSessionID();
	Candidate.SelectedDroneIDs = SelectedDroneIDs;
	Candidate.bCommandSelectionBound = bCommandSelectionBound;
	Candidate.Policy = Policy;
	Candidate.State = ERouteGpsCaptureState::Capturing;
	Candidate.StartedAtUtc = FDateTime::UtcNow();
	if (!ComputeSelectionDigest(SelectedDroneIDs, Candidate.SelectionDigest, OutError))
	{
		return false;
	}

	TMap<FString, FParticipantWindow> CandidateWindows;
	for (const FString& DroneID : SelectedDroneIDs)
	{
		FDroneData Drone;
		if (!Fleet->GetDroneByID(DroneID, Drone))
		{
			OutError = FString::Printf(TEXT("Selected GPS Capture participant is absent: %s"), *DroneID);
			return false;
		}
		if (!Drone.bOnline)
		{
			OutError = FString::Printf(TEXT("Selected GPS Capture participant is offline: %s"), *DroneID);
			return false;
		}
		if (Drone.EndpointIdentity.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("Selected GPS Capture participant has no EndpointIdentity: %s"), *DroneID);
			return false;
		}
		FRouteGpsCaptureParticipantResult Participant;
		Participant.DroneID = DroneID;
		Participant.EndpointIdentity = Drone.EndpointIdentity;
		Candidate.Participants.Add(MoveTemp(Participant));
		FParticipantWindow Window;
		Window.EndpointIdentity = Drone.EndpointIdentity;
		CandidateWindows.Add(DroneID, MoveTemp(Window));
	}

	// Installing B atomically supersedes A. Observations carrying A's request ID
	// are ignored by ObserveTelemetrySample and cannot mutate B.
	if (!bCommandSelectionBound && CurrentSnapshot.bCommandSelectionBound
		&& CurrentSnapshot.State != ERouteGpsCaptureState::Idle)
	{
		LastCommandSelectionSnapshot = CurrentSnapshot;
	}
	CurrentSnapshot = MoveTemp(Candidate);
	if (bCommandSelectionBound)
	{
		LastCommandSelectionSnapshot = CurrentSnapshot;
	}
	WindowsByDroneID = MoveTemp(CandidateWindows);
	CaptureStartedMonotonicSeconds = FPlatformTime::Seconds();
	OutError.Reset();
	return true;
}

void URouteGpsCaptureSubsystem::CancelCapture()
{
	if (CurrentSnapshot.State == ERouteGpsCaptureState::Capturing
		|| CurrentSnapshot.State == ERouteGpsCaptureState::Ready)
	{
		InvalidateCapture(ERouteGpsCaptureFailureReason::Superseded);
	}
}

void URouteGpsCaptureSubsystem::ObserveTelemetrySample(
	const FRouteGpsTelemetryObservation& Observation)
{
	check(IsInGameThread());
	if (CurrentSnapshot.State != ERouteGpsCaptureState::Capturing) return;
	if (!Observation.CaptureRequestID.IsEmpty()
		&& Observation.CaptureRequestID != CurrentSnapshot.CaptureRequestID)
	{
		return;
	}
	FParticipantWindow* Window = WindowsByDroneID.Find(Observation.DroneID);
	if (!Window) return;
	if (!Observation.bVehicleOnline)
	{
		InvalidateCapture(ERouteGpsCaptureFailureReason::Offline);
		return;
	}
	if (Observation.EndpointIdentity != Window->EndpointIdentity)
	{
		InvalidateCapture(ERouteGpsCaptureFailureReason::EndpointChanged);
		return;
	}
	if (Observation.GpsSampleTimestampUtcMs <= 0
		|| !RouteGpsCapturePrivate::IsCoordinateValid(
			Observation.Latitude, Observation.Longitude))
	{
		return;
	}

	if (!Window->Samples.IsEmpty())
	{
		const FUniqueSample& Previous = Window->Samples.Last();
		if (Observation.GpsSampleTimestampUtcMs < Previous.GpsTimestampUtcMs)
		{
			return;
		}
		if (Observation.GpsSampleTimestampUtcMs == Previous.GpsTimestampUtcMs)
		{
			const bool bSameSample = Observation.FixType == Previous.FixType
				&& Observation.Latitude == Previous.Latitude
				&& Observation.Longitude == Previous.Longitude
				&& Observation.AltitudeAMSL == Previous.AltitudeAMSL;
			if (!bSameSample)
			{
				InvalidateCapture(ERouteGpsCaptureFailureReason::UnstablePosition);
			}
			return;
		}
		const double JumpMeters = RouteGpsCapturePrivate::HorizontalDistanceMeters(
			Previous.Latitude, Previous.Longitude,
			Observation.Latitude, Observation.Longitude);
		if (!FMath::IsFinite(JumpMeters)
			|| JumpMeters > CurrentSnapshot.Policy.MaxJumpDistanceMeters)
		{
			InvalidateCapture(ERouteGpsCaptureFailureReason::UnstablePosition);
			return;
		}
	}

	// Every new GPS_RAW_INT identity participates in quality evaluation. A fix
	// downgrade is not allowed to leave an older RTK sample authoritative.
	if (!Observation.bGpsValid
		|| Observation.FixType < CurrentSnapshot.Policy.RequiredFixType)
	{
		InvalidateCapture(ERouteGpsCaptureFailureReason::FixQualityInsufficient);
		return;
	}

	FUniqueSample Sample;
	Sample.GpsTimestampUtcMs = Observation.GpsSampleTimestampUtcMs;
	Sample.Latitude = Observation.Latitude;
	Sample.Longitude = Observation.Longitude;
	Sample.AltitudeAMSL = Observation.AltitudeAMSL;
	Sample.bAltitudeValid = Observation.bAltitudeValid;
	Sample.FixType = Observation.FixType;
	Sample.FirstObservedUtcMs = Observation.TelemetryReceivedUtcMs;
	Sample.FirstObservedMonotonicSeconds = Observation.TelemetryReceivedMonotonicSeconds;
	Window->Samples.Add(Sample);
	for (FRouteGpsCaptureParticipantResult& Participant : CurrentSnapshot.Participants)
	{
		if (Participant.DroneID == Observation.DroneID)
		{
			Participant.FixType = Observation.FixType;
			Participant.FirstGpsSampleUtcMs = Window->Samples[0].GpsTimestampUtcMs;
			Participant.LastGpsSampleUtcMs = Observation.GpsSampleTimestampUtcMs;
			Participant.UniqueSampleCount = Window->Samples.Num();
			break;
		}
	}
	if (Window->Samples.Num() > 4096)
	{
		InvalidateCapture(ERouteGpsCaptureFailureReason::InsufficientSamples);
		return;
	}

	EvaluateCapture(
		Observation.TelemetryReceivedUtcMs,
		Observation.TelemetryReceivedMonotonicSeconds);
}

bool URouteGpsCaptureSubsystem::EvaluateParticipant(
	FRouteGpsCaptureParticipantResult& Participant,
	const FParticipantWindow& Window,
	const int64 NowUtcMs,
	const double NowMonotonicSeconds,
	ERouteGpsCaptureFailureReason& OutFailureReason) const
{
	(void)NowUtcMs;
	OutFailureReason = ERouteGpsCaptureFailureReason::None;
	if (Window.Samples.IsEmpty()) return false;
	const FUniqueSample& Latest = Window.Samples.Last();
	const double InitialAgeMs = FMath::Max(
		0.0,
		static_cast<double>(Latest.FirstObservedUtcMs - Latest.GpsTimestampUtcMs));
	const double MonotonicElapsedMs = FMath::Max(
		0.0,
		(NowMonotonicSeconds - Latest.FirstObservedMonotonicSeconds) * 1000.0);
	if (InitialAgeMs + MonotonicElapsedMs
		> static_cast<double>(CurrentSnapshot.Policy.MaxSampleAgeMilliseconds))
	{
		OutFailureReason = ERouteGpsCaptureFailureReason::GpsStale;
		return false;
	}
	if (Window.Samples.Num() < CurrentSnapshot.Policy.MinUniqueSamples)
	{
		return false;
	}
	if (Latest.GpsTimestampUtcMs - Window.Samples[0].GpsTimestampUtcMs
		< CurrentSnapshot.Policy.MinWindowDurationMilliseconds)
	{
		return false;
	}

	TArray<double> Latitudes;
	TArray<double> Longitudes;
	TArray<float> Altitudes;
	Latitudes.Reserve(Window.Samples.Num());
	Longitudes.Reserve(Window.Samples.Num());
	for (const FUniqueSample& Sample : Window.Samples)
	{
		Latitudes.Add(Sample.Latitude);
		Longitudes.Add(Sample.Longitude);
		if (Sample.bAltitudeValid && FMath::IsFinite(Sample.AltitudeAMSL))
		{
			Altitudes.Add(Sample.AltitudeAMSL);
		}
	}
	const double AnchorLatitude = RouteGpsCapturePrivate::Median(Latitudes);
	const double AnchorLongitude = RouteGpsCapturePrivate::Median(Longitudes);
	RouteAssignmentGeo::FLocalEnuOriginContext Origin;
	FString GeoError;
	if (!RouteAssignmentGeo::BuildLocalEnuOriginContext(
		AnchorLatitude, AnchorLongitude, 0.0, Origin, GeoError))
	{
		OutFailureReason = ERouteGpsCaptureFailureReason::GpsUnavailable;
		return false;
	}

	TArray<double> EastValues;
	TArray<double> NorthValues;
	EastValues.Reserve(Window.Samples.Num());
	NorthValues.Reserve(Window.Samples.Num());
	for (const FUniqueSample& Sample : Window.Samples)
	{
		double East = 0.0;
		double North = 0.0;
		if (!RouteGpsCapturePrivate::ProjectHorizontal(
			Origin, Sample.Latitude, Sample.Longitude, East, North))
		{
			OutFailureReason = ERouteGpsCaptureFailureReason::GpsUnavailable;
			return false;
		}
		EastValues.Add(East);
		NorthValues.Add(North);
	}
	const double CenterEast = RouteGpsCapturePrivate::Median(EastValues);
	const double CenterNorth = RouteGpsCapturePrivate::Median(NorthValues);
	TArray<double> Residuals;
	Residuals.Reserve(EastValues.Num());
	double MaxResidual = 0.0;
	for (int32 Index = 0; Index < EastValues.Num(); ++Index)
	{
		const double Residual = std::hypot(
			EastValues[Index] - CenterEast,
			NorthValues[Index] - CenterNorth);
		Residuals.Add(Residual);
		MaxResidual = FMath::Max(MaxResidual, Residual);
	}
	const double MedianResidual = RouteGpsCapturePrivate::Median(Residuals);
	TArray<double> AbsoluteDeviations;
	AbsoluteDeviations.Reserve(Residuals.Num());
	for (const double Residual : Residuals)
	{
		AbsoluteDeviations.Add(FMath::Abs(Residual - MedianResidual));
	}
	const double RobustSpread = RouteGpsCapturePrivate::Median(AbsoluteDeviations);
	if (MedianResidual > CurrentSnapshot.Policy.MaxMedianResidualMeters
		|| RobustSpread > CurrentSnapshot.Policy.MaxRobustSpreadMeters
		|| MaxResidual > CurrentSnapshot.Policy.MaxSampleResidualMeters)
	{
		OutFailureReason = ERouteGpsCaptureFailureReason::UnstablePosition;
		return false;
	}

	RouteAssignmentGeo::FGeodeticPosition Estimate;
	if (!RouteAssignmentGeo::LocalEnuToGeodetic(
		Origin, CenterEast, CenterNorth, 0.0, Estimate, GeoError))
	{
		OutFailureReason = ERouteGpsCaptureFailureReason::GpsUnavailable;
		return false;
	}
	Participant.FixType = Latest.FixType;
	Participant.FirstGpsSampleUtcMs = Window.Samples[0].GpsTimestampUtcMs;
	Participant.LastGpsSampleUtcMs = Latest.GpsTimestampUtcMs;
	Participant.UniqueSampleCount = Window.Samples.Num();
	Participant.EstimatedLatitude = Estimate.LatitudeDegrees;
	Participant.EstimatedLongitude = Estimate.LongitudeDegrees;
	Participant.AltitudeAMSL = RouteGpsCapturePrivate::MedianFloat(Altitudes);
	Participant.MedianResidualMeters = MedianResidual;
	Participant.RobustSpreadMeters = RobustSpread;
	Participant.MaxResidualMeters = MaxResidual;
	Participant.Status = ERouteGpsCaptureParticipantStatus::Ready;
	Participant.FailureReason = ERouteGpsCaptureFailureReason::None;
	return true;
}

void URouteGpsCaptureSubsystem::EvaluateCapture(
	const int64 NowUtcMs,
	const double NowMonotonicSeconds)
{
	// Ready is an immutable capture result. Freshness is a gate while building
	// the window; selection, endpoint, online, or Fleet-session mutations still
	// invalidate a Ready result through their dedicated handlers.
	if (CurrentSnapshot.State != ERouteGpsCaptureState::Capturing)
	{
		return;
	}
	bool bAllReady = true;
	for (FRouteGpsCaptureParticipantResult& Participant : CurrentSnapshot.Participants)
	{
		const FParticipantWindow* Window = WindowsByDroneID.Find(Participant.DroneID);
		ERouteGpsCaptureFailureReason Failure = ERouteGpsCaptureFailureReason::None;
		if (!Window || !EvaluateParticipant(
			Participant, *Window, NowUtcMs, NowMonotonicSeconds, Failure))
		{
			bAllReady = false;
			if (Failure == ERouteGpsCaptureFailureReason::GpsStale
				|| Failure == ERouteGpsCaptureFailureReason::UnstablePosition
				|| Failure == ERouteGpsCaptureFailureReason::GpsUnavailable)
			{
				InvalidateCapture(Failure);
				return;
			}
		}
	}
	if (bAllReady)
	{
		CurrentSnapshot.State = ERouteGpsCaptureState::Ready;
		CurrentSnapshot.CompletedAtUtc = FDateTime::UtcNow();
		return;
	}
	if ((NowMonotonicSeconds - CaptureStartedMonotonicSeconds) * 1000.0
		> static_cast<double>(CurrentSnapshot.Policy.CaptureTimeoutMilliseconds))
	{
		ERouteGpsCaptureFailureReason TimeoutReason =
			ERouteGpsCaptureFailureReason::InsufficientSamples;
		for (const FRouteGpsCaptureParticipantResult& Participant : CurrentSnapshot.Participants)
		{
			const FParticipantWindow* Window = WindowsByDroneID.Find(Participant.DroneID);
			if (!Window || Window->Samples.IsEmpty())
			{
				TimeoutReason = ERouteGpsCaptureFailureReason::GpsUnavailable;
				break;
			}
		}
		InvalidateCapture(TimeoutReason);
	}
}

void URouteGpsCaptureSubsystem::InvalidateCapture(
	const ERouteGpsCaptureFailureReason Reason)
{
	if (CurrentSnapshot.State == ERouteGpsCaptureState::Idle
		|| CurrentSnapshot.State == ERouteGpsCaptureState::Failed
		|| CurrentSnapshot.State == ERouteGpsCaptureState::Invalidated)
	{
		return;
	}
	CurrentSnapshot.State = Reason == ERouteGpsCaptureFailureReason::SelectionInvalidated
		|| Reason == ERouteGpsCaptureFailureReason::EndpointChanged
		|| Reason == ERouteGpsCaptureFailureReason::FleetSessionChanged
		|| Reason == ERouteGpsCaptureFailureReason::ParticipantMissing
		|| Reason == ERouteGpsCaptureFailureReason::Superseded
		? ERouteGpsCaptureState::Invalidated
		: ERouteGpsCaptureState::Failed;
	CurrentSnapshot.CompletedAtUtc = FDateTime::UtcNow();
	for (FRouteGpsCaptureParticipantResult& Participant : CurrentSnapshot.Participants)
	{
		Participant.Status = ERouteGpsCaptureParticipantStatus::Failed;
		Participant.FailureReason = Reason;
	}
}

void URouteGpsCaptureSubsystem::HandleSelectionChanged()
{
	if (CurrentSnapshot.bCommandSelectionBound
		&& (CurrentSnapshot.State == ERouteGpsCaptureState::Capturing
			|| CurrentSnapshot.State == ERouteGpsCaptureState::Ready))
	{
		InvalidateCapture(ERouteGpsCaptureFailureReason::SelectionInvalidated);
	}
	if (LastCommandSelectionSnapshot.State == ERouteGpsCaptureState::Capturing
		|| LastCommandSelectionSnapshot.State == ERouteGpsCaptureState::Ready)
	{
		LastCommandSelectionSnapshot.State = ERouteGpsCaptureState::Invalidated;
		LastCommandSelectionSnapshot.CompletedAtUtc = FDateTime::UtcNow();
		for (FRouteGpsCaptureParticipantResult& Participant :
			LastCommandSelectionSnapshot.Participants)
		{
			Participant.Status = ERouteGpsCaptureParticipantStatus::Failed;
			Participant.FailureReason =
				ERouteGpsCaptureFailureReason::SelectionInvalidated;
		}
	}
}

void URouteGpsCaptureSubsystem::HandleFleetDataApplied(
	const int64 FleetRevision,
	const int32 UpdatedDroneCount)
{
	(void)FleetRevision;
	(void)UpdatedDroneCount;
	const UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>() : nullptr;
	auto FindInvalidationReason = [Fleet](
		const FRouteGpsCaptureSnapshot& Snapshot,
		ERouteGpsCaptureFailureReason& OutReason)
	{
		OutReason = ERouteGpsCaptureFailureReason::None;
		if (Snapshot.State != ERouteGpsCaptureState::Capturing
			&& Snapshot.State != ERouteGpsCaptureState::Ready)
		{
			return false;
		}
		if (!Fleet || Fleet->GetActiveFleetSessionID() != Snapshot.FleetSessionID)
		{
			OutReason = ERouteGpsCaptureFailureReason::FleetSessionChanged;
			return true;
		}
		for (const FRouteGpsCaptureParticipantResult& Participant : Snapshot.Participants)
		{
			FDroneData Drone;
			if (!Fleet->GetDroneByID(Participant.DroneID, Drone))
			{
				OutReason = ERouteGpsCaptureFailureReason::ParticipantMissing;
				return true;
			}
			if (!Drone.bOnline)
			{
				OutReason = ERouteGpsCaptureFailureReason::Offline;
				return true;
			}
			if (Drone.EndpointIdentity != Participant.EndpointIdentity)
			{
				OutReason = ERouteGpsCaptureFailureReason::EndpointChanged;
				return true;
			}
		}
		return false;
	};

	ERouteGpsCaptureFailureReason CurrentReason =
		ERouteGpsCaptureFailureReason::None;
	if (FindInvalidationReason(CurrentSnapshot, CurrentReason))
	{
		InvalidateCapture(CurrentReason);
	}

	// An explicit Show-Origin capture may temporarily own CurrentSnapshot while
	// the last Command-Selection capture remains available to numbering. Keep
	// that saved authority subject to the same Fleet/endpoint invalidation.
	ERouteGpsCaptureFailureReason SavedReason =
		ERouteGpsCaptureFailureReason::None;
	if (FindInvalidationReason(LastCommandSelectionSnapshot, SavedReason))
	{
		LastCommandSelectionSnapshot.State =
			SavedReason == ERouteGpsCaptureFailureReason::Offline
			? ERouteGpsCaptureState::Failed
			: ERouteGpsCaptureState::Invalidated;
		LastCommandSelectionSnapshot.CompletedAtUtc = FDateTime::UtcNow();
		for (FRouteGpsCaptureParticipantResult& Participant :
			LastCommandSelectionSnapshot.Participants)
		{
			Participant.Status = ERouteGpsCaptureParticipantStatus::Failed;
			Participant.FailureReason = SavedReason;
		}
	}
}

bool URouteGpsCaptureSubsystem::TickFreshness(const float DeltaSeconds)
{
	(void)DeltaSeconds;
	EvaluateCapture(UtcNowMilliseconds(), FPlatformTime::Seconds());
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
void URouteGpsCaptureSubsystem::EvaluateAtForTest(
	const int64 NowUtcMs,
	const double NowMonotonicSeconds)
{
	EvaluateCapture(NowUtcMs, NowMonotonicSeconds);
}
#endif

bool URouteGpsCaptureSubsystem::GetCurrentSnapshot(
	FRouteGpsCaptureSnapshot& OutSnapshot) const
{
	OutSnapshot = CurrentSnapshot;
	return CurrentSnapshot.State != ERouteGpsCaptureState::Idle;
}

bool URouteGpsCaptureSubsystem::GetLatestCommandSelectionSnapshot(
	FRouteGpsCaptureSnapshot& OutSnapshot) const
{
	if (CurrentSnapshot.bCommandSelectionBound
		&& CurrentSnapshot.State != ERouteGpsCaptureState::Idle)
	{
		OutSnapshot = CurrentSnapshot;
		return true;
	}
	OutSnapshot = LastCommandSelectionSnapshot;
	return LastCommandSelectionSnapshot.State != ERouteGpsCaptureState::Idle;
}

int64 URouteGpsCaptureSubsystem::UtcNowMilliseconds()
{
	const FDateTime Now = FDateTime::UtcNow();
	return Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
}
