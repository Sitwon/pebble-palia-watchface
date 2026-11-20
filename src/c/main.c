/**
 * Palia Time — Pebble watchface
 *
 * Shows the in-game time of day for the game Palia on a one-hand 24-hour
 * analog dial, segmented into the Palia day/night cycle (Dawn / Day / Dusk /
 * Night) with colors matching the in-game clock. The real-world digital time
 * is shown in a center badge, following the watch's 12/24-hour setting.
 *
 * Palia time runs at 24x real speed: one in-game day = one real hour, so one
 * in-game minute passes every 2.5 real seconds. Anchors (minutes past the
 * real hour): 12:00 AM = :00, 6:00 AM = :15, 12:00 PM = :30, 6:00 PM = :45.
 *
 * Update driver: a self-rescheduling 2.5s app_timer (one wake per in-game
 * minute, with drift correction) instead of SECOND_UNIT ticks. The canvas is
 * only redrawn when a displayed value changes.
 */

#include <pebble.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

// Dial geometry. Reference values are tuned for emery (200x228, so the
// shorter screen half-extent is 100 px) and are scaled to the actual screen
// in compute_geometry() so every platform keeps the same look.
// The day/night cycle fills the entire screen as angular sectors clipped to
// the rectangle ("square pie"): hour ticks sit on the screen perimeter and
// the hand sweeps inside them.
#define BADGE_RADIUS 28    // center badge radius (minimum; grows on bigger screens)
#define HAND_LENGTH 98     // needle tip radius
#define HAND_BASE_WIDTH 6  // needle half-width at its base
#define HAND_BASE_Y 14     // hand base distance behind the pivot

// Sun & moon decorations, matching the in-game clock (sun at the noon
// position, moon at midnight, stars in the Night sector)
#define ICON_POS_RADIUS 66        // distance of the sun/moon from the dial center
#define ICON_POS_RADIUS_ROUND 56  // tighter on round screens: clears the tick ring
                                  // above AND the badge ring below (chalk: pos 50)
#define SUN_CORE_RADIUS 8
#define SUN_RAY_INNER (SUN_CORE_RADIUS + 3)  // rays start here
#define SUN_RAY_OUTER (SUN_CORE_RADIUS + 8)  // rays end here
#define MOON_RADIUS 11
#define MOON_CUT_DX 6  // crescent cutter disc offset from the moon center
#define MOON_CUT_DY -4

// Palia time: 1 in-game minute = 2.5 real seconds
#define PALIA_MINUTE_MS 2500
#define PALIA_CATCHUP_MS 1250  // shortened delay used to re-sync timer phase

// Segment colors, approximating the in-game clock
#define COLOR_MORNING GColorRajah          // dawn 3 AM - 6 AM: warm sunrise
#define COLOR_DAY GColorPictonBlue         // 6 AM - 6 PM: light sky blue
#define COLOR_EVENING GColorBrilliantRose  // dusk 6 PM - 9 PM: pink
#define COLOR_NIGHT GColorDukeBlue         // 9 PM - 3 AM: dark navy
#define COLOR_RIM GColorWhite
#define COLOR_HAND GColorWhite
#define COLOR_HAND_OUTLINE GColorOxfordBlue
#define COLOR_BADGE GColorOxfordBlue

// ============================================================================
// GLOBAL STATE
// ============================================================================

static Window *s_window;
static Layer *s_canvas_layer;

static AppTimer *s_timer = NULL;

// Last drawn values, so we only redraw when something changed on screen
static int s_last_palia_min = -1;
static int s_last_real_min = -1;
static int s_last_clock_style = -1;

// Screen geometry, computed once in window_load from the layer bounds
static GRect s_bounds;
static GPoint s_center;
static int s_half_w;
static int s_half_h;
static GRect s_cover;  // enlarged rect whose circle covers the screen corners

// Reference proportions (tuned at s_min_half == 100) scaled to this screen
static int s_min_half;
static int s_hand_length;
static int s_icon_pos_r;
static int s_badge_r;
#define BADGE_U(v) ((v) * s_badge_r / BADGE_RADIUS)  // badge-internal offsets

// Real-world time shown in the center badge
static char s_real_time_buf[6];  // "12:34"
static char s_ampm_buf[4];       // "AM" / "PM" (only used in 12-hour mode)

