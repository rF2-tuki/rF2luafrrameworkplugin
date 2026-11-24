# rFactor 2 Lua Framework (SDK V07)

## 概要

## ビルド手順（修正版）

**`.sln` を開かずにビルドする場合は、`BUILD_RELEASE.bat` を右クリックして「管理者として実行」ではなく、通常のダブルクリックで実行してください。** このバッチはVisual Studio 2022の `vswhere.exe` からMSBuildを検出し、`.sln` を経由せず `.vcxproj` を直接ビルドします。

初回ビルドではLua 5.5.1の公式ソースをダウンロードします。インターネット接続が必要です。ダウンロードできない場合は、`Scripts/PrepareLua.ps1` のエラーに表示される保存先へ公式アーカイブを置いて再実行してください。SHA-256も検証します。

必要なVisual Studioコンポーネント:
- Visual Studio 2022「C++によるデスクトップ開発」
- MSVC v143 x64/x86ビルドツール
- Windows 10 SDKまたはWindows 11 SDK

`.sln` を開いた際に `System.Memory, Version=4.0.2.0` が見つからないというエラーが出る場合、そのアセンブリはこのC++プロジェクトから参照していません。Visual Studio本体または導入済み拡張機能側の依存関係である可能性があります。Visual Studio Installerで更新/修復し、拡張機能を無効化して確認してください。ビルドだけなら上記バッチで `.sln` を迂回できます。


Lua 5.5.1を組み込んだrFactor 2 Internals Pluginです。SDK V07クラスから利用できるコールバックをベースに、Luaスクリプトの読み込み、テレメトリ/スコア/グラフィックス情報の公開、いくつかの書き換えAPI、GDI文字オーバーレイAPIを提供します。

## インストール

> 2026-11-10 の修正内容 (起動クラッシュ / 表示不具合) は `FIX_NOTES_ja.md` を参照してください。

1. Visual Studio 2022で `VC2022/rF2LuaFramework.sln` を開きます。
2. x64 / Releaseでビルドします。
3. 出力DLLをrFactor 2の `Bin64/Plugins` に配置します。
4. `Lua` フォルダー一式も **DLLと同じ `Bin64/Plugins` の中** に配置します
   (スクリプトの読み込み先はDLLの場所基準です。実際のパスは
   `Bin64/Plugins/Lua/lua_plugin.log` の `Script directory:` 行で確認できます)。
5. サンプル表示を使う場合は `Lua/Examples` のいずれか **1つだけ** を `Lua/Scripts` へコピーします。

`Scripts` 直下の `*.lua` はすべてロードされます。表示サンプルを同時にコピーしないでください（同名の `onRender` などが後勝ちで上書きされます）。

## Lua API

- `rf2.log(text)` : ログ出力 (`Lua/lua_plugin.log`)
- `rf2.plugin_dir()` / `rf2.script_dir()` : ディレクトリ取得
- `rf2.version` : APIバージョン
- `overlay.text(x, y, text [, r, g, b, size])` : `RenderScreenAfterOverlays` 内で文字描画。色は0-255、座標原点は左上。

主なコールバック:

- `onTelemetry(data)` : `TelemInfoV01` の多数のメンバーをC++名で公開。4輪情報は `data.mWheel[1..4]`。
- `onScoring(data)` : セッション情報と車両スコア情報を公開。
- `onGraphics(data)` : `GraphicsInfoV02` のカメラ・環境光情報を公開。
- `onEnvironment(data)` : `EnvironmentInfoV01.mPath` を公開。
- `onRender(screen)` : 描画フレームごとに呼び出し。`screen.mWidth`, `screen.mHeight` が利用可能。
- `onForceFeedback(value)`, `onCheckHWControl(name, value)`
- `onPhysicsOptions(options)`, `onWeather(info)`, `onCameraControl(info)`, `onMessage(info)`, `onCommentary(info)`

書き換え可能なコールバックはLuaから返した値をC++側で反映します。`nil` または未定義のフィールドは元の値を維持します。SDKが読み取り専用として定義するデータは変更できません。

## サンプル

- `Lua/Examples/debug_overlay.lua` : 広範囲のテレメトリとスコア情報を表示
- `Lua/Examples/race_overlay.lua` : RPM、ギア、速度、燃料、温度、タイヤ温度、順位を表示

## 実装範囲と制限（必読）

この配布物はSDK V07対応の完成版ではなく、検証用の拡張ベースです。現在のコードは `TelemInfoV01`、`ScoringInfoV01`/`VehicleScoringInfoV01`、`GraphicsInfoV02`、`EnvironmentInfoV01` の一部/主要メンバーと、既存の書き換えコールバックを公開しますが、SDK V07で定義された全構造体・全メンバー・全コールバックを網羅していません。

特に以下は未実装です。

- `GetCustomVariable` / `AccessCustomVariable` / `GetCustomVariableSetting`
- `WantsMultiSessionRulesAccess` / `AccessMultiSessionRules`
- `WantsTrackRulesAccess` / `AccessTrackRules`
- `WantsPitMenuAccess` / `AccessPitMenu`
- `CustomControlInfoV01`、`PitMenuV01`、`MultiSessionRulesV01`、`TrackRulesV01` のLua完全公開
- 構造体の将来拡張領域 `mExpansion` の意味付け（SDK上も予約領域）
- `ScreenInfoV01` の全メンバー公開（現在は幅・高さのみ）

また、GDIによるオーバーレイは環境・描画モードによって表示されない可能性があります。DirectXの描画面へ確実に合成する方式ではありません。rFactor 2上での表示確認が必要です。

## ビルド/動作確認状況

この作業環境にはWindows/Visual Studio/rFactor 2実行環境がないため、DLLのビルドおよびゲーム内動作確認は未実施です。ソースとプロジェクトを同梱しています。導入前に既存プラグインをバックアップし、テスト環境でビルド・動作確認してください。


## DX11 Present hook 開発版

このパッケージには実験的なDXGI Present/ResizeBuffers vtable hookを追加しています。詳細は `DX11_PRESENT_HOOK_STATUS_ja.md` と `LUA_WRITE_SUPPORT_ja.md` を参照してください。WindowsでのビルドおよびrFactor 2実機確認は未実施です。
