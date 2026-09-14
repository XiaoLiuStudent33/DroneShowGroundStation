#include "GCSLoginSubsystem.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSLoginCredentialsTest, "DroneGCS.Login.Credentials",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGCSLoginCredentialsTest::RunTest(const FString& Parameters)
{
    const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        FGuid::NewGuid().ToString() + TEXT("-login.dat"));
    FString Error, Contents;
    auto Check = [&](const FString& User, const FString& Password, const FString& Confirm)
    { return UGCSLoginSubsystem::ProcessCredentials(Path, User, Password, Confirm, Error); };
#if UE_BUILD_DEVELOPMENT || UE_BUILD_DEBUG
    TestTrue(TEXT("Development account logs in without registration"), Check(TEXT("123"), TEXT("123"), TEXT("")));
    TestFalse(TEXT("Development login does not persist an account"), IFileManager::Get().FileExists(*Path));
#else
    TestFalse(TEXT("Development credential unavailable in release configurations"), Check(TEXT("123"), TEXT("123"), TEXT("")));
#endif
    TestFalse(TEXT("Development account requires exact password"), Check(TEXT("123"), TEXT("124"), TEXT("")));
    TestFalse(TEXT("Development password does not allow other accounts"), Check(TEXT("124"), TEXT("123"), TEXT("")));
    TestFalse(TEXT("Short initial password rejected"), Check(TEXT("operator"), TEXT("short"), TEXT("short")));
    TestFalse(TEXT("Mismatched confirmation rejected"), Check(TEXT("operator"), TEXT("example-pass-123"), TEXT("different")));
    TestFalse(TEXT("Invalid account rejected"), Check(TEXT("bad\nname"), TEXT("example-pass-123"), TEXT("example-pass-123")));
    TestFalse(TEXT("Validation failures do not create account"), IFileManager::Get().FileExists(*Path));
    TestTrue(TEXT("First account created"), Check(TEXT("operator"), TEXT("example-pass-123"), TEXT("example-pass-123")));
    FFileHelper::LoadFileToString(Contents, *Path);
#if UE_BUILD_DEVELOPMENT || UE_BUILD_DEBUG
    TestTrue(TEXT("Development login also works with an existing real account"), Check(TEXT("123"), TEXT("123"), TEXT("")));
    FString AfterDevelopmentLogin;
    FFileHelper::LoadFileToString(AfterDevelopmentLogin, *Path);
    TestEqual(TEXT("Development login preserves real account"), AfterDevelopmentLogin, Contents);
#endif
    TestFalse(TEXT("No plaintext password persisted"), Contents.Contains(TEXT("example-pass-123")));
    TestTrue(TEXT("Existing account login"), Check(TEXT("operator"), TEXT("example-pass-123"), TEXT("")));
    TestFalse(TEXT("Wrong password rejected"), Check(TEXT("operator"), TEXT("wrong-password-123"), TEXT("")));
    TestFalse(TEXT("Wrong user rejected"), Check(TEXT("other"), TEXT("example-pass-123"), TEXT("")));
    FFileHelper::SaveStringToFile(TEXT("corrupt"), *Path);
    TestFalse(TEXT("Corrupt account fails closed"), Check(TEXT("operator"), TEXT("example-pass-123"), TEXT("example-pass-123")));
    IFileManager::Get().Delete(*Path);
    return true;
}
#endif
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSLoginRecoveryTest,"DroneGCS.Login.Recovery",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSLoginRecoveryTest::RunTest(const FString&)
{
    const FString Base=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation"),FGuid::NewGuid().ToString()+TEXT("-recovery.dat"));
    const FString User=TEXT("local-artist"),Password=TEXT("123456"),NewPassword=TEXT("654321");
    FString Error,Code,NextCode,Data,After;
    auto Action=[&](int32 A,const FString& U,const FString& P,const FString& C,const FString& R,FString& Output){return UGCSLoginSubsystem::ProcessAccountAction(Base,A,U,P,C,R,Error,Output);};
    TestFalse(TEXT("Weak registration rejected"),Action(1,User,TEXT("short"),TEXT("short"),TEXT(""),Code));
    TestFalse(TEXT("19 character registration rejected"),Action(1,User,TEXT("1234567890123456789"),TEXT("1234567890123456789"),TEXT(""),Code));
    TestTrue(TEXT("Registration succeeds"),Action(1,User,Password,Password,TEXT(""),Code));
    TestEqual(TEXT("Recovery has 192 random bits"),Code.Len(),48);
    const FString Account=UGCSLoginSubsystem::ResolveAccountPath(Base,User);
    FFileHelper::LoadFileToString(Data,*Account);
    TestFalse(TEXT("Password not persisted"),Data.Contains(Password));TestFalse(TEXT("Recovery code not persisted"),Data.Contains(Code));
    TestFalse(TEXT("Duplicate registration does not overwrite"),Action(1,User,NewPassword,NewPassword,TEXT(""),NextCode));
    TestTrue(TEXT("New account signs in"),UGCSLoginSubsystem::ProcessCredentials(Account,User,Password,TEXT(""),Error));
    TestFalse(TEXT("Wrong recovery rejected"),Action(2,User,NewPassword,NewPassword,TEXT("bad-code"),NextCode));
    FFileHelper::LoadFileToString(After,*Account);TestEqual(TEXT("Failed reset leaves record unchanged"),After,Data);
    TestTrue(TEXT("Recovery resets password"),Action(2,User,NewPassword,NewPassword,Code,NextCode));
    TestTrue(TEXT("Recovery rotates code"),Code!=NextCode && NextCode.Len()==48);
    TestFalse(TEXT("Old password rejected"),UGCSLoginSubsystem::ProcessCredentials(Account,User,Password,TEXT(""),Error));
    TestTrue(TEXT("New password signs in"),UGCSLoginSubsystem::ProcessCredentials(Account,User,NewPassword,TEXT(""),Error));
    FString Unused;
    TestFalse(TEXT("Used recovery code rejected"),Action(2,User,Password,Password,Code,Unused));
    TestTrue(TEXT("Legacy record setup"),UGCSLoginSubsystem::ProcessCredentials(Base,TEXT("legacy"),Password,Password,Error));
    TestFalse(TEXT("Legacy reset unavailable without enrollment"),Action(2,TEXT("legacy"),NewPassword,NewPassword,Code,Unused));
    TestFalse(TEXT("Legacy enrollment requires password"),Action(3,TEXT("legacy"),NewPassword,TEXT(""),TEXT(""),Unused));
    TestTrue(TEXT("Legacy recovery enrollment"),Action(3,TEXT("legacy"),Password,TEXT(""),TEXT(""),Unused));
    TestTrue(TEXT("Legacy password preserved"),UGCSLoginSubsystem::ProcessCredentials(Base,TEXT("legacy"),Password,TEXT(""),Error));
    TestTrue(TEXT("Legacy reset with enrolled recovery"),Action(2,TEXT("legacy"),NewPassword,NewPassword,Unused,Code));
    IFileManager::Get().Delete(*Account);IFileManager::Get().Delete(*Base);IFileManager::Get().DeleteDirectory(*(Base+TEXT(".accounts")),false,false);
    return true;
}
#endif
