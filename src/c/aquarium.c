#include <pebble.h>

// ============================================================================
// Aquarium watchface
//   - fish swim around a Frutiger-Aero tank (gradient water, light rays,
//     rising bubbles), with a wavy sand bed, plants and a rock
//   - shake to drop food (max once per 10s); fish each chase a *different*
//     pellet and eat it; uneaten food settles on the sand then dissolves
//   - fish occasionally hide behind the rock
//   - feed daily -> the fish breed; fish grow up and the oldest die off when
//     the tank is full; a full day unfed -> they all die -> "Shake to add fish!"
//   - time (left) and date (right) sit in the sand bank
// Lifecycle runs off the real clock so it advances across days.
// ============================================================================

#define MAX_FISH        8
#define MAX_PELLETS     6
#define MAX_BUBBLES     7
#define SUB             16        // sub-pixel fixed-point resolution
#define ANIM_MS         80        // animation frame interval
#define FOOD_PER_SHAKE  3
#define FEED_COOLDOWN_S 10        // anti-spam: one feed per 10 real seconds
#define EAT_PX          8
#define REST_FRAMES     50        // ~4s a pellet lingers on the sand
#define SCHEMA_VERSION  2         // bump when the persisted structs change

// real-time lifecycle thresholds (seconds)
#define GROW_MED_S   (1 * 24 * 3600)
#define GROW_BIG_S   (3 * 24 * 3600)
#define LIFESPAN_S   (14 * 24 * 3600)
#define STARVE_S     (24 * 3600)
#define BREED_DAYS   3

// easter egg: an octopus floats past at 23:09
#define OCTO_HOUR    23
#define OCTO_MIN     9
#define OCTO_SPEED   12        // sub-pixel per frame
#define OCTO_FRAME_DIV 6       // frames per animation cell

// persistence keys
#define PK_VERSION 0
#define PK_FISH    1
#define PK_META    2

typedef struct {
  bool    alive;
  int16_t x, y;       // sub-pixel position (top-left of sprite)
  int16_t vx, vy;     // sub-pixel velocity per frame
  uint8_t species;    // 0 / 1
  uint8_t size;       // 0 baby, 1 med, 2 big
  uint8_t facing;     // 0 left, 1 right
  int16_t target;     // pellet index this fish is chasing, or -1
  int16_t hide_timer; // frames remaining hiding behind the rock (0 = not)
  time_t  born;       // wall-clock birth time
} Fish;

typedef struct {
  bool    active;
  int16_t x, y;       // sub-pixel
  int16_t vy;
  int16_t rest;       // >0: settled on sand, counting down to dissolve
} Pellet;

typedef struct {
  int16_t x, y;       // sub-pixel
  int8_t  vy;         // rise speed (negative)
  int8_t  r;          // radius px
} Bubble;

typedef struct {
  int    consec_days_fed;
  int    last_seen_yday;
  bool   fed_today;
  time_t last_eat_time;
} Meta;

static Window    *s_window;
static Layer     *s_canvas;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static AppTimer  *s_anim_timer;

static Fish   s_fish[MAX_FISH];
static Pellet s_pellets[MAX_PELLETS];
static Bubble s_bubbles[MAX_BUBBLES];
static Meta   s_meta;
static time_t s_last_feed_time = 0;

static GBitmap *s_fish_bmp[2][3][2];   // [species][size][facing]
static GBitmap *s_plant_bmp[2];
static GBitmap *s_rock_bmp;
static GBitmap *s_octo_bmp[2];

static bool s_octo_active = false;
static int  s_octo_x;            // sub-pixel, travels left -> right
static int  s_octo_y;           // base y (px)
static int  s_octo_anim;        // frame counter while active
static int  s_octo_w, s_octo_h;

static GRect s_bounds;
static int   s_sand_top;     // baseline y (px) of the wavy sand surface
static int   s_rock_x, s_rock_y, s_rock_w, s_rock_h;
static char  s_time_buf[8];
static char  s_date_buf[12];

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static GBitmap *fish_bmp(const Fish *f) { return s_fish_bmp[f->species][f->size][f->facing]; }
static GSize fish_size_px(const Fish *f) { return gbitmap_get_bounds(fish_bmp(f)).size; }

static int count_alive(void) {
  int n = 0;
  for (int i = 0; i < MAX_FISH; i++) if (s_fish[i].alive) n++;
  return n;
}

