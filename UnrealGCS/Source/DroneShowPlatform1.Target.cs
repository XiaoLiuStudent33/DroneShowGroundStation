// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class DroneShowPlatform1Target : TargetRules
{
	public DroneShowPlatform1Target(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_5;
		ExtraModuleNames.Add("DroneShowPlatform1");

		// Run for Game builds only, never Editor/PIE. UBT also runs pre-build
		// scripts for cached makefiles. UAT must not be asked to skip Build.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PreBuildSteps.Add("powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"$(ProjectDir)\\..\\Tools\\Launcher\\PreparePayload.ps1\" -ProjectDirectory \"$(ProjectDir)\"");
			PreBuildSteps.Add("if errorlevel 1 exit /b 1");
		}
	}
}