// Clock hand
static GPoint s_hand_points[3];
static GPathInfo s_hand_info = {
    .num_points = 3,
    .points = s_hand_points,
};
static GPath *s_hand_path = NULL;

// ============================================================================
// TIME CONVERSION
// ============================================================================

// Integer square root (no floating point). n is tiny here (a squared
// half-screen-diagonal, a few tens of thousands), so plain int math never
// overflows. Seed below the overflow range, then Newton-refine.
static int isqrt(int n) {
  if (n <= 0) {
    return 0;
  }
  int r = n / 2 + 1;
  while (r * r > n || (r + 1) * (r + 1) <= n) {
    r = (r + n / r) / 2;
  }
  return r;
}

// Current Palia in-game minutes past midnight (0..1439), from the real clock.
// Real seconds past the hour map 1:1 onto 24 in-game hours: :00 -> 12:00 AM,
// :15 -> 6:00 AM, :30 -> 12:00 PM, :45 -> 6:00 PM.
static int palia_minutes_now(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (!t) {
    return 0;
  }
  int secs_past_hour = t->tm_min * 60 + t->tm_sec;
  return (secs_past_hour * 24) / 60;
}

static void update_time_buffers(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (!t) {
    return;
  }

  if (clock_is_24h_style()) {
    strftime(s_real_time_buf, sizeof(s_real_time_buf), "%H:%M", t);
    s_ampm_buf[0] = '\0';
  } else {
    strftime(s_real_time_buf, sizeof(s_real_time_buf), "%I:%M", t);
    // Strip leading zero, e.g. "04:35" -> "4:35"
    if (s_real_time_buf[0] == '0') {
      memmove(s_real_time_buf, &s_real_time_buf[1], sizeof(s_real_time_buf) - 1);
    }
    strftime(s_ampm_buf, sizeof(s_ampm_buf), "%p", t);
  }
}

// ============================================================================
// DRAWING
// ============================================================================

// Compute all screen-derived geometry once; the draw functions below read
// these statics instead of re-deriving them every redraw.
static void compute_geometry(void) {
  s_bounds = layer_get_bounds(window_get_root_layer(s_window));
  s_center = GPoint(s_bounds.size.w / 2, s_bounds.size.h / 2);
  s_half_w = s_bounds.size.w / 2;
  s_half_h = s_bounds.size.h / 2;

  // The sector fills need a circle that covers the screen corners: a circle
  // sized to the screen only reaches its side midpoints (radius = half the
  // longest side). Use radius = half diagonal (+1 slack); the layer clips
  // the overdraw back to the screen.
  int half_diag = isqrt(s_half_w * s_half_w + s_half_h * s_half_h) + 1;
  s_cover = GRect(s_center.x - half_diag, s_center.y - half_diag,
                  2 * half_diag, 2 * half_diag);

  // Scale the emery-tuned reference proportions to the actual screen
  s_min_half = s_half_w < s_half_h ? s_half_w : s_half_h;
  s_hand_length = HAND_LENGTH * s_min_half / 100;
  s_icon_pos_r = PBL_IF_ROUND_ELSE(ICON_POS_RADIUS_ROUND, ICON_POS_RADIUS) *
                 s_min_half / 100;
  s_badge_r = BADGE_RADIUS * s_min_half / 100;
  if (s_badge_r < BADGE_RADIUS) {
    s_badge_r = BADGE_RADIUS;  // keep the digital time readable on small screens
  }
}

