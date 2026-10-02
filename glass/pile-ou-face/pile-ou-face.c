#include "gm_plugin_extensions.h"
#include "gm_plugin_libc.h"
#include "gm_plugin_lvgl_api.h"

/* Standalone glasses app: a quick upward head motion (the IMU "head raise"
 * gesture) launches a coin flip. A real engraved coin (no text printed on
 * it - a star on one face, a cross-and-ring seal on the other) tosses up
 * past a row of conifers and falls back down, tumbling between its two
 * faces and its edge, then settles on PILE or FACE - announced in the
 * title text, picked via the Host's Random extension. The conifers are
 * sized and placed so the coin visibly climbs above their tops at the
 * peak of the throw and sinks back to their level on landing: they are
 * the fixed reference the eye uses to read the coin's height. */

#define SCREEN_MARGIN 20

/* Bitmaps are generated at runtime, sized proportionally to the real
 * display (queried in on_start), but every static buffer is allocated at
 * a fixed worst-case size so the plugin's static RAM footprint stays a
 * compile-time constant. */
#define COIN_SIZE_MIN 40U
#define COIN_SIZE_MAX 100U
#define EDGE_WIDTH_MIN 8U
#define EDGE_WIDTH_MAX 22U
#define TREE_BIG_H_MIN 70U
#define TREE_BIG_H_MAX 150U
#define TREE_BIG_W_MAX 56U
#define TREE_SMALL_H_MIN 16U
#define TREE_SMALL_H_MAX 112U
#define TREE_SMALL_W_MAX 40U
#define TREE_COUNT 4U

/* Spin ticks with growing intervals so the flip visibly decelerates before
 * landing, the way a real coin does. Sum ~= 1.67s of spin; the toss arc
 * below is timed to land exactly when the last tick completes. */
static const uint16_t FLIP_INTERVALS_MS[] = {
    90, 90, 100, 110, 130, 150, 180, 220, 270, 330,
};
#define FLIP_TICK_COUNT \
    (uint32_t)(sizeof(FLIP_INTERVALS_MS) / sizeof(FLIP_INTERVALS_MS[0]))
#define TOTAL_FLIP_MS 1670U

static const uint8_t TREE_X_PERCENT[TREE_COUNT] = {12U, 38U, 64U, 88U};

typedef enum {
    COIN_STATE_IDLE = 0,
    COIN_STATE_SPINNING = 1,
    COIN_STATE_RESULT = 2,
} coin_state_t;

typedef struct {
    const gm_plugin_host_api_t *host;
    const gm_plugin_lvgl_api_t *ui;
    const gm_plugin_libc_extension_api_t *libc;
    const gm_plugin_random_extension_api_t *random;
    gm_plugin_lvgl_obj_t *screen;
    gm_plugin_lvgl_obj_t *tree_image[TREE_COUNT];
    gm_plugin_lvgl_obj_t *title_label;
    gm_plugin_lvgl_obj_t *coin_image;
    gm_plugin_lvgl_obj_t *hint_label;
    coin_state_t state;
    uint32_t flip_tick;
    uint32_t flip_elapsed_ms;
    uint32_t total_flip_elapsed_ms;
    int32_t coin_size;
    int32_t flight_start_x;
    int32_t flight_end_x;
    int32_t flight_base_y;
    int16_t flight_amplitude;
    bool outcome_is_face;
} pile_ou_face_t;

static pile_ou_face_t pf;

#define number gm_plugin_lvgl_style_number
#define color gm_plugin_lvgl_style_color

/* --- Procedural indexed 4-bit bitmaps (16-shade grayscale palette, index 0
 * transparent). Two coin faces (a portrait and a "10E" value mark, both
 * with a reeded rim) plus one edge-on sliver are swapped via
 * image_set_source to sell the tumble; two conifer sizes give the tree row
 * a little depth. Row/stride math follows the exact packed-pixel layout
 * demonstrated in examples/image_animation. */
#define ROW_BYTES(w) (((uint32_t)(w) + 1U) / 2U)
#define IMG_DATA_SIZE(w, h) (64U + ROW_BYTES(w) * (uint32_t)(h))
#define IMG_STRIDE(w, h) ((IMG_DATA_SIZE(w, h) + 3U) & ~3U)

