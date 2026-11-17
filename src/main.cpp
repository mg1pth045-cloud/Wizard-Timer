// Level 1 / Lab 9 : Touch Panel
//
// ต่อยอดจาก Lab 7 (NTP Clock + DHT11 + พัดลมจำลอง PWM): ควบคุมทุกอย่างด้วย "จอสัมผัส" แทนการพิมพ์ใน Serial
//   - หน้า Calibrate: แตะเป้า 4 มุม -> คำนวณค่าแปลงพิกัด -> เก็บลงหน่วยความจำถาวร (Preferences) ไม่ต้องทำใหม่ทุกครั้ง
//   - แท็บ DASH  : แตะการ์ดเวลา = ขอเวลา NTP ใหม่, การ์ด Timer 1-3 ดูอย่างเดียว (แตะแล้วไม่มีผล)
//   - แท็บ TIMERS: นาฬิกาจับเวลา 3 ตัว (Timer 1-3) แต่ละตัวมีปุ่ม START/STOP และ RESET (ส่วนเซนเซอร์/พัดลมยังทำงานเบื้องหลัง ควบคุมผ่าน Serial)
//   - แท็บ SYSTEM: แถบเลื่อนความสว่างจอ (PWM ที่ GPIO21), ปุ่ม SYNC NTP, ปุ่ม TOUCH TEST, ปุ่ม RECALIBRATE (ต้องกดค้าง 1 วินาที)
//   - หน้า TOUCH TEST: แสดงค่าดิบ/พิกัดพิกเซล/แรงกดของจอสัมผัสสดๆ และวาดตามนิ้ว
//
// จอสัมผัสคือชิป XPT2046 (resistive) คุยผ่าน SPI ด้วยขา (ค่าจริงอยู่ใน include/board_config.h):
//   SCK GPIO25, MISO GPIO39, MOSI GPIO32, CS GPIO33  (IRQ GPIO36 ยังไม่ใช้ใน Lab นี้ อ่านแบบ polling)
//   ใช้ SPI ชุด VSPI (จอ TFT ใช้ HSPI) อ่านค่าด้วยโค้ดของเราเอง ไม่ต้องใช้ไลบรารีเพิ่ม
//
// ขา LED / อื่นๆ (เหมือน Lab 6-7):
//   DHT11 DATA = GPIO27, LED น้ำเงิน GPIO17 (PWM = พัดลมจำลอง), LED แดง GPIO4 (เซนเซอร์มีปัญหา),
//   LED เขียว GPIO16 (sync เวลา NTP แล้ว), ไฟหลังจอ GPIO21 (PWM ความสว่างจอ), ปุ่ม BOOT GPIO0 (สลับ AUTO/MANUAL)
//   LED เป็น active-LOW (ส่ง LOW = ติด)
//
// คำสั่ง Serial: m + - 0-9 s n t h (เหมือน Lab 7) และ c = calibrate จอสัมผัสใหม่, k = แสดงค่า calibrate และค่าดิบล่าสุด

#include <Arduino.h>
#include <DHT.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include "app_config.h"
#include "board_config.h"

// ถ้ายังไม่มีไฟล์ secrets.h โปรแกรมก็ยัง build ได้ แต่จะไม่มีเวลาจริง (ส่วนอื่นรวมถึงจอสัมผัสใช้ได้ปกติ)
#if __has_include("secrets.h")
#include "secrets.h"
#else
inline constexpr char WIFI_SSID[] = "";
inline constexpr char WIFI_PASSWORD[] = "";
#endif

namespace {

// ---------------------------------------------------------------------
// สีและตำแหน่งบนจอ
// ---------------------------------------------------------------------
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// สีของแถบหัว: พื้นเป็นสีน้ำเงินทั้งสองธีม ตัวอักษรจึงเป็นสีอ่อนคงที่
constexpr uint16_t COLOR_HEADER_TEXT = rgb565(240, 244, 250);
constexpr uint16_t COLOR_HEADER_DIM = rgb565(180, 200, 225);

// สีที่เปลี่ยนตามธีม Dark/Light (กำหนดค่าใน applyTheme)
uint16_t COLOR_BG, COLOR_HEADER, COLOR_CARD, COLOR_BORDER, COLOR_TEXT, COLOR_DIM;
uint16_t COLOR_GREEN, COLOR_BLUE, COLOR_RED, COLOR_YELLOW, COLOR_ORANGE;
uint16_t COLOR_BUTTON;        // ปุ่มปกติ
uint16_t COLOR_PRESSED;       // ปุ่มขณะถูกกด
uint16_t COLOR_ACTIVE;        // แท็บ/ปุ่มที่เลือกอยู่
uint16_t COLOR_ACTIVE_GREEN;  // ปุ่มโหมดที่เลือกอยู่
bool darkTheme = true;

void applyTheme(bool dark)
{
    darkTheme = dark;
    if (dark) {
        COLOR_BG = rgb565(8, 12, 18);
        COLOR_HEADER = rgb565(20, 70, 120);
        COLOR_CARD = rgb565(22, 30, 42);
        COLOR_BORDER = rgb565(60, 76, 98);
        COLOR_TEXT = rgb565(240, 244, 250);
        COLOR_DIM = rgb565(140, 155, 175);
        COLOR_GREEN = rgb565(60, 220, 120);
        COLOR_BLUE = rgb565(90, 165, 255);
        COLOR_RED = rgb565(240, 85, 85);
        COLOR_YELLOW = rgb565(250, 205, 70);
        COLOR_ORANGE = rgb565(255, 150, 60);
        COLOR_BUTTON = rgb565(34, 48, 68);
        COLOR_PRESSED = rgb565(80, 115, 170);
        COLOR_ACTIVE = rgb565(28, 100, 170);
        COLOR_ACTIVE_GREEN = rgb565(20, 110, 70);
    } else {
        // ธีมสว่าง: สีสถานะเข้มขึ้นเพื่อให้อ่านชัดบนพื้นขาว และปุ่มที่เลือกใช้สีอ่อนเพื่อให้ตัวอักษรเข้มอ่านออก
        COLOR_BG = rgb565(238, 242, 247);
        COLOR_HEADER = rgb565(40, 110, 190);
        COLOR_CARD = rgb565(255, 255, 255);
        COLOR_BORDER = rgb565(170, 182, 200);
        COLOR_TEXT = rgb565(20, 28, 40);
        COLOR_DIM = rgb565(100, 112, 130);
        COLOR_GREEN = rgb565(0, 150, 70);
        COLOR_BLUE = rgb565(20, 100, 210);
        COLOR_RED = rgb565(200, 40, 40);
        COLOR_YELLOW = rgb565(200, 140, 0);
        COLOR_ORANGE = rgb565(220, 100, 0);
        COLOR_BUTTON = rgb565(214, 222, 234);
        COLOR_PRESSED = rgb565(160, 185, 225);
        COLOR_ACTIVE = rgb565(140, 180, 235);
        COLOR_ACTIVE_GREEN = rgb565(150, 225, 180);
    }
}

constexpr int16_t SCREEN_W = 320;
constexpr int16_t SCREEN_H = 240;
constexpr int16_t HEADER_H = 28;
constexpr int16_t HEARTBEAT_X = 118;  // จุดกระพริบในแถบหัว อยู่ถัดจากชื่อ "Wizard Timer"

struct Rect {
    int16_t x, y, w, h;
};

constexpr bool contains(const Rect &r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// แถบแท็บด้านล่าง (3 ปุ่ม สูง 36 พิกเซล ใหญ่พอให้แตะง่ายบนจอ resistive)
constexpr Rect TAB_RECTS[3] = {{4, 202, 100, 36}, {108, 202, 100, 36}, {212, 202, 100, 36}};
constexpr const char *TAB_NAMES[3] = {"DASH", "TIMERS", "SYSTEM"};

// หน้า DASH
constexpr Rect CLOCK_CARD = {8, 32, 304, 62};
constexpr int TIMER_COUNT = 3;
// การ์ด Timer 1-3 แบบย่อในหน้า DASH (แตะ = เริ่ม/หยุด)
constexpr Rect DASH_TIMER_RECTS[TIMER_COUNT] = {{8, 98, 304, 31}, {8, 132, 304, 31}, {8, 166, 304, 31}};

// หน้า TIMERS: แถวละ 1 timer มีไอคอน ชื่อ เวลา ปุ่ม START/STOP และ RESET
constexpr Rect TIMER_ROWS[TIMER_COUNT] = {{8, 32, 304, 52}, {8, 88, 304, 52}, {8, 144, 304, 52}};
constexpr Rect timerStartBtn(int i) { return {static_cast<int16_t>(TIMER_ROWS[i].x + 172), static_cast<int16_t>(TIMER_ROWS[i].y + 8), 64, 36}; }
constexpr Rect timerSetBtn(int i) { return {static_cast<int16_t>(TIMER_ROWS[i].x + 240), static_cast<int16_t>(TIMER_ROWS[i].y + 8), 56, 36}; }

// หน้า SYSTEM
constexpr Rect BL_SLIDER_AREA = {8, 52, 304, 34};
constexpr Rect BL_SLIDER_TRACK = {24, 65, 272, 8};
constexpr Rect SYNC_BTN = {8, 92, 148, 40};
constexpr Rect TEST_BTN = {164, 92, 148, 40};
constexpr Rect RECAL_BTN = {8, 138, 196, 40};
constexpr Rect THEME_BTN = {212, 138, 100, 40};

// หน้า TOUCH TEST
constexpr Rect PAINT_BOX = {8, 92, 304, 106};
constexpr Rect BACK_BTN = {8, 202, 148, 36};
constexpr Rect CLEAR_BTN = {164, 202, 148, 36};

enum class Page : uint8_t { Dashboard, Timers, System, Test, Calibrate };
enum class Widget : uint8_t {
    None,
    TabDash, TabTimers, TabSystem,
    ClockCard,
    DashTimer0, DashTimer1, DashTimer2,
    TimerStart0, TimerStart1, TimerStart2,
    TimerSet0, TimerSet1, TimerSet2,
    BlSlider, SyncBtn, TestBtn, RecalBtn, ThemeBtn,
    TestPaint, TestBack, TestClear
};
enum class Mode : uint8_t { Auto, Manual };
enum class WifiState : uint8_t { NotConfigured, Connecting, Connected, Failed };
enum class NtpState : uint8_t { Idle, Syncing, Synced, Failed };
enum class SimChoice : uint8_t { Off, Random, Sweep };
enum class DataSource : uint8_t { None, Real, SimRandom, SimSweep };

TFT_eSPI tft;
DHT dht(cyd::DHT11_PIN, DHT11);
SPIClass touchSpi(VSPI);  // SPI ชุดสำหรับจอสัมผัส (จอ TFT ใช้ HSPI)
Preferences prefs;        // เก็บค่า calibrate และความสว่างจอแบบถาวร (ไม่หายเมื่อไฟดับ)

Page page = Page::Dashboard;
Mode mode = Mode::Auto;

// ---- WiFi / NTP (เหมือน Lab 7) ----
WifiState wifiState = WifiState::NotConfigured;
uint32_t wifiStateSince = 0;
NtpState ntpState = NtpState::Idle;
uint32_t ntpStartedAt = 0;
uint32_t syncCount = 0;
uint32_t lastSyncAtMs = 0;
volatile bool syncEvent = false;

// ---- DHT11 / ค่าจำลอง / พัดลม (เหมือน Lab 6-7 แต่เกณฑ์อุณหภูมิปรับได้ตอนทำงาน) ----
float sensorTempC = NAN;
float sensorHumidity = NAN;
bool haveReading = false;
uint8_t consecutiveFails = 0;
bool sensorError = false;
uint32_t readCount = 0;
uint32_t okCount = 0;
uint32_t failCount = 0;
uint32_t lastReadMicros = 0;

SimChoice simChoice = SimChoice::Off;
float simTemp = app::SIM_TEMP_START_C;
float simHum = app::SIM_HUM_START;
uint32_t lastSimAt = 0;
DataSource lastReportedSource = DataSource::None;

bool fanOn = false;
int fanDuty = 0;
uint32_t fanValue = 0;
float fanOnTempC = app::FAN_ON_TEMP_C;    // เกณฑ์ "เปิดพัดลม" (ปรับได้จากหน้า FAN)
float fanOffTempC = app::FAN_OFF_TEMP_C;  // เกณฑ์ "ปิดพัดลม" (ต้องต่ำกว่าเกณฑ์เปิด = hysteresis)
bool errorLedOn = false;
bool syncLedOn = false;

int backlightPercent = app::BACKLIGHT_DEFAULT_PERCENT;

// ---- นาฬิกาจับเวลา ----
// START = เอาต์พุต ON และเริ่มนับถอยหลัง, ครบเวลาตั้ง -> เอาต์พุต OFF เอง (กลับสถานะพร้อมเริ่มใหม่)
// ระหว่างนับถ้ากด STOP = เอาต์พุต OFF และรีเซ็ตทันที (ไม่มี pause)
struct Timer {
    bool running = false;    // true = จ่ายไฟอยู่ (กำลังนับ หรือครบเวลาแล้ว)
    bool timeUp = false;     // true = ครบเวลาแล้ว เอาต์พุต ON
    uint8_t presetMin = app::TIMER_DEFAULT_MINUTES;  // เวลาที่ตั้ง (นาที)
    uint32_t startedAt = 0;  // millis() ตอนจ่ายไฟ
};
Timer timers[TIMER_COUNT];

uint32_t timerPreset(int i)
{
    return static_cast<uint32_t>(timers[i].presetMin) * 60000UL;
}

uint32_t timerElapsed(int i, uint32_t now)
{
    return timers[i].running ? now - timers[i].startedAt : 0;
}

// เวลาที่เหลือ: ยังไม่จ่ายไฟ = แสดงค่าที่ตั้งไว้, ครบแล้ว = 0
uint32_t timerRemaining(int i, uint32_t now)
{
    if (!timers[i].running) return timerPreset(i);
    const uint32_t elapsed = timerElapsed(i, now);
    return elapsed >= timerPreset(i) ? 0 : timerPreset(i) - elapsed;
}

// ---- จอสัมผัส ----
// ค่าแปลงจากค่าดิบ (0-4095) เป็นพิกัดจอ: screen = a + b * raw  (แยกแกน x กับ y) swapXY = แกนดิบสลับกับแกนจอ
struct TouchCalibration {
    bool valid = false;
    bool swapXY = false;
    float xa = 0.0f, xb = 0.0f;
    float ya = 0.0f, yb = 0.0f;
};
TouchCalibration cal;

struct RawTouch {
    int16_t x = 0, y = 0;
    int16_t z = 0;  // แรงกด (ยิ่งมากยิ่งกดแรง) น้อยกว่า TOUCH_Z_MIN = ไม่ได้แตะ
};

struct TouchState {
    bool down = false;        // กำลังแตะอยู่ (ผ่านการกันสัญญาณรบกวนแล้ว)
    bool pressEdge = false;   // เพิ่ง "กดลง" ในรอบนี้
    bool releaseEdge = false; // เพิ่ง "ปล่อย" ในรอบนี้
    bool touchingNow = false; // ตัวอย่างล่าสุดมีแรงกดพอหรือไม่
    int16_t rawX = 0, rawY = 0, z = 0;
    int x = 0, y = 0;         // พิกัดจอ (กรองแล้ว)
    int lastX = 0, lastY = 0; // พิกัดล่าสุดตอนยังแตะอยู่
    uint32_t downSince = 0;
    uint8_t pressCount = 0, releaseCount = 0;
    bool smoothReady = false;
};
TouchState touch;
uint32_t lastTouchPollAt = 0;

Widget pressTarget = Widget::None;    // ปุ่มที่นิ้วกดลงตอนแรก
Widget pressedWidget = Widget::None;  // ปุ่มที่ "ดูเหมือนถูกกดอยู่" (นิ้วยังอยู่ในกรอบ)
uint32_t repeatAt = 0;
bool longPressDone = false;
int drawnLongPress = -1;
int lastPaintX = -1, lastPaintY = -1;

// calibrate
int calStep = 0;
int calAccepted = 0;
int calSamples = 0;
long calSumX = 0, calSumY = 0;
bool calWaitRelease = false;
int calRawX[4] = {0, 0, 0, 0};
int calRawY[4] = {0, 0, 0, 0};
uint32_t calMessageUntil = 0;
bool calResultOk = false;

// ---- จับเวลา ----
uint32_t lastReadAt = 0;
uint32_t lastFanAt = 0;
uint32_t lastDisplayAt = 0;
uint32_t lastStatusAt = 0;
uint32_t lastHeartbeatAt = 0;
uint32_t lastSerialAt = 0;
uint32_t lastTimePrintAt = 0;
bool heartbeatOn = true;

// สิ่งที่วาดบนจอไปล่าสุด (วาดเฉพาะส่วนที่เปลี่ยน)
constexpr int NEVER = -32768;
int drawnTimeKey = NEVER;
int drawnDateKey = NEVER;
int drawnBadgeKey = NEVER;
int drawnSyncCount = NEVER;
int drawnTimerTime[TIMER_COUNT] = {NEVER, NEVER, NEVER};
int drawnTimerState[TIMER_COUNT] = {NEVER, NEVER, NEVER};
int drawnBacklight = NEVER;
int drawnHeaderTime = NEVER;
int drawnTestKey = NEVER;

// ---------------------------------------------------------------------
// ตัวช่วยวาด
// ---------------------------------------------------------------------
void drawText(const char *text, int16_t x, int16_t y, uint8_t font, uint16_t fg, uint16_t bg, uint16_t paddingW = 0)
{
    tft.setTextColor(fg, bg);
    tft.setTextPadding(paddingW);
    tft.drawString(text, x, y, font);
    tft.setTextPadding(0);
}

void drawHeader(const char *title)
{
    tft.fillRect(0, 0, SCREEN_W, HEADER_H, COLOR_HEADER);
    drawText(title, 8, 6, 2, COLOR_HEADER_TEXT, COLOR_HEADER);
}

void drawCard(const Rect &r, const char *label)
{
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 6, COLOR_CARD);
    tft.drawRoundRect(r.x, r.y, r.w, r.h, 6, COLOR_BORDER);
    if (label[0] != '\0') {
        drawText(label, r.x + 8, r.y + 4, 2, COLOR_DIM, COLOR_CARD);
    }
}

// ปุ่มสี่เหลี่ยมมุมมน ข้อความกึ่งกลาง
void drawButton(const Rect &r, const char *label, uint16_t fill, uint16_t border, uint16_t textColor, uint8_t font = 2)
{
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 6, fill);
    tft.drawRoundRect(r.x, r.y, r.w, r.h, 6, border);
    tft.setTextColor(textColor, fill);
    tft.setTextPadding(0);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(label, r.x + r.w / 2, r.y + r.h / 2, font);
    tft.setTextDatum(TL_DATUM);
}

