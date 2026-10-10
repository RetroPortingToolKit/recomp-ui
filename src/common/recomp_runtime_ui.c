#include "recomp_runtime_ui_internal.h"
#include "launcher_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int same_text(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    return strcmp(a, b) == 0;
}

int recomp_runtime_ui_item_enabled(const RecompRuntimeUi *ui,
                                   const RecompRuntimeUiItem *item) {
    if (!ui->config.callbacks.is_enabled) return 1;
    return ui->config.callbacks.is_enabled(ui->config.callbacks.context, item) != 0;
}

size_t recomp_runtime_ui_section_item_count(const RecompRuntimeUi *ui,
                                            size_t section) {
    size_t count = 0;
    if (!ui || section >= ui->section_count) return 0;
    for (size_t i = 0; i < ui->config.item_count; ++i)
        if (same_text(ui->config.items[i].section, ui->sections[section])) ++count;
    return count;
}

const RecompRuntimeUiItem *recomp_runtime_ui_section_item(
    const RecompRuntimeUi *ui, size_t section, size_t row) {
    size_t found = 0;
    if (!ui || section >= ui->section_count) return NULL;
    for (size_t i = 0; i < ui->config.item_count; ++i) {
        if (!same_text(ui->config.items[i].section, ui->sections[section])) continue;
        if (found++ == row) return &ui->config.items[i];
    }
    return NULL;
}

/* Every status write withdraws a pending confirmation: its prompt is gone. */
static void show_status(RecompRuntimeUi *ui, const char *text, unsigned frames) {
    snprintf(ui->status, sizeof(ui->status), "%s", text);
    ui->status_frames = ui->status[0] ? frames : 0;
    ui->confirm_item = NULL;
}

static void visibility(RecompRuntimeUi *ui, int open) {
    if (!ui || ui->open == open) return;
    ui->open = open;
    if (!open) ui->in_section = 0;
    if (ui->config.callbacks.visibility_changed)
        ui->config.callbacks.visibility_changed(ui->config.callbacks.context, open);
}

RecompRuntimeUi *recomp_runtime_ui_create(const RecompRuntimeUiConfig *config) {
    if (!config || !config->items || config->item_count == 0) return NULL;
    RecompRuntimeUi *ui = (RecompRuntimeUi *)calloc(1, sizeof(*ui));
    if (!ui) return NULL;
    ui->config = *config;
    ui->dim = RECOMP_RUNTIME_UI_DEFAULT_DIM;
    ui->opacity = 1.0f;
    ui->sections = (const char **)calloc(config->item_count, sizeof(*ui->sections));
    if (!ui->sections) {
        free(ui);
        return NULL;
    }
    for (size_t i = 0; i < config->item_count; ++i) {
        const char *section = config->items[i].section;
        if (!section || !*section) section = "Settings";
        int known = 0;
        for (size_t j = 0; j < ui->section_count; ++j)
            if (same_text(section, ui->sections[j])) known = 1;
        if (!known) ui->sections[ui->section_count++] = section;
    }
    return ui;
}

void recomp_runtime_ui_destroy(RecompRuntimeUi *ui) {
    if (!ui) return;
    if (ui->open && ui->config.callbacks.visibility_changed)
        ui->config.callbacks.visibility_changed(ui->config.callbacks.context, 0);
    free(ui->sections);
    free(ui->owned_items);
    free(ui->owned_view_choices);
    free(ui->owned_view_values);
    free(ui->owned_confirm_prompts);
    free(ui);
}

int recomp_runtime_ui_is_open(const RecompRuntimeUi *ui) {
    return ui && ui->open;
}

void recomp_runtime_ui_open(RecompRuntimeUi *ui) {
    if (!ui) return;
    ui->in_section = 0;
    ui->row_index = 0;
    visibility(ui, 1);
}

void recomp_runtime_ui_close(RecompRuntimeUi *ui) {
    /* Abandon any in-progress edit, so reopening never resumes a stale buffer
     * and wants_text_input cannot stay stuck on with the menu shut. A pending
     * "press again" goes too: reopening must not leave one press from quitting. */
    if (ui) {
        ui->editing_text = 0;
        if (ui->confirm_item) show_status(ui, "", 0);
    }
    visibility(ui, 0);
}

