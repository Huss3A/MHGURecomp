// Software keyboard. On the Switch it is a library applet; here:
//  - ShowKeyboard (naming the hunter, ...) answers with MHGU_NAME (default "Hunter") until an overlay text entry
//    exists;
//  - the inline keyboard (the hunter's name at character creation, chat) initializes (its finished-initialize
//    callback runs on the next Calc) and an Appear is answered on the next Calc with MHGU_NAME, as if typed and
//    confirmed (MHGU_INLINE_CANCEL=1: cancelled instead).
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

// nn::swkbd::ShowKeyboard(String *out, const ShowKeyboardArg &) -> Result. String = {void *ptr; size_t size}.
HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint32_t ptr = LD32(ARG(0)), size = LD32(ARG(0) + 4);
    const char *name = getenv("MHGU_NAME");
    if (!name || !*name) name = "Hunter";
    if (ptr && size) {
        memset(G2H(ptr), 0, size);
        snprintf((char *)G2H(ptr), size, "%s", name);
    }
    LOG("[swkbd] text entry answered: \"%s\"\n", name);
    RET(0);
}
HLE_FUNC("_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb", swkbd_work_size) { RET(0xD000); }
HLE_FUNC("_ZN2nn5swkbd27GetRequiredStringBufferSizeEv", swkbd_string_size) { RET(0x7D4); }

// ---- inline keyboard: callbacks the game set, called from Calc on its own thread
static uint32_t cb_init_done, cb_cancel, init_pending, cancel_pending;
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboardC1Ev", ik_ctor) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboardD1Ev", ik_dtor) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard25GetRequiredWorkBufferSizeEv", ik_work_size) { RET(0x40000); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard10InitializeEPv", ik_initialize) { init_pending = 1; RET(1); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard8FinalizeEv", ik_finalize) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard29SetFinishedInitializeCallbackEPFvvE", ik_cb_init) { cb_init_done = ARG(1); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard24SetDecidedCancelCallbackEPFvvE", ik_cb_cancel) { cb_cancel = ARG(1); }
// DecidedEnterArgUtf8 (as the game's callback reads it): the text inline at +0, its length in bytes at +0x7d4.
static uint32_t cb_enter, enter_pending;
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard27SetDecidedEnterCallbackUtf8EPFvPKNS0_19DecidedEnterArgUtf8EE", ik_cb_enter) { cb_enter = ARG(1); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard28SetChangedStringCallbackUtf8EPFvPKNS0_20ChangedStringArgUtf8EE", ik_cb_changed) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard26SetMovedCursorCallbackUtf8EPFvPKNS0_18MovedCursorArgUtf8EE", ik_cb_moved) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard27SetFinishedKeyboardCallbackEPFvvE", ik_cb_finished) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard31SetReleasedUserWordInfoCallbackEPFvvE", ik_cb_released) { (void)c; }
// Calc returns the keyboard's state (as the applet reports it): 0 not initialized, 1 ready and hidden, 3 shown. The
// game's keyboard driver waits for 1 after a text entry (its "closed" step), so a shown keyboard is reported for one
// Calc, then the text is decided (callback) and the state is back to 1.
static int ik_state, ik_shown_calcs;
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard4CalcEv", ik_calc) {
    uint32_t saved[4] = {c->r[0], c->r[1], c->r[2], c->r[3]};
    if (init_pending && cb_init_done) { init_pending = 0; guest_call(c, cb_init_done); }
    if (init_pending == 0 && ik_state == 0) ik_state = 1;
    if (ik_state == 3 && ik_shown_calcs++ >= 1) {
        if (cancel_pending && cb_cancel) { cancel_pending = 0; guest_call(c, cb_cancel); }
        if (enter_pending && cb_enter) {
            static uint32_t arg;
            if (!arg) arg = hle_alloc(0x800);
            const char *name = getenv("MHGU_NAME");
            if (!name || !*name) name = "Hunter";
            memset(G2H(arg), 0, 0x800);
            snprintf((char *)G2H(arg), 0x7d0, "%s", name);
            ST32(arg + 0x7d4, (uint32_t)strlen((char *)G2H(arg)));
            enter_pending = 0;
            c->r[0] = arg;
            guest_call(c, cb_enter);
        }
        ik_state = 1;
    }
    memcpy(c->r, saved, sizeof saved);
    RET(ik_state);
}
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard6AppearERKNS0_9AppearArgE", ik_appear) {
    ik_state = 3;
    ik_shown_calcs = 0;
    if (getenv("MHGU_INLINE_CANCEL")) { cancel_pending = 1; LOG("[swkbd] inline keyboard: cancelled\n"); return; }
    enter_pending = 1;
    LOG("[swkbd] inline keyboard (frame %llu): answered with MHGU_NAME\n", (unsigned long long)g_present_count);
}
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard9DisappearEv", ik_disappear) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard11SetUtf8ModeEb", ik_utf8) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard12SetCursorPosEi", ik_cursor) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard12SetInputTextERKNS0_9InputTextE", ik_input_text) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard14SetKeytopAlphaEf", ik_alpha) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard14SetKeytopScaleEf", ik_scale) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard18SetKeytopTranslateEff", ik_translate) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard15SetCustomizeDicEPKvjRKNS0_16CustomizeDicInfoE", ik_dic) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard17UnsetCustomizeDicEv", ik_undic) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard15SetUserWordInfoERKNS0_12UserWordInfoE", ik_words) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard17UnsetUserWordInfoEv", ik_unwords) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard25GetImageMemoryRequirementEPjS2_", ik_image_req) {
    if (ARG(1)) ST32(ARG(1), 0x10000);
    if (ARG(2)) ST32(ARG(2), 0x1000);
}
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard8GetImageEPvj", ik_image) { RET(0); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard26IsUsedTouchPointByKeyboardEii", ik_touch) { RET(0); }
HLE_FUNC("_ZNK2nn5swkbd14InlineKeyboard13GetWindowSizeEPiS2_", ik_window_size) {
    if (ARG(1)) ST32(ARG(1), 0);
    if (ARG(2)) ST32(ARG(2), 0);
}
HLE_FUNC("_ZN2nn5swkbd12UserWordInfo25GetRequiredWorkBufferSizeEi", swkbd_words_size) { RET(0x100 + ARG(0) * 0x80); }