// ---------------------------------------------------------------------
// เวลาจริง (NTP) เหมือน Lab 7
// ---------------------------------------------------------------------
void onTimeSync(struct timeval *tv)
{
    (void)tv;
    syncEvent = true;
}

bool timeValid()
{
    return syncCount > 0 && static_cast<uint32_t>(time(nullptr)) >= app::SANE_EPOCH;
}

bool timeStale(uint32_t now)
{
    return ntpState == NtpState::Synced && (now - lastSyncAtMs) > app::NTP_RESYNC_MS * app::NTP_STALE_FACTOR;
}

void formatLocalTime(char *out, size_t size, const char *format)
{
    const time_t t = time(nullptr);
    struct tm local;
    localtime_r(&t, &local);
    strftime(out, size, format, &local);
}

bool makeTimestamp(char *out, size_t size)
{
    if (!timeValid()) {
        snprintf(out, size, "NOT_SYNCED");
        return false;
    }
    formatLocalTime(out, size, "%Y-%m-%d %H:%M:%S");
    return true;
}

void formatAgo(char *out, size_t size, uint32_t ms)
{
    const uint32_t seconds = ms / 1000;
    if (seconds < 60) {
        snprintf(out, size, "%lus", static_cast<unsigned long>(seconds));
    } else if (seconds < 3600) {
        snprintf(out, size, "%lum", static_cast<unsigned long>(seconds / 60));
    } else {
        snprintf(out, size, "%luh%02lum", static_cast<unsigned long>(seconds / 3600),
                 static_cast<unsigned long>((seconds / 60) % 60));
    }
}

const char *ntpStateName(NtpState state)
{
    switch (state) {
        case NtpState::Idle: return "IDLE";
        case NtpState::Syncing: return "SYNCING";
        case NtpState::Synced: return "SYNCED";
        case NtpState::Failed: return "FAILED";
    }
    return "?";
}

void startNtp(uint32_t now)
{
    configTime(app::GMT_OFFSET_SEC, app::DAYLIGHT_OFFSET_SEC, app::NTP_SERVER_1, app::NTP_SERVER_2);
    ntpState = NtpState::Syncing;
    ntpStartedAt = now;
    Serial.printf("[NTP] requesting time from %s / %s ...\n", app::NTP_SERVER_1, app::NTP_SERVER_2);
}

// ขอเวลา NTP ทันที (ใช้กับปุ่มบนจอและคำสั่ง n)
void requestNtpNow(uint32_t now)
{
    if (wifiState == WifiState::Connected) {
        startNtp(now);
    } else {
        Serial.println("[NTP] WiFi is not connected: cannot ask for the time now");
    }
}

void updateNtp(uint32_t now)
{
    if (syncEvent) {
        syncEvent = false;
        syncCount++;
        lastSyncAtMs = now;
        ntpState = NtpState::Synced;
        char stamp[24];
        makeTimestamp(stamp, sizeof(stamp));
        Serial.printf("[NTP] synced #%lu: %s (UTC+%ld)\n", static_cast<unsigned long>(syncCount), stamp,
                      app::GMT_OFFSET_SEC / 3600);
    }

    if (wifiState != WifiState::Connected) {
        if (ntpState == NtpState::Syncing) {
            ntpState = NtpState::Idle;
        }
        return;
    }

    if (ntpState == NtpState::Idle) {
        startNtp(now);
    } else if (ntpState == NtpState::Syncing && now - ntpStartedAt >= app::NTP_TIMEOUT_MS) {
        ntpState = NtpState::Failed;
        Serial.printf("[NTP] no reply within %lu s (UDP port 123 blocked? DNS problem?) - will retry\n",
                      static_cast<unsigned long>(app::NTP_TIMEOUT_MS / 1000));
    } else if (ntpState == NtpState::Failed && now - ntpStartedAt >= app::NTP_RETRY_MS) {
        startNtp(now);
    }
}

// ---------------------------------------------------------------------
// WiFi (แบบย่อจาก Lab 3)
// ---------------------------------------------------------------------
const char *wifiStateName(WifiState state)
{
    switch (state) {
        case WifiState::NotConfigured: return "NOT CONFIGURED";
        case WifiState::Connecting: return "CONNECTING";
        case WifiState::Connected: return "CONNECTED";
        case WifiState::Failed: return "FAILED";
    }
    return "?";
}

void setWifiState(WifiState next)
{
    Serial.printf("[WIFI] %s -> %s\n", wifiStateName(wifiState), wifiStateName(next));
    wifiState = next;
    wifiStateSince = millis();
}

void startWifiConnect()
{
    Serial.println("[WIFI] connecting ...");  // ไม่พิมพ์ชื่อ WiFi เพื่อไม่ให้หลุดเวลาแชร์ log
    WiFi.begin(WIFI_SSID, strlen(WIFI_PASSWORD) > 0 ? WIFI_PASSWORD : nullptr);
    setWifiState(WifiState::Connecting);
}