static size_t wrap_index(size_t current, int delta, size_t count) {
    if (count == 0) return 0;
    if (delta < 0) return current == 0 ? count - 1 : current - 1;
    return current + 1 >= count ? 0 : current + 1;
}

int recomp_runtime_ui_current_value(RecompRuntimeUi *ui,
                                    const RecompRuntimeUiItem *item,
                                    int *value) {
    if (!ui->config.callbacks.get_value || !value) return 0;
    return ui->config.callbacks.get_value(ui->config.callbacks.context,
                                          item, value) != 0;
}

static void accepted_change(RecompRuntimeUi *ui, const char *status) {
    show_status(ui, status ? status : "Applied", 90);
    if (ui->config.callbacks.save)
        ui->config.callbacks.save(ui->config.callbacks.context);
}

static void accepted_action(RecompRuntimeUi *ui) {
    show_status(ui, "Done", 90);
}

/* Items always come from config.items, which are owned_items when the
 * standard builder made the menu. */
static const char *owned_confirm_prompt(const RecompRuntimeUi *ui,
                                        const RecompRuntimeUiItem *item) {
    if (!ui->owned_confirm_prompts) return NULL;
    return ui->owned_confirm_prompts[item - ui->config.items];
}

void recomp_runtime_ui_adjust_current(RecompRuntimeUi *ui, int direction,
                                      int activate, int repeat) {
    const RecompRuntimeUiItem *item = recomp_runtime_ui_section_item(
        ui, ui->section_index, ui->row_index);
    if (!item || !recomp_runtime_ui_item_enabled(ui, item)) return;
    if (item->type == RECOMP_RUNTIME_UI_ACTION) {
        if (!activate || repeat || !ui->config.callbacks.run_action) return;
        const char *prompt = owned_confirm_prompt(ui, item);
        if (prompt && !recomp_runtime_ui_confirm(ui, item, prompt)) return;
        if (ui->config.callbacks.run_action(ui->config.callbacks.context, item))
            accepted_action(ui);
        return;
    }
    if (item->type == RECOMP_RUNTIME_UI_TEXT) {
        /* Activation opens the editor; the backend drives it from there. */
        if (!activate || repeat) return;
        recomp_runtime_ui_current_text(ui, item, ui->edit_buffer,
                                       sizeof(ui->edit_buffer));
        ui->editing_text = 1;
        return;
    }
    if (!ui->config.callbacks.set_value) return;
    int value = 0;
    if (!recomp_runtime_ui_current_value(ui, item, &value)) return;
    int next = value;
    if (item->type == RECOMP_RUNTIME_UI_BOOL) {
        if (repeat) return;
        next = !value;
    } else if (item->type == RECOMP_RUNTIME_UI_CHOICE) {
        int count = item->choice_count ? (int)item->choice_count
                                       : item->maximum - item->minimum + 1;
        if (count <= 0) return;
        int base = value - item->minimum;
        if (item->choice_values) {
            base = 0;
            for (int i = 0; i < count; ++i)
                if (item->choice_values[i] == value) { base = i; break; }
        }
        int delta = direction < 0 ? -1 : 1;
        base = (base + delta + count) % count;
        next = item->choice_values ? item->choice_values[base]
                                   : item->minimum + base;
    } else {
        int step = item->step > 0 ? item->step : 1;
        next = value + (direction < 0 ? -step : step);
        if (next < item->minimum) next = item->minimum;
        if (next > item->maximum) next = item->maximum;
    }
    if (next != value && ui->config.callbacks.set_value(
            ui->config.callbacks.context, item, next))
        accepted_change(ui, "Saved");
}

void recomp_runtime_ui_current_text(RecompRuntimeUi *ui,
                                    const RecompRuntimeUiItem *item, char *buf,
                                    size_t buf_size) {
    if (!buf || buf_size == 0) return;
    buf[0] = '\0';
    if (!ui || !item || !ui->config.callbacks.get_text) return;
    if (!ui->config.callbacks.get_text(ui->config.callbacks.context, item, buf,
                                       buf_size))
        buf[0] = '\0';
    buf[buf_size - 1] = '\0';
}

