// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class DroneShowPlatform1 : ModuleRules
{
	public DroneShowPlatform1(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"UMG",
			"Slate",
			"SlateCore",
			"Sockets",
			"Networking",
			"Niagara"
		});

		PublicIncludePaths.Add(
			Path.Combine(ModuleDirectory, "..", "ThirdParty", "Mavlink", "include"));
		string ProtocolIncludePath = Path.GetFullPath(
			Path.Combine(ModuleDirectory, "..", "..", "..", "Shared", "DSPGCSProtocol"));
		if (!Directory.Exists(ProtocolIncludePath))
		{
			// Local Unicode-path build compatibility: C:\DroneShowPlatform1 is a
			// junction to this project, so its workspace sibling is exposed by a
			// second junction during development. Normal checkouts use the relative
			// path above and never enter this branch.
			ProtocolIncludePath = Path.Combine(
				Path.GetPathRoot(ModuleDirectory),
				"DroneShowPlatformShared",
				"DSPGCSProtocol");
		}
		PublicIncludePaths.Add(ProtocolIncludePath);

		string TransportIncludePath = Path.GetFullPath(
			Path.Combine(ModuleDirectory, "..", "..", "..", "Shared", "DSPGCSTransport"));
		if (!Directory.Exists(TransportIncludePath))
		{
			TransportIncludePath = Path.Combine(
				Path.GetPathRoot(ModuleDirectory),
				"DroneShowPlatformShared",
				"DSPGCSTransport");
		}
		PublicIncludePaths.Add(TransportIncludePath);

		string RouteWaypointIncludePath = Path.GetFullPath(
			Path.Combine(ModuleDirectory, "..", "..", "..", "Shared", "RouteWaypoint"));
		if (!Directory.Exists(RouteWaypointIncludePath))
		{
			RouteWaypointIncludePath = Path.Combine(
				Path.GetPathRoot(ModuleDirectory),
				"DroneShowPlatformShared",
				"RouteWaypoint");
		}
		PublicIncludePaths.Add(RouteWaypointIncludePath);

		string RouteArtifactIncludePath = Path.GetFullPath(
			Path.Combine(ModuleDirectory, "..", "..", "..", "Shared", "RouteArtifact"));
		if (!Directory.Exists(RouteArtifactIncludePath))
		{
			RouteArtifactIncludePath = Path.Combine(
				Path.GetPathRoot(ModuleDirectory),
				"DroneShowPlatformShared",
				"RouteArtifact");
		}
		PublicIncludePaths.Add(RouteArtifactIncludePath);

		string RouteMissionIncludePath = Path.GetFullPath(
			Path.Combine(ModuleDirectory, "..", "..", "..", "Shared", "RouteMission"));
		if (!Directory.Exists(RouteMissionIncludePath))
		{
			RouteMissionIncludePath = Path.Combine(
				Path.GetPathRoot(ModuleDirectory),
				"DroneShowPlatformShared",
				"RouteMission");
		}
		PublicIncludePaths.Add(RouteMissionIncludePath);

		string DroneIdentityIncludePath = Path.GetFullPath(
			Path.Combine(ModuleDirectory, "..", "..", "..", "Shared", "DroneIdentity"));
		if (!Directory.Exists(DroneIdentityIncludePath))
		{
			DroneIdentityIncludePath = Path.Combine(
				Path.GetPathRoot(ModuleDirectory),
				"DroneShowPlatformShared",
				"DroneIdentity");
		}
		PublicIncludePaths.Add(DroneIdentityIncludePath);

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CesiumRuntime",
			"EngineSettings"
		});
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.AddRange(new string[]
			{
				"Ole32.lib",
				"Shell32.lib",
				"Uuid.lib",
				"Bcrypt.lib"
			});
		}
		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