void updateWifi(uint32_t now)
{
    switch (wifiState) {
        case WifiState::NotConfigured:
            break;
        case WifiState::Connecting:
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("[WIFI] connected, RSSI %d dBm\n", WiFi.RSSI());
                setWifiState(WifiState::Connected);
            } else if (now - wifiStateSince >= app::CONNECT_TIMEOUT_MS) {
                WiFi.disconnect(false);
                setWifiState(WifiState::Failed);
            }
            break;
        case WifiState::Connected:
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[WIFI] connection lost (the clock keeps running)");
                startWifiConnect();
            }
            break;
        case WifiState::Failed:
            if (now - wifiStateSince >= app::RETRY_DELAY_MS) {
                startWifiConnect();
            }
            break;
    }
}

// ---------------------------------------------------------------------
// อุณหภูมิที่ "ใช้งาน" (จากเซนเซอร์จริง หรือจำลอง) เหมือน Lab 6-7
// ---------------------------------------------------------------------
float sweepFraction(uint32_t now)
{
    const float half = static_cast<float>(app::SIM_PERIOD_MS) / 2.0f;
    const float t = static_cast<float>(now % app::SIM_PERIOD_MS);
    return (t < half) ? (t / half) : ((static_cast<float>(app::SIM_PERIOD_MS) - t) / half);
}

float randomStep(float maxStep)
{
    return (static_cast<float>(random(-1000, 1001)) / 1000.0f) * maxStep;
}

void updateSimulationRandom()
{
    simTemp = constrain(simTemp + randomStep(app::SIM_TEMP_STEP_C), app::SIM_TEMP_MIN_C, app::SIM_TEMP_MAX_C);
    simHum = constrain(simHum + randomStep(app::SIM_HUM_STEP), app::SIM_HUM_MIN, app::SIM_HUM_MAX);
}

bool sensorValid()
{
    return haveReading && !sensorError;
}

DataSource currentSource()
{
    if (simChoice == SimChoice::Random) return DataSource::SimRandom;
    if (simChoice == SimChoice::Sweep) return DataSource::SimSweep;
    if (sensorValid()) return DataSource::Real;
    if (sensorError && app::SIM_WHEN_NO_SENSOR) return DataSource::SimRandom;
    return DataSource::None;
}

bool isSimulated(DataSource source)
{
    return source == DataSource::SimRandom || source == DataSource::SimSweep;
}

const char *sourceName(DataSource source)
{
    switch (source) {
        case DataSource::Real: return "REAL";
        case DataSource::SimRandom: return "SIM-RANDOM";
        case DataSource::SimSweep: return "SIM-SWEEP";
        case DataSource::None: return "NONE";
    }
    return "?";
}

float effectiveTemp(uint32_t now)
{
    switch (currentSource()) {
        case DataSource::Real: return sensorTempC;
        case DataSource::SimRandom: return simTemp;
        case DataSource::SimSweep:
            return app::SIM_TEMP_MIN_C + sweepFraction(now) * (app::SIM_TEMP_MAX_C - app::SIM_TEMP_MIN_C);
        case DataSource::None: break;
    }
    return NAN;
}

float effectiveHumidity(uint32_t now)
{
    switch (currentSource()) {
        case DataSource::Real: return sensorHumidity;
        case DataSource::SimRandom: return simHum;
        case DataSource::SimSweep:
            return app::SIM_HUM_MAX - sweepFraction(now) * (app::SIM_HUM_MAX - app::SIM_HUM_MIN);
        case DataSource::None: break;
    }
    return NAN;
}

// ---------------------------------------------------------------------
// พัดลมจำลอง = PWM บน LED น้ำเงิน (GPIO17) และ LED อื่นๆ เหมือน Lab 6-7
// ---------------------------------------------------------------------
void writeFanPwm(uint32_t value)
{
    // RGB LED หลังบอร์ดดับตลอด: ไม่ส่งค่าพัดลมไปที่ LED (ค่า fanValue ยังคำนวณไว้ตามเดิม)
    (void)value;
    ledcWrite(cyd::LED_PIN_B, cyd::LED_ACTIVE_LOW ? app::PWM_MAX_VALUE : 0);
}

void applyFanDuty(int percent)
{
    fanDuty = constrain(percent, 0, 100);
    fanValue = (static_cast<uint32_t>(fanDuty) * app::PWM_MAX_VALUE + 50) / 100;
    writeFanPwm(fanValue);
}

void pwmBegin()
{
    ledcAttachChannel(cyd::LED_PIN_B, app::PWM_FREQUENCY_HZ, app::PWM_RESOLUTION_BITS, app::FAN_CHANNEL);  // GPIO17 (LED น้ำเงิน) = พัดลมจำลอง
    applyFanDuty(0);
}

// ความสว่างจอ: PWM ที่ GPIO21 (ขาไฟหลังจอ) ไม่กลับค่าเพราะไฟหลังจอเป็น active-HIGH
void applyBacklight(int percent)
{
    backlightPercent = constrain(percent, app::BACKLIGHT_MIN_PERCENT, 100);
    const uint32_t value = (static_cast<uint32_t>(backlightPercent) * app::PWM_MAX_VALUE + 50) / 100;
    ledcWrite(cyd::TFT_PIN_BACKLIGHT, value);
}

void backlightBegin()
{
    // ต้องทำหลัง tft.init() (ไลบรารีจอเปิดไฟหลังจอด้วย digitalWrite ไว้ก่อน) เหมือน Lab 5
    ledcAttachChannel(cyd::TFT_PIN_BACKLIGHT, app::PWM_FREQUENCY_HZ, app::PWM_RESOLUTION_BITS, app::BACKLIGHT_CHANNEL);
    applyBacklight(backlightPercent);
}

void setErrorLed(bool on)
{
    if (on == errorLedOn) return;
    errorLedOn = on;
    // RGB LED หลังบอร์ดดับตลอด: เก็บสถานะไว้แต่ไม่สั่งขา LED
}

void setSyncLed(bool on)
{
    if (on == syncLedOn) return;
    syncLedOn = on;
    // RGB LED หลังบอร์ดดับตลอด: เก็บสถานะไว้แต่ไม่สั่งขา LED
}

int dutyForTemp(float tempC)
{
    const float span = app::FAN_FULL_TEMP_C - fanOnTempC;
    const float fraction = constrain((tempC - fanOnTempC) / span, 0.0f, 1.0f);
    return app::FAN_MIN_DUTY_PERCENT + static_cast<int>(lroundf(fraction * (100 - app::FAN_MIN_DUTY_PERCENT)));
}

void updateFanAuto(uint32_t now)
{
    const float tempC = effectiveTemp(now);
    const bool wasOn = fanOn;

    if (isnan(tempC)) {
        fanOn = false;
    } else if (!fanOn && tempC >= fanOnTempC) {
        fanOn = true;
    } else if (fanOn && tempC <= fanOffTempC) {
        fanOn = false;
    }

    applyFanDuty((fanOn && !isnan(tempC)) ? dutyForTemp(tempC) : 0);

    if (wasOn != fanOn) {
        char stamp[24];
        makeTimestamp(stamp, sizeof(stamp));
        if (isnan(tempC)) {
            Serial.printf("[FAN] %s -> %s (no valid temperature)  @ %s\n", wasOn ? "ON" : "OFF", fanOn ? "ON" : "OFF", stamp);
        } else {
            Serial.printf("[FAN] %s -> %s at %.1f C  @ %s\n", wasOn ? "ON" : "OFF", fanOn ? "ON" : "OFF", tempC, stamp);
        }
    }
}

void readDht()
{
    readCount++;
    const uint32_t startedAt = micros();
    const float humidity = dht.readHumidity();
    const float tempC = dht.readTemperature();
    lastReadMicros = micros() - startedAt;

    if (!isnan(humidity) && !isnan(tempC)) {
        sensorTempC = tempC;
        sensorHumidity = humidity;
        haveReading = true;
        consecutiveFails = 0;
        sensorError = false;
        okCount++;
        Serial.printf("[DHT11] #%lu OK     temp=%.1f C  hum=%.0f %%  (read took %lu us)\n",
                      static_cast<unsigned long>(readCount), tempC, humidity, static_cast<unsigned long>(lastReadMicros));
    } else {
        failCount++;
        if (consecutiveFails < 255) consecutiveFails++;
        sensorError = consecutiveFails >= app::DHT_FAIL_LIMIT;
        Serial.printf("[DHT11] #%lu FAILED (checksum/timeout) fails in a row=%u%s  (read took %lu us)\n",
                      static_cast<unsigned long>(readCount), consecutiveFails,
                      sensorError ? "  -> SENSOR ERROR: check wiring (VCC/DATA/GND)" : "",
                      static_cast<unsigned long>(lastReadMicros));
    }
}

// ---------------------------------------------------------------------
// จอสัมผัส XPT2046: อ่านค่าดิบผ่าน SPI
// ---------------------------------------------------------------------

// เลือกค่าเฉลี่ยของ "สองค่าที่ใกล้กันที่สุด" จากสามค่า (ตัดค่าสุ่มที่เพี้ยน)
int16_t bestTwoAverage(int16_t a, int16_t b, int16_t c)
{
    const int16_t dab = abs(a - b);
    const int16_t dac = abs(a - c);
    const int16_t dbc = abs(b - c);
    if (dab <= dac && dab <= dbc) return (a + b) >> 1;
    if (dac <= dab && dac <= dbc) return (a + c) >> 1;
    return (b + c) >> 1;
}

// อ่านชิป XPT2046 หนึ่งรอบ คืน true ถ้ามีการแตะ (แรงกด >= TOUCH_Z_MIN)
// ลำดับคำสั่ง: Z1, Z2 -> คำนวณแรงกด; ถ้ากดอยู่ อ่านแกน X และ Y หลายครั้งแล้วเลือกค่าที่ใกล้กัน
// (ผลของแต่ละคำสั่งจะมาในรอบส่งถัดไป จึงอ่านเหลื่อมกัน 1 คำสั่ง)
bool readRawTouch(RawTouch &out)
{
    int16_t data[6] = {0, 0, 0, 0, 0, 0};

    touchSpi.beginTransaction(SPISettings(cyd::TOUCH_SPI_FREQUENCY, MSBFIRST, SPI_MODE0));
    digitalWrite(cyd::TOUCH_PIN_CS, LOW);
    touchSpi.transfer(0xB1);                                 // เริ่มวัด Z1
    const int16_t z1 = static_cast<int16_t>(touchSpi.transfer16(0xC1) >> 3);  // ได้ผล Z1, เริ่มวัด Z2
    int z = z1 + 4095;
    const int16_t z2 = static_cast<int16_t>(touchSpi.transfer16(0x91) >> 3);  // ได้ผล Z2, เริ่มวัดแกนแรก
    z -= z2;                                                  // แรงกด = Z1 + 4095 - Z2
    if (z >= app::TOUCH_Z_MIN) {
        touchSpi.transfer16(0x91);                            // ค่าแรกมักมีสัญญาณรบกวน ทิ้งไป
        data[0] = static_cast<int16_t>(touchSpi.transfer16(0xD1) >> 3);
        data[1] = static_cast<int16_t>(touchSpi.transfer16(0x91) >> 3);
        data[2] = static_cast<int16_t>(touchSpi.transfer16(0xD1) >> 3);
        data[3] = static_cast<int16_t>(touchSpi.transfer16(0x91) >> 3);
    }
    data[4] = static_cast<int16_t>(touchSpi.transfer16(0xD0) >> 3);  // คำสั่งสุดท้ายสั่งให้ชิปพักการวัด
    data[5] = static_cast<int16_t>(touchSpi.transfer16(0) >> 3);
    digitalWrite(cyd::TOUCH_PIN_CS, HIGH);
    touchSpi.endTransaction();

    out.z = static_cast<int16_t>(z < 0 ? 0 : z);
    if (z < app::TOUCH_Z_MIN) {
        return false;
    }
    out.x = bestTwoAverage(data[0], data[2], data[4]);
    out.y = bestTwoAverage(data[1], data[3], data[5]);
    return true;
}

