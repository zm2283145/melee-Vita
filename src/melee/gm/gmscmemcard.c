#include "gmscmemcard.h"

#include "gm_unsplit.h"
#include "gmmain_lib.h"
#include <melee/db/db.h>
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lbcardgame.h>
#include <melee/lb/lbcardnew.h>
#include <melee/lb/lblanguage.h>
#include <melee/mn/inlines.h>
#ifdef TARGET_VITA
#include <melee_save_compat.h>
#include <melee/ty/toy.h>
#include <string.h>
#endif
#include <sysdolphin/baselib/controller.h>

typedef struct {
    u32 unk0;
    u8 mode_id;
} exitData;

struct enterData_x0_t {
    int unk0; ///< ::LbLanguage?
    u8 chan;  ///< memcard channel
    u8 mode_id;
};

typedef enum {
    tickDecision_0,
    tickDecision_1,
    tickDecision_2,
    tickDecision_3,
    tickDecision_4,
    tickDecision_5,
    tickDecision_6,
    tickDecision_7,
    tickDecision_8,
    tickDecision_9,
    tickDecision_10,
    tickDecision_11,
    tickDecision_12,
    tickDecision_13,
    tickDecision_14,
    tickDecision_15,
    tickDecision_16,
    tickDecision_17,
    tickDecision_18,
    tickDecision_19,
    tickDecision_20,
} tickDecision;

typedef struct {
    struct enterData_x0_t unk0;
    exitData unk8;
    int unk10;
    tickDecision decision;
    int unk18;
    u8 unk1C;
} enterData;

#ifdef TARGET_VITA
static void gmVita_CountRecordOrder(MeleeVitaSaveByteOrder order,
                                    int* big_endian, int* native)
{
    if (order == MELEE_VITA_SAVE_BIG_ENDIAN) {
        ++*big_endian;
    } else if (order == MELEE_VITA_SAVE_NATIVE) {
        ++*native;
    }
}

static void gmVita_NormalizeSignedRecord(s32* value)
{
    u32 bits = (u32) *value;
    if (melee_vita_normalize_record_u32(
            &bits, MELEE_VITA_STADIUM_RECORD_MAX))
    {
        *value = (s32) bits;
    }
}

static void gmVita_NormalizeFighterMask(s32* value)
{
    u32 bits = (u32) *value;
    if (melee_vita_normalize_fighter_mask(&bits)) {
        *value = (s32) bits;
    }
}

