#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "HoverLayerSelectionModel.h"

namespace
{
	FHoverLayerPosition MakeHoverPosition(
		const TCHAR* DroneID,
		const double EastMeters,
		const double NorthMeters = 0.0)
	{
		FHoverLayerPosition Position;
		Position.DroneID = DroneID;
		Position.EastMeters = EastMeters;
		Position.NorthMeters = NorthMeters;
		return Position;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoverLayerSelectionSpacingTest,
	"DroneGCS.HoverLayers.01SeparatesNearbyDrones",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHoverLayerSelectionSpacingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FHoverLayerPosition> Positions{
		MakeHoverPosition(TEXT("Drone0"), 0.0),
		MakeHoverPosition(TEXT("Drone1"), 0.5),
		MakeHoverPosition(TEXT("Drone2"), 1.0),
		MakeHoverPosition(TEXT("Drone3"), 1.5)
	};
	FHoverLayerPlan Plan;
	FString Error;
	TestTrue(TEXT("Plan builds"), HoverLayerSelection::BuildPlan(Positions, 0.6, Plan, Error));
	if (!TestEqual(TEXT("Two layers"), Plan.Layers.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("Layer 1 first drone"), Plan.Layers[0][0], FString(TEXT("Drone0")));
	TestEqual(TEXT("Layer 1 second drone"), Plan.Layers[0][1], FString(TEXT("Drone2")));
	TestEqual(TEXT("Layer 2 first drone"), Plan.Layers[1][0], FString(TEXT("Drone1")));
	TestEqual(TEXT("Layer 2 second drone"), Plan.Layers[1][1], FString(TEXT("Drone3")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoverLayerSelectionNoLegacyCapTest,
	"DroneGCS.HoverLayers.02NoLegacyLayerCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHoverLayerSelectionNoLegacyCapTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TArray<FHoverLayerPosition> Positions;
	for (int32 Index = 0; Index < 12; ++Index)
	{
		Positions.Add(MakeHoverPosition(
			*FString::Printf(TEXT("Drone%d"), Index),
			0.0));
	}
	FHoverLayerPlan Plan;
	FString Error;
	TestTrue(TEXT("Plan builds"), HoverLayerSelection::BuildPlan(Positions, 0.6, Plan, Error));
	TestEqual(TEXT("All colocated drones receive distinct layers"), Plan.Layers.Num(), 12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoverLayerSelectionRevalidationTest,
	"DroneGCS.HoverLayers.03RevalidatesCurrentPositions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHoverLayerSelectionRevalidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FString Error;
	TArray<FHoverLayerPosition> Positions{
		MakeHoverPosition(TEXT("Drone0"), 0.0),
		MakeHoverPosition(TEXT("Drone1"), 0.61)
	};
	TestTrue(
		TEXT("Spacing above threshold is valid"),
		HoverLayerSelection::ValidateLayerSpacing(Positions, 0.6, Error));
	Positions[1].EastMeters = 0.59;
	TestFalse(
		TEXT("Movement inside threshold is rejected"),
		HoverLayerSelection::ValidateLayerSpacing(Positions, 0.6, Error));
	TestTrue(TEXT("Rejection names both drones"), Error.Contains(TEXT("Drone0")) && Error.Contains(TEXT("Drone1")));
	return true;
}

#endif
