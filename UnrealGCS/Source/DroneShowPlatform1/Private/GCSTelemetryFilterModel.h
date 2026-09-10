#pragma once

#include "CoreMinimal.h"
#include "GCSTelemetryTypes.h"

// Presentation-only filtering of the existing #187 telemetry contract.
namespace GCSTelemetryFilter
{
enum class EKind : uint8 { Integer, Real, Text, Online };
struct FField { FName Key; FString Label; EKind Kind; };
struct FValue
{
	bool bValid = false;
	int64 Integer = 0;
	double Real = 0;
	FString Text;
};
struct FRule
{
	FName Field = TEXT("Battery");
	FString Operator = TEXT("<");
	FString Input = TEXT("30");
	bool bEnabled = true;
};
const TArray<FField>& Fields();
const FField* FindField(FName Key);
TArray<FString> Operators(EKind Kind);
bool ParseInteger(const FString& Input, int64& Out);
bool Validate(const FRule& Rule, FString& Error);
FValue Read(const FMDHKTelemetryTableRow& Row, FName Field, TOptional<bool> Online, bool bFresh);
bool Matches(const FMDHKTelemetryTableRow& Row, TOptional<bool> Online, bool bFresh,
	const TArray<FRule>& Rules, bool bAny);
}
