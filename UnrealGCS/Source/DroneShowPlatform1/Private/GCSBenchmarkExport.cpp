#include "GCSBenchmarkExport.h"

namespace
{
	double NormalizeThreeDecimalZero(const double Value)
	{
		return FMath::Abs(Value) < 0.0005 ? 0.0 : Value;
	}
}

bool GCSBenchmarkExport::BuildLegacyBenchmarkText(
	const TArray<FGCSBenchmarkExportRow>& Rows,
	const int32 OriginDisplayID,
	const double OriginLatitude,
	const double OriginLongitude,
	FString& OutText,
	FString& OutError)
{
	OutText.Reset();
	OutError.Reset();
	if (Rows.IsEmpty())
	{
		OutError = TEXT("没有可导出的无人机");
		return false;
	}
	if (!FMath::IsFinite(OriginLatitude)
		|| !FMath::IsFinite(OriginLongitude)
		|| OriginLatitude < -90.0
		|| OriginLatitude > 90.0
		|| OriginLongitude < -180.0
		|| OriginLongitude > 180.0)
	{
		OutError = TEXT("桩基原点经纬度无效");
		return false;
	}

	TArray<FGCSBenchmarkExportRow> SortedRows = Rows;
	SortedRows.Sort([](const FGCSBenchmarkExportRow& Left, const FGCSBenchmarkExportRow& Right)
	{
		return Left.DisplayID < Right.DisplayID;
	});

	TSet<int32> SeenDisplayIDs;
	bool bFoundOrigin = false;
	for (const FGCSBenchmarkExportRow& Row : SortedRows)
	{
		if (Row.DisplayID < 0)
		{
			OutError = TEXT("存在无效的无人机显示编号");
			return false;
		}
		if (SeenDisplayIDs.Contains(Row.DisplayID))
		{
			OutError = FString::Printf(TEXT("无人机显示编号 %d 重复"), Row.DisplayID);
			return false;
		}
		if (!FMath::IsFinite(Row.EastMeters) || !FMath::IsFinite(Row.NorthMeters))
		{
			OutError = FString::Printf(TEXT("无人机编号 %d 的相对坐标无效"), Row.DisplayID);
			return false;
		}
		SeenDisplayIDs.Add(Row.DisplayID);
		bFoundOrigin |= Row.DisplayID == OriginDisplayID;
	}
	if (!bFoundOrigin)
	{
		OutError = TEXT("导出列表中未找到桩基原点无人机");
		return false;
	}

	const int64 OriginLatitudeE7 = FMath::RoundToInt64(OriginLatitude * 10000000.0);
	const int64 OriginLongitudeE7 = FMath::RoundToInt64(OriginLongitude * 10000000.0);
	for (const FGCSBenchmarkExportRow& Row : SortedRows)
	{
		const double EastMeters = NormalizeThreeDecimalZero(Row.EastMeters);
		const double NorthMeters = NormalizeThreeDecimalZero(Row.NorthMeters);
		if (Row.DisplayID == OriginDisplayID)
		{
			OutText += FString::Printf(
				TEXT("%d %.3f %.3f %lld %lld\r\n"),
				Row.DisplayID,
				EastMeters,
				NorthMeters,
				static_cast<long long>(OriginLatitudeE7),
				static_cast<long long>(OriginLongitudeE7));
		}
		else
		{
			OutText += FString::Printf(
				TEXT("%d %.3f %.3f\r\n"),
				Row.DisplayID,
				EastMeters,
				NorthMeters);
		}
	}
	return true;
}
