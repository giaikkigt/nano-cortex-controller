// ---- GILIAR CONTROL: home screen with two modes (included at the end of ui.c) ----
// PRESET: the four presets of a bank of four (16 banks = the Nano's 64 presets) as large coloured quadrants.
// STOMP: the signal chain as blocks (Pre FX 1-2, capture, cab, Post FX 1-3): a tap switches a block on / off,
// holding it opens its editor (FX editor, amp knobs, cab settings). The header is the same in both modes:
// the preset as bank + letter ("3B") and its name, the settings button and the PRESET | STOMP switch.
// Below: the bank (PRESET) or the touched block (STOMP), TAP (tap tempo, holding = tempo / metronome) and the tuner.
// Everything is drawn in design units (800 x 480), like the rest of ui.c.

#ifndef UI_REAL_W            // the Waveshare 4.3" (800 x 480) draws in design units 1:1, with its own (larger) fonts
#define SX(v) (v)
#define SR(v) (v)
#define GF_10 (&ui_font_16)
#define GF_11 (&ui_font_20)
#define GF_13 (&ui_font_20)
#define GF_T15 (&ui_font_title_26)
#define GF_T17 (&ui_font_title_30)
#define GF_T19 (&ui_font_title_30)
#define GF_T22 (&ui_font_title_34)
#define GF_T26 (&ui_font_title_40)
#define GF_T46 (&ui_font_title_46)
#else                        // JC3248W535C (480 x 320): the layout is scaled, the fonts are its smaller sizes
#define GF_10 (&ui_font_10)
#define GF_11 (&ui_font_11)
#define GF_13 (&ui_font_13)
#define GF_T15 (&ui_font_title_15)
#define GF_T17 (&ui_font_title_17)
#define GF_T19 (&ui_font_title_19)
#define GF_T22 (&ui_font_title_22)
#define GF_T26 (&ui_font_title_26)
#define GF_T46 (&ui_font_title_46)
#endif

#define GIL_BLOCKS 7          // FX 1, FX 2, capture, cab, FX 3, FX 4, FX 5 (chain order)
#define GIL_HEAD_H 120
#define GIL_BAR_Y 392
#define GIL_BAR_H 80
#define GIL_AMBER 0xFFB020
#define GIL_RED 0xC0352E
#define GIL_LINE 0x5A6270
#define GIL_BG 0x0E1116

static lv_obj_t *s_gil, *s_gil_num, *s_gil_letter, *s_gil_name, *s_gil_dirty;
static lv_obj_t *s_gil_mode_btn[2], *s_gil_mode_lbl[2];
static lv_obj_t *s_gil_stomp, *s_gil_presets;
static lv_obj_t *s_gil_blk[GIL_BLOCKS], *s_gil_blk_icon[GIL_BLOCKS], *s_gil_blk_tag[GIL_BLOCKS];
static lv_obj_t *s_gil_info, *s_gil_info_dot, *s_gil_info_text, *s_gil_info_state;
static lv_obj_t *s_gil_bank_box, *s_gil_bank_text;
static lv_obj_t *s_gil_quad[4], *s_gil_quad_letter[4], *s_gil_quad_name[4], *s_gil_quad_sub[4];
static lv_obj_t *s_gil_tap_bpm, *s_gil_swap;
static int s_gil_models_slot = -1;   // open the effect list of this slot once its editor is up
static uint32_t s_gil_models_tick;
static int s_gil_mode = 1;            // 0 = PRESET, 1 = STOMP
static int s_gil_bank = 0;            // 0-15 (bank of four presets)
static int s_gil_bank_preset = -1;    // the preset the bank followed last
static int s_gil_selected = 1;        // block shown in the info bar
static int s_gil_last_capture = 1, s_gil_last_cab = 1;   // what a tap switches back on
static nano_state_t *s_gil_st;        // last state (PSRAM; for the info bar and the quadrants)
static bool s_gil_have_state;

static const uint32_t GIL_QUAD_COLOR[4] = { 0xA63D49, 0x1F5F96, 0x4F7E3B, 0x7D2FA3 };

// Block index -> FX slot (-1 = capture / cab)
static int gil_slot(int b) { return b < 2 ? b : b == 2 || b == 3 ? -1 : b - 2; }

static lv_obj_t *gil_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static void gil_line(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *l = gil_box(parent, x, y, w, h, GIL_LINE);
    lv_obj_set_clickable(l, false);
}

static void gil_show_mode(void)
{
    lv_obj_set_hidden(s_gil_stomp, s_gil_mode != 1);
    lv_obj_set_hidden(s_gil_presets, s_gil_mode != 0);
    lv_obj_set_hidden(s_gil_info, s_gil_mode != 1);
    lv_obj_set_hidden(s_gil_bank_box, s_gil_mode != 0);
    for (int i = 0; i < 2; i++) {
        bool on = i == s_gil_mode;
        lv_obj_set_style_bg_color(s_gil_mode_btn[i], lv_color_hex(on ? GIL_AMBER : BUTTON_BG), 0);
        lv_obj_set_style_text_color(s_gil_mode_lbl[i], lv_color_hex(on ? 0x1A1200 : MUTED), 0);
    }
}

static void gil_fill(void);
static void gk_build(lv_obj_t *screen);
static void gk_show_amp(void);
static void gk_show_cab(void);
static void gk_open_fx(int slot);
static int s_gk_want;

static void on_gil_mode(lv_event_t *e)
{
    s_gil_mode = (int)(intptr_t)lv_event_get_user_data(e);
    gil_show_mode();
    gil_fill();
}

static void on_gil_settings(lv_event_t *e) { open_settings(e); }
static void on_gil_tap(lv_event_t *e) { on_metro_tap(e); }
static void on_gil_tap_long(lv_event_t *e) { on_tempo_button(e); }
static void on_gil_tuner(lv_event_t *e) { send('t', 0); }

static void gil_models_poll(lv_timer_t *t)
{
    if (s_gil_models_slot < 0 || lv_tick_elaps(s_gil_models_tick) > 3000) { s_gil_models_slot = -1; lv_timer_delete(t); return; }
    if (!lv_obj_is_hidden(s_editor) && s_editor_slot_index == s_gil_models_slot) {
        s_gil_models_slot = -1;
        lv_timer_delete(t);
        open_panel(PANEL_MODEL, 0);
    }
}

// Add an effect to a slot or change the one in it: the FX editor of the slot with its effect list open.
static void gil_choose_effect(int slot)
{
    s_gil_models_slot = slot;
    s_gk_want = -1;
    s_gil_models_tick = lv_tick_get();
    lv_timer_create(gil_models_poll, 50, NULL);
    send('O', slot);
}

