# Luaからの値の変更について

現行ソースには、SDKが値の変更を許可するコールバックに対するLua側の変更処理があります。ただし、SDKの全メンバーを任意に書き換える機能ではありません。

## 実装済みの変更経路
- `onForceFeedback(value)` : ForceFeedbackコールバックの戻り値で力を変更
- `onCheckHWControl(name, value)` : `CheckHWControl` の値を数値で返す
- `onPhysicsOptions(options)` : `PhysicsOptionsV01` の許可された項目をテーブルで返す
- `onWeather(info)` : `WeatherControlInfoV01` の変更可能な天候項目を返す
- `onCameraControl(info)` : カメラ/リプレイ制御情報を変更
- `onMessage(info)` : メッセージ情報を変更
- `onCommentary(info)` : 解説要求情報を変更

## 変更できないもの
`UpdateTelemetry` / `UpdateScoring` / `UpdateGraphics` で受け取る情報は読み取り用です。Luaテーブルを書き換えてもゲーム内部の値には反映されません。例: `mEngineRPM`, `mFuel`, `mGear` は現状の実装では表示・計算用の読み取り値です。

また、`PhysicsOptionsV01` や天候なども、SDKコールバックで変更可能と定義された項目だけが対象です。SDKが読み取り専用としている値をメモリ書き換えで変更する実装は行いません。