#define FACE_STRIDE IMG_STRIDE(COIN_SIZE_MAX, COIN_SIZE_MAX)
#define EDGE_STRIDE IMG_STRIDE(EDGE_WIDTH_MAX, COIN_SIZE_MAX)
#define TREE_BIG_STRIDE IMG_STRIDE(TREE_BIG_W_MAX, TREE_BIG_H_MAX)
#define TREE_SMALL_STRIDE IMG_STRIDE(TREE_SMALL_W_MAX, TREE_SMALL_H_MAX)

_Alignas(4) static uint8_t s_coin_face_data[FACE_STRIDE];
_Alignas(4) static uint8_t s_coin_pile_data[FACE_STRIDE];
_Alignas(4) static uint8_t s_coin_edge_data[EDGE_STRIDE];
_Alignas(4) static uint8_t s_tree_big_data[TREE_BIG_STRIDE];
_Alignas(4) static uint8_t s_tree_small_data[TREE_SMALL_STRIDE];
static gm_plugin_lvgl_image_dsc_t s_coin_face_desc;
static gm_plugin_lvgl_image_dsc_t s_coin_pile_desc;
static gm_plugin_lvgl_image_dsc_t s_coin_edge_desc;
static gm_plugin_lvgl_image_dsc_t s_tree_big_desc;
static gm_plugin_lvgl_image_dsc_t s_tree_small_desc;