// ---------------------------------------------------------------------
// การ calibrate: แปลงค่าดิบเป็นพิกัดจอ
// ---------------------------------------------------------------------
void mapRawToScreen(int16_t rawX, int16_t rawY, int &sx, int &sy)
{
    const float u = cal.swapXY ? rawY : rawX;  // แกนดิบที่ใช้เป็นแกน x ของจอ
    const float v = cal.swapXY ? rawX : rawY;
    sx = constrain(static_cast<int>(lroundf(cal.xa + cal.xb * u)), 0, SCREEN_W - 1);
    sy = constrain(static_cast<int>(lroundf(cal.ya + cal.yb * v)), 0, SCREEN_H - 1);
}

// เป้า 4 มุม (บนซ้าย, บนขวา, ล่างขวา, ล่างซ้าย) ห่างขอบ CAL_MARGIN พิกเซล
void calTarget(int index, int &tx, int &ty)
{
    tx = (index == 0 || index == 3) ? app::CAL_MARGIN : SCREEN_W - 1 - app::CAL_MARGIN;
    ty = (index == 0 || index == 1) ? app::CAL_MARGIN : SCREEN_H - 1 - app::CAL_MARGIN;
}

// คำนวณค่า calibrate จากค่าดิบของเป้าทั้ง 4 คืน false ถ้าข้อมูลไม่สมเหตุสมผล (เช่นแตะผิดมุม)
bool computeCalibration()
{
    // แกนดิบไหนเปลี่ยนมากกว่าเมื่อเลื่อนจากมุมบนซ้ายไปบนขวา (แกน x ของจอ) -> ถ้าเป็นแกน Y ดิบ แปลว่าแกนสลับกัน
    const int dxTop = abs(calRawX[1] - calRawX[0]);
    const int dyTop = abs(calRawY[1] - calRawY[0]);
    TouchCalibration c;
    c.swapXY = dyTop > dxTop;

    auto rawU = [&](int i) { return static_cast<float>(c.swapXY ? calRawY[i] : calRawX[i]); };
    auto rawV = [&](int i) { return static_cast<float>(c.swapXY ? calRawX[i] : calRawY[i]); };
    const float leftU = (rawU(0) + rawU(3)) / 2.0f;
    const float rightU = (rawU(1) + rawU(2)) / 2.0f;
    const float topV = (rawV(0) + rawV(1)) / 2.0f;
    const float bottomV = (rawV(2) + rawV(3)) / 2.0f;
    const float spanU = rightU - leftU;
    const float spanV = bottomV - topV;
    if (fabsf(spanU) < app::CAL_MIN_SPAN || fabsf(spanV) < app::CAL_MIN_SPAN) {
        Serial.printf("[CAL] raw span too small (u=%.0f v=%.0f): did you touch the crosses?\n", spanU, spanV);
        return false;
    }

    int leftX, topY, rightX, bottomY;
    calTarget(0, leftX, topY);
    calTarget(2, rightX, bottomY);
    c.xb = static_cast<float>(rightX - leftX) / spanU;  // พิกเซลต่อหน่วยดิบ (เครื่องหมายลบ = แกนกลับด้าน)
    c.xa = static_cast<float>(leftX) - c.xb * leftU;
    c.yb = static_cast<float>(bottomY - topY) / spanV;
    c.ya = static_cast<float>(topY) - c.yb * topV;

    // ตรวจย้อนกลับ: เอาค่าดิบของแต่ละเป้ามาแปลงเป็นพิกัดจอ ต้องใกล้เป้าจริง
    float worst = 0.0f;
    for (int i = 0; i < 4; i++) {
        int tx, ty;
        calTarget(i, tx, ty);
        const float sx = c.xa + c.xb * rawU(i);
        const float sy = c.ya + c.yb * rawV(i);
        const float err = fmaxf(fabsf(sx - tx), fabsf(sy - ty));
        if (err > worst) worst = err;
    }
    Serial.printf("[CAL] swapXY=%s  x: %.4f*u%+.1f  y: %.4f*v%+.1f  worst target error %.1f px\n", c.swapXY ? "yes" : "no", c.xb, c.xa,
                  c.yb, c.ya, worst);
    if (worst > app::CAL_MAX_ERROR_PX) {
        Serial.println("[CAL] points are not consistent with a rectangle: please try again");
        return false;
    }
    c.valid = true;
    cal = c;
    return true;
}

void saveCalibration()
{
    prefs.putBool("calok", cal.valid);
    prefs.putBool("swap", cal.swapXY);
    prefs.putFloat("xa", cal.xa);
    prefs.putFloat("xb", cal.xb);
    prefs.putFloat("ya", cal.ya);
    prefs.putFloat("yb", cal.yb);
    Serial.println("[CAL] saved to flash (Preferences)");
}

void loadSettings()
{
    prefs.begin("lab9", false);
    if (prefs.getBool("calok", false)) {
        cal.valid = true;
        cal.swapXY = prefs.getBool("swap", false);
        cal.xa = prefs.getFloat("xa", 0.0f);
        cal.xb = prefs.getFloat("xb", 0.0f);
        cal.ya = prefs.getFloat("ya", 0.0f);
        cal.yb = prefs.getFloat("yb", 0.0f);
        Serial.printf("[CAL] loaded from flash: swapXY=%s x: %.4f*u%+.1f  y: %.4f*v%+.1f\n", cal.swapXY ? "yes" : "no", cal.xb, cal.xa,
                      cal.yb, cal.ya);
    } else {
        Serial.println("[CAL] no saved calibration: the calibration screen will start");
    }
    for (int i = 0; i < TIMER_COUNT; i++) {
        char key[8];
        snprintf(key, sizeof(key), "tm%d", i);
        const uint8_t minutes = prefs.getUChar(key, app::TIMER_DEFAULT_MINUTES);
        timers[i].presetMin = (minutes >= app::TIMER_MIN_MINUTES && minutes <= app::TIMER_MAX_MINUTES) ? minutes : app::TIMER_DEFAULT_MINUTES;
    }
    backlightPercent = prefs.getUChar("bl", app::BACKLIGHT_DEFAULT_PERCENT);
    applyTheme(prefs.getBool("dark", true));
}

// ---------------------------------------------------------------------
// อ่านจอสัมผัส: กันสัญญาณรบกวน (debounce) และสร้างเหตุการณ์ กดลง / กำลังกด / ปล่อย
// ---------------------------------------------------------------------
void pollTouch(uint32_t now)
{
    touch.pressEdge = false;
    touch.releaseEdge = false;
    if (now - lastTouchPollAt < app::TOUCH_POLL_MS) return;
    lastTouchPollAt = now;

    RawTouch raw;
    const bool touching = readRawTouch(raw);
    touch.touchingNow = touching;
    touch.z = raw.z;
    if (touching) {
        touch.rawX = raw.x;
        touch.rawY = raw.y;
        if (touch.pressCount < 255) touch.pressCount++;
        touch.releaseCount = 0;
    } else {
        if (touch.releaseCount < 255) touch.releaseCount++;
        touch.pressCount = 0;
    }

    if (!touch.down && touch.pressCount >= app::TOUCH_PRESS_SAMPLES) {  // ต้องเห็นการแตะติดกันหลายครั้งถึงนับว่ากดลง
        touch.down = true;
        touch.pressEdge = true;
        touch.downSince = now;
        touch.smoothReady = false;
    }

    if (touch.down && touching && cal.valid) {
        int sx, sy;
        mapRawToScreen(raw.x, raw.y, sx, sy);
        if (!touch.smoothReady) {
            touch.x = sx;
            touch.y = sy;
            touch.smoothReady = true;
        } else {  // ตัวกรอง EMA ให้พิกัดนิ่งขึ้นขณะลากนิ้ว
            touch.x += static_cast<int>(lroundf(app::TOUCH_SMOOTH_ALPHA * static_cast<float>(sx - touch.x)));
            touch.y += static_cast<int>(lroundf(app::TOUCH_SMOOTH_ALPHA * static_cast<float>(sy - touch.y)));
        }
        touch.lastX = touch.x;
        touch.lastY = touch.y;
    }

    if (touch.down && touch.releaseCount >= app::TOUCH_RELEASE_SAMPLES) {  // ไม่เห็นการแตะติดกันหลายครั้ง = ปล่อยแล้ว
        touch.down = false;
        touch.releaseEdge = true;
    }
}

// ---------------------------------------------------------------------
// ชื่อสำหรับ Serial
// ---------------------------------------------------------------------
const char *widgetName(Widget w)
{
    switch (w) {
        case Widget::None: return "-";
        case Widget::TabDash: return "TabDash";
        case Widget::TabTimers: return "TabTimers";
        case Widget::TabSystem: return "TabSystem";
        case Widget::ClockCard: return "ClockCard";
        case Widget::DashTimer0: return "DashTimer1";
        case Widget::DashTimer1: return "DashTimer2";
        case Widget::DashTimer2: return "DashTimer3";
        case Widget::TimerStart0: return "TimerStart1";
        case Widget::TimerStart1: return "TimerStart2";
        case Widget::TimerStart2: return "TimerStart3";
        case Widget::TimerSet0: return "TimerSet1";
        case Widget::TimerSet1: return "TimerSet2";
        case Widget::TimerSet2: return "TimerSet3";
        case Widget::BlSlider: return "BlSlider";
        case Widget::SyncBtn: return "SyncBtn";
        case Widget::TestBtn: return "TestBtn";
        case Widget::RecalBtn: return "RecalBtn";
        case Widget::ThemeBtn: return "ThemeBtn";
        case Widget::TestPaint: return "TestPaint";
        case Widget::TestBack: return "TestBack";
        case Widget::TestClear: return "TestClear";
    }
    return "?";
}

const char *modeName(Mode m)
{
    return m == Mode::Auto ? "AUTO" : "MANUAL";
}

const char *pageName(Page p)
{
    switch (p) {
        case Page::Dashboard: return "DASH";
        case Page::Timers: return "TIMERS";
        case Page::System: return "SYSTEM";
        case Page::Test: return "TEST";
        case Page::Calibrate: return "CALIBRATE";
    }
    return "?";
}

// ---------------------------------------------------------------------
// ตรวจว่าจุดที่แตะอยู่บนปุ่มไหน (hit test)
// ---------------------------------------------------------------------
Widget hitTest(Page p, int x, int y)
{
    if (p == Page::Dashboard || p == Page::Timers || p == Page::System) {
        if (contains(TAB_RECTS[0], x, y)) return Widget::TabDash;
        if (contains(TAB_RECTS[1], x, y)) return Widget::TabTimers;
        if (contains(TAB_RECTS[2], x, y)) return Widget::TabSystem;
    }
    switch (p) {
        case Page::Dashboard:
            if (contains(CLOCK_CARD, x, y)) return Widget::ClockCard;
            // การ์ด Timer 1-3 ในหน้า DASH ใช้ดูข้อมูลอย่างเดียว ไม่รับการแตะ (ควบคุมที่แท็บ TIMERS)
            break;
        case Page::Timers:
            for (int i = 0; i < TIMER_COUNT; i++) {
                if (contains(timerStartBtn(i), x, y)) return static_cast<Widget>(static_cast<int>(Widget::TimerStart0) + i);
                if (contains(timerSetBtn(i), x, y)) return static_cast<Widget>(static_cast<int>(Widget::TimerSet0) + i);
            }
            break;
        case Page::System:
            if (contains(BL_SLIDER_AREA, x, y)) return Widget::BlSlider;
            if (contains(SYNC_BTN, x, y)) return Widget::SyncBtn;
            if (contains(TEST_BTN, x, y)) return Widget::TestBtn;
            if (contains(RECAL_BTN, x, y)) return Widget::RecalBtn;
            if (contains(THEME_BTN, x, y)) return Widget::ThemeBtn;
            break;
        case Page::Test:
            if (contains(BACK_BTN, x, y)) return Widget::TestBack;
            if (contains(CLEAR_BTN, x, y)) return Widget::TestClear;
            if (contains(PAINT_BOX, x, y)) return Widget::TestPaint;
            break;
        case Page::Calibrate:
            break;
    }
    return Widget::None;
}