int recomp_runtime_ui_commit_text(RecompRuntimeUi *ui,
                                  const RecompRuntimeUiItem *item,
                                  const char *value) {
    if (!ui || !item || !value || !ui->config.callbacks.set_text) return 0;
    if (!ui->config.callbacks.set_text(ui->config.callbacks.context, item,
                                       value))
        return 0;
    accepted_change(ui, "Saved");
    return 1;
}

int recomp_runtime_ui_wants_text_input(const RecompRuntimeUi *ui) {
    return (ui && ui->open && ui->editing_text) ? 1 : 0;
}

static float clamp01(float v) {
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

void recomp_runtime_ui_set_backdrop(RecompRuntimeUi *ui, float dim, float opacity) {
    if (!ui) return;
    ui->dim = clamp01(dim);
    ui->opacity = clamp01(opacity);
}

void recomp_runtime_ui_set_status(RecompRuntimeUi *ui, const char *text) {
    if (!ui) return;
    show_status(ui, text ? text : "", 180);
}

int recomp_runtime_ui_confirm(RecompRuntimeUi *ui,
                              const RecompRuntimeUiItem *item,
                              const char *prompt) {
    if (!ui || !item) return 0;
    /* The prompt still showing is the window: status_frames runs down only
     * while a presentation draws the open menu. */
    if (ui->confirm_item && ui->status_frames &&
        same_text(ui->confirm_item->key, item->key)) {
        ui->confirm_item = NULL;
        return 1;
    }
    show_status(ui, prompt ? prompt : "Press again to confirm", 180);
    ui->confirm_item = item;
    return 0;
}

void recomp_runtime_ui_set_toast(RecompRuntimeUi *ui, const char *title, const char *body) {
    if (!ui) return;
    snprintf(ui->toast_title, sizeof(ui->toast_title), "%s", title ? title : "");
    snprintf(ui->toast_body, sizeof(ui->toast_body), "%s", body ? body : "");
}

int recomp_runtime_ui_toast_visible(const RecompRuntimeUi *ui) {
    return ui && (ui->toast_title[0] || ui->toast_body[0]);
}

void recomp_runtime_ui_enter_section(RecompRuntimeUi *ui, size_t section) {
    if (!ui || section >= ui->section_count) return;
    ui->section_index = section;
    ui->row_index = 0;
    ui->in_section = 1;
}

void recomp_runtime_ui_leave_section(RecompRuntimeUi *ui) {
    if (!ui) return;
    ui->in_section = 0;
    ui->row_index = 0;
}

int recomp_runtime_ui_handle_input(RecompRuntimeUi *ui,
                                   RecompRuntimeUiInput input,
                                   int pressed, int repeat) {
    if (!ui) return 0;
    if (!ui->open) {
        if (pressed && !repeat && input == RECOMP_RUNTIME_UI_INPUT_TOGGLE) {
            recomp_runtime_ui_open(ui);
            return 1;
        }
        return 0;
    }
    if (!pressed) return 1;
    if (input == RECOMP_RUNTIME_UI_INPUT_TOGGLE) {
        if (!repeat) recomp_runtime_ui_close(ui);
        return 1;
    }
    if (input == RECOMP_RUNTIME_UI_INPUT_BACK) {
        if (!repeat) {
            if (ui->in_section) {
                ui->in_section = 0;
                ui->row_index = 0;
            } else {
                recomp_runtime_ui_close(ui);
            }
        }
        return 1;
    }
    if (input == RECOMP_RUNTIME_UI_INPUT_UP ||
        input == RECOMP_RUNTIME_UI_INPUT_DOWN) {
        int delta = input == RECOMP_RUNTIME_UI_INPUT_UP ? -1 : 1;
        size_t count = ui->in_section ? recomp_runtime_ui_section_item_count(ui, ui->section_index)
                                      : ui->section_count;
        size_t *selection = ui->in_section ? &ui->row_index : &ui->section_index;
        *selection = wrap_index(*selection, delta, count);
        return 1;
    }
    if (input == RECOMP_RUNTIME_UI_INPUT_ACCEPT) {
        if (!ui->in_section) {
            if (!repeat) {
                recomp_runtime_ui_enter_section(ui, ui->section_index);
            }
        } else {
            recomp_runtime_ui_adjust_current(ui, 1, 1, repeat);
        }
        return 1;
    }
    if (input == RECOMP_RUNTIME_UI_INPUT_LEFT ||
        input == RECOMP_RUNTIME_UI_INPUT_RIGHT) {
        if (ui->in_section)
            recomp_runtime_ui_adjust_current(ui,
                           input == RECOMP_RUNTIME_UI_INPUT_LEFT ? -1 : 1,
                           0, repeat);
        return 1;
    }
    return 1;
}

#define GLYPH(a,b,c,d,e,f,g) \
    ((uint64_t)(a) | ((uint64_t)(b) << 5) | ((uint64_t)(c) << 10) | \
     ((uint64_t)(d) << 15) | ((uint64_t)(e) << 20) | \
     ((uint64_t)(f) << 25) | ((uint64_t)(g) << 30))

static uint64_t glyph_bits(unsigned char c) {
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
    switch (c) {
    case 'A': return GLYPH(14,17,17,31,17,17,17);
    case 'B': return GLYPH(30,17,17,30,17,17,30);
    case 'C': return GLYPH(14,17,16,16,16,17,14);
    case 'D': return GLYPH(30,17,17,17,17,17,30);
    case 'E': return GLYPH(31,16,16,30,16,16,31);
    case 'F': return GLYPH(31,16,16,30,16,16,16);
    case 'G': return GLYPH(14,17,16,23,17,17,15);
    case 'H': return GLYPH(17,17,17,31,17,17,17);
    case 'I': return GLYPH(14,4,4,4,4,4,14);
    case 'J': return GLYPH(7,2,2,2,18,18,12);
    case 'K': return GLYPH(17,18,20,24,20,18,17);
    case 'L': return GLYPH(16,16,16,16,16,16,31);
    case 'M': return GLYPH(17,27,21,21,17,17,17);
    case 'N': return GLYPH(17,25,21,19,17,17,17);
    case 'O': return GLYPH(14,17,17,17,17,17,14);
    case 'P': return GLYPH(30,17,17,30,16,16,16);
    case 'Q': return GLYPH(14,17,17,17,21,18,13);
    case 'R': return GLYPH(30,17,17,30,20,18,17);
    case 'S': return GLYPH(15,16,16,14,1,1,30);
    case 'T': return GLYPH(31,4,4,4,4,4,4);
    case 'U': return GLYPH(17,17,17,17,17,17,14);
    case 'V': return GLYPH(17,17,17,17,17,10,4);
    case 'W': return GLYPH(17,17,17,21,21,21,10);
    case 'X': return GLYPH(17,17,10,4,10,17,17);
    case 'Y': return GLYPH(17,17,10,4,4,4,4);
    case 'Z': return GLYPH(31,1,2,4,8,16,31);
    case '0': return GLYPH(14,17,19,21,25,17,14);
    case '1': return GLYPH(4,12,4,4,4,4,14);
    case '2': return GLYPH(14,17,1,2,4,8,31);
    case '3': return GLYPH(30,1,1,14,1,1,30);
    case '4': return GLYPH(2,6,10,18,31,2,2);
    case '5': return GLYPH(31,16,16,30,1,1,30);
    case '6': return GLYPH(14,16,16,30,17,17,14);
    case '7': return GLYPH(31,1,2,4,8,8,8);
    case '8': return GLYPH(14,17,17,14,17,17,14);
    case '9': return GLYPH(14,17,17,15,1,1,14);
    case '-': return GLYPH(0,0,0,31,0,0,0);
    case '+': return GLYPH(0,4,4,31,4,4,0);
    case '/': return GLYPH(1,2,2,4,8,8,16);
    case ':': return GLYPH(0,4,4,0,4,4,0);
    case '.': return GLYPH(0,0,0,0,0,6,6);
    case ',': return GLYPH(0,0,0,0,6,6,4);
    case '(': return GLYPH(2,4,8,8,8,4,2);
    case ')': return GLYPH(8,4,2,2,2,4,8);
    case '[': return GLYPH(14,8,8,8,8,8,14);
    case ']': return GLYPH(14,2,2,2,2,2,14);
    case '%': return GLYPH(17,2,4,4,8,16,17);
    case '<': return GLYPH(2,4,8,16,8,4,2);
    case '>': return GLYPH(8,4,2,1,2,4,8);
    case '=': return GLYPH(0,0,31,0,31,0,0);
    case '_': return GLYPH(0,0,0,0,0,0,31);
    case '!': return GLYPH(4,4,4,4,4,0,4);
    case '?': return GLYPH(14,17,1,2,4,0,4);
    case '#': return GLYPH(10,31,10,10,31,10,0);
    case '\'': return GLYPH(4,4,2,0,0,0,0);
    case ' ': return 0;
    default: return GLYPH(14,17,2,4,4,0,4);
    }
}

static uint32_t blend_pixel(uint32_t dst, uint32_t src, unsigned alpha) {
    unsigned dr = (dst >> 16) & 255, dg = (dst >> 8) & 255, db = dst & 255;
    unsigned sr = (src >> 16) & 255, sg = (src >> 8) & 255, sb = src & 255;
    unsigned inv = 255 - alpha;
    return 0xff000000u |
           (((sr * alpha + dr * inv) / 255) << 16) |
           (((sg * alpha + dg * inv) / 255) << 8) |
           ((sb * alpha + db * inv) / 255);
}

static void rect(void *pixels, int width, int height, int pitch, int x, int y,
                 int w, int h, uint32_t color, unsigned alpha) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width) w = width - x;
    if (y + h > height) h = height - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; ++yy) {
        uint32_t *row = (uint32_t *)((unsigned char *)pixels + yy * pitch);
        for (int xx = x; xx < x + w; ++xx)
            row[xx] = alpha == 255 ? (0xff000000u | color)
                                   : blend_pixel(row[xx], color, alpha);
    }
}

