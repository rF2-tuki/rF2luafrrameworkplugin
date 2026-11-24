# rF2 Lua Framework — Luaスクリプト リファレンス

対象: rF2 Lua Framework 1.2 (SDK V07) / Lua 5.5
（この文書はプラグインの実装 `LuaEngine.cpp` / `Plugin.cpp` を正として作成しています。v6 で SDK V07 の全構造体・全コールバックとの突き合わせを行い、未公開だった項目を追加しました）

---

## 1. 基本ルール

- スクリプトの置き場所: `rFactor 2\Bin64\Plugins\Lua\Scripts\*.lua`
- 起動時に**ファイル名の昇順**で全スクリプトが読み込まれます
- 全スクリプトは**1つの Lua 環境（グローバル空間）を共有**します。同名の関数・変数を複数スクリプトで定義すると、後に読み込まれた方で上書きされます
  - 特に `onRender` を定義するオーバーレイ系スクリプトは**同時に1つだけ**にしてください（過去に表示が出なかった原因がこれでした）
- Lua 5.5 の標準ライブラリ（`string`, `math`, `table`, `io`, `os` など）がそのまま使えます
- スクリプトのエラーはゲームを止めません。`Bin64\Plugins\Lua\lua_plugin.log` に `Lua callback error [関数名]: ...` と記録されます
- ログ出力には `print()` ではなく **`rf2.log()`** を使ってください（`print` の出力先はありません）

典型的な書き方は「データ受け取り用コールバックで値を保存し、`onRender` で描く」パターンです。

```lua
local telem = {}

function onTelemetry(data)
    telem = data          -- 最新のテレメトリを保存
end

function onRender(screen) -- 描画はここでだけ行う
    overlay.text(20, 20, string.format("RPM %.0f", telem.rpm or 0), 255, 255, 255, 20)
end
```

---

## 2. プラグインが用意する関数・値

### `rf2` テーブル

| 名前 | 形式 | 説明 |
|---|---|---|
| `rf2.log(msg)` | 関数 | 文字列を `lua_plugin.log` に `[Lua] ` 付きで書き出す |
| `rf2.plugin_dir()` | 関数 | プラグインのフォルダパスを返す（例: `...\Bin64\Plugins`） |
| `rf2.script_dir()` | 関数 | スクリプトフォルダパスを返す（`...\Plugins\Lua\Scripts`） |
| `rf2.time()` | 関数 | プラグイン起動からの経過秒（単調時計）。アニメーション用 |
| `rf2.blink(period [, duty])` | 関数 | 点滅判定。`period` 秒周期で、最初の `duty`（省略時 0.5）の割合だけ `true` を返す（第10節） |
| `rf2.version` | 文字列 | `"rF2 Lua Framework 1.2 (SDK V07)"` |

### `overlay` テーブル（描画）

| 名前 | 形式 | 説明 |
|---|---|---|
| `overlay.text(x, y, text [, r, g, b, size])` | 関数 | `onRender` の中で呼ぶと、そのフレームにテキストを描画する |
| `overlay.image(x, y, path [, width, height [, alpha]])` | 関数 | 画像ファイルを描画する（第10節） |

- `x`, `y`: 画面左上を原点とするピクセル座標
- `r`, `g`, `b`: 色（0〜255、省略時は白 255,255,255）
- `size`: フォントサイズ（省略時 18、8〜72 に丸められます）
- `onRender` の外で呼んでも描画されません（描画リストは `onRender` の呼び出しごとにリセットされます）

---

## 3. コールバック一覧（読み取り系）

定義したものだけが呼ばれます。すべて任意です。