static int rnd(int n) { return n > 0 ? (rand() % n) : 0; }

static void fish_center(const Fish *f, int *cx, int *cy) {
  GSize fs = fish_size_px(f);
  *cx = f->x + fs.w * SUB / 2;
  *cy = f->y + fs.h * SUB / 2;
}

static void spawn_fish(uint8_t species, uint8_t size) {
  for (int i = 0; i < MAX_FISH; i++) {
    if (s_fish[i].alive) continue;
    Fish *f = &s_fish[i];
    f->alive = true; f->species = species; f->size = size;
    f->facing = rnd(2); f->target = -1; f->hide_timer = 0;
    f->born = time(NULL);
    int maxx = (s_bounds.size.w - 30) * SUB;
    int maxy = (s_sand_top - 20) * SUB;
    f->x  = 15 * SUB + rnd(maxx > 0 ? maxx : 1);
    f->y  = 8 * SUB + rnd(maxy > 0 ? maxy : 1);
    f->vx = (rnd(2) ? 1 : -1) * (6 + rnd(10));
    f->vy = (rnd(2) ? 1 : -1) * (3 + rnd(6));
    return;
  }
}

static void add_two_fish(void) {
  spawn_fish(0, 1);
  spawn_fish(1, 1);
  s_meta.last_eat_time = time(NULL);  // grace period before starving
}

static void kill_all_fish(void) {
  for (int i = 0; i < MAX_FISH; i++) s_fish[i].alive = false;
}

static void kill_oldest(void) {
  int oldest = -1;
  for (int i = 0; i < MAX_FISH; i++) {
    if (!s_fish[i].alive) continue;
    if (oldest < 0 || s_fish[i].born < s_fish[oldest].born) oldest = i;
  }
  if (oldest >= 0) s_fish[oldest].alive = false;
}

static void drop_food(void) {
  int dropped = 0;
  for (int i = 0; i < MAX_PELLETS && dropped < FOOD_PER_SHAKE; i++) {
    if (s_pellets[i].active) continue;
    s_pellets[i].active = true;
    s_pellets[i].rest = 0;
    s_pellets[i].x = (8 + rnd(s_bounds.size.w - 16)) * SUB;
    s_pellets[i].y = 2 * SUB;
    s_pellets[i].vy = 4 + rnd(4);
    dropped++;
  }
}

static void try_breed(void) {
  int n = count_alive();
  if (n < 2) return;
  if (n >= MAX_FISH) kill_oldest();
  spawn_fish(rnd(2), 0);  // a baby
}

// ---------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------

static void persist_save(void) {
  persist_write_int(PK_VERSION, SCHEMA_VERSION);
  persist_write_data(PK_FISH, s_fish, sizeof(s_fish));
  persist_write_data(PK_META, &s_meta, sizeof(s_meta));
}

static void persist_load(void) {
  bool ok = persist_exists(PK_VERSION) &&
            persist_read_int(PK_VERSION) == SCHEMA_VERSION;
  if (ok && persist_exists(PK_FISH)) {
    persist_read_data(PK_FISH, s_fish, sizeof(s_fish));
    persist_read_data(PK_META, &s_meta, sizeof(s_meta));
  } else {
    // fresh start: empty tank, "shake to add fish"
    for (int i = 0; i < MAX_FISH; i++) s_fish[i].alive = false;
    s_meta.consec_days_fed = 0;
    s_meta.last_seen_yday  = -1;
    s_meta.fed_today       = false;
    s_meta.last_eat_time   = 0;
  }
}

// ---------------------------------------------------------------------------
// simulation
// ---------------------------------------------------------------------------

// each fish locks onto its own pellet so they don't all swarm the same one
static void assign_target(int fi) {
  Fish *f = &s_fish[fi];
  if (f->target >= 0 && s_pellets[f->target].active) return;  // commit until eaten
  f->target = -1;

  // collect pellets, preferring ones no other fish has claimed
  int freep[MAX_PELLETS], freen = 0;
  int anyp[MAX_PELLETS], anyn = 0;
  for (int j = 0; j < MAX_PELLETS; j++) {
    if (!s_pellets[j].active) continue;
    anyp[anyn++] = j;
    bool taken = false;
    for (int k = 0; k < MAX_FISH; k++) {
      if (k != fi && s_fish[k].alive && s_fish[k].target == j) { taken = true; break; }
    }
    if (!taken) freep[freen++] = j;
  }
  // pick a *random* pellet (not the nearest) so the fish spread out
  if (freen > 0)      f->target = freep[rnd(freen)];
  else if (anyn > 0)  f->target = anyp[rnd(anyn)];
}