// ---------------------------------------------------------------------
// วาดปุ่ม/แท็บ/แถบเลื่อน (แต่ละปุ่มดูเปลี่ยนไปเมื่อถูกกด)
// ---------------------------------------------------------------------
void drawCardBorder(const Rect &r, bool pressed)
{
    tft.drawRoundRect(r.x, r.y, r.w, r.h, 6, pressed ? COLOR_TEXT : COLOR_BORDER);
    tft.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 5, pressed ? COLOR_TEXT : COLOR_CARD);
}

void drawTab(int index)
{
    const Widget w = static_cast<Widget>(static_cast<int>(Widget::TabDash) + index);
    const Page tabPage = static_cast<Page>(index);
    const bool active = page == tabPage;
    const bool pressed = pressedWidget == w;
    drawButton(TAB_RECTS[index], TAB_NAMES[index], pressed ? COLOR_PRESSED : (active ? COLOR_ACTIVE : COLOR_BUTTON),
               active ? COLOR_TEXT : COLOR_BORDER, COLOR_TEXT);
}

void drawTabBar()
{
    for (int i = 0; i < 3; i++) drawTab(i);
}

// แถบเลื่อน: พื้นที่ area (ล้างก่อน) รางยาว track เติมสีตามเปอร์เซ็นต์ และปุ่มวงกลมบอกตำแหน่ง
void drawSlider(const Rect &area, const Rect &track, int percent, uint16_t color)
{
    tft.fillRect(area.x, area.y, area.w, area.h, COLOR_BG);
    tft.fillRoundRect(track.x, track.y, track.w, track.h, 4, COLOR_BORDER);
    const int16_t filled = static_cast<int16_t>(percent * track.w / 100);
    if (filled > 0) {
        tft.fillRoundRect(track.x, track.y, filled, track.h, 4, color);
    }
    const int16_t thumbX = track.x + filled;
    const int16_t thumbY = track.y + track.h / 2;
    tft.fillCircle(thumbX, thumbY, 11, COLOR_TEXT);
    tft.drawCircle(thumbX, thumbY, 11, color);
}

int sliderPercentFromX(const Rect &track, int x)
{
    return constrain((x - track.x) * 100 / track.w, 0, 100);
}

void drawRecalButton(int progressPercent)
{
    const bool pressed = pressedWidget == Widget::RecalBtn;
    drawButton(RECAL_BTN, "RECALIBRATE (hold 1s)", pressed ? COLOR_PRESSED : COLOR_BUTTON, COLOR_BORDER, COLOR_TEXT);
    if (progressPercent > 0) {
        const int16_t w = static_cast<int16_t>((RECAL_BTN.w - 8) * progressPercent / 100);
        tft.fillRect(RECAL_BTN.x + 4, RECAL_BTN.y + RECAL_BTN.h - 9, w, 6, COLOR_GREEN);
    }
}

int timerIndex(Widget w, Widget first)
{
    return static_cast<int>(w) - static_cast<int>(first);
}

// วาด widget ที่ต้องเปลี่ยนรูปตามสถานะ "ถูกกด"
void redrawWidget(Widget w)
{
    const bool pressed = pressedWidget == w;
    switch (w) {
        case Widget::TabDash: drawTab(0); break;
        case Widget::TabTimers: drawTab(1); break;
        case Widget::TabSystem: drawTab(2); break;
        case Widget::ClockCard: drawCardBorder(CLOCK_CARD, pressed); break;
        case Widget::DashTimer0:
        case Widget::DashTimer1:
        case Widget::DashTimer2:
            drawCardBorder(DASH_TIMER_RECTS[timerIndex(w, Widget::DashTimer0)], pressed);
            break;
        case Widget::TimerStart0:
        case Widget::TimerStart1:
        case Widget::TimerStart2: {
            const int i = timerIndex(w, Widget::TimerStart0);
            const bool running = timers[i].running;
            drawButton(timerStartBtn(i), running ? "STOP" : "START", pressed ? COLOR_PRESSED : (running ? COLOR_BUTTON : COLOR_ACTIVE_GREEN),
                       running ? COLOR_RED : COLOR_GREEN, COLOR_TEXT);
            break;
        }
        case Widget::TimerSet0:
        case Widget::TimerSet1:
        case Widget::TimerSet2:
            drawButton(timerSetBtn(timerIndex(w, Widget::TimerSet0)), "SET", pressed ? COLOR_PRESSED : COLOR_BUTTON, COLOR_BORDER,
                       COLOR_TEXT);
            break;
        case Widget::SyncBtn: drawButton(SYNC_BTN, "SYNC NTP", pressed ? COLOR_PRESSED : COLOR_BUTTON, COLOR_BORDER, COLOR_TEXT); break;
        case Widget::TestBtn: drawButton(TEST_BTN, "TOUCH TEST", pressed ? COLOR_PRESSED : COLOR_BUTTON, COLOR_BORDER, COLOR_TEXT); break;
        case Widget::RecalBtn: drawRecalButton(0); break;
        case Widget::ThemeBtn:
            drawButton(THEME_BTN, darkTheme ? "DARK" : "LIGHT", pressed ? COLOR_PRESSED : COLOR_BUTTON, COLOR_BORDER, COLOR_TEXT);
            break;
        case Widget::TestBack: drawButton(BACK_BTN, "BACK", pressed ? COLOR_PRESSED : COLOR_BUTTON, COLOR_BORDER, COLOR_TEXT); break;
        case Widget::TestClear: drawButton(CLEAR_BTN, "CLEAR", pressed ? COLOR_PRESSED : COLOR_BUTTON, COLOR_BORDER, COLOR_TEXT); break;
        case Widget::BlSlider:
        case Widget::TestPaint:
        case Widget::None:
            break;
    }
}

// ---------------------------------------------------------------------
// หน้าจอ: วาดส่วนที่ไม่เปลี่ยนครั้งเดียวตอนเข้าหน้า แล้วอัปเดตเฉพาะส่วนที่เปลี่ยน
// ---------------------------------------------------------------------
void resetDrawnKeys()
{
    drawnTimeKey = NEVER;
    drawnDateKey = NEVER;
    drawnBadgeKey = NEVER;
    drawnSyncCount = NEVER;
    for (int i = 0; i < TIMER_COUNT; i++) {
        drawnTimerTime[i] = NEVER;
        drawnTimerState[i] = NEVER;
    }
    drawnBacklight = NEVER;
    drawnHeaderTime = NEVER;
    drawnTestKey = NEVER;
}

void drawCalibrationTarget()
{
    int tx, ty;
    calTarget(calStep, tx, ty);
    tft.drawFastHLine(tx - 12, ty, 25, COLOR_GREEN);
    tft.drawFastVLine(tx, ty - 12, 25, COLOR_GREEN);
    tft.drawCircle(tx, ty, 7, COLOR_GREEN);
}

void eraseCalibrationTarget(int step)
{
    int tx, ty;
    calTarget(step, tx, ty);
    tft.fillRect(tx - 14, ty - 14, 29, 29, COLOR_BG);
}

void drawCalibrationScreen()
{
    tft.fillScreen(COLOR_BG);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString("Touch calibration", SCREEN_W / 2, 70, 4);
    tft.setTextColor(COLOR_DIM, COLOR_BG);
    tft.drawString("Touch the CENTER of each green cross,", SCREEN_W / 2, 108, 2);
    tft.drawString("hold until the next cross appears.", SCREEN_W / 2, 128, 2);
    tft.drawString("(a fingernail or a stylus is more accurate)", SCREEN_W / 2, 148, 2);
    tft.setTextDatum(TL_DATUM);
    char text[24];
    snprintf(text, sizeof(text), "Point %d / 4", calStep + 1);
    drawText(text, 112, 176, 2, COLOR_YELLOW, COLOR_BG, 96);
    drawCalibrationTarget();
}

// ไอคอนนาฬิกาจับเวลา (วาดด้วยเส้น/วงกลม ไม่ใช้ไฟล์ภาพ) กึ่งกลางที่ (cx, cy) รัศมีวงแหวน r ปรับขนาดได้
void drawStopwatchIcon(int16_t cx, int16_t cy, int16_t r, uint16_t bg)
{
    const uint16_t ring = rgb565(70, 130, 190);
    const uint16_t face = rgb565(228, 232, 240);
    const uint16_t wedge = rgb565(222, 112, 96);
    const uint16_t okFill = rgb565(150, 205, 150);
    const uint16_t okLine = rgb565(50, 140, 80);

    const int16_t ri = r * 21 / 25;  // รัศมีหน้าปัด
    const int16_t p = ri * 7 / 10;
    const int16_t capW = max<int16_t>(r / 2, 4);
    const int16_t capH = max<int16_t>(r / 4, 2);
    tft.fillRoundRect(cx - capW / 2, cy - r - capH + 1, capW, capH, 1, ring);  // ปุ่มด้านบน
    tft.fillCircle(cx, cy, r, ring);
    tft.fillCircle(cx, cy, ri, face);
    tft.fillTriangle(cx, cy, cx, cy - ri, cx + p, cy - p, wedge);  // ส่วนสีแดง (1/4 วง)
    tft.fillTriangle(cx, cy, cx + p, cy - p, cx + ri, cy, wedge);
    if (r >= 16) {
        tft.drawFastVLine(cx, cy + ri - 5, 5, ring);  // ขีดบอกเวลา
        tft.drawFastHLine(cx - ri, cy, 5, ring);
    }
    tft.fillCircle(cx, cy, max<int16_t>(r / 12, 1), ring);

    const int16_t br = max<int16_t>(r * 11 / 25, 5);  // ป้ายถูกต้องมุมล่างขวา
    const int16_t bx = cx + r * 22 / 25, by = cy + r * 18 / 25;
    const int16_t a = br / 2;
    tft.fillCircle(bx, by, br + 2, bg);
    tft.fillCircle(bx, by, br, okFill);
    tft.drawCircle(bx, by, br, okLine);
    for (int8_t d = 0; d < 2; d++) {
        tft.drawLine(bx - a, by + d, bx - a / 4, by + a * 3 / 4 + d, okLine);
        tft.drawLine(bx - a / 4, by + a * 3 / 4 + d, bx + a, by - a * 3 / 4 + d, okLine);
    }
}

void beginCalibration()
{
    page = Page::Calibrate;
    calStep = 0;
    calAccepted = 0;
    calSamples = 0;
    calSumX = calSumY = 0;
    calWaitRelease = false;
    calMessageUntil = 0;
    pressTarget = pressedWidget = Widget::None;
    resetDrawnKeys();
    Serial.println("[CAL] touch the center of each cross (4 points)");
    drawCalibrationScreen();
}

