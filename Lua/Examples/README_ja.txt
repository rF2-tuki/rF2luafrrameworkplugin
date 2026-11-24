Luaオーバーレイサンプル

- debug_overlay.lua: 公開されているテレメトリ/スコア/描画情報の広範なデバッグ表示
- race_overlay.lua: レース中に必要な値を絞った表示
- dx11_overlay_example.lua: RPM / ギア / 燃料だけを表示する最小のDX11表示例

使い方:
1. いずれか1つだけのLuaファイルを、DLLと同じフォルダーにある Lua\Scripts\ へコピーします。
   (DLLを Bin64\Plugins に置いた場合は Bin64\Plugins\Lua\Scripts\ です。
    ゲーム本体フォルダー直下の Plugins ではありません。実際の読み込み先は
    Lua\lua_plugin.log の "Script directory:" 行で確認できます)
2. rFactor 2を起動し、プラグインを読み込ませます。
3. 切り替える場合は、先にコピーしたファイルをScriptsから退避してから別のファイルをコピーします。

重要: Scripts直下の*.luaはすべてアルファベット順に読み込まれ、同名の関数
(onTelemetry / onRender など)は後から読み込まれたファイルで上書きされます。
オーバーレイ系のスクリプトを2つ同時にScriptsへ入れないでください。
以前は dx11_overlay_example.lua がScriptsに同梱されており、debug_overlay.lua を
コピーしても dx11 側で上書きされてデバッグ表示が出ませんでした。修正版では
表示例はすべて Examples 側へ移動しています。

Examplesフォルダー内のファイルは自動読込されません。
overlay.text(x, y, text [, r, g, b, size]) で文字を描画します。座標は画面左上が原点です。
