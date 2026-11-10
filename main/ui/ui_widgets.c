#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, font ? font : &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

lv_obj_t *ui_section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *l = ui_label(parent, title, &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_style_pad_top(l, 10, 0);
    lv_obj_set_style_pad_left(l, 6, 0);
    return l;
}

lv_obj_t *ui_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_width(c, lv_pct(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *row_base(lv_obj_t *parent, const char *icon, uint32_t icon_color, const char *text)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, lv_pct(100), 62);
    lv_obj_set_style_bg_color(r, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(UI_CARD2), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 16, 0);
    lv_obj_set_style_pad_hor(r, 14, 0);
    lv_obj_set_style_pad_ver(r, 0, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    if (icon) {
        lv_obj_t *ic = lv_obj_create(r);
        lv_obj_remove_style_all(ic);
        lv_obj_set_size(ic, 36, 36);
        lv_obj_set_style_radius(ic, 10, 0);
        lv_obj_set_style_bg_color(ic, lv_color_hex(icon_color), 0);
        lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
        lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *il = lv_label_create(ic);
        lv_label_set_text(il, icon);
        lv_obj_set_style_text_color(il, lv_color_white(), 0);
        lv_obj_set_style_text_font(il, &lv_font_montserrat_20, 0);
        lv_obj_center(il);
    }
    lv_obj_t *l = ui_label(r, text, &lv_font_montserrat_20, UI_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(l, 1);
    return r;
}

lv_obj_t *ui_row(lv_obj_t *parent, const char *icon, uint32_t icon_color, const char *text,
                 const char *value, lv_event_cb_t cb, void *user)
{
    lv_obj_t *r = row_base(parent, icon, icon_color, text);
    lv_obj_t *v = ui_label(r, value ? value : "", &lv_font_montserrat_16, UI_MUTED);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(v, 170, 0);
    lv_obj_set_width(v, LV_SIZE_CONTENT);
    lv_obj_set_name(v, "value");
    if (cb) {
        ui_label(r, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, 0x48484A);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, user);
    } else {
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    }
    return r;
}

void ui_row_set_value(lv_obj_t *row, const char *value)
{
    lv_obj_t *v = lv_obj_find_by_name(row, "value");
    if (v && strcmp(lv_label_get_text(v), value ? value : "") != 0) {   /* no redraw if unchanged */
        lv_label_set_text(v, value ? value : "");
    }
}

lv_obj_t *ui_row_switch(lv_obj_t *parent, const char *icon, uint32_t icon_color, const char *text,
                        bool on, lv_event_cb_t cb, void *user)
{
    lv_obj_t *r = row_base(parent, icon, icon_color, text);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 58, 32);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    if (cb) {
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user);
    }
    return sw;
}

lv_obj_t *ui_row_slider(lv_obj_t *parent, const char *icon, const char *text, int min, int max,
                        int val, lv_event_cb_t cb, void *user)
{
    lv_obj_t *c = ui_card(parent);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s  %s", icon ? icon : "", text);
    ui_label(c, buf, &lv_font_montserrat_18, UI_TEXT);
    lv_obj_t *s = lv_slider_create(c);
    lv_obj_set_width(s, lv_pct(94));
    lv_obj_set_height(s, 14);
    lv_obj_set_style_pad_hor(s, 10, 0);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s, 16);
    if (cb) {
        lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, user);
    }
    return s;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 56);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 28, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 24, 0);
    lv_obj_t *l = ui_label(b, text, &lv_font_montserrat_20, UI_TEXT);
    lv_obj_center(l);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    }
    return b;
}

lv_obj_t *ui_round_button(lv_obj_t *parent, const char *symbol, int size, uint32_t color,
                          lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = ui_label(b, symbol, size >= 80 ? &lv_font_montserrat_36 : &lv_font_montserrat_24, UI_TEXT);
    lv_obj_center(l);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    }
    return b;
}

/* ======================================================================== keyboard */

typedef struct {
    char         title[32];
    char         initial[128];
    bool         password;
    ui_text_cb_t done;
    void        *user;
    lv_obj_t    *ta;
} kb_ctx_t;

static void kb_event(lv_event_t *e)
{
    kb_ctx_t *k = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        char text[128];
        strlcpy(text, lv_textarea_get_text(k->ta), sizeof(text));
        ui_text_cb_t done = k->done;
        void *user = k->user;
        ui_page_pop();               /* frees k via on_close */
        if (done) {
            done(text, user);
        }
    } else if (code == LV_EVENT_CANCEL) {
        ui_page_pop();
    }
}

static void kb_show_cb(lv_event_t *e)
{
    kb_ctx_t *k = lv_event_get_user_data(e);
    bool show = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    lv_textarea_set_password_mode(k->ta, !show);
}

static void kb_build(lv_obj_t *c, void *ctx)
{
    kb_ctx_t *k = ctx;
    lv_obj_t *ta = lv_textarea_create(c);
    k->ta = ta;
    lv_obj_set_size(ta, UI_W - 32, 64);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 6);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, k->password);
    lv_textarea_set_text(ta, k->initial);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_border_color(ta, ui_accent(), LV_STATE_FOCUSED);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);

    if (k->password) {
        lv_obj_t *show = lv_checkbox_create(c);
        lv_checkbox_set_text(show, "Show");
        lv_obj_align(show, LV_ALIGN_TOP_RIGHT, -16, 78);
        lv_obj_add_event_cb(show, kb_show_cb, LV_EVENT_VALUE_CHANGED, k);
    }

    lv_obj_t *kb = lv_keyboard_create(c);
    lv_obj_set_size(kb, UI_W, 300);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_18, 0);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_READY, k);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_CANCEL, k);
}

