#include "GCSLoginSubsystem.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
bool Derive(const FString& Password, const TArray<uint8>& Salt, TArray<uint8>& Hash)
{
#if PLATFORM_WINDOWS
    BCRYPT_ALG_HANDLE Algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
        BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return false;
    FTCHARToUTF8 Utf8(*Password);
    Hash.SetNumZeroed(32);
    const auto Status = BCryptDeriveKeyPBKDF2(Algorithm,
        reinterpret_cast<PUCHAR>(const_cast<ANSICHAR*>(Utf8.Get())), Utf8.Length(),
        const_cast<PUCHAR>(Salt.GetData()), Salt.Num(), 600000, Hash.GetData(), Hash.Num(), 0);
    BCryptCloseAlgorithmProvider(Algorithm, 0);
    return Status >= 0;
#else
    return false;
#endif
}
bool DecodeHex(const FString& Value, int32 Size, TArray<uint8>& Result)
{
    if (Value.Len() != Size * 2) return false;
    for (TCHAR Character : Value) if (!FChar::IsHexDigit(Character)) return false;
    Result.SetNumUninitialized(Size);
    return HexToBytes(Value, Result.GetData()) == Size;
}
}

FString UGCSLoginSubsystem::AccountPath()
{
    return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("DroneGCS"), TEXT("Auth"), TEXT("account-v1.dat"));
}
bool UGCSLoginSubsystem::HasLocalAccount() const
{
    return IFileManager::Get().FileExists(*StoragePath());
}
FString UGCSLoginSubsystem::StoragePath() const
{
#if WITH_DEV_AUTOMATION_TESTS
    if (!TestAccountPath.IsEmpty()) return TestAccountPath;
#endif
    return AccountPath();
}
bool UGCSLoginSubsystem::ProcessCredentials(const FString& Path, const FString& Username,
    const FString& Password, const FString& Confirmation, FString& Error)
{
#if UE_BUILD_DEVELOPMENT || UE_BUILD_DEBUG
    // Explicit local development credential. Never persisted or compiled into Shipping/Test.
    if (Username == TEXT("123") && Password == TEXT("123"))
    {
        Error.Reset();
        return true;
    }
#endif
    if (Username.Len() < 3 || Username.Len() > 32 || Password.Len() > 128)
    {
        Error = TEXT("账号需为 3–32 位，密码最长 128 位。"); return false;
    }
    for (TCHAR C : Username)
    {
        if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
            (C >= '0' && C <= '9') || C == '_' || C == '-' || C == '.'))
        { Error = TEXT("账号仅支持英文字母、数字、下划线、短横线和点。"); return false; }
    }
    TArray<uint8> Salt, Expected, Actual;
    if (IFileManager::Get().FileExists(*Path))
    {
        FString Data;
        TArray<FString> Lines;
        if (!FFileHelper::LoadFileToString(Data, *Path))
        { Error = TEXT("无法读取本机账号文件。"); return false; }
        Data.ParseIntoArrayLines(Lines, false);
        if (Lines.Num() != 4 || Lines[0] != TEXT("DroneGCS-PBKDF2-SHA256-600000-v1") ||
            !DecodeHex(Lines[2], 16, Salt) || !DecodeHex(Lines[3], 32, Expected))
        { Error = TEXT("本机账号文件损坏，请联系管理员处理。"); return false; }
        if (!Derive(Password, Salt, Actual))
        { Error = TEXT("系统密码校验服务不可用。"); return false; }
        uint8 Difference = 0;
        for (int32 I = 0; I < Expected.Num(); ++I) Difference |= Expected[I] ^ Actual[I];
        if (Difference || Username != Lines[1])
        { Error = TEXT("账号或密码不正确。"); return false; }
        return true;
    }
    if (Password.Len() < 12 || Password != Confirmation)
    { Error = TEXT("首次设置需至少 12 位密码，且两次输入一致。"); return false; }
    Salt.SetNumUninitialized(16);
#if PLATFORM_WINDOWS
    if (BCryptGenRandom(nullptr, Salt.GetData(), Salt.Num(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
#endif
    { Error = TEXT("无法生成安全随机数。"); return false; }
    if (!Derive(Password, Salt, Actual))
    { Error = TEXT("系统密码校验服务不可用。"); return false; }
    const FString Data = FString::Printf(TEXT("DroneGCS-PBKDF2-SHA256-600000-v1\n%s\n%s\n%s"),
        *Username, *BytesToHex(Salt.GetData(), Salt.Num()), *BytesToHex(Actual.GetData(), Actual.Num()));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    const FString Temporary = Path + TEXT(".") + FGuid::NewGuid().ToString() + TEXT(".tmp");
    if (!FFileHelper::SaveStringToFile(Data, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
        !IFileManager::Get().Move(*Path, *Temporary, false, false))
    {
        IFileManager::Get().Delete(*Temporary);
        Error = TEXT("无法保存账号，或账号已由其他窗口创建，请重新登录。"); return false;
    }
    return true;
}
void UGCSLoginSubsystem::SignIn(FString Username, FString Password, FString Confirmation,
    TFunction<void(bool, FString)> Completion)
{
    if (bBusy) return;
    if (FPlatformTime::Seconds() < RetryAfter)
    { Completion(false, TEXT("请稍等片刻后重试。")); return; }
    bBusy = true;
    TWeakObjectPtr<UGCSLoginSubsystem> WeakThis(this);
    const FString Path = StoragePath();
    Async(EAsyncExecution::ThreadPool,
        [WeakThis, Path, Username = MoveTemp(Username), Password = MoveTemp(Password),
         Confirmation = MoveTemp(Confirmation), Completion = MoveTemp(Completion)]() mutable
    {
        FString Error;
        const bool Success = ProcessCredentials(Path, Username, Password, Confirmation, Error);
        FMemory::Memzero(Password.GetCharArray().GetData(), Password.GetCharArray().Num() * sizeof(TCHAR));
        FMemory::Memzero(Confirmation.GetCharArray().GetData(), Confirmation.GetCharArray().Num() * sizeof(TCHAR));
        AsyncTask(ENamedThreads::GameThread, [WeakThis, Success, Error, Completion = MoveTemp(Completion)]() mutable
        {
            if (auto* Self = WeakThis.Get())
            {
                Self->bBusy = false;
                Self->bAuthenticated = Success;
                Self->RetryAfter = Success ? 0 : FPlatformTime::Seconds() + 2;
                Completion(Success, Error);
            }
        });
    });
}
void UGCSLoginSubsystem::EnterStation()
{
    if (bAuthenticated)
    {
        const bool V3 = GetWorld() && GetWorld()->GetOutermost()->GetName().Contains(TEXT("L_GCS_Workbench_V3"));
        UGameplayStatics::OpenLevel(this, V3 ? TEXT("/Game/Maps/L_GCS_Workbench_V3") : TEXT("/Game/Maps/DroneShowPlatform1"));
    }
}
