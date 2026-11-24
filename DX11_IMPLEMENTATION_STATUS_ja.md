# DX11オーバーレイ実装パッケージ（開発版）

## 今回追加したファイル
- `Include/DX11Overlay.hpp`
- `Source/DX11Overlay.cpp`

Luaの `overlay.text(x, y, text, r, g, b, size)` が作る描画行を、Direct2D/DirectWrite + Direct3D 11のバックバッファ上へ描くバックエンドを追加しました。GDIの `GetDC` / `TextOutA` は削除しています。`ResizeBuffers` 後は `Invalidate()` を呼ぶことでバックバッファ依存リソースを作り直します。

## 重要：まだゲーム画面へ自動表示される完成版ではありません
このパッケージにはDX11描画処理と、Presentフック側から呼び出すためのエクスポート関数 `RenderLuaOverlayDX11(IDXGISwapChain*)` までを実装しています。しかし、rFactor 2のDXGI `IDXGISwapChain::Present` を実際に検出・フックするコード（MinHook等のフック機構）は含めていません。そのため、このままビルドしても `RenderLuaOverlayDX11` を呼ぶものがなく、ゲーム画面には表示されません。

この未実装部分を隠して「DX11対応完了」とは扱いません。Presentフックを追加する際は、少なくとも以下が必要です。
1. rFactor 2が使用するスワップチェーンの特定（複数スワップチェーンを誤選択しないこと）
2. `Present` から `RenderLuaOverlayDX11(swapChain)` を呼ぶこと
3. `ResizeBuffers` の前に `Invalidate()` 相当を呼ぶこと
4. Presentフック解除・プラグイン終了時の安全な停止
5. ゲームのD3D11パイプライン状態を壊さない状態退避・復元

## ビルド
Visual Studio 2022 / MSVC v143 / Windows 10 SDKを想定しています。追加リンクライブラリは `d3d11.lib`, `dxgi.lib`, `d2d1.lib`, `dwrite.lib` です。Lua 5.5ソースの準備は従来どおり `Scripts/PrepareLua.ps1` が行います。

## 検証状況
この実行環境ではWindows/MSVCおよびrFactor 2を起動できないため、コンパイル・ゲーム内描画・ResizeBuffers・VR/複数画面との競合は未検証です。まずWindows上でビルド確認が必要です。
