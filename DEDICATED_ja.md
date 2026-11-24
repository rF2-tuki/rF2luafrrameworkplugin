# Dedicated対応版（v10ベース、2026-11-22）

通常版 v10 に対する変更は `Source/Plugin.cpp` と `Source/LuaEngine.cpp` の2ファイルだけです。
DLL名・ビルド方法（VC2022 の Release x64）は v10 と同じです。

## 何が変わるか

- 起動したプロセスが **rFactor2 Dedicated.exe** の場合、自動で判別して
  **オーバーレイウィンドウを作らず、DX11フックも入れない**「オーバーレイ無しモード」になります。
  画面のない Dedicated で描画系が一切動かないだけで、Lua のデータ系コールバック
  （スコアリング等）はクライアントと同じように動きます。
- クライアント（rFactor2.exe）で読み込まれた場合は **v10 と完全に同じ動作** です
  （オーバーレイ表示あり）。インストールフォルダはクライアントと Dedicated で
  共用されるため、DLL は1本のまま両方に対応する方式にしています。
- 判別はホストexe名に "Dedicated" を含むかどうかです。ログに
  `v10+dedicated: dedicated server host detected; no-overlay mode` と出れば判別成功です。
- Lua からも判別できるよう `rf2.is_dedicated`（true/false）を追加しました。

終了処理は、クライアントで従来から使われている「フックモード時」と同じ経路を
そのまま使います（作っていないオーバーレイの停止関数は即座に正常終了を返します）。

## 同梱スクリプト: Lua\Scripts\02_player_count_launcher.lua

- 人間プレイヤー（AI除く）が 0人→1人以上になった瞬間に1回だけ、
  `Bin64\Plugins\rF2OpenMeteosender.exe` を `--random` 付きで起動します
- WeatherTool v14以降は `--random` を付けると、起動直後に「ランダム」
  ボタンと同じ一括処理（座標→天候取得→rF2送信）を自動実行し、
  成功したら完了ダイアログを出さずにそのまま終了します
  （失敗時はエラーダイアログが出て、アプリは開いたままになります）
- 0人に戻ると再武装し、次に1人以上になったら再び起動します
- exe のパスは `rf2.plugin_dir()`（Bin64\Plugins）から自動で組み立てるため、
  インストール先がどこでも書き換え不要です

注意:
- 発動ホストは既定で Dedicated のみに制限しています（`RUN_ON = "dedicated"`）。
  Scripts フォルダはクライアントと共用されるため、この制限がないと
  クライアント側でも発動してしまいます。変える場合はファイル上部の
  `RUN_ON` を書き換えてください。
- WeatherTool が v13以前だと `--random` は無視され、アプリが開くだけに
  なります。必ず v14以降と組み合わせてください。

## 未検証

こちらでは MSVC ビルド・Dedicated 実機とも未検証です。まずビルドして
Dedicated を起動し、`Bin64\Plugins\Lua\lua_plugin.log` に
`Startup called: instance=... (v10+dedicated)` と
`dedicated server host detected` の2行が出るか確認してください。
