#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "RouteMissionCodec.h"

namespace
{
	namespace Mission = dronegcs::route_mission;
	namespace Wpl = dronegcs::wpl110;

	template <SIZE_T Size>
	bool TestBytes(
		FAutomationTestBase& Test,
		const TCHAR* Label,
		const std::array<std::uint8_t, Size>& Actual,
		const std::array<std::uint8_t, Size>& Expected)
	{
		bool bEqual = true;
		for (SIZE_T Index = 0; Index < Size; ++Index)
		{
			if (Actual[Index] != Expected[Index])
			{
				Test.AddError(FString::Printf(
					TEXT("%s byte %llu: actual=0x%02x expected=0x%02x"),
					Label,
					static_cast<unsigned long long>(Index),
					Actual[Index],
					Expected[Index]));
				bEqual = false;
			}
		}
		return bEqual;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteMissionCodecGoldenTest,
	"DroneGCS.RouteMission.01CrossBuildGolden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteMissionCodecGoldenTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	static_assert(Mission::MissionItemRecordBytesV1 == 38);
	static_assert(Mission::LegacyMissionFtpContainerBytesV1 == 10);
	Mission::Error SizeError;
	std::size_t PayloadBytes = 0;
	TestTrue(TEXT("One-item upload payload size checked"),
		Mission::CheckedLegacyMissionFtpUploadPayloadSizeV1(
			1, PayloadBytes, SizeError));
	TestEqual(TEXT("One-item upload payload is header plus record"),
		static_cast<int64>(PayloadBytes), static_cast<int64>(48));
	TestTrue(TEXT("9000-item upload payload size checked"),
		Mission::CheckedLegacyMissionFtpUploadPayloadSizeV1(
			9000, PayloadBytes, SizeError));
	TestEqual(TEXT("9000-item upload payload size"),
		static_cast<int64>(PayloadBytes), static_cast<int64>(342010));

	Wpl::Row Row;
	Row.Sequence = 0x1234;
	Row.Red = 1;
	Row.Green = 127;
	Row.Blue = 255;
	Row.LatitudeE7 = 0x12345678;
	Row.LongitudeE7 = -2;
	Row.AltitudeE7 = 15000000;
	const std::array<std::uint8_t, 38> ExpectedRecord = {
		0x00,0x00,0x80,0x3f, 0x00,0x00,0xfe,0x42,
		0x00,0x00,0x7f,0x43, 0x00,0x00,0x00,0x00,
		0x78,0x56,0x34,0x12, 0xfe,0xff,0xff,0xff,
		0x00,0x00,0xc0,0x3f, 0x34,0x12,0x10,0x00,
		0x00,0x00,0x03,0x00,0x01,0x00
	};
	std::array<std::uint8_t, Mission::MissionItemRecordBytesV1> ActualRecord{};
	Mission::Error Error;
	TestTrue(TEXT("UE build encodes the Mission record"),
		Mission::EncodeMissionItemRecordV1(Row, ActualRecord, Error));
	TestBytes(*this, TEXT("Mission record"), ActualRecord, ExpectedRecord);

	std::uint32_t AltitudeBits = 0;
	TestTrue(TEXT("UE build encodes halfway altitude"),
		Mission::AltitudeE7ToBinary32Bits(1310720234375LL, AltitudeBits, Error));
	TestEqual(TEXT("UE build preserves ties-to-even bits"), AltitudeBits, 0x48000002U);

	const std::array<std::uint8_t, 10> ExpectedContainer = {
		0x3d,0x76, 0x00,0x00, 0x01,0x00, 0x03,0x00, 0x02,0x00
	};
	std::array<std::uint8_t, Mission::LegacyMissionFtpContainerBytesV1> ActualContainer{};
	TestTrue(TEXT("UE build encodes the legacy compatibility container"),
		Mission::EncodeLegacyMissionFtpContainerV1(
			3, 2, Mission::SubsequentContainerOptionsV1, ActualContainer, Error));
	TestBytes(*this, TEXT("Legacy container"), ActualContainer, ExpectedContainer);
	return true;
}

#endif
