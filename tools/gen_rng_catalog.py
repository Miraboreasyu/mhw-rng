#!/usr/bin/env python3
"""
Generates `src/rng_catalog_data.inl` from `all_rng_sites_1523.tsv` + `mhw.db`,
giving EVERY RNG call site and enclosing RNG function in MHW:I 15.23:
  1. A clean, unique, easy-to-type English alias (e.g. `em.size`, `ai.think`, `em013.act.1`)
  2. Built-in short single-word English keywords (`size`, `think`, `target`, `meal`, `carve`, `fatalis`...)
  3. A clear Chinese functional description + Core Gameplay vs Background Noise classification
"""
import bisect
import os
import re
import sqlite3
from collections import defaultdict
from pathlib import Path

REPO_ROOT  = Path(__file__).resolve().parent.parent
TSV_PATH   = os.environ.get("MHW_RNG_SITES_TSV", "all_rng_sites_1523.tsv")
SCALE_PATH = os.environ.get("MHW_RNG_SCALES_TSV", "rng_site_scales.tsv")
DB_PATH    = os.environ.get("MHW_SEMANTIC_DB", "mhw.db")
OUT_INL    = os.environ.get(
    "MHW_RNG_CATALOG_OUT",
    str(REPO_ROOT / "plugins" / "inspector" / "rng_catalog_data.inl"),
)

# ScaleKind IDs (how MHW's machine code consumes the returned u32 right after the RNG call)
SCALE_FLOAT_100 = 0  # (float)u32 * 2^-32 or * (100 * 2^-32) -> [0.0 .. 100.0)
SCALE_MOD_100   = 1  # u32 % 100 -> integer 0..99
SCALE_INT_MOD   = 2  # u32 % N or bitmask -> small integer index 0..N-1
SCALE_RAW_U32   = 3  # raw 32-bit seed / state word

MONSTER_INFO = {
    "001": ("雌火龙",   "Rathian"),
    "002": ("火龙",     "Rathalos"),
    "007": ("角龙",     "Diablos"),
    "011": ("麒麟",     "Kirin"),
    "013": ("黑龙",     "Fatalis"),
    "018": ("黑狼鸟",   "Garuga"),
    "023": ("金狮子",   "Rajang"),
    "024": ("钢龙",     "Kushala"),
    "026": ("炎妃龙",   "Lunastra"),
    "027": ("炎王龙",   "Teostra"),
    "032": ("轰龙",     "Tigrex"),
    "036": ("熔岩龙",   "Lavasioth"),
    "037": ("迅龙",     "Nargacuga"),
    "042": ("冰牙龙",   "Barioth"),
    "043": ("恐暴龙",   "Deviljho"),
    "044": ("土砂龙",   "Barroth"),
    "045": ("爆锤龙",   "Uragaan"),
    "050": ("煌黑龙",   "Alatreon"),
    "057": ("雷狼龙",   "Zinogre"),
    "063": ("碎龙",     "Brachydios"),
    "080": ("斩龙",     "Glavenus"),
    "100": ("蛮颚龙",   "Anjanath"),
    "101": ("大凶豺龙", "GreatJagras"),
    "102": ("毒妖鸟",   "Pukei"),
    "103": ("灭尽龙",   "Nergigante"),
    "104": ("冥赤龙",   "Safijiiva"),
    "105": ("冥灯龙",   "Xenojiiva"),
    "106": ("熔山龙",   "Zorah"),
    "107": ("搔鸟",     "Kulu"),
    "108": ("泥鱼龙",   "Jyuratodus"),
    "109": ("飞雷龙",   "Tobi"),
    "110": ("浮空龙",   "Paolumu"),
    "111": ("风漂龙",   "Legiana"),
    "112": ("大凶颚龙", "GreatGirros"),
    "113": ("惨爪龙",   "Odogaron"),
    "114": ("骨锤龙",   "Radobaan"),
    "115": ("尸套龙",   "VaalHazak"),
    "116": ("岩贼龙",   "Dodogama"),
    "117": ("绚辉龙",   "KulveTaroth"),
    "118": ("爆鳞龙",   "Bazelgeuse"),
    "120": ("眩鸟",     "Tzitzi"),
    "121": ("贝希摩斯", "Behemoth"),
    "122": ("冰鱼龙",   "Beotodus"),
    "123": ("猛牛龙",   "Banbaro"),
    "124": ("冰咒龙",   "Velkhana"),
    "125": ("溟波龙",   "Namielle"),
    "126": ("天地煌啼龙", "SharaIshvalda"),
    "127": ("鹿首精",   "Leshen"),
}

