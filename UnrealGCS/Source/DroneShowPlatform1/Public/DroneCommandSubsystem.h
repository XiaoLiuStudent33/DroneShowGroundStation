#pragma once

#include "CoreMinimal.h"
#include "Common/UdpSocketReceiver.h"
#include "Containers/Ticker.h"
#include "GCSCommandTypes.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Serialization/ArrayReader.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "DroneCommandSubsystem.generated.h"

class FSocket;

UCLASS()
class DRONESHOWPLATFORM1_API UDroneCommandSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Commands")
	bool SendPing();

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Commands")
	bool SendArm();

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Commands")
	bool SendDisarm();

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Commands")
	bool SendGuided();

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Commands")
	bool SendTakeoff(float AltitudeMeters = 1.0f);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Commands")
	bool SendLand();

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Commands")
	bool SendRTL();

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Commands")
	bool SendLoiter();

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Commands")
	bool SendHover();

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Commands")
	bool SendStop();

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Status")
	FGCSDroneTelemetry GetLatestTelemetry() const
	{
		return LatestTelemetry;
	}

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Status")
	bool IsCommandServiceOnline() const
	{
		return bCommandServiceOnline;
	}

	UPROPERTY(BlueprintAssignable, Category = "Drone GCS|Events")
	FGCSCommandResponseEvent OnCommandResponse;

	UPROPERTY(BlueprintAssignable, Category = "Drone GCS|Events")
	FGCSTelemetryEvent OnTelemetryUpdated;

private:
	bool CreateSocket();
	bool SendCommand(const FString& Command, const TArray<FString>& Arguments = {});
	void HandleDatagram(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint);
	void ProcessMessageOnGameThread(const FString& Message);
	void ProcessStateFields(const TArray<FString>& Fields);
	void BroadcastProtocolError(const FString& Detail);
	void CheckCommandTimeouts();
	FString NextRequestId();

	static bool ParseKeyValue(const FString& Field, FString& Key, FString& Value);

	FSocket* Socket = nullptr;
	TUniquePtr<FUdpSocketReceiver> Receiver;
	TUniquePtr<FIPv4Endpoint> ServerEndpoint;

	FGCSDroneTelemetry LatestTelemetry;
	bool bCommandServiceOnline = false;
	int32 RequestCounter = 0;
	float DefaultTakeoffAltitudeMeters = 1.0f;
	FTSTicker::FDelegateHandle PingTickerHandle;

	struct FPendingCommand
	{
		FString Command;
		double SentAtSeconds = 0.0;
	};

	TMap<FString, FPendingCommand> PendingCommands;
};
