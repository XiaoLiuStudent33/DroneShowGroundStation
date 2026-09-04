#include "GCSTelemetryFilterModel.h"
#include <cerrno>
#include <cstdlib>

namespace GCSTelemetryFilter
{
const TArray<FField>& Fields()
{
	static const TArray<FField> Catalogue = {
		{TEXT("UavID"), TEXT("无人机 ID"), EKind::Integer},
		{TEXT("Online"), TEXT("在线状态"), EKind::Online},
		{TEXT("Battery"), TEXT("剩余电量 %"), EKind::Real},
		{TEXT("Temperature"), TEXT("电池温度 °C"), EKind::Real},
		{TEXT("Cell1"), TEXT("Cell 1 电压 V"), EKind::Real},
		{TEXT("Cell2"), TEXT("Cell 2 电压 V"), EKind::Real},
		{TEXT("Cell3"), TEXT("Cell 3 电压 V"), EKind::Real},
		{TEXT("Cell4"), TEXT("Cell 4 电压 V"), EKind::Real},
		{TEXT("Latitude"), TEXT("纬度"), EKind::Real},
		{TEXT("Longitude"), TEXT("经度"), EKind::Real},
		{TEXT("Altitude"), TEXT("海拔 AMSL m"), EKind::Real},
		{TEXT("TimeBootMs"), TEXT("启动时间 ms"), EKind::Integer},
		{TEXT("GPSUtc"), TEXT("GPS UTC"), EKind::Text},
		{TEXT("ValidFlags"), TEXT("有效标志"), EKind::Integer},
		{TEXT("RelativeAltitude"), TEXT("相对高度 m"), EKind::Real},
		{TEXT("FreeMemoryBytes"), TEXT("可用内存 bytes"), EKind::Integer},
		{TEXT("FlightSWVersion"), TEXT("固件版本"), EKind::Text},
		{TEXT("WaypointDistance"), TEXT("航点距离 m"), EKind::Real},
		{TEXT("CompassVariance"), TEXT("指南针方差"), EKind::Real},
		{TEXT("MagField1"), TEXT("磁场 1 mGauss"), EKind::Real},
		{TEXT("MagField2"), TEXT("磁场 2 mGauss"), EKind::Real},
		{TEXT("BatteryCycleCount"), TEXT("电池循环次数"), EKind::Integer},
		{TEXT("CellVoltageDeltaMv"), TEXT("电芯压差 mV"), EKind::Integer},
		{TEXT("EKFFlags"), TEXT("EKF Flags"), EKind::Integer},
		{TEXT("MissionSeq"), TEXT("当前航点"), EKind::Integer},
		{TEXT("MissionTotal"), TEXT("航点总数"), EKind::Integer},
		{TEXT("BatteryCurrent"), TEXT("电流 A"), EKind::Real},
		{TEXT("GPSFixType"), TEXT("GPS 定位类型"), EKind::Integer},
		{TEXT("SatellitesVisible"), TEXT("卫星数"), EKind::Integer},
		{TEXT("PayloadSchemaVersion"), TEXT("协议版本"), EKind::Integer},
		{TEXT("RGB"), TEXT("RGB"), EKind::Text},
		{TEXT("FlightCustomVersion"), TEXT("固件自定义版本"), EKind::Text},
		{TEXT("HardwareID"), TEXT("硬件 ID"), EKind::Text}
	};
	return Catalogue;
}
const FField* FindField(FName Key)
{
	return Fields().FindByPredicate([Key](const FField& Field) { return Field.Key == Key; });
}
TArray<FString> Operators(EKind Kind)
{
	if (Kind == EKind::Text) return {TEXT("="), TEXT("!="), TEXT("包含"), TEXT("不包含")};
	if (Kind == EKind::Online) return {TEXT("="), TEXT("!=")};
	return {TEXT("<"), TEXT("<="), TEXT("="), TEXT("!="), TEXT(">="), TEXT(">")};
}
bool ParseInteger(const FString& Input, int64& Out)
{
	FString Value = Input.TrimStartAndEnd();
	if (Value.IsEmpty()) return false;
	bool bNegative = Value.StartsWith(TEXT("-"));
	int32 Pos = (bNegative || Value.StartsWith(TEXT("+"))) ? 1 : 0;
	const bool bHex = Value.Mid(Pos, 2).Equals(TEXT("0x"), ESearchCase::IgnoreCase);
	if (bHex) Pos += 2;
	if (Pos == Value.Len()) return false;
	uint64 Result = 0;
	const uint64 Limit = bNegative ? (uint64(MAX_int64) + 1) : uint64(MAX_int64);
	const uint64 Base = bHex ? 16 : 10;
	for (; Pos < Value.Len(); ++Pos)
	{
		TCHAR Ch = FChar::ToLower(Value[Pos]);
		int32 Digit = Ch >= '0' && Ch <= '9' ? Ch - '0' : Ch >= 'a' && Ch <= 'f' ? Ch - 'a' + 10 : -1;
		if (Digit < 0 || uint64(Digit) >= Base || Result > (Limit - Digit) / Base) return false;
		Result = Result * Base + Digit;
	}
	Out = bNegative ? (Result == uint64(MAX_int64) + 1 ? MIN_int64 : -int64(Result)) : int64(Result);
	return true;
}
static bool ParseReal(const FString& Input, double& Out)
{
	const FString Trimmed = Input.TrimStartAndEnd();
	if (Trimmed.IsEmpty()) return false;
	FTCHARToUTF8 Utf8(*Trimmed);
	char* End = nullptr;
	errno = 0;
	Out = std::strtod(Utf8.Get(), &End);
	return End != Utf8.Get() && *End == '\0' && errno != ERANGE && FMath::IsFinite(Out);
}
static bool ParseOnline(const FString& Input, int64& Out)
{
	const FString S = Input.TrimStartAndEnd().ToLower();
	if (S == TEXT("在线") || S == TEXT("online") || S == TEXT("true") || S == TEXT("1")) { Out = 1; return true; }
	if (S == TEXT("离线") || S == TEXT("offline") || S == TEXT("false") || S == TEXT("0")) { Out = 0; return true; }
	return false;
}
static FString IDInput(const FRule& Rule)
{
	FString S = Rule.Input.TrimStartAndEnd();
	if (Rule.Field == TEXT("UavID") && S.StartsWith(TEXT("Drone"))) S.RightChopInline(5);
	return S;
}
bool Validate(const FRule& Rule, FString& Error)
{
	Error.Reset();
	const FField* Field = FindField(Rule.Field);
	if (!Field || !Operators(Field->Kind).Contains(Rule.Operator)) { Error = TEXT("请选择有效字段和比较符"); return false; }
	if (Rule.Input.TrimStartAndEnd().IsEmpty() || Rule.Input.Len() > 256) { Error = TEXT("条件值不能为空，最多 256 字符"); return false; }
	int64 I; double D;
	if (Field->Kind == EKind::Integer && !ParseInteger(IDInput(Rule), I)) Error = TEXT("请输入整数；标志支持 0x 十六进制");
	if (Field->Kind == EKind::Real && !ParseReal(Rule.Input, D)) Error = TEXT("请输入有效数字，不要附加单位");
	if (Field->Kind == EKind::Online && !ParseOnline(Rule.Input, I)) Error = TEXT("请输入 在线 / 离线（或 1 / 0）");
	return Error.IsEmpty();
}
FValue Read(const FMDHKTelemetryTableRow& R, FName Key, TOptional<bool> Online, bool bFresh)
{
	FValue V;
	auto Integer = [&V](int64 N, bool Valid = true) { V.bValid = Valid; V.Integer = N; V.Text = FString::Printf(TEXT("%lld"), N); };
	auto Real = [&V](double N, bool Valid = true) { V.bValid = Valid && FMath::IsFinite(N); V.Real = N; V.Text = FString::SanitizeFloat(N); };
	auto Text = [&V](const FString& S) { V.bValid = !S.IsEmpty() && S != TEXT("--") && S != TEXT("-1"); V.Text = S; };
	if (Key == TEXT("UavID"))
	{
		int32 Start = R.DroneID.Len();
		while (Start > 0 && FChar::IsDigit(R.DroneID[Start - 1])) --Start;
		int64 ID;
		if (ParseInteger(R.DroneID.Mid(Start), ID)) Integer(ID);
		return V;
	}
	if (Key == TEXT("Online"))
	{
		if (Online.IsSet()) { Integer(Online.GetValue() ? 1 : 0); V.Text = Online.GetValue() ? TEXT("在线") : TEXT("离线"); }
		return V;
	}
	if (!R.bReceived || !bFresh) return V;
	if (Key == TEXT("Battery")) Real(R.BatteryRemainingPercent, R.bBatteryRemainingValid);
	else if (Key == TEXT("Temperature")) Real(R.BatteryTemperatureCelsius, R.bBatteryTemperatureValid);
	else if (Key == TEXT("Cell1")) Real(R.Cell1Volts, R.bCell1Valid);
	else if (Key == TEXT("Cell2")) Real(R.Cell2Volts, R.bCell2Valid);
	else if (Key == TEXT("Cell3")) Real(R.Cell3Volts, R.bCell3Valid);
	else if (Key == TEXT("Cell4")) Real(R.Cell4Volts, R.bCell4Valid);
	else if (Key == TEXT("Latitude")) Real(R.LatitudeDeg, R.bGPSValid);
	else if (Key == TEXT("Longitude")) Real(R.LongitudeDeg, R.bGPSValid);
	else if (Key == TEXT("Altitude")) Real(R.AltitudeAmslMeters, R.bAltitudeValid);
	else if (Key == TEXT("TimeBootMs")) Integer(R.TimeBootMs);
	else if (Key == TEXT("GPSUtc")) Text(R.GPSUtc);
	else if (Key == TEXT("ValidFlags")) { Integer(R.ValidFlags); V.Text = R.ValidFlagsText; }
	else if (Key == TEXT("RelativeAltitude")) Real(R.RelativeAltitudeMeters);
	else if (Key == TEXT("FreeMemoryBytes")) Integer(R.FreeMemoryBytes);
	else if (Key == TEXT("FlightSWVersion")) Text(R.FlightSoftwareVersionText);
	else if (Key == TEXT("WaypointDistance")) Real(R.WaypointDistanceMeters);
	else if (Key == TEXT("CompassVariance")) Real(R.CompassVariance);
	else if (Key == TEXT("MagField1")) Real(R.MagneticField1Mgauss);
	else if (Key == TEXT("MagField2")) Real(R.MagneticField2Mgauss);
	else if (Key == TEXT("BatteryCycleCount")) Integer(R.BatteryCycleCount, R.BatteryCycleCount >= 0);
	else if (Key == TEXT("CellVoltageDeltaMv")) Integer(R.CellVoltageDeltaMv, R.CellVoltageDeltaMv >= 0);
	else if (Key == TEXT("EKFFlags")) { int64 N; if (ParseInteger(R.EKFFlagsText, N)) { Integer(N); V.Text = R.EKFFlagsText; } }
	else if (Key == TEXT("MissionSeq")) Integer(R.MissionSequence);
	else if (Key == TEXT("MissionTotal")) Integer(R.MissionTotal);
	else if (Key == TEXT("BatteryCurrent")) Real(R.BatteryCurrentAmps, R.BatteryCurrentAmps != -1.0);
	else if (Key == TEXT("GPSFixType")) Integer(R.GPSFixType);
	else if (Key == TEXT("SatellitesVisible")) Integer(R.SatellitesVisible, R.SatellitesVisible >= 0);
	else if (Key == TEXT("PayloadSchemaVersion")) Integer(R.PayloadSchemaVersion);
	else if (Key == TEXT("RGB")) Text(R.RGBText);
	else if (Key == TEXT("FlightCustomVersion")) Text(R.FlightCustomVersionText);
	else if (Key == TEXT("HardwareID")) Text(R.HardwareID);
	return V;
}
template<typename T> static bool Compare(T A, T B, const FString& Op)
{
	if (Op == TEXT("<")) return A < B;
	if (Op == TEXT("<=")) return A <= B;
	if (Op == TEXT("=")) return A == B;
	if (Op == TEXT("!=")) return A != B;
	if (Op == TEXT(">=")) return A >= B;
	return Op == TEXT(">") && A > B;
}
bool Matches(const FMDHKTelemetryTableRow& Row, TOptional<bool> Online, bool bFresh, const TArray<FRule>& Rules, bool bAny)
{
	int32 Enabled = 0;
	for (const FRule& Rule : Rules)
	{
		if (!Rule.bEnabled) continue;
		++Enabled;
		FString Error;
		bool Match = false;
		const FValue V = Read(Row, Rule.Field, Online, bFresh);
		if (V.bValid && Validate(Rule, Error))
		{
			const EKind Kind = FindField(Rule.Field)->Kind;
			int64 I = 0; double D = 0;
			if (Kind == EKind::Integer) { ParseInteger(IDInput(Rule), I); Match = Compare(V.Integer, I, Rule.Operator); }
			else if (Kind == EKind::Online) { ParseOnline(Rule.Input, I); Match = Compare(V.Integer, I, Rule.Operator); }
			else if (Kind == EKind::Real) { ParseReal(Rule.Input, D); Match = Compare(V.Real, D, Rule.Operator); }
			else
			{
				const FString S = Rule.Input.TrimStartAndEnd();
				if (Rule.Operator == TEXT("=")) Match = V.Text.Equals(S, ESearchCase::IgnoreCase);
				else if (Rule.Operator == TEXT("!=")) Match = !V.Text.Equals(S, ESearchCase::IgnoreCase);
				else if (Rule.Operator == TEXT("包含")) Match = V.Text.Contains(S);
				else Match = !V.Text.Contains(S);
			}
		}
		if (bAny && Match) return true;
		if (!bAny && !Match) return false;
	}
	return Enabled == 0 || !bAny;
}
}