# Category IDs (0..7 = Core Gameplay, 8..12 = Background / Cosmetic)
CAT_EM_STAT   = 0  # 核心:怪物数值
CAT_EM_AI     = 1  # 核心:怪物AI与仇恨
CAT_EM_ACT    = 2  # 核心:怪物招式动作
CAT_QUEST     = 3  # 核心:任务与调查
CAT_PLAYER    = 4  # 核心:猎人与弹道
CAT_OTOMO     = 5  # 核心:随从猫与骑乘
CAT_LOOT      = 6  # 核心:剥取与报酬掉落
CAT_HUB       = 7  # 核心:猫饭/炼金/觉醒/蒸汽机
CAT_BG_ANIMAL = 8  # 背景:环境生物
CAT_BG_STAGE  = 9  # 背景:据点NPC与地图机关
CAT_BG_UI     = 10 # 背景:UI伤害跳字与界面
CAT_BG_VFX    = 11 # 背景:粒子特效/雪地/音效
CAT_BG_OTHER  = 12 # 背景:其它底层随机

CURATED_RVA = {
    0x169BA15: (CAT_EM_STAT, "em.size",        "怪物体型大小金倍率抽取 (cSizeTable::getSizeMultiplier)", "Monster crown/size multiplier roll (cSizeTable::getSizeMultiplier)"),
    0x1CA9596: (CAT_EM_STAT, "em.dying",       "怪物濒死回家血线比例浮动 (cDyingRate::ctor)", "Monster dying/limp HP threshold jitter (cDyingRate::ctor)"),
    0x02C5349: (CAT_EM_STAT, "em.init_hp",     "怪物部位与初始生命参数随机项 (uEnemy)", "Monster part & initial HP variance (uEnemy)"),
    0x02CD7E6: (CAT_EM_STAT, "em.hp_check",    "怪物血量状态随机检定 (uEnemy)", "Monster HP state random check (uEnemy)"),
    0x1339C8C: (CAT_EM_AI,   "ai.think",       "怪物AI思考树(.thk)出招分支概率抽签 (cThinkMgr::updateThk)", "Monster AI think-tree (.thk) branch selection (cThinkMgr::updateThk)"),
    0x1334238: (CAT_EM_AI,   "ai.target",      "怪物随机点名猎人或随从猫仇恨 (TargetRandomPlayerOrPalico)", "Monster random player/Palico aggro pick (TargetRandomPlayerOrPalico)"),
    0x0A2A41D: (CAT_EM_AI,   "ai.target_eval", "怪物AI目标与随从评估随机项 (uAiEm)", "Monster AI target & Palico evaluation roll (uAiEm)"),
    0x1BAA8A9: (CAT_EM_AI,   "ai.em_range",    "怪物AI区间随机参数 (uAiEm)", "Monster AI range parameter roll (uAiEm)"),
    0x1BB746E: (CAT_EM_AI,   "ai.em_lot.1",    "怪物AI区间抽签 #1 (uAiEm)", "Monster AI range lottery #1 (uAiEm)"),
    0x1BB74BB: (CAT_EM_AI,   "ai.em_lot.2",    "怪物AI区间抽签 #2 (uAiEm)", "Monster AI range lottery #2 (uAiEm)"),
    0x1B65EB0: (CAT_QUEST,   "quest.accept",   "接任务开局随机状态抽签 (sQuest::AcceptQuest)", "Quest accept initial state lottery (sQuest::AcceptQuest)"),
    0x1B6015E: (CAT_QUEST,   "quest.bonus.1",  "任务/探索开局环境与增益抽签 #1 (sQuest)", "Quest weather/environment & bonus roll #1 (sQuest)"),
    0x1B60201: (CAT_QUEST,   "quest.bonus.2",  "任务猫车/乱入附加状态抽签 #2 (sQuest)", "Quest extra cart/intruder state roll #2 (sQuest)"),
    0x1B60245: (CAT_QUEST,   "quest.bonus.3",  "任务附加随机槽位 (0~3) 抽签 #3 (sQuest)", "Quest extra state slot (0..3) roll #3 (sQuest)"),
    0x1B6028B: (CAT_QUEST,   "quest.seed_tok", "任务随机标识码生成 (sQuest)", "Quest random seed token generation (sQuest)"),
    0x1B5A2D7: (CAT_QUEST,   "quest.invest",   "接取调查任务随机处理 (sQuest::acceptInvestigation)", "Investigation quest accept RNG (sQuest::acceptInvestigation)"),
    0x1B5FC02: (CAT_QUEST,   "quest.tempered", "历战调查任务派生抽取 (tryAddTemperedInvestigationEx)", "Tempered investigation derivation roll (tryAddTemperedInvestigationEx)"),
    0x1B5E7A7: (CAT_QUEST,   "quest.discover", "怪物发现/乱入抽签数据 (cDiscoverLotData)", "Monster discovery/intruder lottery (cDiscoverLotData)"),
    0x0AFE2BC: (CAT_LOOT,    "loot.carve.1",   "怪物本体/断尾剥取抽签 #1 (populateCarves)", "Monster body/tail carve table roll #1 (populateCarves)"),
    0x0AFE2D9: (CAT_LOOT,    "loot.carve.2",   "怪物本体/断尾剥取抽签 #2 (populateCarves)", "Monster body/tail carve table roll #2 (populateCarves)"),
    0x0AFE326: (CAT_LOOT,    "loot.carve.3",   "怪物本体/断尾剥取抽签 #3 (populateCarves)", "Monster body/tail carve table roll #3 (populateCarves)"),
    0x0AFE345: (CAT_LOOT,    "loot.carve.4",   "怪物本体/断尾剥取抽签 #4 (populateCarves)", "Monster body/tail carve table roll #4 (populateCarves)"),
    0x1898970: (CAT_LOOT,    "loot.item",      "通用道具概率表抽签 (cItemLottery::getRandomEntry)", "Generic item lottery table roll (cItemLottery::getRandomEntry)"),
    0x0A28CB7: (CAT_LOOT,    "loot.track",     "摸脚印/痕迹掉落拾取抽签 (uEnemy::getTrackPickupItem)", "Track/footprint pickup drop roll (uEnemy::getTrackPickupItem)"),
    0x0F99246: (CAT_HUB,     "meal.trigger",   "猫饭技能是否发动概率判定 (Kitchen)", "Canteen food skill activation roll (Kitchen)"),
    0x0F9995A: (CAT_HUB,     "meal.daily",     "猫饭每日随机技能抽取 (uGUIKitchen01)", "Canteen daily random skills roll (uGUIKitchen01)"),
    0x05C9AA7: (CAT_HUB,     "steam.item",     "蒸汽机物品奖励抽取 (cSteamItem::generateRandom)", "Steamworks item reward roll (cSteamItem::generateRandom)"),
    0x066679A: (CAT_HUB,     "steam.code.1",   "蒸汽机三键排列生成 #1 (cSteamControl::start)", "Steamworks 3-key sequence gen #1 (cSteamControl::start)"),
    0x06667BB: (CAT_HUB,     "steam.code.2",   "蒸汽机三键排列生成 #2 (cSteamControl::start)", "Steamworks 3-key sequence gen #2 (cSteamControl::start)"),
    0x197F9CF: (CAT_HUB,     "meld.deco",      "炼金婆婆装饰品珠子抽签 (Melding)", "Elder Melder decoration jewel lottery (Melding)"),
    0x20782B1: (CAT_PLAYER,  "pl.shell_vec",   "投射器/弹丸球面随机散布向量 (rShellParam)", "Slinger/projectile sphere spread vector (rShellParam)"),
    0x04C928D: (CAT_EM_ACT,  "em050.meteor",   "煌黑龙(Alatreon)陨石随机落点 (cEm050MeteorMgr)", "Alatreon meteor landing scatter (cEm050MeteorMgr)"),
    0x02AECBC: (CAT_OTOMO,   "pl.mount",       "骑乘处决/软化点位随机控制 (cEmCharmMountManager)", "Clutch-claw/mount position RNG (cEmCharmMountManager)"),
    0x1E1A75E: (CAT_BG_UI,   "ui.dmg_x",       "伤害跳字屏幕横向微抖动 (formatDamageNumber)", "Damage number screen X jitter (formatDamageNumber)"),
    0x1E1A796: (CAT_BG_UI,   "ui.dmg_y",       "伤害跳字屏幕纵向微抖动 (formatDamageNumber)", "Damage number screen Y jitter (formatDamageNumber)"),
    0x1E1BA00: (CAT_BG_UI,   "ui.behe_dmg_x",  "贝希摩斯伤害跳字横向抖动", "Behemoth damage number X jitter"),
    0x1E1BA38: (CAT_BG_UI,   "ui.behe_dmg_y",  "贝希摩斯伤害跳字纵向抖动", "Behemoth damage number Y jitter"),
}