static void gmVita_NormalizeStadiumRecords(GmSaveData* save)
{
    int big_endian = 0;
    int native = 0;
    int i;

    /* Earlier Vita versions repaired unlocks and trophies, but left the
     * fighter records from imported GameCube saves in PowerPC byte order.
     * Require several independent records before modifying this domain. */
    for (i = 0; i < SELKIND_COUNT; i++) {
        struct FighterData* fighter = &save->x1F2C[i];
        /* Home-Run distances may be the only records left in GameCube order
         * after an earlier Vita version saved its other repaired records. */
        gmVita_NormalizeSignedRecord(&fighter->x7C.x84);
        gmVita_CountRecordOrder(melee_vita_classify_record_u16(
            fighter->x7C.x7E, MELEE_VITA_STADIUM_COMBO_MAX),
            &big_endian, &native);
        gmVita_CountRecordOrder(melee_vita_classify_record_u32(
            fighter->x7C.x94, MELEE_VITA_STADIUM_RECORD_MAX),
            &big_endian, &native);
        gmVita_CountRecordOrder(melee_vita_classify_record_u32(
            (u32) fighter->x7C.x98, MELEE_VITA_STADIUM_RECORD_MAX),
            &big_endian, &native);
        gmVita_CountRecordOrder(melee_vita_classify_record_u32(
            (u32) fighter->x7C.x9C, MELEE_VITA_STADIUM_RECORD_MAX),
            &big_endian, &native);
        gmVita_CountRecordOrder(melee_vita_classify_record_u32(
            (u32) save->unk_30.xB0[i], MELEE_VITA_STADIUM_RECORD_MAX),
            &big_endian, &native);
        gmVita_CountRecordOrder(melee_vita_classify_record_u32(
            (u32) save->unk_30.x114[i], MELEE_VITA_STADIUM_RECORD_MAX),
            &big_endian, &native);
    }
    if (!melee_vita_has_imported_record_evidence(big_endian, native)) {
        return;
    }

    for (i = 0; i < SELKIND_COUNT; i++) {
        struct FighterData* fighter = &save->x1F2C[i];
        int fighter_big_endian = 0;
        int fighter_native = 0;
        u16 flags;

        gmVita_CountRecordOrder(melee_vita_classify_record_u16(
            fighter->x7C.x7E, MELEE_VITA_STADIUM_COMBO_MAX),
            &fighter_big_endian, &fighter_native);
        gmVita_CountRecordOrder(melee_vita_classify_record_u32(
            fighter->x7C.x94, MELEE_VITA_STADIUM_RECORD_MAX),
            &fighter_big_endian, &fighter_native);
        gmVita_CountRecordOrder(melee_vita_classify_record_u32(
            (u32) fighter->x7C.x98, MELEE_VITA_STADIUM_RECORD_MAX),
            &fighter_big_endian, &fighter_native);
        gmVita_CountRecordOrder(melee_vita_classify_record_u32(
            (u32) fighter->x7C.x9C, MELEE_VITA_STADIUM_RECORD_MAX),
            &fighter_big_endian, &fighter_native);
        gmVita_CountRecordOrder(melee_vita_classify_record_u32(
            (u32) save->unk_30.xB0[i], MELEE_VITA_STADIUM_RECORD_MAX),
            &fighter_big_endian, &fighter_native);
        gmVita_CountRecordOrder(melee_vita_classify_record_u32(
            (u32) save->unk_30.x114[i], MELEE_VITA_STADIUM_RECORD_MAX),
            &fighter_big_endian, &fighter_native);

        if (melee_vita_classify_record_u16(
                fighter->x7C.x7E, MELEE_VITA_STADIUM_COMBO_MAX) ==
            MELEE_VITA_SAVE_BIG_ENDIAN)
        {
            fighter->x7C.x7E = melee_vita_swap_u16(fighter->x7C.x7E);
        }
        melee_vita_normalize_record_u32(
            &fighter->x7C.x94, MELEE_VITA_STADIUM_RECORD_MAX);
        gmVita_NormalizeSignedRecord(&fighter->x7C.x98);
        gmVita_NormalizeSignedRecord(&fighter->x7C.x9C);
        gmVita_NormalizeSignedRecord(&fighter->x7C.xA4);
        gmVita_NormalizeSignedRecord(&fighter->x7C.xA8);
        gmVita_NormalizeSignedRecord(&save->unk_30.xB0[i]);
        gmVita_NormalizeSignedRecord(&save->unk_30.x114[i]);

        /* The completion bits and small KO records have no reliable range
         * test by themselves. Convert them only when this fighter has at
         * least two other imported records and no native-order evidence. */
        if (fighter_big_endian >= 2 && fighter_native == 0) {
            memcpy(&flags, &fighter->x7C, sizeof(flags));
            flags = melee_vita_convert_fighter_record_flags(flags);
            memcpy(&fighter->x7C, &flags, sizeof(flags));
            fighter->x7C.xA0 = melee_vita_swap_u16(fighter->x7C.xA0);
            fighter->x7C.xA2 = melee_vita_swap_u16(fighter->x7C.xA2);
        }
    }

    gmVita_NormalizeFighterMask(&save->unk_8.x14);
    gmVita_NormalizeFighterMask(&save->unk_8.x18);
    gmVita_NormalizeFighterMask(&save->unk_8.x1C);
    gmVita_NormalizeFighterMask(&save->unk_28.x4);
    gmVita_NormalizeFighterMask(&save->unk_30.x8);
}

static void gmVita_NormalizeModeScores(GmSaveData* save)
{
    u32 scores[SELKIND_COUNT][3];
    int i;

    for (i = 0; i < SELKIND_COUNT; i++) {
        scores[i][0] = (u32) save->x1F2C[i].x7C.x88;
        scores[i][1] = (u32) save->x1F2C[i].x7C.x8C;
        scores[i][2] = (u32) save->x1F2C[i].x7C.x90;
    }
    if (!melee_vita_normalize_mode_scores(scores, SELKIND_COUNT)) return;
    for (i = 0; i < SELKIND_COUNT; i++) {
        save->x1F2C[i].x7C.x88 = (s32) scores[i][0];
        save->x1F2C[i].x7C.x8C = (s32) scores[i][1];
        save->x1F2C[i].x7C.x90 = (s32) scores[i][2];
    }
}
#endif

/* 1AEE6C */ static void gm_801AEE6C(int, int, int);
/* 1AF0D4 */ static bool gm_801AF0D4(void);
/* 1AF250 */ static void gm_801AF250(void);

