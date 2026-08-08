/**
 * @brief 实现旧 UE -> Console:10006 的 GCS1 文本命令与 STATE 解析。
 *
 * 分类：Legacy / Compatibility Candidate；当前 Production 命令链不调用本文件。
 */
#include "DroneCommandSubsystem.h"

#include "Async/Async.h"
#include "Common/UdpSocketBuilder.h"
#include "Common/UdpSocketReceiver.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/ConfigCacheIni.h"
#include "Serialization/ArrayReader.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
	constexpr int32 ReceiveBufferBytes = 2 * 1024 * 1024;
}

void UDroneCommandSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	bool bNetworkEnabled = false;
	GConfig->GetBool(
		TEXT("DroneGCS.Network"),
		TEXT("Enabled"),
		bNetworkEnabled,
		GGameIni);
	if (!bNetworkEnabled)
	{
		UE_LOG(
			LogTemp,
			Log,
			TEXT("Drone GCS network adapter is disabled; FleetManager is using local data only."));
		return;
	}

	if (!CreateSocket())
	{
		UE_LOG(LogTemp, Error, TEXT("Drone GCS command UDP client failed to initialize."));
		return;
	}

	// PING is the only automatic request. It registers this UE endpoint for
	// passive telemetry updates and never sends a flight-control command.
	SendPing();

	PingTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateWeakLambda(
			this,
			[this](float)
			{
				CheckCommandTimeouts();
				SendPing();
				return true;
			}),
		2.0f);
}

void UDroneCommandSubsystem::Deinitialize()
{
	if (PingTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PingTickerHandle);
		PingTickerHandle.Reset();
	}

	if (Receiver)
	{
		Receiver->Stop();
		Receiver.Reset();
	}

	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}

	ServerEndpoint.Reset();
	PendingCommands.Reset();
	bCommandServiceOnline = false;

	Super::Deinitialize();
}

bool UDroneCommandSubsystem::SendPing()
{
	return SendCommand(TEXT("PING"));
}

bool UDroneCommandSubsystem::SendArm()
{
	return SendCommand(TEXT("ARM"));
}

bool UDroneCommandSubsystem::SendDisarm()
{
	return SendCommand(TEXT("DISARM"));
}

bool UDroneCommandSubsystem::SendGuided()
{
	return SendCommand(TEXT("GUIDED"));
}

bool UDroneCommandSubsystem::SendTakeoff(float AltitudeMeters)
{
	if (!FMath::IsWithinInclusive(AltitudeMeters, 0.5f, 20.0f))
	{
		BroadcastProtocolError(TEXT("BAD_TAKEOFF_ALTITUDE"));
		return false;
	}

	return SendCommand(
		TEXT("TAKEOFF"),
		{ FString::Printf(TEXT("ALT=%.2f"), AltitudeMeters) });
}

bool UDroneCommandSubsystem::SendLand()
{
	return SendCommand(TEXT("LAND"));
}

bool UDroneCommandSubsystem::SendRTL()
{
	return SendCommand(TEXT("RTL"));
}

bool UDroneCommandSubsystem::SendLoiter()
{
	return SendCommand(TEXT("LOITER"));
}

bool UDroneCommandSubsystem::SendHover()
{
	return SendCommand(TEXT("HOVER"));
}

bool UDroneCommandSubsystem::SendStop()
{
	return SendCommand(TEXT("STOP"));
}