void enterPage(Page next)
{
    if (next == Page::Calibrate) {
        beginCalibration();
        return;
    }
    Serial.printf("[UI] page -> %s\n", pageName(next));
    page = next;
    pressTarget = pressedWidget = Widget::None;
    resetDrawnKeys();
    tft.fillScreen(COLOR_BG);
    drawHeader("Wizard Timer");
    tft.fillCircle(HEARTBEAT_X, HEADER_H / 2, 4, COLOR_GREEN);  // จุด heartbeat ในแถบหัว (ชิดชื่อ Wizard Timer)
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(COLOR_HEADER_TEXT, COLOR_HEADER);
    tft.drawString("Approved by Itsara_Amnauy", SCREEN_W - 8, 6, 2);
    tft.setTextDatum(TL_DATUM);

    switch (next) {
        case Page::Dashboard:
            drawCard(CLOCK_CARD, "");
            for (int i = 0; i < TIMER_COUNT; i++) {
                const Rect &r = DASH_TIMER_RECTS[i];
                char name[12];
                snprintf(name, sizeof(name), "Timer %d", i + 1);
                drawCard(r, "");
                drawStopwatchIcon(r.x + 20, r.y + 16, 10, COLOR_CARD);
                drawText(name, r.x + 40, r.y + 8, 2, COLOR_DIM, COLOR_CARD);
            }
            break;
        case Page::Timers:
            for (int i = 0; i < TIMER_COUNT; i++) {
                const Rect &r = TIMER_ROWS[i];
                char name[12];
                snprintf(name, sizeof(name), "Timer %d", i + 1);
                drawCard(r, "");
                drawStopwatchIcon(r.x + 28, r.y + 28, 18, COLOR_CARD);
                drawText(name, r.x + 60, r.y + 6, 2, COLOR_DIM, COLOR_CARD);
                redrawWidget(static_cast<Widget>(static_cast<int>(Widget::TimerStart0) + i));
                redrawWidget(static_cast<Widget>(static_cast<int>(Widget::TimerSet0) + i));
            }
            break;
        case Page::System:
            drawText("BACKLIGHT", 16, 36, 2, COLOR_DIM, COLOR_BG);
            redrawWidget(Widget::SyncBtn);
            redrawWidget(Widget::TestBtn);
            drawRecalButton(0);
            redrawWidget(Widget::ThemeBtn);
            break;
        case Page::Test:
            drawText("TOUCH TEST  (draw in the box)", 16, 34, 2, COLOR_DIM, COLOR_BG);
            tft.drawRect(PAINT_BOX.x, PAINT_BOX.y, PAINT_BOX.w, PAINT_BOX.h, COLOR_BORDER);
            redrawWidget(Widget::TestBack);
            redrawWidget(Widget::TestClear);
            lastPaintX = lastPaintY = -1;
            break;
        case Page::Calibrate:
            break;
    }
    if (next == Page::Dashboard || next == Page::Timers || next == Page::System) {
        drawTabBar();
    }
}

// ป้ายสถานะนาฬิกา (เหมือน Lab 7)
struct Badge {
    const char *text;
    uint16_t color;
    int key;
};

Badge clockBadge(uint32_t now)
{
    if (wifiState == WifiState::NotConfigured) return {"NO WIFI", COLOR_RED, 1};
    if (ntpState == NtpState::Synced) return timeStale(now) ? Badge{"STALE", COLOR_YELLOW, 2} : Badge{"SYNCED", COLOR_GREEN, 3};
    if (ntpState == NtpState::Syncing) return {"SYNCING", COLOR_BLUE, 4};
    if (ntpState == NtpState::Failed) return {"NO SYNC", COLOR_RED, 5};
    if (wifiState == WifiState::Failed) return {"NO WIFI", COLOR_RED, 1};
    return {"WIFI...", COLOR_BLUE, 6};
}

// ---- Timer 1-3: วาดค่า (ใช้ร่วมกันทั้งการ์ดย่อในหน้า DASH และแถวใหญ่ในหน้า TIMERS) ----
void formatTimer(char *out, size_t size, uint32_t ms)
{
    // รูปแบบ HH:MM:SS (ชั่วโมงวนกลับที่ 100 ชั่วโมง)
    snprintf(out, size, "%02lu:%02lu:%02lu", static_cast<unsigned long>((ms / 3600000) % 100), static_cast<unsigned long>((ms / 60000) % 60),
             static_cast<unsigned long>((ms / 1000) % 60));
}

void updateTimerView(int i, uint32_t now, bool onDash)
{
    const Rect &r = onDash ? DASH_TIMER_RECTS[i] : TIMER_ROWS[i];
    const Timer &t = timers[i];
    char text[16];
    // แสดงเวลาที่เหลือ (นับถอยหลัง) ปัดขึ้นเป็นวินาที
    const int remainSec = static_cast<int>((timerRemaining(i, now) + 999) / 1000);
    const int stateKey = t.running ? 1 : 0;  // 0 = เอาต์พุต OFF, 1 = กำลังนับ (เอาต์พุต ON)
    const int timeKey = remainSec * 3 + stateKey;
    if (timeKey != drawnTimerTime[i]) {
        formatTimer(text, sizeof(text), static_cast<uint32_t>(remainSec) * 1000);
        drawText(text, r.x + (onDash ? 118 : 60), r.y + (onDash ? 3 : 24), 4,
                 stateKey == 1 ? COLOR_TEXT : COLOR_DIM, COLOR_CARD, 110);
        drawnTimerTime[i] = timeKey;
    }
    if (stateKey != drawnTimerState[i]) {
        const char *label = stateKey == 1 ? "ON" : "OFF";
        const uint16_t color = stateKey == 1 ? COLOR_GREEN : COLOR_DIM;
        drawText(label, r.x + (onDash ? 244 : 124), r.y + (onDash ? 8 : 6), 2, color, COLOR_CARD, onDash ? 56 : 44);
        drawnTimerState[i] = stateKey;
    }
}
// ---- หน้า DASH ----
void updateDashboard(uint32_t now)
{
    char text[40];
    const bool valid = timeValid();
    const time_t t = time(nullptr);

    const int timeKey = valid ? static_cast<int>(t % 86400) : -1;
    if (timeKey != drawnTimeKey) {
        if (valid) {
            formatLocalTime(text, sizeof(text), "%H:%M:%S");
        } else {
            snprintf(text, sizeof(text), "--:--:--");
        }
        tft.setTextSize(2);
        drawText(text, CLOCK_CARD.x + 8, CLOCK_CARD.y + 4, 4, valid ? (timeStale(now) ? COLOR_YELLOW : COLOR_TEXT) : COLOR_DIM,
                 COLOR_CARD, 200);
        tft.setTextSize(1);
        drawnTimeKey = timeKey;
    }

    // คีย์ของวันที่คำนวณจากเวลาท้องถิ่น (UTC+7) ไม่ใช่ UTC (เรียนรู้จาก Lab 7)
    const int dateKey = valid ? static_cast<int>((static_cast<long>(t) + app::GMT_OFFSET_SEC + app::DAYLIGHT_OFFSET_SEC) / 86400) : -1;
    if (dateKey != drawnDateKey) {
        if (valid) {
            formatLocalTime(text, sizeof(text), "%d/%m/%y");
        } else {
            text[0] = '\0';
        }
        drawText(text, CLOCK_CARD.x + 212, CLOCK_CARD.y + 24, 2, COLOR_DIM, COLOR_CARD, 84);
        drawnDateKey = dateKey;
    }

    const Badge badge = clockBadge(now);
    if (badge.key != drawnBadgeKey) {
        drawText(badge.text, CLOCK_CARD.x + 212, CLOCK_CARD.y + 6, 2, badge.color, COLOR_CARD, 84);
        drawnBadgeKey = badge.key;
    }
    if (static_cast<int>(syncCount) != drawnSyncCount) {
        snprintf(text, sizeof(text), "sync #%lu", static_cast<unsigned long>(syncCount));
        drawText(text, CLOCK_CARD.x + 212, CLOCK_CARD.y + 42, 2, COLOR_DIM, COLOR_CARD, 84);
        drawnSyncCount = static_cast<int>(syncCount);
    }

    for (int i = 0; i < TIMER_COUNT; i++) {
        updateTimerView(i, now, true);
    }
}

// ---- หน้า TIMERS ----
void updateTimersPage(uint32_t now)
{
    for (int i = 0; i < TIMER_COUNT; i++) {
        updateTimerView(i, now, false);
    }
}

// ---- หน้า SYSTEM ----
void updateSystemPage(uint32_t now)
{
    char text[48];
    if (backlightPercent != drawnBacklight) {
        drawSlider(BL_SLIDER_AREA, BL_SLIDER_TRACK, (backlightPercent - app::BACKLIGHT_MIN_PERCENT) * 100 / (100 - app::BACKLIGHT_MIN_PERCENT),
                   COLOR_YELLOW);
        snprintf(text, sizeof(text), "%d%%", backlightPercent);
        drawText(text, 240, 36, 2, COLOR_TEXT, COLOR_BG, 64);
        drawnBacklight = backlightPercent;
    }
}

void updateSystemInfo(uint32_t now)
{
    char text[48];
    char ago[16];
    if (wifiState == WifiState::Connected) {
        if (syncCount > 0) {
            formatAgo(ago, sizeof(ago), now - lastSyncAtMs);
            snprintf(text, sizeof(text), "WiFi %ddBm  sync #%lu  %s ago", WiFi.RSSI(), static_cast<unsigned long>(syncCount), ago);
        } else {
            snprintf(text, sizeof(text), "WiFi %ddBm  waiting for NTP", WiFi.RSSI());
        }
    } else {
        snprintf(text, sizeof(text), "WiFi: %s", wifiStateName(wifiState));
    }
    drawText(text, 16, 184, 2, COLOR_DIM, COLOR_BG, 288);
}

// ---- หน้า TOUCH TEST ----
void updateTestPage()
{
    char text[56];
    // ข้อความอัปเดตเมื่อค่าเปลี่ยน (ใช้ค่าดิบ+สถานะเป็นคีย์)
    const int key = (touch.touchingNow ? 1 : 0) + touch.rawX * 2 + touch.rawY * 8191 + touch.z * 3;
    if (key != drawnTestKey) {
        if (touch.touchingNow) {
            snprintf(text, sizeof(text), "RAW x=%4d y=%4d  z=%4d", touch.rawX, touch.rawY, touch.z);
        } else {
            snprintf(text, sizeof(text), "RAW  -    -       z=%4d", touch.z);
        }
        drawText(text, 16, 52, 2, COLOR_TEXT, COLOR_BG, 288);
        if (touch.down) {
            snprintf(text, sizeof(text), "PIXEL x=%3d y=%3d   DOWN", touch.x, touch.y);
        } else {
            snprintf(text, sizeof(text), "PIXEL last x=%3d y=%3d  UP", touch.lastX, touch.lastY);
        }
        drawText(text, 16, 70, 2, touch.down ? COLOR_GREEN : COLOR_DIM, COLOR_BG, 288);
        drawnTestKey = key;
    }
}

// ---------------------------------------------------------------------
// การทำงานของปุ่ม
// ---------------------------------------------------------------------
void setMode(Mode next)
{
    mode = next;
    if (mode == Mode::Auto) {
        fanOn = false;
    }
    Serial.printf("[MODE] -> %s\n", modeName(mode));
}

void toggleMode()
{
    setMode(mode == Mode::Auto ? Mode::Manual : Mode::Auto);
}

void manualDuty(int percent)
{
    if (mode != Mode::Manual) {
        setMode(Mode::Manual);
    }
    applyFanDuty(percent);
    Serial.printf("[FAN] manual duty = %d%% (value %lu/%lu)\n", fanDuty, static_cast<unsigned long>(fanValue),
                  static_cast<unsigned long>(app::PWM_MAX_VALUE));
}

void cycleSimChoice()
{
    simChoice = static_cast<SimChoice>((static_cast<uint8_t>(simChoice) + 1) % 3);
    if (simChoice == SimChoice::Off) {
        Serial.println("[SIM] off: using the real DHT11 sensor");
    } else if (simChoice == SimChoice::Random) {
        Serial.println("[SIM] random: simulated temperature/humidity (random walk)");
    } else {
        Serial.println("[SIM] sweep: simulated temperature/humidity (smooth up-down cycle)");
    }
}

// ---- ปุ่ม Timer ----
// Timer 1-3 ทำงานเป็นลำดับ: กด START ที่ Timer 1 เท่านั้น ครบเวลาแล้ว Timer 2 เริ่มต่อ แล้วจึง Timer 3
// กด STOP ที่ตัวที่กำลังนับ = หยุดและรีเซ็ตทั้งลำดับ
void redrawTimerStart(int i)
{
    if (page == Page::Timers) {
        redrawWidget(static_cast<Widget>(static_cast<int>(Widget::TimerStart0) + i));
    }
}

void timerStartAt(int i, uint32_t now)
{
    Timer &t = timers[i];
    t.startedAt = now;
    t.running = true;
    t.timeUp = false;
    Serial.printf("[TIMER] timer %d START: output ON for %lu s\n", i + 1, static_cast<unsigned long>(timerPreset(i) / 1000));
    redrawTimerStart(i);
}