static u8 gm_804D6870;
static u16 gm_804D6872;

static int gm_803DD550_jp[] = {
    1, 1, 1, 1, 2, 2, 3, 1, 3, 2, 4, 1, 4, 4, 5, 1, 2, 4, 2, 3, 2, 2, 2,
};
static int gm_803DD550_us[] = {
    1, 1, 1, 1, 3, 3, 3, 3, 3, 3, 5, 2, 5, 2, 3, 2, 2, 4, 2, 3, 2, 1, 2,
};

static enterData enter_data;

void gm_801AEE6C(int arg0, int arg1, int arg2)
{
    float scale = 1.12F;
    switch (arg0) {
    case 0:
        gm_801AE848(0);
        gm_801ADE1C(0, arg1, 0.0F, 0.0F);
        gm_801AE44C(0, scale * (arg2 - 2));
        gm_801AE544(0, -2.0F);
        enter_data.unk10 = 0;
        return;
    case 1:
        gm_801AE848(0);
        gm_801ADE1C(0, arg1, 0.0F, 0.0F);
        gm_801AE44C(0, scale * (arg2 - 2));
        gm_801AE050(0, 0, 3, -3.5F, -scale * (arg2 - 2));
        gm_801AE050(0, 1, 4, 3.5F, -scale * (arg2 - 2));
        enter_data.unk1C = 0U;
        gm_801AE640(0, enter_data.unk1C);
        gm_801AE74C(0, !enter_data.unk1C);
        enter_data.unk10 = 1;
        return;
    case 2:
        gm_801AE848(0);
        gm_801ADE1C(0, arg1, 0.0F, 0.0F);
        gm_801AE44C(0, scale * (arg2 - 2));
        gm_801AE050(0, 0, 2, -3.5F, -scale * (arg2 - 2));
        gm_801AE050(0, 1, 5, 3.5F, -scale * (arg2 - 2));
        enter_data.unk1C = 0U;
        gm_801AE640(0, enter_data.unk1C);
        gm_801AE74C(0, !enter_data.unk1C);
        enter_data.unk10 = 1;
        return;
    }
}

static inline bool gm_801AF0D4_inline(void)
{
    if (lbCardNew_ProbeEx(enter_data.unk0.chan)) {
        if (enter_data.unk18 == 1) {
            enter_data.unk18 = 0;
            enter_data.decision = 1;
            return true;
        }
    } else if (enter_data.unk18 == 0) {
        enter_data.unk18 = 1;
        enter_data.decision = 1;
        return true;
    }
    return false;
}

bool gm_801AF0D4(void)
{
    int saved_unk1C = enter_data.unk1C;

    if (gm_801AF0D4_inline()) {
        return true;
    }

    if (gm_801AEDC8() & 0x40001 ? 1 : 0) {
        if (enter_data.unk1C != 0) {
            if (enter_data.unk10 != 0) {
                sfxMove();
            }
            enter_data.unk1C = 0;
        }
    } else if ((gm_801AEDC8() & 0x80002 ? 1 : 0)) {
        if (enter_data.unk1C < 1) {
            if (enter_data.unk10 != 0) {
                sfxMove();
            }
            enter_data.unk1C = 1;
        }
    }
    if (saved_unk1C != enter_data.unk1C) {
        gm_801AE640(0, enter_data.unk1C);
        gm_801AE74C(0, !enter_data.unk1C);
    }
    return false;
}

static inline u8 set_gm_804D6870_inline(void)
{
    if ((HSD_PadCopyStatus->button & HSD_PAD_L) &&
        (HSD_PadCopyStatus->button & HSD_PAD_R) &&
        (HSD_PadCopyStatus->button & HSD_PAD_A))
    {
        gm_804D6870 = 1;
    }
    return gm_804D6870;
}

static inline bool gm_801AEDC8_flag_check(void)
{
    if (gm_801AEDC8() & (HSD_PAD_START | HSD_PAD_A)) {
        sfxForward();
        return true;
    }
    return false;
}

static inline int get_lang_val(int idx)
{
    int i;
    if (lbLang_IsSavedLanguageUS()) {
        i = idx - 2;
        return gm_803DD550_us[i];
    } else {
        i = idx - 2;
        return gm_803DD550_jp[i];
    }
}

static inline void unk_inline(void)
{
    if (enter_data.unk0.unk0 == 1) {
        gm_801AEE6C(2, 23, get_lang_val(23));
        enter_data.decision = 18;
    } else {
        gm_801AEE6C(2, 24, get_lang_val(24));
        enter_data.decision = 19;
    }
}