| 関数 | 引数 | 呼ばれるタイミング・頻度 |
|---|---|---|
| `onStartup()` | なし | 全スクリプト読み込み後、1回 |
| `onShutdown()` | なし | プラグイン終了時、1回 |
| `onLoad()` / `onUnload()` | なし | シーン（コース）の読み込み / 解放時 |
| `onThreadStarted(type)` / `onThreadStopping(type)` | 整数 | ゲームのスレッド開始 / 停止時（type: 0=マルチメディア、1=シミュレーション） |
| `onDeactivateScreen(screen)` / `onReactivateScreen(screen)` | 画面テーブル | ウィンドウの非アクティブ化 / 再アクティブ化時 |
| `onStartSession()` | なし | セッション開始時 |
| `onEndSession()` | なし | セッション終了時 |
| `onEnterRealtime()` | なし | 走行画面に入ったとき |
| `onExitRealtime()` | なし | 走行画面を出たとき |
| `onTelemetry(data)` | テレメトリテーブル | 走行中、**最大100Hz**に制限（このプラグインの仕様） |
| `onScoring(data)` | スコアリングテーブル | ゲームの更新間隔（約5Hz） |
| `onGraphics(data)` | グラフィックステーブル | **最大30Hz**に制限 |
| `onEnvironment(data)` | 環境テーブル | セッション環境が渡されるとき |
| `onRender(screen)` | 画面テーブル | 描画ごと（約30Hz）。`mWidth` / `mHeight` が常に入ります。ゲームから ScreenInfo が渡された場合は `mPixelFormat`, `mRefreshRate`, `mWindowed`, `mOptionsWidth`, `mOptionsHeight`, `mOptionsLeft`, `mOptionsUpper`, `mOptionsLocation`, `mOptionsPage` も入ります（オーバーレイウィンドウ方式ではサイズのみ） |

受け取ったテーブルは読み取り専用です。書き換えてもゲームには反映されません。

---

## 4. `onTelemetry(data)` のテーブル

自車の詳細データです。多くの項目は **2種類の名前**で読めます（短い名前と、SDKそのままの `m` 付き名前）。どちらも同じ値です。

### 主な項目（短い名前）

| フィールド | 型 | 内容 |
|---|---|---|
| `id` | 整数 | 車両ID |
| `delta_time` | 数値 | 前回更新からの経過時間（秒） |
| `elapsed_time` | 数値 | セッション経過時間（秒） |
| `lap` | 整数 | 現在の周回数 |
| `lap_start_time` | 数値 | この周回の開始時刻（経過時間） |
| `vehicle` / `track` | 文字列 | 車両名 / コース名 |
| `gear` | 整数 | ギア（-1=R、0=N、1〜=前進） |
| `rpm` / `max_rpm` | 数値 | エンジン回転数 / レブリミット |
| `water_temp` / `oil_temp` | 数値 | 水温 / 油温（**摂氏**） |
| `fuel` / `fuel_capacity` | 数値 | 燃料残量 / 容量（リットル） |
| `throttle` / `brake` / `steering` / `clutch` | 数値 | ペダル・ステア入力（フィルター前、概ね 0.0〜1.0、ステアは -1.0〜1.0） |
| `speed_x` / `speed_y` / `speed_z` | 数値 | 車体座標系の速度（m/s）。速さは `math.sqrt(x^2+y^2+z^2)`、km/h は ×3.6 |
| `world_x` / `world_y` / `world_z` | 数値 | ワールド座標 |
| `steering_torque` | 数値 | ステアリングシャフトトルク |
| `engine_torque` | 数値 | エンジントルク |
| `boost` | 数値 | ターボブースト圧 |

### `m` 付き名前でのみ読める追加項目

上記と重複するもの（`mEngineRPM`, `mGear`, `mFuel` など、SDK名そのまま）以外に:

- フィルター後の入力: `mFilteredThrottle`, `mFilteredBrake`, `mFilteredSteering`, `mFilteredClutch`
- ベクトル（`{x=, y=, z=}` テーブル）: `mLocalVel`, `mLocalAccel`, `mPos`, `mLocalRot`, `mLocalRotAccel`, `mLastImpactPos`
- 姿勢行列: `mOri`（1〜3 のベクトルテーブル配列）
- 空力・車高: `mFrontWingHeight`, `mFrontRideHeight`, `mRearRideHeight`, `mDrag`, `mFrontDownforce`, `mRearDownforce`
- 状態: `mClutchRPM`, `mScheduledStops`, `mOverheating`（真偽）, `mDetached`（真偽）, `mHeadlights`（真偽）, `mCurrentSector`, `mSpeedLimiter`, `mMaxGears`, `mIgnitionStarter`
- 衝撃: `mLastImpactET`, `mLastImpactMagnitude`
- タイヤ関連: `mFrontTireCompoundIndex`, `mRearTireCompoundIndex`, `mFrontTireCompoundName`, `mRearTireCompoundName`, `mFrontFlapActivated`, `mRearFlapActivated`, `mRearFlapLegalStatus`
- その他: `mSpeedLimiterAvailable`, `mAntiStallActivated`, `mVisualSteeringWheelRange`, `mPhysicalSteeringWheelRange`, `mRearBrakeBias`, `mFront3rdDeflection`, `mRear3rdDeflection`, `mDentSeverity`（1〜8 の整数配列）, `mPhysicsToGraphicsOffset`（1〜3 の数値配列）

### ホイール情報 `data.wheels`（= `data.mWheel`、同じテーブル）

添字 1〜4（左前・右前・左後・右後）の配列。各要素:

| フィールド（短い名前） | 内容 |
|---|---|
| `ride_height` | ライドハイト |
| `suspension_deflection` | サスペンション変位 |
| `brake_temp` | ブレーキ温度 |
| `tire_load` | タイヤ荷重 |
| `pressure` | 空気圧 |
| `wear` | 摩耗（1.0=新品 → 0.0） |
| `temperature_inner` / `temperature_middle` / `temperature_outer` | タイヤ表面温度（**ケルビン**。摂氏は -273.15） |
| `lateral_force` / `longitudinal_force` | 横方向 / 前後方向の力 |

`m` 付き名前でも同じ項目が読めます（`mRideHeight`, `mBrakeTemp`, `mWear` など）。ほかに `mRotation`, `mCamber`, `mToe`, `mGripFract`, `mSuspForce`, `mBrakePressure`, `mFlat`（真偽）, `mDetached`（真偽）, `mSurfaceType`, `mTerrainName`, `mTireCarcassTemperature`, `mTemperature`（1〜3 のケルビン配列）などがあります。

---

## 5. `onScoring(data)` のテーブル

セッション全体と全車両の順位情報です。フィールド名は SDK そのまま（`m` 付き）です。

### セッション情報

| フィールド | 内容 |
|---|---|
| `mTrackName` | コース名 |
| `mSession` | セッション種別（下記） |
| `mCurrentET` / `mEndET` | 現在 / 終了の経過時間（秒） |
| `mMaxLaps` | 最大周回数 |
| `mLapDist` | 1周の距離（m） |
| `mNumVehicles` | 参加台数 |
| `mGamePhase` | 進行フェーズ（下記） |
| `mYellowFlagState`, `mSectorFlag`（1〜3） | イエローフラグ状態 / セクター別フラグ |
| `mStartLight`, `mNumRedLights` | スタートライト |
| `mInRealtime` | 真偽 |
| `mPlayerName`, `mPlrFileName` | プレイヤー名 / PLRファイル名 |
| `mDarkCloud`, `mRaining` | 雲量 / 雨の強さ（0.0〜1.0） |
| `mAmbientTemp`, `mTrackTemp` | 気温 / 路面温度（**摂氏**、読み取り専用） |
| `mWind` | 風速ベクトル `{x=, y=, z=}` |
| `mMinPathWetness`, `mMaxPathWetness` | 路面の濡れ具合 |
| `mResultsStream` | 結果ストリームへの追加分（改行区切りの文字列） |

`mSession` の値: 0=テストデー、1〜4=プラクティス、5〜8=予選、9=ウォームアップ、10〜13=レース

`mGamePhase` の値: 0=開始前、1=レコンフラップ、2=グリッドウォーク、3=フォーメーションラップ、4=カウントダウン開始、5=グリーンフラッグ、6=フルコースイエロー、7=セッション停止、8=セッション終了

### 車両情報 `data.mVehicle`（添字 1〜台数）

