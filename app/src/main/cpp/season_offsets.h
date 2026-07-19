/* JCC season fields — dump.cs + scan + 用户确认：自动买=阵容内存匹配 */
#pragma once

#define JCC_SEASON_TAG "full-1.0.0"
#define JCC_SEASON_SCAN_DATE "2026-07-19"
/* branch jcc-full-kernel: dump-backed F1-F10 */

/* TACG_Hero_Client (MATCH original SO 0x7e4bc) */
#define JCC_HERO_IID 0x10
#define JCC_HERO_SNAME 0x18
#define JCC_HERO_ICOST 0x60
#define JCC_HERO_PAINT_SMALL 0xf8
#define JCC_HERO_SETNUM 0x114
#define JCC_HERO_ISTAR 0x34
#define JCC_HERO_IQUALITY 0x38

/* PlayerModel (scan + dump MATCH) */
#define JCC_PM_BATTLE_TURN 0x20
#define JCC_PM_HEX_AUGMENT 0x28
#define JCC_PM_RANK 0x4c              /* Rank 0-based；预警用 rank+1 */
#define JCC_PM_MONEY 0x5c
#define JCC_PM_LAST_ENEMY 0x64
#define JCC_PM_RANK_DATA 0x78         /* TAC_UserMatchData* → strName@0x20 */
#define JCC_PM_HP 0xbc
#define JCC_PM_HP_MAX 0xd0
#define JCC_PM_MAX_HERO_NUM 0xf0      /* 人口/等级 Lv */
#define JCC_PM_BUY_HERO_DICT 0x108
#define JCC_PM_HA_STORE_S6 0x128
#define JCC_PM_ACTIVE_HA_IDS 0x148
#define JCC_PM_HA_CONFIG_IDS 0x150
#define JCC_PM_ENEMY_PLAYER 0x1c0
#define JCC_PM_PLAYER_ID 0x270
#define JCC_PM_UNIT_DICT 0x278
#define JCC_PM_WAIT_UNITS 0x388
#define JCC_PM_BATTLE_UNITS 0x390

/* TAC_UserMatchData */
#define JCC_UMD_NAME 0x20             /* strName */

/* UnitData (scan MATCH — board without LoadBody) */
#define JCC_UD_HERO_ID 0x14
#define JCC_UD_PLAYER_ID 0x24
#define JCC_UD_COL 0x30
#define JCC_UD_ROW 0x34
#define JCC_UD_TAB 0xe8
#define JCC_UD_HERO_NAME 0x120
#define JCC_UD_LEVEL 0x148
#define JCC_UD_QUALITY 0x150

/* BuyHeroView shop (SO 0x7d7a4 MATCH) */
#define JCC_BH_LIST_HERO 0x148
#define JCC_BH_CUR_PM 0x168
#define JCC_HR_INDEX 0x160
#define JCC_HR_TAC_BUY 0x1a8 /* TAC_BuyHero* */
#define JCC_HR_DATA_ID 0x1b0

/* TAC_BuyHero / TAC_HeroEntity — 商店槽 conf id */
#define JCC_TB_HERO_ENTITY 0x10
#define JCC_HE_ENTITY_ID 0x10
#define JCC_HE_HERO_CONF 0x14

/* 金铲铲自带阵容 TeamRecommend（用户确认：自动买读这个） */
#define JCC_TRC_MODEL 0x88                 /* TeamRecommendController._teamRecommendModel */
#define JCC_TRM_CUR_RECOMMEND 0x40         /* CurrentRecommendData */
#define JCC_TRM_CUR_STAGE_DATA 0x50        /* CurrentStageRecommendData */
#define JCC_SRTD_HERO_EQUIPS 0x10          /* HeroAndEquipments dict */
#define JCC_SRTD_CORE_HERO 0x50           /* CoreHeroId */

/* ChessBattleGlobal → BuyHeroView（无 FindObjectOfType） */
#define JCC_CBG_SCREEN_MGR 0x108          /* battleScreenMgr */
#define JCC_BSM_SCREEN 0x10               /* BattleScreenMgr.battleScreen */

/* ChessBattleModel */
#define JCC_CBM_PLAYER_DICT 0x38
#define JCC_CBM_CUR_TURN 0x130
#define JCC_CBM_CHAIR_LIST 0x170
#define JCC_CBM_PLAYBOOK_ID 0x1e8
#define JCC_CBM_MY_MATCH_LIST 0x268

/* TACG_HABasicConfig_Client */
#define JCC_HA_IID 0x10
#define JCC_HA_SNAME 0x18
#define JCC_HA_ILEVEL 0x28

/* PlayerListPanel / PlayerListItem */
#define JCC_PLP_ITEM_LIST 0x120
#define JCC_PLI_HEAD_GO 0x60
#define JCC_PLI_ITEM_ID 0x194
#define JCC_PLI_INDEX 0x1a0
#define JCC_PLI_PLAYER_MODEL 0x1b8

/* IL2CPP List / Array */
#define JCC_LIST_ITEMS 0x10
#define JCC_LIST_SIZE 0x18
#define JCC_ARR_FIRST 0x20

#define JCC_NS_HERO "ZGameClient"
#define JCC_CLS_HERO "TACG_Hero_Client"
#define JCC_NS_DB "ZGame"
#define JCC_CLS_DB "DataBaseManager"