def classify_auto(rva, kind, owner, callees, dbfn, dbmod, neighbor_mod):
    if rva in CURATED_RVA:
        return CURATED_RVA[rva]

    va = 0x140000000 + rva
    blob = f"{owner} {callees} {dbfn} {dbmod} {neighbor_mod}"
    low = blob.lower()

    # Safi'jiiva Awakened Weapon Upgrade cluster (0x140526800..0x14052DF00 & 0x140B1C480..0x140B1DC00)
    if (0x140526800 <= va <= 0x14052DF00) or (0x140B1C400 <= va <= 0x140B1DC50) or "equipweaponupgrade" in low or "effect_lot" in low:
        return (CAT_HUB, "awakening.lot", "冥赤龙觉醒武器词条抽取/洗练 (Safi Awakening)", "Safi'jiiva awakened weapon ability roll (Safi Awakening)")

    # Save encryption salt cluster
    if 0x14031B500 <= va <= 0x14031BB00:
        return (CAT_BG_OTHER, "sys.save_salt", "存档初始化加密盐随机 (cPCSaveData)", "Save init encryption salt RNG (cPCSaveData)")

    # 1. Specific Monster IDs (em001..em127 / uEmXXX / nActEmXXX)
    m_em = re.search(r"(?:nActEm|uEm|uAIEm|cEm|em|怪物动作通用族\s*|怪物动作AI\s*)(\d{3})(?:_(\d{2}))?", blob)
    if m_em:
        em_id = m_em.group(1)
        sub_id = m_em.group(2)
        if em_id in MONSTER_INFO:
            m_cn, m_en = MONSTER_INFO[em_id]
            m_label = f"{m_cn}({m_en})"
            m_label_en = m_en
        else:
            m_label = f"怪物em{em_id}"
            m_label_en = f"Monster em{em_id}"
        if sub_id and sub_id != "00":
            m_label = f"{m_label}亚种({em_id}_{sub_id})"
            m_label_en = f"{m_label_en} variant ({em_id}_{sub_id})"
        prefix = f"em{em_id}" + (f"_{sub_id}" if sub_id and sub_id != "00" else "")
        act_m = re.search(r"(?:nActEm\d+(?:Common)?|uEm\d+(?:_\d+)?)::([A-Za-z0-9_]+)", blob)
        act_tag = act_m.group(1) if act_m and act_m.group(1) not in ("vft", "DTI", "ctor") else "动作/逻辑"
        act_tag_en = act_m.group(1) if act_m and act_m.group(1) not in ("vft", "DTI", "ctor") else "action/logic"
        if "aiem" in low or "think" in low:
            return (CAT_EM_AI, f"{prefix}.ai", f"{m_label} AI决策随机项 ({act_tag})", f"{m_label_en} AI decision roll ({act_tag_en})")
        return (CAT_EM_ACT, f"{prefix}.act", f"{m_label} 招式/机制随机项 ({act_tag})", f"{m_label_en} move/mechanic roll ({act_tag_en})")

    m_ems = re.search(r"(?:nActEms|uEms|ems|怪物动作AI\s*s)(\d{3})", blob)
    if m_ems:
        sid = m_ems.group(1)
        return (CAT_EM_ACT, f"ems{sid}.act", f"小型怪物(ems{sid})动作随机项", f"Small monster (ems{sid}) action roll")

    # 2. Shiny drops / Carves / Rewards / Items
    if "initshinydrop" in low or "droppop" in low or "spawnshinydrop" in low or "transmitshinydrop" in low:
        return (CAT_LOOT, "loot.shiny", "怪物掉落物/飞翔爪亮晶晶散落与抽签", "Monster shiny/clutch-claw drop scatter & roll")
    if "populatecarves" in low or "popholder" in low:
        return (CAT_LOOT, "loot.carve", "怪物剥取素材抽签", "Monster material carve roll")
    if "guireward" in low or "exreward" in low or "rem_item" in low:
        return (CAT_LOOT, "loot.reward", "任务结算报酬栏抽签 (uGUIReward)", "Quest reward box lottery (uGUIReward)")
    if "itemlottery" in low:
        return (CAT_LOOT, "loot.lottery", "道具概率表抽签", "Item probability table lottery")

    # 3. Monster generic AI / Action / Dung / Enemy
    if "feces" in low:
        return (CAT_EM_AI, "em.dung", "异臭弹/怪物换区逃跑抽签 (cEmFecesManager)", "Dung pod / monster flee lottery (cEmFecesManager)")
    if "charmmount" in low:
        return (CAT_OTOMO, "pl.mount", "骑乘/勾爪控制随机项", "Mount / clutch-claw control roll")
    if "thinkmgr" in low or "thinkem" in low or "ai_interpreter" in low or "uaiem" in low or "aisubsystem" in low or "mhai" in low:
        return (CAT_EM_AI, "ai.common", "怪物通用AI行为随机项", "Monster generic AI behavior roll")
    if "cemaction" in low or "uenemy" in low or "monster::" in low or "nemdttune" in low or "怪物" in blob:
        return (CAT_EM_ACT, "em.common", "大型怪物通用动作/状态随机项", "Large monster generic action/state roll")

    # 4. Quest / Investigation / Guiding Lands / QuestFSM
    if "tryaddtemperedinvestigation" in low or "investigation" in low:
        return (CAT_QUEST, "quest.invest", "调查任务生成/词条随机抽取", "Investigation quest gen/modifier roll")
    if "endcontentslot" in low or "guidinglands" in low or "glregion" in low or "expedition" in low:
        return (CAT_QUEST, "quest.gl", "聚魔之地/探索环境怪物抽签", "Guiding Lands / expedition monster roll")
    if "questfsm" in low or "randompos" in low or "任务流程" in blob:
        return (CAT_QUEST, "quest.pos", "任务流程随机坐标/翼龙落点抽签 (QuestFSM)", "Quest random spawn/wingdrake landing roll (QuestFSM)")
    if "squest" in low or "rquestdata" in low or "quest" in low:
        return (CAT_QUEST, "quest.sys", "任务系统随机参数", "Quest system random parameter")

    # 5. Player / Weapon / Recoil / Shell / Camera
    if "sightrecoil" in low:
        return (CAT_PLAYER, "pl.recoil", "弩枪后坐力与准星随机偏移 (cSightRecoilParam)", "Bowgun recoil & reticle spread (cSightRecoilParam)")
    if "rshellparam" in low or "ushell" in low:
        return (CAT_PLAYER, "pl.shell", "弹丸/投射物随机轨迹散布", "Projectile/shell trajectory spread")
    if "uwp" in low or "uweapon" in low or "hmaction" in low or "splayer" in low or "uplayer" in low or "猎人" in blob or "武器" in blob:
        return (CAT_PLAYER, "pl.action", "猎人/武器动作随机细节层", "Hunter/weapon action detail roll")

    # 6. Otomo / Palico
    if "otomo" in low or "palico" in low or "随从" in blob:
        return (CAT_OTOMO, "otomo.ai", "随从猫/兽人族支援行动随机项", "Palico/Grimalkyne support action roll")

    # 7. Kitchen / Steam / Melding / UserData rolling seeds
    if "kitchen" in low or (0x140F99000 <= va <= 0x140F99FFF):
        return (CAT_HUB, "meal.sys", "猫饭系统随机种子与技能项", "Canteen system seed & skill roll")
    if "steam" in low:
        return (CAT_HUB, "steam.sys", "蒸汽机随机抽奖逻辑", "Steamworks lottery RNG logic")
    if (0x141104000 <= va <= 0x141106000) or (0x141096000 <= va <= 0x141099000):
        return (CAT_HUB, "meld.sys", "炼金婆婆/调查资源炼金抽签", "Elder Melder / resource melding lottery")
    if (0x14135D800 <= va <= 0x14135DC00) or (0x14136B200 <= va <= 0x14136B500):
        return (CAT_HUB, "hub.rolls", "存档持久化滚动种子推进 (炼金/猫饭/派遣)", "Persistent save rolling seed advance (meld/meal/safari)")

    # 8. Background: Endemic life
    if "animal" in low or "gmfish" in low or "raresetdata" in low or "rarepop" in low or "生物动作" in blob:
        m_an = re.search(r"(?:Animal|l)(\d{3})", blob)
        an_tag = f"animal.{m_an.group(1)}" if m_an else "animal.env"
        return (CAT_BG_ANIMAL, an_tag, "地图环境生物随机游走/刷新 (Endemic Life)", "Endemic life random wander/spawn (Endemic Life)")

    # 9. Background: Stage / Gimmick / NPC
    if "npc" in low or "village" in low or "gimmick" in low or "ugm" in low or "myhouse" in low:
        return (CAT_BG_STAGE, "stage.npc", "据点NPC动作/地图环境机关随机项", "Hub NPC action / stage gimmick roll")

    # 10. Background: UI / HUD
    if "gui" in low or "hud" in low or "工具/ui" in low:
        return (CAT_BG_UI, "ui.misc", "界面动画/提示随机效果 (GUI)", "UI animation/tooltip random effect (GUI)")

    # 11. Background: VFX / Snow / Weather / Audio / CRT / Middleware
    if kind in (8, 9, 10, 11) or rva >= 0x2400000:
        return (CAT_BG_VFX, "vfx.audio", "底层渲染特效/音效噪声/包络随机", "Low-level VFX/audio noise/envelope RNG")
    if kind in (21, 22) or "mheffect" in low or "efc" in low or "snow" in low or "weather" in low or "wwise" in low or "sound" in low or "特效" in blob:
        return (CAT_BG_VFX, "vfx.ptcl", "粒子特效/雪地脚印/环境音效随机", "Particle VFX / snow footprint / ambient audio RNG")

    if "snetwork" in low or "mhnetwork" in low or "session" in low or kind == 7 or "网络" in blob:
        return (CAT_BG_OTHER, "net.lobby", "联机集会区域列表随机洗牌", "Online Gathering Hub session list shuffle")

    return (CAT_BG_OTHER, "misc.fn", "其它内部随机点", "Other internal RNG call site")


