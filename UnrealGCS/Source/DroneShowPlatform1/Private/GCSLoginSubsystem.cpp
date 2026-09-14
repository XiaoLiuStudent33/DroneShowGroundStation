#include "GCSLoginSubsystem.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
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
    TArray<FString> Accounts;
    IFileManager::Get().FindFiles(Accounts,*(StoragePath()+TEXT(".accounts/*.dat")),true,false);
    return IFileManager::Get().FileExists(*StoragePath()) || Accounts.Num()>0;
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
        if ((Lines.Num() != 4 && Lines.Num() != 6) || Lines[0] != TEXT("DroneGCS-PBKDF2-SHA256-600000-v1") ||
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
    if (Password.Len() < 6 || Password.Len() > 18 || Password != Confirmation)
    { Error = TEXT("首次设置需 6–18 位密码，且两次输入一致。"); return false; }
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
    const FString Path = ResolveAccountPath(StoragePath(),Username);
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

FString UGCSLoginSubsystem::ResolveAccountPath(const FString& Base,const FString& Username)
{
    const FString Additional=FPaths::Combine(Base+TEXT(".accounts"),FMD5::HashAnsiString(*Username.ToLower())+TEXT(".dat"));
    if(IFileManager::Get().FileExists(*Additional))return Additional;
    FString Data;TArray<FString> Lines;
    if(FFileHelper::LoadFileToString(Data,*Base)){Data.ParseIntoArrayLines(Lines,false);if(Lines.Num()>=2 && Lines[1].Equals(Username,ESearchCase::IgnoreCase))return Base;}
    return Additional;
}
bool UGCSLoginSubsystem::ProcessAccountAction(const FString& Base,int32 Action,const FString& Username,const FString& Password,
    const FString& Confirmation,const FString& Recovery,FString& Error,FString& NewRecovery)
{
    Error.Reset();NewRecovery.Reset();
    if(Action<1 || Action>3 || Recovery.Len()>128){Error=TEXT("账号操作无效。");return false;}
    if(Username.Len()<3 || Username.Len()>32 || Username==TEXT("123")){Error=TEXT("账号需为 3–32 位，123 为保留测试账号。");return false;}
    for(TCHAR C:Username)if(!((C>='a' && C<='z')||(C>='A' && C<='Z')||(C>='0' && C<='9')||C=='_'||C=='-'||C=='.')){Error=TEXT("账号仅支持英文字母、数字、下划线、短横线和点。");return false;}
    if(Password.Len()>128 || (Action!=3 && (Password.Len()<6 || Password.Len()>18 || Password!=Confirmation)))
    {Error=TEXT("密码需为 6–18 位，且两次输入一致。");return false;}
    const FString Path=ResolveAccountPath(Base,Username);
    const bool Exists=IFileManager::Get().FileExists(*Path);
    if(Action==1 && Exists){Error=TEXT("账号已存在，请登录或找回密码。");return false;}
    FString Data;TArray<FString> Lines;
    if(Action!=1)
    {
        if(!Exists || !FFileHelper::LoadFileToString(Data,*Path)){Error=TEXT("账号或验证信息不正确。");return false;}
        Data.ParseIntoArrayLines(Lines,false);
        if((Lines.Num()!=4 && Lines.Num()!=6) || Lines[0]!=TEXT("DroneGCS-PBKDF2-SHA256-600000-v1"))
        {Error=TEXT("本机账号文件损坏，请联系管理员处理。");return false;}
        if(Action==3)
        {if(!ProcessCredentials(Path,Username,Password,TEXT(""),Error))return false;}
        else
        {
            TArray<uint8> Salt,Hash,Actual;
            if(Lines.Num()!=6){Error=TEXT("该账号尚未设置恢复码。请使用原密码设置恢复码。");return false;}
            if(!DecodeHex(Lines[4],16,Salt)||!DecodeHex(Lines[5],32,Hash)||!Derive(Recovery.TrimStartAndEnd().ToUpper(),Salt,Actual))
            {Error=TEXT("账号或恢复码不正确。");return false;}
            uint8 Difference=0;for(int I=0;I<32;++I)Difference|=Hash[I]^Actual[I];
            if(Difference){Error=TEXT("账号或恢复码不正确。");return false;}
        }
    }
    auto Random=[](TArray<uint8>& Bytes,int32 Count)
    {
        Bytes.SetNumUninitialized(Count);
#if PLATFORM_WINDOWS
        return BCryptGenRandom(nullptr,Bytes.GetData(),Bytes.Num(),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0;
#else
        return false;
#endif
    };
    TArray<uint8> PassSalt,PassHash,RecoveryBytes,RecoverySalt,RecoveryHash;
    if(Action!=3)
    {
        if(!Random(PassSalt,16)||!Derive(Password,PassSalt,PassHash)){Error=TEXT("系统密码校验服务不可用。");return false;}
        Lines={TEXT("DroneGCS-PBKDF2-SHA256-600000-v1"),Username,BytesToHex(PassSalt.GetData(),16),BytesToHex(PassHash.GetData(),32)};
    }
    else Lines.SetNum(4);
    if(!Random(RecoveryBytes,24)||!Random(RecoverySalt,16)){Error=TEXT("无法生成安全随机数。");return false;}
    FString Code=BytesToHex(RecoveryBytes.GetData(),24);
    if(!Derive(Code,RecoverySalt,RecoveryHash)){Error=TEXT("系统密码校验服务不可用。");return false;}
    Lines.Add(BytesToHex(RecoverySalt.GetData(),16));Lines.Add(BytesToHex(RecoveryHash.GetData(),32));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
    const FString Temp=Path+TEXT(".")+FGuid::NewGuid().ToString()+TEXT(".tmp");
    if(!FFileHelper::SaveStringToFile(FString::Join(Lines,TEXT("\n")),*Temp,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        ||!IFileManager::Get().Move(*Path,*Temp,Action!=1,false))
    {IFileManager::Get().Delete(*Temp);Error=TEXT("无法保存账号，请重试。");return false;}
    NewRecovery=MoveTemp(Code);return true;
}
void UGCSLoginSubsystem::ManageAccount(int32 Action,FString Username,FString Password,FString Confirmation,FString Recovery,TFunction<void(bool,FString,FString)> Completion)
{
    if(bBusy)return;
    if(FPlatformTime::Seconds()<RetryAfter){Completion(false,TEXT("请稍等片刻后重试。"),TEXT(""));return;}
    bBusy=true;TWeakObjectPtr<UGCSLoginSubsystem> WeakThis(this);const FString Base=StoragePath();
    Async(EAsyncExecution::ThreadPool,[WeakThis,Base,Action,Username=MoveTemp(Username),Password=MoveTemp(Password),Confirmation=MoveTemp(Confirmation),Recovery=MoveTemp(Recovery),Completion=MoveTemp(Completion)]()mutable
    {
        FString Error,Code;bool Success=ProcessAccountAction(Base,Action,Username,Password,Confirmation,Recovery,Error,Code);
        for(FString* Secret:{&Password,&Confirmation,&Recovery})FMemory::Memzero(Secret->GetCharArray().GetData(),Secret->GetCharArray().Num()*sizeof(TCHAR));
        AsyncTask(ENamedThreads::GameThread,[WeakThis,Success,Error,Code=MoveTemp(Code),Completion=MoveTemp(Completion)]()mutable
        {if(auto* Self=WeakThis.Get()){Self->bBusy=false;Self->RetryAfter=Success?0:FPlatformTime::Seconds()+2;Completion(Success,Error,MoveTemp(Code));}});
    });
}
