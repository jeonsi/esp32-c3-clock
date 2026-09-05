# ESP32-C3 OLED Clock

ESP32-C3와 0.96" 128×64 OLED(SH1106/SSD1306, I2C)로 만든 시계입니다. 시간은 **Wi-Fi(SNTP)** 또는 **아이폰과의 BLE CTS 페어링**으로 동기화합니다. [CYD Clock](https://github.com/jeonsi/cyd_clock_brightness)의 디지털/아날로그 화면을 128×64 흑백 화면에 옮긴 것으로, 시간 관리 방식과 음력·절기·공휴일 데이터(`korean_calendar.h`)를 그대로 가져왔습니다. 터치가 없는 대신 보드의 **BOOT 버튼** 하나로 화면 전환·12/24시간·야간 모드를 조작합니다.

## 화면 구성

### 디지털

```
 ᛒ 2026-08-30 (일)          ← 시간 소스 아이콘 + 날짜(DSEG7 11px) + 한글 요일(굴림 12px). 빨간날은 요일 반전
                    PM      ← AM/PM (DSEG14 11px, 12시간제일 때만)
   11:58           ──       ← HH:MM (DSEG7 Bold 28px)
                    42      ← 초 (DSEG7 11px)
   추석  음 8.15  추분        ← [공휴일(반전) | 세시명절] 음력 날짜 · 현재 절기(절입 당일 반전)
```

- 세 줄 모두 **중앙 정렬**. 시간 블록은 1행과 3행 사이 36px 밴드의 정중앙에 놓입니다.
- 12시간제에서 시의 앞자리는 `1` 아니면 빈칸이므로, CYD처럼 `1`의 잉크 폭만 예약해 한 자리 시(`9:05`)일 때 왼쪽에 죽은 공간이 생기지 않습니다. 24시간제는 `09:05`처럼 두 자리로 표시하고 AM/PM 자리는 비웁니다.
- 하단 줄은 `음`/`음 윤`(윤달)은 한글, 숫자는 DSEG로 표시합니다. 공휴일 이름은 반전, 정월대보름·단오·칠석 같은 세시명절은 일반 글자. 한 줄에 다 들어가지 않으면 절기 → 음력 순으로 생략하고 이름은 항상 남깁니다.
- 색이 없는 흑백 패널이라 CYD의 빨간 요일·초록 절기는 **반전 박스**로 대신합니다.
- 좌상단 8×8 **시간 소스 아이콘**(BLE 룬 / Wi-Fi 아크)은 선택된 동기화 방식을 나타내며, 링크가 끊겨 있는 동안 1초 간격으로 깜박입니다. 아날로그 화면에도 같은 위치에 표시됩니다.

### 아날로그

```
            ,  12  .
         '           '
       9      ●━━━━    3        ← 62px 다이얼 하나만 정중앙 (CYD 아날로그 화면과 같은 구성)
         .     ╲     ,
            '  6   '
```

- 1px 링, 12개 시각 눈금, 12/3/6/9 숫자, 삼각형 시침·분침, 1px 초침(반대쪽 꼬리 포함), 중심 허브
- 분침은 초에 비례해 매초 0.1°, 시침은 매분 0.5°씩 전진

## 주요 기능

### 시간 관리 (SNTP / BLE CTS)
**BOOT 버튼을 3초 이상 누르면** 두 방식을 오갑니다 — NVS에 저장하고 즉시 재부팅해 적용합니다(라디오 스택은 부팅 시에만 초기화). `TIME_SYNC_BLE`은 최초 부팅의 기본값일 뿐입니다(기본: Wi-Fi).

**Wi-Fi (SNTP)**
- ESP32 시스템 클럭을 SNTP로 디시플린. 1시간마다 재동기화, `SNTP_SYNC_MODE_SMOOTH`로 시간이 점프하지 않고 서서히 보정
- NTP 서버: `kr.pool.ntp.org` → `pool.ntp.org` → `time.google.com`, 타임존 `KST-9`