void gm_801AF250(void)
{
    u32 temp_r3 = lb_8001C87C();
    enter_data.unk18 = 1;
    switch (temp_r3) {
    case 1:
    case 2:
        if (enter_data.unk0.unk0 == 0) {
            gm_801AEE6C(1, 7, get_lang_val(6));
        } else {
            gm_801AEE6C(1, 6, get_lang_val(6));
            enter_data.unk1C = 1;
            gm_801AE640(0, enter_data.unk1C);
            gm_801AE74C(0, !enter_data.unk1C);
        }
        enter_data.decision = 2;
        return;
    case 3:
        gm_801AEE6C(1, 8, get_lang_val(8));
        enter_data.decision = 3;
        enter_data.unk1C = 1;
        gm_801AE640(0, enter_data.unk1C);
        gm_801AE74C(0, !enter_data.unk1C);
        return;
    case 4:
        gm_801AEE6C(1, 0xA, get_lang_val(0xA));
        enter_data.decision = 5;
        return;
    case 5:
        gm_801AEE6C(0, 0xE, get_lang_val(0xE));
        enter_data.decision = 9;
        return;
    case 6:
        gm_801AEE6C(0, 0xF, get_lang_val(0xF));
        enter_data.decision = 0xA;
        return;
    case 9:
        gm_801AEE6C(1, 0x10, get_lang_val(0x10));
        enter_data.decision = 0xB;
        enter_data.unk1C = 1U;
        gm_801AE640(0, enter_data.unk1C);
        gm_801AE74C(0, !enter_data.unk1C);
        return;
    case 10:
    case 11:
    case 13:
        gm_801AEE6C(0, 0x13, get_lang_val(0x13));
        enter_data.decision = 0xE;
        return;
    case 12:
        gm_801AEE6C(0, 0x14, get_lang_val(0x14));
        enter_data.decision = 0xF;
        return;
    case 14:
        gm_801AEE6C(0, 0x15, get_lang_val(0x15));
        enter_data.decision = 0x10;
        return;
    case 15:
        gm_801AEE6C(0, 0x16, get_lang_val(0x16));
        enter_data.unk18 = 0;
        enter_data.decision = 0x11;
        return;
    case 0:
    case 7:
    case 8:
    default:
        gm_801A4B60();
    }
}