bool UDroneCommandSubsystem::CreateSocket()
{
	FString ServerIpString = TEXT("192.168.1.254");
	int32 ServerPort = 10006;

	GConfig->GetString(
		TEXT("DroneGCS.Network"),
		TEXT("CommandServerIp"),
		ServerIpString,
		GGameIni);
	GConfig->GetInt(
		TEXT("DroneGCS.Network"),
		TEXT("CommandServerPort"),
		ServerPort,
		GGameIni);
	GConfig->GetFloat(
		TEXT("DroneGCS.Network"),
		TEXT("DefaultTakeoffAltitudeMeters"),
		DefaultTakeoffAltitudeMeters,
		GGameIni);

	FIPv4Address ServerAddress;

	if (!FIPv4Address::Parse(ServerIpString, ServerAddress))
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("Invalid Drone GCS command server IP: %s"),
			*ServerIpString);
		return false;
	}

	ServerEndpoint = MakeUnique<FIPv4Endpoint>(
		ServerAddress,
		static_cast<uint16>(ServerPort));

	Socket = FUdpSocketBuilder(TEXT("DroneGCSCommandClient"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToAddress(FIPv4Address::Any)
		.BoundToPort(0)
		.WithReceiveBufferSize(ReceiveBufferBytes)
		.WithSendBufferSize(ReceiveBufferBytes);

	if (!Socket)
	{
		return false;
	}

	Receiver = MakeUnique<FUdpSocketReceiver>(
		Socket,
		FTimespan::FromMilliseconds(20),
		TEXT("DroneGCSCommandReceiver"));

	Receiver->OnDataReceived().BindUObject(
		this,
		&UDroneCommandSubsystem::HandleDatagram);
	Receiver->Start();

	return true;
}

bool UDroneCommandSubsystem::SendCommand(
	const FString& Command,
	const TArray<FString>& Arguments)
{
	if (!Socket || !ServerEndpoint)
	{
		BroadcastProtocolError(TEXT("UDP_NOT_INITIALIZED"));
		return false;
	}

	const FString RequestId = NextRequestId();
	FString Payload = FString::Printf(TEXT("GCS1|%s|%s"), *RequestId, *Command);

	for (const FString& Argument : Arguments)
	{
		Payload += TEXT("|");
		Payload += Argument;
	}

	FTCHARToUTF8 Utf8Payload(*Payload);
	int32 BytesSent = 0;

	const bool bSent = Socket->SendTo(
		reinterpret_cast<const uint8*>(Utf8Payload.Get()),
		Utf8Payload.Length(),
		BytesSent,
		*ServerEndpoint->ToInternetAddr());

	if (!bSent || BytesSent != Utf8Payload.Length())
	{
		BroadcastProtocolError(TEXT("UDP_SEND_FAILED"));
		return false;
	}

	if (Command != TEXT("PING"))
	{
		FPendingCommand Pending;
		Pending.Command = Command;
		Pending.SentAtSeconds = FPlatformTime::Seconds();
		PendingCommands.Add(RequestId, MoveTemp(Pending));
	}

	return true;
}

void UDroneCommandSubsystem::HandleDatagram(
	const FArrayReaderPtr& Data,
	const FIPv4Endpoint& Endpoint)
{
	if (!Data.IsValid() || !ServerEndpoint || Endpoint != *ServerEndpoint)
	{
		return;
	}

	const ANSICHAR* RawData = reinterpret_cast<const ANSICHAR*>(Data->GetData());
	const FUTF8ToTCHAR Converted(RawData, Data->Num());
	const FString Message(Converted.Length(), Converted.Get());
	const TWeakObjectPtr<UDroneCommandSubsystem> WeakThis(this);

	AsyncTask(
		ENamedThreads::GameThread,
		[WeakThis, Message]()
		{
			if (UDroneCommandSubsystem* StrongThis = WeakThis.Get())
			{
				StrongThis->ProcessMessageOnGameThread(Message);
			}
		});
}

void UDroneCommandSubsystem::ProcessMessageOnGameThread(const FString& Message)
{
	TArray<FString> Fields;
	Message.ParseIntoArray(Fields, TEXT("|"), false);

	if (Fields.Num() < 3 || Fields[0] != TEXT("GCS1"))
	{
		BroadcastProtocolError(TEXT("BAD_RESPONSE_FORMAT"));
		return;
	}

	const FString& RequestId = Fields[1];
	const FString& Type = Fields[2];

	if (Type == TEXT("STATE"))
	{
		ProcessStateFields(Fields);
		return;
	}

	FGCSCommandResponse Response;
	Response.RequestId = RequestId;

	if (Type == TEXT("PONG"))
	{
		bCommandServiceOnline = true;
		Response.Command = TEXT("PING");
		Response.Result = EGCSCommandResult::Pong;
		Response.Detail = Fields.Num() > 3 ? Fields[3] : TEXT("OK");
	}
	else if (Type == TEXT("ACK"))
	{
		Response.Command = Fields.Num() > 3 ? Fields[3] : TEXT("UNKNOWN");
		Response.Result = EGCSCommandResult::Acknowledged;
		Response.Detail = Fields.Num() > 4 ? Fields[4] : TEXT("OK");
	}
	else if (Type == TEXT("ERR"))
	{
		Response.Command = Fields.Num() > 3 ? Fields[3] : TEXT("UNKNOWN");
		Response.Result = EGCSCommandResult::Error;
		Response.Detail = Fields.Num() > 4 ? Fields[4] : TEXT("UNKNOWN_ERROR");
	}
	else
	{
		BroadcastProtocolError(TEXT("UNKNOWN_RESPONSE_TYPE"));
		return;
	}

	PendingCommands.Remove(RequestId);
	OnCommandResponse.Broadcast(Response);
}

void UDroneCommandSubsystem::ProcessStateFields(const TArray<FString>& Fields)
{
	for (int32 Index = 3; Index < Fields.Num(); ++Index)
	{
		FString Key;
		FString Value;

		if (!ParseKeyValue(Fields[Index], Key, Value))
		{
			continue;
		}

		if (Key == TEXT("ONLINE")) LatestTelemetry.bOnline = FCString::Atoi(*Value) != 0;
		else if (Key == TEXT("SYSID")) LatestTelemetry.SystemId = FCString::Atoi(*Value);
		else if (Key == TEXT("COMPID")) LatestTelemetry.ComponentId = FCString::Atoi(*Value);
		else if (Key == TEXT("ARMED")) LatestTelemetry.bArmed = FCString::Atoi(*Value) != 0;
		else if (Key == TEXT("MODE")) LatestTelemetry.Mode = Value;
		else if (Key == TEXT("GPS")) LatestTelemetry.GpsSatellites = FCString::Atoi(*Value);
		else if (Key == TEXT("LAT")) LatestTelemetry.Latitude = FCString::Atod(*Value);
		else if (Key == TEXT("LON")) LatestTelemetry.Longitude = FCString::Atod(*Value);
		else if (Key == TEXT("ALT")) LatestTelemetry.AltitudeMeters = FCString::Atof(*Value);
		else if (Key == TEXT("RELALT")) LatestTelemetry.RelativeAltitudeMeters = FCString::Atof(*Value);
		else if (Key == TEXT("SPD")) LatestTelemetry.GroundSpeedMetersPerSecond = FCString::Atof(*Value);
		else if (Key == TEXT("BAT")) LatestTelemetry.BatteryPercent = FCString::Atoi(*Value);
		else if (Key == TEXT("ROLL")) LatestTelemetry.RollRadians = FCString::Atof(*Value);
		else if (Key == TEXT("PITCH")) LatestTelemetry.PitchRadians = FCString::Atof(*Value);
		else if (Key == TEXT("YAW")) LatestTelemetry.YawRadians = FCString::Atof(*Value);
	}

	OnTelemetryUpdated.Broadcast(LatestTelemetry);
}

void UDroneCommandSubsystem::BroadcastProtocolError(const FString& Detail)
{
	FGCSCommandResponse Response;
	Response.RequestId = TEXT("LOCAL");
	Response.Command = TEXT("PROTOCOL");
	Response.Result = EGCSCommandResult::Error;
	Response.Detail = Detail;
	OnCommandResponse.Broadcast(Response);
}

void UDroneCommandSubsystem::CheckCommandTimeouts()
{
	const double Now = FPlatformTime::Seconds();
	TArray<FString> TimedOutRequestIds;

	for (const TPair<FString, FPendingCommand>& Pair : PendingCommands)
	{
		if (Now - Pair.Value.SentAtSeconds >= 6.0)
		{
			FGCSCommandResponse Response;
			Response.RequestId = Pair.Key;
			Response.Command = Pair.Value.Command;
			Response.Result = EGCSCommandResult::Error;
			Response.Detail = TEXT("COMMAND_TIMEOUT");
			OnCommandResponse.Broadcast(Response);
			TimedOutRequestIds.Add(Pair.Key);
		}
	}

	for (const FString& RequestId : TimedOutRequestIds)
	{
		PendingCommands.Remove(RequestId);
	}
}

FString UDroneCommandSubsystem::NextRequestId()
{
	++RequestCounter;
	return FString::Printf(
		TEXT("UE-%lld-%d"),
		FDateTime::UtcNow().GetTicks(),
		RequestCounter);
}

bool UDroneCommandSubsystem::ParseKeyValue(
	const FString& Field,
	FString& Key,
	FString& Value)
{
	return Field.Split(TEXT("="), &Key, &Value);
}