主な項目: `mID`, `mDriverName`, `mVehicleName`, `mVehicleClass`, `mPlace`（順位）, `mIsPlayer`（真偽）, `mTotalLaps`, `mLapDist`, `mBestLapTime`, `mBestSector1`, `mBestSector2`, `mLastLapTime`, `mLastSector1`, `mLastSector2`, `mCurSector1`, `mCurSector2`, `mTimeBehindNext`, `mTimeBehindLeader`, `mLapsBehindNext`, `mLapsBehindLeader`, `mNumPitstops`, `mNumPenalties`, `mInPits`（真偽）, `mSector`（0=セクター3、1=セクター1、2=セクター2）, `mFinishStatus`（0=なし、1=完走、2=DNF、3=DQ）, `mControl`, `mFlag`, `mUnderYellow`（真偽）, `mInGarageStall`（真偽）, `mPos` / `mLocalVel` / `mLocalAccel` / `mLocalRot` / `mLocalRotAccel`（ベクトル）, `mOri`（姿勢行列、1〜3 のベクトル配列）, `mTimeIntoLap`, `mEstimatedLapTime`, `mLapStartET`, `mPitGroup`, `mPathLateral`, `mTrackEdge`, `mCountLapFlag`, `mHeadlights`, `mPitState`（0=なし、1=要求、2=進入中、3=停止中、4=退出中）, `mServerScored`, `mIndividualPhase`, `mQualification`（予選順位、1始まり、無効時 -1）, `mUpgradePack`（1〜16 の整数配列）

自分の順位を取る例:

```lua
for _, v in ipairs(scoring.mVehicle or {}) do
    if v.mIsPlayer then myplace = v.mPlace end
end
```

---

## 6. `onGraphics(data)` / `onEnvironment(data)`

`onGraphics`: `mCamPos`（カメラ位置ベクトル）, `mCamOri`（1〜3 のベクトル配列）, `mAmbientRed` / `mAmbientGreen` / `mAmbientBlue`（環境光）, `mID`, `mCameraType`

`onEnvironment`: `mPath`（1〜16 の文字列配列、パスが無い要素は `nil`）

---

## 7. コールバック一覧（書き換え系）

ゲーム側の値を変更できるのはこの経路だけです。**戻り値の規則が関数ごとに違う**ので注意してください。

| 関数 | 引数 | 戻り値と効果 |
|---|---|---|
| `onForceFeedback(value)` | 現在のFFB値 | **数値**を返すとその値に差し替え。`nil` で変更なし |
| `onCheckHWControl(name, value)` | コントロール名と現在値 | **数値**を返すと差し替え。対象外の名前には `nil` を返す |
| `onPhysicsOptions(options)` | 現在の設定テーブル | 変更したい項目を入れた**テーブル**を返す（返す項目だけ反映） |
| `onWeather(info)` | 現在の天候テーブル | 変更したい項目を入れた**テーブル**を返すと反映。`nil` で変更なし |
| `onCameraControl(info)` | カメラ/リプレイ情報 | **テーブル, 数値** の2つを返す。数値は「表示する車両ID」。変更しない場合は `return nil, 0` |
| `onMessage(info)` | `{mText=, mDestination=, mTranslate=}` | 書き換えた**テーブル**を返すとメッセージを変更。`nil` で変更なし |
| `onCommentary(info)` | `{mName=, mInput1=, mInput2=, mInput3=, mSkipChecks=}` | 書き換えた**テーブル**を返すと実況リクエストを変更。`nil` で変更なし |

### `onPhysicsOptions` で変更できる項目

ドライビングエイド系（数値）: `mTractionControl`, `mAntiLockBrakes`, `mStabilityControl`, `mAutoShift`, `mAutoClutch`, `mInvulnerable`, `mOppositeLock`, `mSteeringHelp`, `mBrakingHelp`, `mSpinRecovery`, `mAutoPit`, `mAutoLift`, `mAutoBlip`, `mFuelMult`, `mTireMult`, `mMechFail`, `mAllowPitcrewPush`, `mRepeatShifts`, `mHoldClutch`, `mAutoReverse`, `mAlternateNeutral`, `mAIControl`