static void kb_close(void *ctx)
{
    free(ctx);
}

void ui_keyboard(const char *title, const char *initial, bool password, ui_text_cb_t done, void *user)
{
    kb_ctx_t *k = calloc(1, sizeof(*k));
    if (!k) {
        return;
    }
    strlcpy(k->title, title, sizeof(k->title));
    strlcpy(k->initial, initial ? initial : "", sizeof(k->initial));
    k->password = password;
    k->done = done;
    k->user = user;
    ui_page_push_raw(k->title, kb_build, kb_close, k);
}

/* ======================================================================== confirm dialog */

typedef struct {
    ui_confirm_cb_t yes;
    void           *user;
    lv_obj_t       *ov;
} confirm_ctx_t;

static void confirm_btn(lv_event_t *e)
{
    confirm_ctx_t *c = lv_event_get_user_data(e);
    bool yes = lv_obj_get_user_data(lv_event_get_target(e)) != NULL;
    ui_confirm_cb_t cb = c->yes;
    void *user = c->user;
    lv_obj_delete(c->ov);
    free(c);
    if (yes && cb) {
        cb(user);
    }
}

void ui_confirm(const char *title, const char *msg, const char *yes_text, uint32_t yes_color,
                ui_confirm_cb_t yes, void *user)
{
    confirm_ctx_t *c = calloc(1, sizeof(*c));
    c->yes = yes;
    c->user = user;
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    c->ov = ov;
    lv_obj_add_flag(ov, LV_OBJ_FLAG_USER_4);     /* "dialog": closed when the screen sleeps */
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, UI_W, UI_H);
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_80, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *box = ui_card(ov);
    lv_obj_set_width(box, UI_W - 40);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 20, 0);
    lv_obj_set_style_pad_row(box, 12, 0);
    ui_label(box, title, &lv_font_montserrat_24, UI_TEXT);
    lv_obj_t *m = ui_label(box, msg, &lv_font_montserrat_18, 0xC7C7CC);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, lv_pct(100));
    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(row, 8, 0);
    lv_obj_t *no = ui_button(row, "Cancel", UI_CARD2, confirm_btn, c);
    lv_obj_set_user_data(no, NULL);
    lv_obj_t *ok = ui_button(row, yes_text ? yes_text : "OK", yes_color, confirm_btn, c);
    lv_obj_set_user_data(ok, (void *)1);
}

/* ======================================================================== choose one */

typedef struct {
    ui_choice_cb_t cb;
    void          *user;
    const char *const *options;
    int            count, current;
} choose_ctx_t;

static void choose_click(lv_event_t *e)
{
    choose_ctx_t *c = lv_event_get_user_data(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    ui_choice_cb_t cb = c->cb;
    void *user = c->user;
    ui_page_pop();
    if (cb) {
        cb(idx, user);
    }
}

static void choose_build(lv_obj_t *cont, void *ctx)
{
    choose_ctx_t *c = ctx;
    for (int i = 0; i < c->count; i++) {
        lv_obj_t *r = ui_row(cont, NULL, 0, c->options[i], i == c->current ? LV_SYMBOL_OK : "",
                             choose_click, c);
        lv_obj_set_user_data(r, (void *)(intptr_t)i);
    }
}

void ui_choose(const char *title, const char *const *options, int count, int current,
               ui_choice_cb_t cb, void *user)
{
    choose_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb;
    c->user = user;
    c->options = options;
    c->count = count;
    c->current = current;
    ui_page_push(title, choose_build, (page_fn_t)free, c);
}

/* ======================================================================== number picker */

typedef struct {
    ui_choice_cb_t cb;
    void          *user;
    int            min, max, val;
    lv_obj_t      *roller;
} num_ctx_t;

static void num_ok(lv_event_t *e)
{
    num_ctx_t *n = lv_event_get_user_data(e);
    int v = n->min + lv_roller_get_selected(n->roller);
    ui_choice_cb_t cb = n->cb;
    void *user = n->user;
    ui_page_pop();
    if (cb) {
        cb(v, user);
    }
}

static void num_build(lv_obj_t *c, void *ctx)
{
    num_ctx_t *n = ctx;
    size_t cap = (n->max - n->min + 1) * 6 + 1;
    char *opts = malloc(cap);
    size_t pos = 0;
    for (int i = n->min; i <= n->max; i++) {
        pos += snprintf(opts + pos, cap - pos, i == n->max ? "%02d" : "%02d\n", i);
    }
    lv_obj_t *r = lv_roller_create(c);
    n->roller = r;
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    free(opts);
    lv_roller_set_visible_row_count(r, 5);
    lv_obj_set_width(r, 200);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_32, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_bg_color(r, ui_accent(), LV_PART_SELECTED);
    lv_roller_set_selected(r, n->val - n->min, LV_ANIM_OFF);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_t *ok = ui_button(c, "Set", UI_BLUE, num_ok, n);
    lv_obj_set_width(ok, 240);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, -30);
}

void ui_number_picker(const char *title, int min, int max, int val, ui_choice_cb_t cb, void *user)
{
    num_ctx_t *n = calloc(1, sizeof(*n));
    n->cb = cb;
    n->user = user;
    n->min = min;
    n->max = max;
    n->val = val < min ? min : (val > max ? max : val);
    ui_page_push_raw(title, num_build, (page_fn_t)free, n);
}