**BLE CTS** (Wi-Fi 미사용 · **아이폰 전용** — 안드로이드는 OS가 CTS/ANCS를 제공하지 않음)
- ESP32가 CTS(0x1805)·ANCS 솔리시테이션을 실어 광고 → **아이폰 설정 > Bluetooth 목록에서 `ESP32-C3 Clock`(=`BLE_DEVICE_NAME`)을 눌러 최초 1회 페어링** → 같은 연결에서 ESP32가 GATT 클라이언트로 아이폰의 Current Time 특성(0x2A2B)을 읽어 시스템 클럭을 맞춤
- 첫 동기화 전에는 10초마다, 그 뒤에는 1시간마다 다시 읽음
- **자동 재연결**: 페어링 후 아이폰의 ANCS 알림 특성을 구독해 시스템이 재연결을 관리하는 알림 액세서리로 등록됨(스마트워치 방식). 최초 1회 "알림 공유" 허용 요청이 뜨며 허용해야 자동 재연결이 동작(알림 데이터는 사용하지 않음)
- CTS가 주는 것은 폰의 현지 시간이므로 `TZ_INFO` 기준으로 해석 — 폰의 시간대가 다르면 그만큼 틀리게 표시됨
- `ble_time.h`는 CYD 리포의 파일을 거의 그대로 복사한 것입니다(설정 include 한 줄만 다름)

공통
- 비차단 부팅: Wi-Fi 연결(또는 BLE 페어링 안내)/시간 동기화 진행 상황을 화면에 표시하고, 실패해도 무한 재시도
- 화면은 초가 바뀌는 순간(50ms 폴링)에만 다시 그림

### BOOT 버튼
| 조작 | 동작 | 배너 |
|---|---|---|
| 짧게 1회 | 디지털 ↔ 아날로그 | — |
| 짧게 2회 (0.4초 내) | 12시간제 ↔ 24시간제 | `12시간제` / `24시간제` |
| 1~3초 | 야간 모드 ON/OFF | `야간 모드 켜짐` / `야간 모드 꺼짐` |
| 3초 이상 | 시간 소스 전환 후 재부팅 | `BLE 모드로 재시작` / `Wi-Fi 모드로 재시작` |

- 네 설정 모두 NVS(`Preferences`)에 저장되어 재부팅 후 유지. 설정이 바뀔 때만 기록
- 한 번 클릭은 두 번째 클릭이 없다는 걸 확인하기 위해 손을 뗀 뒤 0.4초 후 반영
- **BOOT 버튼(GPIO9)은 이 보드에서 OLED의 SCL과 같은 핀**입니다(아래 하드웨어 참고). 그래서 `pinMode()`로 핀을 건드리지 않고 I2C가 쉬는 사이에 레벨만 읽으며, 누르고 있는 동안은 화면 전송을 멈추고 손을 떼는 순간 동작합니다. 핀이 계속 LOW로 읽히는 경우(배선 오류 등)는 무시하는 안전장치가 있어 시계가 멈추지 않습니다.
- 다른 핀에 버튼을 달아도 됩니다(`FACE_BUTTON_PIN`). 버튼이 없으면 `FACE_CYCLE_S`로 화면을 자동 순환시킬 수 있습니다.

### 야간 밝기
- `NIGHT_FROM_HOUR`~`NIGHT_TO_HOUR`(기본 22시~7시) 동안 패널을 어둡게 합니다. 조도 센서가 없어 CYD의 LDR 자동 밝기 대신 시각 기준입니다.
- 이 계열 패널은 컨트라스트 명령(`0x81`)만으로는 밝기가 거의 변하지 않습니다 — U8g2 초기화가 프리차지 기간을 최대(`0xD9 = 0xF1`)로 잡기 때문입니다. 그래서 야간에는 **프리차지·VCOMH·컨트라스트**를 함께 낮추고, 낮에는 초기화 값(`0xF1 / 0x40 / 0xCF`)으로 복원합니다.
- `NIGHT_DITHER 1`이면 야간에 프레임 버퍼를 체커보드로 걸러 켜지는 픽셀을 절반으로 줄이고(더 어둡게), `NIGHT_OFF 1`이면 어둡게 하는 대신 화면을 끕니다.

### 음력 · 절기 · 공휴일
- `korean_calendar.h`는 CYD 리포의 파일을 **수정 없이 복사**한 것입니다. 음력 2025~2045, KST 절기 2026~2035, 공휴일 2026~2030 테이블을 담고 있으며, 범위를 벗어난 연도에는 해당 부분만 표시가 생략됩니다. 연도가 다가오면 CYD 리포의 파일로 덮어쓰면 됩니다.
- 계산은 날짜가 바뀔 때 한 번만 수행합니다.

## 하드웨어

| 항목 | 값 |
|---|---|
| 보드 | ESP32-C3 개발보드 (Arduino 보드 `ESP32C3 Dev Module`) |
| 디스플레이 | 0.96" 128×64 OLED, SH1106 (또는 SSD1306), I2C — SDA **GPIO8**, SCL **GPIO9** (ESP32-C3 Arduino 코어의 기본 `Wire` 핀) |
| 버튼 | 보드의 BOOT 버튼 = **GPIO9** (SCL과 공유, 위 설명 참고) |

