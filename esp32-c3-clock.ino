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
*/

#include <WiFi.h>
#include <U8g2lib.h>
#include "time.h"
#include "esp_sntp.h"

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

// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const char* weekDays[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
const char* months[12]  = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// ---- Boot state machine ---------------------------------------------------
enum boot_state_t { BOOT_WIFI, BOOT_NTP, BOOT_DONE };
static boot_state_t boot_state = BOOT_WIFI;
static uint32_t     boot_t0;
static uint32_t     wifi_attempt_ms;
static int          wifi_attempts = 1;

static volatile uint32_t last_sync_ms = 0;   // millis() of the last SNTP sync (0 = never)
static int               last_drawn_sec = -1;

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
static void draw_clock(const struct tm & t) {
  char dateStr[32], timeStr[16];

  snprintf(dateStr, sizeof(dateStr), "%s %d %s %d",
           weekDays[t.tm_wday], t.tm_mday, months[t.tm_mon], t.tm_year + 1900);

  bool recently_synced = last_sync_ms != 0 && (millis() - last_sync_ms) < SYNC_MARK_MS;
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d%s",
           t.tm_hour, t.tm_min, t.tm_sec, recently_synced ? "*" : "");

  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_8x13B_tf);
  u8g2.drawStr(0, 16, dateStr);

  u8g2.setFont(u8g2_font_logisoso24_tr);
  u8g2.drawStr(8, 52, timeStr);

  // Wi-Fi dropped: small marker in the corner (SNTP resumes on its own
  // once WiFi.setAutoReconnect brings the link back)
  if (WiFi.status() != WL_CONNECTED) {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(122, 63, "!");
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
