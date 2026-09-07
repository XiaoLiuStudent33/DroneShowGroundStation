#include "HoverLayerSelectionModel.h"

namespace
{
	bool LogicalDroneIDLess(const FString& Left, const FString& Right)
	{
		int32 LeftDigitStart = Left.Len();
		while (LeftDigitStart > 0 && FChar::IsDigit(Left[LeftDigitStart - 1]))
		{
			--LeftDigitStart;
		}
		int32 RightDigitStart = Right.Len();
		while (RightDigitStart > 0 && FChar::IsDigit(Right[RightDigitStart - 1]))
		{
			--RightDigitStart;
		}

		const FString LeftPrefix = Left.Left(LeftDigitStart);
		const FString RightPrefix = Right.Left(RightDigitStart);
		const int32 PrefixComparison = LeftPrefix.Compare(RightPrefix, ESearchCase::IgnoreCase);
		if (PrefixComparison != 0)
		{
			return PrefixComparison < 0;
		}
		if (LeftDigitStart < Left.Len() && RightDigitStart < Right.Len())
		{
			const int32 LeftNumber = FCString::Atoi(*Left.Mid(LeftDigitStart));
			const int32 RightNumber = FCString::Atoi(*Right.Mid(RightDigitStart));
			if (LeftNumber != RightNumber)
			{
				return LeftNumber < RightNumber;
			}
		}
		return Left < Right;
	}

	bool IsSeparated(
		const FHoverLayerPosition& Candidate,
		const TArray<const FHoverLayerPosition*>& Layer,
		const double MinimumSpacingSquared)
	{
		for (const FHoverLayerPosition* Existing : Layer)
		{
			const double DeltaEast = Candidate.EastMeters - Existing->EastMeters;
			const double DeltaNorth = Candidate.NorthMeters - Existing->NorthMeters;
			if (DeltaEast * DeltaEast + DeltaNorth * DeltaNorth
				<= MinimumSpacingSquared)
			{
				return false;
			}
		}
		return true;
	}
}

bool HoverLayerSelection::BuildPlan(
	const TArray<FHoverLayerPosition>& Positions,
	const double MinimumSpacingMeters,
	FHoverLayerPlan& OutPlan,
	FString& OutError)
{
	OutPlan = {};
	OutError.Reset();
	if (Positions.IsEmpty())
	{
		OutError = TEXT("没有可用于分层的无人机。");
		return false;
	}
	if (!FMath::IsFinite(MinimumSpacingMeters) || MinimumSpacingMeters <= 0.0)
	{
		OutError = TEXT("安全间距必须大于 0。");
		return false;
	}

	TArray<const FHoverLayerPosition*> Remaining;
	Remaining.Reserve(Positions.Num());
	TSet<FString> SeenDroneIDs;
	for (const FHoverLayerPosition& Position : Positions)
	{
		if (Position.DroneID.IsEmpty()
			|| !FMath::IsFinite(Position.EastMeters)
			|| !FMath::IsFinite(Position.NorthMeters))
		{
			OutError = TEXT("分层输入包含无效的无人机位置。");
			return false;
		}
		if (SeenDroneIDs.Contains(Position.DroneID))
		{
			OutError = FString::Printf(TEXT("无人机 %s 重复出现。"), *Position.DroneID);
			return false;
		}
		SeenDroneIDs.Add(Position.DroneID);
		Remaining.Add(&Position);
	}
	Remaining.Sort([](const FHoverLayerPosition& Left, const FHoverLayerPosition& Right)
	{
		return LogicalDroneIDLess(Left.DroneID, Right.DroneID);
	});

	OutPlan.MinimumSpacingMeters = MinimumSpacingMeters;
	for (const FHoverLayerPosition* Position : Remaining)
	{
		OutPlan.BaseDroneIDs.Add(Position->DroneID);
	}

	const double MinimumSpacingSquared = MinimumSpacingMeters * MinimumSpacingMeters;
	while (!Remaining.IsEmpty())
	{
		TArray<const FHoverLayerPosition*> CurrentLayer;
		TArray<const FHoverLayerPosition*> Deferred;
		for (const FHoverLayerPosition* Candidate : Remaining)
		{
			if (IsSeparated(*Candidate, CurrentLayer, MinimumSpacingSquared))
			{
				CurrentLayer.Add(Candidate);
			}
			else
			{
				Deferred.Add(Candidate);
			}
		}

		TArray<FString>& LayerIDs = OutPlan.Layers.AddDefaulted_GetRef();
		LayerIDs.Reserve(CurrentLayer.Num());
		for (const FHoverLayerPosition* Position : CurrentLayer)
		{
			LayerIDs.Add(Position->DroneID);
		}
		Remaining = MoveTemp(Deferred);
	}
	return true;
}

bool HoverLayerSelection::ValidateLayerSpacing(
	const TArray<FHoverLayerPosition>& Positions,
	const double MinimumSpacingMeters,
	FString& OutError)
{
	OutError.Reset();
	if (!FMath::IsFinite(MinimumSpacingMeters) || MinimumSpacingMeters <= 0.0)
	{
		OutError = TEXT("安全间距必须大于 0。");
		return false;
	}
	const double MinimumSpacingSquared = MinimumSpacingMeters * MinimumSpacingMeters;
	for (int32 LeftIndex = 0; LeftIndex < Positions.Num(); ++LeftIndex)
	{
		const FHoverLayerPosition& Left = Positions[LeftIndex];
		if (Left.DroneID.IsEmpty()
			|| !FMath::IsFinite(Left.EastMeters)
			|| !FMath::IsFinite(Left.NorthMeters))
		{
			OutError = TEXT("当前分层包含无效的无人机位置。");
			return false;
		}
		for (int32 RightIndex = LeftIndex + 1; RightIndex < Positions.Num(); ++RightIndex)
		{
			const FHoverLayerPosition& Right = Positions[RightIndex];
			const double DeltaEast = Left.EastMeters - Right.EastMeters;
			const double DeltaNorth = Left.NorthMeters - Right.NorthMeters;
			if (DeltaEast * DeltaEast + DeltaNorth * DeltaNorth <= MinimumSpacingSquared)
			{
				OutError = FString::Printf(
					TEXT("%s 与 %s 的水平间距不大于 %.1f m，请重新生成分层。"),
					*Left.DroneID,
					*Right.DroneID,
					MinimumSpacingMeters);
				return false;
			}
		}
	}
	return true;
}