static void draw_cycle(GContext *ctx) {
  // Palia's day/night cycle (see palia.wiki.gg/wiki/Guide:Time_Passage_in_Palia):
  //   Dawn/Morning: 3 AM - 6 AM   (3 in-game hours = 45 degrees)
  //   Day:          6 AM - 6 PM   (12 hours = 180 degrees)
  //   Dusk/Evening: 6 PM - 9 PM   (3 hours = 45 degrees)
  //   Night:        9 PM - 3 AM   (6 hours = 90 degrees)
  // Angles start at 12 o'clock (noon on this dial) and increase clockwise:
  // 6 PM = 1/4, 9 PM = 3/8, 3 AM = 5/8, 6 AM = 3/4.
  //
  // inset_thickness is measured inward from the outside, so use more than
  // the circle radius to fill all the way to the center.
  const int32_t eighth = TRIG_MAX_ANGLE / 8;
  const uint16_t full_inset = s_cover.size.h;

  graphics_context_set_fill_color(ctx, COLOR_EVENING);
  graphics_fill_radial(ctx, s_cover, GOvalScaleModeFillCircle, full_inset,
                       2 * eighth, 3 * eighth);

  graphics_context_set_fill_color(ctx, COLOR_NIGHT);
  graphics_fill_radial(ctx, s_cover, GOvalScaleModeFillCircle, full_inset,
                       3 * eighth, 5 * eighth);

  graphics_context_set_fill_color(ctx, COLOR_MORNING);
  graphics_fill_radial(ctx, s_cover, GOvalScaleModeFillCircle, full_inset,
                       5 * eighth, 6 * eighth);

  // Day spans 6 AM -> noon -> 6 PM across the top; it wraps past 360°, so
  // draw it as two fills.
  graphics_context_set_fill_color(ctx, COLOR_DAY);
  graphics_fill_radial(ctx, s_cover, GOvalScaleModeFillCircle, full_inset,
                       6 * eighth, TRIG_MAX_ANGLE);
  graphics_fill_radial(ctx, s_cover, GOvalScaleModeFillCircle, full_inset,
                       0, 2 * eighth);
}

static void draw_ticks(GContext *ctx) {
  // One tick per in-game hour (24 around the dial), drawn on the SCREEN
  // PERIMETER like the classic "Modern" face: each tick starts on the actual
  // rectangular screen edge along its angular ray and points inward, so the
  // markings trace the rectangle instead of a circle. Bold at 12 AM / 6 AM /
  // noon / 6 PM. Midnight sits at the bottom, so tick angle = half turn
  // clockwise plus one 24th per hour.
  const int32_t hour_step = TRIG_MAX_ANGLE / 24;

  for (int h = 0; h < 24; h++) {
    bool bold = (h % 6) == 0;
    int tick_len = (bold ? 16 : 10) * s_min_half / 100;

    int32_t angle = (TRIG_MAX_ANGLE / 2 + h * hour_step) % TRIG_MAX_ANGLE;
    int32_t sinv = sin_lookup(angle);
    int32_t cosv = cos_lookup(angle);
    int32_t ax = sinv < 0 ? -sinv : sinv;
    int32_t ay = cosv < 0 ? -cosv : cosv;

    GPoint outer;
#if PBL_ROUND
    // Round screens (chalk, gabbro): the perimeter is a circle, so ticks sit
    // on a ring just inside the screen edge instead of the bounding square.
    const int ring_r = s_min_half - 2;
    outer.x = s_center.x + (sinv * ring_r) / TRIG_MAX_RATIO;
    outer.y = s_center.y - (cosv * ring_r) / TRIG_MAX_RATIO;
#else
    // Point where the ray from the center exits the rectangle. Pick the edge
    // the ray reaches FIRST by comparing time-to-edge along each axis
    // (half_w/|sin| vs half_h/|cos|, cross-multiplied to stay in integers —
    // the half-extents differ on non-square screens like emery).
    if (s_half_w * ay <= s_half_h * ax) {
      // Exits through the left or right edge
      outer.x = s_center.x + (sinv >= 0 ? s_half_w : -s_half_w);
      outer.y = s_center.y - (cosv * s_half_w) / ax;
    } else {
      // Exits through the top or bottom edge
      outer.x = s_center.x + (sinv * s_half_h) / ay;
      outer.y = s_center.y - (cosv >= 0 ? s_half_h : -s_half_h);
    }
    // Guard against the computed point slipping a pixel past the edge
    if (outer.x < 0) {
      outer.x = 0;
    } else if (outer.x > s_bounds.size.w - 1) {
      outer.x = s_bounds.size.w - 1;
    }
    if (outer.y < 0) {
      outer.y = 0;
    } else if (outer.y > s_bounds.size.h - 1) {
      outer.y = s_bounds.size.h - 1;
    }
#endif

    // Step back toward the center along the same ray
    GPoint inner = {
        outer.x - (sinv * tick_len) / TRIG_MAX_RATIO,
        outer.y + (cosv * tick_len) / TRIG_MAX_RATIO,
    };

    graphics_context_set_stroke_color(ctx, COLOR_RIM);
    graphics_context_set_stroke_width(ctx, bold ? 3 : 2);
    graphics_draw_line(ctx, inner, outer);
  }
}

