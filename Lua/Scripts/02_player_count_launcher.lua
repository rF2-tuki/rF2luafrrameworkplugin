-- 02_player_count_launcher.lua
-- rF2 Lua Framework (v10 + Dedicated対応版) 用
-- 置き場所: rFactor 2\Bin64\Plugins\Lua\Scripts\
--
-- 動作:
--   Dedicated 上の人間プレイヤーが 0人 -> 1人以上 になった瞬間に1回だけ
--   WeatherTool (rF2OpenMeteosender.exe) を "--random" 付きで起動する。
--   WeatherTool v14以降は "--random" を付けると、起動直後に「ランダム」
--   ボタンと同じ一括処理 (座標 -> 天候取得 -> rF2送信) を自動実行し、
--   成功したら完了ダイアログを出さずにそのまま終了する。
--   人数が 0人 に戻ると再武装し、次に1人以上になったら再び起動する。
--
-- 注意:
--   - 全スクリプトでグローバル環境を共有します。他のスクリプトが
--     onScoring を定義している場合は、そちらが優先（後勝ち）になるため、
--     このファイルの処理を既存の onScoring に組み込んでください。
--     01_example.lua は onScoring を定義していないので併用できます。
--   - 起動先はこのスクリプトが動いているのと同じPC・同じrFactor 2
--     インストールです。

-- ===== 設定 =====
-- 起動対象: rf2.plugin_dir() は Bin64\Plugins そのものなので、その直下。
local EXE_PATH = rf2.plugin_dir() .. "\\rF2OpenMeteosender.exe"
-- WeatherTool v14以降の自動実行オプション (ランダム実行して成功時に終了)。
local EXE_ARGS = "--random"

-- 発動するホスト:
--   "dedicated" = Dedicated 上で動いているときだけ発動 (既定)
--   "client"    = クライアント上で動いているときだけ発動
--   "any"       = 制限なし
-- rf2.is_dedicated は Dedicated対応版DLLでのみ true/false が入ります。
-- 旧v10 DLLでは nil になるため、その場合は制限なしとして扱います。
local RUN_ON = "dedicated"
-- ================

local armed = true  -- true の間だけ発動できる（発動後は 0人に戻るまで false）

local function host_allowed()
    if RUN_ON == "any" then return true end
    if rf2.is_dedicated == nil then return true end
    if RUN_ON == "dedicated" then return rf2.is_dedicated == true end
    if RUN_ON == "client" then return rf2.is_dedicated == false end
    return true
end

local function count_humans(scoring)
    -- mNumVehicles は AI を含む全台数なので使わない。
    -- mControl: 0=ローカルプレイヤー, 1=ローカルAI, 2=リモートプレイヤー
    -- 人間 = 0 と 2 の台数（Dedicated上では実質 2 のみ）。
    local humans = 0
    for _, v in ipairs(scoring.mVehicle or {}) do
        if v.mControl == 0 or v.mControl == 2 then
            humans = humans + 1
        end
    end
    return humans
end

function onScoring(scoring)
    local humans = count_humans(scoring)

    if humans == 0 then
        if not armed then
            armed = true  -- 0人に戻ったので再武装
            rf2.log("[launcher] players back to 0, re-armed")
        end
        return
    end

    -- humans >= 1
    if armed and host_allowed() then
        armed = false  -- onScoring は約5Hzで呼ばれるので、ここで1回だけに絞る
        -- "start" 経由で非同期起動する。os.execute はコマンド終了まで
        -- 呼び出し側が止まるため、exe を直接指定してはいけない。
        local cmd = string.format('start "" /B "%s" %s', EXE_PATH, EXE_ARGS)
        rf2.log(string.format("[launcher] humans=%d, launching: %s", humans, cmd))
        os.execute(cmd)
    end
end
