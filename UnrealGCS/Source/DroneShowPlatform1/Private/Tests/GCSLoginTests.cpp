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
    TestFalse(TEXT("Mismatched confirmation rejected"), Check(TEXT("operator"), TEXT("example-password-123"), TEXT("different")));
    TestFalse(TEXT("Invalid account rejected"), Check(TEXT("bad\nname"), TEXT("example-password-123"), TEXT("example-password-123")));
    TestFalse(TEXT("Validation failures do not create account"), IFileManager::Get().FileExists(*Path));
    TestTrue(TEXT("First account created"), Check(TEXT("operator"), TEXT("example-password-123"), TEXT("example-password-123")));
    FFileHelper::LoadFileToString(Contents, *Path);
#if UE_BUILD_DEVELOPMENT || UE_BUILD_DEBUG
    TestTrue(TEXT("Development login also works with an existing real account"), Check(TEXT("123"), TEXT("123"), TEXT("")));
    FString AfterDevelopmentLogin;
    FFileHelper::LoadFileToString(AfterDevelopmentLogin, *Path);
    TestEqual(TEXT("Development login preserves real account"), AfterDevelopmentLogin, Contents);
#endif
    TestFalse(TEXT("No plaintext password persisted"), Contents.Contains(TEXT("example-password-123")));
    TestTrue(TEXT("Existing account login"), Check(TEXT("operator"), TEXT("example-password-123"), TEXT("")));
    TestFalse(TEXT("Wrong password rejected"), Check(TEXT("operator"), TEXT("wrong-password-123"), TEXT("")));
    TestFalse(TEXT("Wrong user rejected"), Check(TEXT("other"), TEXT("example-password-123"), TEXT("")));
    FFileHelper::SaveStringToFile(TEXT("corrupt"), *Path);
    TestFalse(TEXT("Corrupt account fails closed"), Check(TEXT("operator"), TEXT("example-password-123"), TEXT("example-password-123")));
    IFileManager::Get().Delete(*Path);
    return true;
}
#endif
