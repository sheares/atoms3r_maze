#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "gc9107.h"
#include "joystick2.h"
#include "lp5562.h"
#include "maze.h"

#define TAG "maze"

#define STICK_DEAD     25
#define ANIM_FRAMES    8          // frames to slide between cells (8 × 33 = ~265 ms)
#define FRAME_MS       33

#define BALL_R         4
#define WALL_COLOR     COLOR_BLACK
#define FLOOR_COLOR    COLOR_WHITE
#define BALL_COLOR     COLOR_RED
#define BALL_HIGHLIGHT COLOR_YELLOW
#define PICKUP_COLOR   COLOR_BLUE
#define COUNTER_COLOR  COLOR_YELLOW

#define BTN_HOLD_US        500000      // 500 ms hold-to-regenerate
#define BOOST_DURATION_US  3000000     // 3 s per pickup
#define BOOST_FRAMES       4           // animation speed while boosted
#define NORMAL_FRAMES      8           // normal animation speed
#define PICKUP_COUNT       20          // pickups scattered in the maze
#define PICKUP_R           2           // visible radius (4×4)

// Cell-snapped ball position + animation toward a target cell
static int s_br, s_bc;
static int s_tr, s_tc;
static int s_anim_step = 0;       // 0 = idle, 1..anim_frames() = animating

// Smoothly-interpolated pixel position derived from the cells + anim step
static int s_px, s_py;
static int g_view_x, g_view_y;

// Pickups + score + speed boost
static uint8_t s_pickup[MAZE_H][MAZE_W];
static int     s_collected = 0;
static int64_t s_boost_until_us = 0;

static int anim_frames(void)
{
    return (esp_timer_get_time() < s_boost_until_us) ? BOOST_FRAMES : NORMAL_FRAMES;
}

static void spawn_pickups(void)
{
    memset(s_pickup, 0, sizeof(s_pickup));
    int placed = 0;
    int safety = 0;
    while (placed < PICKUP_COUNT && safety < PICKUP_COUNT * 10) {
        safety++;
        int r = esp_random() % MAZE_H;
        int c = esp_random() % MAZE_W;
        if (s_pickup[r][c]) continue;
        if (r == MAZE_H / 2 && c == MAZE_W / 2) continue;   // don't spawn on the player
        s_pickup[r][c] = 1;
        placed++;
    }
}

static void try_collect(void)
{
    if (s_pickup[s_br][s_bc]) {
        s_pickup[s_br][s_bc] = 0;
        s_collected++;
        int64_t now = esp_timer_get_time();
        if (s_boost_until_us < now) s_boost_until_us = now;
        s_boost_until_us += BOOST_DURATION_US;
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int absi(int v) { return v < 0 ? -v : v; }
static int max_view_x(void) { return MAZE_W * CELL_PX - LCD_WIDTH;  }
static int max_view_y(void) { return MAZE_H * CELL_PX - LCD_HEIGHT; }

static void compute_view(void)
{
    g_view_x = clampi(s_px - LCD_WIDTH  / 2, 0, max_view_x());
    g_view_y = clampi(s_py - LCD_HEIGHT / 2, 0, max_view_y());
}

static void draw_world(void)
{
    gc9107_fill_screen(FLOOR_COLOR);
    int first_c = g_view_x / CELL_PX;
    int last_c  = (g_view_x + LCD_WIDTH  - 1) / CELL_PX;
    int first_r = g_view_y / CELL_PX;
    int last_r  = (g_view_y + LCD_HEIGHT - 1) / CELL_PX;
    if (last_c >= MAZE_W) last_c = MAZE_W - 1;
    if (last_r >= MAZE_H) last_r = MAZE_H - 1;

    for (int r = first_r; r <= last_r; r++) {
        for (int c = first_c; c <= last_c; c++) {
            const maze_cell_t *cell = maze_cell(r, c);
            if (!cell) continue;
            int x = c * CELL_PX - g_view_x;
            int y = r * CELL_PX - g_view_y;
            if (cell->walls & WALL_N) gc9107_draw_hline(x, y, CELL_PX, WALL_COLOR);
            if (cell->walls & WALL_W) gc9107_draw_vline(x, y, CELL_PX, WALL_COLOR);
        }
    }
    if (last_c == MAZE_W - 1)
        gc9107_draw_vline(MAZE_W * CELL_PX - g_view_x - 1, 0, LCD_HEIGHT, WALL_COLOR);
    if (last_r == MAZE_H - 1)
        gc9107_draw_hline(0, MAZE_H * CELL_PX - g_view_y - 1, LCD_WIDTH, WALL_COLOR);
}

static void draw_ball(int cx, int cy)
{
    gc9107_fill_rect(cx - BALL_R, cy - BALL_R, BALL_R * 2, BALL_R * 2, BALL_COLOR);
    gc9107_draw_pixel(cx - 1, cy - 1, BALL_HIGHLIGHT);
}

static void update_ball_pixel(void)
{
    int sx = s_bc * CELL_PX + CELL_PX / 2;
    int sy = s_br * CELL_PX + CELL_PX / 2;
    int tx = s_tc * CELL_PX + CELL_PX / 2;
    int ty = s_tr * CELL_PX + CELL_PX / 2;
    int af = anim_frames();
    if (s_anim_step > 0) {
        s_px = sx + (tx - sx) * s_anim_step / af;
        s_py = sy + (ty - sy) * s_anim_step / af;
    } else {
        s_px = sx;
        s_py = sy;
    }
}

static void draw_pickups(void)
{
    int first_c = g_view_x / CELL_PX;
    int last_c  = (g_view_x + LCD_WIDTH  - 1) / CELL_PX;
    int first_r = g_view_y / CELL_PX;
    int last_r  = (g_view_y + LCD_HEIGHT - 1) / CELL_PX;
    if (last_c >= MAZE_W) last_c = MAZE_W - 1;
    if (last_r >= MAZE_H) last_r = MAZE_H - 1;
    for (int r = first_r; r <= last_r; r++) {
        for (int c = first_c; c <= last_c; c++) {
            if (!s_pickup[r][c]) continue;
            int cx = c * CELL_PX + CELL_PX / 2 - g_view_x;
            int cy = r * CELL_PX + CELL_PX / 2 - g_view_y;
            gc9107_fill_rect(cx - PICKUP_R, cy - PICKUP_R,
                             PICKUP_R * 2, PICKUP_R * 2, PICKUP_COLOR);
        }
    }
}

static void draw_counter(void)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "x%d", s_collected);
    int len = (int)strlen(buf);
    int text_w = len * 6;            // 5×8 font + 1 px gap at scale 1
    int box_w  = text_w + 4;
    int box_h  = 10;
    int x = LCD_WIDTH - box_w - 2;
    int y = 2;
    gc9107_fill_rect(x, y, box_w, box_h, COLOR_BLACK);
    gc9107_draw_string(x + 2, y + 1, buf, COUNTER_COLOR, COLOR_BLACK, 1);
}