static uint32_t clamp_u32(uint32_t value, uint32_t min_value, uint32_t max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static void set_pixel(uint8_t *data, uint16_t row_bytes, uint16_t x,
                       uint16_t y, uint8_t index)
{
    uint8_t *packed = &data[64U + (uint32_t)y * row_bytes + x / 2U];
    if ((x & 1U) == 0U)
        *packed = (uint8_t)((*packed & 0x0fU) | (uint8_t)(index << 4));
    else
        *packed = (uint8_t)((*packed & 0xf0U) | (index & 0x0fU));
}

static void build_palette(uint8_t *data, uint32_t data_size)
{
    uint8_t shade;
    uint32_t index;
    for (index = 0U; index < data_size; ++index) data[index] = 0U;
    for (shade = 1U; shade < 16U; ++shade) {
        const uint8_t brightness = (uint8_t)(shade * 17U);
        data[(uint32_t)shade * 4U] = brightness;
        data[(uint32_t)shade * 4U + 1U] = brightness;
        data[(uint32_t)shade * 4U + 2U] = brightness;
        data[(uint32_t)shade * 4U + 3U] = 255U;
    }
}

static void fill_image_desc(gm_plugin_lvgl_image_dsc_t *desc,
                             const uint8_t *data, uint32_t width,
                             uint32_t height)
{
    desc->struct_size = (uint16_t)sizeof(gm_plugin_lvgl_image_dsc_t);
    desc->width = (uint16_t)width;
    desc->height = (uint16_t)height;
    desc->format = GM_PLUGIN_LVGL_IMAGE_INDEXED_4BIT;
    desc->reserved = 0U;
    desc->data = data;
    desc->data_size = IMG_DATA_SIZE(width, height);
}

/* Buckets the point (dx,dy) into one of 16 equal 22.5-degree angular
 * sectors with no trigonometry: the 8 octants fall out of the signs of
 * dx/dy plus whether |dy|>|dx|, and each octant is bisected by comparing
 * against the fixed-point ratio tan(22.5 deg) ~= 0.414 (424/1024). Used to
 * draw the reeded rim as pure integer math. */
static uint8_t angle_sector16(int32_t dx, int32_t dy)
{
    uint32_t adx = (uint32_t)(dx < 0 ? -dx : dx);
    uint32_t ady = (uint32_t)(dy < 0 ? -dy : dy);
    bool steep = ady > adx;
    uint32_t lo = steep ? adx : ady;
    uint32_t hi = steep ? ady : adx;
    bool half = (lo * 1024U) > (hi * 424U);
    uint8_t octant;

    if (dx >= 0 && dy >= 0)
        octant = steep ? 1U : 0U;
    else if (dx < 0 && dy >= 0)
        octant = steep ? 2U : 3U;
    else if (dx < 0 && dy < 0)
        octant = steep ? 5U : 4U;
    else
        octant = steep ? 6U : 7U;

    return (uint8_t)(octant * 2U + (half ? 1U : 0U));
}

typedef enum { COIN_EMBLEM_PORTRAIT = 0, COIN_EMBLEM_VALUE = 1 } coin_emblem_t;

static void fill_circle(uint8_t *data, uint16_t row_bytes, uint16_t size,
                         int32_t ccx, int32_t ccy, int32_t r, uint8_t shade)
{
    int32_t x, y;
    const int32_t r_sq = r * r;
    for (y = ccy - r; y <= ccy + r; ++y) {
        if (y < 0 || y >= size) continue;
        for (x = ccx - r; x <= ccx + r; ++x) {
            const int32_t dx = x - ccx;
            const int32_t dy = y - ccy;
            if (x < 0 || x >= size || dx * dx + dy * dy > r_sq) continue;
            set_pixel(data, row_bytes, (uint16_t)x, (uint16_t)y, shade);
        }
    }
}

static void fill_rect(uint8_t *data, uint16_t row_bytes, uint16_t size,
                       int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       uint8_t shade)
{
    int32_t x, y;
    for (y = y0; y <= y1; ++y) {
        if (y < 0 || y >= size) continue;
        for (x = x0; x <= x1; ++x) {
            if (x < 0 || x >= size) continue;
            set_pixel(data, row_bytes, (uint16_t)x, (uint16_t)y, shade);
        }
    }
}

/* A plain frontal portrait silhouette (head, hairline, eyes, nose, mouth)
 * stamped in the middle of the "Face" side, built entirely from circles
 * and rectangles already used elsewhere in this file. */
static void draw_portrait(uint8_t *data, uint16_t row_bytes, uint16_t size,
                           int32_t cx, int32_t cy, int32_t radius)
{
    const int32_t head_r = radius * 56 / 100;
    const int32_t hair_y = cy - head_r + head_r * 32 / 100;
    const int32_t eye_r = radius * 6 / 100 > 1 ? radius * 6 / 100 : 2;
    const int32_t eye_off_x = radius * 22 / 100;
    const int32_t eye_off_y = radius * 6 / 100;
    const int32_t nose_half_w = radius * 5 / 100 > 1 ? radius * 5 / 100 : 2;
    const int32_t nose_top = cy + radius * 2 / 100;
    const int32_t nose_bottom = cy + radius * 22 / 100;
    const int32_t mouth_half_w = radius * 15 / 100;
    const int32_t mouth_top = cy + radius * 32 / 100;
    const int32_t mouth_bottom = mouth_top + (radius * 6 / 100 > 1 ? radius * 6 / 100 : 2);
    int32_t x, y;

    for (y = cy - head_r; y <= cy + head_r; ++y) {
        if (y < 0 || y >= size) continue;
        for (x = cx - head_r; x <= cx + head_r; ++x) {
            const int32_t dx = x - cx;
            const int32_t dy = y - cy;
            if (x < 0 || x >= size || dx * dx + dy * dy > head_r * head_r) continue;
            set_pixel(data, row_bytes, (uint16_t)x, (uint16_t)y, (y < hair_y) ? 5U : 13U);
        }
    }
    fill_circle(data, row_bytes, size, cx - eye_off_x, cy - eye_off_y, eye_r, 3U);
    fill_circle(data, row_bytes, size, cx + eye_off_x, cy - eye_off_y, eye_r, 3U);
    fill_rect(data, row_bytes, size, cx - nose_half_w, nose_top, cx + nose_half_w, nose_bottom, 10U);
    fill_rect(data, row_bytes, size, cx - mouth_half_w, mouth_top, cx + mouth_half_w, mouth_bottom, 4U);
}

#define GLYPH_COLS 5
#define GLYPH_ROWS 7

static const uint8_t GLYPH_DIGIT_1[GLYPH_ROWS] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
static const uint8_t GLYPH_DIGIT_0[GLYPH_ROWS] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
static const uint8_t GLYPH_EURO[GLYPH_ROWS] = {0x06, 0x08, 0x1F, 0x08, 0x1F, 0x08, 0x06};

/* Blits a hardcoded 5x7 bitmap glyph (one bit per pixel, MSB-first per row)
 * at (origin_x, origin_y), each source pixel scaled up to a `scale`-wide
 * square - a tiny pixel font, no font rasterizer needed. */
static void blit_glyph(uint8_t *data, uint16_t row_bytes, uint16_t size,
                        const uint8_t *glyph, int32_t origin_x, int32_t origin_y,
                        int32_t scale, uint8_t shade)
{
    int32_t row, col;
    for (row = 0; row < GLYPH_ROWS; ++row) {
        for (col = 0; col < GLYPH_COLS; ++col) {
            if ((glyph[row] & (1U << (GLYPH_COLS - 1 - col))) == 0U) continue;
            fill_rect(data, row_bytes, size,
                      origin_x + col * scale, origin_y + row * scale,
                      origin_x + col * scale + scale - 1, origin_y + row * scale + scale - 1,
                      shade);
        }
    }
}

/* Stamps "10E" (the euro sign drawn as a barred C) centered on the "Pile"
 * side, mimicking a real coin's engraved face value. */
static void draw_value_mark(uint8_t *data, uint16_t row_bytes, uint16_t size,
                             int32_t cx, int32_t cy, int32_t radius)
{
    const int32_t scale = radius / 11 > 1 ? radius / 11 : 1;
    const int32_t glyph_w = GLYPH_COLS * scale;
    const int32_t glyph_h = GLYPH_ROWS * scale;
    const int32_t gap = scale;
    const int32_t total_w = glyph_w * 3 + gap * 2;
    const int32_t origin_x = cx - total_w / 2;
    const int32_t origin_y = cy - glyph_h / 2;

    blit_glyph(data, row_bytes, size, GLYPH_DIGIT_1, origin_x, origin_y, scale, 14U);
    blit_glyph(data, row_bytes, size, GLYPH_DIGIT_0, origin_x + glyph_w + gap, origin_y, scale, 14U);
    blit_glyph(data, row_bytes, size, GLYPH_EURO, origin_x + 2 * (glyph_w + gap), origin_y, scale, 14U);
}

/* A round coin: reeded rim (16 alternating ridge wedges) and a two-band
 * shaded field, stamped afterward with either a portrait (FACE) or a
 * "10E" value mark (PILE) - mimicking a real coin's two distinct faces.
 * Anything outside the disc stays on palette index 0 (transparent). */
static void generate_coin_face(uint8_t *data, uint16_t size,
                                coin_emblem_t emblem)
{
    const uint16_t row_bytes = (uint16_t)ROW_BYTES(size);
    const uint32_t data_size = IMG_DATA_SIZE(size, size);
    const int32_t cx = size / 2;
    const int32_t cy = size / 2;
    const int32_t radius = size / 2 - 2;
    const int32_t radius_sq = radius * radius;
    const int32_t rim_inner = radius - (radius * 10 / 100 + 3);
    const int32_t rim_inner_sq = rim_inner * rim_inner;
    int32_t x, y;

    build_palette(data, data_size);
    for (y = 0; y < size; ++y) {
        for (x = 0; x < size; ++x) {
            const int32_t dx = x - cx;
            const int32_t dy = y - cy;
            const int32_t dist_sq = dx * dx + dy * dy;
            uint8_t shade;

            if (dist_sq > radius_sq) continue;

            if (dist_sq >= rim_inner_sq) {
                shade = (angle_sector16(dx, dy) & 1U) ? 12U : 7U;
            } else if (((dist_sq * 100) / radius_sq) >= 55) {
                shade = 6U;
            } else {
                shade = 9U;
            }
            set_pixel(data, row_bytes, (uint16_t)x, (uint16_t)y, shade);
        }
    }

    if (emblem == COIN_EMBLEM_PORTRAIT)
        draw_portrait(data, row_bytes, size, cx, cy, radius);
    else
        draw_value_mark(data, row_bytes, size, cx, cy, radius);
}

/* A rounded vertical pill with alternating narrow stripes simulates the
 * fluted, light-catching edge of a coin seen side-on. */
static void generate_coin_edge(uint8_t *data, uint16_t width, uint16_t height)
{
    const uint16_t row_bytes = (uint16_t)ROW_BYTES(width);
    const uint32_t data_size = IMG_DATA_SIZE(width, height);
    const int32_t half_w = width / 2;
    const int32_t corner_r = half_w;
    const int32_t corner_r_sq = corner_r * corner_r;
    int32_t x, y;

    build_palette(data, data_size);
    for (y = 0; y < height; ++y) {
        int32_t cap_cy = -1;
        if (y < corner_r)
            cap_cy = corner_r;
        else if (y >= height - corner_r)
            cap_cy = height - corner_r - 1;
        for (x = 0; x < width; ++x) {
            int32_t dx_abs;
            uint8_t shade;
            if (cap_cy >= 0) {
                const int32_t dx = x - half_w;
                const int32_t dy = y - cap_cy;
                if ((dx * dx + dy * dy) > corner_r_sq) continue;
            }
            dx_abs = x - half_w;
            if (dx_abs < 0) dx_abs = -dx_abs;
            shade = ((x / 2) & 1U) ? 11U : 7U;
            if (half_w > 0 && dx_abs > half_w * 70 / 100) shade = 5U;
            set_pixel(data, row_bytes, (uint16_t)x, (uint16_t)y, shade);
        }
    }
}

/* A conifer: a narrow dark trunk topped by three overlapping triangular
 * tiers, each wider than the one above - a classic pine silhouette built
 * entirely from per-row linear width interpolation. */
static void generate_tree(uint8_t *data, uint16_t width, uint16_t height)
{
    const uint16_t row_bytes = (uint16_t)ROW_BYTES(width);
    const uint32_t data_size = IMG_DATA_SIZE(width, height);
    const int32_t trunk_h = (int32_t)height * 15 / 100;
    int32_t trunk_w = (int32_t)width * 22 / 100;
    const int32_t canopy_h = (int32_t)height - trunk_h;
    const int32_t tier_count = 3;
    const int32_t tier_h = canopy_h / tier_count;
    const int32_t cx = width / 2;
    int32_t x, y, tier;

    if (trunk_w < 2) trunk_w = 2;
    build_palette(data, data_size);

    for (y = height - trunk_h; y < height; ++y) {
        for (x = cx - trunk_w / 2; x < cx + trunk_w / 2; ++x) {
            if (x < 0 || x >= width) continue;
            set_pixel(data, row_bytes, (uint16_t)x, (uint16_t)y, 5U);
        }
    }

    for (tier = 0; tier < tier_count; ++tier) {
        const int32_t tier_top = tier * tier_h * 85 / 100;
        const int32_t tier_bottom = tier_top + tier_h;
        const int32_t base_half_width = (width / 2) * (tier + 1) / tier_count;
        const uint8_t shade = (uint8_t)(6U + tier);

        for (y = tier_top; y < tier_bottom && y < canopy_h; ++y) {
            const int32_t half_width =
                base_half_width * (y - tier_top) / (tier_h > 0 ? tier_h : 1);
            for (x = cx - half_width; x <= cx + half_width; ++x) {
                if (x < 0 || x >= width) continue;
                set_pixel(data, row_bytes, (uint16_t)x, (uint16_t)y, shade);
            }
        }
    }
}

static void set_style(gm_plugin_lvgl_obj_t *object,
                       gm_plugin_lvgl_style_prop_t property,
                       gm_plugin_lvgl_style_value_t value)
{
    pf.ui->style_set(object, property, value, GM_PLUGIN_LVGL_SELECTOR_MAIN);
}

static gm_plugin_lvgl_obj_t *make_label(gm_plugin_lvgl_obj_t *parent,
                                         const char *text, uint8_t shade)
{
    gm_plugin_lvgl_obj_t *label = pf.ui->label_create(parent);
    if (label == 0) return 0;
    pf.ui->label_set_text(label, text);
    pf.ui->label_set_long_mode(label, GM_PLUGIN_LVGL_LABEL_WRAP);
    set_style(label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(shade));
    set_style(label, GM_PLUGIN_LVGL_STYLE_TEXT_ALIGN,
              number(GM_PLUGIN_LVGL_TEXT_ALIGN_CENTER));
    return label;
}

static gm_plugin_result_t create_trees(gm_plugin_lvgl_obj_t *parent,
                                        int32_t width, int32_t ground_y)
{
    uint32_t i;
    for (i = 0U; i < TREE_COUNT; ++i) {
        const bool big = (i % 2U) == 0U;
        const gm_plugin_lvgl_image_dsc_t *desc =
            big ? &s_tree_big_desc : &s_tree_small_desc;
        const int32_t cx = (width * TREE_X_PERCENT[i]) / 100;

        pf.tree_image[i] = pf.ui->image_create(parent, desc);
        if (pf.tree_image[i] == 0) return GM_PLUGIN_ENOMEM;
        pf.ui->obj_set_pos(pf.tree_image[i], cx - desc->width / 2,
                            ground_y - desc->height);
    }
    return GM_PLUGIN_OK;
}

static void position_coin(int32_t center_x, int32_t center_y)
{
    pf.ui->obj_set_pos(pf.coin_image, center_x - pf.coin_size / 2,
                        center_y - pf.coin_size / 2);
}

static void show_idle(void)
{
    pf.ui->label_set_text(pf.title_label, "Pile ou Face");
    pf.ui->label_set_text(pf.hint_label, "Coup de tete vers le haut pour lancer");
    pf.ui->image_set_source(pf.coin_image, &s_coin_face_desc);
    position_coin(pf.flight_start_x, pf.flight_base_y);
}

static void show_spin_tick(void)
{
    const gm_plugin_lvgl_image_dsc_t *frame;
    switch (pf.flip_tick % 4U) {
    case 0U: frame = &s_coin_face_desc; break;
    case 2U: frame = &s_coin_pile_desc; break;
    default: frame = &s_coin_edge_desc; break;
    }
    pf.ui->label_set_text(pf.title_label, "Ca tourne...");
    pf.ui->label_set_text(pf.hint_label, "");
    pf.ui->image_set_source(pf.coin_image, frame);
}

static void show_result(void)
{
    pf.ui->label_set_text(pf.title_label,
                           pf.outcome_is_face ? "FACE !" : "PILE !");
    pf.ui->label_set_text(pf.hint_label, "");
    pf.ui->image_set_source(pf.coin_image,
                             pf.outcome_is_face ? &s_coin_face_desc : &s_coin_pile_desc);
    position_coin(pf.flight_end_x, pf.flight_base_y);
}

/* Circular-looking toss: x glides linearly from the bottom-right start to
 * the bottom-left landing spot while y rises and falls along a parabola
 * (4*p*(1-p), peaking at progress p=0.5 and 0 at both ends) - together they
 * trace an arc from one bottom corner to the other. Integer fixed-point
 * (progress in per-mille) avoids any libm dependency, and every term stays
 * well inside int32_t so no 64-bit div/mod runtime helpers are pulled in by
 * this freestanding link. */
static void update_coin_flight(void)
{
    int32_t progress = (int32_t)(pf.total_flip_elapsed_ms * 1000U / TOTAL_FLIP_MS);
    int32_t arc, x, y;
    if (progress > 1000) progress = 1000;
    arc = 4 * progress * (1000 - progress);
    x = pf.flight_start_x +
        (pf.flight_end_x - pf.flight_start_x) * progress / 1000;
    y = pf.flight_base_y -
        ((int32_t)pf.flight_amplitude * arc / (1000 * 1000));
    position_coin(x, y);
}

static void start_flip(void)
{
    uint32_t roll = pf.random->get_u32();
    pf.outcome_is_face = (roll & 1U) != 0U;
    pf.state = COIN_STATE_SPINNING;
    pf.flip_tick = 0U;
    pf.flip_elapsed_ms = 0U;
    pf.total_flip_elapsed_ms = 0U;
    show_spin_tick();
    update_coin_flight();
}

static void advance_flip(uint32_t elapsed_ms)
{
    pf.flip_elapsed_ms += elapsed_ms;
    while (pf.flip_tick < FLIP_TICK_COUNT &&
           pf.flip_elapsed_ms >= FLIP_INTERVALS_MS[pf.flip_tick]) {
        pf.flip_elapsed_ms -= FLIP_INTERVALS_MS[pf.flip_tick];
        pf.flip_tick += 1U;
        if (pf.flip_tick >= FLIP_TICK_COUNT) {
            pf.state = COIN_STATE_RESULT;
            show_result();
            return;
        }
        show_spin_tick();
    }
}

static gm_plugin_result_t pile_ou_face_start(void *context)
{
    gm_plugin_display_info_t display;
    gm_plugin_lvgl_obj_t *root;
    gm_plugin_result_t result;
    uint32_t coin_size, edge_width, tree_big_h, tree_big_w, tree_small_h,
        tree_small_w;
    int32_t ground_y;
    (void)context;

    if (pf.host->display_get_info(&display) != GM_PLUGIN_OK ||
        display.width < 140U || display.height < 140U)
        return GM_PLUGIN_ENOTSUP;

    /* Every proportion below is derived from the real screen size so the
     * scene reads correctly regardless of the device's actual resolution:
     * the coin is sized relative to screen height, the conifers tall
     * enough to serve as a height reference, and the flight amplitude
     * generous enough to clearly clear their tops. */
    coin_size = clamp_u32(display.height * 28U / 100U, COIN_SIZE_MIN, COIN_SIZE_MAX);
    edge_width = clamp_u32(coin_size * 20U / 100U, EDGE_WIDTH_MIN, EDGE_WIDTH_MAX);
    tree_big_h = clamp_u32(display.height * 55U / 100U, TREE_BIG_H_MIN, TREE_BIG_H_MAX);
    tree_big_w = clamp_u32(tree_big_h * 37U / 100U, 20U, TREE_BIG_W_MAX);
    tree_small_h = clamp_u32(tree_big_h * 74U / 100U, TREE_SMALL_H_MIN, TREE_SMALL_H_MAX);
    tree_small_w = clamp_u32(tree_small_h * 36U / 100U, 14U, TREE_SMALL_W_MAX);

    /* The toss arcs from the bottom-right corner to the bottom-left corner:
     * x glides linearly between the two, y rises along the parabola above
     * a shared ground line (see update_coin_flight). The amplitude is set
     * so the peak (mid-arc) puts the coin right at the top of the screen,
     * leaving only enough clearance for its own radius. */
    ground_y = (int32_t)display.height - SCREEN_MARGIN;
    pf.coin_size = (int32_t)coin_size;
    pf.flight_start_x = (int32_t)display.width - SCREEN_MARGIN - pf.coin_size / 2;
    pf.flight_end_x = SCREEN_MARGIN + pf.coin_size / 2;
    pf.flight_base_y = ground_y - pf.coin_size / 2;
    pf.flight_amplitude = (int16_t)clamp_u32(
        (uint32_t)pf.flight_base_y - (uint32_t)SCREEN_MARGIN - (uint32_t)pf.coin_size / 2U,
        20U, 30000U);

    generate_coin_face(s_coin_face_data, (uint16_t)coin_size, COIN_EMBLEM_PORTRAIT);
    generate_coin_face(s_coin_pile_data, (uint16_t)coin_size, COIN_EMBLEM_VALUE);
    generate_coin_edge(s_coin_edge_data, (uint16_t)edge_width, (uint16_t)coin_size);
    generate_tree(s_tree_big_data, (uint16_t)tree_big_w, (uint16_t)tree_big_h);
    generate_tree(s_tree_small_data, (uint16_t)tree_small_w, (uint16_t)tree_small_h);

    fill_image_desc(&s_coin_face_desc, s_coin_face_data, coin_size, coin_size);
    fill_image_desc(&s_coin_pile_desc, s_coin_pile_data, coin_size, coin_size);
    fill_image_desc(&s_coin_edge_desc, s_coin_edge_data, edge_width, coin_size);
    fill_image_desc(&s_tree_big_desc, s_tree_big_data, tree_big_w, tree_big_h);
    fill_image_desc(&s_tree_small_desc, s_tree_small_data, tree_small_w,
                     tree_small_h);

    root = pf.ui->root_get();
    if (root == 0) return GM_PLUGIN_ESTATE;
    pf.ui->obj_clean(root);

    pf.screen = pf.ui->obj_create(root);
    if (pf.screen == 0) return GM_PLUGIN_ENOMEM;
    pf.ui->obj_set_size(pf.screen, display.width, display.height);
    pf.ui->obj_align(pf.screen, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 0);
    set_style(pf.screen, GM_PLUGIN_LVGL_STYLE_BG_COLOR, color(0x00));
    set_style(pf.screen, GM_PLUGIN_LVGL_STYLE_BG_OPA, number(255));
    pf.ui->obj_clear_flag(pf.screen, GM_PLUGIN_LVGL_FLAG_SCROLLABLE);

    /* Trees are created first so they sit behind the coin and the text. */
    if (create_trees(pf.screen, (int32_t)display.width, ground_y) != GM_PLUGIN_OK)
        goto no_memory;

    pf.title_label = make_label(pf.screen, "Pile ou Face", 0x90);
    pf.coin_image = pf.ui->image_create(pf.screen, &s_coin_face_desc);
    pf.hint_label = make_label(pf.screen, "", 0x70);
    if (pf.title_label == 0 || pf.coin_image == 0 || pf.hint_label == 0)
        goto no_memory;

    pf.ui->obj_set_size(pf.title_label, display.width - SCREEN_MARGIN * 2U, 28);
    pf.ui->obj_align(pf.title_label, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0, 14);

    pf.ui->obj_set_size(pf.hint_label, display.width - SCREEN_MARGIN * 2U, 32);
    pf.ui->obj_align(pf.hint_label, GM_PLUGIN_LVGL_ALIGN_BOTTOM_MID, 0, -16);

    pf.state = COIN_STATE_IDLE;
    show_idle();

    result = pf.host->imu_enable(GM_PLUGIN_IMU_ENABLE_GESTURES);
    if (result != GM_PLUGIN_OK) goto no_memory;
    return GM_PLUGIN_OK;

no_memory:
    pf.ui->obj_clean(root);
    pf.screen = 0;
    pf.title_label = 0;
    pf.coin_image = 0;
    pf.hint_label = 0;
    return GM_PLUGIN_ENOMEM;
}

static void pile_ou_face_loop(void *context, uint32_t elapsed_ms)
{
    (void)context;
    if (pf.state != COIN_STATE_SPINNING) return;
    pf.total_flip_elapsed_ms += elapsed_ms;
    if (pf.total_flip_elapsed_ms > TOTAL_FLIP_MS)
        pf.total_flip_elapsed_ms = TOTAL_FLIP_MS;
    update_coin_flight();
    advance_flip(elapsed_ms);
}

static bool pile_ou_face_event(void *context, const gm_plugin_event_t *event)
{
    (void)context;
    if (event == 0 || event->type != GM_PLUGIN_EVENT_IMU_GESTURE) return false;
    if (event->data.imu_gesture.gesture != GM_PLUGIN_IMU_GESTURE_HEAD_RAISE ||
        !event->data.imu_gesture.active)
        return false;
    if (pf.state == COIN_STATE_SPINNING) return false;

    start_flip();
    return true;
}

static void pile_ou_face_stop(void *context)
{
    (void)context;
    gm_plugin_lvgl_obj_t *root = pf.ui->root_get();
    (void)pf.host->imu_enable(GM_PLUGIN_IMU_ENABLE_NONE);
    if (root != 0) pf.ui->obj_clean(root);
    pf.screen = 0;
    pf.title_label = 0;
    pf.coin_image = 0;
    pf.hint_label = 0;
    pf.state = COIN_STATE_IDLE;
}

gm_plugin_result_t gm_plugin_entry(const gm_plugin_host_api_t *host,
                                    gm_plugin_descriptor_t *plugin)
{
    const gm_plugin_capabilities_t required = GM_PLUGIN_CAP_IMU_EVENTS;
    const void *api = (const void *)(uintptr_t)1;
    gm_plugin_result_t result;

    if (host == 0 || plugin == 0 || host->log == 0 ||
        host->display_get_info == 0 || host->graphics.lvgl == 0 ||
        host->imu_enable == 0 || host->extension_get == 0 ||
        !GM_PLUGIN_VERSION_COMPATIBLE(host->abi_version,
                                      GM_PLUGIN_ABI_MIN_VERSION) ||
        host->struct_size < GM_PLUGIN_HOST_API_MIN_SIZE ||
        plugin->struct_size < GM_PLUGIN_DESCRIPTOR_MIN_SIZE ||
        (host->capabilities & required) != required)
        return GM_PLUGIN_ENOTSUP;

    pf.host = host;
    pf.ui = host->graphics.lvgl;
    if (pf.ui->struct_size < GM_PLUGIN_LVGL_API_1_1_SIZE ||
        !GM_PLUGIN_VERSION_COMPATIBLE(pf.ui->api_version,
                                      GM_PLUGIN_VERSION(1U, 1U)) ||
        pf.ui->image_create == 0 || pf.ui->image_set_source == 0)
        return GM_PLUGIN_EVERSION;

    if (gm_plugin_libc_get(host, &pf.libc) != GM_PLUGIN_OK)
        return GM_PLUGIN_ENOTSUP;

    result = host->extension_get(GM_PLUGIN_EXTENSION_RANDOM, &api);
    if (result != GM_PLUGIN_OK) return result;
    pf.random = (const gm_plugin_random_extension_api_t *)api;
    if (pf.random == 0 || pf.random->get_u32 == 0) return GM_PLUGIN_EVERSION;

    plugin->abi_version = GM_PLUGIN_ABI_MIN_VERSION;
    plugin->context = &pf;
    plugin->on_start = pile_ou_face_start;
    plugin->on_loop = pile_ou_face_loop;
    plugin->on_event = pile_ou_face_event;
    plugin->on_stop = pile_ou_face_stop;
    return GM_PLUGIN_OK;
}