주의:

- **GPIO8/9에 `pinMode()`를 호출하면 I2C가 끊겨 화면이 멈춥니다.** 코드에서 버튼 핀이 `SDA`/`SCL`과 같으면 `pinMode()`를 건너뜁니다. (`SDA`/`SCL`은 매크로가 아니라 `static const` 변수라 `#if`로는 비교할 수 없고 런타임 `if`로 검사합니다.)
- U8g2의 SH1106 드라이버는 컨트롤러 RAM 132열 중 2~129열에만 그립니다. RAM 0열부터 표시하는 모듈에서는 0·1열에 전원 인가 시의 무작위 값이 그대로 남아 왼쪽 가장자리에 점이 보이므로, 부팅 시 132열 전체를 한 번 지웁니다(`oled_clear_ram()`). 같은 이유로 오른쪽 2px는 보이지 않을 수 있는데 레이아웃은 x≤124까지만 씁니다.
- SSD1306 모듈이면 스케치 상단의 `U8G2_SSD1306_128X64_NONAME_F_HW_I2C` 생성자로 바꿔 쓰면 됩니다.

## 빌드 방법

Arduino IDE 기준:

1. **보드**: ESP32 Arduino core 3.x (`esp32:esp32:esp32c3`), **Tools > Partition Scheme > "Huge APP (3MB No OTA/1MB SPIFFS)"** — Wi-Fi와 BLE 두 스택이 모두 빌드에 포함되어 기본 앱 파티션(1.31MB)을 넘습니다
2. **라이브러리** (Library Manager): `U8g2`, `NimBLE-Arduino` (2.x)
3. `secrets.h.example`을 `secrets.h`로 복사하고 **Wi-Fi SSID/비밀번호**를 입력 (`secrets.h`는 gitignore되어 커밋되지 않음. BLE 모드만 쓸 경우에도 파일 자체는 필요)
4. 필요 시 스케치 상단의 튜닝 값(타임존, 야간 시간대, 12/24시간·시간 소스 기본값 등) 수정
5. 파일을 UTF-8로 저장(Arduino IDE 기본값) 후 업로드

소스 파일 구성: `esp32-c3-clock.ino`(동작 코드와 튜닝 값) · `ble_time.h`(BLE CTS 시간 동기화) · `clock_fonts.h`(DSEG 폰트, 생성물) · `korean_calendar.h`(음력·절기·공휴일 테이블) · `secrets.h`(Wi-Fi, gitignore) · `tools/`(폰트 생성 도구)

## 폰트 (`clock_fonts.h`, `tools/`)

U8g2에는 DSEG 폰트가 없고 LVGL 폰트도 읽을 수 없어, **TTF → U8g2 폰트 변환기를 직접 만들어** 사용합니다. `clock_fonts.h`는 생성물이며 세 폰트를 합쳐 약 500바이트입니다:

| 폰트 | 용도 |
|---|---|
| `font_dseg7_b_28` | HH:MM — DSEG7 Classic **Bold** 28px, `0-9 :` 공백(숫자 폭) |
| `font_dseg7_r_11` | 날짜·초·음력 숫자 — DSEG7 Classic Regular 11px, `0-9 - .` |
| `font_dseg14_r_11` | AM/PM — DSEG14 Classic Regular 11px, `A M P` |

한글은 U8g2 내장 `u8g2_font_gulim12_t_korean2`(KS X 1001 2,350자, 약 60KB)를 사용합니다. `korean1`(574자)에는 곡·망·백·복·윤·춘·충·칩·토·헌·휴가 없어 절기·요일 표시가 불가능합니다.

특이사항:

- CYD와 같이 DSEG 숫자 **7**의 F세그먼트를 제거(A·B·C만 켜짐)했습니다. `tools/make_no7f.py`가 fontTools로 TTF에서 해당 윤곽선을 삭제해 `*-no7F.ttf`를 만듭니다.
- DSEG의 점(`.`)은 LCD처럼 직전 숫자에 겹치는 소수점이라 advance가 0입니다. 음력 날짜(`8.15`)의 구분자로 읽히도록 4px 칸을 따로 줍니다(`--adv .=4 --xoff .=1`).
- 정체(Regular/Bold)를 사용합니다. 이탤릭은 11~28px에서 지저분해 보여 제외했지만, `mkfont.py --shear`로 원하는 기울기를 다시 줄 수 있습니다.
- 도구:
  - `tools/u8g2font.py` — U8g2 폰트 포맷 코덱(디코드/RLE 인코드, 화면 목업 렌더)
  - `tools/mkfont.py` — Pillow(FreeType)로 글리프를 래스터라이즈해 U8g2 C 배열로 출력
  - `tools/gen_fonts.sh` — 위 도구로 `clock_fonts.h` 재생성 (`pip install pillow fonttools` 후 실행)

  ```bash
  cd tools && ./gen_fonts.sh          # PYTHON=... 로 인터프리터 지정 가능
  ```