static void on_gil_swap(lv_event_t *e)
{
    int slot = gil_slot(s_gil_selected);
    if (slot >= 0) gil_choose_effect(slot);
    else if (s_gil_selected == 2) open_capture_picker(e);
    else open_cab_picker(e);
}

static void on_gil_block(lv_event_t *e)
{
    int b = (int)(intptr_t)lv_event_get_user_data(e);
    s_gil_selected = b;
    int slot = gil_slot(b);
    if (slot >= 0) {
        if (s_gil_have_state && s_gil_st->fx_known && s_gil_st->fx_type[slot]) send('a' + slot, 0);
        else gil_choose_effect(slot);   // empty slot: the effect list, to add one
    } else if (b == 2) {
        int cur = s_gil_have_state ? s_gil_st->capture_slot : 0;
        if (cur) s_gil_last_capture = cur;
        send('C', cur ? 0 : s_gil_last_capture);
    } else {
        int cur = s_gil_have_state ? s_gil_st->cab_slot : 0;
        if (cur) s_gil_last_cab = cur;
        send('I', cur ? 0 : s_gil_last_cab);
    }
    gil_fill();
}

static void on_gil_block_long(lv_event_t *e)
{
    int b = (int)(intptr_t)lv_event_get_user_data(e);
    s_gil_selected = b;
    int slot = gil_slot(b);
    if (slot >= 0) gk_open_fx(slot);
    else if (b == 2) gk_show_amp();
    else gk_show_cab();
    gil_fill();
}

static void on_gil_quad(lv_event_t *e)
{
    int q = (int)(intptr_t)lv_event_get_user_data(e);
    send('g', s_gil_bank * 4 + q + 1);
}

static void on_gil_bank(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    s_gil_bank = (s_gil_bank + step + 16) % 16;
    gil_fill();
}

static lv_obj_t *gil_button(lv_obj_t *parent, int x, int y, int w, int h, lv_event_cb_t cb, int data)
{
    lv_obj_t *b = button(parent, x, y, w, h, cb);
    lv_obj_remove_event_cb(b, cb);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)data);
    return b;
}

