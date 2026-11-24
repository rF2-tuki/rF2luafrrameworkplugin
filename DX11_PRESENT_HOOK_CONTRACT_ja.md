# DXGI Presentフック接続仕様

本プラグインDLLは次の関数をエクスポートします。

```cpp
extern "C" __declspec(dllimport)
void __cdecl RenderLuaOverlayDX11(IDXGISwapChain* swapChain);
```

フックライブラリ側のPresent detourは、元のPresentを呼ぶ前に対象スワップチェーンについてこの関数を呼びます。フックはrFactor 2の描画スレッド上で実行し、Lua VMをフック側から直接操作しないでください。Luaの `onRender` はInternals Pluginの描画コールバックで評価され、生成済み描画行だけをDX11側が取得します。

`ResizeBuffers` をフックする場合は元関数呼び出し前にバックバッファ参照を解放する必要があります。本実装の `DX11Overlay::Invalidate()` は内部関数であり、外部から呼ぶためのエクスポートはまだ設けていません。実際のフック実装時に、同一DLL内の管理クラスから呼び出してください。

Present detour内でD2D描画を行う際は、ゲーム側のD3D11状態を退避・復元する必要があります。現状の描画バックエンドはこの状態保存を実装していないため、フック接続後もそのまま実運用してはいけません。