小数系: `mManualShiftOverrideTime`, `mAutoShiftOverrideTime`, `mSpeedSensitiveSteering`, `mSteerRatioSpeed`

### `onWeather` で変更できる項目

`mET`, `mCloudiness`, `mAmbientTempK`（気温、**ケルビン**）, `mWindMaxSpeed`, `mApplyCloudinessInstantly`（真偽）, `mRaining`（3×3 配列、添字 1〜3）

※ 路面温度（`mTrackTemp`）は SDK 上読み取り専用のため、この経路でも変更できません。

### その他の書き換え系コールバック（v6 で追加）

| 関数 | 内容 |
|---|---|
| `onPitMenu(info)` | ピットメニューの現在のカテゴリ・選択肢が渡されます（`mCategoryIndex`, `mCategoryName`, `mChoiceIndex`, `mChoiceString`, `mNumChoices`）。テーブルを返すと `mCategoryIndex` / `mChoiceIndex` の変更が反映されます。この関数を定義したスクリプトがある場合だけ、ゲームからアクセスを受けます |
| `onTrackRules(info)` | フォーメーションラップ／フルコースイエローのルール情報。入力: `mCurrentET`, `mStage`, `mPoleColumn`, `mAction`（直近アクションの配列: `mCommand`, `mID`, `mET`）、セーフティカー情報など。入出力: `mYellowFlagState`, `mYellowFlagLaps`, `mSafetyCarInstruction`（0=変更なし、1=出動、2=ピットへ）, `mSafetyCarSpeed`, 各種スペーシング・速度制限、`mMessage`、`mParticipant`（各車両の `mRelativeLaps`, `mColumnAssignment`, `mPositionAssignment`, `mAllowedToPit`, `mGoalRelativeDistance`, `mMessage`）。テーブルを返すと入出力項目が反映されます |
| `onMultiSessionRules(info)` | 複数セッション構成のルール情報。入力: `mSession`, `mSpecialSlotID`, `mTrackType`, 参加者情報（`mParticipant` 配列: `mBestPracticeTime`, `mQualificationTime` など）。入出力: `mNumQualSessions`, `mNumRaceSessions`, `mMaxLaps`, `mMaxSeconds`, `mName`、参加者ごとの `mServerScored`, `mGridPosition` |
| `onInitCustomControl(n)` | 起動時に n=1,2,3… と繰り返し呼ばれます。`{ mUntranslatedName = "名前", mRepeat = 0 }` を返すとカスタムコントロールを登録し、`nil` を返すと登録を終了します（`mRepeat`: 0=押すたび1回、1=長押しでリピート、2=押している間）。登録した名前は `onCheckHWControl` に `_名前` という形で届きます |
| `onGetCustomVariable(n)` | カスタム変数（V07）の列挙。n=1,2,3… に対して `{ mCaption = "変数名", mNumSettings = 選択肢数, mCurrentSetting = 初期値 }` を返し、無くなったら `nil`。`mNumSettings = 0` は文字列型の変数になります |
| `onGetCustomVariableSetting(caption, n)` | カスタム変数の n 番目の選択肢名（文字列）を返します |
| `onCustomVariable(var)` | カスタム変数の現在値が届きます（`mCaption`, `mNumSettings`, `mCurrentSetting`）。テーブルで `mCurrentSetting` を返すと値を差し替えられます |

---

## 8. 書き方のコツ・注意

- **値はまだ来ていないことがあります**: セッション開始直後は保存用変数が空なので、`telem.rpm or 0` のように `or` で既定値を用意するか、`if telem.mEngineRPM then` で守ってください。未初期化のまま計算するとエラーになり、そのコールバックの処理が止まります（エラーはログに残ります）
- **重い処理を毎回やらない**: `onTelemetry` は最大100Hzで呼ばれます。文字列整形やログ出力は `onRender` 側に寄せるか、数回に1回に間引いてください
- **温度の単位が項目で違います**: エンジン水温・油温と気温・路面温度は摂氏、タイヤ表面温度はケルビンです
- **速度**: `mLocalVel` は m/s です。km/h は `math.sqrt(v.x^2 + v.y^2 + v.z^2) * 3.6`
- **描画は保存パターンで**: コールバックで受け取ったテーブルを `local` 変数に保存し、`onRender` で読むのが基本形です（付属の `debug_overlay.lua` / `race_overlay.lua` がその形になっています）
- オーバーレイ表示はウィンドウモード／ボーダーレス専用です（外部ウィンドウ方式のため）

