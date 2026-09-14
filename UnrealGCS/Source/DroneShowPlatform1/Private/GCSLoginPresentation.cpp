#include "GCSLoginPresentation.h"
FString FGCSLoginPresentation::StatusText(const FString& Source) const
{
    if (Source.IsEmpty()) return FString();
    struct FLine { const TCHAR* S; const TCHAR* T; const TCHAR* E; const TCHAR* J; };
    static const FLine Lines[] = {
        {TEXT("账号操作无效。"),TEXT("帳號操作無效。"),TEXT("Invalid account action."),TEXT("無効なアカウント操作です。")},
        {TEXT("账号需为 3–32 位，123 为保留测试账号。"),TEXT("帳號需為 3–32 位，123 為保留測試帳號。"),TEXT("Use 3–32 characters. The account 123 is reserved."),TEXT("3〜32文字で入力してください。123は予約済みです。")},
        {TEXT("密码需为 6–18 位，且两次输入一致。"),TEXT("密碼需為 6–18 位，且兩次輸入一致。"),TEXT("Use 6–18 characters and matching passwords."),TEXT("6〜18文字で同じパスワードを入力してください。")},
        {TEXT("账号已存在，请登录或找回密码。"),TEXT("帳號已存在，請登入或找回密碼。"),TEXT("Account already exists. Sign in or reset its password."),TEXT("登録済みです。ログインまたは再設定してください。")},
        {TEXT("账号或验证信息不正确。"),TEXT("帳號或驗證資訊不正確。"),TEXT("Incorrect account or verification details."),TEXT("アカウントまたは認証情報が正しくありません。")},
        {TEXT("该账号尚未设置恢复码。请使用原密码设置恢复码。"),TEXT("該帳號尚未設定復原碼。請使用原密碼設定。"),TEXT("No recovery code set. Use your current password to set one up."),TEXT("復旧コード未設定です。現在のパスワードで設定してください。")},
        {TEXT("账号或恢复码不正确。"),TEXT("帳號或復原碼不正確。"),TEXT("Incorrect account or recovery code."),TEXT("アカウントまたは復旧コードが正しくありません。")},
        {TEXT("无法保存账号，请重试。"),TEXT("無法儲存帳號，請重試。"),TEXT("Could not save account. Please try again."),TEXT("保存できませんでした。もう一度お試しください。")},
        {TEXT("正在校验本机账号…"),TEXT("正在驗證本機帳號…"),TEXT("Verifying account…"),TEXT("アカウントを確認しています…")},
        {TEXT("登录成功，正在加载地面站场景…"),TEXT("登入成功，正在載入地面站場景…"),TEXT("Signed in. Loading workspace…"),TEXT("ログイン成功。ワークスペースを読み込み中…")},
        {TEXT("地面站场景缺失，请检查安装文件。"),TEXT("地面站場景缺失，請檢查安裝檔案。"),TEXT("Workspace missing. Check the installation."),TEXT("シーンが見つかりません。インストールを確認してください。")},
        {TEXT("账号或密码不正确。"),TEXT("帳號或密碼不正確。"),TEXT("Incorrect account or password."),TEXT("アカウントまたはパスワードが正しくありません。")},
        {TEXT("请稍等片刻后重试。"),TEXT("請稍候再試。"),TEXT("Please wait a moment and try again."),TEXT("少し待ってから、もう一度お試しください。")},
        {TEXT("首次设置需 6–18 位密码，且两次输入一致。"),TEXT("首次設定需 6–18 位密碼，且兩次輸入一致。"),TEXT("Use 6–18 characters and matching passwords."),TEXT("6〜18文字のパスワードを、確認欄にも同じように入力してください。")},
        {TEXT("账号需为 3–32 位，密码最长 128 位。"),TEXT("帳號需為 3–32 位，密碼最長 128 位。"),TEXT("Account: 3–32 characters. Password: up to 128."),TEXT("アカウントは3〜32文字、パスワードは128文字以内です。")},
        {TEXT("账号仅支持英文字母、数字、下划线、短横线和点。"),TEXT("帳號僅支援英文字母、數字、底線、連字號和點。"),TEXT("Use letters, numbers, underscore, hyphen or dot."),TEXT("アカウントには英数字、_、-、. を使用してください。")},
        {TEXT("无法读取本机账号文件。"),TEXT("無法讀取本機帳號檔案。"),TEXT("Cannot read the local account file."),TEXT("ローカルアカウントファイルを読み込めません。")},
        {TEXT("本机账号文件损坏，请联系管理员处理。"),TEXT("本機帳號檔案損毀，請聯絡管理員。"),TEXT("Account file damaged. Contact your administrator."),TEXT("アカウントファイルが破損しています。管理者に連絡してください。")},
        {TEXT("系统密码校验服务不可用。"),TEXT("系統密碼驗證服務無法使用。"),TEXT("Password verification is unavailable."),TEXT("パスワード検証サービスを利用できません。")},
        {TEXT("无法生成安全随机数。"),TEXT("無法產生安全隨機數。"),TEXT("Secure random generation failed."),TEXT("安全な乱数を生成できません。")},
        {TEXT("无法保存账号，或账号已由其他窗口创建，请重新登录。"),TEXT("無法儲存帳號，或帳號已由其他視窗建立，請重新登入。"),TEXT("Cannot save account, or it already exists. Sign in again."),TEXT("保存できないか、別のウィンドウで作成済みです。再度ログインしてください。")}
    };
    for (const auto& L : Lines) if (Source == L.S) return Text(L.S, L.T, L.E, L.J);
    return Text(TEXT("登录失败，请重试。"),TEXT("登入失敗，請重試。"),TEXT("Sign-in failed. Please try again."),TEXT("ログインできませんでした。もう一度お試しください。"));
}