void gm_Scene_MemCard_OnFrame(void)
{
    int temp_r29;
    u8 _[0x14];

    if (DbLevel >= DbLKind_DebugRom && set_gm_804D6870_inline() != 0) {
        if (HSD_PadCopyStatus->trigger & HSD_PAD_L) {
            if (gm_804D6872 > 6) {
                gm_804D6872 -= 1;
                gm_801AEE6C(0, gm_804D6872, get_lang_val(gm_804D6872));
            }
        } else if ((HSD_PadCopyStatus->trigger & HSD_PAD_R)) {
            if (gm_804D6872 < 0x18) {
                gm_804D6872 += 1;
                gm_801AEE6C(0, gm_804D6872, get_lang_val(gm_804D6872));
            }
        }
        if ((HSD_PadCopyStatus->button & HSD_PAD_L) &&
            (HSD_PadCopyStatus->button & HSD_PAD_R) &&
            (HSD_PadCopyStatus->button & HSD_PAD_B))
        {
            gm_801A4B60();
        }
        return;
    }

    switch (enter_data.decision) {
    case 0:
        temp_r29 = lb_8001CBBC();
#ifdef TARGET_VITA
        if ((temp_r29 == 0 || temp_r29 == 2) && lb_8001B6E0(1) == 0) {
            GmSaveData* save = gmMainLib_GetSaveData();
            MeleeVitaProgressFields progress = {
                save->unlocked_characters,
                save->x186A,
                (u64) save->x1A68,
            };
            melee_vita_normalize_progress_fields(&progress);
            save->unlocked_characters = progress.unlocked_characters;
            save->x186A = progress.unlocked_stages;
            save->x1A68 = (s64) progress.completed_events;
            gmVita_NormalizeStadiumRecords(save);
            gmVita_NormalizeModeScores(save);
            Toy_NormalizeImportedSaveData();
        }
#endif
        gmMainLib_8015FA34(temp_r29);
        if (temp_r29 == 0 || temp_r29 == 2) {
            enter_data.unk8.unk0 = 1;
            enter_data.decision = 0x14;
        } else {
            enter_data.decision = 1;
        }
        break;
    case 1:
        gm_801AF250();
        break;
    case 2:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            if (enter_data.unk1C == 0) {
                if (enter_data.unk0.unk0 == 0) {
                    enter_data.decision = 0;
                    lbCardGame_SetCardStatus(LbCardStatus_0);
                } else {
                    enter_data.unk8.unk0 = 1;
                    enter_data.decision = 20;
                    lbCardGame_SetCardStatus(LbCardStatus_0);
                    lbCardGame_SaveChanges();
                }
            } else {
                unk_inline();
            }
        }
        break;
    case 3:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            if (enter_data.unk1C == 0) {
                gm_801AEE6C(0, 9, get_lang_val(9));
                enter_data.decision = 4;
            } else {
                unk_inline();
            }
        }
        break;
    case 4:
        if (!gm_801AF0D4()) {
            if (!lb_8001CC4C()) {
                gm_801AEE6C(0, 0xB, get_lang_val(0xB));
                enter_data.decision = 6;
            } else {
                gm_801AEE6C(0, 0xD, get_lang_val(0xD));
                enter_data.decision = 8;
            }
        }
        break;
    case 5:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            if (enter_data.unk1C == 0) {
                gm_801AEE6C(0, 0xB, get_lang_val(0xB));
                enter_data.decision = 6;
            } else {
                unk_inline();
            }
        }
        break;
    case 6:
        if (!gm_801AF0D4()) {
            if (!lb_8001C8BC()) {
                gm_801AEE6C(0, 0xC, get_lang_val(0xC));
                enter_data.decision = 7;
            } else {
                gm_801AEE6C(0, 0xD, get_lang_val(0xD));
                enter_data.decision = 8;
            }
        }
        break;
    case 7:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            enter_data.unk8.unk0 = 1;
            enter_data.decision = 0x14;
            lbCardGame_SetCardStatus(0);
        }
        break;
    case 8:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            unk_inline();
        }
        break;
    case 9:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            unk_inline();
        }
        break;
    case 10:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            unk_inline();
        }
        break;
    case 11:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            if (enter_data.unk1C == 0) {
                if (lb_8001B8C8(enter_data.unk0.chan) == 0) {
                    gm_801AEE6C(0, 0x11, get_lang_val(0x11));
                    enter_data.decision = 0xC;
                } else {
                    gm_801AEE6C(0, 0x12, get_lang_val(0x12));
                    enter_data.decision = 0xD;
                }
            } else {
                unk_inline();
            }
        }
        break;
    case 12:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            enter_data.decision = 1;
        }
        break;
    case 13:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            unk_inline();
        }
        break;
    case 14:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            unk_inline();
        }
        break;
    case 15:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            unk_inline();
        }
        break;
    case 16:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            unk_inline();
        }
        break;
    case 17:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            unk_inline();
        }
        break;
    case 18:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            if (enter_data.unk1C == 0) {
                enter_data.unk8.unk0 = 0;
                enter_data.decision = tickDecision_20;
                lbCardGame_SetCardStatus(LbCardStatus_4);
            } else {
                enter_data.decision = 1;
            }
        }
        break;
    case 19:
        if (!gm_801AF0D4() && gm_801AEDC8_flag_check()) {
            if (enter_data.unk1C == 0) {
                enter_data.unk8.unk0 = 0;
                enter_data.decision = tickDecision_20;
                lbCardGame_SetCardStatus(LbCardStatus_4);
            } else {
                enter_data.decision = tickDecision_1;
            }
        }
        break;
    case 20:
        gm_801A4B60();
        break;
    default:
        gm_801A4B60();
        break;
    }
}

static inline bool checkUnk0(void)
{
    if (enter_data.unk0.unk0 == 0) {
        return tickDecision_0;
    }
    return tickDecision_1;
}

void gm_Scene_MemCard_OnEnter(void* user_data)
{
    enterData* data = user_data;

    memzero(&enter_data, sizeof(enter_data));
    if (data != NULL) {
        enter_data.unk0 = data->unk0;
    }
    enter_data.decision = checkUnk0();
    enter_data.unk8.mode_id = enter_data.unk0.mode_id;
    lbCardNew_AllocWorkArea();
    lbCardGame_LoadArchive(0);
    gm_801ADDD8();
    gm_804D6870 = 0;
    gm_804D6872 = 6;
}

void gm_Scene_MemCard_OnExit(void* user_data)
{
    exitData* data = user_data;
    if (data != NULL) {
        *data = enter_data.unk8;
    }
    gm_801AE848(0);
}