static void gil_build(lv_obj_t *parent)
{
    s_gil_st = heap_caps_calloc(1, sizeof(nano_state_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // The original main screen (top bar, cards, tiles, pedals) goes into a hidden holder: covered objects that change
    // (animations of the tiles) would still make LVGL send whole frames to the panel - and a touch read right after a
    // frame fails. Hidden objects are not drawn; the original code keeps updating them as before.
    lv_obj_t *old = lv_obj_create(parent);
    lv_obj_remove_style_all(old);
    lv_obj_set_pos(old, 0, 0);
    lv_obj_set_size(old, 800, 480);
    lv_obj_set_scrollable(old, false);
    lv_obj_set_clickable(old, false);
    for (int i = (int)lv_obj_get_child_count(parent) - 1; i >= 0; i--) {
        lv_obj_t *c = lv_obj_get_child(parent, i);
        if (c != old) lv_obj_set_parent(c, old);
    }
    lv_obj_set_hidden(old, true);
    s_gil = gil_box(parent, 0, 0, 800, 480, GIL_BG);
    lv_obj_set_clickable(s_gil, false);

    // Header: "3B" + name, settings, PRESET | STOMP
    lv_obj_t *head = lv_obj_create(s_gil);
    lv_obj_remove_style_all(head);
    lv_obj_set_pos(head, 18, 14);
    lv_obj_set_size(head, 530, 100);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_clickable(head, false);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 6, 0);
    s_gil_num = label(head, GF_T46, TEXT);
    s_gil_letter = label(head, GF_T46, 0x5BD48A);
    s_gil_name = label(head, GF_T26, TEXT);
    lv_obj_set_style_pad_left(s_gil_name, 14, 0);
    lv_label_set_long_mode(s_gil_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_gil_name, 330);
    s_gil_dirty = gil_box(head, 0, 0, 14, 14, ORANGE);
    lv_obj_set_style_radius(s_gil_dirty, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_hidden(s_gil_dirty, true);

    lv_obj_t *gear = gil_button(s_gil, 730, 6, 60, 50, on_gil_settings, 0);
    lv_obj_set_style_border_width(gear, 0, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_TRANSP, 0);
    lv_obj_center(ui_icon(gear, UI_ICON_GEAR, 26, TEXT));

    static const char *const words[2] = { "PRESET", "STOMP" };
    for (int i = 0; i < 2; i++) {
        s_gil_mode_btn[i] = gil_button(s_gil, 560 + i * 115, 62, 113, 50, on_gil_mode, i);
        lv_obj_set_style_radius(s_gil_mode_btn[i], 8, 0);
        s_gil_mode_lbl[i] = label(s_gil_mode_btn[i], GF_T15, MUTED);
        lv_label_set_text(s_gil_mode_lbl[i], words[i]);
        lv_obj_center(s_gil_mode_lbl[i]);
    }

    // STOMP: the chain (two rows, joined as on a floor unit)
    s_gil_stomp = lv_obj_create(s_gil);
    lv_obj_remove_style_all(s_gil_stomp);
    lv_obj_set_pos(s_gil_stomp, 0, GIL_HEAD_H);
    lv_obj_set_size(s_gil_stomp, 800, GIL_BAR_Y - GIL_HEAD_H - 6);
    lv_obj_set_scrollable(s_gil_stomp, false);
    lv_obj_set_clickable(s_gil_stomp, false);
    const int bw = 100, bh = 84, pad = 70;
    const int gap = (800 - 2 * pad - 4 * bw) / 3;
    const int y1 = 34, y2 = y1 + bh + 58;
    const int y1c = y1 + bh / 2, y2c = y2 + bh / 2, ym = y1 + bh + 29;
    const int xr = pad + 3 * (bw + gap) + bw + 22, xl = pad - 26;
    gil_line(s_gil_stomp, 46, y1c - 2, xr - 46, 4);
    gil_line(s_gil_stomp, xr - 2, y1c, 4, ym - y1c);
    gil_line(s_gil_stomp, xl, ym - 2, xr - xl, 4);
    gil_line(s_gil_stomp, xl - 2, ym, 4, y2c - ym);
    gil_line(s_gil_stomp, xl, y2c - 2, 800 - 56 - xl, 4);
    lv_obj_t *in = label(s_gil_stomp, GF_11, 0xC9CFD8);
    lv_label_set_text(in, "In");
    lv_obj_set_pos(in, 10, y1c - 12);
    lv_obj_t *out = label(s_gil_stomp, GF_11, 0xC9CFD8);
    lv_label_set_text(out, "Out");
    lv_obj_set_pos(out, 800 - 52, y2c - 12);
    for (int b = 0; b < GIL_BLOCKS; b++) {
        int col = b < 4 ? b : b - 4, x = pad + col * (bw + gap), y = b < 4 ? y1 : y2;
        lv_obj_t *o = gil_box(s_gil_stomp, x, y, bw, bh, GIL_BG);
        lv_obj_set_style_radius(o, 14, 0);
        lv_obj_set_style_border_width(o, 3, 0);
        lv_obj_set_clickable(o, true);
        lv_obj_set_ext_click_area(o, 8);
        lv_obj_add_event_cb(o, on_gil_block, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)b);
        lv_obj_add_event_cb(o, on_gil_block_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)b);
        s_gil_blk[b] = o;
        lv_obj_t *img = lv_image_create(o);
        lv_image_set_scale(img, 256 * 44 / 36);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
        lv_obj_center(img);
        lv_obj_set_clickable(img, false);
        s_gil_blk_icon[b] = img;
        lv_obj_t *tag = label(s_gil_stomp, GF_10, 0xFFFFFF);
        lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tag, 6, 0);
        lv_obj_set_style_pad_hor(tag, 8, 0);
        lv_obj_set_style_pad_ver(tag, 2, 0);
        lv_label_set_long_mode(tag, LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(tag, SX(bw + 40), 0);
        lv_obj_set_pos(tag, x, y - 28);
        s_gil_blk_tag[b] = tag;
    }

    // PRESET: four quadrants
    s_gil_presets = lv_obj_create(s_gil);
    lv_obj_remove_style_all(s_gil_presets);
    lv_obj_set_pos(s_gil_presets, 0, GIL_HEAD_H);
    lv_obj_set_size(s_gil_presets, 800, GIL_BAR_Y - GIL_HEAD_H - 6);
    lv_obj_set_scrollable(s_gil_presets, false);
    lv_obj_set_clickable(s_gil_presets, false);
    for (int q = 0; q < 4; q++) {
        int x = 10 + (q % 2) * 395, y = 6 + (q / 2) * 130;
        lv_obj_t *o = gil_box(s_gil_presets, x, y, 385, 122, GIL_QUAD_COLOR[q]);
        lv_obj_set_style_radius(o, 16, 0);
        lv_obj_set_clickable(o, true);
        lv_obj_add_event_cb(o, on_gil_quad, LV_EVENT_CLICKED, (void *)(intptr_t)q);
        s_gil_quad[q] = o;
        s_gil_quad_letter[q] = label(o, GF_T46, 0xFFFFFF);
        lv_obj_set_style_text_opa(s_gil_quad_letter[q], LV_OPA_60, 0);
        lv_obj_align(s_gil_quad_letter[q], LV_ALIGN_TOP_RIGHT, -18, 6);
        s_gil_quad_name[q] = label(o, GF_T19, 0xFFFFFF);
        lv_label_set_long_mode(s_gil_quad_name[q], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_gil_quad_name[q], 290);
        lv_obj_align(s_gil_quad_name[q], LV_ALIGN_BOTTOM_LEFT, 16, -34);
        s_gil_quad_sub[q] = label(o, GF_11, 0xFFFFFF);
        lv_obj_set_style_text_opa(s_gil_quad_sub[q], LV_OPA_80, 0);
        lv_obj_align(s_gil_quad_sub[q], LV_ALIGN_BOTTOM_LEFT, 16, -12);
    }

    // Bottom bar: info (STOMP) or bank (PRESET), TAP, tuner
    s_gil_info = gil_box(s_gil, 15, GIL_BAR_Y, 520, GIL_BAR_H, PANEL_BG);
    lv_obj_set_style_radius(s_gil_info, 12, 0);
    lv_obj_set_style_border_width(s_gil_info, 1, 0);
    lv_obj_set_style_border_color(s_gil_info, lv_color_hex(HAIRLINE), 0);
    s_gil_info_dot = gil_box(s_gil_info, 18, 32, 16, 16, GIL_AMBER);
    lv_obj_set_style_radius(s_gil_info_dot, LV_RADIUS_CIRCLE, 0);
    s_gil_info_text = label(s_gil_info, GF_T17, TEXT);
    lv_label_set_long_mode(s_gil_info_text, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_gil_info_text, 360);
    lv_obj_align(s_gil_info_text, LV_ALIGN_LEFT_MID, 46, 0);
    lv_obj_set_width(s_gil_info_text, 250);
    s_gil_info_state = label(s_gil_info, GF_13, MUTED);
    lv_obj_align(s_gil_info_state, LV_ALIGN_LEFT_MID, 306, 0);
    s_gil_swap = gil_button(s_gil_info, 372, 10, 138, 58, on_gil_swap, 0);
    lv_obj_t *swap_word = label(s_gil_swap, GF_T15, TEXT);
    lv_label_set_text(swap_word, "TROCAR");
    lv_obj_center(swap_word);

    s_gil_bank_box = gil_box(s_gil, 15, GIL_BAR_Y, 520, GIL_BAR_H, PANEL_BG);
    lv_obj_set_style_radius(s_gil_bank_box, 12, 0);
    lv_obj_set_style_border_width(s_gil_bank_box, 1, 0);
    lv_obj_set_style_border_color(s_gil_bank_box, lv_color_hex(HAIRLINE), 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = gil_button(s_gil_bank_box, i ? 440 : 6, 6, 72, 66, on_gil_bank, i ? 1 : -1);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_t *l = label(b, GF_T26, TEXT);
        lv_label_set_text(l, i ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_center(l);
    }
    s_gil_bank_text = label(s_gil_bank_box, GF_T17, TEXT);
    lv_obj_align(s_gil_bank_text, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *tap = gil_button(s_gil, 548, GIL_BAR_Y, 112, GIL_BAR_H, on_gil_tap, 0);
    lv_obj_add_event_cb(tap, on_gil_tap_long, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *tap_word = label(tap, GF_10, TEXT);
    lv_label_set_text(tap_word, "TAP");
    lv_obj_align(tap_word, LV_ALIGN_TOP_MID, 0, 6);
    s_gil_tap_bpm = label(tap, GF_T22, GIL_AMBER);
    lv_label_set_text(s_gil_tap_bpm, "--");
    lv_obj_align(s_gil_tap_bpm, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_obj_t *tuner = gil_button(s_gil, 672, GIL_BAR_Y, 113, GIL_BAR_H, on_gil_tuner, 0);
    lv_obj_t *fork = lv_image_create(tuner);
    lv_image_set_src(fork, NANO_ICONS[NANO_ICON_TUNER]);
    lv_obj_set_style_image_recolor_opa(fork, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(fork, lv_color_hex(TEXT), 0);
    lv_obj_center(fork);

    gil_show_mode();
    gk_build(lv_obj_get_screen(parent));
}

static void gil_fill(void)
{
    if (!s_gil) return;
    if (!s_gil_st) return;
    const nano_state_t *st = s_gil_st;
    int cur = s_gil_have_state ? st->current_preset : 0;
    char text[96];
    if (cur >= 1 && cur <= NANO_PRESETS) {
        snprintf(text, sizeof(text), "%d", (cur - 1) / 4 + 1);
        set_text(s_gil_num, text);
        snprintf(text, sizeof(text), "%c", 'A' + (cur - 1) % 4);
        set_text(s_gil_letter, text);
        const char *name = st->preset_names[cur - 1];
        set_text(s_gil_name, name[0] ? name : "");
    } else {
        set_text(s_gil_num, "-");
        set_text(s_gil_letter, "");
        set_text(s_gil_name, "Procurando a Nano...");
    }
    set_hidden(s_gil_dirty, !(s_gil_have_state && st->dirty));
    if (s_gil_have_state && st->tempo > 0) snprintf(text, sizeof(text), "%d", (int)lroundf(st->tempo));
    else snprintf(text, sizeof(text), "--");
    set_text(s_gil_tap_bpm, text);

    // Blocks
    for (int b = 0; b < GIL_BLOCKS; b++) {
        int slot = gil_slot(b);
        uint32_t color = 0x6B7480;
        bool on = false, empty = false;
        int icon = NANO_ICON_UTILITY;
        char tag[48];
        if (slot >= 0) {
            uint32_t type = s_gil_have_state && st->fx_known ? st->fx_type[slot] : 0;
            const nano_fx_model_t *m = type ? nano_fx_model(type) : NULL;
            empty = !type;
            on = type && st->fx_on[slot];
            if (m) { color = m->color; icon = m->icon; snprintf(tag, sizeof(tag), "%s", m->name); }
            else snprintf(tag, sizeof(tag), empty ? "FX %d" : "FX %d ?", slot + 1);
        } else if (b == 2) {
            color = GIL_RED; icon = NANO_ICON_CAPTURE;
            on = s_gil_have_state && st->capture_slot > 0;
            snprintf(tag, sizeof(tag), "CAP AMP");
        } else {
            color = GIL_RED; icon = NANO_ICON_CAB;
            on = s_gil_have_state && st->cab_slot > 0;
            if (on) snprintf(tag, sizeof(tag), "IR %d", st->cab_slot);
            else snprintf(tag, sizeof(tag), "CAB/IR");
        }
        lv_obj_t *o = s_gil_blk[b];
        set_color_prop(o, LV_STYLE_BG_COLOR, on && b != 3 ? color : GIL_BG);
        set_color_prop(o, LV_STYLE_BORDER_COLOR, on ? color : blend(color, GIL_BG, 0.5f));
        set_num_prop(o, LV_STYLE_BORDER_OPA, empty ? LV_OPA_40 : LV_OPA_COVER);
        set_num_prop(o, LV_STYLE_OUTLINE_WIDTH, b == s_gil_selected ? 3 : 0);
        set_color_prop(o, LV_STYLE_OUTLINE_COLOR, TEXT);
        set_num_prop(o, LV_STYLE_OUTLINE_PAD, SR(3));
        set_image(s_gil_blk_icon[b], NANO_ICONS[icon]);
        set_color_prop(s_gil_blk_icon[b], LV_STYLE_IMAGE_RECOLOR, on && b != 3 ? ink_for(color) : color);
        set_num_prop(s_gil_blk_icon[b], LV_STYLE_OPA, empty ? LV_OPA_30 : LV_OPA_COVER);
        set_text(s_gil_blk_tag[b], tag);
        set_color_prop(s_gil_blk_tag[b], LV_STYLE_BG_COLOR, empty ? 0x2A313C : color);
        set_color_prop(s_gil_blk_tag[b], LV_STYLE_TEXT_COLOR, empty ? MUTED : ink_for(color));
        if (b == s_gil_selected) {
            static const char *const where[GIL_BLOCKS] = { "FX 1", "FX 2", "CAP AMP", "CAB/IR", "FX 3", "FX 4", "FX 5" };
            if (b == 2 && s_gil_have_state && st->capture_slot > 0 && st->capture[0])   // the amp: its capture's name
                snprintf(text, sizeof(text), "CAP AMP %d \xC2\xB7 %.60s", st->capture_slot, st->capture);
            else if (b == 3 && s_gil_have_state && st->cab_slot > 0 && st->cab[0])
                snprintf(text, sizeof(text), "IR %d \xC2\xB7 %.60s", st->cab_slot, st->cab);
            else snprintf(text, sizeof(text), "%s \xC2\xB7 %s", where[b], tag);
            set_text(s_gil_info_text, text);
            set_text(s_gil_info_state, empty ? "vazio" : on ? "ON" : "OFF");
            set_text(lv_obj_get_child(s_gil_swap, 0), empty ? "ADICIONAR" : "TROCAR");
            set_color_prop(s_gil_info_dot, LV_STYLE_BG_COLOR, color);
        }
    }

    // Quadrants
    if (cur >= 1 && s_gil_bank_preset != cur) {   // the bank follows a preset change
        s_gil_bank = (cur - 1) / 4;
        s_gil_bank_preset = cur;
    }
    for (int q = 0; q < 4; q++) {
        int n = s_gil_bank * 4 + q + 1;
        bool active = n == cur;
        uint32_t bg = active ? blend(GIL_QUAD_COLOR[q], 0xFFFFFF, 0.45f) : GIL_QUAD_COLOR[q];
        uint32_t ink = active ? 0x0B1A2A : 0xFFFFFF;
        set_color_prop(s_gil_quad[q], LV_STYLE_BG_COLOR, bg);
        set_num_prop(s_gil_quad[q], LV_STYLE_OUTLINE_WIDTH, active ? 3 : 0);
        set_color_prop(s_gil_quad[q], LV_STYLE_OUTLINE_COLOR, TEXT);
        set_num_prop(s_gil_quad[q], LV_STYLE_OUTLINE_PAD, SR(2));
        snprintf(text, sizeof(text), "%c", 'A' + q);
        set_text(s_gil_quad_letter[q], text);
        set_color_prop(s_gil_quad_letter[q], LV_STYLE_TEXT_COLOR, active ? 0x0B4F8A : 0xFFFFFF);
        const char *name = s_gil_have_state ? st->preset_names[n - 1] : "";
        snprintf(text, sizeof(text), "%d%c %s", s_gil_bank + 1, 'A' + q, name);
        set_text(s_gil_quad_name[q], text);
        set_color_prop(s_gil_quad_name[q], LV_STYLE_TEXT_COLOR, ink);
        snprintf(text, sizeof(text), "Preset %d", n);
        set_text(s_gil_quad_sub[q], text);
        set_color_prop(s_gil_quad_sub[q], LV_STYLE_TEXT_COLOR, ink);
    }
    snprintf(text, sizeof(text), "BANCO %d de 16", s_gil_bank + 1);
    set_text(s_gil_bank_text, text);
}

static void gil_show(const nano_state_t *st)
{
    if (!s_gil_st) return;
    *s_gil_st = *st;
    s_gil_have_state = true;
    if (st->capture_slot) s_gil_last_capture = st->capture_slot;
    if (st->cab_slot) s_gil_last_cab = st->cab_slot;
    gil_fill();
}

// ---- GILIAR CONTROL: knob editor (as on a floor unit) ----
// Over the FX editor of the project (it keeps reading the values and sending them; this only shows them as knobs,
// switches and option buttons) or, for the capture block, over the amp knobs. 5 x 2 cells: a range is a knob, an
// option with two choices a switch, one with more a button that steps through them. The last cell of the page is the
// block on / off. Header: back, the effect's name, previous / next block, save. "Editor completo" shows the project's
// own editor (effect list, FX presets, A / B, pad).

#define GK_CELLS 10
#define GK_PER_PAGE 9           // parameters per page (the tenth cell is the block's on / off)
#define GK_HEAD_H 72
#define GK_FOOT_Y 420
#define GK_CYAN 0x4FB3E6
enum { GK_NONE, GK_FX, GK_AMP, GK_CAB };
enum { GKC_EMPTY, GKC_KNOB, GKC_SWITCH, GKC_OPTION, GKC_BUTTON };

static lv_obj_t *s_gk, *s_gk_title, *s_gk_chip, *s_gk_chip_lbl, *s_gk_page_btn, *s_gk_page_lbl, *s_gk_full, *s_gk_dirty, *s_gk_hint;
static lv_obj_t *s_gk_cell[GK_CELLS], *s_gk_lbl[GK_CELLS], *s_gk_val[GK_CELLS], *s_gk_arc[GK_CELLS];
static lv_obj_t *s_gk_sw[GK_CELLS], *s_gk_sw_dot[GK_CELLS], *s_gk_opt[GK_CELLS][2], *s_gk_btn[GK_CELLS], *s_gk_btn_lbl[GK_CELLS];
static int s_gk_kind = GK_NONE, s_gk_page, s_gk_slot = -1, s_gk_want = -1;
static int s_gk_map[GK_CELLS];      // FX: parameter index of a cell (-1 = none, -2 = on / off); AMP: 0-4 knobs, 5-8 the rest
static uint8_t s_gk_ckind[GK_CELLS];

static void gk_hide(void)
{
    s_gk_kind = GK_NONE;
    lv_obj_set_hidden(s_gk, true);
}

static void gk_cell_show(int c, int kind, const char *caption)
{
    s_gk_ckind[c] = (uint8_t)kind;
    set_hidden(s_gk_arc[c], kind != GKC_KNOB);
    set_hidden(s_gk_val[c], kind != GKC_KNOB);
    set_hidden(s_gk_sw[c], kind != GKC_SWITCH);
    set_hidden(s_gk_opt[c][0], kind != GKC_SWITCH);
    set_hidden(s_gk_opt[c][1], kind != GKC_SWITCH);
    set_hidden(s_gk_btn[c], kind != GKC_OPTION && kind != GKC_BUTTON);
    set_text(s_gk_lbl[c], caption ? caption : "");
}

static void gk_switch_show(int c, const char *a, const char *b, int sel)
{
    set_text(s_gk_opt[c][0], a);
    set_text(s_gk_opt[c][1], b);
    set_color_prop(s_gk_opt[c][0], LV_STYLE_TEXT_COLOR, sel == 0 ? TEXT : 0x6F7987);
    set_color_prop(s_gk_opt[c][1], LV_STYLE_TEXT_COLOR, sel == 1 ? TEXT : 0x6F7987);
    set_align_if(s_gk_sw_dot[c], sel == 1 ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_TOP_MID, 0, sel == 1 ? -3 : 3);
}

static void gk_knob_show(int c, float n, const char *text)
{
    if (!lv_obj_has_state(s_gk_arc[c], LV_STATE_PRESSED) && n >= 0) {
        int32_t v = (int32_t)lroundf(n * 1000);
        if (lv_arc_get_value(s_gk_arc[c]) != v) lv_arc_set_value(s_gk_arc[c], v);
    }
    set_num_prop(s_gk_arc[c], LV_STYLE_OPA, n < 0 ? LV_OPA_40 : LV_OPA_COVER);
    set_text(s_gk_val[c], text);
}

static const char *const GK_AMP_NAMES[5] = { "GAIN", "BASS", "MID", "TREBLE", "VOLUME" };
static const int GK_AMP_KNOB[4] = { NANO_AMP_GAIN, NANO_AMP_BASS, NANO_AMP_MID, NANO_AMP_TREBLE };

// Everything the cells show, from the editor's / the amp's current values.
static void gk_fill(void)
{
    char text[48];
    bool dirty = s_gil_have_state && s_gil_st && s_gil_st->dirty;
    set_hidden(s_gk_dirty, !dirty);
    if (s_gk_kind == GK_AMP) {
        const nano_state_t *st = s_gil_st;
        set_text(s_gk_title, st && st->capture[0] ? st->capture : "Capture");
        set_text(s_gk_chip_lbl, "CAP");
        set_hidden(s_gk_page_btn, true);
        set_hidden(s_gk_full, true);
        set_text(s_gk_hint, "Muda na Nano na hora \xC2\xB7 \xE2\x9C\x93 salva o preset");
        for (int c = 0; c < GK_CELLS; c++) {
            if (c < 4) {
                int v = s_amp_values[GK_AMP_KNOB[c]];
                snprintf(text, sizeof(text), "%.1f", (double)(v * 10.0f / 255.0f));
                gk_cell_show(c, GKC_KNOB, GK_AMP_NAMES[c]);
                gk_knob_show(c, v / 255.0f, text);
            } else if (c == 4) {
                snprintf(text, sizeof(text), "%.1f dB", (double)nano_capture_volume_db(s_capture_volume));
                gk_cell_show(c, GKC_KNOB, "VOLUME");
                gk_knob_show(c, s_capture_volume / 255.0f, text);
            } else if (c == 5) {
                gk_cell_show(c, GKC_BUTTON, "AMP");
                if (st && st->capture_slot) snprintf(text, sizeof(text), "Cap %02d", st->capture_slot);
                else snprintf(text, sizeof(text), "Bypass");
                set_text(s_gk_btn_lbl[c], text);
            } else if (c == 6) {
                gk_cell_show(c, GKC_BUTTON, "GABINETE");
                if (st && st->cab_slot) snprintf(text, sizeof(text), "IR %d", st->cab_slot);
                else snprintf(text, sizeof(text), "Bypass");
                set_text(s_gk_btn_lbl[c], text);
            } else if (c == 7) {
                gk_cell_show(c, GKC_SWITCH, "BLOCO");
                gk_switch_show(c, "Off", "On", st && st->capture_slot > 0 ? 1 : 0);
            } else if (c == 8) {
                gk_cell_show(c, GKC_BUTTON, "EXPRESS\xC3\x83O");
                set_text(s_gk_btn_lbl[c], "Ajustar");
            } else gk_cell_show(c, GKC_EMPTY, "");
        }
        return;
    }
    if (s_gk_kind == GK_CAB) {
        static const char *const NAMES[3] = { "OUTPUT", "HIGH PASS", "LOW PASS" };
        const nano_state_t *st = s_gil_st;
        set_text(s_gk_title, st && st->cab[0] ? st->cab : "Gabinete");
        set_text(s_gk_chip_lbl, "IR");
        set_hidden(s_gk_page_btn, true);
        set_hidden(s_gk_full, true);
        set_text(s_gk_hint, "Muda na Nano na hora \xC2\xB7 \xE2\x9C\x93 salva o preset");
        for (int c = 0; c < GK_CELLS; c++) {
            if (c < 3) {
                int row = 1 + c;
                bool known = !lv_obj_has_state(s_level_slider[row], LV_STATE_DISABLED);
                const char *v = lv_label_get_text(s_level_value[row]);
                gk_cell_show(c, GKC_KNOB, NAMES[c]);
                gk_knob_show(c, known ? lv_slider_get_value(s_level_slider[row]) / (float)CAB_SLIDER_MAX : -1,
                             known && v[0] ? v : "\xE2\x80\x93");
            } else if (c == 5) {
                gk_cell_show(c, GKC_BUTTON, "GABINETE");
                if (st && st->cab_slot) snprintf(text, sizeof(text), "IR %d", st->cab_slot);
                else snprintf(text, sizeof(text), "Bypass");
                set_text(s_gk_btn_lbl[c], text);
            } else if (c == 6) {
                gk_cell_show(c, GKC_BUTTON, "AMP");
                if (st && st->capture_slot) snprintf(text, sizeof(text), "Cap %02d", st->capture_slot);
                else snprintf(text, sizeof(text), "Bypass");
                set_text(s_gk_btn_lbl[c], text);
            } else if (c == 7) {
                gk_cell_show(c, GKC_SWITCH, "BLOCO");
                gk_switch_show(c, "Off", "On", st && st->cab_slot > 0 ? 1 : 0);
            } else gk_cell_show(c, GKC_EMPTY, "");
        }
        return;
    }
    // FX
    const nano_fx_model_t *m = s_editor_model_def;
    int count = ed_param_count();
    int pages = count > GK_PER_PAGE ? (count + GK_PER_PAGE - 1) / GK_PER_PAGE : 1;
    if (s_gk_page >= pages) s_gk_page = 0;
    set_text(s_gk_title, m ? m->name : "Vazio");
    snprintf(text, sizeof(text), "FX %d", s_gk_slot + 1);
    set_text(s_gk_chip_lbl, text);
    set_hidden(s_gk_page_btn, pages < 2);
    snprintf(text, sizeof(text), "%d/%d " LV_SYMBOL_RIGHT, s_gk_page + 1, pages);
    set_text(s_gk_page_lbl, text);
    set_hidden(s_gk_full, false);
    bool on = s_gk_slot >= 0 && s_slot_on[s_gk_slot];
    set_text(s_gk_hint, !m ? "Posi\xC3\xA7\xC3\xA3o vazia: toque no nome acima para escolher um efeito"
                       : !on ? "Efeito desligado: ligue para ler e mudar os valores"
                       : "Muda na Nano na hora \xC2\xB7 \xE2\x9C\x93 salva o preset");
    for (int c = 0; c < GK_CELLS; c++) {
        if (c == GK_CELLS - 1) {
            s_gk_map[c] = -2;
            gk_cell_show(c, GKC_SWITCH, "BLOCO");
            gk_switch_show(c, "Off", "On", on ? 1 : 0);
            continue;
        }
        int idx = s_gk_page * GK_PER_PAGE + c;
        if (!m || idx >= count) {
            s_gk_map[c] = -1;
            gk_cell_show(c, GKC_EMPTY, "");
            continue;
        }
        s_gk_map[c] = idx;
        const nano_param_t *p = ed_param(idx);
        float n = s_param_norm[idx];
        if (p->kind == NANO_PARAM_RANGE) {
            char v[32];
            if (n < 0) snprintf(v, sizeof(v), "\xE2\x80\x93");
            else snprintf(v, sizeof(v), "%s%s%s", s_param_text[idx], p->unit && p->unit[0] ? " " : "", p->unit ? p->unit : "");
            gk_cell_show(c, GKC_KNOB, p->name);
            gk_knob_show(c, n, v);
        } else if (p->option_count == 2) {
            char a[16], b[16];
            option_text(p, 0, a, sizeof(a));
            option_text(p, 1, b, sizeof(b));
            gk_cell_show(c, GKC_SWITCH, p->name);
            gk_switch_show(c, a, b, option_index(p, n));
        } else {
            gk_cell_show(c, GKC_OPTION, p->name);
            set_text(s_gk_btn_lbl[c], n < 0 ? "\xE2\x80\x93" : s_param_text[idx]);
        }
    }
}

static void gk_show_amp(void)
{
    s_gk_kind = GK_AMP;
    s_gk_page = 0;
    gk_fill();
    lv_obj_set_hidden(s_gk, false);
    lv_obj_move_foreground(s_gk);
    lv_obj_move_foreground(s_toast);
}

// The cab's output, high pass and low pass: read from the Nano each time (as the project's cab dialog does).
static void gk_show_cab(void)
{
    for (int row = 1; row < LEVEL_ROWS && row < AMP_ROW; row++) {
        set_level_row_enabled(row, false);
        lv_label_set_text(s_level_value[row], "");
    }
    send('y', 1);
    s_gk_kind = GK_CAB;
    s_gk_page = 0;
    gk_fill();
    lv_obj_set_hidden(s_gk, false);
    lv_obj_move_foreground(s_gk);
    lv_obj_move_foreground(s_toast);
}

static void gk_show_fx(int slot)
{
    s_gk_kind = GK_FX;
    s_gk_slot = slot;
    s_gk_page = 0;
    gk_fill();
    lv_obj_set_hidden(s_gk, false);
    lv_obj_move_foreground(s_gk);
    lv_obj_move_foreground(s_toast);
}

// Opens the knob editor of FX slot `slot` once the project's editor is up for it.
static void gk_open_fx(int slot)
{
    s_gk_want = slot;
    s_gil_models_slot = -1;
    send('O', slot);
}

static void gk_tick(lv_timer_t *t)
{
    (void)t;
    if (s_gk_want >= 0 && !lv_obj_is_hidden(s_editor) && s_editor_slot_index == s_gk_want) {
        gk_show_fx(s_gk_want);
        s_gk_want = -1;
    }
    if (s_gk_kind == GK_FX && lv_obj_is_hidden(s_editor)) gk_hide();   // the editor was closed (or another took over)
    if (s_gk_kind != GK_NONE && !lv_obj_is_hidden(s_gk)) gk_fill();
}

static void on_gk_back(lv_event_t *e)
{
    if (s_gk_kind == GK_FX) editor_leave();
    if (s_gk_kind == GK_CAB) send('y', 0);
    gk_hide();
}

static void on_gk_full(lv_event_t *e) { gk_hide(); }   // the project's own editor stays open below

// The name in the header: change the effect (the editor's effect list) or the amp (captures, with the library).
static void on_gk_change(lv_event_t *e)
{
    if (s_gk_kind == GK_FX) {
        gk_hide();                      // the project's editor of this slot is open below: its effect list
        open_panel(PANEL_MODEL, 0);
    } else if (s_gk_kind == GK_AMP) {
        open_capture_picker(e);
        lv_obj_move_foreground(s_picker);
    } else if (s_gk_kind == GK_CAB) {
        open_cab_picker(e);
        lv_obj_move_foreground(s_picker);
    }
}

static void on_gk_save(lv_event_t *e)
{
    on_save_button(e);
    lv_obj_move_foreground(s_ask);   // the question comes above the knob editor
}

static void on_gk_page(lv_event_t *e)
{
    s_gk_page++;
    gk_fill();
}

// Previous / next block of the chain: FX 1, FX 2, the amp, FX 3, FX 4, FX 5.
static void on_gk_nav(lv_event_t *e)
{
    static const int order[7] = { 0, 1, -1, -2, 2, 3, 4 };   // FX 1, FX 2, amp, cab, FX 3-5
    int step = (int)(intptr_t)lv_event_get_user_data(e), at = 2;
    int here = s_gk_kind == GK_FX ? s_gk_slot : s_gk_kind == GK_CAB ? -2 : -1;
    for (int i = 0; i < 7; i++) if (order[i] == here) at = i;
    int next = order[(at + step + 7) % 7];
    if (s_gk_kind == GK_FX) editor_leave();
    if (s_gk_kind == GK_CAB) send('y', 0);
    gk_hide();
    if (next == -1) { s_gil_selected = 2; gk_show_amp(); }
    else if (next == -2) { s_gil_selected = 3; gk_show_cab(); }
    else { s_gil_selected = next < 2 ? next : next + 2; gk_open_fx(next); }
    gil_fill();
}

static void on_gk_arc(lv_event_t *e)
{
    int c = (int)(intptr_t)lv_event_get_user_data(e);
    float n = lv_arc_get_value(s_gk_arc[c]) / 1000.0f;
    if (s_gk_kind == GK_AMP) {
        int pos = (int)lroundf(n * 255);
        level_changed(c < 4 ? AMP_ROW + c : 0, pos, true);
    } else if (s_gk_kind == GK_CAB && c < 3) {
        if (!lv_obj_has_state(s_level_slider[1 + c], LV_STATE_DISABLED))
            level_changed(1 + c, (int)lroundf(n * CAB_SLIDER_MAX), true);
    } else if (s_gk_kind == GK_FX && s_gk_map[c] >= 0) {
        int idx = s_gk_map[c];
        set_param_norm(idx, n);
        row_changed(idx, n);
    }
    gk_fill();
}

static void on_gk_cell(lv_event_t *e)   // a tap on a switch or an option button
{
    int c = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_gk_kind == GK_AMP) {
        const nano_state_t *st = s_gil_st;
        if (c == 5) { open_capture_picker(e); lv_obj_move_foreground(s_picker); }
        else if (c == 6) { open_cab_picker(e); lv_obj_move_foreground(s_picker); }
        else if (c == 7) {
            int cur = st ? st->capture_slot : 0;
            if (cur) s_gil_last_capture = cur;
            send('C', cur ? 0 : s_gil_last_capture);
        } else if (c == 8) open_expression(e);
        return;
    }
    if (s_gk_kind == GK_CAB) {
        const nano_state_t *st = s_gil_st;
        if (c == 5) { open_cab_picker(e); lv_obj_move_foreground(s_picker); }
        else if (c == 6) { open_capture_picker(e); lv_obj_move_foreground(s_picker); }
        else if (c == 7) {
            int cur = st ? st->cab_slot : 0;
            if (cur) s_gil_last_cab = cur;
            send('I', cur ? 0 : s_gil_last_cab);
        }
        return;
    }
    if (s_gk_kind != GK_FX) return;
    if (s_gk_map[c] == -2) { send('a' + s_gk_slot, 0); return; }
    int idx = s_gk_map[c];
    if (idx < 0) return;
    const nano_param_t *p = ed_param(idx);
    if (p->kind != NANO_PARAM_ENUM || p->option_count < 2) return;
    int i = option_index(p, s_param_norm[idx]);
    i = (i + 1) % p->option_count;
    float n = (float)i / (float)(p->option_count - 1);
    set_param_norm(idx, n);
    row_changed(idx, n);
    gk_fill();
}

static void gk_build(lv_obj_t *screen)
{
    s_gk = gil_box(screen, 0, 0, 800, 480, 0x0E1116);
    lv_obj_set_clickable(s_gk, true);   // nothing below reacts while it is up
    // header
    lv_obj_t *head = gil_box(s_gk, 0, 0, 800, GK_HEAD_H, 0x161A21);
    lv_obj_t *back = gil_button(head, 8, 9, 62, 54, on_gk_back, 0);
    lv_obj_t *bl = label(back, GF_T26, TEXT);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
    lv_obj_center(bl);
    lv_obj_t *change = gil_button(head, 80, 9, 430, 54, on_gk_change, 0);
    lv_obj_set_style_bg_opa(change, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(change, lv_color_hex(0x2A313C), 0);
    s_gk_title = label(change, GF_T26, GK_CYAN);
    lv_label_set_long_mode(s_gk_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_gk_title, 370);
    lv_obj_align(s_gk_title, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *caret = label(change, GF_T26, GK_CYAN);
    lv_label_set_text(caret, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(caret, &lv_font_montserrat_20, 0);
    lv_obj_align(caret, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_t *nav = gil_box(head, 520, 9, 172, 54, 0x2A313C);
    lv_obj_set_style_radius(nav, 10, 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = gil_button(nav, i ? 118 : 0, 0, 54, 54, on_gk_nav, i ? 1 : -1);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_t *l = label(b, GF_T26, TEXT);
        lv_label_set_text(l, i ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_center(l);
    }
    s_gk_chip = gil_box(nav, 54, 11, 64, 32, 0xF2E36B);
    lv_obj_set_style_radius(s_gk_chip, 6, 0);
    s_gk_chip_lbl = label(s_gk_chip, GF_T15, 0x1A1200);
    lv_obj_center(s_gk_chip_lbl);
    lv_obj_t *save = gil_button(head, 704, 9, 88, 54, on_gk_save, 0);
    lv_obj_t *sl = label(save, GF_T26, TEXT);
    lv_label_set_text(sl, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_20, 0);
    lv_obj_center(sl);
    gil_line(s_gk, 0, GK_HEAD_H, 800, 2);
    // cells
    const int cw = 160, ch = (GK_FOOT_Y - GK_HEAD_H - 2) / 2;
    for (int c = 0; c < GK_CELLS; c++) {
        int x = (c % 5) * cw, y = GK_HEAD_H + 2 + (c / 5) * ch;
        lv_obj_t *o = gil_box(s_gk, x, y, cw, ch, 0x161A21);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(0x2A313C), 0);
        lv_obj_set_style_border_side(o, LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM, 0);
        s_gk_cell[c] = o;
        s_gk_lbl[c] = label(o, GF_10, TEXT);
        lv_obj_set_style_text_letter_space(s_gk_lbl[c], 1, 0);
        lv_label_set_long_mode(s_gk_lbl[c], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_gk_lbl[c], cw - 20);
        lv_obj_set_pos(s_gk_lbl[c], 12, 10);
        // knob
        s_gk_val[c] = label(o, GF_T17, TEXT);
        lv_obj_align(s_gk_val[c], LV_ALIGN_LEFT_MID, 10, 14);
        lv_obj_t *arc = lv_arc_create(o);
        lv_obj_set_size(arc, 76, 76);
        lv_obj_align(arc, LV_ALIGN_RIGHT_MID, -8, 16);
        lv_arc_set_rotation(arc, 135);
        lv_arc_set_bg_angles(arc, 0, 270);
        lv_arc_set_range(arc, 0, 1000);
        lv_obj_set_style_arc_width(arc, SR(6), LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(0x2A313C), LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, SR(6), LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, lv_color_hex(GK_CYAN), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(arc, lv_color_hex(0xE8EBF0), LV_PART_KNOB);
        lv_obj_set_style_pad_all(arc, 3, LV_PART_KNOB);
        lv_obj_set_ext_click_area(arc, 14);
        lv_obj_add_event_cb(arc, on_gk_arc, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)c);
        s_gk_arc[c] = arc;
        // switch (two choices)
        lv_obj_t *sw = gil_box(o, 14, 52, 36, 76, 0x0E1116);
        lv_obj_set_style_radius(sw, 18, 0);
        lv_obj_set_style_border_width(sw, 3, 0);
        lv_obj_set_style_border_color(sw, lv_color_hex(0x3A414C), 0);
        lv_obj_set_clickable(sw, true);
        lv_obj_set_ext_click_area(sw, 20);
        lv_obj_add_event_cb(sw, on_gk_cell, LV_EVENT_CLICKED, (void *)(intptr_t)c);
        s_gk_sw[c] = sw;
        s_gk_sw_dot[c] = gil_box(sw, 0, 0, 24, 24, 0x0E1116);
        lv_obj_set_style_radius(s_gk_sw_dot[c], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_gk_sw_dot[c], 3, 0);
        lv_obj_set_style_border_color(s_gk_sw_dot[c], lv_color_hex(GK_CYAN), 0);
        for (int k = 0; k < 2; k++) {
            s_gk_opt[c][k] = label(o, GF_13, TEXT);
            lv_label_set_long_mode(s_gk_opt[c][k], LV_LABEL_LONG_DOT);
            lv_obj_set_width(s_gk_opt[c][k], cw - 68);
            lv_obj_set_pos(s_gk_opt[c][k], 62, k ? 98 : 62);
        }
        // option button / action button
        s_gk_btn[c] = gil_button(o, 10, 62, cw - 20, 60, on_gk_cell, c);
        s_gk_btn_lbl[c] = label(s_gk_btn[c], GF_T15, TEXT);
        lv_label_set_long_mode(s_gk_btn_lbl[c], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_gk_btn_lbl[c], cw - 44);
        lv_obj_center(s_gk_btn_lbl[c]);
        gk_cell_show(c, GKC_EMPTY, "");
    }
    // footer
    lv_obj_t *foot = gil_box(s_gk, 0, GK_FOOT_Y, 800, 480 - GK_FOOT_Y, 0x0E1116);
    s_gk_hint = label(foot, GF_11, MUTED);
    lv_label_set_long_mode(s_gk_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_gk_hint, 420);
    lv_obj_align(s_gk_hint, LV_ALIGN_LEFT_MID, 14, 0);
    set_text(s_gk_hint, "Muda na Nano na hora \xC2\xB7 \xE2\x9C\x93 salva o preset");
    s_gk_full = gil_button(foot, 440, 8, 150, 44, on_gk_full, 0);
    lv_obj_t *fl = label(s_gk_full, GF_11, TEXT);
    lv_label_set_text(fl, "Editor completo");
    lv_obj_center(fl);
    s_gk_page_btn = gil_button(foot, 598, 8, 86, 44, on_gk_page, 0);
    s_gk_page_lbl = label(s_gk_page_btn, GF_11, TEXT);
    lv_obj_center(s_gk_page_lbl);
    s_gk_dirty = label(foot, GF_10, GIL_AMBER);
    lv_label_set_text(s_gk_dirty, "N\xC3\x83O SALVO");
    lv_obj_set_style_bg_color(s_gk_dirty, lv_color_hex(0x2B2112), 0);
    lv_obj_set_style_bg_opa(s_gk_dirty, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_gk_dirty, 8, 0);
    lv_obj_set_style_pad_hor(s_gk_dirty, 8, 0);
    lv_obj_set_style_pad_ver(s_gk_dirty, 4, 0);
    lv_obj_align(s_gk_dirty, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_hidden(s_gk, true);
    lv_timer_create(gk_tick, 150, NULL);
}
