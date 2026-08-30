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
      - a small "*" is shown for a few seconds after every successful sync

    Bottom line: Korean lunar date + current solar term (절기) + holiday name,
    from the CYD clock's korean_calendar.h (tables are copied verbatim, so
    refresh that file from the CYD project when the years run out).
    USE_HANGUL selects the rendering:
      1 (default): 굴림 12px Korean (u8g2_font_gulim12_t_korean2, ~60 KB flash)
           "2026-08-30 일"            weekday inverted on Sunday / public holiday
           "추석  음 8.15        추분"  [holiday(inverted) | festival] lunar  term
      0: ASCII only, no extra font
           "Sun 30 Aug 2026"
           "L 7.11             Ipchu"  "L+" = leap month, term in Revised Romanization
    In both modes the term is drawn inverted on the day it begins (CYD: green),
    and red days (CYD: red weekday) become an inverted weekday.
*/

#include <WiFi.h>
#include <U8g2lib.h>
#include "time.h"
#include "esp_sntp.h"
#include "korean_calendar.h"

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
#define USE_HANGUL           1                    // 1: Korean bottom line (gulim 12px), 0: ASCII only

#if USE_HANGUL
#define FONT_KO   u8g2_font_gulim12_t_korean2     // 2446 KS X 1001 syllables (korean1 lacks 곡망백복윤춘충칩토헌휴)
#define DATE_Y    12                              // baselines: 12px Korean line needs the extra room
#define TIME_Y    49
#define BOTTOM_Y  61
#else
#define DATE_Y    16
#define TIME_Y    52
#define BOTTOM_Y  62
#endif

// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const char* weekDays[7]   = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
const char* weekDaysKo[7] = { "일", "월", "화", "수", "목", "금", "토" };
const char* months[12]  = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// Revised-Romanization names for TERM_KR[] (korean_calendar.h), same order.
static const char* const TERM_ASCII[24] = {
  "Sohan", "Daehan", "Ipchun", "Usu", "Gyeongchip", "Chunbun", "Cheongmyeong", "Gogu",
  "Ipha", "Soman", "Mangjong", "Haji", "Soseo", "Daeseo", "Ipchu", "Cheoseo",
  "Baengno", "Chubun", "Hanro", "Sanggang", "Ipdong", "Soseol", "Daeseol", "Dongji",
};

// ---- Boot state machine ---------------------------------------------------
enum boot_state_t { BOOT_WIFI, BOOT_NTP, BOOT_DONE };
static boot_state_t boot_state = BOOT_WIFI;
static uint32_t     boot_t0;
static uint32_t     wifi_attempt_ms;
static int          wifi_attempts = 1;

static volatile uint32_t last_sync_ms = 0;   // millis() of the last SNTP sync (0 = never)
static int               last_drawn_sec = -1;

// ---- Per-day calendar info (lunar date, solar term, red day) --------------
// Recomputed only when the date changes; the lookups are table scans.
static struct {
  int         yday  = -1;      // tm_yday of the cached day (-1 = none)
  int         year  = -1;
  char        lunar[16];       // "음 7.11" / "음 윤7.11"  (ASCII: "L 7.11" / "L+7.11"), "" outside the table
  const char* term  = nullptr; // term name (Korean or ASCII) or nullptr
  bool        term_today = false;
  bool        red_day    = false;
  const char* event = nullptr; // holiday / festival name (Korean only) or nullptr
  bool        event_holiday = false;  // true: public holiday (inverted), false: festival (plain)
} day_info;

static void update_day_info(const struct tm & t) {
  if (t.tm_yday == day_info.yday && t.tm_year == day_info.year) return;
  day_info.yday = t.tm_yday;
  day_info.year = t.tm_year;

  klc_date_t ld;
  bool have_lunar = klc_solar_to_lunar(&t, &ld);
  if (have_lunar) {
#if USE_HANGUL
    snprintf(day_info.lunar, sizeof(day_info.lunar), "음 %s%d.%d",
             ld.leap ? "윤" : "", ld.month, ld.day);
#else
    snprintf(day_info.lunar, sizeof(day_info.lunar), "L%c%d.%d",
             ld.leap ? '+' : ' ', ld.month, ld.day);
#endif
  } else {
    day_info.lunar[0] = '\0';
  }

  const char* kr = kst_current_term(&t, &day_info.term_today);
#if USE_HANGUL
  day_info.term = kr;
#else
  // kst_current_term() returns a pointer into TERM_KR[]; map it to the
  // ASCII table by index so korean_calendar.h stays identical to the CYD copy.
  day_info.term = nullptr;
  if (kr) {
    for (int i = 0; i < 24; i++) {
      if (kr == TERM_KR[i]) { day_info.term = TERM_ASCII[i]; break; }
    }
  }
#endif

  day_info.red_day = kr_is_red_day(&t);

  // Holiday name first (inverted), otherwise a seasonal festival (plain).
  uint32_t ymd = (uint32_t)(t.tm_year + 1900) * 10000u + (uint32_t)(t.tm_mon + 1) * 100u + (uint32_t)t.tm_mday;
  day_info.event = kr_holiday_name(ymd);
  day_info.event_holiday = day_info.event != nullptr;
  if (!day_info.event && have_lunar) day_info.event = kr_lunar_festival(&ld);

  Serial.printf("Day info: %s term=%s%s red=%d event=%s\n", day_info.lunar,
                day_info.term ? day_info.term : "-", day_info.term_today ? "(today)" : "",
                day_info.red_day, day_info.event ? day_info.event : "-");
}