## 9. 最小サンプル

```lua
-- my_overlay.lua （Scripts フォルダに置く。onRender を持つスクリプトは1つだけ）
local t, s = {}, {}

function onStartup()
    rf2.log("my overlay loaded")
end

function onTelemetry(data) t = data end
function onScoring(data) s = data end

function onRender(screen)
    local y = 16
    local function line(str, r, g, b, size)
        overlay.text(16, y, str, r or 255, g or 255, b or 255, size or 18)
        y = y + (size or 18) + 6
    end
    line(string.format("RPM  %.0f", t.rpm or 0))
    line(string.format("GEAR %d", t.gear or 0))
    line(string.format("FUEL %.1f L", t.fuel or 0), 150, 220, 255)
end
```

付属サンプル: `Lua\Examples\debug_overlay.lua`（全項目表示）, `race_overlay.lua`（レース用最小表示）, `dx11_overlay_example.lua`（簡易版）, `Lua\Scripts\01_example.lua`（書き換え系コールバックの雛形）

---

## 10. 画像の描画と点滅

### 画像（`overlay.image`）

```lua
function onRender(screen)
    overlay.image(20, 100, "logo.png")              -- 原寸で表示
    overlay.image(20, 100, "logo.png", 200, 80)     -- 200x80 に伸縮
    overlay.image(20, 100, "logo.png", 200)         -- 幅200、高さは縦横比を維持
    overlay.image(20, 100, "logo.png", 200, 80, 128) -- 不透明度 50%（alpha 0〜255）
end
```

- 対応形式は Windows の標準コーデックが読めるもの（PNG / JPEG / BMP / GIF など）。透過 PNG はそのまま透過します
- 相対パスは **Scripts フォルダ基準**で解決されます。絶対パスも使えます
- 画像は初回だけデコードしてキャッシュされます。存在しないファイルは描画されず、パスごとに1回だけログに記録されます
- 文字と画像は呼んだ順に重なります（後に呼んだものが上）

### 点滅（`rf2.blink`）

点滅は「点滅させたい間隔で描くかどうかを切り替える」方式です。テキストにも画像にも、すべての要素に使えます。

```lua
function onRender(screen)
    -- 0.8秒周期で点滅（半分点灯・半分消灯）
    if rf2.blink(0.8) then
        overlay.text(20, 20, "LOW FUEL", 255, 80, 80, 24)
    end
    -- 1秒周期、点灯70%・消灯30%で画像を点滅
    if rf2.blink(1.0, 0.7) then
        overlay.image(20, 60, "warn.png", 48, 48)
    end
end
```

- `rf2.blink(period)` は `period` 秒周期で前半だけ `true`、`rf2.blink(period, duty)` は点灯割合を変更できます
- 色・大きさの変更は従来通り `overlay.text` の引数そのもので行います（点滅と組み合わせれば「点滅しながら色を変える」も可能です。`rf2.time()` を使えばグラデーションのような連続変化もできます）

---

## 11. 網羅範囲について（SDK V07 監査の結果）

v6 で `InternalsPlugin.hpp` の全構造体・全コールバックと突き合わせました。データ項目は原則すべて公開しています。以下だけは「変数」ではないため意図的に公開していません。

- `mExpansion` / `mUnused` 系: SDK が将来用に確保した予約領域（中身は常に未使用）
- `mAppWindow` / `mDevice` / `mRenderTarget` / `mDriver` / `mHWND`: OS のハンドルやポインタで、Lua から意味のある操作ができないもの
