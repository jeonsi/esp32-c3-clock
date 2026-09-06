/*  ESP32-C3 + 0.96" 128x64 OLED (SH1106 / SSD1306, I2C) clock

    Time keeping follows the CYD clock (cyd_clock_brightness.ino):
      - two time sources, chosen with a very long press (3 s) of the BOOT
        button and stored in NVS (applies via an immediate restart, since
        the radio stacks are only initialised at boot):
          Wi-Fi: SNTP disciplines the system clock (configTzTime +
            SNTP_SYNC_MODE_SMOOTH), resync every NTP_SYNC_INTERVAL_MS
          BLE: Current Time Service from a paired iPhone (ble_time.h,
            copied from the CYD clock; needs NimBLE-Arduino 2.x and the
            "Huge APP" partition scheme - both stacks are in the binary).
            With BLE_DUTY_CYCLE the radio runs only around each resync
            (~25 mA saved): bonds survive in NVS, so the iPhone reconnects
            by itself whenever advertising restarts
      - an 8x8 icon at the bottom-left shows the selected source (BT rune /
        Wi-Fi arcs), blinks once per second while the link is down (or, in
        BLE mode, while a resync window waits for the phone), and turns
        into an inverted box once no sync has landed for SYNC_STALE_MS
        (2 intervals) - the time shown is then getting stale
      - non-blocking boot: the OLED shows Wi-Fi / BLE / sync progress and
        retries forever instead of hanging in a blind while() loop
      - the display is redrawn on the second boundary (polled every 50 ms),
        not on a drifting delay(1000)

    BOOT button: a short press cycles the faces, a double click switches
    12/24-hour time, a long press (1 s) toggles night dimming, a very long
    press (3 s) switches the time source (BLE <-> Wi-Fi, restarts); each
    change shows a banner. All four settings are remembered in NVS like
    the CYD clock. On the ESP32-C3 the BOOT button (GPIO9) is also the OLED's
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
                        PM       DSEG14 11px (12-hour mode only)
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

      - night dimming: between NIGHT_FROM_HOUR and NIGHT_TO_HOUR the panel
        is dimmed (there is no light sensor, so this goes by the clock,
        unlike the CYD's LDR). The contrast command alone (0x81) barely
        changes these panels - U8g2's init sets the pre-charge period to its
        maximum (0xD9 = 0xF1), which dominates - so night mode lowers
        pre-charge, VCOMH and contrast together and restores the init values
        by day. NIGHT_DITHER additionally lights only every other pixel;
        NIGHT_OFF switches the panel off instead.
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
#include "esp_bt.h"     // esp_bt_controller_mem_release() on Wi-Fi boots
#include "korean_calendar.h"
#include "clock_fonts.h"
// ble_time.h is included after the tunables below - it needs TZ_INFO,
// NTP_SYNC_INTERVAL_MS and BLE_DEVICE_NAME.

// Wi-Fi credentials live in secrets.h (gitignored).
// Copy secrets.h.example to secrets.h and fill in your own.
#include "secrets.h"
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ---- Tunables (same values as the CYD clock_config.h) ---------------------
#define TZ_INFO              "KST-9"              // POSIX TZ: UTC+9, no DST
#define NTP_SYNC_INTERVAL_MS (60 * 60 * 1000)     // resync every hour (SNTP and BLE CTS alike)
#define SYNC_STALE_MS        (2UL * NTP_SYNC_INTERVAL_MS)  // no sync for this long -> inverted source icon
#define WIFI_RETRY_MS        (30 * 1000)          // re-issue WiFi.begin() every 30 s
#define TIME_SYNC_BLE        0                    // first-boot default source: 1 = BLE CTS, 0 = Wi-Fi SNTP (NVS "tsrc")
#define BLE_DEVICE_NAME      "ESP32-C3 Clock"     // shown in the iPhone's Bluetooth list
#define BLE_DUTY_CYCLE       1                    // 1: BLE radio on only around each resync, 0: always on (CYD style)
#define BLE_LINGER_MS        (60 * 1000)          // UPPER BOUND on-time after a sync (first pairing needs the ANCS prompt)
#define BLE_SETTLE_MS        (2 * 1000)           // once synced AND ANCS-subscribed, close the window after this instead
#define BLE_SYNC_TIMEOUT_MS  (3 * 60 * 1000)      // close a fruitless resync window after this, retry next interval
#define TIME_12H_DEFAULT     1                    // 1: "11:58" + AM/PM, 0: "23:58" - until toggled (stored in NVS)
#define DISPLAY_FLIP         0                    // 1: rotate the screen 180 degrees (clock mounted upside down)
#define FACE_BUTTON_PIN      9                    // BOOT button (= OLED SCL, see above); any free pin -> GND also works; -1 = none
#define FACE_CYCLE_S         0                    // >0: also switch faces automatically every N seconds
#define FACE_DEFAULT         FACE_DIGITAL         // face used until the button is pressed once
#define LONG_PRESS_MS        1000                 // hold this long to toggle night mode instead of the face
#define SRC_PRESS_MS         3000                 // hold this long to switch the time source (BLE/Wi-Fi) and restart
#define DOUBLE_CLICK_MS      400                  // second click within this = toggle 12/24 h (single click acts after it)
#define MSG_MS               2000                 // how long the "야간 모드 켜짐/꺼짐" banner stays
#define NIGHT_ENABLE_DEFAULT 1                    // night dimming on until toggled (stored in NVS)
// Source-side overrides for the button-set settings, for when the BOOT
// button is hard to reach. -1 = leave the NVS-stored value alone; any other
// value is written to NVS on every boot. Flash once with the value you want,
// then set it back to -1 - left in place, the button change is undone on
// each reboot (the value sticks either way, it is stored in NVS).
#define OVERRIDE_FACE        -1                   // 0 = digital, 1 = analog
#define OVERRIDE_NIGHT       -1                   // 0 = night dimming off, 1 = on
#define OVERRIDE_12H         -1                   // 0 = 24-hour, 1 = 12-hour
#define OVERRIDE_TIME_SRC    -1                   // 0 = Wi-Fi SNTP, 1 = BLE CTS

// Night dimming (by the clock; the C3 board has no light sensor)
#define NIGHT_FROM_HOUR      20                   // dim from 20:00 ...
#define NIGHT_TO_HOUR        7                    // ... until 07:00 (may wrap past midnight)
// Night panel settings (day restores U8g2's init values 0xCF / 0xF1 / 0x40).
// Brightness is mostly set by the pre-charge phase-2 length (0xD9 high
// nibble, 1..15) and only fine-tuned by contrast; 0x11 + VCOMH 0 + contrast 0
// is black on some panels, so start from these and adjust to taste.
#define CONTRAST_NIGHT       0x80                 // 0x81: 0..255. On this panel 0x40 is barely visible, 0x80 a comfortable night level
#define NIGHT_PRECHARGE      0x11                 // 0xD9: phase2<<4 | phase1, each 1..15 (init 0xF1)
#define NIGHT_VCOMH          0x00                 // 0xDB: 0x00 / 0x20 / 0x30 (init 0x40)
#define NIGHT_DITHER         0                    // 1: also light only every other pixel at night (checkerboard)
#define NIGHT_OFF            0                    // 1: turn the panel off at night instead of dimming

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
#define DATE_GAP     4                            // date .. (weekday)
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

#include "ble_time.h"

// The SH1106 driver also drives SSD1306 modules, so it is the safe pick for
// both panels this clock has run on (1.3" SH1106 and 0.96" SSD1306): U8g2's
// SH1106_128X64_NONAME reuses the SSD1306 init sequence (including the
// SSD1306-only charge-pump command 0x8D, which an SH1106 ignores), and its
// per-page writes with the SH1106's +2 column offset are neutralised on an
// SSD1306 by the horizontal-addressing mode that same init sets up. The
// reverse is not true - the SSD1306 driver uses commands the SH1106 lacks.
// DISPLAY_FLIP rotates everything 180 degrees in the frame buffer (U8G2_R2).
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(DISPLAY_FLIP ? U8G2_R2 : U8G2_R0, U8X8_PIN_NONE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(DISPLAY_FLIP ? U8G2_R2 : U8G2_R0, U8X8_PIN_NONE);

const char* weekDaysKo[7] = { "일", "월", "화", "수", "목", "금", "토" };

// ---- Faces ----------------------------------------------------------------
enum face_t { FACE_DIGITAL, FACE_ANALOG, FACE_COUNT };
static face_t      face_mode = FACE_DEFAULT;
static Preferences prefs;
static bool        time_sync_ble = (TIME_SYNC_BLE != 0);   // NVS "tsrc"; applied at boot
static bool        ble_radio_on  = false;
static uint32_t    ble_window_t0 = 0;             // when the current radio window opened
static volatile uint32_t last_sync_ok_ms = 0;     // millis() of the last successful sync (0 = none since boot)

// 8x8 time-source icons for the top-left corner (XBM, LSB = leftmost pixel)
static const uint8_t ICON_BT[8]   = { 0x08, 0x18, 0x26, 0x1C, 0x1C, 0x26, 0x18, 0x08 };
static const uint8_t ICON_WIFI[8] = { 0x7E, 0x81, 0x3C, 0x42, 0x00, 0x18, 0x18, 0x00 };
#define ICON_AREA 10   // icon column at the bottom-left: 8 px icon + 2 px gap
#define ICON_Y    53   // icon rows 53..60, centred on the bottom line's Hangul body

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
  static bool     seen_high  = false;         // fail-safe: a pin stuck low (miswired, no pull-up) is ignored
  static uint32_t press_ms   = 0;             // when the confirmed press began
  static bool     click_pending = false;      // one short click seen, waiting for a possible second
  static uint32_t click_ms   = 0;
  int level = digitalRead(FACE_BUTTON_PIN);
  // A lone short click acts once the double-click window has closed.
  if (click_pending && !button_down && millis() - click_ms > DOUBLE_CLICK_MS) {
    click_pending = false;
    next_face(true);
  }
  if (level == HIGH) seen_high = true;
  if (level != last_level) {
    last_level = level;
    t_change = millis();
    return;
  }
  if (millis() - t_change < 40) return;
  if (level == LOW) {
    if (seen_high && millis() - t_change < 8000) {   // confirmed press; a >8 s "press" is a stuck pin, not one
      if (!button_down) press_ms = t_change;
      button_down = true;
    } else {
      button_down = false;
    }
  } else if (button_down) {
    button_down = false;                      // confirmed release: t_change is the release time
    if (t_change - press_ms >= SRC_PRESS_MS) {
      click_pending = false;
      toggle_source();                        // restarts, does not return
    } else if (t_change - press_ms >= LONG_PRESS_MS) {
      click_pending = false;
      toggle_night();
    } else if (click_pending && t_change - click_ms <= DOUBLE_CLICK_MS) {
      click_pending = false;                  // second click: 12/24 h
      toggle_12h();
    } else {
      click_pending = true;                   // first click: wait for a second one
      click_ms = t_change;
    }
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
  last_sync_ok_ms = millis();
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

// ---- Night dimming --------------------------------------------------------
static bool is_night(int hour) {
  if (NIGHT_FROM_HOUR == NIGHT_TO_HOUR) return false;
  if (NIGHT_FROM_HOUR < NIGHT_TO_HOUR)  return hour >= NIGHT_FROM_HOUR && hour < NIGHT_TO_HOUR;
  return hour >= NIGHT_FROM_HOUR || hour < NIGHT_TO_HOUR;       // wraps past midnight
}

static bool     night_now     = false;
static bool     night_enabled = NIGHT_ENABLE_DEFAULT;
static bool     time_12h      = TIME_12H_DEFAULT;
static uint32_t msg_until_ms  = 0;          // banner visible while millis() < this
static const char* msg_text   = nullptr;

static void show_banner(const char* text) {
  msg_text = text;
  msg_until_ms = millis() + MSG_MS;
  last_drawn_sec = -1;                      // redraw now
}

// Switch the time source and restart - the radio stacks (Wi-Fi / NimBLE)
// are only initialised at boot, like on the CYD.
static void toggle_source(void) {
  time_sync_ble = !time_sync_ble;
  prefs.putInt("tsrc", time_sync_ble ? 1 : 0);
  Serial.printf("Time source -> %s, restarting\n", time_sync_ble ? "BLE" : "WIFI");
  const char* text = time_sync_ble ? "BLE 모드로 재시작" : "Wi-Fi 모드로 재시작";
  u8g2.clearBuffer();
  u8g2.setFont(FONT_KO);
  u8g2.drawUTF8((SCREEN_W - adv_width(text)) / 2, SCREEN_H / 2 + 5, text);
  u8g2.sendBuffer();
  delay(1500);
  ESP.restart();
}

static void toggle_12h(void) {
  time_12h = !time_12h;
  prefs.putInt("h12", time_12h ? 1 : 0);
  show_banner(time_12h ? "12시간제" : "24시간제");
  Serial.printf("Time format: %s\n", time_12h ? "12h" : "24h");
}

static void toggle_night(void) {
  night_enabled = !night_enabled;
  prefs.putInt("night", night_enabled ? 1 : 0);
  show_banner(night_enabled ? "야간 모드 켜짐" : "야간 모드 꺼짐");   // also re-applies brightness
  Serial.printf("Night mode: %s\n", night_enabled ? "on" : "off");
}

static void panel_cmd2(uint8_t cmd, uint8_t arg) {
  u8x8_t* u8x8 = u8g2.getU8x8();
  u8x8_cad_StartTransfer(u8x8);
  u8x8_cad_SendCmd(u8x8, cmd);
  u8x8_cad_SendArg(u8x8, arg);
  u8x8_cad_EndTransfer(u8x8);
}

static void panel_cmd1(uint8_t cmd) {
  u8x8_t* u8x8 = u8g2.getU8x8();
  u8x8_cad_StartTransfer(u8x8);
  u8x8_cad_SendCmd(u8x8, cmd);
  u8x8_cad_EndTransfer(u8x8);
}

// Called once per redraw; only talks to the panel when the state changes.
// Some panels (the 1.3" SH1106 module, unlike the 0.96" one) go black when
// 0xD9/0xDB/0x81 are rewritten while the display is running, so (a) the
// first "day" application is skipped - after u8g2.begin() the panel is
// already in the day state - and (b) real transitions are wrapped in
// display-off/on, the same condition the init sequence writes them under.
static void apply_brightness(const struct tm & t) {
  static int applied = -1;              // -1 = nothing sent yet
  int want = (night_enabled && is_night(t.tm_hour)) ? 1 : 0;
  if (want == applied) return;
#if !NIGHT_OFF
  if (applied == -1 && want == 0) {     // boot in daytime: panel is already there
    applied = 0;
    night_now = false;
    return;
  }
#endif
  applied = want;
  night_now = want;
#if NIGHT_OFF
  u8g2.setPowerSave(want);              // 1 = display off (RAM kept, redraws continue unseen)
#else
  panel_cmd1(0xAE);                     // display off while the analog settings change
  if (want) {
    panel_cmd2(0xD9, NIGHT_PRECHARGE);  // pre-charge period
    panel_cmd2(0xDB, NIGHT_VCOMH);      // VCOMH deselect level
    panel_cmd2(0x81, CONTRAST_NIGHT);   // contrast
  } else {                              // values from U8g2's ssd1306/sh1106 128x64 noname init sequence
    panel_cmd2(0xD9, 0xF1);
    panel_cmd2(0xDB, 0x40);
    panel_cmd2(0x81, 0xCF);
  }
  panel_cmd1(0xAF);                     // display on
#endif
  Serial.printf("Brightness: %s\n", want ? "night" : "day");
}

// Optional software dimming: keep only a checkerboard of the frame buffer.
// Each buffer byte is 8 vertical pixels of one column, so alternate the
// mask per column (0x55 / 0xAA) to get the pattern.
static void dither_buffer(void) {
#if NIGHT_DITHER
  if (!night_now) return;
  uint8_t* buf = u8g2.getBufferPtr();
  int n = 8 * u8g2.getBufferTileHeight() * u8g2.getBufferTileWidth();
  for (int i = 0; i < n; i++) buf[i] &= (i & 1) ? 0xAA : 0x55;
#endif
}

// Inverted banner across the middle of the screen (night mode toggled).
static void draw_banner(void);

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

// Status screen used while booting (line3 optional)
static void draw_status(const char * line1, const char * line2, const char * line3 = "") {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_STATUS);
  u8g2.drawStr(0, 20, line1);
  u8g2.drawStr(0, 38, line2);
  u8g2.drawStr(0, 56, line3);
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
        if (time_sync_ble) {
          snprintf(buf, sizeof(buf), "%lus  (%s)",
                   (unsigned long)((millis() - boot_t0) / 1000),
                   ble_time_connected() ? "connected" : "advertising");
          draw_status("Waiting for BLE sync...", buf, "Pair: iPhone > Bluetooth");
        } else {
          snprintf(buf, sizeof(buf), "%lus",
                   (unsigned long)((millis() - boot_t0) / 1000));
          draw_status("Waiting for time sync...", buf);
        }
      }
      break;
    }

    default:
      break;
  }
}

// ---- BLE duty cycle --------------------------------------------------------
// With BLE_DUTY_CYCLE the radio runs only around each resync: once a sync has
// landed the stack is stopped (bonds live in NVS and survive) and one
// NTP_SYNC_INTERVAL_MS later it is restarted - the bonded iPhone sees the
// advertising and reconnects on its own (usually within seconds, sometimes
// tens of seconds). Once the sync has landed AND the ANCS subscription
// succeeded (iOS refuses the CCCD write until 알림 공유 is granted, so success
// doubles as "not mid-first-pairing"), the window closes BLE_SETTLE_MS later -
// keeping it open longer only invites connect/drop churn on a flaky link.
// Without a subscription it stays open up to BLE_LINGER_MS past the sync so
// the very first pairing can finish the ANCS ("알림 공유") step, and a window
// that never syncs (phone away) closes after BLE_SYNC_TIMEOUT_MS.
static void ble_duty_poll(void) {
#if BLE_DUTY_CYCLE
  static uint32_t last_count = 0;   // cts_sync_count already credited
  static uint32_t synced_ms  = 0;   // when this window's sync landed (0 = not yet)
  static uint32_t next_ms    = 0;   // radio off: when to open the next window
  if (!ble_radio_on) {
    if ((int32_t)(millis() - next_ms) >= 0) {
      Serial.println("BLE: radio on for resync");
      cts_synced_once = false;      // back to the 10 s retry cadence inside the window
      ble_time_begin();
      ble_radio_on  = true;
      ble_window_t0 = millis();
      synced_ms = 0;
    }
    return;
  }
  ble_time_tick();                  // periodic CTS read while the radio is on
  if (cts_sync_count != last_count) {
    last_count = cts_sync_count;
    last_sync_ok_ms = millis();
    if (!synced_ms) synced_ms = millis();
  }
  bool early = synced_ms && ancs_subscribed && !ancs_busy &&
               millis() - synced_ms >= BLE_SETTLE_MS;
  if (synced_ms && (early || millis() - synced_ms >= BLE_LINGER_MS)) {
    ble_time_end();
    ble_radio_on = false;
    next_ms = synced_ms + NTP_SYNC_INTERVAL_MS;
    Serial.println(early ? "BLE: synced and subscribed - radio off early"
                         : "BLE: radio off until the next resync");
  } else if (!synced_ms && boot_state == BOOT_DONE &&
             millis() - ble_window_t0 >= BLE_SYNC_TIMEOUT_MS) {
    // during boot (no valid time yet) the window stays open indefinitely
    ble_time_end();
    ble_radio_on = false;
    next_ms = millis() + NTP_SYNC_INTERVAL_MS;
    Serial.println("BLE: no sync in this window, retrying next interval");
  }
#else
  ble_time_tick();
  static uint32_t seen_count = 0;
  if (cts_sync_count != seen_count) {
    seen_count = cts_sync_count;
    last_sync_ok_ms = millis();
  }
#endif
}

// Time-source icon in the bottom-left corner (like the CYD's link icon):
// BT rune or Wi-Fi arcs for the
// selected source, blinking once per second while its link is down. In BLE
// mode the radio is off most of the time by design (duty cycle) - that idle
// state shows a steady icon; it only blinks while a window is waiting.
// Once nothing has synced for SYNC_STALE_MS (counted from boot when no sync
// has landed yet, e.g. after a soft restart with a still-valid clock) the
// icon becomes an inverted box: the displayed time is free-running.
static void draw_source_icon(const struct tm & t) {
  const uint8_t* icon = time_sync_ble ? ICON_BT : ICON_WIFI;
  if (millis() - last_sync_ok_ms > SYNC_STALE_MS) {
    u8g2.drawBox(0, ICON_Y - 1, 10, 10);
    u8g2.setDrawColor(0);
    u8g2.drawXBM(1, ICON_Y, 8, 8, icon);
    u8g2.setDrawColor(1);
    return;
  }
  bool up = time_sync_ble ? (ble_radio_on ? ble_time_connected() : true)
                          : (WiFi.status() == WL_CONNECTED);
  if (!up && (t.tm_sec & 1)) return;
  u8g2.drawXBM(1, ICON_Y, 8, 8, icon);
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
  draw_source_icon(t);

  // ---- Row 2: HH:MM big, AM/PM over seconds in a narrow column, all centred
  const char* ampm = t.tm_hour < 12 ? "AM" : "PM";
  if (time_12h) {
    int hh = t.tm_hour % 12;
    if (hh == 0) hh = 12;
    snprintf(timeStr, sizeof(timeStr), "%2d:%02d", hh, t.tm_min);   // leading blank = one digit wide
  } else {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", t.tm_hour, t.tm_min);
  }
  snprintf(secStr, sizeof(secStr), "%02d", t.tm_sec);

  u8g2.setFont(FONT_TIME);
  int time_w = adv_width(timeStr);
  // 12-hour mode: the leading cell only ever holds a '1' or nothing, and
  // the '1' is just segments B/C at the far right of its cell. Reserve only
  // that ink (like the CYD face): shrink the field by the '1' glyph's left
  // bearing and draw the string that much further left, so the blank part
  // of the leading cell hangs outside the field and nothing looks pushed right.
  int lead_blank = 0;
  if (time_12h) {
    lead_blank = u8g2_GetXOffsetGlyph(u8g2.getU8g2(), '1');
    time_w -= lead_blank;
  }
  u8g2.setFont(FONT_SEC);
  int col_w = adv_width(secStr);
  if (time_12h) {
    u8g2.setFont(FONT_AMPM);
    int ampm_w = adv_width(ampm);
    if (ampm_w > col_w) col_w = ampm_w;
  }
  x = (SCREEN_W - (time_w + COL_GAP + col_w)) / 2;
  u8g2.setFont(FONT_TIME);
  u8g2.drawStr(x - lead_blank, TIME_Y, timeStr);
  int col_x = x + time_w + COL_GAP;
  if (time_12h) {
    u8g2.setFont(FONT_AMPM);
    u8g2.drawStr(col_x, AMPM_Y, ampm);
  }
  u8g2.drawHLine(col_x, SEP_Y, col_w);
  u8g2.setFont(FONT_SEC);
  u8g2.drawStr(col_x, SEC_Y, secStr);

  // ---- Row 3: [event] lunar term, centred in the space right of the
  // source icon. The event name always fits; the term is dropped when the
  // line would overflow, then the lunar date (with the icon reserving 10 px,
  // the term drops for slightly more holiday names than before; the lunar
  // date still fits them all). "음"/"음 윤" in Hangul, digits in DSEG.
  {
    const int W = SCREEN_W - ICON_AREA - 2;
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

    x = ICON_AREA + (SCREEN_W - ICON_AREA - total) / 2;
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

  draw_banner();
  dither_buffer();
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
  draw_source_icon(t);

  draw_banner();
  dither_buffer();
  u8g2.sendBuffer();
}

static void draw_banner(void) {
  if (!msg_text || (int32_t)(millis() - msg_until_ms) >= 0) return;
  u8g2.setFont(FONT_KO);
  int w = adv_width(msg_text);
  int x = (SCREEN_W - w) / 2;
  int y = SCREEN_H / 2 + 4;                 // baseline: 12 px glyphs centred on the screen
  u8g2.setDrawColor(1);
  u8g2.drawBox(x - 6, y - 13, w + 12, 18);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(x, y, msg_text);
  u8g2.setDrawColor(1);
}

void setup() {
  // 80 MHz is plenty for a 1 Hz clock face and saves a few mA over the
  // 160 MHz default (the C3 maximum); Wi-Fi/BLE need at least 80 MHz.
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  Serial.printf("CPU %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
  u8g2.begin();
  oled_clear_ram();

#if FACE_BUTTON_PIN >= 0
  // SDA/SCL are runtime constants in the ESP32 core (not macros - a #if
  // against them silently compares with 0). An I2C pin is already
  // input-enabled and pulled up, and pinMode() on it would break the bus.
  if (FACE_BUTTON_PIN != SDA && FACE_BUTTON_PIN != SCL) {
    pinMode(FACE_BUTTON_PIN, INPUT_PULLUP);
  }
  Serial.printf("Face button on GPIO%d%s, level %d\n", FACE_BUTTON_PIN,
                (FACE_BUTTON_PIN == SDA || FACE_BUTTON_PIN == SCL) ? " (shared with I2C)" : "",
                digitalRead(FACE_BUTTON_PIN));
#endif
  prefs.begin("clock", false);
  // Push source-side OVERRIDE_* values (see the tunables) into NVS.
#if OVERRIDE_FACE >= 0
  prefs.putInt("face", OVERRIDE_FACE);
  Serial.printf("Override: face = %s\n", OVERRIDE_FACE ? "analog" : "digital");
#endif
#if OVERRIDE_NIGHT >= 0
  prefs.putInt("night", OVERRIDE_NIGHT);
  Serial.printf("Override: night mode %s\n", OVERRIDE_NIGHT ? "on" : "off");
#endif
#if OVERRIDE_12H >= 0
  prefs.putInt("h12", OVERRIDE_12H);
  Serial.printf("Override: %s format\n", OVERRIDE_12H ? "12h" : "24h");
#endif
#if OVERRIDE_TIME_SRC >= 0
  prefs.putInt("tsrc", OVERRIDE_TIME_SRC);
  Serial.printf("Override: time source %s\n", OVERRIDE_TIME_SRC ? "BLE" : "WIFI");
#endif
  int f = prefs.getInt("face", (int)FACE_DEFAULT);
  face_mode = (f >= 0 && f < FACE_COUNT) ? (face_t)f : FACE_DEFAULT;
  night_enabled = prefs.getInt("night", NIGHT_ENABLE_DEFAULT) != 0;
  time_12h      = prefs.getInt("h12", TIME_12H_DEFAULT) != 0;
  time_sync_ble = prefs.getInt("tsrc", TIME_SYNC_BLE) != 0;
  Serial.printf("Night mode: %s, time format: %s, time source: %s\n",
                night_enabled ? "on" : "off", time_12h ? "12h" : "24h",
                time_sync_ble ? "BLE" : "WIFI");

  boot_t0 = wifi_attempt_ms = millis();
  if (time_sync_ble) {
    draw_status("Starting BLE...", "");
    ble_time_begin();
    ble_radio_on  = true;
    ble_window_t0 = millis();
    boot_state = BOOT_NTP;               // no Wi-Fi stage; wait for the first CTS read
    draw_status("Waiting for BLE sync...", "", "Pair: iPhone > Bluetooth");
  } else {
    // The linked-in BLE controller statically reserves DRAM even when it is
    // never initialized. A Wi-Fi boot keeps BLE off until the next reboot
    // (switching the time source reboots anyway), so hand that memory back
    // to the heap - same patch as the CYD clock, where LVGL needed it; here
    // it is simply free RAM.
    uint32_t heap_before = ESP.getFreeHeap();
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);   // C3: BLE-only controller
    Serial.printf("Free heap: %u -> %u after BT release\n",
                  (unsigned)heap_before, (unsigned)ESP.getFreeHeap());
    draw_status("Connecting to Wi-Fi...", "");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, password);
  }
}

void loop() {
  button_poll();
  if (time_sync_ble) ble_duty_poll();   // CTS resync + radio duty cycle
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
    apply_brightness(t);
    if (face_mode == FACE_ANALOG) draw_analog(t);
    else                          draw_clock(t);
  }
  delay(50);
}