static void draw_sun(GContext *ctx) {
  // White sun with rays at the noon (top) position, like the in-game clock
  GPoint pos = GPoint(s_center.x, s_center.y - s_icon_pos_r);

  graphics_context_set_fill_color(ctx, COLOR_RIM);
  graphics_fill_circle(ctx, pos, SUN_CORE_RADIUS);

  graphics_context_set_stroke_color(ctx, COLOR_RIM);
  graphics_context_set_stroke_width(ctx, 2);
  for (int i = 0; i < 8; i++) {
    int32_t angle = (i * TRIG_MAX_ANGLE) / 8;
    GPoint inner = {
        pos.x + (sin_lookup(angle) * SUN_RAY_INNER) / TRIG_MAX_RATIO,
        pos.y - (cos_lookup(angle) * SUN_RAY_INNER) / TRIG_MAX_RATIO,
    };
    GPoint outer = {
        pos.x + (sin_lookup(angle) * SUN_RAY_OUTER) / TRIG_MAX_RATIO,
        pos.y - (cos_lookup(angle) * SUN_RAY_OUTER) / TRIG_MAX_RATIO,
    };
    graphics_draw_line(ctx, inner, outer);
  }
}

static void draw_moon(GContext *ctx) {
  // White crescent moon at the midnight (bottom) position: a full disc with
  // an offset disc of the night-sky color cutting the crescent
  GPoint pos = GPoint(s_center.x, s_center.y + s_icon_pos_r);

  graphics_context_set_fill_color(ctx, COLOR_RIM);
  graphics_fill_circle(ctx, pos, MOON_RADIUS);

  graphics_context_set_fill_color(ctx, COLOR_NIGHT);
  graphics_fill_circle(ctx, GPoint(pos.x + MOON_CUT_DX, pos.y + MOON_CUT_DY),
                       MOON_RADIUS);
}

static void draw_stars(GContext *ctx) {
  // A few small stars scattered in the Night sector. Offsets are reference
  // pixels for emery (s_min_half == 100) and scale with the screen.
  static const GPoint star_offsets[] = {
      {-38, 62}, {30, 75}, {-18, 92},
  };
  static const int star_radii[] = {2, 2, 1};

  graphics_context_set_fill_color(ctx, COLOR_RIM);
  for (size_t i = 0; i < ARRAY_LENGTH(star_offsets); i++) {
    graphics_fill_circle(
        ctx,
        GPoint(s_center.x + star_offsets[i].x * s_min_half / 100,
               s_center.y + star_offsets[i].y * s_min_half / 100),
        (star_radii[i] * s_min_half + 50) / 100);
  }
}

static void draw_hand(GContext *ctx) {
  // Needle points from the center outward. Midnight (0 min) points straight
  // down, 6 AM left, noon up, 6 PM right. The canvas sources the time itself
  // so system-triggered redraws (not just timer ticks) stay correct.
  int palia_min = palia_minutes_now();
  int32_t angle =
      (TRIG_MAX_ANGLE / 2 + (int32_t)palia_min * TRIG_MAX_ANGLE / 1440) %
      TRIG_MAX_ANGLE;

  gpath_rotate_to(s_hand_path, angle);
  gpath_move_to(s_hand_path, s_center);

  graphics_context_set_fill_color(ctx, COLOR_HAND);
  gpath_draw_filled(ctx, s_hand_path);

  graphics_context_set_stroke_color(ctx, COLOR_HAND_OUTLINE);
  graphics_context_set_stroke_width(ctx, 1);
  gpath_draw_outline(ctx, s_hand_path);
}