// Draw text with a filled box behind it and the glyphs cut out (mono "highlight").
static void draw_str_inverted(int x, int y, const char* s) {
  int w = u8g2.getUTF8Width(s);
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

// Two-line status screen used while booting
static void draw_status(const char * line1, const char * line2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
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
//
// USE_HANGUL=1                                  USE_HANGUL=0
//   y  2..14  "2026-08-30 일"        !           y  4..18  "Sun 30 Aug 2026"     !
//   y 26..49  "23:59:59*"                        y 28..52  "23:59:59*"
//   y 51..63  "추석  음 8.15      추분"           y 53..63  "L 7.11        Ipchu"
// Weekday inverted on red days, term inverted on the day it begins,
// holiday name inverted; "!" at top-right while Wi-Fi is down.
static void draw_clock(const struct tm & t) {
  char dateStr[32], timeStr[16];

  update_day_info(t);

  bool recently_synced = last_sync_ms != 0 && (millis() - last_sync_ms) < SYNC_MARK_MS;
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d%s",
           t.tm_hour, t.tm_min, t.tm_sec, recently_synced ? "*" : "");

  u8g2.clearBuffer();

  // ---- Date line
  u8g2.setFont(u8g2_font_8x13B_tf);
#if USE_HANGUL
  snprintf(dateStr, sizeof(dateStr), "%d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  u8g2.drawStr(1, DATE_Y, dateStr);
  u8g2.setFont(FONT_KO);
  draw_str_hl(1 + u8g2.getStrWidth(dateStr) + 8, DATE_Y, weekDaysKo[t.tm_wday], day_info.red_day);
#else
  snprintf(dateStr, sizeof(dateStr), "%d %s %d", t.tm_mday, months[t.tm_mon], t.tm_year + 1900);
  const char* wd = weekDays[t.tm_wday];
  draw_str_hl(1, DATE_Y, wd, day_info.red_day);
  u8g2.drawStr(1 + u8g2.getStrWidth(wd) + 8, DATE_Y, dateStr);
#endif

  // ---- Time
  u8g2.setFont(u8g2_font_logisoso24_tr);
  u8g2.drawStr(8, TIME_Y, timeStr);

  // ---- Bottom line: [event] lunar ........ term
  // Widest first: the event name always fits; the term is dropped when the
  // line would overflow, then the lunar date.
#if USE_HANGUL
  u8g2.setFont(FONT_KO);
#else
  u8g2.setFont(u8g2_font_6x12_tf);
#endif
  {
    const int GAP = 8, W = 128 - 2;
    int x = 1;
    int ev_w    = day_info.event    ? u8g2.getUTF8Width(day_info.event) + GAP : 0;
    int lunar_w = day_info.lunar[0] ? u8g2.getUTF8Width(day_info.lunar)       : 0;
    int term_w  = day_info.term     ? u8g2.getUTF8Width(day_info.term)        : 0;
    bool show_term  = term_w  && ev_w + lunar_w + (lunar_w ? GAP : 0) + term_w <= W;
    bool show_lunar = lunar_w && ev_w + lunar_w <= W;

    if (day_info.event) {
      draw_str_hl(x, BOTTOM_Y, day_info.event, day_info.event_holiday);
      x += ev_w;
    }
    if (show_lunar) u8g2.drawUTF8(x, BOTTOM_Y, day_info.lunar);
    if (show_term)  draw_str_hl(128 - 1 - term_w, BOTTOM_Y, day_info.term, day_info.term_today);
  }

  // Wi-Fi dropped: small marker at the top-right corner (SNTP resumes on
  // its own once WiFi.setAutoReconnect brings the link back)
  if (WiFi.status() != WL_CONNECTED) {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(122, DATE_Y, "!");
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