static void render(void)
{
    update_ball_pixel();
    compute_view();
    draw_world();
    draw_pickups();
    draw_ball(s_px - g_view_x, s_py - g_view_y);
    draw_counter();
    gc9107_flush();
}

// Rotated 90° CW from native: stick Y → ball row; stick X → ball column.
// Picks the dominant axis so diagonal pushes collapse to 4-way movement.
static void pick_direction(const joystick2_data_t *j, int *dr, int *dc)
{
    *dr = *dc = 0;
    int ax = absi(j->x), ay = absi(j->y);
    if (ax < STICK_DEAD && ay < STICK_DEAD) return;
    if (ay > ax)        *dr = (j->y > 0) ?  1 : -1;
    else                *dc = (j->x > 0) ?  1 : -1;
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot");
    gc9107_init();
    gc9107_set_rotation(2);

    i2c_master_bus_handle_t sys_bus = NULL;
    if (lp5562_init(&sys_bus)) lp5562_set_backlight(sys_bus, 200);

    if (joystick2_init() != ESP_OK) {
        ESP_LOGE(TAG, "Joystick2 not found");
        gc9107_fill_screen(COLOR_BLACK);
        gc9107_draw_string(14, 24, "NO",       COLOR_RED,   COLOR_BLACK, 3);
        gc9107_draw_string(2,  60, "JOYSTICK", COLOR_RED,   COLOR_BLACK, 2);
        gc9107_draw_string(4,  92, "Plug in &", COLOR_WHITE, COLOR_BLACK, 1);
        gc9107_draw_string(4, 104, "reset.",    COLOR_WHITE, COLOR_BLACK, 1);
        gc9107_flush();
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    joystick2_calibrate_centre();

    maze_generate(0);
    spawn_pickups();
    s_br = s_tr = MAZE_H / 2;
    s_bc = s_tc = MAZE_W / 2;
    render();

    bool prev_btn = false;
    int64_t btn_down_us = 0;
    bool    btn_fired = false;
    int64_t last_frame = esp_timer_get_time();

    while (1) {
        joystick2_data_t js;
        bool dirty = false;
        int64_t now = esp_timer_get_time();

        if (joystick2_read(&js) == ESP_OK) {
            // Hold-to-regenerate button
            if (js.btn && !prev_btn) btn_down_us = now;
            if (js.btn) {
                if (!btn_fired && (now - btn_down_us) > BTN_HOLD_US) {
                    ESP_LOGI(TAG, "regenerating maze");
                    maze_generate(0);
                    spawn_pickups();
                    s_collected = 0;
                    s_boost_until_us = 0;
                    s_br = s_tr = MAZE_H / 2;
                    s_bc = s_tc = MAZE_W / 2;
                    s_anim_step = 0;
                    dirty = true;
                    btn_fired = true;
                }
            } else {
                btn_down_us = 0;
                btn_fired = false;
            }
            prev_btn = js.btn;

            // Accept a new direction only when not animating
            if (s_anim_step == 0) {
                int dr, dc;
                pick_direction(&js, &dr, &dc);
                if ((dr || dc) && maze_can_move(s_br, s_bc, dr, dc)) {
                    s_tr = s_br + dr;
                    s_tc = s_bc + dc;
                    s_anim_step = 1;
                    dirty = true;
                }
            }
        }

        // Advance animation
        if (s_anim_step > 0) {
            s_anim_step++;
            if (s_anim_step >= anim_frames()) {
                s_br = s_tr;
                s_bc = s_tc;
                s_anim_step = 0;
                try_collect();
            }
            dirty = true;
        }

        // Boost timer expiring → refresh the visible state
        static bool was_boosting = false;
        bool boosting_now = esp_timer_get_time() < s_boost_until_us;
        if (was_boosting && !boosting_now) dirty = true;
        was_boosting = boosting_now;

        if (dirty) render();

        int dt_ms = (esp_timer_get_time() - last_frame) / 1000;
        int wait_ms = FRAME_MS - dt_ms;
        if (wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms));
        last_frame = esp_timer_get_time();
    }
}
