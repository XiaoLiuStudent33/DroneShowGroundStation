#pragma once
#include "CoreMinimal.h"

// Localized login presentation only. No fleet, network or command dependencies.
struct FGCSLoginPresentation
{
    int32 Language = 0;
    int32 Theme = 0;
    int32 Previous = 0;
    int32 Direction = 1;
    float Transition = 1.f;
    double AnimationTime = 0;
    double ThemeTime = 0;
    bool Paused = false;
    bool Remember = false;
    bool RevealPassword = false;
    void Next(int32 Delta)
    {
        if (Transition < 1.f) return;
        Previous = Theme; Theme = (Theme + Delta + 3) % 3;
        Direction = Delta < 0 ? -1 : 1; Transition = 0; ThemeTime = 0;
    }
    void Advance(float Delta)
    {
        const float D = FMath::Min(Delta, .1f);
        if (!Paused) { AnimationTime += D; ThemeTime += D; }
        Transition = FMath::Min(1.f, Transition + D / .75f);
        if (!Paused && ThemeTime >= 14.) Next(1);
    }
    FString Text(const TCHAR* Simplified, const TCHAR* Traditional, const TCHAR* English, const TCHAR* Japanese) const
    {
        const TCHAR* Values[] = { Simplified, Traditional, English, Japanese };
        return Values[FMath::Clamp(Language, 0, 3)];
    }
    FString Title(int32 Index) const
    {
        if (Index == 0) return Text(TEXT("让灵感，在夜空成形"),TEXT("讓靈感，在夜空成形"),TEXT("Give imagination a sky"),TEXT("街を、光のステージに"));
        if (Index == 1) return Text(TEXT("让每束光，汇成同一方向"),TEXT("讓每束光，匯成同一方向"),TEXT("Move together, shine together"),TEXT("光をひとつの方向へ"));
        return Text(TEXT("以光为笔，编织无限可能"),TEXT("以光為筆，編織無限可能"),TEXT("Light in endless motion"),TEXT("光で織りなす無限の可能性"));
    }
    FString Caption(int32 Index) const
    {
        if (Index == 0) return Text(TEXT("星环 · 灯阵演示"),TEXT("星環 · 燈陣演示"),TEXT("SATURN / LIGHT SHOW"),TEXT("星の環 · ライトショー"));
        if (Index == 1) return Text(TEXT("光翼 · 编队演示"),TEXT("光翼 · 編隊演示"),TEXT("LIGHT WING / FORMATION"),TEXT("光の翼 · フォーメーション"));
        return Text(TEXT("光潮 · 光影演示"),TEXT("光潮 · 光影演示"),TEXT("LIGHT TIDE / LIGHT SHOW"),TEXT("光の波 · ライトショー"));
    }
    FString StatusText(const FString& Source) const;
};
