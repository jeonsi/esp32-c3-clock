/*  ESP32-C3 + 0.96" 128x64 OLED (SH1106 / SSD1306, I2C) clock

    Time keeping follows the CYD clock (cyd_clock_brightness.ino):
      - time is kept by the ESP32 system clock and disciplined by SNTP
        (configTzTime + SNTP_SYNC_MODE_SMOOTH), so it stays accurate
        between syncs and is slewed instead of jumping when a sync lands
      - resync every NTP_SYNC_INTERVAL_MS against three servers
      - non-blocking boot: the OLED shows Wi-Fi / NTP progress and retries
        forever instead of hanging in a blind while() loop
      - the display is redrawn on the second boundary (polled every 50 ms),
        not on a drifting delay(1000)

    Two faces, cycled with the BOOT button and remembered in NVS like the
    CYD clock. On the ESP32-C3 the BOOT button (GPIO9) is also the OLED's
    SCL (default Wire pins are SDA 8 / SCL 9), so the pin is never
    reconfigured with pinMode() - that freezes the display. It is only
    sampled with digitalRead() between frames, when the bus is idle and the
    line sits high; a press pulls it low. Nothing is sent to the OLED while
    the button is held, and the face changes on release. FACE_BUTTON_PIN can
    also be a separate button to GND on any free pin, or FACE_CYCLE_S can
    alternate the faces automatically.

    Digital (a 128x64 rendition of the CYD digital face, everything centred):

        2026-08-30 (일)          DSEG7 11px date + 굴림 12px weekday,
                                 weekday inverted on Sunday / public holiday
                        PM       DSEG14 11px (TIME_12H)
        11:58           ──       DSEG7 Bold 28px HH:MM
                        42       DSEG7 11px seconds
        추석  음 8.15  추분       굴림 12px: [holiday(inverted) | festival]
                                 lunar date - "음"/"음 윤" (leap month) in 굴림,
                                 the numbers in the DSEG7 11px - and the solar
                                 term (inverted on the day it begins)

    Analog: like the CYD analog face, one big dial and nothing else - 62 px
      ring centred on the screen, 12 hour ticks, 12/3/6/9 numerals, tapered
      hour and minute hands, a thin second hand. The minute hand creeps
      0.1 degree per second and the hour hand 0.5 degree per minute.

      - Wi-Fi drops and SNTP syncs are only logged to Serial; the faces
        themselves have no status marker
      - the calendar data (lunar 2025-2045, KST solar terms 2026-2035,
        public holidays 2026-2030) is korean_calendar.h copied verbatim from
        the CYD clock; refresh it from there when the years run out
      - the DSEG fonts (upright faces; the italics looked messy at this
        resolution) are generated from the TTFs by tools/gen_fonts.sh
        (U8g2 format, ~500 bytes total); the Korean font is U8g2's built-in
        u8g2_font_gulim12_t_korean2 (~60 KB; korean1 lacks 11 needed glyphs)
*/

#include <WiFi.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <math.h>
#include "time.h"
#include "esp_sntp.h"
#include "korean_calendar.h"
#include "clock_fonts.h"

// Wi-Fi credentials live in secrets.h (gitignored).
// Copy secrets.h.example to secrets.h and fill in your own.
#include "secrets.h"
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ---- Tunables (same values as the CYD clock_config.h) ---------------------
#define TZ_INFO              "KST-9"              // POSIX TZ: UTC+9, no DST
#define NTP_SYNC_INTERVAL_MS (30 * 60 * 1000)     // resync every 30 minutes
#define WIFI_RETRY_MS        (30 * 1000)          // re-issue WiFi.begin() every 30 s
#define TIME_12H             1                    // 1: "11:58" + AM/PM, 0: "23:58"
#define FACE_BUTTON_PIN      9                    // BOOT button (= OLED SCL, see above); any free pin -> GND also works; -1 = none
#define FACE_CYCLE_S         0                    // >0: also switch faces automatically every N seconds
#define FACE_DEFAULT         FACE_DIGITAL         // face used until the button is pressed once
#define BUTTON_SHARES_I2C    (FACE_BUTTON_PIN == SDA || FACE_BUTTON_PIN == SCL)

