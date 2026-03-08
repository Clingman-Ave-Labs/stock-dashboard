/*
   Clingman Ave Labs - Stock Dashboard v1.21
   ---------------------------------------------------------------
   - Multi-ticker (up to 10 Yahoo symbols, drag-to-reorder)
   - Yahoo Finance v8 chart API (no API key)
   - 8 chart views, individually togglable
   - Configurable refresh interval (5-120 min)
   - PSRAM-backed JSON parsing for large responses
   - Dynamic trading hours from Yahoo (crypto/forex 24/7 support)
   - Smart weekend sleep (sleeps until next trading session)
   - Button debounce: 200ms ignore window after ext0 wake
   - Y-axis scales from penny stocks ($0.002) to BTC ($100k+)
   - Division-by-zero protection in all charting math
   - Config portal: WiFi + Dashboard + Factory Reset
   - Live ticker validation via Yahoo AP+STA
   - NVS keys use fixed char buffers (no String alloc)
   - (c) 2026 Parker Redding & Clingman Ave Labs
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include "heltec-eink-modules.h"
#include <Fonts/FreeSansBold9pt7b.h>

// ---------------------------------------------------------------
// HARDWARE
// ---------------------------------------------------------------
#define BAT_PIN         7
#define BAT_CTRL_PIN    46
#define BAT_CALIBRATION 4.9
const gpio_num_t BUTTON_PIN = GPIO_NUM_21;

// ---------------------------------------------------------------
// CONSTANTS
// ---------------------------------------------------------------
#define uS_TO_S        1000000ULL
#define PORTAL_TIMEOUT 300000
#define LONG_HOLD_MS   3000
#define BTN_DEBOUNCE   200       // ms to ignore after ext0 wake
#define MAX_TICKERS    10

#define VIEW_1DAY  0
#define VIEW_5DAY  1
#define VIEW_1MO   2
#define VIEW_6MO   3
#define VIEW_YTD   4
#define VIEW_1YR   5
#define VIEW_5YR   6
#define VIEW_ALL   7
#define VIEW_COUNT 8

static const char* yRange[VIEW_COUNT]  = {"1d","5d","1mo","6mo","ytd","1y","5y","max"};
static const char* yIntvl[VIEW_COUNT]  = {"15m","15m","60m","1d","1d","1wk","1wk","1mo"};
static const char* vLabel[VIEW_COUNT]  = {"1 DAY","5 DAY","1 MONTH","6 MONTH","YTD","1 YEAR","5 YEARS","ALL"};

#define MAX_BARS 60
float  gPrices[MAX_BARS];
time_t gTimes[MAX_BARS];
int    gBarCnt = 0;

// ---------------------------------------------------------------
// NVS CONFIG
// ---------------------------------------------------------------
int     cfgRefresh = 30;
uint8_t cfgViews   = 0xFF;
int     cfgNTkr    = 0;
char    cfgTkr[MAX_TICKERS][20]; // fixed buffers
char    cfgSSID[64]="";
char    cfgPASS[64]="";
bool    cfgDashOK  = false;
bool    cfgEverOK  = false;  // has device EVER fetched data successfully? (NVS-backed)

// ---------------------------------------------------------------
// RTC-PERSISTED STATE
// ---------------------------------------------------------------
RTC_DATA_ATTR float  rPrice=0, rPct=0, rPrev=0;
RTC_DATA_ATTR float  rHigh=0, rLow=0, rOpen=0;
RTC_DATA_ATTR long   rVol=0;
RTC_DATA_ATTR char   rCur[8]  = "USD";
RTC_DATA_ATTR char   rTZ[64]  = "EST5EDT,M3.2.0,M11.1.0"; // 64-byte buffer
#define LOCAL_TZ "EST5EDT,M3.2.0,M11.1.0"
RTC_DATA_ATTR time_t rTrdStart=0, rTrdEnd=0;
RTC_DATA_ATTR time_t rUpdEpoch = 0;
RTC_DATA_ATTR int    rView=VIEW_1DAY, rTkrIdx=0;
RTC_DATA_ATTR bool   rMktOpen=false;
RTC_DATA_ATTR bool   rLastOK=false;   // was the most recent update successful?

// ---------------------------------------------------------------
// LAYOUT
// ---------------------------------------------------------------
#define CH_X    5
#define CH_W    158
#define CH_TOP  14
#define CH_BOT  118
#define CH_H    (CH_BOT-CH_TOP)
#define XL_Y    120
#define DV_X    168
#define INF_X   173
#define R_E     293
#define PL_CX   (CH_X+30+(CH_W-30)/2)

// AP SSID with hardware-seeded 4-hex suffix from eFuse MAC
static char AP_NAME[24] = "StockDash-Setup";
static const byte  DNS_P   = 53;

String gStatus = "Init";
WebServer   srv(80);
DNSServer   dns;
Preferences prefs;
EInkDisplay_VisionMasterE290 dsp;
bool gInet = false;

// ---------------------------------------------------------------
// NVS: LOAD (non-destructive read)
// ---------------------------------------------------------------
void nvsLoad() {
  char k[12];
  prefs.begin("wifi",true);
  strlcpy(cfgSSID, prefs.getString("ssid","").c_str(), sizeof(cfgSSID));
  strlcpy(cfgPASS, prefs.getString("pass","").c_str(), sizeof(cfgPASS));
  prefs.end();

  prefs.begin("cfg",true);
  cfgRefresh = constrain((int)prefs.getInt("refresh",30), 5, 120);
  cfgViews   = prefs.getUChar("views", 0xFF);
  if(!cfgViews) cfgViews = 0x01;
  cfgDashOK  = prefs.getBool("dashOK", false);
  cfgEverOK  = prefs.getBool("everOK", false);
  cfgNTkr    = constrain((int)prefs.getInt("nTkr",0), 0, MAX_TICKERS);
  for(int i=0; i<cfgNTkr; i++) {
    snprintf(k, sizeof(k), "t%d", i);
    strlcpy(cfgTkr[i], prefs.getString(k,"").c_str(), sizeof(cfgTkr[i]));
  }
  prefs.end();
}

void nvsWifi() {
  prefs.begin("wifi",false);
  prefs.putString("ssid", cfgSSID);
  prefs.putString("pass", cfgPASS);
  prefs.end();
}

void nvsDash() {
  char k[12];
  prefs.begin("cfg",false);
  prefs.putInt("refresh", cfgRefresh);
  prefs.putUChar("views", cfgViews);
  prefs.putBool("dashOK", true);
  cfgDashOK = true;
  prefs.putInt("nTkr", cfgNTkr);
  for(int i=0; i<cfgNTkr; i++) {
    snprintf(k, sizeof(k), "t%d", i);
    prefs.putString(k, cfgTkr[i]);
  }
  for(int i=cfgNTkr; i<MAX_TICKERS; i++) {
    snprintf(k, sizeof(k), "t%d", i);
    prefs.remove(k);
  }
  prefs.end();
}

// ---------------------------------------------------------------
// TICKER HELPERS
// ---------------------------------------------------------------
const char* sym() {
  if(rTkrIdx < cfgNTkr) return cfgTkr[rTkrIdx];
  if(cfgNTkr > 0) return cfgTkr[0];
  return "???";
}

String symLC() { String s = sym(); s.toLowerCase(); return s; }

String pfx() {
  String c(rCur);
  if(c=="EUR") return "E "; if(c=="USD") return "$ ";
  if(c=="GBP") return "L "; if(c=="GBp") return "p ";
  if(c=="JPY") return "Y "; if(c=="CHF") return "F ";
  if(c=="CAD") return "C$ "; return c+" ";
}

String sfx() {
  String c(rCur);
  if(c=="GBp") return " GBP";
  return " " + c;
}

void detectTZ(const char* ex) {
  String e(ex);
  if(e=="GER"||e=="FRA"||e.indexOf("XETRA")>=0)
    strlcpy(rTZ,"CET-1CEST,M3.5.0,M10.5.0/3",sizeof(rTZ));
  else if(e=="LSE"||e=="LON")
    strlcpy(rTZ,"GMT0BST,M3.5.0/1,M10.5.0",sizeof(rTZ));
  else if(e=="TYO"||e=="JPX")
    strlcpy(rTZ,"JST-9",sizeof(rTZ));
  else if(e=="HKG")
    strlcpy(rTZ,"HKT-8",sizeof(rTZ));
  else if(e=="CCC"||e=="CCY")
    strlcpy(rTZ,"UTC0",sizeof(rTZ));
  else if(e=="TSX"||e=="TOR")
    strlcpy(rTZ,"EST5EDT,M3.2.0,M11.1.0",sizeof(rTZ));
  else if(e=="ASX"||e=="AX")
    strlcpy(rTZ,"AEST-10AEDT,M10.1.0,M4.1.0/3",sizeof(rTZ));
  else
    strlcpy(rTZ,"EST5EDT,M3.2.0,M11.1.0",sizeof(rTZ));
}

// ---------------------------------------------------------------
// TRADING HOURS (Yahoo UNIX timestamps)
// ---------------------------------------------------------------
bool is24h() {
  return (rTrdEnd - rTrdStart) >= 82800; // >= 23h
}

bool isTradingNow() {
  time_t now; time(&now);
  if(rTrdStart == 0 || rTrdEnd == 0) return false;
  if(is24h()) return true;
  return (now >= rTrdStart && now < rTrdEnd);
}

// ---------------------------------------------------------------
// SLEEP (smart weekend handling)
// ---------------------------------------------------------------
uint64_t sleepSecs() {
  time_t now; time(&now);

  // Currently trading or 24/7 -> normal interval
  if(isTradingNow()) return (uint64_t)cfgRefresh * 60;

  // Next session start known and in the future
  if(rTrdStart > now) return (uint64_t)(rTrdStart - now);

  // Both in the past -> session ended, Yahoo gave us today's period
  // Estimate next open: for weekday, add ~16h; for Fri, add ~64h
  struct tm t;
  configTzTime(rTZ, "pool.ntp.org");
  if(!getLocalTime(&t)) return (uint64_t)cfgRefresh * 60;

  int dow = t.tm_wday;
  int cur = t.tm_hour * 60 + t.tm_min;

  // For 24h markets, just use refresh interval
  if(is24h()) return (uint64_t)cfgRefresh * 60;

  // Estimate next open from trading period duration
  // Default: assume market opens around 9:30 local next business day
  int mktOpenMin = 570; // 9:30 default
  if(rTrdStart > 0) {
    struct tm os;
    localtime_r(&rTrdStart, &os);
    mktOpenMin = os.tm_hour * 60 + os.tm_min;
  }

  if(dow >= 1 && dow <= 4) {
    // Mon-Thu after close: sleep until tomorrow open
    return (uint64_t)(((24*60) - cur + mktOpenMin) * 60);
  } else if(dow == 5) {
    // Friday after close: sleep until Monday open
    return (uint64_t)(((24*60) - cur + mktOpenMin + 2*24*60) * 60);
  } else if(dow == 6) {
    // Saturday: sleep until Monday open
    return (uint64_t)(((24*60) - cur + mktOpenMin + 24*60) * 60);
  } else {
    // Sunday: sleep until Monday open
    return (uint64_t)(((24*60) - cur + mktOpenMin) * 60);
  }
}

int nextView(int c) {
  for(int i=1; i<=VIEW_COUNT; i++) {
    int v = (c+i) % VIEW_COUNT;
    if(cfgViews & (1<<v)) return v;
  }
  return c;
}

// ---------------------------------------------------------------
// BATTERY (3.7V LiPo curve)
// ---------------------------------------------------------------
float batV() {
  pinMode(BAT_CTRL_PIN, OUTPUT);
  digitalWrite(BAT_CTRL_PIN, HIGH); delay(10);
  uint32_t r = 0;
  for(int i=0; i<10; i++) { r += analogReadMilliVolts(BAT_PIN); delay(2); }
  digitalWrite(BAT_CTRL_PIN, LOW);
  return (r / 10.0f) * (BAT_CALIBRATION / 1000.0f);
}

int batPct() {
  float v = batV(); int p;
  if(v >= 4.2f)      p = 100;
  else if(v >= 3.95f) p = map((long)(v*100), 395, 420, 75, 100);
  else if(v >= 3.7f)  p = map((long)(v*100), 370, 395, 50, 75);
  else if(v >= 3.5f)  p = map((long)(v*100), 350, 370, 25, 50);
  else if(v >= 3.3f)  p = map((long)(v*100), 330, 350, 5, 25);
  else if(v >= 3.0f)  p = map((long)(v*100), 300, 330, 0, 5);
  else p = 0;
  return constrain(p, 0, 100);
}

void drawBat(int x, int y) {
  int p = batPct();
  dsp.drawRect(x, y, 16, 8, BLACK);
  dsp.fillRect(x+16, y+2, 2, 4, BLACK);
  int fw = map(p, 0, 100, 0, 12);
  if(fw > 0) dsp.fillRect(x+2, y+2, fw, 4, BLACK);
  dsp.setTextSize(1); dsp.setFont(NULL);
  char b[6]; snprintf(b, sizeof(b), "%d%%", p);
  dsp.setCursor(x+20, y); dsp.print(b);
}

// ---------------------------------------------------------------
// DISPLAY HELPERS
// ---------------------------------------------------------------
void drR(const char* t, int re, int y, int sz) {
  dsp.setTextSize(sz); dsp.setFont(NULL);
  int w = strlen(t) * 6 * sz;
  dsp.setCursor(re - w, y); dsp.print(t);
}
void drR(String t, int re, int y, int sz) { drR(t.c_str(), re, y, sz); }

void drC(const char* t, int cx, int y, int sz) {
  dsp.setTextSize(sz); dsp.setFont(NULL);
  int w = strlen(t) * 6 * sz;
  dsp.setCursor(cx - w/2, y); dsp.print(t);
}
void drC(String t, int cx, int y, int sz) { drC(t.c_str(), cx, y, sz); }

// Y-axis: penny stocks to BTC-level, max ~5 chars
String fmtY(float v) {
  char b[12]; float a = fabsf(v);
  if(a < 0.001f)         snprintf(b, sizeof(b), "%.4f", v);    // 0.0003
  else if(a < 0.1f)      snprintf(b, sizeof(b), "%.3f", v);    // 0.042
  else if(a < 100.0f)    snprintf(b, sizeof(b), "%.2f", v);    // 36.39
  else if(a < 1000.0f)   snprintf(b, sizeof(b), "%.0f", v);    // 365
  else if(a < 10000.0f)  snprintf(b, sizeof(b), "%.2fk", v/1000); // 5.23k
  else if(a < 100000.0f) snprintf(b, sizeof(b), "%.1fk", v/1000); // 67.8k
  else                    snprintf(b, sizeof(b), "%.0fk", v/1000); // 234k
  return String(b);
}

// Compact for H/L/O/PC and change values: no 5-digit numbers
String fmtC(float v) {
  char b[12]; float a = fabsf(v);
  if(a < 0.001f)         snprintf(b, sizeof(b), "%.4f", v);
  else if(a < 0.1f)      snprintf(b, sizeof(b), "%.3f", v);
  else if(a < 100.0f)    snprintf(b, sizeof(b), "%.2f", v);
  else if(a < 10000.0f)  snprintf(b, sizeof(b), "%.0f", v);
  else if(a < 100000.0f) snprintf(b, sizeof(b), "%.1fk", v/1000);
  else                    snprintf(b, sizeof(b), "%.0fk", v/1000);
  return String(b);
}

String fmtVol(long v) {
  char b[16];
  if(v >= 1000000000L)   snprintf(b, sizeof(b), "Vol:%.1fB", (float)v/1e9);
  else if(v >= 1000000L) snprintf(b, sizeof(b), "Vol:%.1fM", (float)v/1e6);
  else if(v >= 1000L)    snprintf(b, sizeof(b), "Vol:%.0fK", (float)v/1e3);
  else                   snprintf(b, sizeof(b), "Vol:%ld", v);
  return String(b);
}

String fmtXL(time_t ts) {
  struct tm t; localtime_r(&ts, &t); char b[16];
  static const char* mo[] = {"Jan","Feb","Mar","Apr","May","Jun",
                              "Jul","Aug","Sep","Oct","Nov","Dec"};
  if(rView == VIEW_1DAY)
    snprintf(b, sizeof(b), "%d:%02d", t.tm_hour, t.tm_min);
  else if(rView == VIEW_5DAY) {
    static const char* dy[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
    snprintf(b, sizeof(b), "%s", dy[t.tm_wday]);
  } else if(rView >= VIEW_5YR)
    snprintf(b, sizeof(b), "%s'%02d", mo[t.tm_mon], (t.tm_year+1900)%100);
  else
    snprintf(b, sizeof(b), "%s %d", mo[t.tm_mon], t.tm_mday);
  return String(b);
}

// ---------------------------------------------------------------
// CHART (division-by-zero safe)
// ---------------------------------------------------------------
void drawChart() {
  int pX = CH_X+30, pW = CH_W-30, pY = CH_TOP, pH = CH_H;
  dsp.setTextSize(1); dsp.setFont(NULL);

  if(gBarCnt == 0) {
    dsp.setCursor(pX, pY + pH/2 - 4);
    dsp.print("No data yet");
    return;
  }

  float lo = gPrices[0], hi = gPrices[0];
  for(int i = 1; i < gBarCnt; i++) {
    if(gPrices[i] < lo) lo = gPrices[i];
    if(gPrices[i] > hi) hi = gPrices[i];
  }

  // Protect against flat line / single value
  float range = hi - lo;
  if(range < 0.0001f) {
    float mid = (hi + lo) / 2.0f;
    if(mid < 0.01f) mid = 0.01f; // penny stock floor
    float bump = mid * 0.02f;     // 2% padding
    if(bump < 0.001f) bump = 0.001f;
    lo = mid - bump;
    hi = mid + bump;
    range = hi - lo;
  }

  float pad = range * 0.08f;
  lo -= pad; hi += pad;
  float span = hi - lo;
  if(span < 0.0001f) span = 0.0001f; // absolute guard

  // Y-axis labels (right-aligned to axis)
  drR(fmtY(hi),  pX-2, pY, 1);
  drR(fmtY((hi+lo)/2), pX-2, pY+pH/2-3, 1);
  drR(fmtY(lo),  pX-2, pY+pH-7, 1);

  // Grid
  for(int g = pX; g < pX+pW; g += 3) {
    dsp.drawPixel(g, pY, BLACK);
    dsp.drawPixel(g, pY+pH/2, BLACK);
    dsp.drawPixel(g, pY+pH-1, BLACK);
  }
  dsp.drawLine(pX, pY, pX, pY+pH-1, BLACK);
  dsp.drawLine(pX, pY+pH-1, pX+pW-1, pY+pH-1, BLACK);

  // Plot
  int xD = max(gBarCnt - 1, 1);
  float xS = (float)(pW - 1) / (float)xD;
  for(int i = 0; i < gBarCnt - 1; i++) {
    int x1 = pX + (int)(i * xS);
    int x2 = pX + (int)((i+1) * xS);
    int y1 = pY + pH-1 - (int)(((gPrices[i]   - lo) / span) * (pH-1));
    int y2 = pY + pH-1 - (int)(((gPrices[i+1] - lo) / span) * (pH-1));
    dsp.drawLine(x1, constrain(y1,pY,pY+pH-1),
                 x2, constrain(y2,pY,pY+pH-1), BLACK);
  }

  // Dot on latest
  int lx = pX + (int)((gBarCnt-1) * xS);
  int ly = pY + pH-1 - (int)(((gPrices[gBarCnt-1] - lo) / span) * (pH-1));
  dsp.fillCircle(lx, constrain(ly,pY,pY+pH-1), 2, BLACK);

  // X ticks + labels
  int xM = pX+pW/2, xR = pX+pW-1, tY = pY+pH-1;
  dsp.drawLine(pX, tY, pX, tY+2, BLACK);
  dsp.drawLine(xM, tY, xM, tY+2, BLACK);
  dsp.drawLine(xR, tY, xR, tY+2, BLACK);

  if(gBarCnt >= 2) {
    dsp.setCursor(pX, XL_Y); dsp.print(fmtXL(gTimes[0]));
    drC(fmtXL(gTimes[gBarCnt/2]), xM, XL_Y, 1);
    drR(fmtXL(gTimes[gBarCnt-1]), pX+pW, XL_Y, 1);
  }
}

// ---------------------------------------------------------------
// MAIN SCREEN
// ---------------------------------------------------------------
void drawScreen() {
  dsp.setRotation(1); dsp.setTextColor(BLACK); dsp.clear();
  drawBat(5, 2);
  drC(vLabel[rView], PL_CX, 2, 1);
  if(rUpdEpoch) {
    setenv("TZ", LOCAL_TZ, 1); tzset();
    struct tm lt; localtime_r(&rUpdEpoch, &lt);
    char tb[16]; strftime(tb, sizeof(tb), "%H:%M", &lt);
    setenv("TZ", rTZ, 1); tzset();
    drR(tb, R_E, 2, 1);
  } else {
    drR("--:--", R_E, 2, 1);
  }
  if(!rLastOK && gStatus != "Init") {
    drR("OFF", 257, 2, 1);
  }
  dsp.drawLine(0, 12, 296, 12, BLACK);
  drawChart();
  for(int y = 12; y < 128; y += 2) dsp.drawPixel(DV_X, y, BLACK);

  int ry = 15;
  dsp.setTextSize(2); dsp.setFont(NULL);
  dsp.setCursor(INF_X, ry); dsp.print(sym()); ry += 20;
  char ps[20]; snprintf(ps, sizeof(ps), "%s%.2f", pfx().c_str(), rPrice);
  dsp.setCursor(INF_X, ry); dsp.print(ps); ry += 20;

  dsp.setTextSize(1);
  String pS = (rPct >= 0) ? "+" : "";
  if(rPct > -0.01f && rPct < 0.01f) pS = "";
  dsp.setCursor(INF_X, ry);
  dsp.print(pS + String(rPct, 2) + "%");
  float ac = rPrice - rPrev;
  String aS = (ac >= 0) ? "+" : "";
  String acFmt = aS + fmtC(ac) + sfx();
  char ab[24]; strlcpy(ab, acFmt.c_str(), sizeof(ab));
  drR(ab, R_E, ry, 1); ry += 12;

  dsp.drawLine(INF_X, ry, R_E, ry, BLACK); ry += 5;
  String hs = "H:" + fmtC(rHigh);
  dsp.setCursor(INF_X, ry); dsp.print(hs);
  String ls = "L:" + fmtC(rLow);
  dsp.setCursor(INF_X+66, ry); dsp.print(ls); ry += 11;
  String os = "O:" + fmtC(rOpen);
  dsp.setCursor(INF_X, ry); dsp.print(os);
  String pcs = "PC:" + fmtC(rPrev);
  dsp.setCursor(INF_X+60, ry); dsp.print(pcs); ry += 12;

  dsp.drawLine(INF_X, ry, R_E, ry, BLACK); ry += 5;
  if(rVol > 0) {
    dsp.setCursor(INF_X, ry); dsp.print(fmtVol(rVol)); ry += 11;
  }
  if(!rLastOK && gStatus != "Init") {
    if(!cfgEverOK) {
      dsp.setCursor(INF_X, ry); dsp.print("No WiFi."); ry += 9;
      dsp.setCursor(INF_X, ry); dsp.print("Check credentials."); ry += 9;
      dsp.setCursor(INF_X, ry); dsp.print("Hold btn to setup");
    } else {
      dsp.setCursor(INF_X, ry); dsp.print("Offline."); ry += 9;
      dsp.setCursor(INF_X, ry); dsp.print("Cached data shown."); ry += 9;
      dsp.setCursor(INF_X, ry); dsp.print("Retrying soon.");
    }
  } else {
    dsp.setCursor(INF_X, ry); dsp.print("finance.yahoo.com"); ry += 9;
    dsp.setCursor(INF_X, ry); dsp.print("/quote/" + symLC());
  }
  dsp.update();
}

// ---------------------------------------------------------------
// E-INK SPLASH SCREENS
// ---------------------------------------------------------------
void drawSplash(const char* mode, const char* line1, const char* line2,
                const char* helpUrl = nullptr) {
  dsp.setRotation(1); dsp.setTextColor(BLACK); dsp.clear();
  dsp.setFont(&FreeSansBold9pt7b); dsp.setTextSize(1);
  dsp.setCursor(5, 16); dsp.print("Stock Dashboard");
  dsp.setFont(NULL); dsp.setTextSize(1);
  dsp.setCursor(210, 4); dsp.print("v1.21");
  dsp.drawLine(5, 22, 291, 22, BLACK);
  dsp.setCursor(5, 30); dsp.print(mode);
  dsp.setCursor(5, 46); dsp.print(line1);
  dsp.setTextSize(2);
  dsp.setCursor(5, 60); dsp.print(AP_NAME);
  dsp.setTextSize(1);
  dsp.setCursor(5, 82); dsp.print("Open: 192.168.4.1");
  if(line2[0]) { dsp.setCursor(5, 96); dsp.print(line2); }
  dsp.drawLine(5, 110, 291, 110, BLACK);
  if(helpUrl) {
    dsp.setCursor(5, 114); dsp.print(helpUrl);
  } else {
    dsp.setCursor(5, 118); dsp.print("(c) 2026 P. Redding & Clingman Ave Labs");
  }
  dsp.update();
}

void splashFirstBoot() {
  drawSplash("WELCOME - FIRST TIME SETUP",
             "1. Connect phone to WiFi:",
             "2. Enter your WiFi credentials",
             "github.com/Clingman-Ave-Labs/stock-dashboard");
}
void splashDashSetup() {
  drawSplash("DASHBOARD SETUP REQUIRED",
             "Reconnect phone to WiFi:",
             "Go to Dashboard tab to add stocks.");
}
void splashConfig() {
  drawSplash("CONFIGURATION MODE",
             "Connect to WiFi:",
             "Add stocks, set interval, change WiFi",
             "github.com/Clingman-Ave-Labs/stock-dashboard");
}

// ---------------------------------------------------------------
// PORTAL HTML (with drag-to-reorder, no-wrap validation)
// ---------------------------------------------------------------
String buildHTML() {
  bool wOK = (strlen(cfgSSID) > 0);
  bool dFirst = (wOK && !cfgDashOK);

  String h = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Stock Dashboard v1.21</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0a0a0a;color:#e8e8e8;min-height:100vh;padding:16px}
.hdr{text-align:center;padding:20px 0 16px;border-bottom:1px solid #222;margin-bottom:20px}
.hdr .lab{font-size:11px;letter-spacing:3px;text-transform:uppercase;color:#555;margin-bottom:4px}
.hdr h1{font-size:24px;font-weight:700;color:#fff}
.tabs{display:flex;gap:0}
.tab{flex:1;padding:12px;text-align:center;background:#111;border:1px solid #222;color:#666;font-weight:600;font-size:13px;cursor:pointer;transition:all .2s}
.tab.active{background:#1a1a1a;color:#4a9eff;border-bottom-color:#1a1a1a}
.tab:first-child{border-radius:10px 0 0 0}.tab:last-child{border-radius:0 10px 0 0}
.pnl{background:#1a1a1a;border:1px solid #222;border-top:none;border-radius:0 0 10px 10px;padding:24px;display:none}
.pnl.active{display:block}
label{display:block;font-size:11px;font-weight:600;letter-spacing:1px;text-transform:uppercase;color:#666;margin:16px 0 6px}
label:first-child{margin-top:0}
input[type=text],input[type=password],input[type=number]{width:100%;padding:12px;background:#0f0f0f;border:1px solid #2a2a2a;border-radius:8px;color:#fff;font-size:15px}
input:focus{outline:none;border-color:#4a9eff}
.chk{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px}
.chk label{display:flex;align-items:center;gap:6px;font-size:13px;color:#aaa;letter-spacing:0;text-transform:none;margin:0;padding:8px 12px;background:#111;border:1px solid #222;border-radius:6px;cursor:pointer}
.chk input{width:auto;margin:0}.chk input:checked+span{color:#4a9eff}
.tr{display:flex;gap:8px;align-items:center;margin-bottom:8px;cursor:grab;background:#111;border:1px solid #222;border-radius:8px;padding:6px 8px}
.tr.dragging{opacity:0.4;border-color:#4a9eff}
.tr.over{border-top:2px solid #4a9eff}
.tr input{flex:1;min-width:80px;background:transparent;border:1px solid #333;border-radius:6px;padding:8px;color:#fff;font-size:14px}
.tr .grip{color:#444;font-size:18px;cursor:grab;user-select:none;padding:0 4px}
.tr .st{font-size:12px;flex-shrink:0;color:#888;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:120px}
.tr .rm{background:#333;color:#999;border:none;border-radius:6px;padding:6px 10px;cursor:pointer;font-size:14px;flex-shrink:0}
.tr .rm:hover{background:#c0392b;color:#fff}
.add{display:inline-block;padding:8px 16px;background:#222;color:#4a9eff;border:1px solid #333;border-radius:8px;cursor:pointer;font-size:13px;font-weight:600;margin-top:4px}
.add:hover{background:#333}
button[type=submit]{width:100%;padding:14px;background:#4a9eff;color:#fff;font-size:15px;font-weight:700;border:none;border-radius:10px;cursor:pointer;margin-top:20px;transition:all .2s}
button[type=submit]:hover{background:#3a8ef0}
button[type=submit]:disabled{background:#333;color:#666;cursor:not-allowed}
.msg{display:none;text-align:center;padding:16px;color:#4a9eff;font-size:14px;margin-top:12px}
.foot{text-align:center;font-size:11px;color:#333;margin-top:24px}
.sr{font-size:12px;color:#555;margin-top:4px;cursor:pointer;display:flex;align-items:center;gap:6px}
.sr input{width:auto;margin:0}
.warn{background:#2a1a00;border:1px solid #553300;border-radius:8px;padding:14px;margin-bottom:16px;color:#ffaa33;font-size:13px;line-height:1.5}
.vh{font-size:11px;color:#555;margin-top:4px;margin-bottom:12px}
.dng{background:#1a0a0a;border:1px solid #552222;border-radius:8px;padding:16px;margin-bottom:16px;color:#cc4444;font-size:13px;line-height:1.6}
.bd{width:100%;padding:14px;background:#c0392b;color:#fff;font-size:15px;font-weight:700;border:none;border-radius:10px;cursor:pointer;margin-top:12px}
.bd:hover{background:#e74c3c}.bd:disabled{background:#333;color:#666;cursor:not-allowed}
</style></head><body>
<div class="hdr"><div class="lab">Clingman Ave Labs</div><h1>Stock Dashboard v1.21</h1></div>
<div class="tabs">
<div class="tab)rawliteral";
  if(!dFirst) h += " active";
  h += R"rawliteral(" onclick="sw(0)">WiFi</div>
<div class="tab)rawliteral";
  if(dFirst) h += " active";
  h += R"rawliteral(" onclick="sw(1)">Dashboard</div>
<div class="tab" onclick="sw(2)">Reset</div>
</div>
<div class="pnl)rawliteral";
  if(!dFirst) h += " active";
  h += R"rawliteral(" id="p0">
<form id="wf" method="POST" action="/saveWifi">
<label>Network Name (SSID)</label>
<input type="text" name="ssid" value=")rawliteral";
  h += cfgSSID;
  h += R"rawliteral(" autocomplete="off" required>
<label>Password</label>
<input type="password" id="wp" name="pass" value=")rawliteral";
  h += cfgPASS;
  h += R"rawliteral(" autocomplete="off" required>
<label class="sr"><input type="checkbox" onclick="var p=document.getElementById('wp');p.type=p.type==='password'?'text':'password'">Show password</label>
<button type="submit">Save WiFi</button>
</form><div class="msg" id="wm">WiFi saved. Rebooting...</div>
</div>
<div class="pnl)rawliteral";
  if(dFirst) h += " active";
  h += R"rawliteral(" id="p1">)rawliteral";

  if(!wOK) {
    h += R"rawliteral(<div class="warn">Configure WiFi first. Save WiFi credentials, then return here to set up your dashboard.</div>
<button type="submit" disabled>Save WiFi First</button>)rawliteral";
  } else {
    h += R"rawliteral(<form id="df" method="POST" action="/saveDash">
<label>Refresh Interval (minutes)</label>
<input type="number" name="refresh" min="5" max="120" value=")rawliteral";
    h += String(cfgRefresh);
    h += R"rawliteral(">
<label>Enabled Views</label><div class="chk">)rawliteral";
    for(int i = 0; i < VIEW_COUNT; i++) {
      h += "<label><input type=\"checkbox\" name=\"v" + String(i) + "\" value=\"1\"";
      if(cfgViews & (1<<i)) h += " checked";
      h += "><span>" + String(vLabel[i]) + "</span></label>";
    }
    h += R"rawliteral(</div>
<label>Tracked Tickers</label>
<p class="vh">Drag to reorder. Each is validated live.</p>
<div id="tl">)rawliteral";
    for(int i = 0; i < cfgNTkr; i++) {
      h += "<div class=\"tr\" draggable=\"true\"><span class=\"grip\">::</span>"
           "<input type=\"text\" name=\"tkr\" value=\"" + String(cfgTkr[i]) +
           "\" placeholder=\"e.g. AAPL\" autocapitalize=\"characters\" oninput=\"vT(this)\">"
           "<div class=\"st\" id=\"s" + String(i) + "\"></div>"
           "<button type=\"button\" class=\"rm\" onclick=\"this.parentNode.remove();cA()\">X</button></div>";
    }
    h += R"rawliteral(</div>
<div class="add" onclick="aT()">+ Add Ticker</div>
<div id="ve" style="color:#ff5555;font-size:13px;margin-top:8px;display:none"></div>
<button type="submit" id="db">Save Dashboard</button>
</form><div class="msg" id="dm">Dashboard saved. Rebooting...</div>)rawliteral";
  }

  h += R"rawliteral(</div>
<div class="pnl" id="p2">
<div class="dng"><strong>Factory Reset</strong><br><br>
Erases all settings: WiFi, tickers, views, interval. Device reboots into first-time setup.<br><br>
This cannot be undone.</div>
<label style="font-size:13px;color:#888;cursor:pointer;margin-bottom:12px">
<input type="checkbox" id="rc" style="width:auto;margin-right:8px" onchange="document.getElementById('rb').disabled=!this.checked">
I understand this will erase everything</label>
<button class="bd" id="rb" disabled onclick="doR()">Factory Reset</button>
<div class="msg" id="rm">Resetting... rebooting.</div>
</div>
<div class="foot">&copy; 2026 Parker Redding &amp; Clingman Ave Labs</div>
<script>
function sw(n){document.querySelectorAll('.tab').forEach(function(t,i){t.classList.toggle('active',i==n)});document.querySelectorAll('.pnl').forEach(function(p,i){p.classList.toggle('active',i==n)});}
var vc={},tid=0;
function aT(){
  if(document.querySelectorAll('.tr').length>=10){alert('Max 10');return;}
  var id='n'+tid++;var d=document.createElement('div');d.className='tr';d.draggable=true;
  d.innerHTML='<span class="grip">::<\/span><input type="text" name="tkr" placeholder="e.g. AAPL" autocapitalize="characters" oninput="vT(this)"><div class="st" id="'+id+'"><\/div><button type="button" class="rm" onclick="this.parentNode.remove();cA()">X<\/button>';
  document.getElementById('tl').appendChild(d);initDrag();cA();
}
function vT(el){
  var s=el.value.trim().toUpperCase(),st=el.parentNode.querySelector('.st');
  if(!s){st.textContent="";st.style.color='#888';delete vc[el];cA();return;}
  st.textContent='...';st.style.color='#888';vc[el]='pending';
  clearTimeout(el._vt);
  el._vt=setTimeout(function(){
    fetch('/val?s='+encodeURIComponent(s)).then(function(r){return r.json()}).then(function(d){
      if(d.ok){var n=d.n?' '+d.n:"";st.innerHTML='\u2713<span style="color:#aaa;font-size:11px;margin-left:4px">'+n+'<\/span>';st.style.color='#2ecc71';vc[el]='ok';}
      else{st.textContent='\u2717 Not found';st.style.color='#e74c3c';vc[el]='bad';}
      cA();
    })["catch"](function(){st.textContent='? Offline';st.style.color='#f39c12';vc[el]='ok';cA();});
  },600);
}
function cA(){
  var ok=true,cnt=0;
  document.querySelectorAll('#tl .tr input[name=tkr]').forEach(function(el){if(el.value.trim()){cnt++;if(vc[el]==='bad')ok=false;}});
  if(cnt===0)ok=false;
  var b=document.getElementById('db'),e=document.getElementById('ve');
  if(b){b.disabled=!ok;}
  if(e){if(cnt===0)e.textContent='Add at least one ticker.';else e.textContent='Fix invalid tickers before saving.';e.style.display=ok?'none':'block';}
}
var dragEl=null;
function initDrag(){
  document.querySelectorAll('.tr').forEach(function(row){
    row.ondragstart=function(ev){dragEl=this;this.classList.add('dragging');ev.dataTransfer.effectAllowed='move';};
    row.ondragend=function(){this.classList.remove('dragging');document.querySelectorAll('.tr').forEach(function(r){r.classList.remove('over')});};
    row.ondragover=function(ev){ev.preventDefault();ev.dataTransfer.dropEffect='move';this.classList.add('over');};
    row.ondragleave=function(){this.classList.remove('over');};
    row.ondrop=function(ev){ev.preventDefault();this.classList.remove('over');if(dragEl!==this){document.getElementById('tl').insertBefore(dragEl,this);}};
  });
}
var wf=document.getElementById('wf');
if(wf)wf.addEventListener('submit',function(){this.querySelector('button').style.display='none';document.getElementById('wm').style.display='block';});
var df=document.getElementById('df');
if(df)df.addEventListener('submit',function(ev){
  var ok=true;document.querySelectorAll('#tl input[name=tkr]').forEach(function(el){if(el.value.trim()&&vc[el]==='bad')ok=false;});
  if(!ok){ev.preventDefault();return;}
  this.querySelector('#db').style.display='none';document.getElementById('dm').style.display='block';
});
function doR(){document.getElementById('rb').style.display='none';document.getElementById('rc').parentNode.style.display='none';document.getElementById('rm').style.display='block';fetch('/rst',{method:'POST'});}
document.querySelectorAll('#tl input[name=tkr]').forEach(function(el){if(el.value.trim())vT(el);});
initDrag();cA();
</script></body></html>)rawliteral";
  return h;
}

static const char OK_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Saved</title>
<style>*{margin:0;padding:0}body{font-family:-apple-system,sans-serif;background:#0a0a0a;color:#e8e8e8;min-height:100vh;display:flex;align-items:center;justify-content:center}.c{background:#1a1a1a;border-radius:16px;padding:40px 28px;max-width:400px;text-align:center}h1{margin:16px 0 10px}p{color:#888;font-size:15px}.n{margin-top:20px;font-size:13px;color:#555}</style></head>
<body><div class="c"><div style="font-size:56px">&#x2713;</div><h1>Saved!</h1><p>Rebooting with new settings.</p><p class="n">Close this tab.</p></div></body></html>
)rawliteral";

// ---------------------------------------------------------------
// WEB HANDLERS
// ---------------------------------------------------------------
bool gDone = false;

void hRoot()    { srv.send(200, "text/html", buildHTML()); }
void hRedir()   { srv.sendHeader("Location","http://192.168.4.1/",true); srv.send(302,"text/plain",""); }

void hVal() {
  String s = srv.arg("s"); s.trim(); s.toUpperCase();
  if(!s.length()) { srv.send(200,"application/json","{\"ok\":false}"); return; }
  if(!gInet) { srv.send(200,"application/json","{\"ok\":true,\"n\":\"(offline)\"}"); return; }

  WiFiClientSecure cl; cl.setInsecure(); HTTPClient h;
  String url = "https://query1.finance.yahoo.com/v8/finance/chart/" + s + "?range=1d&interval=1d";
  bool ok = false; String nm = "";
  if(h.begin(cl, url)) {
    h.setTimeout(8000); h.addHeader("User-Agent","Mozilla/5.0");
    if(h.GET() == 200) {
      DynamicJsonDocument d(4096);
      if(!deserializeJson(d, h.getString())) {
        JsonObject r = d["chart"]["result"][0];
        if(!r.isNull()) {
          ok = true;
          if(r["meta"].containsKey("longName"))
            nm = r["meta"]["longName"].as<String>();
          else if(r["meta"].containsKey("shortName"))
            nm = r["meta"]["shortName"].as<String>();
        }
      }
    }
    h.end();
  }
  String resp = "{\"ok\":" + String(ok?"true":"false");
  if(nm.length()) { nm.replace("\"","\\\""); resp += ",\"n\":\"" + nm + "\""; }
  resp += "}";
  srv.send(200, "application/json", resp);
}

void hSaveW() {
  if(srv.hasArg("ssid") && srv.hasArg("pass")) {
    strlcpy(cfgSSID, srv.arg("ssid").c_str(), sizeof(cfgSSID));
    strlcpy(cfgPASS, srv.arg("pass").c_str(), sizeof(cfgPASS));
    // Trim in place
    for(int i=strlen(cfgSSID)-1; i>=0 && cfgSSID[i]==' '; i--) cfgSSID[i]=0;
    for(int i=strlen(cfgPASS)-1; i>=0 && cfgPASS[i]==' '; i--) cfgPASS[i]=0;
    if(strlen(cfgSSID) > 0) {
      nvsWifi();
      srv.send_P(200, "text/html", OK_HTML);
      gDone = true; return;
    }
  }
  srv.sendHeader("Location","/",true); srv.send(302,"text/plain","");
}

void hSaveD() {
  if(!strlen(cfgSSID)) { hRedir(); return; }
  if(srv.hasArg("refresh"))
    cfgRefresh = constrain(srv.arg("refresh").toInt(), 5, 120);
  cfgViews = 0;
  for(int i=0; i<VIEW_COUNT; i++)
    if(srv.hasArg("v" + String(i))) cfgViews |= (1<<i);
  if(!cfgViews) cfgViews = 0x01;

  cfgNTkr = 0;
  for(int i=0; i<srv.args() && cfgNTkr < MAX_TICKERS; i++) {
    if(srv.argName(i) == "tkr") {
      String v = srv.arg(i); v.trim(); v.toUpperCase();
      if(v.length() > 0) {
        strlcpy(cfgTkr[cfgNTkr], v.c_str(), sizeof(cfgTkr[cfgNTkr]));
        cfgNTkr++;
      }
    }
  }
  if(!cfgNTkr) { hRedir(); return; }
  if(rTkrIdx >= cfgNTkr) rTkrIdx = 0;
  if(!(cfgViews & (1<<rView))) rView = nextView(rView);
  nvsDash();
  srv.send_P(200, "text/html", OK_HTML);
  gDone = true;
}

void hReset() {
  prefs.begin("wifi",false); prefs.clear(); prefs.end();
  prefs.begin("cfg",false);  prefs.clear(); prefs.end();
  srv.send_P(200, "text/html", OK_HTML);
  gDone = true;
}

// ---------------------------------------------------------------
// CAPTIVE PORTAL
// ---------------------------------------------------------------
bool runPortal(bool first) {
  bool needDash = (!cfgDashOK && strlen(cfgSSID) > 0);

  if(first || !strlen(cfgSSID)) {
    WiFi.mode(WIFI_AP); gInet = false;
    Serial.println("[PORT] AP-only");
  } else {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(cfgSSID, cfgPASS);
    int r = 0; while(WiFi.status() != WL_CONNECTED && r < 30) { delay(500); r++; }
    gInet = (WiFi.status() == WL_CONNECTED);
    Serial.println("[PORT] AP+STA inet=" + String(gInet?"Y":"N"));
  }

  WiFi.softAP(AP_NAME, ""); delay(500);
  IPAddress ip(192,168,4,1);
  WiFi.softAPConfig(ip, ip, IPAddress(255,255,255,0));
  dns.start(DNS_P, "*", ip);

  srv.on("/",         HTTP_GET,  hRoot);
  srv.on("/saveWifi", HTTP_POST, hSaveW);
  srv.on("/saveDash", HTTP_POST, hSaveD);
  srv.on("/val",      HTTP_GET,  hVal);
  srv.on("/rst",      HTTP_POST, hReset);
  // Captive portal hooks
  const char* hooks[] = {"/hotspot-detect.html","/generate_204","/gen_204",
    "/ncsi.txt","/connecttest.txt","/redirect","/success.txt",
    "/library/test/success.html"};
  for(auto& p : hooks) srv.on(p, HTTP_GET, hRoot);
  srv.onNotFound(hRedir);
  srv.begin();

  if(first)        splashFirstBoot();
  else if(needDash) splashDashSetup();
  else              splashConfig();

  unsigned long t0 = millis(); gDone = false;
  while(millis() - t0 < PORTAL_TIMEOUT) {
    dns.processNextRequest(); srv.handleClient();
    if(gDone) { delay(2500); break; }
  }
  srv.stop(); dns.stop();
  WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
  return gDone;
}

// ---------------------------------------------------------------
// YAHOO FINANCE (PSRAM-backed JSON)
// ---------------------------------------------------------------
void fetchYahoo() {
  WiFiClientSecure cl; cl.setInsecure();
  HTTPClient http;
  String url = "https://query1.finance.yahoo.com/v8/finance/chart/"
               + String(sym()) + "?range=" + yRange[rView]
               + "&interval=" + yIntvl[rView];
  Serial.println("[YH] " + url);

  if(!http.begin(cl, url)) { gStatus = "Conn"; return; }
  http.setTimeout(12000);
  http.addHeader("User-Agent", "Mozilla/5.0");
  int code = http.GET();
  Serial.printf("[YH] HTTP:%d\n", code);
  if(code != 200) { gStatus = "H" + String(code); http.end(); return; }

  String pay = http.getString();
  http.end();
  Serial.printf("[YH] len=%d\n", pay.length());

  // Use PSRAM if available, else heap
  DynamicJsonDocument* doc;
  size_t docSize = max((size_t)pay.length() * 2, (size_t)32768);
  if(docSize > 65536) docSize = 65536; // cap

  #if defined(BOARD_HAS_PSRAM) || defined(ESP_PSRAM)
    doc = new DynamicJsonDocument(docSize);
    Serial.printf("[YH] PSRAM doc %d bytes\n", docSize);
  #else
    if(docSize > 48000) docSize = 48000; // heap safety
    doc = new DynamicJsonDocument(docSize);
    Serial.printf("[YH] Heap doc %d bytes\n", docSize);
  #endif

  DeserializationError err = deserializeJson(*doc, pay);
  if(err) {
    Serial.printf("[YH] JSON err: %s\n", err.c_str());
    gStatus = "JSON";
    delete doc; return;
  }

  JsonObject chart = (*doc)["chart"];
  if(chart.isNull() || !chart["error"].isNull()) {
    gStatus = "API";
    delete doc; return;
  }
  JsonObject res = chart["result"][0];
  if(res.isNull()) { gStatus = "NoRes"; delete doc; return; }

  JsonObject meta = res["meta"];
  float price = meta["regularMarketPrice"].as<float>();
  float prev  = meta["chartPreviousClose"].as<float>();

  if(meta.containsKey("currency"))
    strlcpy(rCur, meta["currency"].as<const char*>(), sizeof(rCur));
  if(meta.containsKey("exchangeName"))
    detectTZ(meta["exchangeName"].as<const char*>());

  // Trading period
  if(meta.containsKey("currentTradingPeriod")) {
    JsonObject reg = meta["currentTradingPeriod"]["regular"];
    if(!reg.isNull()) {
      rTrdStart = (time_t)reg["start"].as<unsigned long>();
      rTrdEnd   = (time_t)reg["end"].as<unsigned long>();
      Serial.printf("[YH] trd: %lu->%lu\n", (unsigned long)rTrdStart, (unsigned long)rTrdEnd);
    }
  }
  rMktOpen = isTradingNow();

  if(price > 0.01f) {
    rPrice = price; rPrev = prev;
    rPct = (prev > 0.01f) ? ((price - prev) / prev) * 100.0f : 0;
    gStatus = "OK";
  } else if(prev > 0.01f) {
    rPrice = prev; rPct = 0; rPrev = prev;
    gStatus = "OK(cl)";
  } else {
    gStatus = "Zero";
  }

  if(meta.containsKey("regularMarketDayHigh")) {
    float v = meta["regularMarketDayHigh"].as<float>(); if(v > 0.01f) rHigh = v; }
  if(meta.containsKey("regularMarketDayLow")) {
    float v = meta["regularMarketDayLow"].as<float>();  if(v > 0.01f) rLow = v; }
  if(meta.containsKey("regularMarketOpen")) {
    float v = meta["regularMarketOpen"].as<float>();    if(v > 0.01f) rOpen = v; }
  if(rOpen < 0.01f && rPrev > 0.01f) rOpen = rPrev;
  if(meta.containsKey("regularMarketVolume"))
    rVol = meta["regularMarketVolume"].as<long>();

  // Chart data
  gBarCnt = 0;
  if(res.containsKey("timestamp")) {
    JsonArray ts = res["timestamp"];
    JsonArray cl = res["indicators"]["quote"][0]["close"];
    int tot = ts.size(), si = 0;
    if(tot > MAX_BARS) si = tot - MAX_BARS;
    for(int i = si; i < tot; i++) {
      if(cl[i].isNull()) continue;
      float c = cl[i].as<float>();
      if(c > 0.001f) { // allow penny stocks
        gPrices[gBarCnt] = c;
        gTimes[gBarCnt]  = (time_t)ts[i].as<unsigned long>();
        gBarCnt++;
        if(gBarCnt >= MAX_BARS) break;
      }
    }
  }
  Serial.printf("[YH] %d bars\n", gBarCnt);
  delete doc;
}

// ---------------------------------------------------------------
// UPDATE
// ---------------------------------------------------------------
void doUpdate() {
  WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  WiFi.begin(cfgSSID, cfgPASS);
  int r = 0; while(WiFi.status() != WL_CONNECTED && r < 40) { delay(500); r++; }
  if(WiFi.status() != WL_CONNECTED) {
    gStatus = "WiFi"; rLastOK = false;
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF); return;
  }
  configTzTime(rTZ, "pool.ntp.org");
  struct tm ti; int a = 0;
  while(!getLocalTime(&ti) && a < 10) { delay(500); a++; }
  if(a < 10) time(&rUpdEpoch);
  fetchYahoo();
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);

  // Track connection success for state-aware UI
  if(gStatus == "OK" || gStatus == "OK(cl)") {
    rLastOK = true;
    if(!cfgEverOK) {
      prefs.begin("cfg", false);
      prefs.putBool("everOK", true);
      prefs.end();
      cfgEverOK = true;
    }
  } else {
    rLastOK = false;
  }
}

// ---------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // Build unique AP SSID from hardware eFuse MAC (last 2 bytes = 4 hex chars)
  uint64_t mac = ESP.getEfuseMac();
  snprintf(AP_NAME, sizeof(AP_NAME), "StockDash-%04X",
           (uint16_t)(mac >> 32));

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(300);

  nvsLoad();

  esp_sleep_wakeup_cause_t wr = esp_sleep_get_wakeup_cause();
  bool btnW = (wr == ESP_SLEEP_WAKEUP_EXT0);
  bool cold = (wr == ESP_SLEEP_WAKEUP_UNDEFINED);

  Serial.println("\n===== Stock Dashboard v1.21 =====");
  Serial.printf("W:%d V:%d T:%d/%d(%s) B:%d%%\n",
    wr, rView, rTkrIdx, cfgNTkr, sym(), batPct());

  // Cold boot + button held: config portal (non-destructive)
  if(cold) {
    // Wait 200ms then check if button held
    delay(200);
    bool held = (digitalRead(BUTTON_PIN) == LOW);
    if(held) {
      // Wait up to 1s to confirm intentional hold
      unsigned long hs = millis();
      while(millis() - hs < 800 && digitalRead(BUTTON_PIN) == LOW) delay(10);
      held = (digitalRead(BUTTON_PIN) == LOW);
    }
    if(held) {
      Serial.println("Reset+hold -> portal");
      bool first = (strlen(cfgSSID) == 0);
      if(runPortal(first)) ESP.restart();
      if(!strlen(cfgSSID)) {
        esp_sleep_enable_timer_wakeup(30ULL*60*uS_TO_S);
        esp_deep_sleep_start(); return;
      }
    }
    rView = VIEW_1DAY; rTkrIdx = 0;
    if(!(cfgViews & (1<<rView))) rView = nextView(rView);
  }

  // Button wake with debounce
  if(btnW) {
    delay(BTN_DEBOUNCE); // let pin settle after ext0 wake

    // Now check: is button still held?
    if(digitalRead(BUTTON_PIN) == LOW) {
      // User is actively pressing — measure duration
      unsigned long ps = millis();
      bool longH = false;
      while(digitalRead(BUTTON_PIN) == LOW) {
        if(millis() - ps >= LONG_HOLD_MS) { longH = true; break; }
        delay(10);
      }
      if(longH && cfgNTkr > 1) {
        rTkrIdx = (rTkrIdx + 1) % cfgNTkr;
        rView = VIEW_1DAY;
        if(!(cfgViews & (1<<rView))) rView = nextView(rView);
        rPrice=0;rPct=0;rPrev=0;rHigh=0;rLow=0;rOpen=0;rVol=0;
        gBarCnt=0;rTrdStart=0;rTrdEnd=0;
        Serial.println("Long -> " + String(sym()));
      } else if(!longH) {
        rView = nextView(rView);
        Serial.println("Short -> " + String(vLabel[rView]));
      }
    } else {
      // Button already released — treat as short press
      rView = nextView(rView);
      Serial.println("Quick -> " + String(vLabel[rView]));
    }
  }

  // No WiFi -> first boot portal
  if(!strlen(cfgSSID)) {
    Serial.println("No WiFi -> portal");
    if(runPortal(true)) ESP.restart();
    esp_sleep_enable_timer_wakeup(30ULL*60*uS_TO_S);
    esp_deep_sleep_start(); return;
  }

  // WiFi OK but no dashboard config -> setup portal
  if(!cfgDashOK) {
    Serial.println("No dash -> portal");
    if(runPortal(false)) ESP.restart();
    esp_sleep_enable_timer_wakeup(30ULL*60*uS_TO_S);
    esp_deep_sleep_start(); return;
  }

  // No tickers configured
  if(cfgNTkr == 0) {
    Serial.println("No tickers -> portal");
    if(runPortal(false)) ESP.restart();
    esp_sleep_enable_timer_wakeup(30ULL*60*uS_TO_S);
    esp_deep_sleep_start(); return;
  }

  // Normal operation
  doUpdate();
  uint64_t sl = sleepSecs();
  Serial.printf("[SLEEP] %lu sec\n", (unsigned long)sl);
  drawScreen();
  esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 0);
  esp_sleep_enable_timer_wakeup(sl * uS_TO_S);
  esp_deep_sleep_start();
}

void loop() {}