static void step_pellets(void) {
  for (int i = 0; i < MAX_PELLETS; i++) {
    Pellet *p = &s_pellets[i];
    if (!p->active) continue;
    if (p->rest > 0) {                 // settled on the sand, dissolving
      if (--p->rest == 0) p->active = false;
      continue;
    }
    p->y += p->vy;
    if (p->y / SUB >= s_sand_top - 2) { // hit the sand: rest, still edible
      p->y = (s_sand_top - 2) * SUB;
      p->vy = 0;
      p->rest = REST_FRAMES;
    }
  }
}

static void step_fish(void) {
  const int SPEED = 22;
  int any_food = 0;
  for (int i = 0; i < MAX_PELLETS; i++) if (s_pellets[i].active) any_food = 1;

  for (int i = 0; i < MAX_FISH; i++) {
    Fish *f = &s_fish[i];
    if (!f->alive) continue;

    int cx, cy; fish_center(f, &cx, &cy);

    if (any_food) {
      f->hide_timer = 0;
      assign_target(i);
      Pellet *p = (f->target >= 0) ? &s_pellets[f->target] : NULL;
      if (p) {
        int dx = p->x - cx, dy = p->y - cy;
        f->vx = (dx / 6 > SPEED) ? SPEED : (dx / 6 < -SPEED ? -SPEED : dx / 6);
        f->vy = (dy / 6 > SPEED) ? SPEED : (dy / 6 < -SPEED ? -SPEED : dy / 6);
        if (abs(dx) < EAT_PX * SUB && abs(dy) < EAT_PX * SUB) {
          p->active = false;
          f->target = -1;
          s_meta.fed_today = true;
          s_meta.last_eat_time = time(NULL);
        }
      }
    } else if (f->hide_timer > 0) {
      // swim to the rock and tuck in behind it
      f->hide_timer--;
      int hx = (s_rock_x + s_rock_w / 2) * SUB;
      int hy = (s_rock_y + s_rock_h / 3) * SUB;  // upper mass: reachable + covered
      int dx = hx - cx, dy = hy - cy;
      f->vx = (dx / 8 > 12) ? 12 : (dx / 8 < -12 ? -12 : dx / 8);
      f->vy = (dy / 8 > 10) ? 10 : (dy / 8 < -10 ? -10 : dy / 8);
    } else {
      f->target = -1;
      if (rnd(100) < 5) f->vx += rnd(7) - 3;
      if (rnd(100) < 5) f->vy += rnd(5) - 2;
      if (f->vx > SPEED) f->vx = SPEED;
      if (f->vx < -SPEED) f->vx = -SPEED;
      if (f->vy > SPEED / 2) f->vy = SPEED / 2;
      if (f->vy < -SPEED / 2) f->vy = -SPEED / 2;
      if (rnd(450) < 2) f->hide_timer = 70 + rnd(90);  // ~6-13s hideout
    }

    f->x += f->vx;
    f->y += f->vy;

    GSize fs = fish_size_px(f);
    int maxx = (s_bounds.size.w - fs.w) * SUB;
    int maxy = (s_sand_top - fs.h) * SUB;
    if (f->x < 0)    { f->x = 0;    f->vx = -f->vx; }
    if (f->x > maxx) { f->x = maxx; f->vx = -f->vx; }
    if (f->y < 0)    { f->y = 0;    f->vy = -f->vy; }
    if (f->y > maxy) { f->y = maxy; f->vy = -f->vy; }

    if (f->vx > 1)  f->facing = 1;
    if (f->vx < -1) f->facing = 0;
  }
}

static void step_bubbles(void) {
  for (int i = 0; i < MAX_BUBBLES; i++) {
    s_bubbles[i].y += s_bubbles[i].vy;          // vy is negative -> rises
    s_bubbles[i].x += (rnd(3) - 1);             // gentle horizontal wobble
    if (s_bubbles[i].y < 0) {                   // respawn at the bottom
      s_bubbles[i].y = s_sand_top * SUB;
      s_bubbles[i].x = rnd(s_bounds.size.w) * SUB;
    }
  }
}

