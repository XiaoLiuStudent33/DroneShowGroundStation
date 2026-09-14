#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "GCSLoginSubsystem.generated.h"

// Local workstation login, not a replacement for backend command authorization.
UCLASS()
class DRONESHOWPLATFORM1_API UGCSLoginSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    bool HasLocalAccount() const;
    bool IsAuthenticated() const { return bAuthenticated; }
    bool IsBusy() const { return bBusy; }
    void SignIn(FString Username, FString Password, FString Confirmation,
        TFunction<void(bool, FString)> Completion);
    // Actions: 1 register, 2 recover password, 3 enroll recovery using existing password.
    void ManageAccount(int32 Action,FString Username,FString Password,FString Confirmation,FString Recovery,
        TFunction<void(bool,FString,FString)> Completion);
    static FString ResolveAccountPath(const FString& Base,const FString& Username);
    static bool ProcessAccountAction(const FString& Base,int32 Action,const FString& Username,const FString& Password,
        const FString& Confirmation,const FString& Recovery,FString& Error,FString& NewRecovery);
    void EnterStation();
    static FString AccountPath();
    // Shared by the worker and isolated automation tests. Never logs credentials.
    static bool ProcessCredentials(const FString& Path, const FString& Username,
        const FString& Password, const FString& Confirmation, FString& Error);
private:
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FGCSLoginSmoke;
    friend struct FGCSLoginAccountSmoke;
    FString TestAccountPath;
#endif
    FString StoragePath() const;
    bool bAuthenticated = false;
    bool bBusy = false;
    double RetryAfter = 0;
};
