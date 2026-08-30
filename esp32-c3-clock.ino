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

    Screen (a 128x64 rendition of the CYD digital face, everything centred):

        2026-08-30 (일)          DSEG7 Italic 11px date + 굴림 12px weekday,
                                 weekday inverted on Sunday / public holiday
                        PM       DSEG14 Italic 11px (TIME_12H)
        11:58           ──       DSEG7 Bold Italic 28px HH:MM
                        42       DSEG7 Italic 11px seconds
        추석  음 8.15  추분       굴림 12px: [holiday(inverted) | festival]
                                 lunar date - "음"/"음 윤" (leap month) in 굴림,
                                 the numbers in the DSEG7 11px - and the solar
                                 term (inverted on the day it begins)

      - top-left corner: "!" while Wi-Fi is down, "*" for a few seconds
        after every successful SNTP sync
      - the calendar data (lunar 2025-2045, KST solar terms 2026-2035,
        public holidays 2026-2030) is korean_calendar.h copied verbatim from
        the CYD clock; refresh it from there when the years run out
      - the DSEG fonts are generated from the TTFs by tools/gen_fonts.sh
        (U8g2 format, ~800 bytes total, all sheared to the same ~14 degree
        slant); the Korean font is U8g2's built-in
        u8g2_font_gulim12_t_korean2 (~60 KB; korean1 lacks 11 needed glyphs)
*/

#include <WiFi.h>
#include <U8g2lib.h>
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
#define SYNC_MARK_MS         (5 * 1000)           // how long "*" stays after a sync
#define TIME_12H             1                    // 1: "11:58" + AM/PM, 0: "23:58"

// ---- Fonts / layout -------------------------------------------------------
#define FONT_KO      u8g2_font_gulim12_t_korean2  // 12px Hangul, ascent 10 / descent 2
#define FONT_DATE    font_dseg7_i_11              // 11px tall slanted digits, "0-9" "-" "."
#define FONT_TIME    font_dseg7_bi_28             // 29px tall digits, "0-9" ":" " "
#define FONT_SEC     font_dseg7_i_11
#define FONT_AMPM    font_dseg14_i_11             // 10px tall "A" "M" "P"
#define FONT_STATUS  u8g2_font_6x12_tf            // boot screen, "!" / "*"

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

// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const char* weekDaysKo[7] = { "일", "월", "화", "수", "목", "금", "토" };

// ---- Boot state machine ---------------------------------------------------
enum boot_state_t { BOOT_WIFI, BOOT_NTP, BOOT_DONE };
static boot_state_t boot_state = BOOT_WIFI;
static uint32_t     boot_t0;
static uint32_t     wifi_attempt_ms;
static int          wifi_attempts = 1;

static volatile uint32_t last_sync_ms = 0;   // millis() of the last SNTP sync (0 = never)
static int               last_drawn_sec = -1;

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
  last_sync_ms = millis();
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
  u8g2.setFont(FONT_SEC);
  int col_w = adv_width(secStr);
#if TIME_12H
  u8g2.setFont(FONT_AMPM);
  int ampm_w = adv_width(ampm);
  if (ampm_w > col_w) col_w = ampm_w;
#endif
  x = (SCREEN_W - (time_w + COL_GAP + col_w)) / 2;
  u8g2.setFont(FONT_TIME);
  u8g2.drawStr(x, TIME_Y, timeStr);
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

  // ---- Status corner (top-left, the date starts at x=7):
  // "!" while Wi-Fi is down (SNTP resumes on its own once WiFi.setAutoReconnect
  // brings the link back), "*" briefly after a successful sync.
  bool recently_synced = last_sync_ms != 0 && (millis() - last_sync_ms) < SYNC_MARK_MS;
  if (WiFi.status() != WL_CONNECTED || recently_synced) {
    u8g2.setFont(FONT_STATUS);
    u8g2.drawStr(0, DATE_Y, WiFi.status() != WL_CONNECTED ? "!" : "*");
  }

  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  u8g2.begin();
  draw_status("Connecting to Wi-Fi...", "");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  boot_t0 = wifi_attempt_ms = millis();
}

void loop() {
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
    draw_clock(t);
  }
  delay(50);
}