static void step_octopus(void) {
  if (!s_octo_active) return;
  s_octo_anim++;
  s_octo_x += OCTO_SPEED;
  if (s_octo_x / SUB > s_bounds.size.w + 4) s_octo_active = false;  // gone
}

// kick off the 23:09 cameo
static void maybe_octopus(struct tm *t) {
  if (t->tm_hour == OCTO_HOUR && t->tm_min == OCTO_MIN && !s_octo_active) {
    s_octo_active = true;
    s_octo_anim = 0;
    s_octo_x = -(s_octo_w + 4) * SUB;        // start off the left edge
    s_octo_y = 12 + rnd(s_sand_top / 3);     // upper-ish water
  }
}

static void anim_tick(void *ctx) {
  step_pellets();
  step_fish();
  step_bubbles();
  step_octopus();
  layer_mark_dirty(s_canvas);
  s_anim_timer = app_timer_register(ANIM_MS, anim_tick, NULL);
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------

#if defined(PBL_COLOR)
static void draw_water(GContext *ctx, int w, int wb) {
  for (int y = 0; y < wb; y += 5) {
    uint8_t r = 95  - 95  * y / wb;   // light aqua -> deep blue
    uint8_t g = 205 - 110 * y / wb;
    uint8_t b = 232 - 67  * y / wb;
    graphics_context_set_fill_color(ctx, GColorFromRGB(r, g, b));
    graphics_fill_rect(ctx, GRect(0, y, w, 6), 0, GCornerNone);
  }
  // soft diagonal light rays
  graphics_context_set_stroke_color(ctx, GColorFromRGB(165, 230, 250));
  int rays[2] = { w / 4, (w * 3) / 5 };
  for (int r = 0; r < 2; r++) {
    for (int i = 0; i < 4; i++) {
      graphics_draw_line(ctx, GPoint(rays[r] + i, 0),
                         GPoint(rays[r] + i + 16, wb));
    }
  }
}
#endif

static void draw_sand(GContext *ctx, int w, int h) {
#if defined(PBL_COLOR)
  graphics_context_set_stroke_color(ctx, GColorFromRGB(222, 200, 120));
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
  for (int x = 0; x < w; x++) {
    int32_t ang = (x * TRIG_MAX_ANGLE) / 38;
    int yo = (4 * sin_lookup(ang)) / TRIG_MAX_RATIO;
    graphics_draw_line(ctx, GPoint(x, s_sand_top + yo), GPoint(x, h));
  }
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int w = b.size.w, h = b.size.h;

#if defined(PBL_COLOR)
  draw_water(ctx, w, s_sand_top);
#endif

  draw_sand(ctx, w, h);

  graphics_context_set_compositing_mode(ctx, GCompOpSet);

  // plants rising from the sand
  GSize p0 = gbitmap_get_bounds(s_plant_bmp[0]).size;
  GSize p1 = gbitmap_get_bounds(s_plant_bmp[1]).size;
  graphics_draw_bitmap_in_rect(ctx, s_plant_bmp[0], GRect(8, s_sand_top - p0.h + 2, p0.w, p0.h));
  graphics_draw_bitmap_in_rect(ctx, s_plant_bmp[1], GRect(w - p1.w - 10, s_sand_top - p1.h + 2, p1.w, p1.h));

  // pellets
#if defined(PBL_COLOR)
  graphics_context_set_fill_color(ctx, GColorYellow);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  for (int i = 0; i < MAX_PELLETS; i++) {
    if (!s_pellets[i].active) continue;
    graphics_fill_circle(ctx, GPoint(s_pellets[i].x / SUB, s_pellets[i].y / SUB), 2);
  }

  // fish (drawn before the rock so a hiding fish is occluded by it)
  for (int i = 0; i < MAX_FISH; i++) {
    if (!s_fish[i].alive) continue;
    GBitmap *bmp = fish_bmp(&s_fish[i]);
    GSize fs = gbitmap_get_bounds(bmp).size;
    graphics_draw_bitmap_in_rect(ctx, bmp, GRect(s_fish[i].x / SUB, s_fish[i].y / SUB, fs.w, fs.h));
  }

  // rock (same bitmap pattern as the plants)
  GSize rk = gbitmap_get_bounds(s_rock_bmp).size;
  graphics_draw_bitmap_in_rect(ctx, s_rock_bmp, GRect((w - rk.w) / 2, s_sand_top - rk.h + 14, rk.w, rk.h));

  // easter egg: octopus floating past at 23:09 (bobs gently as it drifts)
  if (s_octo_active) {
    int bob = (5 * sin_lookup(s_octo_anim * (TRIG_MAX_ANGLE / 28))) / TRIG_MAX_RATIO;
    int frame = (s_octo_anim / OCTO_FRAME_DIV) % 2;
    graphics_draw_bitmap_in_rect(ctx, s_octo_bmp[frame],
        GRect(s_octo_x / SUB, s_octo_y + bob, s_octo_w, s_octo_h));
  }

  // bubbles (foreground sparkle)
#if defined(PBL_COLOR)
  graphics_context_set_stroke_color(ctx, GColorCeleste);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
  for (int i = 0; i < MAX_BUBBLES; i++) {
    graphics_draw_circle(ctx, GPoint(s_bubbles[i].x / SUB, s_bubbles[i].y / SUB), s_bubbles[i].r);
  }

  // empty-tank prompt
  if (count_alive() == 0) {
    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
    graphics_draw_text(ctx, "Shake to\nadd fish!",
        fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
        GRect(0, s_sand_top / 2 - 30, w, 70),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

// ---------------------------------------------------------------------------
// time + daily lifecycle
// ---------------------------------------------------------------------------

static void update_clock(struct tm *t) {
  strftime(s_time_buf, sizeof(s_time_buf), clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  strftime(s_date_buf, sizeof(s_date_buf), "%a %d", t);
  text_layer_set_text(s_time_layer, s_time_buf);
  text_layer_set_text(s_date_layer, s_date_buf);
}

static void lifecycle(time_t now, struct tm *t) {
  for (int i = 0; i < MAX_FISH; i++) {
    if (!s_fish[i].alive) continue;
    long age = now - s_fish[i].born;
    if (age >= LIFESPAN_S) { s_fish[i].alive = false; continue; }
    if (age >= GROW_BIG_S)      s_fish[i].size = 2;
    else if (age >= GROW_MED_S) s_fish[i].size = 1;
    else                        s_fish[i].size = 0;
  }

  int yday = t->tm_yday;
  if (s_meta.last_seen_yday < 0) s_meta.last_seen_yday = yday;
  if (yday != s_meta.last_seen_yday) {
    if (s_meta.fed_today) s_meta.consec_days_fed++;
    else                  s_meta.consec_days_fed = 0;
    s_meta.fed_today = false;
    s_meta.last_seen_yday = yday;
    if (s_meta.consec_days_fed >= BREED_DAYS) {
      try_breed();
      s_meta.consec_days_fed = 0;
    }
  }

  if (count_alive() > 0 && s_meta.last_eat_time > 0 &&
      (now - s_meta.last_eat_time) > STARVE_S) {
    kill_all_fish();
  }
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  time_t now = time(NULL);
  update_clock(tick_time);
  lifecycle(now, tick_time);
  maybe_octopus(tick_time);
  persist_save();
}

static void tap_handler(AccelAxisType axis, int32_t direction) {
  if (count_alive() == 0) {
    add_two_fish();
  } else {
    time_t now = time(NULL);
    if (now - s_last_feed_time >= FEED_COOLDOWN_S) {
      drop_food();
      s_last_feed_time = now;
    }
  }
  layer_mark_dirty(s_canvas);
}

// ---------------------------------------------------------------------------
// window lifecycle
// ---------------------------------------------------------------------------

static void load_bitmaps(void) {
  s_fish_bmp[0][0][0] = gbitmap_create_with_resource(RESOURCE_ID_FISH_A_BABY_L);
  s_fish_bmp[0][0][1] = gbitmap_create_with_resource(RESOURCE_ID_FISH_A_BABY_R);
  s_fish_bmp[0][1][0] = gbitmap_create_with_resource(RESOURCE_ID_FISH_A_MED_L);
  s_fish_bmp[0][1][1] = gbitmap_create_with_resource(RESOURCE_ID_FISH_A_MED_R);
  s_fish_bmp[0][2][0] = gbitmap_create_with_resource(RESOURCE_ID_FISH_A_BIG_L);
  s_fish_bmp[0][2][1] = gbitmap_create_with_resource(RESOURCE_ID_FISH_A_BIG_R);
  s_fish_bmp[1][0][0] = gbitmap_create_with_resource(RESOURCE_ID_FISH_B_BABY_L);
  s_fish_bmp[1][0][1] = gbitmap_create_with_resource(RESOURCE_ID_FISH_B_BABY_R);
  s_fish_bmp[1][1][0] = gbitmap_create_with_resource(RESOURCE_ID_FISH_B_MED_L);
  s_fish_bmp[1][1][1] = gbitmap_create_with_resource(RESOURCE_ID_FISH_B_MED_R);
  s_fish_bmp[1][2][0] = gbitmap_create_with_resource(RESOURCE_ID_FISH_B_BIG_L);
  s_fish_bmp[1][2][1] = gbitmap_create_with_resource(RESOURCE_ID_FISH_B_BIG_R);
  s_plant_bmp[0] = gbitmap_create_with_resource(RESOURCE_ID_PLANT_1);
  s_plant_bmp[1] = gbitmap_create_with_resource(RESOURCE_ID_PLANT_2);
  s_rock_bmp = gbitmap_create_with_resource(RESOURCE_ID_ROCK);
  s_octo_bmp[0] = gbitmap_create_with_resource(RESOURCE_ID_OCTOPUS_F0);
  s_octo_bmp[1] = gbitmap_create_with_resource(RESOURCE_ID_OCTOPUS_F1);
}

static void destroy_bitmaps(void) {
  for (int s = 0; s < 2; s++)
    for (int z = 0; z < 3; z++)
      for (int d = 0; d < 2; d++)
        gbitmap_destroy(s_fish_bmp[s][z][d]);
  gbitmap_destroy(s_plant_bmp[0]);
  gbitmap_destroy(s_plant_bmp[1]);
  gbitmap_destroy(s_rock_bmp);
  gbitmap_destroy(s_octo_bmp[0]);
  gbitmap_destroy(s_octo_bmp[1]);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_bounds = layer_get_bounds(root);
  int w = s_bounds.size.w, h = s_bounds.size.h;
  int sand_h = 34 + (h > 168 ? 8 : 0);
  s_sand_top = h - sand_h;

  load_bitmaps();

  GSize rs = gbitmap_get_bounds(s_rock_bmp).size;
  s_rock_w = rs.w; s_rock_h = rs.h;
  s_rock_x = (w - s_rock_w) / 2;
  s_rock_y = s_sand_top - s_rock_h + 14;

  GSize os = gbitmap_get_bounds(s_octo_bmp[0]).size;
  s_octo_w = os.w; s_octo_h = os.h;

  for (int i = 0; i < MAX_BUBBLES; i++) {
    s_bubbles[i].x = rnd(w) * SUB;
    s_bubbles[i].y = rnd(s_sand_top) * SUB;
    s_bubbles[i].vy = -(2 + rnd(3));
    s_bubbles[i].r = 1 + rnd(2);
  }

  s_canvas = layer_create(s_bounds);
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);

  // time (left) + date (right), vertically centred in the sand bank
  int ty = s_sand_top + (sand_h - 26) / 2;
  s_time_layer = text_layer_create(GRect(4, ty, w / 2, 26));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite));
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentLeft);
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  int dy = s_sand_top + (sand_h - 20) / 2;
  s_date_layer = text_layer_create(GRect(w / 2 - 4, dy, w / 2, 20));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite));
  text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentRight);
  layer_add_child(root, text_layer_get_layer(s_date_layer));

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  update_clock(t);
  lifecycle(now, t);
  maybe_octopus(t);   // in case the face is opened right at 23:09

  s_anim_timer = app_timer_register(ANIM_MS, anim_tick, NULL);
}

static void window_unload(Window *window) {
  if (s_anim_timer) app_timer_cancel(s_anim_timer);
  persist_save();
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_date_layer);
  layer_destroy(s_canvas);
  destroy_bitmaps();
}

static void init(void) {
  srand(time(NULL));
  persist_load();

  s_window = window_create();
  window_set_background_color(s_window, PBL_IF_COLOR_ELSE(GColorFromRGB(95, 205, 232), GColorWhite));
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  accel_tap_service_subscribe(tap_handler);
}

static void deinit(void) {
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