// ---- Fonts / layout -------------------------------------------------------
#define FONT_KO      u8g2_font_gulim12_t_korean2  // 12px Hangul, ascent 10 / descent 2
#define FONT_DATE    font_dseg7_r_11              // 12px tall digits, "0-9" "-" "."
#define FONT_TIME    font_dseg7_b_28              // 29px tall digits, "0-9" ":" " "
#define FONT_SEC     font_dseg7_r_11
#define FONT_AMPM    font_dseg14_r_11             // 10px tall "A" "M" "P"
#define FONT_STATUS  u8g2_font_6x12_tf            // boot screen

// Baselines. Row 1 spans y 0..14, row 3 y 51..63; the 36 px band between
// them holds the 29 px time centred (y 18..46). The 11 px DSEG digits sit one
// row lower than the Hangul baseline so they centre on the Hangul body.
#define DATE_Y       12                           // "(일)"
#define DATE_NUM_Y   13                           // "2026-08-30"
#define TIME_Y       47
#define AMPM_Y       29                           // top of AM/PM (y 18..28) aligned with the digits' top
#define SEP_Y        31                           // rule between AM/PM and seconds
#define SEC_Y        47                           // seconds bottom-aligned with the digits
#define BOTTOM_Y     61                           // Hangul
#define BOTTOM_NUM_Y 62                           // lunar digits
#define DATE_GAP     6                            // date .. (weekday)
#define LUNAR_GAP    3                            // "음" .. "8.15"
#define COL_GAP      4                            // HH:MM .. AM/PM-seconds column
#define PART_GAP     8                            // between bottom-line items
#define SCREEN_W     128
#define SCREEN_H     64

// Analog face
#define FONT_DIAL    u8g2_font_5x7_tf             // 12 / 3 / 6 / 9
#define DIAL_CX      64
#define DIAL_CY      32
#define DIAL_R       31                           // outer ring
#define TICK_R1      29                           // hour ticks, from..to
#define TICK_R2      27
#define NUM_R        21                           // centre of the numerals
#define HOUR_LEN     14
#define HOUR_HALF_W  2                            // half base width of the tapered hand
#define MIN_LEN      21
#define MIN_HALF_W   1
#define SEC_LEN      25
#define SEC_TAIL     5
#define HUB_R        2

// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const char* weekDaysKo[7] = { "일", "월", "화", "수", "목", "금", "토" };

// ---- Faces ----------------------------------------------------------------
enum face_t { FACE_DIGITAL, FACE_ANALOG, FACE_COUNT };
static face_t      face_mode = FACE_DEFAULT;
static Preferences prefs;

// ---- Boot state machine ---------------------------------------------------
enum boot_state_t { BOOT_WIFI, BOOT_NTP, BOOT_DONE };
static boot_state_t boot_state = BOOT_WIFI;
static uint32_t     boot_t0;
static uint32_t     wifi_attempt_ms;
static int          wifi_attempts = 1;

static int          last_drawn_sec = -1;

static void set_face(face_t f, bool save) {
  face_mode = f;
  if (save) prefs.putInt("face", (int)f);   // button presses only - the auto cycle would wear NVS
  last_drawn_sec = -1;                      // redraw now
  Serial.printf("Face: %s\n", f == FACE_ANALOG ? "analog" : "digital");
}

static void next_face(bool save) {
  set_face((face_t)((face_mode + 1) % FACE_COUNT), save);
}

// Push button: cycle faces on each press. A level must hold for 40 ms (two
// 50 ms polls) to count, and the face changes on RELEASE, so that when the
// button shares the I2C clock line the redraw goes out on a free bus.
// button_down is true while a confirmed press is held; loop() sends nothing
// to the OLED during that time.
static bool button_down = false;

// Optional auto cycle every FACE_CYCLE_S seconds for boards without a button.
static void button_poll(void) {
#if FACE_CYCLE_S > 0
  static uint32_t last_cycle_ms = 0;
  if (millis() - last_cycle_ms > FACE_CYCLE_S * 1000UL) {
    last_cycle_ms = millis();
    next_face(false);
  }
#endif
#if FACE_BUTTON_PIN >= 0
  static int      last_level = HIGH;
  static uint32_t t_change   = 0;
  int level = digitalRead(FACE_BUTTON_PIN);
  if (level != last_level) {
    last_level = level;
    t_change = millis();
    return;
  }
  if (millis() - t_change < 40) return;
  if (level == LOW) {
    button_down = true;                       // confirmed press
  } else if (button_down) {
    button_down = false;                      // confirmed release
    next_face(true);
  }
#endif
}