static void outline(void *pixels, int width, int height, int pitch, int x, int y,
                    int w, int h, int thickness, uint32_t color) {
    rect(pixels,width,height,pitch,x,y,w,thickness,color,255);
    rect(pixels,width,height,pitch,x,y+h-thickness,w,thickness,color,255);
    rect(pixels,width,height,pitch,x,y,thickness,h,color,255);
    rect(pixels,width,height,pitch,x+w-thickness,y,thickness,h,color,255);
}

static void draw_text(void *pixels, int width, int height, int pitch, int x,
                      int y, int scale, uint32_t color, const char *text,
                      int max_x) {
    if (!text) return;
    int cursor = x;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p >= 0x80) continue;
        if (cursor + 5 * scale > max_x) break;
        uint64_t bits = glyph_bits(*p);
        for (int gy = 0; gy < 7; ++gy) {
            unsigned row = (unsigned)((bits >> (gy * 5)) & 31);
            for (int gx = 0; gx < 5; ++gx)
                if (row & (1u << (4 - gx)))
                    rect(pixels,width,height,pitch,cursor+gx*scale,y+gy*scale,
                         scale,scale,color,255);
        }
        cursor += 6 * scale;
    }
}

static int text_width(const char *text, int scale) {
    int count = 0;
    if (!text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (*p < 0x80) ++count;
    return count * 6 * scale;
}

static void item_value(RecompRuntimeUi *ui, const RecompRuntimeUiItem *item,
                       char *out, size_t out_size) {
    if (item->type == RECOMP_RUNTIME_UI_ACTION) {
        snprintf(out, out_size, ">");
        return;
    }
    int value = 0;
    if (!recomp_runtime_ui_current_value(ui, item, &value)) {
        snprintf(out, out_size, "?");
    } else if (item->type == RECOMP_RUNTIME_UI_BOOL) {
        snprintf(out, out_size, "%s", value ? "On" : "Off");
    } else if (item->type == RECOMP_RUNTIME_UI_CHOICE && item->choices) {
        size_t index = item->choice_count;
        if (item->choice_values) {
            for (size_t i = 0; i < item->choice_count; ++i)
                if (item->choice_values[i] == value) { index = i; break; }
        } else if (value >= item->minimum &&
                   (size_t)(value - item->minimum) < item->choice_count) {
            index = (size_t)(value - item->minimum);
        }
        if (index < item->choice_count)
            snprintf(out, out_size, "%s", item->choices[index]);
        else
            snprintf(out, out_size, "?");
    } else {
        snprintf(out, out_size, "%d", value);
    }
}

/* The toast in the frame's top rows: a panel sized to its longest line. */
static void render_toast_argb(RecompRuntimeUi *ui, void *pixels, int width,
                              int height, int pitch, int scale) {
    LauncherTheme theme = launcher_theme_by_name(ui->config.theme);
    #define RGB8(c) (((uint32_t)((c).r * 255.0f) << 16) | \
                     ((uint32_t)((c).g * 255.0f) << 8) | \
                     (uint32_t)((c).b * 255.0f))
    const char *lines[8];
    int line_count = 0;
    char body[sizeof(ui->toast_body)];
    snprintf(body, sizeof(body), "%s", ui->toast_body);
    if (ui->toast_title[0]) lines[line_count++] = ui->toast_title;
    for (char *p = body; *p && line_count < 8;) {
        lines[line_count++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        p = nl + 1;
    }
    int widest = 0;
    for (int i = 0; i < line_count; ++i) {
        int w = text_width(lines[i], scale);
        if (w > widest) widest = w;
    }
    const int pad = 4 * scale, row = 9 * scale;
    int panel_w = widest + pad * 2;
    if (panel_w > width - 2 * scale) panel_w = width - 2 * scale;
    const int panel_h = line_count * row + pad * 2 - 2 * scale;
    const int x = (width - panel_w) / 2, y = 4 * scale;
    rect(pixels,width,height,pitch,x,y,panel_w,panel_h,RGB8(theme.panel),
         (unsigned)(230.0f * ui->opacity));
    outline(pixels,width,height,pitch,x,y,panel_w,panel_h,scale > 1 ? scale / 2 : 1,
            RGB8(theme.accent2));
    for (int i = 0; i < line_count; ++i)
        draw_text(pixels,width,height,pitch,x+pad,y+pad+i*row,scale,
                  i == 0 && ui->toast_title[0] ? RGB8(theme.accent2) : RGB8(theme.text),
                  lines[i],x+panel_w-pad);
    #undef RGB8
}

void recomp_runtime_ui_render_argb8888(RecompRuntimeUi *ui, void *pixels,
                                       int width, int height, int pitch) {
    if (!ui || !pixels || width <= 0 || height <= 0 || pitch < width * 4) return;
    if (recomp_runtime_ui_toast_visible(ui) && !ui->open) {
        int toast_scale = height / 224;
        render_toast_argb(ui, pixels, width, height, pitch, toast_scale < 1 ? 1 : toast_scale);
    }
    if (!ui->open) return;
    LauncherTheme theme = launcher_theme_by_name(ui->config.theme);
    int scale = height / 224;
    if (scale < 1) scale = 1;
    int margin = 6 * scale;
    int panel_w = 244 * scale;
    if (panel_w > width - margin * 2) panel_w = width - margin * 2;
    int panel_h = height - margin * 2;
    int panel_x = (width - panel_w) / 2;
    int panel_y = margin;
    int pad = 8 * scale;
    #define RGB8(c) (((uint32_t)((c).r * 255.0f) << 16) | \
                     ((uint32_t)((c).g * 255.0f) << 8) | \
                     (uint32_t)((c).b * 255.0f))
    const uint32_t white = RGB8(theme.text), muted = RGB8(theme.text_muted);
    const uint32_t accent = RGB8(theme.accent2), panel = RGB8(theme.panel);
    const uint32_t selected = RGB8(theme.control_hovered);
    const uint32_t disabled = RGB8(theme.text_muted);
    const uint32_t border = RGB8(theme.border);

    /* This presentation's own default dim is 130/255. */
    const float dim = 130.0f * ui->dim / RECOMP_RUNTIME_UI_DEFAULT_DIM;
    rect(pixels,width,height,pitch,0,0,width,height,0x000000,
         (unsigned)(dim > 255.0f ? 255.0f : dim));
    rect(pixels,width,height,pitch,panel_x,panel_y,panel_w,panel_h,panel,
         (unsigned)(238.0f * ui->opacity));
    outline(pixels,width,height,pitch,panel_x,panel_y,panel_w,panel_h,
            scale,accent);
    const char *title = ui->config.title ? ui->config.title : "Settings";
    const char *subtitle = ui->config.subtitle;
    const int title_w = text_width(title, scale);
    const int subtitle_w = text_width(subtitle, scale);
    const int header_inner_w = panel_w - pad * 2;
    const int stacked_header = subtitle && subtitle[0] &&
        title_w + subtitle_w + 4 * scale > header_inner_w;
    draw_text(pixels,width,height,pitch,panel_x+pad,panel_y+pad,scale,accent,
              title,panel_x+panel_w-pad);
    if (subtitle && subtitle[0]) {
        const int subtitle_y = panel_y + pad +
            (stacked_header ? 10 * scale : 0);
        int subtitle_x = panel_x + panel_w - pad - subtitle_w;
        if (subtitle_x < panel_x + pad) subtitle_x = panel_x + pad;
        draw_text(pixels,width,height,pitch,subtitle_x,
                  subtitle_y,scale,muted,subtitle,panel_x+panel_w-pad);
    }
    const int header_extra = stacked_header ? 10 * scale : 0;
    rect(pixels,width,height,pitch,panel_x+pad,
         panel_y+24*scale+header_extra,
         panel_w-pad*2,scale,border,255);

    int list_y = panel_y + 32 * scale + header_extra;
    int footer_y = panel_y + panel_h - 28 * scale;
    int row_h = 15 * scale;
    int visible = (footer_y - list_y) / row_h;
    if (visible < 1) visible = 1;
    size_t count = ui->in_section ? recomp_runtime_ui_section_item_count(ui, ui->section_index)
                                  : ui->section_count;
    size_t selection = ui->in_section ? ui->row_index : ui->section_index;
    size_t first = 0;
    if (selection >= (size_t)visible) first = selection - (size_t)visible + 1;
    for (int line = 0; line < visible && first + (size_t)line < count; ++line) {
        size_t index = first + (size_t)line;
        int y = list_y + line * row_h;
        if (index == selection)
            rect(pixels,width,height,pitch,panel_x+pad/2,y-3*scale,
                 panel_w-pad,row_h,selected,255);
        if (!ui->in_section) {
            const char *name = ui->sections[index];
            draw_text(pixels,width,height,pitch,panel_x+pad,y,scale,
                      index == selection ? white : muted,name,
                      panel_x+panel_w-pad-12*scale);
            draw_text(pixels,width,height,pitch,panel_x+panel_w-pad-5*scale,y,
                      scale,index == selection ? accent : muted,">",
                      panel_x+panel_w-pad);
        } else {
            const RecompRuntimeUiItem *item = recomp_runtime_ui_section_item(
                ui, ui->section_index, index);
            int enabled = recomp_runtime_ui_item_enabled(ui, item);
            uint32_t color = enabled ? (index == selection ? white : muted)
                                     : disabled;
            char value[96];
            item_value(ui, item, value, sizeof(value));
            int value_w = text_width(value, scale);
            draw_text(pixels,width,height,pitch,panel_x+pad,y,scale,color,
                      item->label,panel_x+panel_w-pad-value_w-8*scale);
            draw_text(pixels,width,height,pitch,panel_x+panel_w-pad-value_w,y,
                      scale,index == selection && enabled ? accent : color,
                      value,panel_x+panel_w-pad);
        }
    }

    const char *help = "UP/DOWN SELECT  LEFT/RIGHT CHANGE  ENTER ACCEPT";
    if (ui->in_section) {
        const RecompRuntimeUiItem *item = recomp_runtime_ui_section_item(
            ui, ui->section_index, ui->row_index);
        if (item && item->description && *item->description) help = item->description;
    }
    rect(pixels,width,height,pitch,panel_x+pad,footer_y-4*scale,
         panel_w-pad*2,scale,border,255);
    draw_text(pixels,width,height,pitch,panel_x+pad,footer_y+3*scale,scale,
              muted,help,panel_x+panel_w-pad);
    if (ui->status_frames) {
        int status_w = text_width(ui->status, scale);
        draw_text(pixels,width,height,pitch,panel_x+panel_w-pad-status_w,
                  panel_y+panel_h-12*scale,scale,accent,ui->status,
                  panel_x+panel_w-pad);
        --ui->status_frames;
    }
    #undef RGB8
    if (recomp_runtime_ui_toast_visible(ui))
        render_toast_argb(ui, pixels, width, height, pitch, scale);
}
