#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "GCSBenchmarkExport.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSBenchmarkExportFormatTest,
	"DroneGCS.Export.Benchmark.LegacyFormat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGCSBenchmarkExportFormatTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	TArray<FGCSBenchmarkExportRow> Rows;
	Rows.Add({ 2, -0.642, 0.557 });
	Rows.Add({ 0, 0.009, 0.011 });
	Rows.Add({ 1, -0.586, 1.135 });

	FString Text;
	FString Error;
	TestTrue(
		TEXT("Valid rows produce a benchmark payload"),
		GCSBenchmarkExport::BuildLegacyBenchmarkText(
			Rows,
			0,
			31.9327934,
			118.6380798,
			Text,
			Error));
	TestEqual(TEXT("Successful export has no error"), Error, FString());
	TestEqual(
		TEXT("Payload is sorted and matches the modified BGC2 legacy format"),
		Text,
		FString(
			TEXT("0 0.009 0.011 319327934 1186380798\r\n")
			TEXT("1 -0.586 1.135\r\n")
			TEXT("2 -0.642 0.557\r\n")));

	Rows.Add({ 1, 2.0, 3.0 });
	TestFalse(
		TEXT("Duplicate display numbers are rejected"),
		GCSBenchmarkExport::BuildLegacyBenchmarkText(
			Rows,
			0,
			31.9327934,
			118.6380798,
			Text,
			Error));
	TestTrue(TEXT("Duplicate-number failure explains the cause"), Error.Contains(TEXT("重复")));
	return true;
}

#endif