// ---- Per-day calendar info (lunar date, solar term, red day, event) -------
// Recomputed only when the date changes; the lookups are table scans.
static struct {
  int         yday  = -1;      // tm_yday of the cached day (-1 = none)
  int         year  = -1;
  char        lunar_kr[8];     // "음" / "음 윤" (leap month), "" outside the table
  char        lunar_num[8];    // "7.11"
  const char* term  = nullptr; // solar term name or nullptr
  bool        term_today = false;
  bool        red_day    = false;
  const char* event = nullptr; // holiday / festival name or nullptr
  bool        event_holiday = false;  // true: public holiday (inverted), false: festival (plain)
} day_info;

static void update_day_info(const struct tm & t) {
  if (t.tm_yday == day_info.yday && t.tm_year == day_info.year) return;
  day_info.yday = t.tm_yday;
  day_info.year = t.tm_year;

  klc_date_t ld;
  bool have_lunar = klc_solar_to_lunar(&t, &ld);
  if (have_lunar) {
    snprintf(day_info.lunar_kr,  sizeof(day_info.lunar_kr),  "음%s", ld.leap ? " 윤" : "");
    snprintf(day_info.lunar_num, sizeof(day_info.lunar_num), "%d.%d", ld.month, ld.day);
  } else {
    day_info.lunar_kr[0] = day_info.lunar_num[0] = '\0';
  }

  day_info.term    = kst_current_term(&t, &day_info.term_today);
  day_info.red_day = kr_is_red_day(&t);

  // Holiday name first (inverted), otherwise a seasonal festival (plain).
  uint32_t ymd = (uint32_t)(t.tm_year + 1900) * 10000u + (uint32_t)(t.tm_mon + 1) * 100u + (uint32_t)t.tm_mday;
  day_info.event = kr_holiday_name(ymd);
  day_info.event_holiday = day_info.event != nullptr;
  if (!day_info.event && have_lunar) day_info.event = kr_lunar_festival(&ld);

  Serial.printf("Day info: %s %s term=%s%s red=%d event=%s\n", day_info.lunar_kr, day_info.lunar_num,
                day_info.term ? day_info.term : "-", day_info.term_today ? "(today)" : "",
                day_info.red_day, day_info.event ? day_info.event : "-");
}

void time_sync_notification_cb(struct timeval * tv) {
  (void)tv;
  struct tm t;
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  Serial.printf("NTP sync: %s\n", buf);
}

static void sntp_begin(void) {
  sntp_set_time_sync_notification_cb(time_sync_notification_cb);
  sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);   // slew the clock instead of jumping
  configTzTime(TZ_INFO, "kr.pool.ntp.org", "pool.ntp.org", "time.google.com");
  sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
  sntp_restart();                              // apply the new interval
}

// ---- Drawing helpers ------------------------------------------------------
// Width of a (UTF-8) string as drawn: the sum of the glyph advances.
// Not U8g2's getStrWidth / getUTF8Width: those are built with
// U8G2_BALANCED_STR_WIDTH_CALCULATION, which adds the first glyph's left
// bearing again on the right. DSEG's "1" is only its right-hand segments
// (x offset 16), so "12:05" would report 111 px for 98 px of ink and push
// the whole time block off the right edge.
static int adv_width(const char* s) {
  int w = 0;
  while (*s) {
    uint8_t c = (uint8_t)*s++;
    uint16_t enc;
    if (c < 0x80)              enc = c;
    else if ((c & 0xE0) == 0xC0) { enc = (c & 0x1F) << 6;  enc |= (uint8_t)*s++ & 0x3F; }
    else if ((c & 0xF0) == 0xE0) { enc = (c & 0x0F) << 12; enc |= ((uint8_t)*s++ & 0x3F) << 6; enc |= (uint8_t)*s++ & 0x3F; }
    else { while (((uint8_t)*s & 0xC0) == 0x80) s++; continue; }   // 4-byte sequences: not in our fonts
    w += u8g2_GetGlyphWidth(u8g2.getU8g2(), enc);
  }
  return w;
}