void timerToggle(int i, uint32_t now)
{
    if (timers[i].running) {  // กด STOP: รีเซ็ตทุกตัวและเอาต์พุต OFF
        for (int k = 0; k < TIMER_COUNT; k++) {
            timers[k].running = false;
            timers[k].timeUp = false;
            redrawTimerStart(k);
        }
        Serial.printf("[TIMER] timer %d STOP: sequence reset, all outputs OFF\n", i + 1);
        return;
    }
    // ยังไม่ได้เริ่มลำดับ: ต้องเริ่มที่ Timer 1 เท่านั้น
    for (int k = 0; k < TIMER_COUNT; k++) {
        if (timers[k].running) {
            Serial.printf("[TIMER] sequence is running: timer %d cannot be started by hand\n", i + 1);
            return;
        }
    }
    if (i != 0) {
        Serial.println("[TIMER] sequence must start from timer 1: press START on timer 1");
        return;
    }
    timerStartAt(0, now);
}

// ครบเวลาตั้ง -> เอาต์พุต OFF แล้วเริ่ม timer ถัดไป (ตัวสุดท้ายครบ = จบลำดับ)
void updateTimers(uint32_t now)
{
    for (int i = 0; i < TIMER_COUNT; i++) {
        Timer &t = timers[i];
        if (t.running && timerElapsed(i, now) >= timerPreset(i)) {
            t.running = false;
            t.timeUp = false;
            Serial.printf("[TIMER] timer %d time up: output OFF\n", i + 1);
            redrawTimerStart(i);
            if (i + 1 < TIMER_COUNT) {
                timerStartAt(i + 1, t.startedAt + timerPreset(i));  // ต่อเวลาจากจุดที่ตัวก่อนหน้าสิ้นสุดพอดี ไม่คลาดเคลื่อนสะสม
            } else {
                Serial.println("[TIMER] sequence finished");
            }
        }
    }
}

// เพิ่มเวลาตั้งทีละ 1 นาที วนกลับเมื่อเกินค่าสูงสุด (ทำได้เฉพาะตอนยังไม่จ่ายไฟ) และจำไว้ตลอดไป
void timerNextPreset(int i)
{
    Timer &t = timers[i];
    if (t.running) {
        Serial.printf("[TIMER] timer %d is running: STOP it before changing the set time\n", i + 1);
        return;
    }
    t.presetMin = t.presetMin >= app::TIMER_MAX_MINUTES ? app::TIMER_MIN_MINUTES : t.presetMin + 1;
    char key[8];
    snprintf(key, sizeof(key), "tm%d", i);
    prefs.putUChar(key, t.presetMin);
    Serial.printf("[TIMER] timer %d set time = %lu s (saved)\n", i + 1, static_cast<unsigned long>(timerPreset(i) / 1000));
}
// คืนเวลาตั้งเป็นค่าเริ่มต้น (1 นาที) ทำได้เฉพาะตอนยังไม่จ่ายไฟ
void timerResetPreset(int i)
{
    Timer &t = timers[i];
    if (t.running) {
        Serial.printf("[TIMER] timer %d is running: STOP it before resetting the set time\n", i + 1);
        return;
    }
    t.presetMin = app::TIMER_DEFAULT_MINUTES;
    char key[8];
    snprintf(key, sizeof(key), "tm%d", i);
    prefs.putUChar(key, t.presetMin);
    Serial.printf("[TIMER] timer %d set time reset to %u min (saved)\n", i + 1, t.presetMin);
}

// เริ่มกด (กดลง) ที่ widget
void onPress(Widget w, uint32_t now)
{
    repeatAt = now + app::REPEAT_DELAY_MS;
    longPressDone = false;
    drawnLongPress = -1;
    if (w == Widget::TimerSet0 || w == Widget::TimerSet1 || w == Widget::TimerSet2) {
        tone(cyd::AUDIO_PIN, 2000, 60);  // ปิ๊บสั้นๆ ตอนกด SET (ไม่บล็อกโปรแกรม)
    }
}

// กำลังกดค้าง / ลาก
void onHold(Widget w, uint32_t now)
{
    switch (w) {
        case Widget::BlSlider: {
            const int fraction = sliderPercentFromX(BL_SLIDER_TRACK, touch.x);
            const int percent = app::BACKLIGHT_MIN_PERCENT + fraction * (100 - app::BACKLIGHT_MIN_PERCENT) / 100;
            if (percent != backlightPercent) applyBacklight(percent);
            break;
        }
        case Widget::RecalBtn:
            if (pressedWidget == w && !longPressDone) {
                const uint32_t held = now - touch.downSince;
                const int progress = static_cast<int>(min<uint32_t>(100, held * 100 / app::LONG_PRESS_MS));
                if (progress != drawnLongPress) {
                    drawRecalButton(progress);
                    drawnLongPress = progress;
                }
                if (held >= app::LONG_PRESS_MS) {
                    longPressDone = true;
                    Serial.println("[UI] long press: starting touch calibration");
                    beginCalibration();
                }
            }
            break;
        case Widget::TimerSet0:
        case Widget::TimerSet1:
        case Widget::TimerSet2:
            if (pressedWidget == w && !longPressDone && now - touch.downSince >= app::SET_RESET_HOLD_MS) {
                longPressDone = true;  // กันไม่ให้ตอนปล่อยนิ้วนับเป็นคลิกเพิ่ม 1 นาที
                timerResetPreset(timerIndex(w, Widget::TimerSet0));
            }
            break;
        case Widget::TestPaint:
            if (touch.x != lastPaintX || touch.y != lastPaintY) {
                tft.fillCircle(touch.x, touch.y, 2, COLOR_GREEN);
                lastPaintX = touch.x;
                lastPaintY = touch.y;
            }
            break;
        default:
            break;
    }
}

// สลับธีม Dark/Light แล้ววาดหน้าปัจจุบันใหม่ทั้งหน้า และจำค่าไว้ตลอดไป
void toggleTheme()
{
    applyTheme(!darkTheme);
    prefs.putBool("dark", darkTheme);
    Serial.printf("[UI] theme -> %s (saved)\n", darkTheme ? "DARK" : "LIGHT");
    if (page == Page::Calibrate) {
        drawCalibrationScreen();
    } else {
        enterPage(page);
    }
}

// ปล่อยนิ้วบน widget เดิมที่กดลง = "คลิก" ปุ่มนั้น
void onActivate(Widget w, uint32_t now)
{
    switch (w) {
        case Widget::TabDash: if (page != Page::Dashboard) enterPage(Page::Dashboard); break;
        case Widget::TabTimers: if (page != Page::Timers) enterPage(Page::Timers); break;
        case Widget::TabSystem: if (page != Page::System) enterPage(Page::System); break;
        case Widget::ClockCard: requestNtpNow(now); break;
        case Widget::DashTimer0:
        case Widget::DashTimer1:
        case Widget::DashTimer2: timerToggle(timerIndex(w, Widget::DashTimer0), now); break;
        case Widget::TimerStart0:
        case Widget::TimerStart1:
        case Widget::TimerStart2: timerToggle(timerIndex(w, Widget::TimerStart0), now); break;
        case Widget::TimerSet0:
        case Widget::TimerSet1:
        case Widget::TimerSet2:
            if (!longPressDone) timerNextPreset(timerIndex(w, Widget::TimerSet0));
            break;
        case Widget::SyncBtn: requestNtpNow(now); break;
        case Widget::TestBtn: enterPage(Page::Test); break;
        case Widget::ThemeBtn: toggleTheme(); break;
        case Widget::TestBack: enterPage(Page::System); break;
        case Widget::TestClear:
            tft.fillRect(PAINT_BOX.x + 1, PAINT_BOX.y + 1, PAINT_BOX.w - 2, PAINT_BOX.h - 2, COLOR_BG);
            lastPaintX = lastPaintY = -1;
            break;
        default:
            break;
    }
}

// ปล่อยนิ้ว (ไม่ว่าจะคลิกหรือไม่)
void onRelease(Widget w)
{
    if (w == Widget::BlSlider) {
        prefs.putUChar("bl", static_cast<uint8_t>(backlightPercent));  // จำความสว่างไว้ตลอดไป
        Serial.printf("[UI] backlight %d%% saved\n", backlightPercent);
    }
    if (w == Widget::RecalBtn && page == Page::System) {
        drawRecalButton(0);
    }
}

// ---------------------------------------------------------------------
// ประมวลผลเหตุการณ์สัมผัสของหน้าปกติ
// ---------------------------------------------------------------------
void handleTouchEvents(uint32_t now)
{
    if (touch.pressEdge) {
        pressTarget = hitTest(page, touch.x, touch.y);
        pressedWidget = pressTarget;
        Serial.printf("[TOUCH] down  x=%3d y=%3d  raw=(%4d,%4d) z=%4d  page=%s target=%s\n", touch.x, touch.y, touch.rawX, touch.rawY,
                      touch.z, pageName(page), widgetName(pressTarget));
        onPress(pressTarget, now);
        redrawWidget(pressTarget);
    }

    if (touch.down && pressTarget != Widget::None) {
        // นิ้วเลื่อนออกนอกปุ่ม = ปุ่มดูเหมือนไม่ถูกกด (ปล่อยตรงนั้นจะไม่ทำงาน) เหมือนปุ่มบนมือถือ
        const bool stillInside = hitTest(page, touch.x, touch.y) == pressTarget;
        const Widget wanted = stillInside ? pressTarget : Widget::None;
        if (wanted != pressedWidget) {
            pressedWidget = wanted;
            redrawWidget(pressTarget);
        }
        onHold(pressTarget, now);
    }

    if (touch.releaseEdge) {
        const Widget releasedOn = hitTest(page, touch.lastX, touch.lastY);
        const bool clicked = pressTarget != Widget::None && releasedOn == pressTarget;
        const uint32_t held = now - touch.downSince;
        Serial.printf("[TOUCH] up    x=%3d y=%3d  held=%lu ms  target=%s  %s\n", touch.lastX, touch.lastY, static_cast<unsigned long>(held),
                      widgetName(pressTarget), clicked ? "click" : "(not a click)");
        const Widget target = pressTarget;
        pressedWidget = Widget::None;
        pressTarget = Widget::None;
        redrawWidget(target);
        const Page before = page;
        if (clicked) onActivate(target, now);
        if (page == before) onRelease(target);
    }
}