static void draw_center_badge(GContext *ctx) {
  graphics_context_set_fill_color(ctx, COLOR_BADGE);
  graphics_fill_circle(ctx, s_center, s_badge_r);

  graphics_context_set_stroke_color(ctx, COLOR_RIM);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_circle(ctx, s_center, s_badge_r);

  // Real-world digital time, centered. In 12-hour mode the AM/PM suffix is
  // drawn underneath at a smaller size, so the time sits slightly high; in
  // 24-hour mode there is no suffix line, so use a larger font that fills
  // the badge better. Rects scale with the badge radius.
  graphics_context_set_text_color(ctx, COLOR_RIM);
  GFont time_font;
  GRect time_rect;
  if (s_ampm_buf[0] != '\0') {
    time_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    time_rect = GRect(s_center.x - BADGE_U(34), s_center.y - BADGE_U(16),
                      BADGE_U(68), BADGE_U(22));
  } else {
    time_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    time_rect = GRect(s_center.x - BADGE_U(40), s_center.y - BADGE_U(17),
                      BADGE_U(80), BADGE_U(26));
  }
  graphics_draw_text(ctx, s_real_time_buf, time_font, time_rect,
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  if (s_ampm_buf[0] != '\0') {
    GRect ampm_rect = GRect(s_center.x - BADGE_U(34), s_center.y + BADGE_U(6),
                            BADGE_U(68), BADGE_U(16));
    graphics_draw_text(ctx, s_ampm_buf,
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       ampm_rect, GTextOverflowModeWordWrap,
                       GTextAlignmentCenter, NULL);
  }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  // Full-screen day/night cycle and perimeter ticks
  draw_cycle(ctx);
  draw_ticks(ctx);

  // Sun, moon and stars (drawn under the hand, like the in-game clock)
  draw_sun(ctx);
  draw_moon(ctx);
  draw_stars(ctx);

  // Hand + center badge on top
  draw_hand(ctx);
  draw_center_badge(ctx);
}

// ============================================================================
// UPDATE SCHEDULING
// ============================================================================

// Recompute everything shown on screen; redraw only when a value changed
// (unless forced). Returns true when a redraw was requested.
static bool refresh_display(bool force) {
  int palia_min = palia_minutes_now();
  int real_min = -1;
  int clock_style = clock_is_24h_style() ? 1 : 0;

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (t) {
    real_min = t->tm_min;
  }

  if (!force && palia_min == s_last_palia_min &&
      real_min == s_last_real_min && clock_style == s_last_clock_style) {
    return false;
  }

  s_last_palia_min = palia_min;
  s_last_real_min = real_min;
  s_last_clock_style = clock_style;
  update_time_buffers();
  if (s_canvas_layer) {
    layer_mark_dirty(s_canvas_layer);
  }
  return true;
}

// One Palia minute passes every 2.5 real seconds. Redraw only when a value
// on screen changed; if we woke mid-interval (nothing changed) shorten the
// next delay to re-sync to the true 2.5s boundary.
static void timer_callback(void *data) {
  s_timer = NULL;
  s_timer = app_timer_register(
      refresh_display(false) ? PALIA_MINUTE_MS : PALIA_CATCHUP_MS,
      timer_callback, NULL);
}

// ============================================================================
// WINDOW HANDLERS
// ============================================================================

static void window_load(Window *window) {
  compute_geometry();

  Layer *window_layer = window_get_root_layer(window);
  s_canvas_layer = layer_create(layer_get_bounds(window_layer));
  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);

  // Needle shape: long thin triangle pointing up, rotated per update. The
  // wide base sits under the center badge.
  s_hand_points[0] = GPoint(0, -s_hand_length);        // tip
  s_hand_points[1] = GPoint(HAND_BASE_WIDTH, HAND_BASE_Y);   // right base
  s_hand_points[2] = GPoint(-HAND_BASE_WIDTH, HAND_BASE_Y);  // left base
  s_hand_path = gpath_create(&s_hand_info);

  refresh_display(true);
}

static void window_unload(Window *window) {
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  if (s_hand_path) {
    gpath_destroy(s_hand_path);
    s_hand_path = NULL;
  }
  if (s_canvas_layer) {
    layer_destroy(s_canvas_layer);
    s_canvas_layer = NULL;
  }
}

// ============================================================================
// APPLICATION LIFECYCLE
// ============================================================================

static void init(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = window_load,
                                           .unload = window_unload,
                                       });
  window_stack_push(s_window, true);

  // Kick off the update loop: first wake after one Palia minute
  s_timer = app_timer_register(PALIA_MINUTE_MS, timer_callback, NULL);
}

static void deinit(void) {
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  if (s_window) {
    window_destroy(s_window);
    s_window = NULL;
  }
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
