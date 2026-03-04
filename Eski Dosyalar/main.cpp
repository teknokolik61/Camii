/*
  cami.ino - ESP32 Röle + Ezan Vakti + Telegram + Buton (ILCE_ID=9206 Çankaya)

  FIX (/guncelle reset / stack canary):
  - /guncelle komutu Telegram handler içinde fetch/parse yapmaz.
  - Sadece pending flag koyar; indirme/parsing loop içindeki worker'da yapılır.
  - HTTP TLS client local stack yerine global kullanılır.

  /dinigünler FORMAT + SIRALAMA (GÜNCEL):
  - Tek satır: "✅ 🟡 YAKLASAN - Ramazan Günü 23 Miladi: 13.03.2026"
  - En yakın -> en uzak (AKTIF en üstte, sonra YAKLASAN en yakın tarihten)
  - Sadece AKTIF + YAKLASAN listelenir. Hiç yoksa: "Yakın tarihte dini gün yok"
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <UniversalTelegramBot.h>
#include <esp_system.h>
#include "secrets.h"

// ===== Admin NVS reset (gerekirse 1 yap, bir kere calistir, sonra 0'a cek) =====
#define FORCE_RESET_ADMINS 0

// =====================
// WiFi
// =====================
static const char* WIFI_SSID = SECRET_WIFI_SSID;
static const char* WIFI_PASS = SECRET_WIFI_PASS;

// =====================
// Telegram
// =====================
static const char* BOT_TOKEN = SECRET_BOT_TOKEN;
static const char* CHAT_ID   = SECRET_CHAT_ID;
static const int64_t OWNER_ADMIN_ID = 1253195249; // /myid ile görürsün

// =====================
// ÖZEL GÜNLER ENABLE (0=kapalı, 1=açık)
// =====================
#define EN_1_RECEB_UC_AYLAR_BASLANGIC   1
#define EN_2_RECEB_REGAIB_KANDILI      1
#define EN_26_RECEB_MIRAC_KANDILI      1
#define EN_14_SABAN_BERAT_KANDILI      1
#define EN_26_RAMAZAN_KADIR_GECESI     1
#define EN_1_SEVVAL_RB_1               1
#define EN_2_SEVVAL_RB_2               1
#define EN_3_SEVVAL_RB_3               1
#define EN_9_ZILHICCE_AREFE            1
#define EN_10_ZILHICCE_KB_1            1
#define EN_11_ZILHICCE_KB_2            1
#define EN_12_ZILHICCE_KB_3            1
#define EN_13_ZILHICCE_KB_4            1
#define EN_11_REBIULEVVEL_MEVLID       1
#define EN_5_RECEB_REGAIB_KANDILI      1

// Ramazan ayındaki tüm günleri dini gün gibi dahil et
#define EN_RAMAZAN_TUM_GUNLER           1

// Telegram poll + spam koruma
static const uint32_t TG_POLL_MS  = 1500;
static const uint32_t TG_DEDUP_MS = 25000;

// =====================
// Röle
// =====================
static const int  RELAY_PIN = 23;
static const bool RELAY_ACTIVE_LOW = true;

// =====================
// Buton (sadece ON)
// =====================
static const int BUTTON_PIN = 27;              // GPIO27 <-> GND
static const uint32_t BTN_DEBOUNCE_MS = 40;

// =====================
// Ofsetler
// =====================
static const int ON_OFFSET_SEC  = 60; // Akşam +1 dk
static const int OFF_OFFSET_SEC = 60; // İmsak -1 dk

// =====================
// Haftalık güncelleme: Pazartesi 03:05
// =====================
static const int UPDATE_DOW  = 1;  // 0=Pazar,1=Pzt,2=Sal,3=Çar,4=Per,5=Cum,6=Cmt
static const int UPDATE_HOUR = 3;
static const int UPDATE_MIN  = 5;

// =====================
// Yaklaşan dini gün bildirimi
// =====================
static const int SP_NOTIFY_BEFORE_SEC = 3600; // 1 saat önce

// =====================
// API
// =====================
static const char* EZAN_API = "https://ezanvakti.emushaf.net";
static const uint32_t ILCE_ID_FIXED = 9206;

// =====================
// NVS vakit verisi
// =====================
static const uint8_t TIMES_VER = 3;

struct __attribute__((packed)) DayTimes {
  uint32_t ymd;            // YYYYMMDD
  uint16_t imsakMin;       // 0..1439
  uint16_t aksamMin;       // 0..1439
  char     hicriUzun[28];  // örn: "26 Ramazan 1447"
};

static const int MAX_DAYS = 45;
static DayTimes  g_days[MAX_DAYS];
static uint16_t  g_dayCount = 0;

// =====================
// Admin listesi (NVS)
// =====================
static const int MAX_ADMINS = 20;
static int64_t  g_adminIds[MAX_ADMINS];
static uint8_t  g_adminCount = 0;

// =====================
// Özel gün tanımı
// =====================
struct SpecialDef {
  uint8_t   day;
  const char* monthKey;  // receb/saban/ramazan/sevval/zilhicce/rebiulevvel
  const char* name;
  uint8_t   enabled;
};

static const SpecialDef g_specials[] = {
  {  1, "receb",       "Üç Ayların Başlangıcı",          (uint8_t)EN_1_RECEB_UC_AYLAR_BASLANGIC },
  {  2, "receb",       "Regaib Kandili (2 Receb)",       (uint8_t)EN_2_RECEB_REGAIB_KANDILI },
  { 26, "receb",       "Mirac Kandili",                  (uint8_t)EN_26_RECEB_MIRAC_KANDILI },
  { 14, "saban",       "Berat Kandili",                  (uint8_t)EN_14_SABAN_BERAT_KANDILI },
  { 26, "ramazan",     "Kadir Gecesi",                   (uint8_t)EN_26_RAMAZAN_KADIR_GECESI },
  {  1, "sevval",      "Ramazan Bayramı 1. Gün",         (uint8_t)EN_1_SEVVAL_RB_1 },
  {  2, "sevval",      "Ramazan Bayramı 2. Gün",         (uint8_t)EN_2_SEVVAL_RB_2 },
  {  3, "sevval",      "Ramazan Bayramı 3. Gün",         (uint8_t)EN_3_SEVVAL_RB_3 },
  {  9, "zilhicce",    "Kurban Bayramı Arefe Günü",      (uint8_t)EN_9_ZILHICCE_AREFE },
  { 10, "zilhicce",    "Kurban Bayramı 1. Gün",          (uint8_t)EN_10_ZILHICCE_KB_1 },
  { 11, "zilhicce",    "Kurban Bayramı 2. Gün",          (uint8_t)EN_11_ZILHICCE_KB_2 },
  { 12, "zilhicce",    "Kurban Bayramı 3. Gün",          (uint8_t)EN_12_ZILHICCE_KB_3 },
  { 13, "zilhicce",    "Kurban Bayramı 4. Gün",          (uint8_t)EN_13_ZILHICCE_KB_4 },
  { 11, "rebiulevvel", "Mevlid Kandili",                 (uint8_t)EN_11_REBIULEVVEL_MEVLID },
  {  5, "receb",       "Regaib Kandili (5 Receb)",       (uint8_t)EN_5_RECEB_REGAIB_KANDILI },
};
static const uint8_t SPECIAL_COUNT = sizeof(g_specials)/sizeof(g_specials[0]);

// =====================
// Global state
// =====================
Preferences prefs;

WiFiClientSecure tgClient;
UniversalTelegramBot bot(BOT_TOKEN, tgClient);

// HTTP için global TLS client
static WiFiClientSecure g_httpClient;

static bool g_relayState = false;
static bool g_manualOnLatched = false;

// scheduled pencere aktifken /off verilirse, pencere bitene kadar OFF override
static time_t g_manualOffUntilTs = 0;

// Otomatik Perşembe->Cuma penceresi
static time_t g_thuOnTs  = 0;
static time_t g_thuOffTs = 0;

// En yakın / aktif dini pencere (özel gün + Ramazan günü)
static time_t g_spOnTs  = 0;
static time_t g_spOffTs = 0;
static String g_spName  = "";
static String g_spHicri = "";
static uint32_t g_spYmd = 0;

// özel gün yaklaşan bildirim (her pencere 1 kez)
static time_t g_lastSpNotifyOnTs = 0;

// ZORUNLU OFF: bir sonraki İmsak - 60 sn
static time_t g_nextImsakOffTs = 0;
static time_t g_lastImsakOffEventTs = 0;

// (opsiyonel) zorunlu OFF sonrası kısa ON engeli
static time_t g_blockOnUntilTs = 0;

// Telegram polling + dedup
static uint32_t g_lastTgPollMs = 0;
static String   g_lastTgMsg = "";
static uint32_t g_lastTgMsgMs = 0;

// WiFi durum takip
static wl_status_t g_lastWiFi = WL_IDLE_STATUS;
static uint32_t g_lastWiFiTryMs = 0;

// ===== /guncelle güvenli worker =====
static bool     g_updatePending = false;
static bool     g_updateInProgress = false;
static uint32_t g_updateCooldownUntilMs = 0;
static String   g_updateRequesterWho = "";
static int64_t  g_updateRequesterId  = 0;

// NVS: Telegram last update_id
static const char* NVS_KEY_TG_LAST = "tgLast"; // int64

// =====================
// Zaman geçerli mi?
// =====================
static bool isTimeValid() { return time(nullptr) >= 1700000000; }

static void formatDateTime(time_t ts, char* out, size_t outSz) {
  if (ts == 0) { snprintf(out, outSz, "-"); return; }
  tm t{}; localtime_r(&ts, &t);
  strftime(out, outSz, "%Y-%m-%d %H:%M:%S", &t);
}

static String nowStamp() {
  if (!isTimeValid()) return "NO_TIME";
  time_t now = time(nullptr);
  char buf[32]; formatDateTime(now, buf, sizeof(buf));
  return String(buf);
}

static void ymdToDdMmYyyy(uint32_t ymd, char* out, size_t outSz) {
  int y = (int)(ymd / 10000u);
  int m = (int)((ymd / 100u) % 100u);
  int d = (int)(ymd % 100u);
  snprintf(out, outSz, "%02d.%02d.%04d", d, m, y);
}

// =====================
// Telegram send (dedup)
// =====================
static void tgSend(const String& msg, bool force = false) {
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t nowMs = millis();
  if (!force) {
    if (msg == g_lastTgMsg && (nowMs - g_lastTgMsgMs) < TG_DEDUP_MS) return;
  }

  tgClient.setInsecure();
  bool ok = bot.sendMessage(CHAT_ID, msg, "");
  if (ok) {
    g_lastTgMsg = msg;
    g_lastTgMsgMs = nowMs;
  }
}

static void logSerialAndTg(const String& msg, bool notifyTg = true, bool forceTg = false) {
  Serial.print("["); Serial.print(nowStamp()); Serial.print("] ");
  Serial.println(msg);
  if (notifyTg) tgSend("[" + nowStamp() + "] " + msg, forceTg);
}

// =====================
// Röle kontrol
// =====================
static void relayWrite(bool on) {
  g_relayState = on;
  if (RELAY_ACTIVE_LOW) digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  else                  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
}

// =====================
// Tarih/saat yardımcı
// =====================
static uint32_t ymdFromTm(const tm& t) {
  return (uint32_t)(t.tm_year + 1900) * 10000u + (uint32_t)(t.tm_mon + 1) * 100u + (uint32_t)t.tm_mday;
}

static uint32_t ymdToday() {
  time_t now = time(nullptr);
  tm t{}; localtime_r(&now, &t);
  return ymdFromTm(t);
}

static uint16_t hhmmToMin(const String& hhmm) {
  if (hhmm.length() < 4) return 0;
  int hh = hhmm.substring(0, 2).toInt();
  int mm = hhmm.substring(3, 5).toInt();
  if (hh < 0) hh = 0; if (hh > 23) hh = 23;
  if (mm < 0) mm = 0; if (mm > 59) mm = 59;
  return (uint16_t)(hh * 60 + mm);
}

static uint32_t parseDateToYmd(const String& s) {
  if (s.length() >= 10 && s.charAt(4) == '-' && s.charAt(7) == '-') {
    int y = s.substring(0, 4).toInt();
    int m = s.substring(5, 7).toInt();
    int d = s.substring(8, 10).toInt();
    return (uint32_t)y * 10000u + (uint32_t)m * 100u + (uint32_t)d;
  }
  if (s.length() >= 10 && s.charAt(2) == '.' && s.charAt(5) == '.') {
    int d = s.substring(0, 2).toInt();
    int m = s.substring(3, 5).toInt();
    int y = s.substring(6, 10).toInt();
    return (uint32_t)y * 10000u + (uint32_t)m * 100u + (uint32_t)d;
  }
  return 0;
}

static time_t epochFromYmdAndMin(uint32_t ymd, uint16_t minutesFromMidnight) {
  int year  = (int)(ymd / 10000u);
  int month = (int)((ymd / 100u) % 100u);
  int day   = (int)(ymd % 100u);

  tm t{};
  t.tm_year = year - 1900;
  t.tm_mon  = month - 1;
  t.tm_mday = day;
  t.tm_hour = minutesFromMidnight / 60;
  t.tm_min  = minutesFromMidnight % 60;
  t.tm_sec  = 0;
  return mktime(&t);
}

static uint32_t addDaysYmd(uint32_t ymd, int deltaDays) {
  int year  = (int)(ymd / 10000u);
  int month = (int)((ymd / 100u) % 100u);
  int day   = (int)(ymd % 100u);

  tm t{};
  t.tm_year = year - 1900;
  t.tm_mon  = month - 1;
  t.tm_mday = day;
  t.tm_hour = 12; t.tm_min = 0; t.tm_sec = 0;
  time_t e = mktime(&t) + (time_t)deltaDays * 86400;
  tm out{}; localtime_r(&e, &out);
  return ymdFromTm(out);
}

// =====================
// Cache yardımcı
// =====================
static int findIdx(uint32_t ymd) {
  for (uint16_t i = 0; i < g_dayCount; i++) if (g_days[i].ymd == ymd) return (int)i;
  return -1;
}
static bool hasYmd(uint32_t ymd) { return findIdx(ymd) >= 0; }

static void sortDaysByYmd() {
  if (g_dayCount < 2) return;
  for (uint16_t i = 0; i < g_dayCount - 1; i++) {
    for (uint16_t j = i + 1; j < g_dayCount; j++) {
      if (g_days[j].ymd < g_days[i].ymd) {
        DayTimes tmp = g_days[i];
        g_days[i] = g_days[j];
        g_days[j] = tmp;
      }
    }
  }
}

// =====================
// Hicri parse + normalize month
// =====================
static String normMonthKey(const String& in) {
  String s = in;
  s.trim();

  s.replace("Ç", "C"); s.replace("ç", "c");
  s.replace("Ğ", "G"); s.replace("ğ", "g");
  s.replace("İ", "I"); s.replace("ı", "i");
  s.replace("Ö", "O"); s.replace("ö", "o");
  s.replace("Ş", "S"); s.replace("ş", "s");
  s.replace("Ü", "U"); s.replace("ü", "u");

  s.toLowerCase();
  if (s == "recep") s = "receb";
  if (s == "rebiyulevvel") s = "rebiulevvel";
  return s;
}

static bool parseHicri(const char* s, int& dayOut, String& monthOut, int& yearOut) {
  if (!s || !*s) return false;
  String h(s); h.trim();

  int firstSp = h.indexOf(' ');
  int lastSp  = h.lastIndexOf(' ');
  if (firstSp < 0 || lastSp <= firstSp) return false;

  dayOut = h.substring(0, firstSp).toInt();
  monthOut = h.substring(firstSp + 1, lastSp);
  monthOut.trim();
  yearOut = h.substring(lastSp + 1).toInt();

  if (dayOut <= 0 || dayOut > 30) return false;
  if (monthOut.length() == 0) return false;
  if (yearOut <= 0) yearOut = 0;
  return true;
}

// =====================
// Admin NVS
// =====================
static void saveAdminsToNvs() {
  prefs.putUChar("admCnt", g_adminCount);
  prefs.putBytes("admBlob", g_adminIds, (size_t)g_adminCount * sizeof(int64_t));
}

static void resetAdminsToOwner() {
  prefs.remove("admCnt");
  prefs.remove("admBlob");
  g_adminCount = 0;
  g_adminIds[g_adminCount++] = OWNER_ADMIN_ID;
  saveAdminsToNvs();
}

static void loadAdminsFromNvs() {
  g_adminCount = prefs.getUChar("admCnt", 0);
  if (g_adminCount > MAX_ADMINS) g_adminCount = 0;

  if (g_adminCount > 0) {
    size_t need = (size_t)g_adminCount * sizeof(int64_t);
    size_t got  = prefs.getBytes("admBlob", g_adminIds, need);
    if (got != need) g_adminCount = 0;
  }

  if (g_adminCount == 0) {
    g_adminIds[0] = OWNER_ADMIN_ID;
    g_adminCount = 1;
    saveAdminsToNvs();
  }
}

static bool isAdmin(int64_t id) {
  for (uint8_t i = 0; i < g_adminCount; i++) if (g_adminIds[i] == id) return true;
  return false;
}

static void ensureOwnerIsAdmin() {
  if (!isAdmin(OWNER_ADMIN_ID)) {
    if (g_adminCount < MAX_ADMINS) {
      g_adminIds[g_adminCount++] = OWNER_ADMIN_ID;
      saveAdminsToNvs();
    }
  }
}

static bool addAdmin(int64_t id) {
  if (id <= 0) return false;
  if (isAdmin(id)) return true;
  if (g_adminCount >= MAX_ADMINS) return false;
  g_adminIds[g_adminCount++] = id;
  saveAdminsToNvs();
  return true;
}

static bool delAdmin(int64_t id) {
  if (id == OWNER_ADMIN_ID) return false;
  if (g_adminCount <= 1) return false;

  int idx = -1;
  for (uint8_t i = 0; i < g_adminCount; i++) if (g_adminIds[i] == id) { idx = i; break; }
  if (idx < 0) return false;

  for (uint8_t i = idx; i + 1 < g_adminCount; i++) g_adminIds[i] = g_adminIds[i + 1];
  g_adminCount--;
  saveAdminsToNvs();
  return true;
}

// =====================
// NVS vakit verisi
// =====================
static void loadTimesFromNvs() {
  uint8_t ver = prefs.getUChar("timesVer", 0);
  if (ver != TIMES_VER) {
    prefs.remove("dayCount");
    prefs.remove("daysBlob");
    prefs.putUChar("timesVer", TIMES_VER);
    g_dayCount = 0;
    return;
  }

  g_dayCount = prefs.getUShort("dayCount", 0);
  if (g_dayCount > MAX_DAYS) g_dayCount = 0;
  if (g_dayCount == 0) return;

  size_t need = (size_t)g_dayCount * sizeof(DayTimes);
  size_t got  = prefs.getBytes("daysBlob", g_days, need);
  if (got != need) g_dayCount = 0;
}

static void saveTimesToNvs() {
  prefs.putUChar("timesVer", TIMES_VER);
  prefs.putUShort("dayCount", g_dayCount);
  prefs.putBytes("daysBlob", g_days, (size_t)g_dayCount * sizeof(DayTimes));
  if (isTimeValid()) prefs.putUInt("lastUpdYmd", ymdToday());
}

// =====================
// Telegram last_id (NVS)
// =====================
static void tgLoadLastIdFromNvs() {
  int64_t last = prefs.getLong64(NVS_KEY_TG_LAST, 0);
  if (last > 0) {
    bot.last_message_received = (long)last;
    Serial.print("[BOOT] TG last_id loaded: ");
    Serial.println((long long)last);
  }
}
static void tgSaveLastIdToNvsIfNew() {
  int64_t last = (int64_t)bot.last_message_received;
  if (last <= 0) return;
  int64_t cur = prefs.getLong64(NVS_KEY_TG_LAST, 0);
  if (last > cur) prefs.putLong64(NVS_KEY_TG_LAST, last);
}

// =====================
// HTTP: tüm payload al (global TLS client)
// =====================
static bool httpGetPayload(const String& url, String& payloadOut, int& httpCodeOut) {
  if (WiFi.status() != WL_CONNECTED) return false;

  g_httpClient.setInsecure();

  HTTPClient http;
  http.setTimeout(25000);
  http.setReuse(false);
  http.useHTTP10(true);

  if (!http.begin(g_httpClient, url)) return false;

  httpCodeOut = http.GET();
  if (httpCodeOut != 200) { http.end(); return false; }

  payloadOut = http.getString();
  http.end();

  yield();
  return payloadOut.length() > 0;
}

// =====================
// 30 günlük vakit indir + NVS'e kaydet
// =====================
static bool fetchAndStoreMonthly(bool notifyTg = true) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(EZAN_API) + "/vakitler/" + String(ILCE_ID_FIXED);

  int code = 0;
  String payload;
  if (!httpGetPayload(url, payload, code)) {
    logSerialAndTg("❌ Vakit cekme FAIL (HTTP) code=" + String(code), notifyTg, true);
    return false;
  }

  StaticJsonDocument<384> filter;
  filter[0]["MiladiTarihUzunIso8601"] = true;
  filter[0]["MiladiTarihKisa"]        = true;
  filter[0]["MiladiTarihKisaIso8601"] = true;
  filter[0]["Imsak"]                  = true;
  filter[0]["Aksam"]                  = true;
  filter[0]["HicriTarihUzun"]         = true;

  DynamicJsonDocument doc(64 * 1024);
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    logSerialAndTg(String("❌ JSON parse error: ") + err.c_str(), notifyTg, true);
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  uint16_t n = 0;

  for (JsonObject o : arr) {
    if (n >= MAX_DAYS) break;

    String s = String((const char*)(o["MiladiTarihUzunIso8601"] | ""));
    if (s.length() == 0) s = String((const char*)(o["MiladiTarihKisa"] | ""));
    if (s.length() == 0) s = String((const char*)(o["MiladiTarihKisaIso8601"] | ""));

    uint32_t ymd = parseDateToYmd(s);
    if (ymd == 0) continue;

    String imsak = String((const char*)(o["Imsak"] | ""));
    String aksam = String((const char*)(o["Aksam"] | ""));
    if (imsak.length() < 4 || aksam.length() < 4) continue;

    String hicri = String((const char*)(o["HicriTarihUzun"] | ""));
    hicri.trim();

    g_days[n].ymd      = ymd;
    g_days[n].imsakMin = hhmmToMin(imsak);
    g_days[n].aksamMin = hhmmToMin(aksam);
    memset(g_days[n].hicriUzun, 0, sizeof(g_days[n].hicriUzun));
    if (hicri.length() > 0) strncpy(g_days[n].hicriUzun, hicri.c_str(), sizeof(g_days[n].hicriUzun) - 1);

    n++;
    if ((n & 0x03) == 0) yield();
  }

  if (n < 10) {
    logSerialAndTg("❌ Vakitler az geldi! n=" + String(n), notifyTg, true);
    return false;
  }

  g_dayCount = n;
  sortDaysByYmd();
  saveTimesToNvs();

  logSerialAndTg("✅ Vakitler guncellendi. gunSayisi=" + String(g_dayCount), notifyTg, true);
  return true;
}

// =====================
// Otomatik Perşembe->Cuma penceresi
// =====================
static bool computeThuFriWindowForNow(time_t now, time_t& onTs, time_t& offTs) {
  if (!isTimeValid() || g_dayCount == 0) return false;

  tm nt{}; localtime_r(&now, &nt);
  const int THU = 4;
  int deltaToLastThu = (nt.tm_wday - THU + 7) % 7;

  tm base = nt;
  base.tm_hour = 12; base.tm_min = 0; base.tm_sec = 0;
  time_t baseNoon = mktime(&base);
  time_t thuNoon  = baseNoon - (time_t)deltaToLastThu * 86400;

  tm thuTm{}; localtime_r(&thuNoon, &thuTm);

  uint32_t ymdThu = ymdFromTm(thuTm);
  uint32_t ymdFri = addDaysYmd(ymdThu, +1);

  auto build = [&](uint32_t yThu, uint32_t yFri, time_t& a, time_t& b)->bool {
    int iThu = findIdx(yThu);
    int iFri = findIdx(yFri);
    if (iThu < 0 || iFri < 0) return false;

    a = epochFromYmdAndMin(yThu, g_days[iThu].aksamMin) + ON_OFFSET_SEC;
    b = epochFromYmdAndMin(yFri, g_days[iFri].imsakMin) - OFF_OFFSET_SEC;
    return (b > a);
  };

  if (!build(ymdThu, ymdFri, onTs, offTs)) return false;

  if (now >= offTs) {
    uint32_t nextThu = addDaysYmd(ymdThu, +7);
    uint32_t nextFri = addDaysYmd(nextThu, +1);
    if (!build(nextThu, nextFri, onTs, offTs)) return false;
  }
  return true;
}

// =====================
// ZORUNLU OFF: bir sonraki imsak-1dk
// =====================
static bool computeNextImsakOff(time_t now, time_t& nextOffTs) {
  if (!isTimeValid() || g_dayCount == 0) return false;

  tm t{}; localtime_r(&now, &t);
  uint32_t today = ymdFromTm(t);

  int iToday = findIdx(today);
  if (iToday < 0) return false;

  time_t offToday = epochFromYmdAndMin(today, g_days[iToday].imsakMin) - OFF_OFFSET_SEC;
  if (now < offToday) { nextOffTs = offToday; return true; }

  uint32_t tomorrow = addDaysYmd(today, +1);
  int iTom = findIdx(tomorrow);
  if (iTom < 0) return false;

  nextOffTs = epochFromYmdAndMin(tomorrow, g_days[iTom].imsakMin) - OFF_OFFSET_SEC;
  return true;
}

static void getScheduleState(time_t now, bool& thuOn, bool& spOn, time_t& schedOffMax) {
  thuOn = (g_thuOnTs && g_thuOffTs && now >= g_thuOnTs && now < g_thuOffTs);
  spOn  = (g_spOnTs  && g_spOffTs  && now >= g_spOnTs  && now < g_spOffTs);
  schedOffMax = 0;
  if (thuOn) schedOffMax = g_thuOffTs;
  if (spOn && g_spOffTs > schedOffMax) schedOffMax = g_spOffTs;
}

static void enforceImsakOffIfDue() {
  if (!isTimeValid() || g_nextImsakOffTs == 0) return;

  time_t now = time(nullptr);

  const time_t STALE_SEC = 600;
  const time_t FIRE_WIN  = 300;

  if (g_nextImsakOffTs < (now - STALE_SEC)) {
    time_t noff = 0;
    if (computeNextImsakOff(now, noff)) g_nextImsakOffTs = noff;
  }

  if (g_nextImsakOffTs == 0) return;
  if (g_nextImsakOffTs < (now - STALE_SEC)) return;

  if (now > (g_nextImsakOffTs + FIRE_WIN)) {
    time_t noff = 0;
    if (computeNextImsakOff(now, noff)) g_nextImsakOffTs = noff;
    return;
  }

  if (now >= g_nextImsakOffTs && g_lastImsakOffEventTs != g_nextImsakOffTs) {
    g_lastImsakOffEventTs = g_nextImsakOffTs;

    bool wasOn = g_relayState;
    relayWrite(false);
    g_manualOnLatched = false;

    g_blockOnUntilTs = now + 120;

    char ts[32];
    formatDateTime(g_nextImsakOffTs, ts, sizeof(ts));
    logSerialAndTg(String("⛔ ZORUNLU OFF (Imsak-1dk): ") + ts + "  (manuel iptal)", true, true);
    if (wasOn) logSerialAndTg("🔕 ROLE: OFF (Zorunlu Imsak-1dk)", true, true);

    time_t newOff = 0;
    if (computeNextImsakOff(now + 2, newOff)) g_nextImsakOffTs = newOff;
    else g_nextImsakOffTs = 0;
  }
}

// =====================
// WiFi keep-alive
// =====================
static void wifiKeepAlive() {
  wl_status_t st = WiFi.status();

  if (st != g_lastWiFi) {
    g_lastWiFi = st;
    if (st == WL_CONNECTED) logSerialAndTg("📶 WiFi BAGLANDI. IP=" + WiFi.localIP().toString(), true, true);
    else                   logSerialAndTg("📶 WiFi KOPTU (status=" + String((int)st) + ")", true, true);
  }

  if (st == WL_CONNECTED) return;

  uint32_t nowMs = millis();
  if (nowMs - g_lastWiFiTryMs < 15000) return;
  g_lastWiFiTryMs = nowMs;

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logSerialAndTg("🔁 WiFi yeniden baglanma denemesi...", true);
}

// =====================
// Bugün cache'de yoksa online ise çek
// =====================
static void ensureTodayInCache() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!isTimeValid()) return;

  if (g_dayCount == 0 || !hasYmd(ymdToday())) {
    logSerialAndTg("📥 Cache bugunu icermiyor -> vakit indiriliyor...", true, true);
    if (fetchAndStoreMonthly(true)) loadTimesFromNvs();
  }
}

// =====================
// Özel gün penceresi
// =====================
static bool computeWindowForSpecial(const SpecialDef& sp, time_t& onTs, time_t& offTs, uint32_t& ymdEvent, String& hicriText) {
  if (g_dayCount == 0) return false;

  int foundIdx = -1;

  for (uint16_t i = 0; i < g_dayCount; i++) {
    int hd=0, hy=0; String hm;
    if (!parseHicri(g_days[i].hicriUzun, hd, hm, hy)) continue;

    String key = normMonthKey(hm);
    if (hd == sp.day && key == String(sp.monthKey)) {
      foundIdx = (int)i;
      break;
    }
  }

  if (foundIdx < 0) return false;

  ymdEvent  = g_days[foundIdx].ymd;
  hicriText = String(g_days[foundIdx].hicriUzun);

  uint32_t ymdPrev = addDaysYmd(ymdEvent, -1);
  int ip = findIdx(ymdPrev);
  if (ip < 0) return false;

  onTs  = epochFromYmdAndMin(ymdPrev,  g_days[ip].aksamMin) + ON_OFFSET_SEC;
  offTs = epochFromYmdAndMin(ymdEvent, g_days[foundIdx].imsakMin) - OFF_OFFSET_SEC;

  return (offTs > onTs);
}

static bool buildWindowForEventIdx(int idxEvent, time_t& onTs, time_t& offTs) {
  if (idxEvent < 0 || idxEvent >= (int)g_dayCount) return false;
  uint32_t ymdEvent = g_days[idxEvent].ymd;
  uint32_t ymdPrev  = addDaysYmd(ymdEvent, -1);

  int ip = findIdx(ymdPrev);
  if (ip < 0) return false;

  onTs  = epochFromYmdAndMin(ymdPrev,  g_days[ip].aksamMin) + ON_OFFSET_SEC;
  offTs = epochFromYmdAndMin(ymdEvent, g_days[idxEvent].imsakMin) - OFF_OFFSET_SEC;

  return (offTs > onTs);
}

static bool isRamazanIdx(int idx, int& dayNoOut) {
  dayNoOut = 0;
  if (idx < 0 || idx >= (int)g_dayCount) return false;

  int hd=0, hy=0; String hm;
  if (!parseHicri(g_days[idx].hicriUzun, hd, hm, hy)) return false;

  if (normMonthKey(hm) == "ramazan") {
    dayNoOut = hd;
    return (hd >= 1 && hd <= 30);
  }
  return false;
}

// =====================
// En yakın / aktif dini pencereyi seç
// =====================
static void computeNextSpecial(time_t now) {
  g_spOnTs = 0; g_spOffTs = 0; g_spName = ""; g_spHicri = ""; g_spYmd = 0;

  bool foundActive = false;
  time_t bestOn = 0, bestOff = 0;
  String bestName = "", bestHicri = "";
  uint32_t bestYmd = 0;

  // 1) Klasik özel günler
  for (uint8_t i = 0; i < SPECIAL_COUNT; i++) {
    if (g_specials[i].enabled == 0) continue;

    time_t on=0, off=0; uint32_t ymdEv=0; String hicri;
    if (!computeWindowForSpecial(g_specials[i], on, off, ymdEv, hicri)) continue;

    bool active = (now >= on && now < off);
    if (active) {
      if (!foundActive || on < bestOn) {
        foundActive = true;
        bestOn = on; bestOff = off; bestName = g_specials[i].name; bestHicri = hicri; bestYmd = ymdEv;
      }
    } else if (!foundActive) {
      if (on > now && (bestOn == 0 || on < bestOn)) {
        bestOn = on; bestOff = off; bestName = g_specials[i].name; bestHicri = hicri; bestYmd = ymdEv;
      }
    }
  }

#if EN_RAMAZAN_TUM_GUNLER
  // 2) Ramazan tüm günler
  for (int idx = 0; idx < (int)g_dayCount; idx++) {
    int dayNo = 0;
    if (!isRamazanIdx(idx, dayNo)) continue;

    time_t on=0, off=0;
    if (!buildWindowForEventIdx(idx, on, off)) continue;

    bool active = (now >= on && now < off);

    String name  = String("Ramazan Günü ") + String(dayNo);
    String hicri = String(g_days[idx].hicriUzun);
    uint32_t ymdEv = g_days[idx].ymd;

    if (active) {
      if (!foundActive || on < bestOn) {
        foundActive = true;
        bestOn = on; bestOff = off; bestName = name; bestHicri = hicri; bestYmd = ymdEv;
      }
    } else if (!foundActive) {
      if (on > now && (bestOn == 0 || on < bestOn)) {
        bestOn = on; bestOff = off; bestName = name; bestHicri = hicri; bestYmd = ymdEv;
      }
    }
  }
#endif

  if (bestOn != 0 && bestOff != 0) {
    g_spOnTs = bestOn;
    g_spOffTs = bestOff;
    g_spName = bestName;
    g_spHicri = bestHicri;
    g_spYmd = bestYmd;
  }
}

// =====================
// Özel gün yaklaşan bildirim
// =====================
static void specialNotifyTick() {
  if (g_updateInProgress) return;
  if (!isTimeValid()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (g_spOnTs == 0 || g_spOffTs == 0 || g_spName.length() == 0) return;

  time_t now = time(nullptr);
  if (now >= g_spOffTs) return;
  if (g_lastSpNotifyOnTs == g_spOnTs) return;

  time_t notifyTs = g_spOnTs - (time_t)SP_NOTIFY_BEFORE_SEC;
  if (now >= notifyTs) {
    char onb[32], offb[32];
    formatDateTime(g_spOnTs, onb, sizeof(onb));
    formatDateTime(g_spOffTs, offb, sizeof(offb));

    String msg;
    msg += "⏳ Yaklaşan dini gün\n";
    msg += "📌 " + g_spName + "\n";
    if (g_spHicri.length() > 0) msg += "🗓️ " + g_spHicri + "\n";
    msg += "ON:  " + String(onb) + "\n";
    msg += "OFF: " + String(offb) + "\n";
    msg += "Kural: Akşam+1dk -> İmsak-1dk";

    tgSend("[" + nowStamp() + "] " + msg, true);
    g_lastSpNotifyOnTs = g_spOnTs;
  }
}

// =====================
// Haftalık update
// =====================
static void weeklyUpdateTick() {
  if (g_updateInProgress || g_updatePending) return;
  if (!isTimeValid()) return;

  time_t now = time(nullptr);
  tm t{}; localtime_r(&now, &t);

  uint32_t today = ymdFromTm(t);
  uint32_t lastUpd = prefs.getUInt("lastUpdYmd", 0);

  static uint32_t lastMinuteKey = 0;
  uint32_t minuteKey = today * 1440u + (uint32_t)(t.tm_hour * 60 + t.tm_min);
  if (minuteKey == lastMinuteKey) return;
  lastMinuteKey = minuteKey;

  if (t.tm_wday != UPDATE_DOW) return;
  if (today == lastUpd) return;

  bool isMainTime = (t.tm_hour == UPDATE_HOUR && t.tm_min == UPDATE_MIN);
  bool isRetry    = (t.tm_min == 5);

  if ((isMainTime || isRetry) && WiFi.status() == WL_CONNECTED) {
    logSerialAndTg("📥 Pazartesi update: 30 gunluk vakit cekiliyor...", true, true);
    if (fetchAndStoreMonthly(true)) {
      loadTimesFromNvs();

      time_t on2=0, off2=0;
      if (computeThuFriWindowForNow(time(nullptr), on2, off2)) { g_thuOnTs = on2; g_thuOffTs = off2; }

      computeNextSpecial(time(nullptr));

      time_t nextOff=0;
      if (computeNextImsakOff(time(nullptr), nextOff)) g_nextImsakOffTs = nextOff;

      char a[32], b[32], c[32], s1[32], s2[32];
      formatDateTime(g_thuOnTs, a, sizeof(a));
      formatDateTime(g_thuOffTs, b, sizeof(b));
      formatDateTime(g_nextImsakOffTs, c, sizeof(c));
      formatDateTime(g_spOnTs, s1, sizeof(s1));
      formatDateTime(g_spOffTs, s2, sizeof(s2));

      String msg = String("🕒 Persembe: ON=")+a+" OFF="+b+
                   "\n⏱️ Zorunlu OFF: "+c+
                   "\n🎉 Dini Gun: " + (g_spName.length()? g_spName : String("-")) +
                   "\n   ON=" + String(s1) + " OFF=" + String(s2);
      logSerialAndTg(msg, true, true);
    } else {
      logSerialAndTg("❌ Pazartesi update FAIL (sonraki :05'te tekrar dener)", true, true);
    }
  }
}

// =====================
// Buton (sadece ON)
// =====================
static void handleButton() {
  static bool lastStable = true;
  static bool lastRead = true;
  static uint32_t lastChangeMs = 0;

  bool r = digitalRead(BUTTON_PIN);
  if (r != lastRead) { lastRead = r; lastChangeMs = millis(); }

  if ((millis() - lastChangeMs) > BTN_DEBOUNCE_MS) {
    if (lastStable != lastRead) {
      lastStable = lastRead;
      if (lastStable == LOW) {
        g_manualOffUntilTs = 0;
        g_manualOnLatched = true;
        if (!g_relayState) {
          relayWrite(true);
          logSerialAndTg("🟢 BUTON: Manual ON verildi", true, true);
        } else {
          logSerialAndTg("🟢 BUTON: Manual ON (zaten ON)", true);
        }
      }
    }
  }
}

// =====================
// Komut eşleştirme
// =====================
static bool cmdIs(const String& text, const char* cmd) {
  if (text == cmd) return true;
  String p = String(cmd) + "@";
  return text.startsWith(p);
}

static String whoStr(const telegramMessage& m) {
  String who;
  if (m.from_name.length() > 0) who += "(" + m.from_name + ") ";
  if (m.from_id.length() > 0)   who += "id=" + m.from_id;
  return who;
}

static int64_t parseIdArg(const String& text) {
  int sp = text.indexOf(' ');
  if (sp < 0) return 0;
  String s = text.substring(sp + 1);
  s.trim();
  if (s.length() == 0) return 0;
  return atoll(s.c_str());
}

// =====================
// /dinigünler için hafif liste (stack şişirmesin)
// =====================
enum ItemKind : uint8_t { KIND_SPECIAL=0, KIND_RAMAZAN=1 };

struct SpItemLite {
  uint32_t ymd;          // Miladi event günü
  time_t on;
  time_t off;
  uint8_t stateGroup;    // 0=aktif, 1=yaklaşan, 2=geçmiş
  uint8_t enabled;       // 1/0
  uint8_t kind;          // ItemKind
  uint8_t specialIndex;  // KIND_SPECIAL
  uint8_t ramazanDay;    // KIND_RAMAZAN
};

static const int MAX_SP_LIST = (int)SPECIAL_COUNT + 40;
static SpItemLite g_spList[MAX_SP_LIST];

static void sortSpLite(SpItemLite* arr, int n) {
  for (int i=0;i<n-1;i++){
    for (int j=i+1;j<n;j++){
      bool swap = false;
      if (arr[j].stateGroup < arr[i].stateGroup) swap = true;
      else if (arr[j].stateGroup == arr[i].stateGroup) {
        if (arr[j].ymd < arr[i].ymd) swap = true;                // en yakın tarih en üstte
        else if (arr[j].ymd == arr[i].ymd && arr[j].on < arr[i].on) swap = true;
      }
      if (swap) { SpItemLite tmp=arr[i]; arr[i]=arr[j]; arr[j]=tmp; }
    }
  }
}

// =====================
// /guncelle worker
// =====================
static void updateWorkerTick() {
  if (!g_updatePending) return;
  if (g_updateInProgress) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if ((int32_t)(millis() - g_updateCooldownUntilMs) < 0) return;

  g_updateInProgress = true;
  g_updatePending = false;
  g_updateCooldownUntilMs = millis() + 60000; // 60sn

  tgSend("[" + nowStamp() + "] 📥 Manuel guncelleme basladi...\n👤 " + g_updateRequesterWho, true);

  bool ok = fetchAndStoreMonthly(true);
  if (ok) {
    loadTimesFromNvs();

    time_t on2=0, off2=0;
    if (isTimeValid() && computeThuFriWindowForNow(time(nullptr), on2, off2)) { g_thuOnTs = on2; g_thuOffTs = off2; }

    if (isTimeValid()) computeNextSpecial(time(nullptr));

    time_t nextOff=0;
    if (isTimeValid() && computeNextImsakOff(time(nullptr), nextOff)) g_nextImsakOffTs = nextOff;

    tgSend("[" + nowStamp() + "] ✅ Manuel guncelleme OK\n👤 " + g_updateRequesterWho, true);
  } else {
    tgSend("[" + nowStamp() + "] ❌ Manuel guncelleme FAIL\n👤 " + g_updateRequesterWho, true);
  }

  g_updateInProgress = false;
}

// =====================
// Röle karar + override + bloklar
// =====================
static void applyRelayLogic() {
  bool scheduledOn = false;

  time_t now = 0;
  bool hasTime = isTimeValid();
  if (hasTime) now = time(nullptr);

  if (hasTime && g_manualOffUntilTs != 0 && now >= g_manualOffUntilTs) {
    g_manualOffUntilTs = 0;
  }

  if (hasTime) {
    bool thuOn=false, spOn=false; time_t schedOff=0;
    getScheduleState(now, thuOn, spOn, schedOff);
    scheduledOn = thuOn || spOn;
  }

  bool shouldOn = scheduledOn || g_manualOnLatched;

  if (hasTime && g_manualOffUntilTs != 0 && now < g_manualOffUntilTs) shouldOn = false;
  if (hasTime && g_blockOnUntilTs != 0 && now < g_blockOnUntilTs)     shouldOn = false;

  if (shouldOn != g_relayState) {
    relayWrite(shouldOn);
    logSerialAndTg(shouldOn ? "🔔 ROLE: ON" : "🔕 ROLE: OFF", true);
  }
}

// =====================
// Telegram handler
// =====================
static void handleTelegram() {
  if (g_updateInProgress) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t nowMs = millis();
  if (nowMs - g_lastTgPollMs < TG_POLL_MS) return;
  g_lastTgPollMs = nowMs;

  int n = bot.getUpdates(bot.last_message_received + 1);
  while (n) {
    for (int i = 0; i < n; i++) {
      String chat_id = bot.messages[i].chat_id;
      String text    = bot.messages[i].text;
      if (chat_id != String(CHAT_ID)) continue;

      String who = whoStr(bot.messages[i]);
      int64_t uid = atoll(bot.messages[i].from_id.c_str());
      bool isAdminUser = isAdmin(uid);

      if (cmdIs(text, "/myid")) {
        bot.sendMessage(CHAT_ID, "👤 " + who, "");
      }
      else if (cmdIs(text, "/help") || cmdIs(text, "/yardim") || cmdIs(text, "/yardım")) {
        String msg;
        msg += "Komutlar:\n";
        msg += "/durum\n/myid\n/dinigunler\n";
        msg += "\nAdmin:\n";
        msg += "/on /off /guncelle\n/admin_list\n/admin_add <id>\n/admin_del <id>\n";
        bot.sendMessage(CHAT_ID, msg, "");
      }
      else if (cmdIs(text, "/durum") || cmdIs(text, "/status")) {
        char a[32], b[32], c[32], s1[32], s2[32], mo[32];
        formatDateTime(g_thuOnTs, a, sizeof(a));
        formatDateTime(g_thuOffTs, b, sizeof(b));
        formatDateTime(g_nextImsakOffTs, c, sizeof(c));
        formatDateTime(g_spOnTs, s1, sizeof(s1));
        formatDateTime(g_spOffTs, s2, sizeof(s2));
        formatDateTime(g_manualOffUntilTs, mo, sizeof(mo));

        bool thuOn=false, spOn=false; time_t schedOff=0;
        if (isTimeValid()) getScheduleState(time(nullptr), thuOn, spOn, schedOff);

        String msg;
        msg += "📌 Durum\n";
        msg += "Role: " + String(g_relayState ? "ON" : "OFF") + "\n";
        msg += "ManualLatch: " + String(g_manualOnLatched ? "ON" : "OFF") + "\n";
        msg += "ManualOffOverrideUntil: " + String(mo) + "\n";
        msg += "Persembe->Cuma ON: " + String(a) + "\n";
        msg += "Persembe->Cuma OFF: " + String(b) + "\n";
        if (g_spName.length() > 0) {
          msg += "Dini Gun (" + g_spName + ") ON: " + String(s1) + "\n";
          msg += "Dini Gun (" + g_spName + ") OFF: " + String(s2) + "\n";
          if (g_spHicri.length() > 0) msg += "Hicri: " + g_spHicri + "\n";
        } else {
          msg += "Dini Gun: -\n";
        }
        msg += "Zorunlu OFF (Imsak-1dk): " + String(c) + "\n";
        msg += "DBG thuOn=" + String(thuOn ? "1":"0") + " spOn=" + String(spOn ? "1":"0") + "\n";
        msg += "Admins: " + String((int)g_adminCount) + " (OWNER=" + String((long long)OWNER_ADMIN_ID) + ")\n";
        msg += "Sen: " + who;

        bot.sendMessage(CHAT_ID, msg, "");
      }
      else if (cmdIs(text, "/dinigunler") || cmdIs(text, "/dinigünler")) {
        if (g_dayCount == 0) {
          bot.sendMessage(CHAT_ID, "Yakın tarihte dini gün yok", "");
        } else {
          time_t now = isTimeValid() ? time(nullptr) : 0;
          int cnt = 0;

          // özel günler
          for (uint8_t k = 0; k < SPECIAL_COUNT && cnt < MAX_SP_LIST; k++) {
            time_t on=0, off=0; uint32_t ymdEv=0; String hicri;
            if (!computeWindowForSpecial(g_specials[k], on, off, ymdEv, hicri)) continue;

            uint8_t group = 2;
            if (isTimeValid() && now >= on && now < off) group = 0;
            else if (isTimeValid() && on > now)          group = 1;

            // Sadece AKTIF + YAKLASAN
            if (group == 2) continue;

            SpItemLite it{};
            it.ymd = ymdEv;
            it.on = on; it.off = off;
            it.enabled = g_specials[k].enabled;
            it.kind = KIND_SPECIAL;
            it.specialIndex = k;
            it.stateGroup = group;

            g_spList[cnt++] = it;
          }

#if EN_RAMAZAN_TUM_GUNLER
          // ramazan günleri
          for (int idx=0; idx<(int)g_dayCount && cnt < MAX_SP_LIST; idx++) {
            int dayNo=0;
            if (!isRamazanIdx(idx, dayNo)) continue;

            time_t on=0, off=0;
            if (!buildWindowForEventIdx(idx, on, off)) continue;

            uint8_t group = 2;
            if (isTimeValid() && now >= on && now < off) group = 0;
            else if (isTimeValid() && on > now)          group = 1;

            if (group == 2) continue;

            SpItemLite it{};
            it.ymd = g_days[idx].ymd;
            it.on = on; it.off = off;
            it.enabled = 1;
            it.kind = KIND_RAMAZAN;
            it.ramazanDay = (uint8_t)dayNo;
            it.stateGroup = group;

            g_spList[cnt++] = it;
          }
#endif

          if (cnt == 0) {
            bot.sendMessage(CHAT_ID, "Yakın tarihte dini gün yok", "");
          } else {
            sortSpLite(g_spList, cnt);

            String out;
            for (int ii=0; ii<cnt; ii++) {
              char ddmmyyyy[16];
              ymdToDdMmYyyy(g_spList[ii].ymd, ddmmyyyy, sizeof(ddmmyyyy));

              String st = (g_spList[ii].stateGroup==0) ? "🟢 AKTIF" : "🟡 YAKLASAN";
              String en = g_spList[ii].enabled ? "✅" : "🚫";

              String name;
              if (g_spList[ii].kind == KIND_SPECIAL) {
                name = String(g_specials[g_spList[ii].specialIndex].name);
              } else {
                name = String("Ramazan Günü ") + String(g_spList[ii].ramazanDay);
              }

              // İstenen tek satır format
              out += en + " " + st + " - " + name + " Miladi: " + String(ddmmyyyy) + "\n";

              if (out.length() > 3300) { bot.sendMessage(CHAT_ID, out, ""); out = ""; }
            }
            if (out.length() > 0) bot.sendMessage(CHAT_ID, out, "");
          }
        }
      }

      // Admin komutları
      else if (cmdIs(text, "/admin_list") || cmdIs(text, "/admins")) {
        if (!isAdminUser) {
          bot.sendMessage(CHAT_ID, "⛔ Yetkisiz: /admin_list\n👤 " + who, "");
          logSerialAndTg("⛔ YETKISIZ /admin_list  " + who, true, true);
        } else {
          String msg = "👮 Admin listesi:\n";
          for (uint8_t k = 0; k < g_adminCount; k++) {
            msg += String((long long)g_adminIds[k]);
            if (g_adminIds[k] == OWNER_ADMIN_ID) msg += " (OWNER)";
            msg += "\n";
          }
          bot.sendMessage(CHAT_ID, msg, "");
        }
      }
      else if (text.startsWith("/admin_add")) {
        if (!isAdminUser) {
          bot.sendMessage(CHAT_ID, "⛔ Yetkisiz: /admin_add\n👤 " + who, "");
          logSerialAndTg("⛔ YETKISIZ /admin_add  " + who, true, true);
        } else {
          int64_t newId = parseIdArg(text);
          if (newId == 0) bot.sendMessage(CHAT_ID, "Kullanim: /admin_add 123456789\nİpucu: kisi /myid yazsin.", "");
          else {
            bool ok = addAdmin(newId);
            if (ok) {
              bot.sendMessage(CHAT_ID, "✅ Admin eklendi: " + String((long long)newId) + "\n👤 Ekleyen: " + who, "");
              logSerialAndTg("✅ Admin eklendi: " + String((long long)newId) + "  Ekleyen: " + who, true, true);
            } else bot.sendMessage(CHAT_ID, "❌ Admin eklenemedi (liste dolu olabilir).", "");
          }
        }
      }
      else if (text.startsWith("/admin_del")) {
        if (!isAdminUser) {
          bot.sendMessage(CHAT_ID, "⛔ Yetkisiz: /admin_del\n👤 " + who, "");
          logSerialAndTg("⛔ YETKISIZ /admin_del  " + who, true, true);
        } else {
          int64_t delId = parseIdArg(text);
          if (delId == 0) bot.sendMessage(CHAT_ID, "Kullanim: /admin_del 123456789", "");
          else {
            bool ok = delAdmin(delId);
            if (ok) {
              bot.sendMessage(CHAT_ID, "✅ Admin silindi: " + String((long long)delId) + "\n👤 Silen: " + who, "");
              logSerialAndTg("✅ Admin silindi: " + String((long long)delId) + "  Silen: " + who, true, true);
            } else bot.sendMessage(CHAT_ID, "❌ Admin silinemedi (OWNER/son admin olabilir veya yok).", "");
          }
        }
      }
      else if (cmdIs(text, "/on")) {
        if (!isAdminUser) {
          bot.sendMessage(CHAT_ID, "⛔ Yetkisiz: /on\n👤 " + who, "");
          logSerialAndTg("⛔ YETKISIZ /on  " + who, true, true);
        } else {
          g_manualOffUntilTs = 0;

          if (isTimeValid()) {
            time_t noff=0;
            if (computeNextImsakOff(time(nullptr), noff)) g_nextImsakOffTs = noff;
          }

          if (isTimeValid() && g_blockOnUntilTs != 0 && time(nullptr) < g_blockOnUntilTs) {
            bot.sendMessage(CHAT_ID, "⛔ Su an ON engelli (Imsak zorunlu OFF sonrasi).\n👤 " + who, "");
          } else {
            g_manualOnLatched = true;
            relayWrite(true);
            logSerialAndTg("🟢 TELEGRAM: Manual ON  " + who, true, true);
            bot.sendMessage(CHAT_ID, "✅ Manual ON. (Imsak-1dk'da otomatik OFF)\n👤 " + who, "");
          }
        }
      }
      else if (cmdIs(text, "/off")) {
        if (!isAdminUser) {
          bot.sendMessage(CHAT_ID, "⛔ Yetkisiz: /off\n👤 " + who, "");
          logSerialAndTg("⛔ YETKISIZ /off  " + who, true, true);
        } else {
          time_t untilTs = 0;
          if (isTimeValid()) {
            bool thuOn=false, spOn=false; time_t schedOff=0;
            getScheduleState(time(nullptr), thuOn, spOn, schedOff);
            if (schedOff) untilTs = schedOff;
          }
          g_manualOffUntilTs = untilTs;

          g_manualOnLatched = false;
          relayWrite(false);
          logSerialAndTg("🔴 TELEGRAM: Manual OFF  " + who, true, true);

          if (g_manualOffUntilTs != 0) {
            char ub[32]; formatDateTime(g_manualOffUntilTs, ub, sizeof(ub));
            bot.sendMessage(CHAT_ID, "✅ Manual OFF (Scheduled override)\n⛔ Tekrar ON olmayacak (pencere bitişi): " + String(ub) + "\n👤 " + who, "");
          } else {
            bot.sendMessage(CHAT_ID, "✅ Manual OFF\n👤 " + who, "");
          }
        }
      }
      else if (cmdIs(text, "/guncelle") || cmdIs(text, "/güncelle") || cmdIs(text, "/update")) {
        if (!isAdminUser) {
          bot.sendMessage(CHAT_ID, "⛔ Yetkisiz: /guncelle\n👤 " + who, "");
          logSerialAndTg("⛔ YETKISIZ /guncelle  " + who, true, true);
        } else {
          tgSaveLastIdToNvsIfNew();

          if (g_updateInProgress) {
            bot.sendMessage(CHAT_ID, "⏳ Zaten guncelleme calisiyor.\n👤 " + who, "");
          } else if (g_updatePending) {
            bot.sendMessage(CHAT_ID, "⏳ Guncelleme kuyrukta.\n👤 " + who, "");
          } else if ((int32_t)(millis() - g_updateCooldownUntilMs) < 0) {
            bot.sendMessage(CHAT_ID, "⏳ Bekle: guncelleme koruma (cooldown) aktif.\n👤 " + who, "");
          } else {
            g_updatePending = true;
            g_updateRequesterWho = who;
            g_updateRequesterId  = uid;

            bot.sendMessage(CHAT_ID, "✅ /guncelle alindi. Cache guncellemesi baslatilacak...\n👤 " + who, "");
            logSerialAndTg("📌 /guncelle kuyruğa alindi  " + who, true, true);
          }
        }
      }
    }

    n = bot.getUpdates(bot.last_message_received + 1);
  }

  tgSaveLastIdToNvsIfNew();
}

// =====================
// Setup / Loop
// =====================
void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(200);

  esp_reset_reason_t rr = esp_reset_reason();
  Serial.print("[BOOT] ResetReason=");
  Serial.println((int)rr);

  prefs.begin("cami", false);

#if FORCE_RESET_ADMINS
  resetAdminsToOwner();
#endif

  loadAdminsFromNvs();
  ensureOwnerIsAdmin();

  loadTimesFromNvs();
  tgLoadLastIdFromNvs();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logSerialAndTg("🚀 Sistem basladi. WiFi baglaniyor...", true, true);

  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 12000) delay(250);

  if (WiFi.status() == WL_CONNECTED) logSerialAndTg("📶 WiFi baglandi. IP=" + WiFi.localIP().toString(), true, true);
  else logSerialAndTg("⚠️ WiFi baglanamadi (offline devam).", true, true);

  configTime(3 * 3600, 0, "pool.ntp.org", "time.google.com");

  time_t now = time(nullptr);
  startMs = millis();
  while (now < 1700000000 && (millis() - startMs) < 12000) { delay(200); now = time(nullptr); }

  logSerialAndTg(isTimeValid() ? "🕰️ NTP saat hazir." : "⚠️ NTP saat hazir degil (offline olabilir).", true, true);

  ensureTodayInCache();

  time_t on2=0, off2=0;
  if (isTimeValid() && computeThuFriWindowForNow(time(nullptr), on2, off2)) { g_thuOnTs = on2; g_thuOffTs = off2; }

  if (isTimeValid()) computeNextSpecial(time(nullptr));

  time_t nextOff=0;
  if (isTimeValid() && computeNextImsakOff(time(nullptr), nextOff)) g_nextImsakOffTs = nextOff;

  char a[32], b[32], c[32], s1[32], s2[32];
  formatDateTime(g_thuOnTs, a, sizeof(a));
  formatDateTime(g_thuOffTs, b, sizeof(b));
  formatDateTime(g_nextImsakOffTs, c, sizeof(c));
  formatDateTime(g_spOnTs, s1, sizeof(s1));
  formatDateTime(g_spOffTs, s2, sizeof(s2));

  String initMsg = String("🧩 Init: Persembe ON=")+a+" OFF="+b+
                   "\n⏱️ Zorunlu OFF="+c+
                   "\n🎉 Dini Gun: " + (g_spName.length()? g_spName : String("-")) +
                   "\n   ON=" + String(s1) + " OFF=" + String(s2);
  logSerialAndTg(initMsg, true, true);

  applyRelayLogic();
}

void loop() {
  wifiKeepAlive();

  // /guncelle worker önce
  updateWorkerTick();

  handleTelegram();
  handleButton();
  weeklyUpdateTick();

  if (isTimeValid()) {
    time_t now = time(nullptr);

    if (g_thuOffTs == 0 || now >= g_thuOffTs) {
      time_t on2=0, off2=0;
      if (!computeThuFriWindowForNow(now, on2, off2)) {
        ensureTodayInCache();
        (void)computeThuFriWindowForNow(now, on2, off2);
      }
      if (on2 && off2) { g_thuOnTs = on2; g_thuOffTs = off2; }
    }

    if (g_spOffTs == 0 || now >= g_spOffTs) {
      ensureTodayInCache();
      computeNextSpecial(now);
    }

    if (g_nextImsakOffTs == 0 || now >= (g_nextImsakOffTs + 5)) {
      time_t noff=0;
      if (!computeNextImsakOff(now, noff)) {
        ensureTodayInCache();
        (void)computeNextImsakOff(now, noff);
      }
      if (noff) g_nextImsakOffTs = noff;
    }
  }

  specialNotifyTick();
  enforceImsakOffIfDue();
  applyRelayLogic();

  delay(250);
}