// Draw text with a filled box behind it and the glyphs cut out (mono "highlight").
static void draw_str_inverted(int x, int y, const char* s) {
  int w = adv_width(s);
  int a = u8g2.getAscent();
  int d = u8g2.getDescent();    // negative
  u8g2.drawBox(x - 1, y - a - 1, w + 2, a - d + 2);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(x, y, s);
  u8g2.setDrawColor(1);
}

static void draw_str_hl(int x, int y, const char* s, bool highlight) {
  if (highlight) draw_str_inverted(x, y, s);
  else           u8g2.drawUTF8(x, y, s);
}

// The SH1106 has 132 columns of RAM and U8g2's driver only ever writes
// columns 2..129 (the window most 1.3" modules show). A module that shows
// RAM columns 0..127 keeps whatever columns 0 and 1 held at power-up - a
// stray ":"-like speck at the left edge that no clearBuffer() can touch.
// Zero all 132 columns once, in 12-byte I2C transfers.
static void oled_clear_ram(void) {
  u8x8_t* u8x8 = u8g2.getU8x8();
  static uint8_t zeros[12] = { 0 };
  for (uint8_t page = 0; page < 8; page++) {
    u8x8_cad_StartTransfer(u8x8);
    u8x8_cad_SendCmd(u8x8, 0xB0 | page);   // page address
    u8x8_cad_SendCmd(u8x8, 0x00);          // column address, low nibble  = 0
    u8x8_cad_SendCmd(u8x8, 0x10);          // column address, high nibble = 0
    u8x8_cad_EndTransfer(u8x8);
    for (int col = 0; col < 132; col += sizeof(zeros)) {
      u8x8_cad_StartTransfer(u8x8);
      u8x8_cad_SendData(u8x8, sizeof(zeros), zeros);
      u8x8_cad_EndTransfer(u8x8);
    }
  }
}

// Two-line status screen used while booting
static void draw_status(const char * line1, const char * line2) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_STATUS);
  u8g2.drawStr(0, 24, line1);
  u8g2.drawStr(0, 44, line2);
  u8g2.sendBuffer();
}

static void boot_poll(void) {
  static uint32_t last_ui_ms = 0;
  char buf[32];

  switch (boot_state) {

    case BOOT_WIFI:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("Connected to Wi-Fi network with IP Address: ");
        Serial.println(WiFi.localIP());
        sntp_begin();
        boot_t0 = millis();
        draw_status("Waiting for time sync...", "");
        boot_state = BOOT_NTP;
        break;
      }
      if (millis() - wifi_attempt_ms > WIFI_RETRY_MS) {
        wifi_attempt_ms = millis();
        wifi_attempts++;
        Serial.printf("Wi-Fi retry #%d\n", wifi_attempts);
        WiFi.disconnect();
        WiFi.begin(ssid, password);
      }
      if (millis() - last_ui_ms > 1000) {
        last_ui_ms = millis();
        snprintf(buf, sizeof(buf), "%lus  (try %d)",
                 (unsigned long)((millis() - boot_t0) / 1000), wifi_attempts);
        draw_status("Connecting to Wi-Fi...", buf);
      }
      break;

    case BOOT_NTP: {
      struct tm t;
      if (getLocalTime(&t, 0)) {
        boot_state = BOOT_DONE;
        last_drawn_sec = -1;   // force an immediate first draw
        break;
      }
      if (millis() - last_ui_ms > 1000) {
        last_ui_ms = millis();
        snprintf(buf, sizeof(buf), "%lus",
                 (unsigned long)((millis() - boot_t0) / 1000));
        draw_status("Waiting for time sync...", buf);
      }
      break;
    }

    default:
      break;
  }
}