// ---------------------------------------------------------------------
// หน้า calibrate: แตะเป้าทีละจุด เฉลี่ยค่าดิบ แล้วคำนวณค่าแปลงพิกัด
// ---------------------------------------------------------------------
void handleCalibration(uint32_t now)
{
    if (calMessageUntil != 0) {  // กำลังแสดงข้อความผลลัพธ์
        if (now >= calMessageUntil) {
            calMessageUntil = 0;
            if (calResultOk) {
                enterPage(Page::Dashboard);
            } else {
                beginCalibration();  // ล้มเหลว: เริ่มใหม่ (ไม่ใช้ค่าที่ผิด)
            }
        }
        return;
    }

    if (calWaitRelease) {  // รอให้ปล่อยนิ้วก่อนไปเป้าถัดไป
        if (!touch.down && !touch.touchingNow) {
            calWaitRelease = false;
            eraseCalibrationTarget(calStep);
            calStep++;
            if (calStep >= 4) {
                const bool ok = computeCalibration();
                calResultOk = ok;
                tft.fillRect(0, 60, SCREEN_W, 130, COLOR_BG);
                tft.setTextDatum(TC_DATUM);
                tft.setTextColor(ok ? COLOR_GREEN : COLOR_RED, COLOR_BG);
                tft.drawString(ok ? "Calibration saved" : "Calibration FAILED", SCREEN_W / 2, 100, 4);
                tft.setTextColor(COLOR_DIM, COLOR_BG);
                tft.drawString(ok ? "Starting the panel..." : "Please try again", SCREEN_W / 2, 140, 2);
                tft.setTextDatum(TL_DATUM);
                if (ok) saveCalibration();
                calMessageUntil = now + 1500;
            } else {
                char text[24];
                snprintf(text, sizeof(text), "Point %d / 4", calStep + 1);
                drawText(text, 112, 176, 2, COLOR_YELLOW, COLOR_BG, 96);
                drawCalibrationTarget();
                calSamples = 0;
                calAccepted = 0;
                calSumX = calSumY = 0;
            }
        }
        return;
    }

    if (touch.touchingNow) {
        calSamples++;
        if (calSamples > 3) {  // ข้ามตัวอย่างแรกๆ (ยังไม่นิ่ง)
            calSumX += touch.rawX;
            calSumY += touch.rawY;
            calAccepted++;
        }
        if (calAccepted >= app::CAL_SAMPLES) {
            calRawX[calStep] = static_cast<int>(calSumX / calAccepted);
            calRawY[calStep] = static_cast<int>(calSumY / calAccepted);
            Serial.printf("[CAL] point %d raw = (%d, %d)\n", calStep + 1, calRawX[calStep], calRawY[calStep]);
            int tx, ty;
            calTarget(calStep, tx, ty);
            tft.fillCircle(tx, ty, 5, COLOR_GREEN);  // จุดทึบ = รับค่าแล้ว (ให้ปล่อยนิ้ว)
            calWaitRelease = true;
        }
    } else {  // ยกนิ้วก่อนเก็บครบ = เริ่มเก็บใหม่
        calSamples = 0;
        calAccepted = 0;
        calSumX = calSumY = 0;
    }
}

// ---------------------------------------------------------------------
// Serial และปุ่ม BOOT
// ---------------------------------------------------------------------
void printHelp()
{
    Serial.println("[HELP] fan:  m = AUTO/MANUAL,  + / - = duty +-10% (MANUAL),  0-9 = duty 0..90% (MANUAL),  s = REAL/SIM");
    Serial.println("[HELP] time: n = ask NTP again,  t = print the time now");
    Serial.println("[HELP] touch: c = calibrate the touch screen again,  k = print the calibration and the last raw touch values");
    Serial.println("[HELP] h = help.   BOOT button on the board = AUTO/MANUAL");
}

void printTime(uint32_t now)
{
    char stamp[24];
    if (makeTimestamp(stamp, sizeof(stamp))) {
        char ago[16];
        formatAgo(ago, sizeof(ago), now - lastSyncAtMs);
        Serial.printf("[TIME] %s (UTC+%ld)  sync #%lu, last sync %s ago%s\n", stamp, app::GMT_OFFSET_SEC / 3600,
                      static_cast<unsigned long>(syncCount), ago, timeStale(now) ? "  STALE" : "");
    } else {
        Serial.printf("[TIME] NOT SYNCED  (wifi=%s ntp=%s)\n", wifiStateName(wifiState), ntpStateName(ntpState));
    }
}

void printTouchInfo()
{
    Serial.printf("[TOUCH] calibration: %s", cal.valid ? "valid" : "NOT calibrated");
    if (cal.valid) {
        Serial.printf("  swapXY=%s x: %.4f*u%+.1f  y: %.4f*v%+.1f", cal.swapXY ? "yes" : "no", cal.xb, cal.xa, cal.yb, cal.ya);
    }
    Serial.printf("\n[TOUCH] last raw=(%d,%d) z=%d  touching=%s  down=%s\n", touch.rawX, touch.rawY, touch.z, touch.touchingNow ? "yes" : "no",
                  touch.down ? "yes" : "no");
}

void printStatus()
{
    const uint32_t now = millis();
    const DataSource source = currentSource();
    const float tempC = effectiveTemp(now);
    const float humidity = effectiveHumidity(now);
    char tempText[16], humText[16];
    if (isnan(tempC)) snprintf(tempText, sizeof(tempText), "--"); else snprintf(tempText, sizeof(tempText), "%.1f", tempC);
    if (isnan(humidity)) snprintf(humText, sizeof(humText), "--"); else snprintf(humText, sizeof(humText), "%.0f", humidity);

    Serial.printf("[STATUS] page=%s src=%-10s temp=%s C hum=%s %% | mode=%s fan=%s duty=%3d%% on>=%.0f off<=%.0f | bl=%d%% touch z=%d%s\n",
                  pageName(page), sourceName(source), tempText, humText, modeName(mode), fanDuty > 0 ? "RUN " : "STOP", fanDuty, fanOnTempC,
                  fanOffTempC, backlightPercent, touch.z, sensorError ? "  SENSOR ERROR" : "");
}

void handleSerialInput(uint32_t now)
{
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c == 'm' || c == 'M') {
            toggleMode();
        } else if (c == '+' || c == '=') {
            manualDuty(fanDuty + app::MANUAL_STEP_PERCENT);
        } else if (c == '-' || c == '_') {
            manualDuty(fanDuty - app::MANUAL_STEP_PERCENT);
        } else if (c >= '0' && c <= '9') {
            manualDuty((c - '0') * 10);
        } else if (c == 's' || c == 'S') {
            cycleSimChoice();
        } else if (c == 'n' || c == 'N') {
            requestNtpNow(now);
        } else if (c == 't' || c == 'T') {
            printTime(now);
        } else if (c == 'c' || c == 'C') {
            beginCalibration();
        } else if (c == 'd' || c == 'D') {
            toggleTheme();
        } else if (c == 'k' || c == 'K') {
            printTouchInfo();
        } else if (c == 'h' || c == 'H' || c == '?') {
            printHelp();
        }
    }
}

bool bootButtonPressed(uint32_t now)
{
    static int stableState = HIGH;
    static uint32_t lastChangeAt = 0;
    const int reading = digitalRead(cyd::BOOT_BUTTON_PIN);
    if (reading != stableState && now - lastChangeAt >= app::BUTTON_DEBOUNCE_MS) {
        stableState = reading;
        lastChangeAt = now;
        return reading == LOW;
    }
    return false;
}

void updateHeartbeat()
{
    heartbeatOn = !heartbeatOn;
    if (page != Page::Calibrate) {
        tft.fillCircle(HEARTBEAT_X, HEADER_H / 2, 4, heartbeatOn ? COLOR_GREEN : COLOR_HEADER);
    }
}

}  // namespace

void setup()
{
    Serial.begin(app::SERIAL_BAUD);
    delay(200);

    Serial.println();
    Serial.println("==========================================");
    Serial.println("Hello World from ESP32-2432S028 (CYD)!");
    Serial.println("Level 1 / Lab 9 : Touch Panel");
    Serial.println("==========================================");
    Serial.printf("[TOUCH] XPT2046 pins: SCK=GPIO%u MISO=GPIO%u MOSI=GPIO%u CS=GPIO%u (IRQ GPIO%u unused)\n", cyd::TOUCH_PIN_CLK,
                  cyd::TOUCH_PIN_MISO, cyd::TOUCH_PIN_MOSI, cyd::TOUCH_PIN_CS, cyd::TOUCH_PIN_IRQ);

    cyd::ledInit();
    pinMode(cyd::BOOT_BUTTON_PIN, INPUT_PULLUP);

    loadSettings();  // ค่า calibrate และความสว่างจอที่เคยตั้งไว้

    tft.init();  // ไลบรารีจอเปิดไฟหลังจอ (digitalWrite) ให้ก่อน
    tft.setRotation(cyd::DISPLAY_ROTATION);
    tft.setTextDatum(TL_DATUM);
    tft.fillScreen(COLOR_BG);
    backlightBegin();  // จากนี้ GPIO21 ถูกคุมด้วย PWM (ปรับความสว่างได้)

    pwmBegin();
    dht.begin();
    lastReadAt = millis();

    // จอสัมผัส: เริ่ม SPI ชุด VSPI ด้วยขาของ XPT2046
    pinMode(cyd::TOUCH_PIN_CS, OUTPUT);
    digitalWrite(cyd::TOUCH_PIN_CS, HIGH);
    touchSpi.begin(cyd::TOUCH_PIN_CLK, cyd::TOUCH_PIN_MISO, cyd::TOUCH_PIN_MOSI, cyd::TOUCH_PIN_CS);

    // WiFi + NTP
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    sntp_set_time_sync_notification_cb(onTimeSync);
    sntp_set_sync_interval(app::NTP_RESYNC_MS);
    if (strlen(WIFI_SSID) > 0) {
        startWifiConnect();
    } else {
        Serial.println("[WIFI] include/secrets.h not found (or WIFI_SSID is empty): no real time (the touch panel still works)");
    }

    printHelp();
    if (cal.valid) {
        enterPage(Page::Dashboard);
    } else {
        beginCalibration();
    }
}

void loop()
{
    const uint32_t now = millis();

    // 1) จอสัมผัส (อ่านทุก TOUCH_POLL_MS) แล้วประมวลผลเหตุการณ์กด/ลาก/ปล่อย
    pollTouch(now);
    if (page == Page::Calibrate) {
        if (touch.pressEdge || touch.releaseEdge || touch.touchingNow || calWaitRelease || calMessageUntil != 0) {
            handleCalibration(now);
        }
    } else {
        handleTouchEvents(now);
    }

    // 2) อ่านเซนเซอร์เป็นระยะ (ใช้เวลา ~25 ms ระหว่างนั้นไม่ได้อ่านจอสัมผัส)
    if (now - lastReadAt >= app::DHT_READ_INTERVAL_MS) {
        lastReadAt = now;
        readDht();
    }

    // 3) ปุ่ม BOOT และคำสั่ง Serial
    if (bootButtonPressed(now)) {
        toggleMode();
    }
    handleSerialInput(now);

    // 4) WiFi และเวลา NTP
    updateWifi(now);
    updateNtp(now);
    setSyncLed(ntpState == NtpState::Synced);

    // 5) แหล่งข้อมูล: สุ่มค่า SIM และแจ้งเมื่อแหล่งเปลี่ยน
    const DataSource source = currentSource();
    if (source == DataSource::SimRandom && now - lastSimAt >= app::SIM_UPDATE_MS) {
        lastSimAt = now;
        updateSimulationRandom();
    }
    if (source != lastReportedSource) {
        Serial.printf("[SRC] data source: %s -> %s\n", sourceName(lastReportedSource), sourceName(source));
        lastReportedSource = source;
    }

    // 6) ตรรกะพัดลม + LED
    if (now - lastFanAt >= app::FAN_UPDATE_MS) {
        lastFanAt = now;
        if (mode == Mode::Auto) {
            updateFanAuto(now);
        }
        setErrorLed(sensorError && simChoice == SimChoice::Off);
    }

    // 6.5) Timer 1-3 (ON-delay)
    updateTimers(now);

    // 7) วาดจอ (เฉพาะส่วนที่เปลี่ยน ของหน้าที่เปิดอยู่)
    if (page != Page::Calibrate && now - lastDisplayAt >= app::DISPLAY_REFRESH_MS) {
        lastDisplayAt = now;
        switch (page) {
            case Page::Dashboard: updateDashboard(now); break;
            case Page::Timers: updateTimersPage(now); break;
            case Page::System: updateSystemPage(now); break;
            case Page::Test: updateTestPage(); break;
            case Page::Calibrate: break;
        }
    }
    if (page == Page::System && now - lastStatusAt >= app::STATUS_REFRESH_MS) {
        lastStatusAt = now;
        updateSystemInfo(now);
    }
    if (now - lastHeartbeatAt >= app::HEARTBEAT_MS) {
        lastHeartbeatAt = now;
        updateHeartbeat();
    }

    if (now - lastSerialAt >= app::SERIAL_PRINT_MS * 5) {  // สรุปสถานะทุก 5 วินาที (ไม่ให้รบกวนบรรทัด [TOUCH])
        lastSerialAt = now;
        printStatus();
    }
    if (now - lastTimePrintAt >= app::TIME_PRINT_MS * 6) {
        lastTimePrintAt = now;
        printTime(now);
    }
}