def c_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main():
    db = sqlite3.connect(DB_PATH)
    cur = db.cursor()
    fn_info = {}
    named_fn_list = []
    for addr, end, fullname, mod in cur.execute("SELECT addr, end, fullname, module FROM functions ORDER BY addr"):
        fn_info[addr] = (end, fullname, mod or "")
        if not fullname.startswith("Global::FUN_") and (mod or "") != "Global":
            named_fn_list.append((addr, fullname, mod or ""))
    named_addrs = [x[0] for x in named_fn_list]

    def nearest_named_context(va):
        idx = bisect.bisect_left(named_addrs, va)
        best = ""
        best_dist = 0x5000
        for cand_i in (idx - 1, idx):
            if 0 <= cand_i < len(named_fn_list):
                dist = abs(named_fn_list[cand_i][0] - va)
                if dist < best_dist:
                    best_dist = dist
                    best = f"{named_fn_list[cand_i][1]} [{named_fn_list[cand_i][2]}]"
        return best

    scale_map = {}
    with open(SCALE_PATH, encoding="utf-8") as sf:
        for line in sf:
            sp = line.strip().split("\t")
            if len(sp) == 2:
                rva_k = int(sp[0], 16)
                tag = sp[1]
                if tag == "FLOAT":
                    scale_map[rva_k] = SCALE_FLOAT_100
                elif tag == "MOD100":
                    scale_map[rva_k] = SCALE_MOD_100
                elif tag == "INTMOD":
                    scale_map[rva_k] = SCALE_INT_MOD
                else:
                    scale_map[rva_k] = SCALE_RAW_U32

    raw_rows = []
    seen_rva = set()
    with open(TSV_PATH, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\r\n").split("\t")
            if len(parts) < 5:
                continue
            rva = int(parts[0], 16)
            if rva in seen_rva:
                continue
            seen_rva.add(rva)
            kind = int(parts[1])
            fentry = int(parts[2], 16)
            owner = parts[3]
            callees = parts[4]
            fend, dbfn, dbmod = fn_info.get(fentry, (fentry + 0x40, "", ""))
            nctx = nearest_named_context(0x140000000 + rva) if (owner == "no-func" or (owner.startswith("FUN_") and "[" not in owner and not callees)) else ""
            # Determine exact ScaleKind
            if rva in scale_map:
                sk = scale_map[rva]
            elif kind in (5, 8): # randUnitSphere / mt19937_envelope_range
                sk = SCALE_FLOAT_100
            else:
                sk = SCALE_RAW_U32
            raw_rows.append((rva, kind, sk, fentry, fend, owner, callees, dbfn, dbmod, nctx))

    raw_rows.sort(key=lambda x: x[0])

    classified = []
    base_counts = defaultdict(int)
    for rva, kind, sk, fentry, fend, owner, callees, dbfn, dbmod, nctx in raw_rows:
        cat, base_alias, cn_desc, en_desc = classify_auto(rva, kind, owner, callees, dbfn, dbmod, nctx)
        is_curated = (rva in CURATED_RVA)
        base_counts[base_alias] += 1
        tech = owner.split(" [")[0]
        if "[vft=" in owner:
            vft_part = owner.split("[vft=")[1].split("]")[0].split(";")[0]
            tech = f"{tech} ({vft_part})"
        elif "[caller=" in owner:
            cal_part = owner.split("[caller=")[1].split("]")[0].split(";")[0]
            tech = f"{tech} <- {cal_part}"
        classified.append([rva, cat, sk, base_alias, cn_desc, en_desc, tech, is_curated, fentry, fend])

    seq_tracker = defaultdict(int)
    fn_ranges = {}
    for item in classified:
        rva, cat, sk, base_alias, cn_desc, en_desc, tech, is_curated, fentry, fend = item
        if not is_curated and base_counts[base_alias] > 1:
            seq_tracker[base_alias] += 1
            item[3] = f"{base_alias}.{seq_tracker[base_alias]}"
        if fentry >= 0x140001000 and fend > fentry:
            frva_lo = fentry - 0x140000000
            frva_hi = fend - 0x140000000
            if frva_lo not in fn_ranges:
                fn_ranges[frva_lo] = (frva_lo, frva_hi, cat, sk, base_alias, cn_desc, en_desc, tech)

    # Fast single-word English shortcuts (no IME switching needed in MHW chat!)
    shortcuts = [
        # Core Monster Stats & AI
        ("size",      "em.size",         "怪物体型大小金倍率抽取",             "Monster crown/size multiplier roll"),
        ("crown",     "em.size",         "怪物体型大小金倍率抽取",             "Monster crown/size multiplier roll"),
        ("dying",     "em.dying",        "怪物濒死血线浮动",                   "Monster dying/limp HP threshold jitter"),
        ("hp",        "em.init_hp",      "怪物部位与初始生命参数",             "Monster part & initial HP variance"),
        ("think",     "ai.think",        "怪物AI思考树(.thk)出招分支",         "Monster AI think-tree (.thk) branch"),
        ("atk",       "ai.think",        "怪物AI思考树(.thk)出招分支",         "Monster AI think-tree (.thk) branch"),
        ("target",    "ai.target",       "怪物随机点名猎人或随从猫仇恨",       "Monster random player/Palico aggro"),
        ("aggro",     "ai.target",       "怪物随机点名猎人或随从猫仇恨",       "Monster random player/Palico aggro"),
        ("dung",      "em.dung*",        "异臭弹/怪物换区逃跑抽签",            "Dung pod / monster flee lottery"),
        # Quest & Investigation
        ("quest",     "quest.accept",    "接任务开局随机状态",                 "Quest accept initial RNG state"),
        ("bonus",     "quest.bonus.*",   "任务天气/猫车/开局增益抽签",         "Quest weather/cart/bonus rolls"),
        ("weather",   "quest.bonus.*",   "任务天气/猫车/开局增益抽签",         "Quest weather/cart/bonus rolls"),
        ("invest",    "quest.invest*",   "调查任务词条随机抽取",               "Investigation modifier rolls"),
        ("tempered",  "quest.tempered",  "历战调查任务派生抽取",               "Tempered investigation derivation roll"),
        ("gl",        "quest.gl*",       "聚魔之地/探索怪物抽签",              "Guiding Lands / expedition monster roll"),
        # Loot / Drops / Rewards
        ("carve",     "loot.carve.*",    "怪物剥取素材抽签(全部4处)",          "Monster carve rolls (all 4)"),
        ("shiny",     "loot.shiny.*",    "怪物掉落物/亮晶晶抽签(全部)",        "Monster shiny drops (all)"),
        ("drop",      "loot.shiny.*",    "怪物掉落物/亮晶晶抽签(全部)",        "Monster shiny drops (all)"),
        ("reward",    "loot.reward.*",   "任务结算报酬栏抽签(全部)",           "Quest reward box rolls (all)"),
        ("item",      "loot.item",       "通用道具概率表抽签",                 "Generic item lottery table roll"),
        ("track",     "loot.track",      "摸脚印/痕迹掉落拾取抽签",            "Track/footprint pickup drop roll"),
        # Hub / Kitchen / Melding / Steam / Awakening
        ("meal",      "meal.trigger",    "猫饭技能是否发动概率判定",           "Canteen food skill activation roll"),
        ("food",      "meal.trigger",    "猫饭技能是否发动概率判定",           "Canteen food skill activation roll"),
        ("daily",     "meal.daily",      "猫饭每日随机技能抽取",               "Canteen daily random skills roll"),
        ("steam",     "steam.*",         "蒸汽机抽奖(全部)",                   "Steamworks RNG sites (all)"),
        ("meld",      "meld.*",          "炼金婆婆珠子/护石抽签",              "Elder Melder jewel/charm rolls"),
        ("deco",      "meld.deco",       "炼金婆婆装饰品珠子抽签",             "Elder Melder decoration jewel lottery"),
        ("awaken",    "awakening.lot.*", "冥赤龙觉醒武器词条洗练(全部)",       "Safi awakened weapon ability rolls (all)"),
        # Player / Ammo / Mount / Palico
        ("shell",     "pl.shell*",       "投射物/弹丸随机散布",                "Projectile/shell spread rolls"),
        ("ammo",      "pl.shell*",       "投射物/弹丸随机散布",                "Projectile/shell spread rolls"),
        ("recoil",    "pl.recoil*",      "弩枪后坐力与准星偏移",               "Bowgun recoil & reticle spread"),
        ("mount",     "pl.mount*",       "骑乘/勾爪点位随机控制",              "Mount / clutch-claw position RNG"),
        ("palico",    "otomo.*",         "随从猫/兽人族支援随机项",            "Palico/Grimalkyne support rolls"),
        ("otomo",     "otomo.*",         "随从猫/兽人族支援随机项",            "Palico/Grimalkyne support rolls"),
        # Popular Monster English Names
        ("fatalis",   "em013*",          "黑龙(Fatalis/em013)全部专属随机项",   "Fatalis (em013) all RNG sites"),
        ("alatreon",  "em050*",          "煌黑龙(Alatreon/em050)全部专属随机项", "Alatreon (em050) all RNG sites"),
        ("velkhana",  "em124*",          "冰咒龙(Velkhana/em124)全部专属随机项", "Velkhana (em124) all RNG sites"),
        ("nergi",     "em103*",          "灭尽龙(Nergigante/em103)全部专属随机项", "Nergigante (em103) all RNG sites"),
        ("rajang",    "em023*",          "金狮子(Rajang/em023)全部专属随机项",   "Rajang (em023) all RNG sites"),
        ("safi",      "em104*",          "冥赤龙(Safijiiva/em104)全部专属随机项", "Safi'jiiva (em104) all RNG sites"),
        ("kulve",     "em117*",          "绚辉龙(KulveTaroth/em117)全部专属随机项", "Kulve Taroth (em117) all RNG sites"),
        ("teostra",   "em027*",          "炎王龙(Teostra/em027)全部专属随机项",  "Teostra (em027) all RNG sites"),
        ("lunastra",  "em026*",          "炎妃龙(Lunastra/em026)全部专属随机项", "Lunastra (em026) all RNG sites"),
        ("kushala",   "em024*",          "钢龙(Kushala/em024)全部专属随机项",    "Kushala Daora (em024) all RNG sites"),
        ("kirin",     "em011*",          "麒麟(Kirin/em011)全部专属随机项",      "Kirin (em011) all RNG sites"),
        ("namielle",  "em125*",          "溟波龙(Namielle/em125)全部专属随机项", "Namielle (em125) all RNG sites"),
        ("shara",     "em126*",          "天地煌啼龙(Shara/em126)全部专属随机项", "Shara Ishvalda (em126) all RNG sites"),
        ("zinogre",   "em057*",          "雷狼龙(Zinogre/em057)全部专属随机项",  "Zinogre (em057) all RNG sites"),
        ("tigrex",    "em032*",          "轰龙(Tigrex/em032)全部专属随机项",     "Tigrex (em032) all RNG sites"),
        ("narga",     "em037*",          "迅龙(Nargacuga/em037)全部专属随机项",  "Nargacuga (em037) all RNG sites"),
        ("brachy",    "em063*",          "碎龙(Brachydios/em063)全部专属随机项", "Brachydios (em063) all RNG sites"),
        ("glavenus",  "em080*",          "斩龙(Glavenus/em080)全部专属随机项",   "Glavenus (em080) all RNG sites"),
        ("rathalos",  "em002*",          "火龙(Rathalos/em002)全部专属随机项",   "Rathalos (em002) all RNG sites"),
        ("rathian",   "em001*",          "雌火龙(Rathian/em001)全部专属随机项",  "Rathian (em001) all RNG sites"),
        ("diablos",   "em007*",          "角龙(Diablos/em007)全部专属随机项",    "Diablos (em007) all RNG sites"),
        ("jho",       "em043*",          "恐暴龙(Deviljho/em043)全部专属随机项", "Deviljho (em043) all RNG sites"),
        ("bazel",     "em118*",          "爆鳞龙(Bazelgeuse/em118)全部专属随机项", "Bazelgeuse (em118) all RNG sites"),
        ("vaal",      "em115*",          "尸套龙(VaalHazak/em115)全部专属随机项", "Vaal Hazak (em115) all RNG sites"),
        ("odo",       "em113*",          "惨爪龙(Odogaron/em113)全部专属随机项",  "Odogaron (em113) all RNG sites"),
        ("barioth",   "em042*",          "冰牙龙(Barioth/em042)全部专属随机项",   "Barioth (em042) all RNG sites"),
        ("garuga",    "em018*",          "黑狼鸟(Garuga/em018)全部专属随机项",    "Yian Garuga (em018) all RNG sites"),
        ("behemoth",  "em121*",          "贝希摩斯(Behemoth/em121)全部专属随机项", "Behemoth (em121) all RNG sites"),
    ]

    sorted_fn_ranges = sorted(fn_ranges.values(), key=lambda x: x[0])

    with open(OUT_INL, "w", encoding="utf-8") as out:
        out.write("// Auto-generated by tools/gen_rng_catalog.py — DO NOT EDIT MANUALLY\n")
        out.write(f"// Total indexed static RNG call sites : {len(classified)}\n")
        out.write(f"// Total indexed enclosing RNG funcs   : {len(sorted_fn_ranges)}\n\n")

        out.write("static constexpr CatalogSiteEntry kCatalogSites[] = {\n")
        for rva, cat, sk, alias, cn_desc, en_desc, tech, _, _, _ in classified:
            out.write(f'    {{ 0x{rva:07X}U, {cat}U, {sk}U, "{c_escape(alias)}", u8"{c_escape(cn_desc)}", "{c_escape(en_desc)}", "{c_escape(tech)}" }},\n')
        out.write("};\n\n")

        out.write("static constexpr CatalogFuncRange kCatalogFuncRanges[] = {\n")
        for flo, fhi, cat, sk, base_alias, cn_desc, en_desc, tech in sorted_fn_ranges:
            out.write(f'    {{ 0x{flo:07X}U, 0x{fhi:07X}U, {cat}U, {sk}U, "{c_escape(base_alias)}", u8"{c_escape(cn_desc)}", "{c_escape(en_desc)}", "{c_escape(tech)}" }},\n')
        out.write("};\n\n")

        out.write("static constexpr ShortcutAliasEntry kBuiltinShortcuts[] = {\n")
        for key, target_pat, desc, en_desc in shortcuts:
            out.write(f'    {{ u8"{c_escape(key)}", "{c_escape(target_pat)}", u8"{c_escape(desc)}", "{c_escape(en_desc)}" }},\n')
        out.write("};\n")

    cat_names = {
        0: "核心:怪物数值", 1: "核心:怪物AI", 2: "核心:怪物招式", 3: "核心:任务调查",
        4: "核心:猎人弹道", 5: "核心:随从骑乘", 6: "核心:剥取报酬", 7: "核心:猫饭炼金觉醒",
        8: "背景:环境生物", 9: "背景:据点机关", 10: "背景:UI界面", 11: "背景:特效音效", 12: "背景:其它底层"
    }
    by_cat = defaultdict(int)
    for item in classified:
        by_cat[item[1]] += 1
    print(f"Generated {OUT_INL}: {len(classified)} sites, {len(sorted_fn_ranges)} func ranges")
    for cid in sorted(cat_names):
        print(f"  [{cid:2d}] {cat_names[cid]:16s}: {by_cat[cid]:4d} sites")


if __name__ == "__main__":
    main()