// ---- Clock face -----------------------------------------------------------
static void draw_clock(const struct tm & t) {
  char dateStr[16], wdStr[8], timeStr[8], secStr[4];

  update_day_info(t);

  u8g2.clearBuffer();

  // ---- Row 1: "2026-08-30" + "(일)", centred; weekday inverted on red days
  snprintf(dateStr, sizeof(dateStr), "%d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  snprintf(wdStr, sizeof(wdStr), "(%s)", weekDaysKo[t.tm_wday]);
  u8g2.setFont(FONT_DATE);
  int date_w = adv_width(dateStr);
  u8g2.setFont(FONT_KO);
  int wd_w = adv_width(wdStr);
  int x = (SCREEN_W - (date_w + DATE_GAP + wd_w)) / 2;
  u8g2.setFont(FONT_DATE);
  u8g2.drawStr(x, DATE_NUM_Y, dateStr);
  u8g2.setFont(FONT_KO);
  draw_str_hl(x + date_w + DATE_GAP, DATE_Y, wdStr, day_info.red_day);

  // ---- Row 2: HH:MM big, AM/PM over seconds in a narrow column, all centred
#if TIME_12H
  int hh = t.tm_hour % 12;
  if (hh == 0) hh = 12;
  snprintf(timeStr, sizeof(timeStr), "%2d:%02d", hh, t.tm_min);   // leading blank = one digit wide
  const char* ampm = t.tm_hour < 12 ? "AM" : "PM";
#else
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d", t.tm_hour, t.tm_min);
#endif
  snprintf(secStr, sizeof(secStr), "%02d", t.tm_sec);

  u8g2.setFont(FONT_TIME);
  int time_w = adv_width(timeStr);
  // 12-hour mode: the leading cell only ever holds a '1' or nothing, and
  // the '1' is just segments B/C at the far right of its cell. Reserve only
  // that ink (like the CYD face): shrink the field by the '1' glyph's left
  // bearing and draw the string that much further left, so the blank part
  // of the leading cell hangs outside the field and nothing looks pushed right.
  int lead_blank = 0;
#if TIME_12H
  lead_blank = u8g2_GetXOffsetGlyph(u8g2.getU8g2(), '1');
  time_w -= lead_blank;
#endif
  u8g2.setFont(FONT_SEC);
  int col_w = adv_width(secStr);
#if TIME_12H
  u8g2.setFont(FONT_AMPM);
  int ampm_w = adv_width(ampm);
  if (ampm_w > col_w) col_w = ampm_w;
#endif
  x = (SCREEN_W - (time_w + COL_GAP + col_w)) / 2;
  u8g2.setFont(FONT_TIME);
  u8g2.drawStr(x - lead_blank, TIME_Y, timeStr);
  int col_x = x + time_w + COL_GAP;
#if TIME_12H
  u8g2.setFont(FONT_AMPM);
  u8g2.drawStr(col_x, AMPM_Y, ampm);
#endif
  u8g2.drawHLine(col_x, SEP_Y, col_w);
  u8g2.setFont(FONT_SEC);
  u8g2.drawStr(col_x, SEC_Y, secStr);

  // ---- Row 3: [event] lunar term, centred. The event name always fits;
  // the term is dropped when the line would overflow, then the lunar date.
  // The lunar date is "음"/"음 윤" in Hangul followed by DSEG digits.
  {
    const int W = SCREEN_W - 2;
    int kr_w = 0, num_w = 0;
    if (day_info.lunar_num[0]) {
      u8g2.setFont(FONT_KO);   kr_w  = adv_width(day_info.lunar_kr);
      u8g2.setFont(FONT_DATE); num_w = adv_width(day_info.lunar_num);
    }
    u8g2.setFont(FONT_KO);
    int ev_w    = day_info.event ? adv_width(day_info.event) : 0;
    int lunar_w = num_w ? kr_w + LUNAR_GAP + num_w : 0;
    int term_w  = day_info.term  ? adv_width(day_info.term)  : 0;
    int ev_gap  = ev_w ? ev_w + PART_GAP : 0;
    bool show_term  = term_w  && ev_gap + lunar_w + (lunar_w ? PART_GAP : 0) + term_w <= W;
    bool show_lunar = lunar_w && ev_gap + lunar_w <= W;

    int total = 0, n = 0;
    if (ev_w)       { total += ev_w;    n++; }
    if (show_lunar) { total += lunar_w; n++; }
    if (show_term)  { total += term_w;  n++; }
    if (n > 1) total += PART_GAP * (n - 1);

    x = (SCREEN_W - total) / 2;
    if (ev_w) {
      draw_str_hl(x, BOTTOM_Y, day_info.event, day_info.event_holiday);
      x += ev_w + PART_GAP;
    }
    if (show_lunar) {
      u8g2.drawUTF8(x, BOTTOM_Y, day_info.lunar_kr);
      u8g2.setFont(FONT_DATE);
      u8g2.drawStr(x + kr_w + LUNAR_GAP, BOTTOM_NUM_Y, day_info.lunar_num);
      u8g2.setFont(FONT_KO);
      x += lunar_w + PART_GAP;
    }
    if (show_term) draw_str_hl(x, BOTTOM_Y, day_info.term, day_info.term_today);
  }

  u8g2.sendBuffer();
}

// ---- Analog face ----------------------------------------------------------
// Polar helper: angle in degrees clockwise from 12 o'clock.
static void polar(float deg, float r, int & x, int & y) {
  float a = deg * (float)M_PI / 180.0f;
  x = DIAL_CX + (int)lroundf(r * sinf(a));
  y = DIAL_CY - (int)lroundf(r * cosf(a));
}

// Tapered hand: a triangle from a short base across the hub to the tip.
static void draw_hand(float deg, int len, int half_w) {
  float a  = deg * (float)M_PI / 180.0f;
  float px = cosf(a), py = sinf(a);          // unit vector perpendicular to the hand
  int tx, ty; polar(deg, len, tx, ty);
  u8g2.drawTriangle(DIAL_CX + (int)lroundf(px * half_w), DIAL_CY + (int)lroundf(py * half_w),
                    DIAL_CX - (int)lroundf(px * half_w), DIAL_CY - (int)lroundf(py * half_w),
                    tx, ty);
}

static void draw_analog(const struct tm & t) {
  int x, y, x2, y2;
  u8g2.clearBuffer();

  u8g2.drawCircle(DIAL_CX, DIAL_CY, DIAL_R);
  for (int i = 0; i < 12; i++) {
    polar(i * 30.0f, TICK_R1, x, y);
    polar(i * 30.0f, TICK_R2, x2, y2);
    u8g2.drawLine(x, y, x2, y2);
  }
  u8g2.setFont(FONT_DIAL);
  static const char* const nums[4] = { "12", "3", "6", "9" };
  for (int i = 0; i < 4; i++) {
    polar(i * 90.0f, NUM_R, x, y);
    int w = u8g2.getStrWidth(nums[i]);
    u8g2.drawStr(x - w / 2, y + 3, nums[i]);   // 5x7: cap height 7 -> centre on y
  }

  float sec_deg  = t.tm_sec * 6.0f;
  float min_deg  = (t.tm_min + t.tm_sec / 60.0f) * 6.0f;
  float hour_deg = ((t.tm_hour % 12) + t.tm_min / 60.0f) * 30.0f;
  draw_hand(hour_deg, HOUR_LEN, HOUR_HALF_W);
  draw_hand(min_deg,  MIN_LEN,  MIN_HALF_W);
  polar(sec_deg, SEC_LEN, x, y);
  polar(sec_deg + 180.0f, SEC_TAIL, x2, y2);
  u8g2.drawLine(x2, y2, x, y);
  u8g2.drawDisc(DIAL_CX, DIAL_CY, HUB_R);

  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  u8g2.begin();
  oled_clear_ram();
  draw_status("Connecting to Wi-Fi...", "");

#if FACE_BUTTON_PIN >= 0 && !BUTTON_SHARES_I2C
  pinMode(FACE_BUTTON_PIN, INPUT_PULLUP);   // an I2C pin is already input-enabled and pulled up; pinMode() would break the bus
#endif
  prefs.begin("clock", false);
  int f = prefs.getInt("face", (int)FACE_DEFAULT);
  face_mode = (f >= 0 && f < FACE_COUNT) ? (face_t)f : FACE_DEFAULT;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  boot_t0 = wifi_attempt_ms = millis();
}

void loop() {
  button_poll();
  if (button_down) {          // the button may be holding SCL low: don't touch the bus until it is released
    delay(50);
    return;
  }

  if (boot_state != BOOT_DONE) {
    boot_poll();
    delay(50);
    return;
  }

  // Redraw only when the second changes, so the display flips right on
  // the second boundary (never more than ~50 ms late) and never skips one.
  struct tm t;
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  if (t.tm_sec != last_drawn_sec) {
    last_drawn_sec = t.tm_sec;
    if (face_mode == FACE_ANALOG) draw_analog(t);
    else                          draw_clock(t);
  }
  delay(50);
}