## 설정 튜닝

튜닝 값은 모두 `esp32-c3-clock.ino` 상단에 모여 있습니다.

| 매크로 | 기본값 | 설명 |
|---|---|---|
| `TZ_INFO` | `"KST-9"` | POSIX 타임존 문자열 |
| `NTP_SYNC_INTERVAL_MS` | 1시간 | 재동기화 주기(SNTP·BLE CTS 공통) |
| `TIME_SYNC_BLE` | 0 | 시간 소스의 최초 부팅 기본값(1 = BLE CTS, 0 = Wi-Fi). 이후 3초 길게 눌러 전환(NVS 저장, 재부팅) |
| `BLE_DEVICE_NAME` | "ESP32-C3 Clock" | BLE 모드에서 아이폰 Bluetooth 목록에 표시되는 이름 |
| `SRC_PRESS_MS` | 3000 | 시간 소스 전환으로 인식할 누름 시간 |
| `WIFI_RETRY_MS` | 30초 | Wi-Fi 연결 재시도 주기 |
| `TIME_12H_DEFAULT` | 1 | 첫 부팅 시 12시간제(1)/24시간제(0). 이후 더블 클릭으로 바꾸고 NVS에 저장 |
| `FACE_DEFAULT` | `FACE_DIGITAL` | 첫 부팅 시 화면 |
| `FACE_BUTTON_PIN` | 9 | 버튼 핀. BOOT(=SCL) 또는 GND로 연결한 별도 버튼. -1이면 버튼 없음 |
| `FACE_CYCLE_S` | 0 | >0이면 N초마다 화면 자동 전환(버튼 없는 보드용, NVS에 쓰지 않음) |
| `DOUBLE_CLICK_MS` | 400 | 더블 클릭 판정 시간. 한 번 클릭은 이 시간 뒤 반영 |
| `LONG_PRESS_MS` | 1000 | 길게 누름(야간 모드 토글) 판정 시간 |
| `MSG_MS` | 2000 | 설정 변경 배너 표시 시간 |
| `NIGHT_ENABLE_DEFAULT` | 1 | 첫 부팅 시 야간 모드 사용 여부. 이후 길게 눌러 바꾸고 NVS에 저장 |
| `NIGHT_FROM_HOUR` / `NIGHT_TO_HOUR` | 22 / 7 | 야간 시간대(자정 넘김 가능). 둘이 같으면 야간 모드 없음 |
| `CONTRAST_NIGHT` | `0x80` | 야간 컨트라스트(`0x81`). 이 패널 실측: `0x40`부터 희미하게 보이고 `0x80`이 적당 |
| `NIGHT_PRECHARGE` | `0x11` | 야간 프리차지 기간(`0xD9`). 상위 니블(1~15)이 밝기를 크게 좌우. 낮 값 `0xF1` |
| `NIGHT_VCOMH` | `0x00` | 야간 VCOMH(`0xDB`). 낮 값 `0x40` |
| `NIGHT_DITHER` | 0 | 1이면 야간에 픽셀 절반만 켬(체커보드) |
| `NIGHT_OFF` | 0 | 1이면 야간에 어둡게 하는 대신 화면을 끔 |
| `DATE_Y` / `TIME_Y` / `BOTTOM_Y` 등 | — | 디지털 화면 각 줄의 baseline과 간격 |
| `DIAL_R`, `HOUR_LEN`, `MIN_LEN`, `SEC_LEN` 등 | 31, 14, 21, 25 | 아날로그 다이얼 반지름과 바늘 길이(px) |

## 라이선스

- 프로젝트 코드: [MIT License](LICENSE)
- 폰트: `clock_fonts.h`와 `tools/fonts/`의 DSEG 폰트(수정본 포함)는 keshikan의 DSEG로, [SIL Open Font License 1.1](tools/fonts/DSEG-LICENSE.txt)을 따릅니다
