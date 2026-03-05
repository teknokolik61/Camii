/*
  src/main.cpp - ESP32 Röle + Ezan Vakti + Telegram (Menü/Panel) + Buton (ILCE_ID=9206 Çankaya)

  Bu sürümde:
  - /start /menu /panel -> Telegram inline menü açar
  - "Şerefeler" tek tuş Toggle (AÇ↔KAPAT) + buton yazısı anlık duruma göre değişir (AÇ ✅ / KAPAT ❌)
  - Menü: Şerefeler Toggle / Yenile / (Yetkili ise) Ayarlar
  - Ayarlar > Toleranslar:
      Akşam Tolerans (g_onOffsetSec) ve Sabah Tolerans (g_offOffsetSec) dakika bazında (+/- 1 dk) ayarlanır
      (adım 60 sn) ve NVS'e kaydedilir.
  - Ayarlar sekmesi sadece yetkili (admin) kullanıcıda görünür.
  - Mevcut /durum /dinigunler /on /off /guncelle admin sistemi korunur.

  WEB (GÜNCEL):
  - "/" Public sayfa: herkes görür (durum + anahtar alanı). AYAR GÖSTERMEZ.
  - "/admin" Yönetim sayfası: sekmeler (Durum / Tolerans / Dini Günler / Komutlar)
  - Toleranslar ve Dini Günler sadece anahtar doğruysa (WEB_KEY) API üzerinden görünür.
  - /api/public (auth yok) sadece durum json döner.
  - /api/settings ve /api/action (auth ister) ayarlar/komutlar.

  Not:
  - UniversalTelegramBot kütüphanesinde mesaj düzenleme için:
      sendMessageWithInlineKeyboard(chat_id, text, "", keyboardJson, message_id)
    kullanılır. (message_id != 0 ise editMessageText çağrılır)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// PSRAM allocator for ArduinoJson (ESP32-S3 8MB PSRAM)
struct PsramAllocator {
  void* allocate(size_t size) {
    if (psramFound()) return ps_malloc(size);
    return malloc(size);
  }
  void deallocate(void* ptr) { free(ptr); }
  void* reallocate(void* ptr, size_t size) {
    if (psramFound()) return ps_realloc(ptr, size);
    return realloc(ptr, size);
  }
};
using PsramJsonDocument = BasicJsonDocument<PsramAllocator>;
#include <cstring>  // memcmp
#include <Preferences.h>
#include <time.h>
#include <UniversalTelegramBot.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <nvs.h>
#include "secrets.h"

#ifndef SECRET_WIFI_SSID
#define SECRET_WIFI_SSID ""
#endif
#ifndef SECRET_WIFI_PASS
#define SECRET_WIFI_PASS ""
#endif
#ifndef SECRET_WEB_KEY
#define SECRET_WEB_KEY ""
#endif

// ===== Admin NVS reset (gerekirse 1 yap, bir kere calistir, sonra 0'a cek) =====
#define FORCE_RESET_ADMINS 0

// =====================
// WiFi (Secrets öncelikli, secrets boşsa NVS fallback)
// =====================
static String g_wifiSsid = String(SECRET_WIFI_SSID);
static String g_wifiPass = String(SECRET_WIFI_PASS);

// NVS keys (WiFi + Web Key)
static const char* NVS_KEY_WIFI_SSID = "wifiSsid";
static const char* NVS_KEY_WIFI_PASS = "wifiPass";
static const char* NVS_KEY_WEB_KEY   = "webKey";
static const char* NVS_KEY_BOT_TOKEN = "botToken";
static const char* NVS_KEY_CHAT_ID   = "chatId";
static const char* NVS_KEY_HNY_DAY   = "hnyDay";
static const char* NVS_KEY_HNY_MON   = "hnyMon";
static const char* NVS_KEY_AUTO_HYR  = "autoHyr";
static const char* NVS_KEY_HNY_LAST  = "hnyLast";
static const char* NVS_KEY_ULOG_BLOB = "uLogBlob";
static const char* NVS_KEY_ULOG_META = "uLogMeta"; // idx + cnt
static const char* NVS_KEY_SLOG_BLOB = "sLogBlob";
static const char* NVS_KEY_SLOG_META = "sLogMeta";
static const char* NVS_KEY_LAST_ALIVE = "lastAlive";  // son bilinen çalışma zamanı (epoch)

// WEB_KEY (Secrets öncelikli, secrets boşsa NVS fallback)
static String g_webKey = String(SECRET_WEB_KEY);

// Kaynak bilgisi (UI için)
static String g_wifiSrc   = "secrets"; // secrets / nvs / none
static String g_webKeySrc = "secrets"; // secrets / nvs / none
static String g_botTokenSrc = "secrets";
static String g_chatIdSrc   = "secrets";
static uint8_t  g_hnyDay  = 1;
static uint8_t  g_hnyMon  = 0;
static bool     g_autoHicriYear = false;
static uint16_t g_hnyLastYear = 0;


// =====================
// Sürüm
// =====================

// =====================
// CPU İş Yükü İzleme (loop frekansı tabanlı)
// =====================
// loop() iterasyon hızını ölçer (loops/sec).
// delay(50) ile teorik max ~20/s. İş yükü arttıkça frekans düşer.
// Avantaj: HTTP blocking süreleri doğru yansır (düşük frekans = yoğun sistem).
static uint32_t g_loopCount        = 0;
static uint32_t g_loopWindowStartMs = 0;
static float    g_loopsPerSec      = 0.0f;
static float    g_cpuUsageTotal    = 0.0f;  // 0..100

// micros() tabanlı gerçek CPU ölçümü
static uint32_t g_cpuWorkUs   = 0;  // Gerçek iş süresi (blocking I/O hariç)
static uint32_t g_cpuIoUs     = 0;  // Blocking I/O bekleme süresi
static uint32_t g_cpuDelayUs  = 0;  // delay() boşta bekleme
static uint32_t g_cpuWindowMs = 0;

static void cpuWindowUpdate() {
  uint32_t now = millis();
  if (now - g_cpuWindowMs < 5000) return; // 5sn pencere
  g_cpuWindowMs = now;

  // loops/sec (bilgi amaçlı)
  uint32_t elapsed = now - g_loopWindowStartMs;
  if (elapsed > 0) g_loopsPerSec = (float)g_loopCount * 1000.0f / (float)elapsed;
  g_loopCount = 0;
  g_loopWindowStartMs = now;

  // CPU = iş süresi / (iş + boşta) oranı
  uint32_t totalUs = g_cpuWorkUs + g_cpuIoUs + g_cpuDelayUs;
  if (totalUs > 0) {
    g_cpuUsageTotal = 100.0f * (float)g_cpuWorkUs / (float)totalUs;
  } else {
    g_cpuUsageTotal = 0.0f;
  }
  if (g_cpuUsageTotal < 0.0f) g_cpuUsageTotal = 0.0f;
  if (g_cpuUsageTotal > 100.0f) g_cpuUsageTotal = 100.0f;

  g_cpuWorkUs = 0;
  g_cpuIoUs = 0;
  g_cpuDelayUs = 0;
}

static const char* APP_VERSION = "v12.041"; // 12-item refactor
static const int   FW_VER_NUM  = 12041;    // sayısal versiyon (OTA karşılaştırma)
static const char* APP_AUTHOR  = "Miraç Bahadır ÖZTÜRK";

// =====================
// Log Ring Buffer
// =====================
Preferences prefs; // NVS (log + ayarlar icin erken tanimlandi)
static const uint8_t LOG_MAX = 20;

struct LogEntry {
  char ts[20];   // "2025-03-04 14:05:32"
  char msg[80];
};

static LogEntry g_userLog[LOG_MAX];
static uint8_t  g_userLogIdx = 0;
static uint8_t  g_userLogCnt = 0;

static LogEntry g_sysLog[LOG_MAX];
static uint8_t  g_sysLogIdx = 0;
static uint8_t  g_sysLogCnt = 0;

static void logSaveToNvs(const char* blobKey, const char* metaKey, LogEntry* buf, uint8_t idx, uint8_t cnt) {
  prefs.putBytes(blobKey, buf, (size_t)LOG_MAX * sizeof(LogEntry));
  uint16_t meta = ((uint16_t)idx << 8) | cnt;
  prefs.putUShort(metaKey, meta);
}

static void logLoadFromNvs(const char* blobKey, const char* metaKey, LogEntry* buf, uint8_t& idx, uint8_t& cnt) {
  size_t need = (size_t)LOG_MAX * sizeof(LogEntry);
  size_t got = prefs.getBytes(blobKey, buf, need);
  if (got != need) { idx = 0; cnt = 0; return; }
  uint16_t meta = prefs.getUShort(metaKey, 0);
  idx = (uint8_t)(meta >> 8);
  cnt = (uint8_t)(meta & 0xFF);
  if (idx >= LOG_MAX) idx = 0;
  if (cnt > LOG_MAX) cnt = 0;
}

static void logPush(LogEntry* buf, uint8_t& idx, uint8_t& cnt, const char* msg,
                    const char* blobKey, const char* metaKey) {
  LogEntry& e = buf[idx];
  if (time(nullptr) >= 1700000000) {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    snprintf(e.ts, sizeof(e.ts), "%04d-%02d-%02d %02d:%02d:%02d",
             t->tm_year+1900, t->tm_mon+1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
  } else {
    uint32_t s = millis() / 1000;
    snprintf(e.ts, sizeof(e.ts), "boot+%lus", (unsigned long)s);
  }
  strncpy(e.msg, msg, sizeof(e.msg)-1);
  e.msg[sizeof(e.msg)-1] = '\0';
  idx = (idx + 1) % LOG_MAX;
  if (cnt < LOG_MAX) cnt++;
  logSaveToNvs(blobKey, metaKey, buf, idx, cnt);
}

static void logUser(const char* msg) { logPush(g_userLog, g_userLogIdx, g_userLogCnt, msg, NVS_KEY_ULOG_BLOB, NVS_KEY_ULOG_META); }
static void logUser(const String& msg) { logUser(msg.c_str()); }
static void logSys(const char* msg)  { logPush(g_sysLog, g_sysLogIdx, g_sysLogCnt, msg, NVS_KEY_SLOG_BLOB, NVS_KEY_SLOG_META); }
static void logSys(const String& msg) { logSys(msg.c_str()); }

// =====================
// Ağ (Statik IP) + İlçe ID (NVS)
// =====================
struct NetCfg {
  bool     useStatic = false;
  uint32_t ip        = 0;   // packed a.b.c.d -> 0xAABBCCDD
  uint32_t gw        = 0;
  uint32_t mask      = 0;
  uint32_t dns1      = 0;
  uint32_t dns2      = 0;
};

static NetCfg g_netCfg;

static const char* NVS_KEY_NET_USE  = "netUse";
static const char* NVS_KEY_NET_IP   = "netIP";
static const char* NVS_KEY_NET_GW   = "netGW";
static const char* NVS_KEY_NET_MASK = "netMASK";
static const char* NVS_KEY_NET_DNS1 = "netDNS1";
static const char* NVS_KEY_NET_DNS2 = "netDNS2";

static uint32_t g_ilceId = 9206;
static const char* NVS_KEY_ILCE = "ilceId";
static const char* NVS_KEY_HTTP_PORT = "httpPort";

static uint16_t g_httpPort = 80;

// (Ağ ayarı değişince) güvenli restart planı
static uint32_t g_restartAtMs = 0;
static String   g_restartWhy  = "";


// =====================
// Telegram
// =====================
static String g_botToken = String(SECRET_BOT_TOKEN);
static const char* CHAT_ID   = SECRET_CHAT_ID;
// OWNER admin: secrets.h'da tanımlıysa oradan gelsin
#ifndef SECRET_OWNER_ADMIN_ID
#define SECRET_OWNER_ADMIN_ID 1253195249
#endif
static const int64_t OWNER_ADMIN_ID = SECRET_OWNER_ADMIN_ID;

// Aktif chat (panelin gösterileceği yer). Varsayılan: secrets.h içindeki CHAT_ID.
// OWNER /menu /panel /start komutunu başka bir sohbette kullanırsa otomatik o sohbete taşınır (NVS'e kaydedilir).
static String g_activeChatId = String(SECRET_CHAT_ID);
static const char* NVS_KEY_ACTIVE_CHAT = "actChat";

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
// Ramazan ayındaki tüm günleri dini gün gibi dahil et
#define EN_RAMAZAN_TUM_GUNLER           1

// Telegram poll + spam koruma
// Not: Daha hızlı tepki için poll süresi biraz düşürüldü.
static const uint32_t TG_POLL_MS  = 900;
static const uint32_t TG_DEDUP_MS = 25000;

// =====================
// Röle
// =====================
// Pin tanımları platformio.ini build_flags'tan gelir
// Fallback (doğrudan derleme için)
#ifndef RELAY_PIN
  #define RELAY_PIN 23
#endif
#ifndef BUTTON_PIN
  #define BUTTON_PIN 27
#endif
#ifndef BOARD_TYPE
  #define BOARD_TYPE 0
#endif
#ifndef BOARD_NAME
  #define BOARD_NAME "Bilinmeyen"
#endif

// Firmware'e gömülü board parmak izi (OTA uyumluluk kontrolü)
// getBoardTag() setup'ta çağrılır -> compiler strip edemez
#define _CAMI_STR(x) #x
#define _CAMI_XSTR(x) _CAMI_STR(x)
static const char* getBoardTag() {
  static const char tag[] = "\x7F" "CAMI_BT:" _CAMI_XSTR(BOARD_TYPE) "\x7F";
  return tag;
}
// Versiyon tag'ı (ayrı, OTA kontrolü için)
static const char* getVersionTag() {
  static char vtag[32];
  snprintf(vtag, sizeof(vtag), "\x7E" "CAMI_VN:%d" "\x7E", FW_VER_NUM);
  return vtag;
}
static const bool RELAY_ACTIVE_LOW = true;

// =====================
// Buton (sadece ON)
// =====================
// Button: fiziksel buton ile röle kontrolü
static const uint32_t BTN_DEBOUNCE_MS = 40;

// =====================
// Toleranslar (NVS - dakika adımı 60sn)
// =====================
static const int DEFAULT_ON_OFFSET_SEC  = 60;  // Akşam tolerans (Akşam +X)
static const int DEFAULT_OFF_OFFSET_SEC = 60;  // Sabah tolerans (İmsak -X)
static int g_onOffsetSec  = DEFAULT_ON_OFFSET_SEC;
static int g_offOffsetSec = DEFAULT_OFF_OFFSET_SEC;

// NVS key
static const char* NVS_KEY_ONOFFS  = "onOffs";
static const char* NVS_KEY_OFFOFFS = "offOffs";

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
  uint8_t   defaultEnabled;
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
};
static const uint8_t SPECIAL_COUNT = sizeof(g_specials)/sizeof(g_specials[0]);

// =====================
// Global state
// =====================

// =====================
// Dini gün enable ayarları (NVS kalıcı)
// - ✅ aktif olanlar: dini gün kuralı çalışır
// - ❌ kapalı olanlar: dini gün kuralı çalışmaz ve /dinigunler listesinde görünmez
// Not: Perşembe->Cuma kuralı her zaman çalışır (bu ayarlar onu etkilemez).
// =====================
static const uint8_t SP_SETTINGS_VER = 1;
static const char*   NVS_KEY_SP_VER  = "spVer";
static const char*   NVS_KEY_SP_MASK = "spMask";
static const char*   NVS_KEY_SP_RAM  = "spRam"; // bool

static uint32_t g_spEnableMask = 0; // bit=1 -> aktif
static bool     g_enableRamazanAll = (EN_RAMAZAN_TUM_GUNLER ? true : false);

// (Telegram) Dini Günler ayar menüsü kaldırıldı (Web Panel üzerinden yönetilir)

static inline uint32_t spValidMask() {
  return (SPECIAL_COUNT >= 32) ? 0xFFFFFFFFu : ((1u << SPECIAL_COUNT) - 1u);
}

static uint32_t spDefaultMask() {
  uint32_t m = 0;
  for (uint8_t i = 0; i < SPECIAL_COUNT; i++) {
    if (g_specials[i].defaultEnabled) m |= (1u << i);
  }
  return (m & spValidMask());
}

static bool saveSpecialEnableToNvs(String* errOut = nullptr) {
  // Preferences put* fonksiyonları başarısız olursa 0 döndürebilir.
  size_t a = prefs.putUChar(NVS_KEY_SP_VER, SP_SETTINGS_VER);
  size_t b = prefs.putUInt(NVS_KEY_SP_MASK, (g_spEnableMask & spValidMask()));
  size_t c = prefs.putBool(NVS_KEY_SP_RAM, g_enableRamazanAll);

  if (a == 0 || b == 0 || c == 0) {
    if (errOut) *errOut = "nvs_write_fail";
    return false;
  }
  return true;
}

static void loadSpecialEnableFromNvs() {
  uint8_t ver = prefs.getUChar(NVS_KEY_SP_VER, 0);
  if (ver != SP_SETTINGS_VER) {
    g_spEnableMask = spDefaultMask();
    g_enableRamazanAll = (EN_RAMAZAN_TUM_GUNLER ? true : false);
    saveSpecialEnableToNvs();
    return;
  }

  g_spEnableMask = prefs.getUInt(NVS_KEY_SP_MASK, spDefaultMask());
  g_spEnableMask &= spValidMask();
  g_enableRamazanAll = prefs.getBool(NVS_KEY_SP_RAM, (EN_RAMAZAN_TUM_GUNLER ? true : false));
}

// =====================
// Dini gün tarih override (Web üzerinden Hicri gün/ay/yıl seçimi)
// - useDefault=1: main.cpp'deki sabit (g_specials[]) kuralı
// - useDefault=0: kullanıcı override ettiği hicri gün/ay/yıl ile eşleşme aranır
// =====================
static const uint8_t SP_OVERRIDE_VER = 1;
static const char*   NVS_KEY_SP_OV_VER  = "spOvVer";
static const char*   NVS_KEY_SP_OV_BLOB = "spOvBlob";

static const int HICRI_YEAR_MIN = 1447;
static const int HICRI_YEAR_MAX = 1461;

struct __attribute__((packed)) SpOverride {
  uint8_t  useDefault; // 1=default (main.cpp), 0=custom
  uint8_t  day;        // 1..30
  uint8_t  month;      // 0..11 (Hicri month index)
  uint16_t year;       // 1447..1461 (custom için)
};

static SpOverride g_spOv[SPECIAL_COUNT];

static String normMonthKey(const String& in);

static int monthIndexFromKey(const String& inKey) {
  String k = normMonthKey(inKey);
  if (k == "muharrem") return 0;
  if (k == "safer") return 1;
  if (k == "rebiulevvel") return 2;
  if (k == "rebiulahir") return 3;
  if (k == "cemaziyelevvel") return 4;
  if (k == "cemaziyelahir") return 5;
  if (k == "receb") return 6;
  if (k == "saban") return 7;
  if (k == "ramazan") return 8;
  if (k == "sevval") return 9;
  if (k == "zilkade") return 10;
  if (k == "zilhicce") return 11;
  return -1;
}

static const char* monthKeyFromIndex(uint8_t idx) {
  switch (idx) {
    case 0:  return "muharrem";
    case 1:  return "safer";
    case 2:  return "rebiulevvel";
    case 3:  return "rebiulahir";
    case 4:  return "cemaziyelevvel";
    case 5:  return "cemaziyelahir";
    case 6:  return "receb";
    case 7:  return "saban";
    case 8:  return "ramazan";
    case 9:  return "sevval";
    case 10: return "zilkade";
    case 11: return "zilhicce";
    default: return "ramazan";
  }
}

static void spOverrideDefaults() {
  for (uint8_t i = 0; i < SPECIAL_COUNT; i++) {
    int mi = monthIndexFromKey(g_specials[i].monthKey);
    if (mi < 0) mi = 0;
    g_spOv[i].useDefault = 1;
    g_spOv[i].day  = g_specials[i].day;
    g_spOv[i].month = (uint8_t)mi;
    g_spOv[i].year = 0;
  }
}

static bool saveSpecialOverrideToNvs(String* errOut = nullptr) {
  size_t a = prefs.putUChar(NVS_KEY_SP_OV_VER, SP_OVERRIDE_VER);
  size_t b = prefs.putBytes(NVS_KEY_SP_OV_BLOB, g_spOv, (size_t)SPECIAL_COUNT * sizeof(SpOverride));
  if (a == 0 || b == 0) {
    if (errOut) *errOut = "nvs_write_fail";
    return false;
  }
  return true;
}

static void loadSpecialOverrideFromNvs() {
  uint8_t ver = prefs.getUChar(NVS_KEY_SP_OV_VER, 0);
  if (ver != SP_OVERRIDE_VER) {
    spOverrideDefaults();
    (void)saveSpecialOverrideToNvs();
    return;
  }

  size_t need = (size_t)SPECIAL_COUNT * sizeof(SpOverride);
  size_t got  = prefs.getBytes(NVS_KEY_SP_OV_BLOB, g_spOv, need);
  if (got != need) {
    spOverrideDefaults();
    (void)saveSpecialOverrideToNvs();
    return;
  }

  // sanity
  for (uint8_t i = 0; i < SPECIAL_COUNT; i++) {
    if (g_spOv[i].useDefault != 0 && g_spOv[i].useDefault != 1) g_spOv[i].useDefault = 1;
    if (g_spOv[i].day < 1 || g_spOv[i].day > 30) g_spOv[i].day = g_specials[i].day;
    if (g_spOv[i].month > 11) {
      int mi = monthIndexFromKey(g_specials[i].monthKey);
      if (mi < 0) mi = 0;
      g_spOv[i].month = (uint8_t)mi;
    }
    if (g_spOv[i].year != 0) {
      if (g_spOv[i].year < HICRI_YEAR_MIN || g_spOv[i].year > HICRI_YEAR_MAX) g_spOv[i].year = HICRI_YEAR_MIN;
    }
  }
}



static inline bool isSpecialEnabled(uint8_t idx) {
  if (idx >= SPECIAL_COUNT) return false;
  return ((g_spEnableMask >> idx) & 1u) != 0;
}

WiFiClientSecure tgClient;
UniversalTelegramBot* _pBot = nullptr;
#define bot (*_pBot)

static void reinitBot(const String& token) {
  if (token == g_botToken && _pBot) return; // Aynı token, gereksiz new/delete yapma
  g_botToken = token;
  if (_pBot) delete _pBot;
  _pBot = new UniversalTelegramBot(token, tgClient);
}

// =====================
// Web Panel (HTTP)
// =====================
// WEB_KEY runtime: g_webKey (secrets öncelikli, secrets boşsa NVS)
static WebServer*  g_web = nullptr;

// HTTP için TLS client artık httpGetPayload() içinde lokal olarak oluşturuluyor.

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

// ZORUNLU OFF: bir sonraki İmsak - SabahTolerans
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
static uint32_t g_wifiRetryIntervalMs = 15000;
static const uint32_t WIFI_RETRY_MAX_MS = 120000;

// Boot ve kullanıcı aktivitesi (ağdan büyük işlemleri geciktirmek için)
static uint32_t g_bootStartMs = 0;
static uint32_t g_lastUserActivityMs = 0;

// ===== /guncelle güvenli worker =====
static bool     g_updatePending = false;
static bool     g_updateInProgress = false;
static uint32_t g_updateCooldownUntilMs = 0;
static String   g_updateRequesterWho = "";

// ===== Web OTA (dosya yukleme) =====
// Upload esnasinda ag islerini hafifletmek (TG/HTTP) icin kullanilir.
static bool     g_webOtaInProgress = false;

// NVS: Telegram last update_id
static const char* NVS_KEY_TG_LAST = "tgLast"; // int64

// ===== Telegram panel/menu state =====
static int    g_panelMsgId = 0;  // panel mesaj id (edit için)
static String g_mainBottom = ""; // ana menü alt bilgi alanı

// UI işlemlerini (panel edit/gönder) Telegram callback içinde değil,
// döngüde ayrı bir "worker" ile yapmak daha stabil (kitlenme riskini azaltır).
static bool   g_uiRefreshPending = false;

// Otomatik menü: her reboot / yeniden bağlantıda panel göster
static bool     g_autoMenuPending = true;
static uint32_t g_lastAutoMenuMs  = 0;
static const uint32_t AUTO_MENU_COOLDOWN_MS = 60000; // 60sn (WiFi flapping spam önleme)

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

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// millis() taşma güvenli karşılaştırma
static inline bool millisPassed(uint32_t deadlineMs) {
  return (deadlineMs != 0) && ((int32_t)(millis() - deadlineMs) >= 0);
}

static void loadOffsetsFromNvs() {
  int on  = prefs.getInt(NVS_KEY_ONOFFS,  DEFAULT_ON_OFFSET_SEC);
  int off = prefs.getInt(NVS_KEY_OFFOFFS, DEFAULT_OFF_OFFSET_SEC);

  // 0..1800 sn (0..30 dk)
  g_onOffsetSec  = clampInt(on,  0, 1800);
  g_offOffsetSec = clampInt(off, 0, 1800);
}

static bool saveOffsetsToNvs(String* errOut = nullptr) {
  size_t a = prefs.putInt(NVS_KEY_ONOFFS,  g_onOffsetSec);
  size_t b = prefs.putInt(NVS_KEY_OFFOFFS, g_offOffsetSec);
  if (a == 0 || b == 0) {
    if (errOut) *errOut = "nvs_write_fail";
    return false;
  }
  return true;
}

// =====================
// Ağ/İlçe yardımcıları
// =====================
static uint32_t packIp4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}
static uint32_t ipToU32(const IPAddress& ip) {
  return packIp4(ip[0], ip[1], ip[2], ip[3]);
}
static IPAddress u32ToIp(uint32_t v) {
  return IPAddress((uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)(v));
}


static String makeBaseUrl(IPAddress ip, uint16_t port) {
  String u = String("http://") + ip.toString();
  if (port != 80) u += ":" + String((int)port);
  return u;
}
static String makeBaseUrlForCurrent() {
  IPAddress ip = (g_netCfg.useStatic && g_netCfg.ip != 0) ? u32ToIp(g_netCfg.ip) : WiFi.localIP();
  return makeBaseUrl(ip, g_httpPort);
}
static void scheduleRestart(uint32_t afterMs, const String& why) {
  g_restartAtMs = millis() + afterMs;
  g_restartWhy  = why;
}

static void loadNetFromNvs() {
  g_netCfg.useStatic = prefs.getBool(NVS_KEY_NET_USE, false);
  g_netCfg.ip   = prefs.getUInt(NVS_KEY_NET_IP,   0);
  g_netCfg.gw   = prefs.getUInt(NVS_KEY_NET_GW,   0);
  g_netCfg.mask = prefs.getUInt(NVS_KEY_NET_MASK, 0);
  g_netCfg.dns1 = prefs.getUInt(NVS_KEY_NET_DNS1, 0);
  g_netCfg.dns2 = prefs.getUInt(NVS_KEY_NET_DNS2, 0);

  // temel doğrulama (tamamı dolu değilse statik ip'i iptal et)
  if (g_netCfg.useStatic) {
    if (g_netCfg.ip == 0 || g_netCfg.gw == 0 || g_netCfg.mask == 0) {
      g_netCfg.useStatic = false;
    }
  }
}

static bool saveNetToNvs(String* errOut = nullptr) {
  size_t a = prefs.putBool(NVS_KEY_NET_USE, g_netCfg.useStatic);
  size_t b = prefs.putUInt(NVS_KEY_NET_IP,   g_netCfg.ip);
  size_t c = prefs.putUInt(NVS_KEY_NET_GW,   g_netCfg.gw);
  size_t d = prefs.putUInt(NVS_KEY_NET_MASK, g_netCfg.mask);
  size_t e = prefs.putUInt(NVS_KEY_NET_DNS1, g_netCfg.dns1);
  size_t f = prefs.putUInt(NVS_KEY_NET_DNS2, g_netCfg.dns2);
  if (a == 0 || b == 0 || c == 0 || d == 0 || e == 0 || f == 0) {
    if (errOut) *errOut = "nvs_write_fail";
    return false;
  }
  return true;
}

static void loadIlceFromNvs() {
  uint32_t id = prefs.getUInt(NVS_KEY_ILCE, 9206);
  if (id < 1 || id > 999999) id = 9206;
  g_ilceId = id;
}
static bool saveIlceToNvs(String* errOut = nullptr) {
  if (g_ilceId < 1 || g_ilceId > 999999) g_ilceId = 9206;
  size_t a = prefs.putUInt(NVS_KEY_ILCE, g_ilceId);
  if (a == 0) {
    if (errOut) *errOut = "nvs_write_fail";
    return false;
  }
  return true;
}


static void loadHttpPortFromNvs() {
  uint32_t p = prefs.getUInt(NVS_KEY_HTTP_PORT, 80);
  if (p < 1 || p > 65535) p = 80;
  g_httpPort = (uint16_t)p;
}
static bool saveHttpPortToNvs(uint16_t port, String* errOut = nullptr) {
  if (port < 1 || port > 65535) port = 80;
  size_t a = prefs.putUInt(NVS_KEY_HTTP_PORT, (uint32_t)port);
  if (a == 0) {
    if (errOut) *errOut = "nvs_write_fail";
    return false;
  }
  g_httpPort = (uint16_t)port;
  return true;
}


static void applyWiFiNetCfgIfNeeded() {
  if (!g_netCfg.useStatic) return;

  IPAddress ip   = u32ToIp(g_netCfg.ip);
  IPAddress gw   = u32ToIp(g_netCfg.gw);
  IPAddress mask = u32ToIp(g_netCfg.mask);

  IPAddress dns1 = (g_netCfg.dns1 != 0) ? u32ToIp(g_netCfg.dns1) : gw;
  IPAddress dns2 = (g_netCfg.dns2 != 0) ? u32ToIp(g_netCfg.dns2) : dns1;

  // WiFi.begin'den ÖNCE çağrılmalı
  bool ok = WiFi.config(ip, gw, mask, dns1, dns2);
  Serial.print("[NET] Static IP config: ");
  Serial.print(ok ? "OK " : "FAIL ");
  Serial.print(ip); Serial.print(" gw="); Serial.print(gw);
  Serial.print(" mask="); Serial.print(mask);
  Serial.print(" dns1="); Serial.print(dns1);
  Serial.print(" dns2="); Serial.println(dns2);
}

// =====================
// Secrets.h -> NVS fallback (WiFi + WEB_KEY)
// - secrets doluysa her zaman secrets kullanılır
// - secrets boşsa NVS'deki değerler kullanılır (yoksa boş kabul edilir)
// =====================
static inline bool isEmptyCstr(const char* s) { return (!s) || (s[0] == '\0'); }

static void loadWiFiWebKeyFromNvsFallback() {
  // WiFi
  if (isEmptyCstr(SECRET_WIFI_SSID)) {
    String ns = prefs.getString(NVS_KEY_WIFI_SSID, "");
    String np = prefs.getString(NVS_KEY_WIFI_PASS, "");
    ns.trim();
    np.trim();
    if (ns.length() > 0) {
      g_wifiSsid = ns;
      g_wifiPass = np;
      g_wifiSrc  = "nvs";
    } else {
      g_wifiSsid = "";
      g_wifiPass = "";
      g_wifiSrc  = "none";
    }
  } else {
    g_wifiSsid = String(SECRET_WIFI_SSID);
    g_wifiPass = String(SECRET_WIFI_PASS);
    g_wifiSrc  = "secrets";
  }

  // WEB_KEY
  if (isEmptyCstr(SECRET_WEB_KEY)) {
    String wk = prefs.getString(NVS_KEY_WEB_KEY, "");
    wk.trim();
    g_webKey = wk;
    g_webKeySrc = (wk.length() ? "nvs" : "none");
  } else {
    g_webKey = String(SECRET_WEB_KEY);
    g_webKeySrc = "secrets";
  }
}

// NVS bot token / chat id fallback (NVS varsa secrets yerine kullan)
static void loadBotTokenChatIdFromNvs() {
  // BOT_TOKEN: NVS varsa NVS, yoksa secrets.h
  String nvsToken = prefs.getString(NVS_KEY_BOT_TOKEN, "");
  nvsToken.trim();
  if (nvsToken.length() > 0) {
    reinitBot(nvsToken);
    g_botTokenSrc = "nvs";
  } else {
    g_botTokenSrc = "secrets";
  }

  // CHAT_ID: NVS varsa NVS, yoksa secrets.h
  String nvsChatId = prefs.getString(NVS_KEY_CHAT_ID, "");
  nvsChatId.trim();
  if (nvsChatId.length() > 0) {
    g_activeChatId = nvsChatId;
    g_chatIdSrc = "nvs";
  } else {
    g_activeChatId = String(CHAT_ID);
    g_chatIdSrc = "secrets";
  }
}

static String getWiFiSsidForUi() {
  if (WiFi.status() == WL_CONNECTED) {
    String s = WiFi.SSID();
    if (s.length()) return s;
  }
  return g_wifiSsid;
}

static void wifiBeginNow() {
  if (g_wifiSsid.length() == 0) {
    Serial.println("[WIFI] SSID bos (secrets + NVS). WiFi baslatilmadi.");
    return;
  }
  applyWiFiNetCfgIfNeeded();
  WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());
}

// =====================
// Telegram yardımcı: TLS hazırlık (tek seferlik)
// =====================
static bool g_tgInsecureSet = false;
static void tgPrepare(uint16_t timeoutMs = 12000) {
  if (!g_tgInsecureSet) {
    tgClient.setInsecure();
    tgClient.setHandshakeTimeout(10); // TLS handshake timeout 10sn (default 0=infinite)
    g_tgInsecureSet = true;
  }
  tgClient.setTimeout(timeoutMs);
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

  tgPrepare();
  bool ok = bot.sendMessage(g_activeChatId, msg, "");
  if (ok) {
    g_lastTgMsg = msg;
    g_lastTgMsgMs = nowMs;
  }
}

static bool tgSendTo(const String& cid, const String& msg) {
  if (WiFi.status() != WL_CONNECTED) return false;
  tgPrepare();
  return bot.sendMessage(cid, msg, "");
}

static void logSerialAndTg(const String& msg, bool notifyTg = true, bool forceTg = false) {
  String ts = nowStamp();
  // Tek allocasyon + reserve ile heap fragmantasyonu azalt
  String logLine;
  logLine.reserve(ts.length() + msg.length() + 6);
  logLine = "[";
  logLine += ts;
  logLine += "] ";
  logLine += msg;

  Serial.println(logLine);
  if (notifyTg) tgSend(logLine, forceTg);
}

// =====================
// Dış IP sorgu
// =====================
static String fetchExternalIp() {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  http.setTimeout(3000); // 3sn timeout (boot gecikmesini azalt)
  http.begin("http://api.ipify.org");
  int code = http.GET();
  String ip = "";
  if (code == 200) ip = http.getString();
  http.end();
  ip.trim();
  return ip;
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

// Dakika -> "HH:MM" string
static String minToHhmm(uint16_t m) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", m / 60, m % 60);
  return String(buf);
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
// Aktif chat (panel hedefi) NVS
// =====================
static void loadActiveChatFromNvs() {
  String s = prefs.getString(NVS_KEY_ACTIVE_CHAT, "");
  s.trim();
  if (s.length() > 0) g_activeChatId = s;
  else g_activeChatId = String(CHAT_ID);
}

static void saveActiveChatToNvs() {
  if (g_activeChatId.length() == 0) g_activeChatId = String(CHAT_ID);
  prefs.putString(NVS_KEY_ACTIVE_CHAT, g_activeChatId);
}

static bool isActiveChatId(const String& cid) {
  return (cid == g_activeChatId);
}

// =====================
// HTTP: tüm payload al (global TLS client)
// =====================
static bool httpGetPayload(const String& url, String& payloadOut, int& httpCodeOut) {
  if (WiFi.status() != WL_CONNECTED) return false;

  // Lokal TLS client kullanarak global Telegram client ile çakışma riskini engelle
  WiFiClientSecure localClient;
  localClient.setInsecure();

  HTTPClient http;
  http.setTimeout(25000);
  http.setReuse(false);
  http.useHTTP10(true);

  if (!http.begin(localClient, url)) return false;

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

  String url = String(EZAN_API) + "/vakitler/" + String(g_ilceId);

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

  PsramJsonDocument doc(64 * 1024); // PSRAM'da allocate (ana heap'i korur)
  if (doc.capacity() == 0) {
    logSerialAndTg("❌ JSON buffer alloc FAIL (heap yetersiz). Free=" + String((int)ESP.getFreeHeap()), notifyTg, true);
    return false;
  }
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

    a = epochFromYmdAndMin(yThu, g_days[iThu].aksamMin) + (time_t)g_onOffsetSec;
    b = epochFromYmdAndMin(yFri, g_days[iFri].imsakMin) - (time_t)g_offOffsetSec;
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
// ZORUNLU OFF: bir sonraki imsak - SabahTolerans
// =====================
static bool computeNextImsakOff(time_t now, time_t& nextOffTs) {
  if (!isTimeValid() || g_dayCount == 0) return false;

  tm t{}; localtime_r(&now, &t);
  uint32_t today = ymdFromTm(t);

  int iToday = findIdx(today);
  if (iToday < 0) return false;

  time_t offToday = epochFromYmdAndMin(today, g_days[iToday].imsakMin) - (time_t)g_offOffsetSec;
  if (now < offToday) { nextOffTs = offToday; return true; }

  uint32_t tomorrow = addDaysYmd(today, +1);
  int iTom = findIdx(tomorrow);
  if (iTom < 0) return false;

  nextOffTs = epochFromYmdAndMin(tomorrow, g_days[iTom].imsakMin) - (time_t)g_offOffsetSec;
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
    logSerialAndTg(String("⛔ ZORUNLU OFF (İmsak - Sabah tolerans): ") + ts + "  (manuel iptal)", true, true);
    if (wasOn) logSerialAndTg("🔕 ROLE: OFF (Zorunlu)", true, true);
    if (wasOn) logSys("Imsak zorunlu OFF");

    time_t newOff = 0;
    if (computeNextImsakOff(now + 2, newOff)) g_nextImsakOffTs = newOff;
    else g_nextImsakOffTs = 0;
  }
}

// =====================
// WiFi keep-alive + watchdog
// =====================
static uint32_t g_wifiDisconnectedSinceMs = 0; // WiFi koptuğundaki millis
static const uint32_t WIFI_WATCHDOG_MS = 600000; // 10dk bağlanamazsa restart

static void wifiKeepAlive() {
  wl_status_t st = WiFi.status();

  if (st != g_lastWiFi) {
    g_lastWiFi = st;
    if (st == WL_CONNECTED) {
      logSerialAndTg("📶 WiFi BAGLANDI. IP=" + WiFi.localIP().toString(), false, false);
      g_autoMenuPending = true;
      // İlk bağlantıda Telegram'a boot mesajı gönder (IP sonra gelir)
      static bool _bootIpSent = false;
      static bool _extIpPending = false;
      if (!_bootIpSent) {
        _bootIpSent = true;
        esp_reset_reason_t br = esp_reset_reason();
        String bootMsg = "🚀 Sistem basladi\n📡 LAN: " + WiFi.localIP().toString();
        if (br == ESP_RST_TASK_WDT) bootMsg += "\n⚠️ Onceki kapanma: Watchdog";
        else if (br == ESP_RST_PANIC) bootMsg += "\n⚠️ Onceki kapanma: Panic";
        bootMsg += "\n🔖 " + String(APP_VERSION) + " (" + BOARD_NAME + ")";
        tgSend(bootMsg, true);
        logSys("WiFi baglandi, IP=" + WiFi.localIP().toString());
        _extIpPending = true; // Dış IP'yi sonra al
      }
      // Dış IP'yi ayrı adımda al (boot mesajını bloklama)
      if (_extIpPending) {
        _extIpPending = false;
        String extIp = fetchExternalIp();
        if (extIp.length() > 0) tgSend("🌐 Dis IP: " + extIp, true);
      }
      g_wifiRetryIntervalMs = 15000; // backoff sıfırla
      g_wifiDisconnectedSinceMs = 0; // watchdog sıfırla
    } else {
      logSerialAndTg("📶 WiFi KOPTU (status=" + String((int)st) + ")", false, false);
      logSys("WiFi koptu");
      if (g_wifiDisconnectedSinceMs == 0) g_wifiDisconnectedSinceMs = millis();
    }
  }

  if (st == WL_CONNECTED) return;

  // WiFi watchdog: çok uzun süredir bağlanamıyorsa cihazı yeniden başlat
  if (g_wifiDisconnectedSinceMs != 0 && (millis() - g_wifiDisconnectedSinceMs) > WIFI_WATCHDOG_MS) {
    Serial.println("[WIFI] Watchdog: 10dk boyunca baglanilamadi, ESP restart...");
    logSys("WiFi watchdog: 10dk baglanti yok, reboot");
    delay(200);
    ESP.restart();
  }

  uint32_t nowMs = millis();
  if (nowMs - g_lastWiFiTryMs < g_wifiRetryIntervalMs) return;
  g_lastWiFiTryMs = nowMs;

  // Exponential backoff (max 120sn)
  if (g_wifiRetryIntervalMs < WIFI_RETRY_MAX_MS) {
    g_wifiRetryIntervalMs = (g_wifiRetryIntervalMs * 3) / 2;
    if (g_wifiRetryIntervalMs > WIFI_RETRY_MAX_MS) g_wifiRetryIntervalMs = WIFI_RETRY_MAX_MS;
  }

  WiFi.disconnect();
  applyWiFiNetCfgIfNeeded();
  wifiBeginNow();
  logSerialAndTg("🔁 WiFi yeniden baglanma denemesi... (sonraki: " + String(g_wifiRetryIntervalMs/1000) + "sn)", true);
}

// =====================
// Bugün cache'de yoksa online ise çek
// =====================
static void ensureTodayInCache() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!isTimeValid()) return;

  // NTP senkronizasyon logu (bir kez)
  static bool _ntpLogged = false;
  if (!_ntpLogged) {
    _ntpLogged = true;
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
    logSys("NTP senkronize: " + String(buf));

    // Güç kesintisi tespiti
    uint32_t lastAlive = prefs.getULong(NVS_KEY_LAST_ALIVE, 0);
    time_t nowEpoch = time(nullptr);
    if (lastAlive > 0 && nowEpoch > (time_t)lastAlive) {
      uint32_t outSec = (uint32_t)(nowEpoch - lastAlive);
      if (outSec > 120) { // 2dk'dan uzun kesinti anlamlı
        uint32_t outMin = outSec / 60;
        // Son çalışma zamanını formatla
        time_t la = (time_t)lastAlive;
        struct tm* lt = localtime(&la);
        char laBuf[20];
        snprintf(laBuf, sizeof(laBuf), "%02d:%02d", lt->tm_hour, lt->tm_min);
        String outMsg = "⚡ Guc kesintisi tespit edildi\n⏱️ Son calisma: " + String(laBuf) +
                        "\n⏱️ Kesinti suresi: ~" + String(outMin) + " dk";
        tgSend(outMsg, true);
        logSys("Guc kesintisi: ~" + String(outMin) + "dk");
      }
    }
    // İlk alive kaydı
    prefs.putULong(NVS_KEY_LAST_ALIVE, (uint32_t)nowEpoch);
  }

  // Boot'tan hemen sonra veya kullanıcı yeni etkileşimdeyken
  // büyük HTTP/JSON işlemlerini başlatma (Telegram komutlarına geç tepki olmasın).
  uint32_t nowMs = millis();
  if (g_bootStartMs != 0 && (nowMs - g_bootStartMs) < 20000) return;            // ilk 20sn dokunma
  if (g_lastUserActivityMs != 0 && (nowMs - g_lastUserActivityMs) < 15000) return; // son 15sn'de kullanıcı varsa dokunma
  if (g_updateInProgress || g_updatePending) return;

  if (g_dayCount == 0 || !hasYmd(ymdToday())) {
    logSerialAndTg("📥 Cache bugunu icermiyor -> vakit indiriliyor...", false, false);
    logSys("Otomatik vakit indirme basladi");
    // Arka planda çekimde Telegram'a ekstra mesaj atma; sadece serial/log.
    if (fetchAndStoreMonthly(false)) loadTimesFromNvs();
  }
}

// =====================
// Özel gün penceresi
// =====================
static bool computeWindowForSpecial(uint8_t spIdx, time_t& onTs, time_t& offTs, uint32_t& ymdEvent, String& hicriText) {
  if (g_dayCount == 0) return false;
  if (spIdx >= SPECIAL_COUNT) return false;

  const SpecialDef& sp = g_specials[spIdx];
  bool useDef = (g_spOv[spIdx].useDefault != 0);

  int    ruleDay  = useDef ? (int)sp.day : (int)g_spOv[spIdx].day;
  String ruleMon  = useDef ? String(sp.monthKey) : String(monthKeyFromIndex(g_spOv[spIdx].month));
  ruleMon = normMonthKey(ruleMon);
  int    ruleYear = useDef ? 0 : (int)g_spOv[spIdx].year;

  int foundIdx = -1;

  for (uint16_t i = 0; i < g_dayCount; i++) {
    int hd=0, hy=0; String hm;
    if (!parseHicri(g_days[i].hicriUzun, hd, hm, hy)) continue;

    String key = normMonthKey(hm);
    if (hd == ruleDay && key == ruleMon) {
      // Custom seçildiyse yıl da eşleşsin (API yıl vermezse ay+gün ile kabul)
      if (ruleYear != 0) {
        if (hy != 0 && hy != ruleYear) continue;
      }
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

  onTs  = epochFromYmdAndMin(ymdPrev,  g_days[ip].aksamMin) + (time_t)g_onOffsetSec;
  offTs = epochFromYmdAndMin(ymdEvent, g_days[foundIdx].imsakMin) - (time_t)g_offOffsetSec;

  return (offTs > onTs);
}

static bool buildWindowForEventIdx(int idxEvent, time_t& onTs, time_t& offTs) {
  if (idxEvent < 0 || idxEvent >= (int)g_dayCount) return false;
  uint32_t ymdEvent = g_days[idxEvent].ymd;
  uint32_t ymdPrev  = addDaysYmd(ymdEvent, -1);

  int ip = findIdx(ymdPrev);
  if (ip < 0) return false;

  onTs  = epochFromYmdAndMin(ymdPrev,  g_days[ip].aksamMin) + (time_t)g_onOffsetSec;
  offTs = epochFromYmdAndMin(ymdEvent, g_days[idxEvent].imsakMin) - (time_t)g_offOffsetSec;

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
    if (!isSpecialEnabled(i)) continue;

    time_t on=0, off=0; uint32_t ymdEv=0; String hicri;
    if (!computeWindowForSpecial(i, on, off, ymdEv, hicri)) continue;

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

  if (g_enableRamazanAll) {
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
  }

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
    msg += "Kural: Akşam + Akşam tolerans -> İmsak - Sabah tolerans";

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
    logSys("Haftalik vakit guncelleme basladi");
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
// Haftalık otomatik restart (Salı 04:00)
// =====================
static void weeklyRestartTick() {
  if (!isTimeValid()) return;
  static uint32_t lastCheckMs = 0;
  if (millis() - lastCheckMs < 60000) return; // Dakikada bir kontrol yeterli
  lastCheckMs = millis();
  static uint32_t lastCheckDay = 0;
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  uint32_t ymd = (uint32_t)((t->tm_year+1900)*10000 + (t->tm_mon+1)*100 + t->tm_mday);

  // Salı (tm_wday==2), saat 04:00-04:05 arası, günde bir kez
  if (t->tm_wday == 2 && t->tm_hour == 4 && t->tm_min < 5 && ymd != lastCheckDay) {
    lastCheckDay = ymd;
    // OTA veya güncelleme devam ediyorsa atlat
    if (g_webOtaInProgress || g_updateInProgress) {
      Serial.println("[MAINT] Haftalik restart ertelendi (OTA/guncelleme aktif)");
      return;
    }
    Serial.println("[MAINT] Haftalik otomatik restart...");
    logSys("Haftalik otomatik restart");
    tgSend("🔄 Haftalık bakım: otomatik yeniden başlatma", true);
    delay(500);
    ESP.restart();
  }
}

// =====================
// Birleşik röle kontrolü (tüm kaynaklar için tek nokta)
// source: "BUTON", "TG", "TG Panel", "WEB"
// who: kullanıcı bilgisi (Telegram'dan gelen ad)
// Dönüş: 0=OK, 1=blocked (imsak), 2=zaten açık (buton için)
// =====================
static int manualRelayOn(const char* source, const String& who = "") {
  g_manualOffUntilTs = 0;

  // İmsak OFF sonrası engel kontrolü
  if (isTimeValid()) {
    time_t noff = 0;
    if (computeNextImsakOff(time(nullptr), noff)) g_nextImsakOffTs = noff;
    if (g_blockOnUntilTs != 0 && time(nullptr) < g_blockOnUntilTs) {
      return 1; // blocked
    }
  }

  bool wasOff = !g_relayState;
  g_manualOnLatched = true;
  relayWrite(true);

  // Log
  String src(source);
  String whoStr = who.length() > 0 ? (" (" + who + ")") : "";
  if (wasOff) {
    logSerialAndTg("🟢 " + src + ": Manual ON" + whoStr, true, true);
    logUser(src + ": Role ACILDI" + whoStr);
  }
  return wasOff ? 0 : 2; // 0=açıldı, 2=zaten açıktı
}

// Dönüş: untilTs (0 = pencere yok)
static time_t manualRelayOff(const char* source, const String& who = "") {
  time_t untilTs = 0;
  if (isTimeValid()) {
    bool thuOn = false, spOn = false; time_t schedOff = 0;
    getScheduleState(time(nullptr), thuOn, spOn, schedOff);
    if (schedOff) untilTs = schedOff;
  }
  g_manualOffUntilTs = untilTs;
  g_manualOnLatched = false;
  relayWrite(false);

  String src(source);
  String whoStr = who.length() > 0 ? (" (" + who + ")") : "";
  logSerialAndTg("🔴 " + src + ": Manual OFF" + whoStr, true, true);
  logUser(src + ": Role KAPANDI" + whoStr);
  return untilTs;
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
        int rc = manualRelayOn("BUTON");
        if (rc == 2) logSerialAndTg("🟢 BUTON: Manual ON (zaten ON)", true);
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
// /dinigunler için hafif liste
// =====================
enum ItemKind : uint8_t { KIND_SPECIAL=0, KIND_RAMAZAN=1 };

struct SpItemLite {
  uint32_t ymd;          // Miladi event günü
  time_t on;
  time_t off;
  uint8_t stateGroup;    // 0=aktif, 1=yaklaşan, 2=geçmiş
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
  if (g_updateCooldownUntilMs != 0 && !millisPassed(g_updateCooldownUntilMs)) return;

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
    logSys("Vakit guncelleme OK");
  } else {
    tgSend("[" + nowStamp() + "] ❌ Manuel guncelleme FAIL\n👤 " + g_updateRequesterWho, true);
    logSys("Vakit guncelleme FAIL");
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
    logSys(shouldOn ? "Cizelge: Role ACILDI" : "Cizelge: Role KAPANDI");
  }
}

// =====================
// Yardımcı: schedule hesaplarını yenile
// =====================
static void recomputeAllSchedules() {
  if (!isTimeValid()) return;

  time_t now = time(nullptr);

  time_t on2=0, off2=0;
  if (!computeThuFriWindowForNow(now, on2, off2)) {
    ensureTodayInCache();
    (void)computeThuFriWindowForNow(now, on2, off2);
  }
  if (on2 && off2) { g_thuOnTs = on2; g_thuOffTs = off2; }

  computeNextSpecial(now);

  time_t noff=0;
  if (!computeNextImsakOff(now, noff)) {
    ensureTodayInCache();
    (void)computeNextImsakOff(now, noff);
  }
  if (noff) g_nextImsakOffTs = noff;

  applyRelayLogic();
}

// =====================
// Panel/Menu: metin + keyboard üretimi
// =====================
static String fmtMin(int sec) {
  int m = sec / 60;
  return String(m) + " dk";
}

static String buildPanelTextMain() {
  String s;
  s.reserve(300);
  s += "🕌 Cami Panel\n";
  s += "📌 Şerefeler: " + String(g_relayState ? "AÇIK ✅" : "KAPALI ❌") + "\n";
  s += "⏱️ Akşam tolerans: " + fmtMin(g_onOffsetSec) + " | Sabah tolerans: " + fmtMin(g_offOffsetSec) + "\n";
  if (isTimeValid()) s += "🕰️ " + nowStamp() + "\n";
  else s += "🕰️ NO_TIME\n";
  s += "\nSeçim yap:";

  // Alt bilgi alanı (kısa not / son işlem)
  if (g_mainBottom.length() > 0) {
    s += "\n\n────────────\n";
    s += g_mainBottom;
  }
  return s;
}

static void panelShowOrEdit(const String& text, const String& kbJson, int messageId);

static String kbMain() {
  String toggleTxt = g_relayState ? "Şerefeler: KAPAT ❌" : "Şerefeler: AÇ ✅";

  // Basitleştirilmiş ana menü:
  // - Şerefeler Toggle
  // - Yenile
  String kb = "[[";
  kb += "{ \"text\":\"" + toggleTxt + "\", \"callback_data\":\"TOGGLE\" }";
  kb += "],";
  kb += "[{ \"text\":\"🔄 Yenile\", \"callback_data\":\"REFRESH\" }]";
  kb += "]";
  return kb;
}

static void panelShowOrEdit(const String& text, const String& kbJson, int messageId) {
  // Telegram HTTPS istekleri bazen uzun sürebiliyor.
  // WDT'yi tetiklememek için küçük yield'ler ve daha makul timeout kullanıyoruz.
  tgPrepare(8000);

  yield();

  bool ok = false;

  // 1) Önce mevcut paneli edit etmeyi dene
  if (messageId != 0) {
    ok = bot.sendMessageWithInlineKeyboard(g_activeChatId, text, "", kbJson, messageId); // edit
    yield();
    if (!ok) {
      // Panel mesajı silinmiş/geçersiz olabilir -> yeni mesaj göndereceğiz
      g_panelMsgId = 0;
    }
  }

  // 2) Edit olmadıysa (veya messageId=0 ise) yeni panel gönder
  if (messageId == 0 || !ok) {
    ok = bot.sendMessageWithInlineKeyboard(g_activeChatId, text, "", kbJson);
    yield();
    if (ok) {
      // Kütüphane son gönderilen mesajın id'sini buraya yazar
      g_panelMsgId = bot.last_sent_message_id;
    }
  }

  if (!ok) {
    // Son çare: kullanıcıya hata bilgisi bırak.
    bot.sendMessage(g_activeChatId, "❌ Menü açılamadı (Telegram API). /menu yazıp tekrar dene.", "");
  }
}

static void openMainPanel() {
  panelShowOrEdit(buildPanelTextMain(), kbMain(), g_panelMsgId);
}

static void requestUiRefresh() {
  g_uiRefreshPending = true;
}

static void uiRefreshTick() {
  if (!g_uiRefreshPending) return;
  if (g_updateInProgress)  return;
  if (WiFi.status() != WL_CONNECTED) return;

  // tek sefer çalıştır
  g_uiRefreshPending = false;
  openMainPanel();
}

// =====================
// Callback Query ACK (Telegram'da "bekliyor" spinner'ını hemen kapatır)
// =====================
static void cbAnswer(const String& qid, const String& msg, bool alert) {
  if (qid.length() == 0) return;
  tgPrepare(3000);
  yield();
  /*ACK*/ bot.answerCallbackQuery(qid, msg, alert);
}

// =====================
// Otomatik menü (reboot / WiFi yeniden bağlanınca)
// =====================
static void autoMenuTick() {
  if (!g_autoMenuPending) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t nowMs = millis();
  if (g_lastAutoMenuMs != 0 && (nowMs - g_lastAutoMenuMs) < AUTO_MENU_COOLDOWN_MS) return;

  openMainPanel();

  g_lastAutoMenuMs = nowMs;
  g_autoMenuPending = false;
}

// =====================
// Durum metni (panel ve /durum için)
// =====================
static String buildStatusText(const String& who) {
  char a[32], b[32], c[32], s1[32], s2[32], mo[32];
  formatDateTime(g_thuOnTs, a, sizeof(a));
  formatDateTime(g_thuOffTs, b, sizeof(b));
  formatDateTime(g_nextImsakOffTs, c, sizeof(c));
  formatDateTime(g_spOnTs, s1, sizeof(s1));
  formatDateTime(g_spOffTs, s2, sizeof(s2));
  formatDateTime(g_manualOffUntilTs, mo, sizeof(mo));

  bool thuOn=false, spOn=false; time_t schedOff=0;
  if (isTimeValid()) getScheduleState(time(nullptr), thuOn, spOn, schedOff);

  // Uptime hesapla
  uint32_t uptimeSec = millis() / 1000;
  uint32_t days = uptimeSec / 86400;
  uint32_t hours = (uptimeSec % 86400) / 3600;
  uint32_t mins  = (uptimeSec % 3600) / 60;
  char uptimeBuf[32];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%ud %uh %um", days, hours, mins);

  String msg;
  msg.reserve(700); // heap fragmantasyonu azalt
  msg += "📌 Durum (" + String(APP_VERSION) + ")\n";
  msg += "Role: "; msg += (g_relayState ? "ON" : "OFF"); msg += "\n";
  msg += "ManualLatch: "; msg += (g_manualOnLatched ? "ON" : "OFF"); msg += "\n";
  msg += "ManualOffOverrideUntil: "; msg += mo; msg += "\n";
  msg += "Akşam tolerans: "; msg += fmtMin(g_onOffsetSec); msg += "\n";
  msg += "Sabah tolerans: "; msg += fmtMin(g_offOffsetSec); msg += "\n";
  msg += "Persembe->Cuma ON: "; msg += a; msg += "\n";
  msg += "Persembe->Cuma OFF: "; msg += b; msg += "\n";
  if (g_spName.length() > 0) {
    msg += "Dini Gun ("; msg += g_spName; msg += ") ON: "; msg += s1; msg += "\n";
    msg += "Dini Gun ("; msg += g_spName; msg += ") OFF: "; msg += s2; msg += "\n";
    if (g_spHicri.length() > 0) { msg += "Hicri: "; msg += g_spHicri; msg += "\n"; }
  } else {
    msg += "Dini Gun: -\n";
  }
  msg += "Zorunlu OFF (İmsak - Sabah tolerans): "; msg += c; msg += "\n";
  msg += "DBG thuOn="; msg += (thuOn ? "1":"0"); msg += " spOn="; msg += (spOn ? "1":"0"); msg += "\n";
  msg += "Admins: "; msg += String((int)g_adminCount); msg += "\n";
  msg += "Uptime: "; msg += uptimeBuf; msg += "\n";
  msg += "Heap: "; msg += String((int)ESP.getFreeHeap()); msg += " / Min: "; msg += String((int)ESP.getMinFreeHeap()); msg += "\n";
  msg += "Sen: "; msg += who;

  return msg;
}

// =====================
// Dini günler listesi (tek string veya parçalı gönderim)
// =====================
// =====================
// Ortak: aktif/yaklaşan dini günlerin listesini g_spList'e doldur
// =====================
static int buildSpListSorted() {
  if (g_dayCount == 0) return 0;

  time_t now = isTimeValid() ? time(nullptr) : 0;
  bool tValid = isTimeValid();
  int cnt = 0;

  for (uint8_t k = 0; k < SPECIAL_COUNT && cnt < MAX_SP_LIST; k++) {
    if (!isSpecialEnabled(k)) continue;
    time_t on=0, off=0; uint32_t ymdEv=0; String hicri;
    if (!computeWindowForSpecial(k, on, off, ymdEv, hicri)) continue;

    uint8_t group = 2;
    if (tValid && now >= on && now < off) group = 0;
    else if (tValid && on > now)          group = 1;
    if (group == 2) continue;

    SpItemLite it{};
    it.ymd = ymdEv; it.on = on; it.off = off;
    it.kind = KIND_SPECIAL; it.specialIndex = k; it.stateGroup = group;
    g_spList[cnt++] = it;
  }

  if (g_enableRamazanAll) {
    for (int idx=0; idx<(int)g_dayCount && cnt < MAX_SP_LIST; idx++) {
      int dayNo=0;
      if (!isRamazanIdx(idx, dayNo)) continue;

      time_t on=0, off=0;
      if (!buildWindowForEventIdx(idx, on, off)) continue;

      uint8_t group = 2;
      if (tValid && now >= on && now < off) group = 0;
      else if (tValid && on > now)          group = 1;
      if (group == 2) continue;

      SpItemLite it{};
      it.ymd = g_days[idx].ymd; it.on = on; it.off = off;
      it.kind = KIND_RAMAZAN; it.ramazanDay = (uint8_t)dayNo; it.stateGroup = group;
      g_spList[cnt++] = it;
    }
  }

  if (cnt > 0) sortSpLite(g_spList, cnt);
  return cnt;
}

static String formatSpItem(const SpItemLite& item) {
  char ddmmyyyy[16];
  ymdToDdMmYyyy(item.ymd, ddmmyyyy, sizeof(ddmmyyyy));
  String st = (item.stateGroup==0) ? "🟢 AKTIF" : "🟡 YAKLASAN";
  String name;
  if (item.kind == KIND_SPECIAL) name = String(g_specials[item.specialIndex].name);
  else name = String("Ramazan Günü ") + String(item.ramazanDay);
  return String("✅ ") + st + " - " + name + " Miladi: " + String(ddmmyyyy) + "\n";
}

static void sendDiniGunlerList() {
  int cnt = buildSpListSorted();
  if (cnt == 0) { tgSendTo(g_activeChatId, "Yakın tarihte dini gün yok"); return; }
  String out;
  for (int ii=0; ii<cnt; ii++) {
    out += formatSpItem(g_spList[ii]);
    if (out.length() > 3300) { tgSendTo(g_activeChatId, out); out = ""; }
  }
  if (out.length() > 0) tgSendTo(g_activeChatId, out);
}

static String buildDiniGunlerWebText() {
  if (!isTimeValid()) return "NO_TIME (NTP bekleniyor)";
  int cnt = buildSpListSorted();
  if (cnt == 0) return "Yakın tarihte dini gün yok";
  String out;
  out.reserve(cnt * 120);
  for (int ii=0; ii<cnt; ii++) {
    out += formatSpItem(g_spList[ii]);
    if (out.length() > 7500) { out += "…\n"; break; }
  }
  return out;
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

  int cycles = 0;
  int n = bot.getUpdates(bot.last_message_received + 1);
  while (n && cycles++ < 3) {
    for (int i = 0; i < n; i++) {
      String chat_id = bot.messages[i].chat_id;
      String text    = bot.messages[i].text;
      String who = whoStr(bot.messages[i]);
      int64_t uid = atoll(bot.messages[i].from_id.c_str());
      bool isAdminUser = isAdmin(uid);
      bool isOwnerUser = (uid == OWNER_ADMIN_ID);

      // Panel açma komutları (OWNER farklı sohbette kullanırsa aktif chat otomatik taşınır)
      bool cmdPanel = (cmdIs(text, "/start") || cmdIs(text, "/menu") || cmdIs(text, "/panel"));
      bool cmdPair  = (cmdIs(text, "/admin_menu")  || cmdIs(text, "/pair")  || cmdIs(text, "/eslestir") || cmdIs(text, "/eşleştir"));
      bool cmdMyId  = (cmdIs(text, "/myid"));

      // Aktif chat dışından sadece OWNER için: /myid ve (otomatik taşıma) /menu-/panel-/start veya /admin_menu izinli
      if (!isActiveChatId(chat_id)) {
        if (isOwnerUser && (cmdPanel || cmdPair)) {
          g_activeChatId = chat_id;
          saveActiveChatToNvs();
          g_panelMsgId = 0; // yeni chat'te yeni panel gönderilsin
          g_lastTgMsg = ""; g_lastTgMsgMs = 0;
          logSerialAndTg("📌 Aktif chat değişti -> " + g_activeChatId, false, false);
        } else if (isOwnerUser && cmdMyId) {
          // /myid her yerden çalışsın
        } else {
          continue;
        }
      }

      // Kullanıcı aktivitesi: arka plandaki ağır işlemleri (vakit indirme gibi) geciktirmek için
      g_lastUserActivityMs = millis();

      // Panel mesaj id'yi callback ile güncelle (edit stabil olsun)
      if (bot.messages[i].type == "callback_query") {
        g_panelMsgId = bot.messages[i].message_id; // int
        String qid = bot.messages[i].query_id;

        // ---- CALLBACK ACTIONS ----
        const String data = text; // callback_data

        if (data == "REFRESH") {
          cbAnswer(qid, "Yenileniyor…", false);
          requestUiRefresh();
        }
        else if (data == "BACK_MAIN") {
          cbAnswer(qid, "Ana menü", false);
          requestUiRefresh();
        }
        else if (data == "TOGGLE") {
          if (!isAdminUser) {
            cbAnswer(qid, "Yetkisiz", true);
          } else {
            cbAnswer(qid, g_relayState ? "Kapatılıyor…" : "Açılıyor…", false);

            if (g_relayState) {
              manualRelayOff("TG Panel", who);
            } else {
              int rc = manualRelayOn("TG Panel", who);
              if (rc == 1) {
                g_mainBottom = "⛔ Şu an ON engelli (Zorunlu OFF sonrası)";
              }
            }

            requestUiRefresh();
          }
        }
        else {
          cbAnswer(qid, "Bu menü kaldırıldı", false);
          requestUiRefresh();
        }
        continue; // callback işlendi
      }

      // ---- NORMAL MESSAGE COMMANDS ----

      if (cmdMyId) {
        String msg;
        msg += "👤 " + who + "\n";
        msg += "chat_id=" + chat_id + "\n";
        msg += "active_chat=" + g_activeChatId + "\n";
        msg += "admin=" + String(isAdminUser ? "1" : "0") + " owner=" + String(isOwnerUser ? "1" : "0");
        tgSendTo(chat_id, msg);
      }
      else if (cmdPair) {
        // Geri uyumluluk: /pair komutu artik /admin_menu olarak degisti
        bool usedLegacyPair = cmdIs(text, "/pair");
        if (usedLegacyPair && isOwnerUser) {
          tgSendTo(chat_id, "ℹ️ Komut degisti: /pair yerine /admin_menu kullan.\nYine de eslestirme yapildi.");
        }
        if (!isOwnerUser) {
          tgSendTo(chat_id, "⛔ Yetkisiz: /admin_menu (sadece OWNER)");
        } else {
          g_activeChatId = chat_id;
          saveActiveChatToNvs();
          g_panelMsgId = 0;
          g_lastTgMsg = ""; g_lastTgMsgMs = 0;
          tgSendTo(chat_id, "✅ Eşleştirildi. Artık komutlar bu sohbetten çalışır.");
          g_mainBottom = "";
          requestUiRefresh();
        }
      }
      else if (cmdPanel) {
        // Panel aç / edit et
        g_mainBottom = "";
        requestUiRefresh();
      }
      else if (cmdIs(text, "/help") || cmdIs(text, "/yardim") || cmdIs(text, "/yardım")) {
        String msg;
        msg += "Komutlar:\n";
        msg += "/panel (/start /menu)\n";
        msg += "/durum\n/myid\n/dinigunler\n";
        msg += "/admin_menu (OWNER)\n";
        msg += "\nAdmin:\n";
        msg += "/on /off /guncelle\n/admin_list\n/admin_add <id>\n/admin_del <id>\n";
        tgSendTo(chat_id, msg);
      }
      else if (cmdIs(text, "/durum") || cmdIs(text, "/status")) {
        tgSendTo(chat_id, buildStatusText(who));
        logUser("TG: /durum (" + who + ")");
      }
      else if (cmdIs(text, "/dinigunler") || cmdIs(text, "/dinigünler")) {
        sendDiniGunlerList();
      }

      // Admin komutları
      else if (cmdIs(text, "/admin_list") || cmdIs(text, "/admins")) {
        if (!isAdminUser) {
          tgSendTo(chat_id, "⛔ Yetkisiz: /admin_list\n👤 " + who);
          logSerialAndTg("⛔ YETKISIZ /admin_list  " + who, true, true);
        } else {
          String msg = "👮 Admin listesi:\n";
          for (uint8_t k = 0; k < g_adminCount; k++) {
            msg += String((long long)g_adminIds[k]);
            if (g_adminIds[k] == OWNER_ADMIN_ID) msg += " (OWNER)";
            msg += "\n";
          }
          tgSendTo(chat_id, msg);
        }
      }
      else if (text.startsWith("/admin_add")) {
        if (!isAdminUser) {
          tgSendTo(chat_id, "⛔ Yetkisiz: /admin_add\n👤 " + who);
          logSerialAndTg("⛔ YETKISIZ /admin_add  " + who, true, true);
        } else {
          int64_t newId = parseIdArg(text);
          if (newId == 0) tgSendTo(chat_id, "Kullanim: /admin_add 123456789\nİpucu: kisi /myid yazsin.");
          else {
            bool ok = addAdmin(newId);
            if (ok) {
              tgSendTo(chat_id, "✅ Admin eklendi: " + String((long long)newId) + "\n👤 Ekleyen: " + who);
              logSerialAndTg("✅ Admin eklendi: " + String((long long)newId) + "  Ekleyen: " + who, true, true);
              logUser("TG: Admin eklendi (" + who + ")");
            } else tgSendTo(chat_id, "❌ Admin eklenemedi (liste dolu olabilir).");
          }
        }
      }
      else if (text.startsWith("/admin_del")) {
        if (!isAdminUser) {
          tgSendTo(chat_id, "⛔ Yetkisiz: /admin_del\n👤 " + who);
          logSerialAndTg("⛔ YETKISIZ /admin_del  " + who, true, true);
        } else {
          int64_t delId = parseIdArg(text);
          if (delId == 0) tgSendTo(chat_id, "Kullanim: /admin_del 123456789");
          else {
            bool ok = delAdmin(delId);
            if (ok) {
              tgSendTo(chat_id, "✅ Admin silindi: " + String((long long)delId) + "\n👤 Silen: " + who);
              logSerialAndTg("✅ Admin silindi: " + String((long long)delId) + "  Silen: " + who, true, true);
              logUser("TG: Admin silindi (" + who + ")");
            } else tgSendTo(chat_id, "❌ Admin silinemedi (OWNER/son admin olabilir veya yok).");
          }
        }
      }
      else if (cmdIs(text, "/on")) {
        if (!isAdminUser) {
          tgSendTo(chat_id, "⛔ Yetkisiz: /on\n👤 " + who);
          logSerialAndTg("⛔ YETKISIZ /on  " + who, true, true);
        } else {
          int rc = manualRelayOn("TG", who);
          if (rc == 1) {
            tgSendTo(chat_id, "⛔ Su an ON engelli (Zorunlu OFF sonrasi).\n👤 " + who);
          } else {
            char offb[32];
            formatDateTime(g_nextImsakOffTs, offb, sizeof(offb));
            int tolMin = (g_offOffsetSec >= 0) ? (g_offOffsetSec / 60) : 0;
            String offInfo = String(offb);
            if (g_nextImsakOffTs == 0) offInfo = "-";
            tgSendTo(chat_id, "✅ Manual ON.\n⛔ Zorunlu OFF: " + offInfo + " (İmsak - Sabah tolerans: " + String(tolMin) + " dk)\n👤 " + who);
          }
        }
      }
      else if (cmdIs(text, "/off")) {
        if (!isAdminUser) {
          tgSendTo(chat_id, "⛔ Yetkisiz: /off\n👤 " + who);
          logSerialAndTg("⛔ YETKISIZ /off  " + who, true, true);
        } else {
          time_t untilTs = manualRelayOff("TG", who);

          if (untilTs != 0) {
            char ub[32]; formatDateTime(untilTs, ub, sizeof(ub));
            tgSendTo(chat_id, "✅ Manual OFF (Scheduled override)\n⛔ Tekrar ON olmayacak (pencere bitişi): " + String(ub) + "\n👤 " + who);
          } else {
            tgSendTo(chat_id, "✅ Manual OFF\n👤 " + who);
          }
        }
      }
      else if (cmdIs(text, "/guncelle") || cmdIs(text, "/güncelle") || cmdIs(text, "/update")) {
        if (!isAdminUser) {
          tgSendTo(chat_id, "⛔ Yetkisiz: /guncelle\n👤 " + who);
          logSerialAndTg("⛔ YETKISIZ /guncelle  " + who, true, true);
        } else {
          tgSaveLastIdToNvsIfNew();

          if (g_updateInProgress) {
            tgSendTo(chat_id, "⏳ Zaten guncelleme calisiyor.\n👤 " + who);
          } else if (g_updatePending) {
            tgSendTo(chat_id, "⏳ Guncelleme kuyrukta.\n👤 " + who);
          } else if (g_updateCooldownUntilMs != 0 && !millisPassed(g_updateCooldownUntilMs)) {
            tgSendTo(chat_id, "⏳ Bekle: guncelleme koruma (cooldown) aktif.\n👤 " + who);
          } else {
            g_updatePending = true;
            g_updateRequesterWho = who;

            tgSendTo(chat_id, "✅ /guncelle alindi. Cache guncellemesi baslatilacak...\n👤 " + who);
            logSerialAndTg("📌 /guncelle kuyruğa alindi  " + who, true, true);
            logUser("TG: Vakit guncelleme (" + who + ")");
          }
        }
      }
    }

    yield();
    n = bot.getUpdates(bot.last_message_received + 1);
  }

  // Güvenlik: beklenmeyen bir durumda aynı update'ler tekrar gelirse sonsuz döngüye girme
  if (n && cycles >= 3) {
    Serial.println("[TG] Uyari: getUpdates dongusu limite takildi (muhtemel tekrarlayan update). Bir sonraki poll'da devam.");
  }

  tgSaveLastIdToNvsIfNew();
}


// =====================
// Web Panel (HTTP) - API KEY
// =====================
// Timing-safe string karşılaştırma (side-channel koruması)
static bool secureCompare(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); i++) {
    diff |= (uint8_t)(a[i] ^ b[i]);
  }
  return (diff == 0);
}

static bool webAuthOk() {
  if (g_webKey.length() == 0) return true; // auth kapalı
  // Header
  if (g_web->hasHeader("X-API-KEY")) {
    String k = g_web->header("X-API-KEY");
    k.trim();
    if (secureCompare(k, g_webKey)) return true;
  }
  // Query
  if (g_web->hasArg("k")) {
    String kq = g_web->arg("k");
    kq.trim();
    if (secureCompare(kq, g_webKey)) return true;
  }
  return false;
}

static void webSend401() {
  g_web->send(401, "application/json", "{\"ok\":false,\"err\":\"unauthorized\"}");
}
// Auth helper: false döndürürse zaten 401 gönderilmiştir, handler return etmeli
static bool webRequireAuth() {
  if (webAuthOk()) return true;
  webSend401();
  return false;
}
// JSON body parse helper: false döndürürse 400 gönderilmiştir
static bool webParseBody(DynamicJsonDocument& doc) {
  String body = g_web->arg("plain");
  if (body.length() == 0 || deserializeJson(doc, body)) {
    g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"json\"}");
    return false;
  }
  return true;
}
// JSON hata yanıt helper
static void webSendJsonError(int code, const char* err, const char* msg = nullptr) {
  String r = "{\"ok\":false,\"err\":\""; r += err; r += "\"";
  if (msg) { r += ",\"msg\":\""; r += msg; r += "\""; }
  r += "}";
  g_web->send(code, "application/json", r);
}

// Basit yetki kontrolü endpoint'i (Public sayfadan "anahtar doğru mu" kontrolü için)
static void webHandleAuthCheck() {
  if (!webRequireAuth()) return;
  g_web->send(200, "application/json", "{\"ok\":true}");
}

// =====================
// Web HTML (Public + Admin Tabs)
// =====================
static const char WEB_PUBLIC_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="tr"><head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"/>
<meta name="theme-color" content="#0b6"/>
<meta name="apple-mobile-web-app-capable" content="yes"/>
<title>Cami Otomasyon</title>
<style>
:root{--ac:#0b6;--bg:#f0f4f3;--card:#fff;--cb:rgba(0,0,0,.06);--tx:#1a2332;--ts:#6b7c8d;--ib:#f6f8fa;--ibr:#d0d7de;--hd:linear-gradient(135deg,#0b6 0%,#087f5b 50%,#0a5e42 100%)}
.dk{--bg:#0d1117;--card:#161b22;--cb:rgba(255,255,255,.06);--tx:#e6edf3;--ts:#8b949e;--ib:#21262d;--ibr:#30363d;--hd:linear-gradient(135deg,#0d2818 0%,#0a1628 50%,#1a0a28 100%)}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',-apple-system,BlinkMacSystemFont,sans-serif;background:var(--bg);color:var(--tx);transition:background .3s,color .3s}
.hdr{background:var(--hd);padding:20px 16px 16px;border-radius:0 0 24px 24px;box-shadow:0 4px 20px rgba(0,0,0,.15);position:relative;z-index:2}
.hdr-in{max-width:720px;margin:0 auto}
.hdr h1{font-size:22px;font-weight:800;color:#fff;letter-spacing:-.02em}
.hdr .sub{font-size:12px;color:rgba(255,255,255,.7);margin-top:3px}
.hdr-btns{display:flex;gap:6px}
.hdr-btn{width:36px;height:36px;border-radius:10px;border:1px solid rgba(255,255,255,.2);background:rgba(255,255,255,.1);color:#fff;cursor:pointer;display:flex;align-items:center;justify-content:center;font-size:16px}
.tabs{display:flex;gap:5px;margin-top:14px;overflow-x:auto;padding-bottom:2px;-webkit-overflow-scrolling:touch}
.tabs::-webkit-scrollbar{display:none}
.tab{padding:7px 13px;border-radius:18px;border:none;font-size:12px;font-weight:600;cursor:pointer;white-space:nowrap;transition:all .2s}
.tab.on{background:rgba(255,255,255,.93);color:#0a5e42}
.tab.off{background:rgba(255,255,255,.12);color:rgba(255,255,255,.85)}
.tab.lock{border:1px solid rgba(255,255,255,.25);background:transparent;color:rgba(255,255,255,.85)}
.main{max-width:720px;margin:0 auto;padding:16px 14px 40px;position:relative;z-index:1}
.cd{background:var(--card);border:1px solid var(--cb);border-radius:18px;padding:18px;margin-bottom:14px;box-shadow:0 2px 10px rgba(0,0,0,.04);transition:all .3s}
.dk .cd{box-shadow:0 2px 10px rgba(0,0,0,.25)}
.cd h3{font-size:17px;font-weight:700;margin-bottom:14px}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.grid3{display:grid;grid-template-columns:repeat(3,1fr);gap:9px}
.grid4{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}
.btn{padding:9px 18px;border-radius:11px;border:none;font-size:13px;font-weight:600;cursor:pointer;transition:all .2s;display:inline-flex;align-items:center;gap:5px}
.btn-p{background:var(--ac);color:#fff}
.btn-s{background:var(--ib);color:var(--tx);border:1px solid var(--cb)}
.btn-d{background:transparent;color:#e74c3c;border:1px solid rgba(231,76,60,.3)}
.btn-relay{padding:14px 28px;border-radius:14px;border:none;font-size:16px;font-weight:700;cursor:pointer;transition:all .3s;display:inline-flex;align-items:center;gap:8px;min-width:200px;justify-content:center}
.btn-relay.on{background:linear-gradient(135deg,#0b6,#087f5b);color:#fff;box-shadow:0 4px 16px rgba(0,180,130,.3)}
.btn-relay.off{background:linear-gradient(135deg,#e74c3c,#c0392b);color:#fff;box-shadow:0 4px 16px rgba(231,76,60,.3)}
input[type=text],input[type=password],input[type=number],select{padding:9px 12px;border-radius:11px;border:1.5px solid var(--ibr);background:var(--ib);color:var(--tx);font-size:13px;outline:none;width:100%;box-sizing:border-box;transition:border .2s}
input:focus,select:focus{border-color:var(--ac)!important;box-shadow:0 0 0 3px rgba(0,180,130,.12)}
input:disabled,select:disabled{opacity:.5;cursor:not-allowed;background:rgba(128,128,128,.08)}
label.lbl{font-size:12px;font-weight:600;color:var(--ts);margin-bottom:5px;display:block;letter-spacing:.02em}
.badge{display:inline-block;padding:4px 10px;border-radius:99px;font-size:12px;font-weight:600}
.bg-ok{background:rgba(0,180,130,.1);color:var(--ac)}
.bg-err{background:rgba(231,76,60,.08);color:#e74c3c}
.bg-warn{background:rgba(255,180,0,.1);color:#b08000}
.stat{display:flex;align-items:center;gap:7px;padding:7px 11px;border-radius:9px;background:rgba(128,128,128,.06);font-size:12px}
.stat .k{color:var(--ts)}.stat .v{margin-left:auto;font-weight:600;font-variant-numeric:tabular-nums}
.bar-wrap{margin-bottom:12px}
.bar-hdr{display:flex;justify-content:space-between;font-size:12px;margin-bottom:4px}
.bar-hdr .k{font-weight:600}.bar-hdr .v{color:var(--ts);font-variant-numeric:tabular-nums}
.bar{height:7px;border-radius:4px;background:rgba(128,128,128,.1);overflow:hidden}
.bar-fill{height:100%;border-radius:4px;transition:width 1s ease}
.prayer{border-radius:14px;padding:12px 8px;text-align:center;border:1.5px solid var(--cb);background:rgba(128,128,128,.04);transition:all .3s;position:relative}
.prayer.next{background:rgba(0,180,130,.1);border-color:var(--ac);box-shadow:0 3px 14px rgba(0,180,130,.15)}
.prayer .nm{font-size:11px;opacity:.6;margin-bottom:2px}
.prayer .tm{font-size:20px;font-weight:700;font-variant-numeric:tabular-nums}
.prayer.next .tm{color:var(--ac);font-size:23px}
.sbox{border-radius:13px;padding:12px 14px;border:1px solid var(--cb);background:rgba(128,128,128,.03)}
.live{display:flex;align-items:center;gap:5px;padding:4px 10px;border-radius:18px;font-size:11px;font-weight:600;cursor:pointer}
.live.on{background:rgba(0,180,130,.1);color:var(--ac)}
.live .dot{width:7px;height:7px;border-radius:50%}
.live.on .dot{background:var(--ac);animation:pulse 2s infinite}
.sig-bars{display:flex;align-items:flex-end;gap:2px;height:16px}
.gauge-svg{transform:rotate(-90deg)}
table.sp{width:100%;border-collapse:collapse;font-size:13px}
table.sp th{padding:8px 5px;text-align:left;color:var(--ts);font-weight:600;font-size:11px;border-bottom:2px solid var(--cb)}
table.sp td{padding:8px 5px;border-bottom:1px solid var(--cb)}
table.sp select{width:auto;padding:5px 7px;font-size:12px}
.warn{padding:9px 12px;border-radius:11px;background:rgba(255,200,0,.07);border:1px solid rgba(255,200,0,.15);font-size:12px;color:var(--ts)}
.ilce-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
#toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%) translateY(20px);background:#1a1a2e;color:#fff;padding:10px 22px;border-radius:13px;font-size:13px;font-weight:500;opacity:0;transition:all .3s;z-index:999;pointer-events:none;box-shadow:0 6px 24px rgba(0,0,0,.3);max-width:90vw}
#toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
.hidden{display:none!important}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
@media(max-width:520px){.grid4{grid-template-columns:repeat(2,1fr)}.ilce-grid{grid-template-columns:1fr}}
</style>
</head><body>
<div class="hdr"><div class="hdr-in">
<div style="display:flex;justify-content:space-between;align-items:flex-start">
<div><h1>🕌 Cami Otomasyon</h1><div class="sub" id="hdrSub">-</div></div>
<div class="hdr-btns">
<button class="hdr-btn" onclick="toggleDark()" id="darkBtn">🌙</button>
<button class="hdr-btn hidden" id="rebootBtn" onclick="doReboot()" title="Yeniden Başlat">🔄</button>
<button class="hdr-btn hidden" id="logoutBtn" onclick="doLogout()">🚪</button>
</div></div>
<div class="tabs" id="tabBar"></div>
</div></div>
<div class="main" id="content"></div>
<div id="toast"></div>
<script>
var S={dark:false,authed:false,key:'',tab:'home',autoRef:true,pub:{},sys:{},sets:{},acfg:{}};
var refreshTimer=null;
function $(id){return document.getElementById(id)}
function toast(m){var t=$('toast');t.textContent=m;t.className='show';clearTimeout(t._t);t._t=setTimeout(function(){t.className=''},2800)}
function fmtB(b){return b>=1048576?(b/1048576).toFixed(1)+' MB':b>=1024?(b/1024|0)+' KB':b+' B'}
function fmtUp(s){var d=s/86400|0,h=(s%86400)/3600|0,m=(s%3600)/60|0;return d>0?d+'g '+h+'s':h>0?h+'s '+m+'dk':m+'dk'}
function getKey(){return localStorage.getItem('WEB_KEY')||''}
function saveKey(k){localStorage.setItem('WEB_KEY',k);S.key=k}
function api(path,opts){opts=opts||{};opts.headers=opts.headers||{};var k=getKey();if(k)opts.headers['X-API-KEY']=k;return fetch(path,opts)}
function barColor(pct){return pct>80?'#e74c3c':pct>60?'#f39c12':'#0b6'}
function rssiQ(r){return r>-50?'Mükemmel':r>-60?'Çok İyi':r>-70?'İyi':r>-80?'Zayıf':'Çok Zayıf'}
function rssiBars(r){return r>-50?4:r>-60?3:r>-70?2:r>-80?1:0}
function toggleDark(){S.dark=!S.dark;document.body.classList.toggle('dk',S.dark);$('darkBtn').textContent=S.dark?'☀️':'🌙';localStorage.setItem('DARK',S.dark?'1':'0')}

// ── Auth ──
function doLogin(){
  var inp=$('loginKey');if(inp)saveKey(inp.value.trim());
  api('/api/authcheck').then(function(r){
    if(r.ok){S.authed=true;S.tab='system';toast('✅ Giriş başarılı');loadSystem();loadSettings();loadAdminCfg();renderAll();}
    else toast('❌ Şifre hatalı');
  }).catch(function(){toast('Bağlantı hatası')});
}
function doLogout(){localStorage.removeItem('WEB_KEY');S.key='';S.authed=false;S.tab='home';toast('Çıkış yapıldı');renderAll()}
function doReboot(){if(!confirm('Sistem yeniden başlatılsın mı?'))return;toast('🔄 Yeniden başlatılıyor...');api('/api/action',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cmd:'reboot'})}).then(function(){setTimeout(function(){location.reload()},5000)}).catch(function(){setTimeout(function(){location.reload()},5000)})}

// ── Tabs ──
var TABS=[
  {id:'home',label:'🏠 Durum',pub:true},
  {id:'system',label:'📊 Sistem'},
  {id:'tolerans',label:'⚙️ Toleranslar'},
  {id:'dinigun',label:'🕌 Dini Günler'},
  {id:'komut',label:'🧰 Komutlar'},
  {id:'log',label:'📋 Log'},
  {id:'ayarlar',label:'🛠️ Ayarlar'}
];
function setTab(id){S.tab=id;renderAll()}
function renderTabs(){
  var h='';TABS.forEach(function(t){if(!t.pub&&!S.authed)return;h+='<button class="tab '+(S.tab===t.id?'on':'off')+'" onclick="setTab(\''+t.id+'\')">'+t.label+'</button>';});
  if(!S.authed)h+='<button class="tab '+(S.tab==='login'?'on':'lock')+'" onclick="setTab(\'login\')">🔒 Giriş</button>';
  $('tabBar').innerHTML=h;$('logoutBtn').className=S.authed?'hdr-btn':'hdr-btn hidden';$('rebootBtn').className=S.authed?'hdr-btn':'hdr-btn hidden';
}

// ── Data ──
function loadPublic(){fetch('/api/public').then(function(r){return r.json()}).then(function(d){S.pub=d;$('hdrSub').textContent=(d.version||'')+' • '+(d.now||'-');if(S.tab==='home')renderHome();if(S.tab==='komut')renderKomut()}).catch(function(){})}
function loadSystem(){api('/api/system').then(function(r){if(r.status===401)return null;return r.json()}).then(function(d){if(d){S.sys=d;if(S.tab==='system')renderSystem()}}).catch(function(){})}
function loadSettings(){api('/api/settings').then(function(r){if(r.status===401)return null;return r.json()}).then(function(d){if(d)S.sets=d}).catch(function(){})}
function loadAdminCfg(){api('/api/admincfg').then(function(r){if(r.status===401)return null;return r.json()}).then(function(d){if(d&&d.ok){S.acfg=d;if(S.tab==='ayarlar')renderAyarlar()}}).catch(function(){})}
function refreshData(){loadPublic();if(S.authed&&S.tab==='system')loadSystem();if(S.authed&&S.tab==='log')loadLogs()}
function startAutoRef(){clearInterval(refreshTimer);if(S.autoRef)refreshTimer=setInterval(refreshData,15000)}

// ── UI Helpers ──
function gauge(pct,size,label,detail){var r=(size-10)/2,c=2*Math.PI*r,off=c-(Math.min(100,Math.max(0,pct))/100)*c,col=barColor(pct);return'<div style="text-align:center"><div style="position:relative;display:inline-block;width:'+size+'px;height:'+size+'px"><svg width="'+size+'" height="'+size+'" class="gauge-svg"><circle cx="'+(size/2)+'" cy="'+(size/2)+'" r="'+r+'" fill="none" stroke="rgba(128,128,128,.1)" stroke-width="7"/><circle cx="'+(size/2)+'" cy="'+(size/2)+'" r="'+r+'" fill="none" stroke="'+col+'" stroke-width="7" stroke-dasharray="'+c+'" stroke-dashoffset="'+off+'" stroke-linecap="round" style="transition:stroke-dashoffset 1s"/></svg><div style="position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);font-size:'+(size*.22)+'px;font-weight:700;font-variant-numeric:tabular-nums">'+Math.round(pct)+'%</div></div><div style="font-size:10px;font-weight:600;color:var(--ts);margin-top:3px">'+label+'</div>'+(detail?'<div style="font-size:9px;color:var(--ts);opacity:.7">'+detail+'</div>':'')+'</div>'}
function sigBars(rssi){var n=rssiBars(rssi),col=n>=3?'#0b6':n===2?'#f39c12':'#e74c3c',h='<div class="sig-bars">';for(var i=1;i<=4;i++)h+='<div style="width:4px;height:'+(3+i*3)+'px;border-radius:1px;background:'+(i<=n?col:'rgba(128,128,128,.15)')+'"></div>';return h+'</div>'}
function barH(pct,label,detail,color){var c=color||barColor(pct);return'<div class="bar-wrap"><div class="bar-hdr"><span class="k">'+label+'</span><span class="v">'+detail+'</span></div><div class="bar"><div class="bar-fill" style="width:'+Math.min(100,pct)+'%;background:'+c+'"></div></div></div>'}
function statH(icon,label,val){return'<div class="stat"><span>'+icon+'</span><span class="k">'+label+'</span><span class="v">'+val+'</span></div>'}
function prayerH(name,time,isNext){return'<div class="prayer'+(isNext?' next':'')+'">'+(isNext?'<div style="position:absolute;top:4px;right:6px;color:var(--ac);font-size:11px">★</div>':'')+'<div class="nm">'+name+'</div><div class="tm">'+time+'</div></div>'}

function nextPrayer(d){if(!d||!d.now)return'';var hm=(d.now.split(' ')[1]||'').substring(0,5);if(d.imsak&&d.imsak>hm)return'imsak';if(d.aksam&&d.aksam>hm)return'aksam';return'imsak'}
function cmdAction(cmd){api('/api/action',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cmd:cmd})}).then(function(r){if(r.status===401){toast('Yetkisiz');return}return r.json()}).then(function(d){if(d){toast(d.ok?(d.msg||'✅ Tamam'):('❌ '+(d.msg||d.err||'Hata')));loadPublic()}}).catch(function(){toast('Bağlantı hatası')})}

// ══════════ HOME ══════════
function renderHome(){
  var d=S.pub;if(!d.ok){$('content').innerHTML='<div class="cd"><p>Yükleniyor...</p></div>';return}
  var np=nextPrayer(d),pN={imsak:'🌙 İmsak',gunes:'🌅 Güneş',ogle:'☀️ Öğle',ikindi:'🌤 İkindi',aksam:'🌇 Akşam',yatsi:'🌃 Yatsı'},h='';
  h+='<div class="cd"><div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:14px"><div><h3 style="margin:0">Namaz Vakitleri</h3><div style="font-size:12px;color:var(--ts);margin-top:3px">'+(d.hicri||'')+'</div></div>';
  h+='<div class="live'+(S.autoRef?' on':'')+'" onclick="S.autoRef=!S.autoRef;startAutoRef();renderAll()"><div class="dot" style="background:'+(S.autoRef?'var(--ac)':'var(--ts)')+'"></div>'+(S.autoRef?'Canlı':'Durdu')+'</div></div>';
  h+='<div class="grid3">';['imsak','gunes','ogle','ikindi','aksam','yatsi'].forEach(function(k){var t=d[k]||'—';h+=prayerH(pN[k],t,np===k&&t!=='—')});h+='</div></div>';
  h+='<div class="cd"><h3 style="margin-bottom:12px">Sistem Durumu</h3><div class="grid2">';
  var ron=d.relay;h+='<div class="sbox" style="border-color:'+(ron?'rgba(0,180,130,.2)':'rgba(231,76,60,.15)')+'"><div style="font-size:11px;color:var(--ts);margin-bottom:3px">⚡ Şerefeler</div><div style="font-size:18px;font-weight:700;color:'+(ron?'var(--ac)':'#e74c3c')+'">'+(ron?'AÇIK ✅':'KAPALI ❌')+'</div></div>';
  h+='<div class="sbox"><div style="font-size:11px;color:var(--ts);margin-bottom:3px">🕰️ Saat</div><div style="font-size:18px;font-weight:700;font-variant-numeric:tabular-nums">'+((d.now||'').split(' ')[1]||'-')+'</div></div>';
  h+='<div class="sbox"><div style="font-size:11px;color:var(--ts);margin-bottom:3px">📶 WiFi</div><div style="display:flex;align-items:center;gap:6px">'+sigBars(d.rssi||0)+'<div><div style="font-size:13px;font-weight:600">'+(d.ssid||'-')+'</div><div style="font-size:10px;color:var(--ts)">'+(d.rssi||0)+' dBm</div></div></div></div>';
  h+='<div class="sbox"><div style="font-size:11px;color:var(--ts);margin-bottom:3px">📊 Performans</div><div style="font-size:13px;font-weight:600">İş Yükü: '+(d.cpuUsage||0)+'%</div><div style="font-size:10px;color:var(--ts)">RAM: '+fmtB(d.freeHeap||0)+' boş</div></div>';
  h+='</div><div style="margin-top:10px;padding:9px 12px;border-radius:10px;background:rgba(128,128,128,.03);display:flex;flex-wrap:wrap;gap:5px 16px;font-size:12px;color:var(--ts)">';
  h+='<span>Tolerans: <b style="color:var(--tx)">'+(d.onTolMin||0)+'dk / '+(d.offTolMin||0)+'dk</b></span>';
  h+='<span>Cache: <b style="color:var(--tx)">'+(d.dayCount||0)+' gün</b></span>';
  h+='<span>İlçe: <b style="color:var(--tx)">'+(d.ilceId||0)+'</b></span>';
  h+='<span>Uptime: <b style="color:var(--tx)">'+fmtUp(d.uptimeSec||0)+'</b></span></div></div>';
  $('content').innerHTML=h;
}

// ══════════ SYSTEM ══════════
function renderSystem(){
  var d=S.sys;if(!d||!d.ok){$('content').innerHTML='<div class="cd"><p>Yükleniyor...</p></div>';loadSystem();return}
  var ram=d.ram||{},fl=d.flash||{},cpu=d.cpu||{},wifi=d.wifi||{},nvs=d.nvs||{},h='';
  h+='<div class="cd"><div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:18px"><h3 style="margin:0">📊 Sistem İzleme</h3>';
  h+='<div class="live'+(S.autoRef?' on':'')+'" onclick="S.autoRef=!S.autoRef;startAutoRef();renderAll()"><div class="dot" style="background:'+(S.autoRef?'var(--ac)':'var(--ts)')+'"></div>'+(S.autoRef?'Canlı':'Durdu')+'</div></div>';
  h+='<div class="grid4" style="text-align:center">';
  h+=gauge(ram.usedPct||0,85,'RAM',fmtB(ram.free||0)+' boş');
  var lps=cpu.loopsPerSec||0;var lpsStr=lps>=10?Math.round(lps)+'/s':lps>0?lps+'/s':'<1/s';
  var ioP=cpu.ioPct||0;
  h+='<div>'+gauge(cpu.usageTotal||0,85,'İş Yükü',lpsStr)+'<div style="font-size:10px;color:var(--ts);margin-top:2px">I/O: '+ioP+'%</div></div>';
  h+=gauge(fl.sketchPct||0,85,'Flash',fmtB(fl.sketchFree||0)+' boş');
  h+='<div style="display:flex;flex-direction:column;align-items:center;justify-content:center">'+sigBars(wifi.rssi||0)+'<div style="font-size:18px;font-weight:700;margin-top:5px;font-variant-numeric:tabular-nums">'+(wifi.rssi||0)+'</div><div style="font-size:10px;font-weight:600;color:var(--ts)">WiFi dBm</div><div style="font-size:9px;color:var(--ts)">'+rssiQ(wifi.rssi||0)+'</div></div>';
  h+='</div></div>';
  h+='<div class="cd"><h3>🧠 RAM (Heap)</h3>'+barH(ram.usedPct||0,'Kullanılan',fmtB(ram.used||0)+' / '+fmtB(ram.total||0))+'<div class="grid2">'+statH('📦','Boş',fmtB(ram.free||0))+statH('📉','Min Boş',fmtB(ram.minFree||0))+statH('🧩','Max Blok',fmtB(ram.maxAlloc||0))+statH('💾','Toplam',fmtB(ram.total||0))+'</div></div>';
  h+='<div class="cd"><h3>⚡ İşlemci</h3><div class="grid2" style="margin-bottom:12px">'+statH('🏷️','Model',cpu.model||'-')+statH('🧮','Çekirdek',cpu.cores||0)+statH('⏱️','Frekans',(cpu.freqMHz||0)+' MHz')+statH('🔄','Döngü',lpsStr)+statH('📊','İş Yükü',(cpu.usageTotal||0)+'%')+statH('📡','I/O Bekleme',ioP+'%');
  if(cpu.tempC)h+=statH('🌡️','Sıcaklık',cpu.tempC+'°C');
  h+=statH('⏰','Uptime',fmtUp(d.uptimeSec||0))+'</div></div>';
  h+='<div class="cd"><h3>💿 Flash</h3>'+barH(fl.sketchPct||0,'Firmware',fmtB(fl.sketch||0)+' / '+fmtB((fl.sketch||0)+(fl.sketchFree||0)))+'<div class="grid2">'+statH('📀','Toplam',fmtB(fl.total||0))+statH('📦','OTA Boş',fmtB(fl.sketchFree||0))+'</div></div>';
  h+='<div class="cd"><h3>📡 WiFi</h3><div class="grid2">'+statH('📶','SSID',wifi.ssid||'-')+statH('📊','Sinyal',(wifi.rssi||0)+' dBm')+statH('🌐','IP',wifi.ip||'-')+statH('🔗','MAC',wifi.mac||'-')+statH('📻','Kanal',wifi.channel||0)+statH('📡','TX',wifi.txPower||0)+'</div></div>';
  if(nvs.totalEntries){h+='<div class="cd"><h3>🗄️ NVS</h3>'+barH(((nvs.usedEntries||0)/(nvs.totalEntries||1)*100),'Entries',(nvs.usedEntries||0)+' / '+(nvs.totalEntries||0))+statH('📂','Namespace',nvs.nsCount||0)+'</div>'}
  $('content').innerHTML=h;
}

// ══════════ TOLERANS ══════════
function renderTolerans(){
  var d=S.sets,h='<div class="cd"><h3>⚙️ Toleranslar</h3><div class="grid2">';
  h+='<div><label class="lbl">Akşam tolerans (dk)</label><input type="number" id="onMin" min="0" max="30" value="'+(d.onTolMin||0)+'"/><div style="font-size:11px;color:var(--ts);margin-top:3px">Akşam + X dakika</div></div>';
  h+='<div><label class="lbl">Sabah tolerans (dk)</label><input type="number" id="offMin" min="0" max="30" value="'+(d.offTolMin||0)+'"/><div style="font-size:11px;color:var(--ts);margin-top:3px">İmsak - X dakika</div></div>';
  h+='</div><div class="row" style="margin-top:16px"><button class="btn btn-p" onclick="saveTolerans()">💾 Kaydet</button><button class="btn btn-s" onclick="loadSettings();loadPublic();toast(\'Yenileniyor...\')">🔄 Yenile</button></div></div>';
  $('content').innerHTML=h;
}
function saveTolerans(){
  var body=JSON.stringify({onTolMin:parseInt($('onMin').value||'0',10),offTolMin:parseInt($('offMin').value||'0',10)});
  api('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:body}).then(function(r){if(r.status===401){toast('Yetkisiz');return}return r.json()}).then(function(d){if(d)toast(d.ok?'✅ Kaydedildi':'❌ '+(d.detail||d.err||'Hata'));loadSettings();loadPublic()}).catch(function(){toast('Bağlantı hatası')});
}

// ══════════ DİNİ GÜNLER ══════════
var HMONTHS=['Muharrem','Safer','Rebiülevvel','Rebiülahir','Cemaziyelevvel','Cemaziyelahir','Receb','Şaban','Ramazan','Şevval','Zilkade','Zilhicce'];
function renderDiniGunler(){
  var d=S.sets;if(!d.specials){$('content').innerHTML='<div class="cd"><p>Yükleniyor...</p></div>';return}
  var h='<div class="cd"><h3>🕌 Dini Günler</h3><div style="overflow-x:auto"><table class="sp"><thead><tr><th></th><th>Dini Gün</th><th>Gün</th><th>Ay</th><th>Yıl</th><th>Vars.</th></tr></thead><tbody>';
  d.specials.forEach(function(sp){
    var dis=sp.useDefault?' disabled':'';
    h+='<tr style="opacity:'+(sp.en?1:.5)+'"><td><input type="checkbox" id="sp_en_'+sp.id+'" '+(sp.en?'checked':'')+'/></td>';
    h+='<td style="font-weight:500;white-space:nowrap">'+sp.name+'</td>';
    h+='<td><select id="sp_day_'+sp.id+'"'+dis+'>';for(var i=1;i<=30;i++)h+='<option value="'+i+'"'+(sp.hDay===i?' selected':'')+'>'+i+'</option>';h+='</select></td>';
    h+='<td><select id="sp_mon_'+sp.id+'"'+dis+'>';HMONTHS.forEach(function(m,j){h+='<option value="'+j+'"'+(sp.hMonth===j?' selected':'')+'>'+m+'</option>'});h+='</select></td>';
    h+='<td><select id="sp_yr_'+sp.id+'"'+dis+'>';for(var y=1447;y<=1461;y++)h+='<option value="'+y+'"'+(sp.hYear===y?' selected':'')+'>'+y+'</option>';h+='</select></td>';
    h+='<td style="text-align:center"><input type="checkbox" id="sp_def_'+sp.id+'" '+(sp.useDefault?'checked':'')+' onchange="toggleSpDef('+sp.id+')"/></td></tr>';
  });
  h+='<tr style="background:rgba(0,180,130,.03)"><td><input type="checkbox" id="ram_all" '+(d.ramazanAll?'checked':'')+'></td><td colspan="5" style="font-weight:600;color:var(--ac)">☪️ Ramazan (Tüm Günler)</td></tr>';
  h+='</tbody></table></div><div class="row" style="margin-top:16px"><button class="btn btn-p" onclick="saveDiniGunler()">💾 Kaydet</button><button class="btn btn-s" onclick="loadSettings();setTimeout(renderDiniGunler,500)">🔄 Yenile</button></div></div>';
  h+='<div class="cd"><h3>🗓️ Aktif/Yaklaşan Günler</h3><button class="btn btn-s" onclick="loadDGText()">🔄 Yenile</button><pre id="dgText" style="white-space:pre-wrap;margin-top:10px;background:var(--ib);border:1px solid var(--cb);border-radius:10px;padding:10px;font-size:12px">-</pre></div>';
  $('content').innerHTML=h;loadDGText();
}
function toggleSpDef(id){var ch=$('sp_def_'+id),dis=ch&&ch.checked;['sp_day_'+id,'sp_mon_'+id,'sp_yr_'+id].forEach(function(eid){var el=$(eid);if(el)el.disabled=dis});if(dis&&S.sets&&S.sets.specials){var sp=S.sets.specials.find(function(s){return s.id===id});if(sp){var d=$('sp_day_'+id),m=$('sp_mon_'+id),y=$('sp_yr_'+id);if(d)d.value=sp.defDay;if(m)m.value=sp.defMonth;if(y)y.value=sp.defYear}}}
function loadDGText(){api('/api/dinigunler').then(function(r){return r.text()}).then(function(t){var el=$('dgText');if(el)el.textContent=t&&t.trim().length?t:'-'}).catch(function(){})}
function saveDiniGunler(){
  var d=S.sets,specials=[];
  if(d.specials){d.specials.forEach(function(sp){var en=$('sp_en_'+sp.id),def=$('sp_def_'+sp.id),day=$('sp_day_'+sp.id),mon=$('sp_mon_'+sp.id),yr=$('sp_yr_'+sp.id);specials.push({id:sp.id,en:en?en.checked:false,useDefault:def?def.checked:true,hDay:day?parseInt(day.value,10):1,hMonth:mon?parseInt(mon.value,10):0,hYear:yr?parseInt(yr.value,10):1447})})}
  var ramAll=$('ram_all')?$('ram_all').checked:false;
  api('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ramazanAll:ramAll,specials:specials})}).then(function(r){if(r.status===401){toast('Yetkisiz');return}return r.json()}).then(function(d){if(d)toast(d.ok?'✅ Kaydedildi':'❌ '+(d.detail||d.err||'Hata'));loadSettings()}).catch(function(){toast('Bağlantı hatası')});
}

// ══════════ KOMUTLAR ══════════
function renderKomut(){
  var d=S.pub,ron=d.relay,h='';
  var upP=d.updatePending,upR=d.updateInProgress;
  var upSt=upR?'🔄 Aktif':upP?'⏳ Kuyrukta':'✅ Boşta';
  var upCol=upR?'bg-warn':upP?'bg-warn':'bg-ok';
  h+='<div class="cd"><h3>🧰 Komutlar</h3>';
  h+='<div style="display:flex;align-items:center;gap:10px;margin-bottom:14px;padding:10px 14px;border-radius:11px;background:rgba(128,128,128,.04);border:1px solid var(--cb)"><span style="font-size:12px;color:var(--ts)">Güncelleme:</span><span class="badge '+upCol+'">'+upSt+'</span>';
  if(upP)h+='<button class="btn btn-d" style="padding:5px 12px;font-size:12px" onclick="cmdAction(\'cancelUpdate\')">❌ İptal</button>';
  h+='</div>';
  h+='<div class="row"><button class="btn btn-p" onclick="cmdAction(\'updateTimes\')">📥 Vakitleri Güncelle</button><button class="btn btn-s" onclick="cmdAction(\'recompute\')">🧮 Çizelge Yenile</button></div></div>';
  h+='<div class="cd"><h3>⚡ Şerefeler Kontrolü</h3><div style="text-align:center;padding:10px 0">';
  h+='<button class="btn-relay '+(ron?'on':'off')+'" onclick="cmdAction(\''+(ron?'relayOff':'relayOn')+'\')">'+(ron?'🟢 AÇIK — Kapat':'🔴 KAPALI — Aç')+'</button>';
  h+='<div style="font-size:12px;color:var(--ts);margin-top:10px">Durum: <b style="color:'+(ron?'var(--ac)':'#e74c3c')+'">'+(ron?'AÇIK':'KAPALI')+'</b></div>';
  h+='</div></div>';
  $('content').innerHTML=h;
}

// ══════════ LOG ══════════
function loadLogs(){api('/api/logs').then(function(r){if(r.status===401)return null;return r.json()}).then(function(d){if(d&&d.ok){S.logs=d;if(S.tab==='log')renderLog()}}).catch(function(){})}
function logTable(arr){if(!arr||arr.length===0)return'<div style="padding:12px;color:var(--ts);font-size:12px;text-align:center">Henuz kayit yok</div>';var h='<table style="width:100%;border-collapse:collapse;font-size:12px"><thead><tr style="border-bottom:2px solid var(--cb)"><th style="text-align:left;padding:8px 6px;color:var(--ts);font-weight:600;width:140px">Tarih / Saat</th><th style="text-align:left;padding:8px 6px;color:var(--ts);font-weight:600">İşlem</th></tr></thead><tbody>';for(var i=arr.length-1;i>=0;i--){var e=arr[i];h+='<tr style="border-bottom:1px solid var(--cb)"><td style="padding:6px;white-space:nowrap;color:var(--ts);font-variant-numeric:tabular-nums">'+e.ts+'</td><td style="padding:6px">'+e.msg+'</td></tr>'}h+='</tbody></table>';return h}
function renderLog(){
  var d=S.logs;if(!d){$('content').innerHTML='<div class="cd"><p>Yükleniyor...</p></div>';loadLogs();return}
  var h='';
  h+='<div class="cd"><div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:14px"><h3 style="margin:0">👤 Kullanıcı İşlemleri</h3><span style="font-size:11px;color:var(--ts)">'+(d.user?d.user.length:0)+' kayıt</span></div>';
  h+=logTable(d.user);
  h+='</div>';
  h+='<div class="cd"><div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:14px"><h3 style="margin:0">⚙️ Sistem İşlemleri</h3><span style="font-size:11px;color:var(--ts)">'+(d.sys?d.sys.length:0)+' kayıt</span></div>';
  h+=logTable(d.sys);
  h+='</div>';
  h+='<div style="text-align:center;padding:10px"><button class="btn btn-s" onclick="loadLogs();toast(\'Yenileniyor...\')">🔄 Yenile</button></div>';
  $('content').innerHTML=h;
}

// ══════════ AYARLAR ══════════
function renderAyarlar(){
  var ac=S.acfg||{},net=ac.net||{},creds=ac.creds||{},h='';
  // 1. Ağ Ayarları
  var isStatic=net.useStatic;
  var ip=isStatic&&net.ip?net.ip:(net.liveIp||''),gw=isStatic&&net.gw?net.gw:(net.liveGw||''),mask=isStatic&&net.mask?net.mask:(net.liveMask||'');
  var dns1=net.dns1||(net.liveDns1||''),dns2=net.dns2||(net.liveDns2||''),port=net.httpPort||80;
  var dis=isStatic?'':' disabled';
  h+='<div class="cd"><h3>📡 Ağ Ayarları</h3>';
  h+='<div class="row" style="margin-bottom:12px"><label style="display:flex;align-items:center;gap:6px;cursor:pointer"><input type="checkbox" id="useStatic" '+(isStatic?'checked':'')+' onchange="toggleStatic()"/> Statik IP</label>';
  h+='<span class="badge '+(isStatic?'bg-ok':'bg-warn')+'">'+(isStatic?'Statik':'DHCP')+'</span></div>';
  h+='<div class="grid2" style="grid-template-columns:repeat(auto-fit,minmax(160px,1fr))">';
  h+='<div><label class="lbl">IP</label><input type="text" id="ipS" value="'+ip+'"'+dis+'/></div>';
  h+='<div><label class="lbl">Gateway</label><input type="text" id="gwS" value="'+gw+'"'+dis+'/></div>';
  h+='<div><label class="lbl">Mask</label><input type="text" id="maskS" value="'+mask+'"'+dis+'/></div>';
  h+='<div><label class="lbl">DNS1</label><input type="text" id="dns1S" value="'+dns1+'"'+dis+'/></div>';
  h+='<div><label class="lbl">DNS2</label><input type="text" id="dns2S" value="'+dns2+'"'+dis+'/></div>';
  h+='<div><label class="lbl">Port</label><input type="number" id="portS" value="'+port+'"/></div>';
  h+='</div><div class="row" style="margin-top:14px"><button class="btn btn-p" onclick="saveNet()">💾 Ağ Kaydet</button></div>';
  h+='<div style="font-size:11px;color:var(--ts);margin-top:6px">Ağ ayarı kaydedilince cihaz yeniden başlatılır.</div></div>';

  // 2. WiFi Bilgileri (ayrı kart, ağ altında)
  h+='<div class="cd"><h3>🔐 WiFi Bilgileri</h3>';
  h+='<div class="grid2" style="grid-template-columns:repeat(auto-fit,minmax(180px,1fr))">';
  h+='<div><label class="lbl">WiFi SSID</label><div style="display:flex;gap:6px"><input type="text" id="wfSsid" value="'+(creds.nvsWifiSsid||'')+'" style="flex:1"/><button class="btn btn-s" style="padding:6px 10px;font-size:12px;white-space:nowrap" onclick="wifiScan()">📶 Tara</button></div><select id="wfScanList" class="hidden" style="margin-top:6px" onchange="wfPickSsid()"><option>-</option></select></div>';
  h+='<div><label class="lbl">WiFi Şifre</label><input type="password" id="wfPass" placeholder="değiştirmek için yaz"/></div></div>';
  h+='<div class="row" style="margin-top:12px"><button class="btn btn-p" onclick="saveWifi()">💾 Kaydet</button>';
  h+='<button class="btn btn-d" onclick="clearWifi()">🧹 WiFi Sil</button></div>';
  h+='<div style="font-size:11px;color:var(--ts);margin-top:6px">WiFi: '+(creds.wifiSrc||'-')+'</div></div>';

  // 2.5 Hicri Yil
  var hny=ac.hicriYear||{};
  h+='<div class="cd"><h3>📅 Hicri Yıl Ayarları</h3>';
  h+='<div class="grid2" style="margin-bottom:12px">';
  h+='<div><label class="lbl">Yılbaşı Günü</label><select id="hnyDay">';for(var dd=1;dd<=30;dd++)h+='<option value="'+dd+'"'+((hny.day||1)===dd?' selected':'')+'>'+dd+'</option>';h+='</select></div>';
  h+='<div><label class="lbl">Yılbaşı Ayı</label><select id="hnyMon">';var _hm=['Muharrem','Safer','Rebiülevvel','Rebiülahir','Cemaziyelevvel','Cemaziyelahir','Receb','Şaban','Ramazan','Şevval','Zilkade','Zilhicce'];_hm.forEach(function(m,j){h+='<option value="'+j+'"'+((hny.mon||0)===j?' selected':'')+'>'+m+'</option>'});h+='</select></div>';
  h+='</div>';
  h+='<label style="display:flex;align-items:center;gap:8px;cursor:pointer;margin-bottom:10px"><input type="checkbox" id="hnyAuto" '+(hny.auto?'checked':'')+'/>  Yılları Otomatik Güncelle</label>';
  if(hny.lastYear)h+='<div style="font-size:11px;color:var(--ts)">Son güncellenen yıl: '+hny.lastYear+'</div>';
  h+='<div class="row" style="margin-top:12px"><button class="btn btn-p" onclick="saveHicriYear()">💾 Kaydet</button></div>';
  h+='<div style="font-size:11px;color:var(--ts);margin-top:6px">Yılbaşı geçtiğinde dini günlerin yılı otomatik +1 olur.</div></div>';

  // 3. İlçe ID Bulucu (üstte) + İlçe Kodu (altta)
  h+='<div class="cd"><h3>🔍 Ezan Vakti İlçe ID Bulucu</h3>';
  h+='<div class="ilce-grid"><div><label class="lbl">Ülke</label><select id="ilceUlke" onchange="ilceLoadCities()"><option>Yükleniyor...</option></select></div>';
  h+='<div><label class="lbl">Şehir</label><select id="ilceSehir" disabled onchange="ilceLoadDistricts()"><option>Önce ülke seçin</option></select></div>';
  h+='<div><label class="lbl">İlçe</label><select id="ilceIlce" disabled onchange="ilceSelect()"><option>Önce şehir seçin</option></select></div></div>';
  h+='<div id="ilceResult" class="hidden" style="margin-top:10px;padding:10px;border-radius:10px;background:rgba(0,180,130,.06);border:1px solid rgba(0,180,130,.15);font-size:14px;font-weight:600">İlçe ID: <span id="ilceFoundId">-</span> <button class="btn btn-p" style="margin-left:10px;padding:5px 12px;font-size:12px" onclick="ilceApply()">Uygula</button></div>';
  h+='<div style="font-size:11px;color:var(--ts);margin-top:6px" id="ilceStat">-</div>';
  h+='<div style="margin-top:16px;padding-top:16px;border-top:1px solid var(--cb)"><h3 style="font-size:15px;margin-bottom:12px">📍 İlçe Kodu</h3>';
  h+='<div class="row"><div style="flex:1;min-width:100px"><label class="lbl">İlçe ID</label><input type="number" id="ilceId" value="'+(ac.ilceId||'')+'"/></div>';
  h+='<button class="btn btn-p" onclick="saveIlce()">💾</button><button class="btn btn-s" onclick="cmdAction(\'updateTimes\')">📥 Güncelle</button></div></div></div>';

  // 4. Kimlik Bilgileri
  h+='<div class="cd"><h3>🔑 Kimlik Bilgileri</h3>';
  h+='<div style="overflow-x:auto"><table class="sp"><thead><tr><th>Alan</th><th>Kaynak</th><th>Durum</th><th>Değiştir</th></tr></thead><tbody>';
  h+='<tr><td style="font-weight:600">Web Şifre</td><td><span class="badge '+(creds.webKeySrc==='nvs'?'bg-ok':'bg-warn')+'">'+(creds.webKeySrc||'-')+'</span></td>';
  h+='<td>'+(creds.activeWebKeySet?'✅ Ayarlı':'❌ Yok')+'</td>';
  h+='<td><input type="password" id="newWebKey" placeholder="Yeni şifre" style="width:140px;font-size:12px"/></td></tr>';
  h+='<tr><td style="font-weight:600">Bot Token</td><td><span class="badge '+(creds.botTokenSrc==='nvs'?'bg-ok':'bg-warn')+'">'+(creds.botTokenSrc||'-')+'</span></td>';
  h+='<td>'+(creds.botTokenSet?'✅ Ayarlı':'❌ Yok')+'</td>';
  h+='<td><input type="password" id="newBotToken" placeholder="Yeni token" style="width:140px;font-size:12px"/></td></tr>';
  h+='<tr><td style="font-weight:600">Chat ID</td><td><span class="badge '+(creds.chatIdSrc==='nvs'?'bg-ok':'bg-warn')+'">'+(creds.chatIdSrc||'-')+'</span></td>';
  h+='<td style="font-variant-numeric:tabular-nums">'+(creds.chatId||'-')+'</td>';
  h+='<td><input type="text" id="newChatId" placeholder="Yeni chat ID" style="width:140px;font-size:12px"/></td></tr>';
  h+='</tbody></table></div>';
  h+='<div class="row" style="margin-top:12px"><button class="btn btn-p" onclick="saveKimlik()">💾 Kaydet</button>';
  h+='<button class="btn btn-d" onclick="clearWebKey()">🧹 Web Şifre Sil</button>';
  h+='<button class="btn btn-d" onclick="clearBotToken()">🧹 Bot Token Sil</button>';
  h+='<button class="btn btn-d" onclick="clearChatId()">🧹 Chat ID Sil</button></div>';
  h+='<div style="font-size:11px;color:var(--ts);margin-top:6px">NVS\'e kaydedilen bilgiler secrets.h\'yi geçersiz kılar. Sil butonu NVS\'i temizler ve secrets.h aktif olur.</div></div>';

  // 5. Telegram Admin
  h+='<div class="cd"><h3>👮 Telegram Admin</h3><div class="row" style="margin-bottom:10px">';
  h+='<div style="flex:1;min-width:140px"><label class="lbl">Admin ID</label><input type="text" id="adminId"/></div>';
  h+='<button class="btn btn-p" onclick="adminAdd()">➕</button><button class="btn btn-d" onclick="adminDel()">➖</button></div>';
  h+='<div id="adminsList" style="display:flex;gap:6px;flex-wrap:wrap"></div></div>';

  // 6. OTA
  h+='<div class="cd"><h3>⬆️ Firmware Güncelle</h3><div class="row"><input type="file" id="fwFile" accept=".bin"/> <button class="btn btn-p" onclick="uploadFw()">⬆️ Yükle</button></div>';
  h+='<div style="margin-top:10px"><div class="bar" style="height:6px"><div class="bar-fill" id="fwBar" style="width:0%;background:var(--ac)"></div></div><div style="font-size:11px;color:var(--ts);margin-top:4px" id="fwStat">-</div></div></div>';

  // 7. Factory Reset
  h+='<div class="cd" style="border-color:rgba(231,76,60,.2)"><h3 style="color:#e74c3c">🧨 Fabrika Ayarları</h3>';
  h+='<div class="row"><div style="flex:1;min-width:180px"><label class="lbl">Owner Key</label><input type="password" id="rstKey"/></div>';
  h+='<button class="btn btn-s" onclick="rstCheck()">Doğrula</button></div>';
  h+='<div id="rstWrap" class="hidden" style="margin-top:10px"><label><input type="checkbox" id="rstConfirm"/> Evet, sıfırla</label> <button class="btn btn-d" onclick="rstDo()" style="margin-top:8px">⚠️ Sıfırla</button></div></div>';

  $('content').innerHTML=h;
  if(ac.admins)renderAdmins(ac.admins);
  ilceLoadCountries();
}

function toggleStatic(){var ch=$('useStatic').checked;['ipS','gwS','maskS','dns1S','dns2S'].forEach(function(id){var el=$(id);if(el)el.disabled=!ch})}
function renderAdmins(list){var el=$('adminsList');if(!el)return;el.innerHTML='';if(!list||!list.length){el.innerHTML='<span style="font-size:12px;color:var(--ts)">-</span>';return}list.forEach(function(a){el.innerHTML+='<span class="badge bg-ok">'+a.id+(a.owner?' (OWNER)':'')+'</span>'})}

// İlçe ID Bulucu API
var _ilkeUlk=[],_ilkeSeh=[],_ilkeIlc=[];
function ilceLoadCountries(){fetch('https://ezanvakti.emushaf.net/ulkeler').then(function(r){return r.json()}).then(function(d){_ilkeUlk=d;_ilkeUlk.sort(function(a,b){return a.UlkeAdi.localeCompare(b.UlkeAdi,'tr')});var s=$('ilceUlke');s.innerHTML='<option value="">Ülke seçin</option>';_ilkeUlk.forEach(function(u){s.innerHTML+='<option value="'+u.UlkeID+'">'+u.UlkeAdi+'</option>'});var st=$('ilceStat');if(st)st.textContent='Ülkeler yüklendi.'}).catch(function(){var st=$('ilceStat');if(st)st.textContent='Ülkeler yüklenemedi (CORS?)'})}
function ilceLoadCities(){var uid=$('ilceUlke').value;var s2=$('ilceSehir'),s3=$('ilceIlce');s2.innerHTML='<option>Yükleniyor...</option>';s2.disabled=true;s3.innerHTML='<option>Önce şehir seçin</option>';s3.disabled=true;$('ilceResult').className='hidden';if(!uid){s2.innerHTML='<option>Önce ülke seçin</option>';return}fetch('https://ezanvakti.emushaf.net/sehirler/'+uid).then(function(r){return r.json()}).then(function(d){_ilkeSeh=d;_ilkeSeh.sort(function(a,b){return a.SehirAdi.localeCompare(b.SehirAdi,'tr')});s2.innerHTML='<option value="">Şehir seçin</option>';_ilkeSeh.forEach(function(c){s2.innerHTML+='<option value="'+c.SehirID+'">'+c.SehirAdi+'</option>'});s2.disabled=false;var st=$('ilceStat');if(st)st.textContent='Şehirler yüklendi.'}).catch(function(){s2.innerHTML='<option>Yüklenemedi</option>';var st=$('ilceStat');if(st)st.textContent='Şehirler yüklenemedi.'})}
function ilceLoadDistricts(){var cid=$('ilceSehir').value;var s3=$('ilceIlce');s3.innerHTML='<option>Yükleniyor...</option>';s3.disabled=true;$('ilceResult').className='hidden';if(!cid){s3.innerHTML='<option>Önce şehir seçin</option>';return}fetch('https://ezanvakti.emushaf.net/ilceler/'+cid).then(function(r){return r.json()}).then(function(d){_ilkeIlc=d;_ilkeIlc.sort(function(a,b){return a.IlceAdi.localeCompare(b.IlceAdi,'tr')});s3.innerHTML='<option value="">İlçe seçin</option>';_ilkeIlc.forEach(function(i){s3.innerHTML+='<option value="'+i.IlceID+'">'+i.IlceAdi+'</option>'});s3.disabled=false;var st=$('ilceStat');if(st)st.textContent='İlçeler yüklendi.'}).catch(function(){s3.innerHTML='<option>Yüklenemedi</option>'})}
function ilceSelect(){var v=$('ilceIlce').value;if(!v){$('ilceResult').className='hidden';return}$('ilceFoundId').textContent=v;$('ilceResult').className='';var st=$('ilceStat');if(st)st.textContent='İlçe ID bulundu: '+v}
function ilceApply(){var v=$('ilceFoundId').textContent;if(v&&v!=='-'){var el=$('ilceId');if(el)el.value=v;toast('İlçe ID: '+v+' uygulandı')}}

// Admin API calls
function saveHicriYear(){var d=parseInt($('hnyDay').value||'1',10),m=parseInt($('hnyMon').value||'0',10),au=$('hnyAuto')?$('hnyAuto').checked:false;postAcfg({hicriYear:{day:d,mon:m,auto:au}},function(){loadAdminCfg()})}
function postAcfg(payload,cb){api('/api/admincfg',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)}).then(function(r){if(r.status===401){toast('Yetkisiz');return}return r.json()}).then(function(d){if(!d)return;toast(d.ok?(d.msg||'✅ OK'):('❌ '+(d.err||'Hata')));if(cb)cb(d)}).catch(function(){toast('Bağlantı hatası')})}
function saveNet(){postAcfg({net:{useStatic:$('useStatic').checked,ip:$('ipS').value.trim(),gw:$('gwS').value.trim(),mask:$('maskS').value.trim(),dns1:$('dns1S').value.trim(),dns2:$('dns2S').value.trim(),httpPort:parseInt($('portS').value||'80',10)}},function(d){if(d.nextUrl){toast('🔁 Yeni: '+d.nextUrl);setTimeout(function(){location.href=d.nextUrl},2500)}else if(d.reboot)toast('🔁 Yeniden başlıyor...')})}
function saveIlce(){postAcfg({ilceId:parseInt($('ilceId').value||'0',10)})}
function adminAdd(){var id=($('adminId').value||'').trim();if(!id){toast('ID gir');return}postAcfg({adminAdd:id},function(){loadAdminCfg()})}
function adminDel(){var id=($('adminId').value||'').trim();if(!id){toast('ID gir');return}postAcfg({adminDel:id},function(){loadAdminCfg()})}
function saveKimlik(){
  var payload={};
  var wk=$('newWebKey'),bt=$('newBotToken'),ci=$('newChatId');
  if(wk&&wk.value.trim())payload.creds={webKey:wk.value.trim()};
  if(bt&&bt.value.trim())payload.botToken=bt.value.trim();
  if(ci&&ci.value.trim())payload.chatId=ci.value.trim();
  if(!Object.keys(payload).length){toast('Değişiklik yok');return}
  postAcfg(payload,function(d){if(wk)wk.value='';if(bt)bt.value='';if(ci)ci.value='';loadAdminCfg()});
}
function clearBotToken(){if(!confirm('Bot Token NVS silinsin mi? secrets.h aktif olur.'))return;postAcfg({clearBotToken:true},function(){loadAdminCfg()})}
function clearChatId(){if(!confirm('Chat ID NVS silinsin mi? secrets.h aktif olur.'))return;postAcfg({clearChatId:true},function(){loadAdminCfg()})}
function clearWebKey(){if(!confirm('Web Şifre NVS silinsin mi? secrets.h aktif olur.'))return;postAcfg({creds:{clearWebKey:true}},function(){localStorage.removeItem('WEB_KEY');loadAdminCfg()})}
function wifiScan(){toast('📶 Taranıyor...');api('/api/wifiscan').then(function(r){if(r.status===401){toast('Yetkisiz');return}return r.json()}).then(function(d){if(!d)return;var sel=$('wfScanList');if(!sel)return;sel.innerHTML='<option value="">-- Ağ Seçin ('+d.count+') --</option>';if(d.networks){d.networks.sort(function(a,b){return b.rssi-a.rssi});d.networks.forEach(function(n){sel.innerHTML+='<option value="'+n.ssid+'">'+n.ssid+' ('+n.rssi+' dBm'+(n.enc?' 🔒':'')+')  </option>'})}sel.className='';toast(d.count+' ağ bulundu')}).catch(function(){toast('Tarama hatası')})}
function wfPickSsid(){var sel=$('wfScanList'),inp=$('wfSsid');if(sel&&inp&&sel.value){inp.value=sel.value;toast('SSID: '+sel.value)}}
function saveWifi(){
  var ss=($('wfSsid').value||'').trim();if(!ss){toast('SSID boş olamaz');return}
  var pw=$('wfPass');var ps=pw?pw.value:'';
  if(!confirm('WiFi test edilecek: '+ss))return;
  toast('📶 Bağlantı test ediliyor...');
  api('/api/wifitest',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ss,pass:ps})}).then(function(r){return r.json()}).then(function(d){if(d.testing){var pollCnt=0;var pollId=setInterval(function(){pollCnt++;api('/api/wifistatus').then(function(r){return r.json()}).then(function(s){if(s.state===3){clearInterval(pollId);toast('✅ '+s.msg);if(pw)pw.value='';if(s.ip)setTimeout(function(){location.href='http://'+s.ip},3000)}else if(s.state===5){clearInterval(pollId);toast('⛔ '+s.msg)}}).catch(function(){if(pollCnt>30)clearInterval(pollId)})},1000)}else{toast('⛔ '+(d.msg||'Hata'))}}).catch(function(){toast('❌ Ağ hatası')})
}
function clearWifi(){if(!confirm('WiFi NVS silinsin mi?'))return;postAcfg({creds:{clearWifi:true}},function(d){loadAdminCfg();if(d.reboot)setTimeout(function(){location.href='/'},2500)})}
var _rstOk=false,_rstKey='';
function rstCheck(){var k=($('rstKey').value||'').trim();if(!k){toast('Şifre gir');return}fetch('/api/authcheck',{headers:{'X-API-KEY':k}}).then(function(r){if(r.ok){_rstOk=true;_rstKey=k;$('rstWrap').className='';toast('✅ Doğrulandı')}else{_rstOk=false;toast('❌ Yanlış')}}).catch(function(){toast('Hata')})}
function rstDo(){if(!_rstOk){toast('Önce doğrula');return}if(!$('rstConfirm').checked){toast('Onay kutusunu işaretle');return}fetch('/api/factory_reset',{method:'POST',headers:{'Content-Type':'application/json','X-API-KEY':_rstKey},body:JSON.stringify({ownerKey:_rstKey,confirm:true})}).then(function(r){if(r.ok){toast('🧨 Sıfırlanıyor...');setTimeout(function(){location.href='/'},2500)}else toast('❌ Reddedildi')}).catch(function(){toast('Hata')})}
function uploadFw(){var inp=$('fwFile');if(!inp||!inp.files||!inp.files.length){toast('Dosya seç');return}if(!confirm('Firmware yüklensin mi?'))return;var f=inp.files[0];var reader=new FileReader();reader.onload=function(ev){var buf=new Uint8Array(ev.target.result);var btTag=[0x7F,67,65,77,73,95,66,84,58];var fwBt=-1;for(var i=0;i<buf.length-btTag.length-1;i++){var ok=true;for(var j=0;j<btTag.length;j++){if(buf[i+j]!==btTag[j]){ok=false;break}}if(ok){fwBt=buf[i+btTag.length]-48;break}}var devBt=(S.pub&&S.pub.boardType!==undefined)?S.pub.boardType:-1;if(fwBt>=0&&devBt>=0&&fwBt!==devBt){toast('⛔ Yanlis firmware! Dosya board='+fwBt+', Cihaz board='+devBt);return}if(fwBt<0){if(!confirm('Board bilgisi bulunamadi. Devam?'))return}var vnTag=[0x7E,67,65,77,73,95,86,78,58];var fwVer=-1;for(var i=0;i<buf.length-vnTag.length-2;i++){var ok=true;for(var j=0;j<vnTag.length;j++){if(buf[i+j]!==vnTag[j]){ok=false;break}}if(ok){var ns='';for(var k=i+vnTag.length;k<buf.length&&buf[k]>=48&&buf[k]<=57;k++)ns+=String.fromCharCode(buf[k]);fwVer=parseInt(ns)||0;break}}var devVer=(S.pub&&S.pub.fwVerNum)?S.pub.fwVerNum:0;if(fwVer>0&&devVer>0&&fwVer<=devVer){toast('⛔ Firmware zaten güncel veya eski (cihaz: '+devVer+', dosya: '+fwVer+')');return}var xhr=new XMLHttpRequest();xhr.open('POST','/update',true);var k=getKey();if(k)xhr.setRequestHeader('X-API-KEY',k);if(fwBt>=0)xhr.setRequestHeader('X-Board-Type',String(fwBt));if(fwVer>0)xhr.setRequestHeader('X-Firmware-Ver',String(fwVer));xhr.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded*100/e.total);var b=$('fwBar');if(b)b.style.width=p+'%';var s=$('fwStat');if(s)s.textContent=p+'%'}};xhr.onload=function(){if(xhr.status===200){toast('✅ Yüklendi');setTimeout(function(){location.href='/'},5000)}else if(xhr.status===401)toast('⛔ Yetkisiz');else{try{var r=JSON.parse(xhr.responseText);toast('⛔ '+(r.msg||r.err||'Hata'))}catch(e){toast('❌ Hata: '+xhr.status)}}};xhr.onerror=function(){toast('❌ Ağ hatası')};var fd=new FormData();fd.append('firmware',f,f.name);xhr.send(fd)};reader.readAsArrayBuffer(f)}

// ── Login ──
function renderLogin(){var h='<div class="cd" style="text-align:center;padding:36px 18px"><div style="font-size:44px;margin-bottom:14px">🔐</div><h3 style="margin-bottom:6px">Yönetim Paneli</h3><p style="color:var(--ts);font-size:13px;margin-bottom:18px">Ayarları değiştirmek için giriş yapın</p><div style="max-width:280px;margin:0 auto"><input type="password" id="loginKey" placeholder="Şifre" style="text-align:center;font-size:15px;margin-bottom:10px" onkeydown="if(event.key===\'Enter\')doLogin()"/><button class="btn btn-p" style="width:100%;justify-content:center;padding:11px" onclick="doLogin()">🔓 Giriş Yap</button></div></div>';$('content').innerHTML=h;var el=$('loginKey');if(el){el.value=getKey();el.focus()}}

// ── Render ──
function renderContent(){var t=S.tab;if(t==='home'){renderHome();return}if(t==='login'&&!S.authed){renderLogin();return}if(!S.authed){renderLogin();return}if(t==='system')renderSystem();else if(t==='tolerans')renderTolerans();else if(t==='dinigun')renderDiniGunler();else if(t==='komut')renderKomut();else if(t==='log')renderLog();else if(t==='ayarlar')renderAyarlar()}
function renderAll(){renderTabs();renderContent()}

// ── Init ──
(function(){
  S.key=getKey();S.dark=localStorage.getItem('DARK')==='1';if(S.dark)document.body.classList.add('dk');$('darkBtn').textContent=S.dark?'☀️':'🌙';
  if(S.key){api('/api/authcheck').then(function(r){if(r.ok){S.authed=true;if(location.pathname==='/admin')S.tab='system';renderAll();loadSettings();loadAdminCfg()}else renderAll()}).catch(function(){renderAll()})}else renderAll();
  loadPublic();startAutoRef();
})();
</script></body></html>

)HTML";

static const char WEB_ADMIN_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><script>location.href="/";</script></head><body></body></html>
)HTML";

// =====================
// Web handlers
// =====================
static void webHandleRoot() {
  g_web->send_P(200, "text/html; charset=utf-8", WEB_PUBLIC_HTML);
}

static void webHandleAdmin() {
  g_web->send_P(200, "text/html; charset=utf-8", WEB_ADMIN_HTML);
}

// Public API (auth yok) - sadece durum
static void webHandlePublic() {
  DynamicJsonDocument doc(2048);
  doc["ok"]   = true;
  doc["version"] = String(APP_VERSION) + " (" + BOARD_NAME + ")";
  doc["boardType"] = BOARD_TYPE;
  doc["fwVerNum"] = FW_VER_NUM;
  doc["author"]  = APP_AUTHOR;
  doc["relay"]= g_relayState;
  doc["ip"]   = WiFi.localIP().toString();
  doc["ssid"] = getWiFiSsidForUi();
  doc["httpPort"] = (int)g_httpPort;
  doc["baseUrl"] = makeBaseUrlForCurrent();
  doc["now"]  = isTimeValid() ? nowStamp() : String("NO_TIME");

  // Debug/teşhis: web panelden ayarların etkisini görebilmek için
  doc["timeValid"] = isTimeValid();
  doc["dayCount"]  = (int)g_dayCount;
  doc["onTolMin"]  = (int)(g_onOffsetSec / 60);
  doc["offTolMin"] = (int)(g_offOffsetSec / 60);
  doc["ramazanAll"] = g_enableRamazanAll;
  doc["spMask"]    = (uint32_t)(g_spEnableMask & spValidMask());
  doc["ilceId"]    = (uint32_t)g_ilceId;
  doc["lastUpdYmd"] = (uint32_t)prefs.getUInt("lastUpdYmd", 0);

  // Sistem sağlığı
  doc["freeHeap"]    = (uint32_t)ESP.getFreeHeap();
  doc["minFreeHeap"] = (uint32_t)ESP.getMinFreeHeap();
  doc["uptimeSec"]   = (uint32_t)(millis() / 1000);
  doc["rssi"]        = (int)WiFi.RSSI();
  doc["cpuUsage"]    = (int)(g_cpuUsageTotal + 0.5f);
  doc["heapTotal"]   = (uint32_t)ESP.getHeapSize();
  doc["updatePending"]    = g_updatePending;
  doc["updateInProgress"] = g_updateInProgress;

  // Bugünün namaz vakitleri (mevcut: imsak + akşam)
  int todayIdx = findIdx(ymdToday());
  if (todayIdx >= 0) {
    doc["imsak"] = minToHhmm(g_days[todayIdx].imsakMin);
    doc["aksam"] = minToHhmm(g_days[todayIdx].aksamMin);
    if (g_days[todayIdx].hicriUzun[0] != '\0') {
      doc["hicri"] = String(g_days[todayIdx].hicriUzun);
    }
  }

  String out;
  serializeJson(doc, out);
  g_web->send(200, "application/json", out);
}


static int defaultHicriYearForSpecial(uint8_t spIdx) {
  if (spIdx >= SPECIAL_COUNT) return HICRI_YEAR_MIN;
  int d = (int)g_specials[spIdx].day;
  String m = normMonthKey(String(g_specials[spIdx].monthKey));
  for (uint16_t i = 0; i < g_dayCount; i++) {
    int hd=0, hy=0; String hm;
    if (!parseHicri(g_days[i].hicriUzun, hd, hm, hy)) continue;
    if (hd == d && normMonthKey(hm) == m) {
      if (hy >= HICRI_YEAR_MIN && hy <= HICRI_YEAR_MAX) return hy;
    }
  }
  return HICRI_YEAR_MIN;
}

static void webHandleGetSettings() {
  if (!webRequireAuth()) return;

  DynamicJsonDocument doc(8192);
  doc["ok"] = true;
  doc["onTolMin"]  = (int)(g_onOffsetSec / 60);
  doc["offTolMin"] = (int)(g_offOffsetSec / 60);
  doc["ramazanAll"] = g_enableRamazanAll;
  doc["ip"] = WiFi.localIP().toString();
  doc["ssid"] = getWiFiSsidForUi();
  doc["httpPort"] = (int)g_httpPort;
  doc["baseUrl"] = makeBaseUrlForCurrent();

    JsonArray arr = doc.createNestedArray("specials");
  for (uint8_t i = 0; i < SPECIAL_COUNT; i++) {
    JsonObject o = arr.createNestedObject();
    o["id"]   = (int)i;
    o["name"] = g_specials[i].name;
    o["en"]   = isSpecialEnabled(i);

    // Hicri tarih override UI (Dini Günler sayfası)
    const int defDay   = (int)g_specials[i].day;
    int defMonIdx      = monthIndexFromKey(String(g_specials[i].monthKey));
    if (defMonIdx < 0) defMonIdx = 0;
    const int defYear  = defaultHicriYearForSpecial(i);

    const bool useDef  = (g_spOv[i].useDefault != 0);
    int hDay   = useDef ? defDay    : (int)g_spOv[i].day;
    int hMonth = useDef ? defMonIdx : (int)g_spOv[i].month;
    int hYear  = useDef ? defYear   : (int)g_spOv[i].year;

    // clamp (UI tarafı zaten sınırlar ama güvenli olsun)
    if (hDay < 1) hDay = 1; if (hDay > 30) hDay = 30;
    if (hMonth < 0) hMonth = 0; if (hMonth > 11) hMonth = 11;
    if (hYear < HICRI_YEAR_MIN || hYear > HICRI_YEAR_MAX) hYear = defYear;

    o["useDefault"] = useDef;
    o["hDay"]   = hDay;
    o["hMonth"] = hMonth;
    o["hYear"]  = hYear;
    o["defDay"]   = defDay;
    o["defMonth"] = defMonIdx;
    o["defYear"]  = defYear;
  }
String out;
  serializeJson(doc, out);
  g_web->send(200, "application/json", out);
}

static void webHandlePostSettings() {
  if (!webRequireAuth()) return;

  String body = g_web->arg("plain");
  if (body.length() == 0) { g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"empty\"}"); return; }

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"json\"}");
    return;
  }

  int onTolMin  = doc["onTolMin"]  | (g_onOffsetSec/60);
  int offTolMin = doc["offTolMin"] | (g_offOffsetSec/60);
  bool ramAll   = doc["ramazanAll"] | g_enableRamazanAll;

  // clamp 0..30 dk
  if (onTolMin  < 0) onTolMin = 0;  if (onTolMin  > 30) onTolMin  = 30;
  if (offTolMin < 0) offTolMin= 0;  if (offTolMin > 30) offTolMin = 30;

  g_onOffsetSec  = onTolMin * 60;
  g_offOffsetSec = offTolMin * 60;

  // specials array
  if (doc.containsKey("specials")) {
    uint32_t mask = 0;

    for (JsonObject it : doc["specials"].as<JsonArray>()) {
      int  id  = it["id"] | -1;
      bool en  = it["en"] | false;
      bool useDef = it["useDefault"] | true;

      int hDay   = it["hDay"] | 0;
      int hMonth = it["hMonth"] | 0;
      int hYear  = it["hYear"] | HICRI_YEAR_MIN;

      if (id < 0 || id >= (int)SPECIAL_COUNT) continue;

      if (en) mask |= (1u << (uint8_t)id);

      // Varsayılan işaretliyse: main.cpp sabit tarih (g_specials) kullanılır
      if (useDef) {
        g_spOv[id].useDefault = 1;
        g_spOv[id].day  = g_specials[id].day;
        int dm = monthIndexFromKey(g_specials[id].monthKey);
        if (dm < 0) dm = 0;
        g_spOv[id].month = (uint8_t)dm;
        g_spOv[id].year  = 0;
      } else {
        // Custom: kullanıcı gün/ay/yıl seçti
        if (hDay < 1 || hDay > 30) continue;
        if (hMonth < 0 || hMonth > 11) continue;
        if (hYear < HICRI_YEAR_MIN || hYear > HICRI_YEAR_MAX) continue;

        g_spOv[id].useDefault = 0;
        g_spOv[id].day  = (uint8_t)hDay;
        g_spOv[id].month = (uint8_t)hMonth;
        g_spOv[id].year  = (uint16_t)hYear;
      }
    }

    g_spEnableMask = (mask & spValidMask());
  }

  g_enableRamazanAll = ramAll;

  String err1, err2;
  bool ok1 = saveOffsetsToNvs(&err1);
  bool ok2 = saveSpecialEnableToNvs(&err2);
  String err3;
  bool ok3 = saveSpecialOverrideToNvs(&err3);

#ifndef NVS_SKIP_READBACK
  // NVS read-back (teşhis - production'da NVS_SKIP_READBACK tanımlanabilir)
  int rbOn  = prefs.getInt(NVS_KEY_ONOFFS,  -99999);
  int rbOff = prefs.getInt(NVS_KEY_OFFOFFS, -99999);
  uint8_t rbVer = prefs.getUChar(NVS_KEY_SP_VER, 0);
  uint32_t rbMask = prefs.getUInt(NVS_KEY_SP_MASK, 0) & spValidMask();
  bool rbRam = prefs.getBool(NVS_KEY_SP_RAM, false);

  uint8_t rbOvVer = prefs.getUChar(NVS_KEY_SP_OV_VER, 0);
  SpOverride rbOv[SPECIAL_COUNT];
  size_t rbOvGot = prefs.getBytes(NVS_KEY_SP_OV_BLOB, rbOv, sizeof(rbOv));
  bool rbOvMatch = (rbOvVer == SP_OVERRIDE_VER) && (rbOvGot == sizeof(rbOv)) && (memcmp(rbOv, g_spOv, sizeof(rbOv)) == 0);

  bool match =
    (rbOn == g_onOffsetSec) &&
    (rbOff == g_offOffsetSec) &&
    (rbVer == SP_SETTINGS_VER) &&
    (rbMask == (g_spEnableMask & spValidMask())) &&
    (rbRam == g_enableRamazanAll) &&
    rbOvMatch;
#else
  bool match = true;
#endif

  // Çizelgeleri tekrar hesapla (zaman geçerliyse hemen etkiler)
  recomputeAllSchedules();

  DynamicJsonDocument out(768);
  out["ok"] = (ok1 && ok2 && ok3 && match);
  if (!out["ok"].as<bool>()) {
    out["err"] = "nvs";
    out["detail"] = (err1.length() ? err1 : (err2.length() ? err2 : (err3.length() ? err3 : "readback_mismatch")));
  }
  JsonObject saved = out.createNestedObject("saved");
  saved["onTolMin"] = (int)(g_onOffsetSec / 60);
  saved["offTolMin"] = (int)(g_offOffsetSec / 60);
  saved["ramazanAll"] = g_enableRamazanAll;
  saved["spMask"] = (uint32_t)(g_spEnableMask & spValidMask());

#ifndef NVS_SKIP_READBACK
  JsonObject rb = out.createNestedObject("nvs");
  rb["onOffs"] = rbOn;
  rb["offOffs"] = rbOff;
  rb["spVer"] = rbVer;
  rb["spMask"] = rbMask;
  rb["spOvVer"] = rbOvVer;
  rb["spOvMatch"] = rbOvMatch;
  rb["spRam"] = rbRam;
#endif

  String outStr;
  serializeJson(out, outStr);
  g_web->send(200, "application/json", outStr);

  Serial.print("[WEB] /api/settings saved=");
  Serial.print(out["ok"].as<bool>() ? "OK" : "FAIL");
  Serial.print(" onTolMin=");
  Serial.print((int)(g_onOffsetSec/60));
  Serial.print(" offTolMin=");
  Serial.print((int)(g_offOffsetSec/60));
  Serial.print(" spMask=0x");
  Serial.print(String((uint32_t)(g_spEnableMask & spValidMask()), HEX));
  Serial.print(" ramAll=");
  Serial.println(g_enableRamazanAll ? "1" : "0");

  // Log: neyin değiştiğini kaydet
  if (doc.containsKey("specials")) {
    logUser("WEB: Dini gun ayarlari kaydedildi");
  } else {
    logUser("WEB: Tolerans kaydedildi (" + String((int)(g_onOffsetSec/60)) + "/" + String((int)(g_offOffsetSec/60)) + "dk)");
  }
}



static void webHandleGetDiniGunler() {
  if (!webRequireAuth()) return;
  String s = buildDiniGunlerWebText();
  g_web->send(200, "text/plain; charset=utf-8", s);
}

static void webHandleGetAdminCfg() {
  if (!webRequireAuth()) return;

  DynamicJsonDocument doc(4096);
  doc["ok"] = true;
  doc["ilceId"] = (uint32_t)g_ilceId;

  JsonObject net = doc.createNestedObject("net");
  net["useStatic"] = g_netCfg.useStatic;
  net["ip"]   = (g_netCfg.useStatic && g_netCfg.ip)   ? u32ToIp(g_netCfg.ip).toString()   : String("");
  net["gw"]   = (g_netCfg.useStatic && g_netCfg.gw)   ? u32ToIp(g_netCfg.gw).toString()   : String("");
  net["mask"] = (g_netCfg.useStatic && g_netCfg.mask) ? u32ToIp(g_netCfg.mask).toString() : String("");
  net["dns1"] = (g_netCfg.dns1) ? u32ToIp(g_netCfg.dns1).toString() : String("");
  net["dns2"] = (g_netCfg.dns2) ? u32ToIp(g_netCfg.dns2).toString() : String("");
  net["httpPort"] = (uint16_t)g_httpPort;

  JsonArray admins = doc.createNestedArray("admins");
  for (uint8_t i = 0; i < g_adminCount; i++) {
    JsonObject a = admins.createNestedObject();
    a["id"] = String((long long)g_adminIds[i]);   // string -> JS güvenli
    a["owner"] = (g_adminIds[i] == OWNER_ADMIN_ID);
  }

  // Canlı WiFi bilgileri (DHCP/statik farketmez)
  net["liveIp"]   = WiFi.localIP().toString();
  net["liveGw"]   = WiFi.gatewayIP().toString();
  net["liveMask"] = WiFi.subnetMask().toString();
  net["liveDns1"] = WiFi.dnsIP(0).toString();
  net["liveDns2"] = WiFi.dnsIP(1).toString();

  // creds (WiFi/Web) - şifreleri döndürmeyiz, sadece durum bilgisi
  JsonObject creds = doc.createNestedObject("creds");
  creds["wifiSrc"] = g_wifiSrc;
  creds["webKeySrc"] = g_webKeySrc;
  creds["activeWifiSsid"] = g_wifiSsid;
  creds["activeWebKeySet"] = (g_webKey.length() > 0);
  creds["botTokenSrc"] = g_botTokenSrc;
  creds["chatIdSrc"] = g_chatIdSrc;
  creds["chatId"] = g_activeChatId;
  creds["botTokenSet"] = (g_botToken.length() > 0);

  String nvsSsid = prefs.getString(NVS_KEY_WIFI_SSID, "");
  nvsSsid.trim();
  creds["nvsWifiSsid"] = nvsSsid;

  String nvsPass = prefs.getString(NVS_KEY_WIFI_PASS, "");
  creds["nvsHasWifiPass"] = (nvsPass.length() > 0);

  String nvsWk = prefs.getString(NVS_KEY_WEB_KEY, "");
  nvsWk.trim();
  creds["nvsHasWebKey"] = (nvsWk.length() > 0);

  creds["secretsWifiSet"]   = !isEmptyCstr(SECRET_WIFI_SSID);
  creds["secretsWebKeySet"] = !isEmptyCstr(SECRET_WEB_KEY);
  JsonObject hny = doc.createNestedObject("hicriYear");
  hny["day"]  = (int)g_hnyDay;
  hny["mon"]  = (int)g_hnyMon;
  hny["auto"] = g_autoHicriYear;
  hny["lastYear"] = (int)g_hnyLastYear;

  String out;
  serializeJson(doc, out);
  g_web->send(200, "application/json", out);
}

static bool parseIpString(const String& s, uint32_t& outPacked) {
  String t = s; t.trim();
  if (t.length() == 0) { outPacked = 0; return true; }
  IPAddress ip;
  if (!ip.fromString(t)) return false;
  outPacked = ipToU32(ip);
  return true;
}

static void webHandlePostAdminCfg() {
  if (!webRequireAuth()) return;

  String body = g_web->arg("plain");
  if (body.length() == 0) { g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"empty\"}"); return; }

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, body);
  if (err) { g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"json\"}"); return; }

  bool needReboot = false;
  String msg = "";

  // ---- İlçe ID ----
  if (doc.containsKey("ilceId")) {
    uint32_t id = (uint32_t)(doc["ilceId"] | (int)g_ilceId);
    if (id < 1 || id > 999999) {
      g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"ilce\"}");
      return;
    }
    if (id != g_ilceId) {
      g_ilceId = id;
      String nvsErr;
      if (!saveIlceToNvs(&nvsErr)) { g_web->send(500, "application/json", "{\"ok\":false,\"err\":\"nvs\"}"); return; }

      // Eski vakit cache'i bu ilçe ile uyumsuz -> temizle
      prefs.remove("dayCount");
      prefs.remove("daysBlob");
      prefs.remove("lastUpdYmd");
      g_dayCount = 0;

      // schedule reset
      g_thuOnTs = g_thuOffTs = 0;
      g_spOnTs = g_spOffTs = 0;
      g_spName = ""; g_spHicri = ""; g_spYmd = 0;
      g_nextImsakOffTs = 0;

      msg += "İlçe ID güncellendi. ";
      logUser("WEB: Ilce ID degistirildi (" + String(g_ilceId) + ")");
    }
  }

  // ---- Ağ (Statik IP) ----
  if (doc.containsKey("net")) {
    JsonObject n = doc["net"].as<JsonObject>();
    NetCfg nc = g_netCfg;

    nc.useStatic = (bool)(n["useStatic"] | nc.useStatic);

    String sIp   = String((const char*)(n["ip"]   | ""));
    String sGw   = String((const char*)(n["gw"]   | ""));
    String sMask = String((const char*)(n["mask"] | ""));
    String sD1   = String((const char*)(n["dns1"] | ""));
    String sD2   = String((const char*)(n["dns2"] | ""));
    int httpPort = (int)(n["httpPort"] | (int)g_httpPort);

    uint32_t pip=nc.ip, pgw=nc.gw, pm=nc.mask, pd1=nc.dns1, pd2=nc.dns2;
    if (!parseIpString(sIp, pip) || !parseIpString(sGw, pgw) || !parseIpString(sMask, pm) ||
        !parseIpString(sD1, pd1) || !parseIpString(sD2, pd2)) {
      g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"ip\"}");
      return;
    }
    nc.ip = pip; nc.gw = pgw; nc.mask = pm; nc.dns1 = pd1; nc.dns2 = pd2;
    if (httpPort < 1) httpPort = 80; if (httpPort > 65535) httpPort = 65535;

    if (nc.useStatic) {
      if (nc.ip == 0 || nc.gw == 0 || nc.mask == 0) {
        g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"net\"}");
        return;
      }
    }

    bool changed = (nc.useStatic != g_netCfg.useStatic) ||
                   (nc.ip != g_netCfg.ip) || (nc.gw != g_netCfg.gw) || (nc.mask != g_netCfg.mask) ||
                   (nc.dns1 != g_netCfg.dns1) || (nc.dns2 != g_netCfg.dns2);

    if (changed) {
      g_netCfg = nc;
      String nvsErr;
      if (!saveNetToNvs(&nvsErr)) { g_web->send(500, "application/json", "{\"ok\":false,\"err\":\"nvs\"}"); return; }
      needReboot = true;
      msg += "Ağ ayarı kaydedildi. ";
      logUser("WEB: Ag ayari degistirildi");
      scheduleRestart(1500, "Net config changed (web)");
    }

    // HTTP port
    if ((uint16_t)httpPort != g_httpPort) {
      String nvsErr;
      if (!saveHttpPortToNvs((uint16_t)httpPort, &nvsErr)) { g_web->send(500, "application/json", "{\"ok\":false,\"err\":\"nvs\"}"); return; }
      needReboot = true;
      msg += "Port kaydedildi. ";
      scheduleRestart(1500, "HTTP port changed (web)");
    }
  }


// ---- WiFi / WEB_KEY (NVS) ----
bool wifiCredChanged = false;
bool webKeyChanged = false;
if (doc.containsKey("creds")) {
  JsonObject c = doc["creds"].as<JsonObject>();

  if ((bool)(c["clearWifi"] | false)) {
    prefs.remove(NVS_KEY_WIFI_SSID);
    prefs.remove(NVS_KEY_WIFI_PASS);
    wifiCredChanged = true;
    msg += "WiFi NVS silindi. ";
    logUser("WEB: WiFi bilgileri silindi");
  }
  if ((bool)(c["clearWebKey"] | false)) {
    prefs.remove(NVS_KEY_WEB_KEY);
    webKeyChanged = true;
    msg += "Web şifre silindi. ";
    logUser("WEB: Web sifre silindi");
  }

  if (c.containsKey("wifiSsid")) {
    String ss = String((const char*)(c["wifiSsid"] | ""));
    ss.trim();
    if (ss.length() > 0) {
      prefs.putString(NVS_KEY_WIFI_SSID, ss);
      wifiCredChanged = true;
      msg += "WiFi SSID kaydedildi. ";
      logUser("WEB: WiFi SSID degistirildi");
    }
  }

  if (c.containsKey("wifiPass")) {
    String pw = String((const char*)(c["wifiPass"] | ""));
    pw.trim();
    if (pw.length() > 0) {
      prefs.putString(NVS_KEY_WIFI_PASS, pw);
      msg += "WiFi şifre kaydedildi. ";
    } else {
      prefs.remove(NVS_KEY_WIFI_PASS);
      msg += "WiFi şifre temizlendi. ";
    }
    wifiCredChanged = true;
  }

  if (c.containsKey("webKey")) {
    String wk = String((const char*)(c["webKey"] | ""));
    wk.trim();
    if (wk.length() > 0) {
      prefs.putString(NVS_KEY_WEB_KEY, wk);
      msg += "Web şifre kaydedildi. ";
    } else {
      prefs.remove(NVS_KEY_WEB_KEY);
      msg += "Web şifre temizlendi. ";
    }
    webKeyChanged = true;
  }

  if (wifiCredChanged || webKeyChanged) {
    // secrets öncelikli; secrets boşsa NVS değerleri RAM'e alınır
    loadWiFiWebKeyFromNvsFallback();

    // WiFi: secrets boşsa, NVS değişikliği ancak reboot ile temiz uygulanır
    if (wifiCredChanged && isEmptyCstr(SECRET_WIFI_SSID)) {
      needReboot = true;
      scheduleRestart(1500, "WiFi creds changed (web)");
    }
    // WEB_KEY: secrets boşsa RAM'de anında güncellendi (reboot şart değil)
  }
}

  // ---- Hicri Yil Ayarlari ----
  if (doc.containsKey("hicriYear")) {
    JsonObject hy = doc["hicriYear"].as<JsonObject>();
    int d = hy["day"] | (int)g_hnyDay;
    int m = hy["mon"] | (int)g_hnyMon;
    bool au = hy["auto"] | g_autoHicriYear;
    if (d < 1) d = 1; if (d > 30) d = 30;
    if (m < 0) m = 0; if (m > 11) m = 11;
    g_hnyDay = (uint8_t)d;
    g_hnyMon = (uint8_t)m;
    g_autoHicriYear = au;
    prefs.putUChar(NVS_KEY_HNY_DAY, g_hnyDay);
    prefs.putUChar(NVS_KEY_HNY_MON, g_hnyMon);
    prefs.putBool(NVS_KEY_AUTO_HYR, g_autoHicriYear);
    msg += "Hicri yil ayari kaydedildi. ";
    logUser("WEB: Hicri yil ayari kaydedildi");
  }

  // ---- Bot Token / Chat ID (NVS) ----
  if (doc.containsKey("botToken")) {
    String bt = String((const char*)(doc["botToken"] | ""));
    bt.trim();
    if (bt.length() > 10) {
      prefs.putString(NVS_KEY_BOT_TOKEN, bt);
      reinitBot(bt);
      g_botTokenSrc = "nvs";
      msg += "Bot Token kaydedildi. ";
      logUser("WEB: Bot Token degistirildi");
    }
  }
  if ((bool)(doc["clearBotToken"] | false)) {
    prefs.remove(NVS_KEY_BOT_TOKEN);
    reinitBot(String(SECRET_BOT_TOKEN));
    g_botTokenSrc = "secrets";
    msg += "Bot Token NVS silindi (secrets aktif). ";
    logUser("WEB: Bot Token silindi");
  }

  if (doc.containsKey("chatId")) {
    String ci = String((const char*)(doc["chatId"] | ""));
    ci.trim();
    if (ci.length() > 0) {
      prefs.putString(NVS_KEY_CHAT_ID, ci);
      g_activeChatId = ci;
      prefs.putString(NVS_KEY_ACTIVE_CHAT, ci);
      g_chatIdSrc = "nvs";
      msg += "Chat ID kaydedildi. ";
      logUser("WEB: Chat ID degistirildi");
    }
  }
  if ((bool)(doc["clearChatId"] | false)) {
    prefs.remove(NVS_KEY_CHAT_ID);
    g_activeChatId = String(CHAT_ID);
    prefs.putString(NVS_KEY_ACTIVE_CHAT, g_activeChatId);
    g_chatIdSrc = "secrets";
    msg += "Chat ID NVS silindi (secrets aktif). ";
    logUser("WEB: Chat ID silindi");
  }

  // ---- Admin add/del ----
  if (doc.containsKey("adminAdd")) {
    String s = String((const char*)(doc["adminAdd"] | ""));
    s.trim();
    int64_t id = atoll(s.c_str());
    if (id <= 0) {
      g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"admin_add\"}");
      return;
    }
    if (!addAdmin(id)) {
      g_web->send(200, "application/json", "{\"ok\":false,\"err\":\"admin_add_fail\"}");
      return;
    }
    msg += "Admin eklendi. ";
    logUser("WEB: Admin eklendi");
  }

  if (doc.containsKey("adminDel")) {
    String s = String((const char*)(doc["adminDel"] | ""));
    s.trim();
    int64_t id = atoll(s.c_str());
    if (id <= 0) {
      g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"admin_del\"}");
      return;
    }
    if (!delAdmin(id)) {
      g_web->send(200, "application/json", "{\"ok\":false,\"err\":\"admin_del_fail\"}");
      return;
    }
    msg += "Admin silindi. ";
    logUser("WEB: Admin silindi");
  }

  if (msg.length() == 0) msg = "OK";

  DynamicJsonDocument out(512);
  out["ok"] = true;
  out["msg"] = msg;
  out["reboot"] = needReboot;
  out["wifiSrc"] = g_wifiSrc;
  out["webKeySrc"] = g_webKeySrc;
  out["baseUrl"] = makeBaseUrlForCurrent();
  if (needReboot) out["nextUrl"] = makeBaseUrl((g_netCfg.useStatic && g_netCfg.ip!=0)?u32ToIp(g_netCfg.ip):WiFi.localIP(), g_httpPort);

  String outStr;
  serializeJson(out, outStr);
  g_web->send(200, "application/json", outStr);

  // Eğer reboot planlandıysa loop'ta yapılacak (burada bloklama yok)
}

static void webHandleLogs() {
  if (!webRequireAuth()) return;
  DynamicJsonDocument doc(4096);
  doc["ok"] = true;
  JsonArray ua = doc.createNestedArray("user");
  for (int i = 0; i < g_userLogCnt; i++) {
    int ri = (g_userLogIdx - g_userLogCnt + i + LOG_MAX) % LOG_MAX;
    JsonObject o = ua.createNestedObject();
    o["ts"] = String(g_userLog[ri].ts);
    o["msg"] = String(g_userLog[ri].msg);
  }
  JsonArray sa = doc.createNestedArray("sys");
  for (int i = 0; i < g_sysLogCnt; i++) {
    int ri = (g_sysLogIdx - g_sysLogCnt + i + LOG_MAX) % LOG_MAX;
    JsonObject o = sa.createNestedObject();
    o["ts"] = String(g_sysLog[ri].ts);
    o["msg"] = String(g_sysLog[ri].msg);
  }
  String out;
  serializeJson(doc, out);
  g_web->send(200, "application/json", out);
}

// WiFi test state machine (non-blocking)
static int      g_wfTestState = 0; // 0=idle,1=disconnecting,2=connecting,3=ok,4=restoring,5=fail
static String   g_wfTestSsid, g_wfTestPass, g_wfTestOldSsid, g_wfTestOldPass;
static uint32_t g_wfTestStartMs = 0;
static String   g_wfTestNewIp;

static void webHandleWifiTest() {
  if (!webRequireAuth()) return;

  if (g_wfTestState != 0) {
    g_web->send(409, "application/json", "{\"ok\":false,\"err\":\"busy\",\"msg\":\"WiFi testi devam ediyor\"}");
    return;
  }

  DynamicJsonDocument doc(512);
  if (!webParseBody(doc)) return;

  g_wfTestSsid = String((const char*)(doc["ssid"] | ""));
  g_wfTestPass = String((const char*)(doc["pass"] | ""));
  g_wfTestSsid.trim(); g_wfTestPass.trim();
  if (g_wfTestSsid.length() == 0) {
    g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"ssid\"}");
    return;
  }

  // Eski WiFi bilgilerini yedekle
  g_wfTestOldSsid = g_wifiSsid;
  g_wfTestOldPass = g_wifiPass;
  g_wfTestNewIp = "";

  Serial.printf("[WIFI-TEST] Baslatiliyor: %s\n", g_wfTestSsid.c_str());
  g_wfTestState = 1;
  g_wfTestStartMs = millis();

  g_web->send(200, "application/json", "{\"ok\":true,\"msg\":\"WiFi testi baslatildi\",\"testing\":true}");
}

static void webHandleWifiStatus() {
  if (!webRequireAuth()) return;
  DynamicJsonDocument out(256);
  out["state"] = g_wfTestState;
  if (g_wfTestState == 3) {
    out["ok"] = true;
    out["msg"] = "WiFi baglantisi basarili! Yeniden baslatiliyor...";
    out["ip"] = g_wfTestNewIp;
    out["reboot"] = true;
  } else if (g_wfTestState == 5) {
    out["ok"] = false;
    out["msg"] = "WiFi baglantisi basarisiz! Parola hatali veya ag bulunamadi.";
  } else {
    out["ok"] = true;
    out["msg"] = "Test devam ediyor...";
  }
  String s;
  serializeJson(out, s);
  g_web->send(200, "application/json", s);
}

static void wifiTestTick() {
  if (g_wfTestState == 0) return;

  switch (g_wfTestState) {
    case 1: // Disconnect
      WiFi.disconnect(true);
      g_wfTestStartMs = millis();
      g_wfTestState = 2;
      WiFi.begin(g_wfTestSsid.c_str(), g_wfTestPass.c_str());
      Serial.printf("[WIFI-TEST] Deneniyor: %s\n", g_wfTestSsid.c_str());
      break;

    case 2: // Bağlanmayı bekle (12sn timeout)
      if (WiFi.status() == WL_CONNECTED) {
        g_wfTestNewIp = WiFi.localIP().toString();
        Serial.printf("[WIFI-TEST] BASARILI! IP=%s\n", g_wfTestNewIp.c_str());

        prefs.putString(NVS_KEY_WIFI_SSID, g_wfTestSsid);
        if (g_wfTestPass.length() > 0) prefs.putString(NVS_KEY_WIFI_PASS, g_wfTestPass);
        else prefs.remove(NVS_KEY_WIFI_PASS);

        logUser("WEB: WiFi degistirildi (" + g_wfTestSsid + ")");
        logSys("WiFi test OK: " + g_wfTestSsid);
        g_wfTestState = 3;
        scheduleRestart(3000, "WiFi test OK - reboot");
      } else if (millis() - g_wfTestStartMs > 12000) {
        Serial.println("[WIFI-TEST] BASARISIZ - eski WiFi'ye donuluyor");
        logSys("WiFi test FAIL: " + g_wfTestSsid);
        WiFi.disconnect(true);
        g_wfTestStartMs = millis();
        g_wfTestState = 4;
        WiFi.begin(g_wfTestOldSsid.c_str(), g_wfTestOldPass.c_str());
      }
      break;

    case 4: // Eski WiFi'ye dönüş bekle (8sn)
      if (WiFi.status() == WL_CONNECTED || millis() - g_wfTestStartMs > 8000) {
        g_wfTestState = 5; // Sonuç: fail
      }
      break;

    case 5: // Fail — 10sn sonra idle'a dön
      if (millis() - g_wfTestStartMs > 18000) { // totalde ~20sn sonra
        g_wfTestState = 0;
        g_wfTestSsid = ""; g_wfTestPass = "";
        g_wfTestOldSsid = ""; g_wfTestOldPass = "";
      }
      break;
  }
}

static void webHandleWifiScan() {
  if (!webRequireAuth()) return;
  esp_task_wdt_reset(); // Scan uzun sürebilir, WDT besle
  int n = WiFi.scanNetworks(false, false, false, 200); // 200ms/kanal
  esp_task_wdt_reset();
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray("networks");
  if (n > 0) {
    for (int i = 0; i < n && i < 20; i++) {
      JsonObject o = arr.createNestedObject();
      o["ssid"] = WiFi.SSID(i);
      o["rssi"] = WiFi.RSSI(i);
      o["enc"]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
  }
  doc["count"] = n > 0 ? n : 0;
  WiFi.scanDelete();
  String out;
  serializeJson(doc, out);
  g_web->send(200, "application/json", out);
}

static void webHandlePostAction() {
  if (!webRequireAuth()) return;

  String body = g_web->arg("plain");
  if (body.length() == 0) { g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"empty\"}"); return; }

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, body);
  if (err) { g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"json\"}"); return; }

  String cmd = String((const char*)(doc["cmd"] | ""));
  cmd.trim();

  DynamicJsonDocument out(1024);
  out["ok"] = false;

  if (cmd == "recompute") {
    recomputeAllSchedules();
    out["ok"] = true;
    out["msg"] = "✅ Çizelge yenilendi";
  }
  else if (cmd == "updateTimes") {
    if (g_updateInProgress) {
      out["err"] = "busy";
      out["msg"] = "⏳ Zaten güncelleme çalışıyor";
    } else if (g_updatePending) {
      out["err"] = "queued";
      out["msg"] = "⏳ Zaten kuyrukta";
    } else if (g_updateCooldownUntilMs != 0 && !millisPassed(g_updateCooldownUntilMs)) {
      out["err"] = "cooldown";
      out["msg"] = "⏳ Cooldown aktif";
    } else {
      g_updatePending = true;
      g_updateRequesterWho = "(WEB)";
      out["ok"] = true;
      out["msg"] = "✅ Vakit güncelleme kuyruğa alındı";
      logUser("WEB: Vakit guncelleme istendi");
    }
  }
  else if (cmd == "relayOn") {
    int rc = manualRelayOn("WEB");
    if (rc == 1) {
      out["err"] = "blocked";
      out["msg"] = "⛔ ON engelli (zorunlu OFF sonrası)";
    } else {
      out["ok"] = true;
      out["msg"] = "🟢 Şerefeler AÇILDI";
    }
  }
  else if (cmd == "relayOff") {
    manualRelayOff("WEB");
    out["ok"] = true;
    out["msg"] = "🔴 Şerefeler KAPANDI";
  }
  else if (cmd == "cancelUpdate") {
    if (g_updatePending) {
      g_updatePending = false;
      out["ok"] = true;
      out["msg"] = "Guncelleme iptal edildi";
    } else {
      out["err"] = "nothing";
      out["msg"] = "Kuyrukta guncelleme yok";
    }
  }
  else if (cmd == "reboot") {
    out["ok"] = true;
    out["msg"] = "Sistem yeniden baslatiliyor...";
    logUser("WEB: Reboot istendi");
    String outStr;
    serializeJson(out, outStr);
    g_web->send(200, "application/json", outStr);
    delay(500);
    ESP.restart();
    return;
  }
  else {
    out["err"] = "unknown";
    out["msg"] = "Bilinmeyen cmd";
  }

  String s;
  serializeJson(out, s);
  g_web->send(200, "application/json", s);
}


static void webHandlePostFactoryReset() {
  if (!webRequireAuth()) return;

  String body = g_web->arg("plain");
  if (body.length() == 0) { g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"empty\"}"); return; }

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, body);
  if (err) { g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"json\"}"); return; }

  String ownerKey = String((const char*)(doc["ownerKey"] | ""));
  ownerKey.trim();
  bool confirm = (bool)(doc["confirm"] | false);

  if (ownerKey != g_webKey) { g_web->send(403, "application/json", "{\"ok\":false,\"err\":\"bad_key\"}"); return; }
  if (!confirm) { g_web->send(400, "application/json", "{\"ok\":false,\"err\":\"confirm\"}"); return; }

  logUser("WEB: Fabrika sifirlama");
  logSys("Fabrika sifirlama, reboot");
  prefs.clear();
  g_web->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  scheduleRestart(800, "Factory reset (web)");
}


// =====================
// Web OTA (PC upload: POST /update)
// - Header/query ile aynı WEB_KEY doğrulaması kullanılır.
// - Upload sırasında loop'ta ağır işler durdurulur.
// =====================
static bool g_otaBoardChecked = false;
static bool g_otaBoardMismatch = false;
static bool g_otaVerOld = false;
static int  g_otaFileVer = 0;

static void webHandleOtaUpload() {
  if (!webAuthOk()) return;

  HTTPUpload& up = g_web->upload();

  if (up.status == UPLOAD_FILE_START) {
    g_webOtaInProgress = true;
    g_otaBoardChecked = false;
    g_otaBoardMismatch = false;
    g_otaVerOld = false;
    g_otaFileVer = 0;

    // JS tarafı X-Board-Type header gönderiyor
    String hdr = g_web->header("X-Board-Type");
    if (hdr.length() > 0) {
      int fileBt = hdr.toInt();
      g_otaBoardChecked = true;
      if (fileBt != BOARD_TYPE) {
        g_otaBoardMismatch = true;
        Serial.printf("[OTA] BOARD MISMATCH! firmware=%d, device=%d\n", fileBt, BOARD_TYPE);
      } else {
        Serial.printf("[OTA] Board OK (type=%d)\n", fileBt);
      }
    }

    // JS tarafı X-Firmware-Ver header gönderiyor
    String vhdr = g_web->header("X-Firmware-Ver");
    if (vhdr.length() > 0) {
      g_otaFileVer = vhdr.toInt();
      if (g_otaFileVer > 0 && g_otaFileVer <= FW_VER_NUM) {
        g_otaVerOld = true;
        Serial.printf("[OTA] VERSION OLD! file=%d, device=%d\n", g_otaFileVer, FW_VER_NUM);
      } else {
        Serial.printf("[OTA] Version OK (file=%d > device=%d)\n", g_otaFileVer, FW_VER_NUM);
      }
    }

    Serial.print("[OTA] Upload start: ");
    Serial.println(up.filename);

    if (g_otaBoardMismatch || g_otaVerOld) return; // Upload'ı başlatma

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      Serial.print("[OTA] Update.begin FAILED. err=");
      Serial.println(Update.getError());
    }
  }
  else if (up.status == UPLOAD_FILE_WRITE) {
    if (g_otaBoardMismatch || g_otaVerOld) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      Serial.print("[OTA] Update.write FAILED. err=");
      Serial.println(Update.getError());
    }
  }
  else if (up.status == UPLOAD_FILE_END) {
    if (g_otaBoardMismatch || g_otaVerOld) {
      Serial.println(g_otaBoardMismatch ? "[OTA] Board mismatch - iptal" : "[OTA] Version old - iptal");
      g_webOtaInProgress = false;
      return;
    }
    bool ok = Update.end(true);
    Serial.print("[OTA] Upload end. ok=");
    Serial.println(ok ? "1" : "0");
    if (!ok) {
      Serial.print("[OTA] Update.end FAILED. err=");
      Serial.println(Update.getError());
    }
    g_webOtaInProgress = false;
  }
  else if (up.status == UPLOAD_FILE_ABORTED) {
    Serial.println("[OTA] Upload aborted");
    if (!g_otaBoardMismatch && !g_otaVerOld) Update.abort();
    g_webOtaInProgress = false;
  }

  yield();
}


// Heap durumu izleme (5dk arayla serial log)
static void logHeapIfNeeded() {
  static uint32_t lastCheckMs = 0;
  uint32_t now = millis();
  if ((now - lastCheckMs) < 300000UL) return; // 5 dakika
  lastCheckMs = now;
  uint32_t freeH = ESP.getFreeHeap();

  // Özellik 5: Son çalışma zamanını NVS'e kaydet (15dk arayla — flash ömrü koruma)
  static uint32_t lastAliveMs = 0;
  if ((now - lastAliveMs) >= 900000UL && isTimeValid()) { // 15 dakika
    lastAliveMs = now;
    prefs.putULong(NVS_KEY_LAST_ALIVE, (uint32_t)time(nullptr));
  }

  // Özellik 4: Heap koruma
  if (freeH < 10240) { // < 10KB kritik
    Serial.printf("[HEAP] KRITIK! %u bytes - otomatik restart\n", freeH);
    logSys("Heap kritik (" + String(freeH) + "B), reboot");
    // NOT: tgSend() burada çağrılmaz — TLS handshake ~15KB heap ister, crash döngüsüne neden olur
    delay(200);
    ESP.restart();
  }
  if (freeH < 20480) { // < 20KB uyarı
    Serial.printf("[WARN] Dusuk heap: %u bytes (min: %u)\n", freeH, (uint32_t)ESP.getMinFreeHeap());
  }
}

static void webHandleOtaFinish() {
  if (!webRequireAuth()) return;

  if (g_otaBoardMismatch) {
    g_web->send(400, "application/json",
      "{\"ok\":false,\"err\":\"board_mismatch\",\"msg\":\"Yanlis firmware! Bu cihaz: " BOARD_NAME "\"}");
    logUser("WEB: OTA REDDEDILDI (yanlis board)");
    return;
  }

  if (g_otaVerOld) {
    String vmsg = "{\"ok\":false,\"err\":\"version_old\",\"msg\":\"Firmware zaten guncel veya eski (cihaz: "
                  + String(FW_VER_NUM) + ", dosya: " + String(g_otaFileVer) + ")\"}";
    g_web->send(400, "application/json", vmsg);
    logUser("WEB: OTA REDDEDILDI (eski versiyon)");
    return;
  }

  bool ok = !Update.hasError();
  if (!ok) {
    String msg = String("{\"ok\":false,\"err\":\"update\",\"code\":") + String((int)Update.getError()) + "}";
    g_web->send(500, "application/json", msg);
    return;
  }

  g_web->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  logUser("WEB: Firmware OTA yuklendi");
  logSys("Firmware guncelleme, reboot");
  scheduleRestart(1200, "Web OTA");
}




// =====================
// /api/system - Detaylı sistem bilgisi
// =====================
static void webHandleSystem() {
  if (!webRequireAuth()) return;

  DynamicJsonDocument doc(2048);
  doc["ok"] = true;

  // ── RAM ──
  JsonObject ram = doc.createNestedObject("ram");
  uint32_t totalHeap = ESP.getHeapSize();
  uint32_t freeHeap  = ESP.getFreeHeap();
  uint32_t minHeap   = ESP.getMinFreeHeap();
  uint32_t maxAlloc  = ESP.getMaxAllocHeap();
  ram["total"]    = totalHeap;
  ram["free"]     = freeHeap;
  ram["used"]     = totalHeap - freeHeap;
  ram["minFree"]  = minHeap;
  ram["maxAlloc"] = maxAlloc;
  ram["usedPct"]  = (totalHeap > 0) ? (int)(100.0f * (float)(totalHeap - freeHeap) / (float)totalHeap) : 0;

  // PSRAM (varsa)
  uint32_t psTotal = ESP.getPsramSize();
  if (psTotal > 0) {
    JsonObject psram = doc.createNestedObject("psram");
    uint32_t psFree = ESP.getFreePsram();
    psram["total"] = psTotal;
    psram["free"]  = psFree;
    psram["used"]  = psTotal - psFree;
  }

  // ── Flash ──
  JsonObject flash = doc.createNestedObject("flash");
  uint32_t flashTotal  = ESP.getFlashChipSize();
  uint32_t sketchSize  = ESP.getSketchSize();
  uint32_t sketchFree  = ESP.getFreeSketchSpace();
  flash["total"]       = flashTotal;
  flash["sketch"]      = sketchSize;
  flash["sketchFree"]  = sketchFree;
  flash["sketchPct"]   = (sketchSize + sketchFree > 0) ? (int)(100.0f * (float)sketchSize / (float)(sketchSize + sketchFree)) : 0;

  // ── CPU ──
  JsonObject cpu = doc.createNestedObject("cpu");
  cpu["model"]    = String(ESP.getChipModel()) + " (" + BOARD_NAME + ")";
  cpu["revision"] = (int)ESP.getChipRevision();
  cpu["cores"]    = (int)ESP.getChipCores();
  cpu["freqMHz"]  = (int)ESP.getCpuFreqMHz();
  cpu["usageTotal"] = (int)(g_cpuUsageTotal + 0.5f);
  cpu["loopsPerSec"] = (int)(g_loopsPerSec * 10.0f + 0.5f) / 10.0f; // 1 ondalık
  // I/O bekleme yüzdesi (Telegram + HTTP)
  {
    uint32_t tot = g_cpuWorkUs + g_cpuIoUs + g_cpuDelayUs;
    cpu["ioPct"] = tot > 0 ? (int)(100.0f * (float)g_cpuIoUs / (float)tot + 0.5f) : 0;
  }

  // Dahili sıcaklık (kalibre değil ama trend gösterir)
  #if defined(CONFIG_IDF_TARGET_ESP32)
    cpu["tempC"] = (int)(temperatureRead() + 0.5f);
  #elif defined(CONFIG_IDF_TARGET_ESP32S3)
    // ESP32-S3: temperatureRead() Arduino core 3.x ile çalışır
    float _t = temperatureRead();
    if (_t > -20 && _t < 120) cpu["tempC"] = (int)(_t + 0.5f);
  #endif

  // ── WiFi ──
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"]    = WiFi.SSID();
  wifi["rssi"]    = (int)WiFi.RSSI();
  wifi["ip"]      = WiFi.localIP().toString();
  wifi["mac"]     = WiFi.macAddress();
  wifi["channel"] = (int)WiFi.channel();
  wifi["txPower"] = (int)WiFi.getTxPower();

  // RSSI kalite seviyesi
  int rssi = WiFi.RSSI();
  const char* quality = "Yok";
  if (rssi > -50) quality = "Mukemmel";
  else if (rssi > -60) quality = "Cok Iyi";
  else if (rssi > -70) quality = "Iyi";
  else if (rssi > -80) quality = "Zayif";
  else quality = "Cok Zayif";
  wifi["quality"] = quality;

  // ── Uptime ──
  uint32_t uptimeSec = millis() / 1000;
  doc["uptimeSec"] = uptimeSec;

  // ── NVS İstatistikleri ──
  nvs_stats_t nvsStats;
  if (nvs_get_stats(NULL, &nvsStats) == ESP_OK) {
    JsonObject nvs = doc.createNestedObject("nvs");
    nvs["usedEntries"]  = (int)nvsStats.used_entries;
    nvs["freeEntries"]  = (int)nvsStats.free_entries;
    nvs["totalEntries"] = (int)nvsStats.total_entries;
    nvs["nsCount"]      = (int)nvsStats.namespace_count;
  }

  String out;
  serializeJson(doc, out);
  g_web->send(200, "application/json", out);
}

static void webSetup() {
  // WebServer port'u runtime (NVS) ile değiştirilebilsin diye pointer kullandık.
  if (g_web) { delete g_web; g_web = nullptr; }
  g_web = new WebServer((int)g_httpPort);

  const char* hdrs[] = {"X-API-KEY", "X-Board-Type", "X-Firmware-Ver"};
  g_web->collectHeaders(hdrs, 3);

  g_web->on("/", HTTP_GET, webHandleRoot);
  g_web->on("/admin", HTTP_GET, webHandleAdmin);

  g_web->on("/api/public", HTTP_GET, webHandlePublic);
  g_web->on("/api/authcheck", HTTP_GET, webHandleAuthCheck);

  g_web->on("/api/settings", HTTP_GET, webHandleGetSettings);
  g_web->on("/api/settings", HTTP_POST, webHandlePostSettings);

  g_web->on("/api/dinigunler", HTTP_GET, webHandleGetDiniGunler);

  g_web->on("/api/admincfg", HTTP_GET, webHandleGetAdminCfg);
  g_web->on("/api/admincfg", HTTP_POST, webHandlePostAdminCfg);

  g_web->on("/api/action", HTTP_POST, webHandlePostAction);

  g_web->on("/api/factory_reset", HTTP_POST, webHandlePostFactoryReset);

  g_web->on("/api/system", HTTP_GET, webHandleSystem);
  g_web->on("/api/logs", HTTP_GET, webHandleLogs);
  g_web->on("/api/wifiscan", HTTP_GET, webHandleWifiScan);
  g_web->on("/api/wifitest", HTTP_POST, webHandleWifiTest);
  g_web->on("/api/wifistatus", HTTP_GET, webHandleWifiStatus);

  // Web OTA upload
  g_web->on("/update", HTTP_POST, webHandleOtaFinish, webHandleOtaUpload);

  g_web->onNotFound([]() {
    g_web->send(404, "text/plain", "Not found");
  });

  g_web->begin();
}


// =====================
// Setup / Loop
// =====================
void setup() {
  g_bootStartMs = millis();

  // ESP32-S3 USB-CDC için erken başlat
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] Cami Otomasyon baslatiyor...");
  Serial.print("[BOOT] Board: "); Serial.println(BOARD_NAME);
  Serial.print("[BOOT] Tag: "); Serial.println(getBoardTag());
  Serial.print("[BOOT] Ver: "); Serial.println(getVersionTag());
  Serial.print("[BOOT] Relay=GPIO"); Serial.print(RELAY_PIN);
  Serial.print(" Button=GPIO"); Serial.println(BUTTON_PIN);

  // ---- Runtime chip dogrulama ----
  String chipModel = ESP.getChipModel();
  bool chipOk = true;
  #if BOARD_TYPE == 1
    if (chipModel.indexOf("ESP32-S3") >= 0) chipOk = false; // ESP32 firmware, S3 chip
  #elif BOARD_TYPE == 2
    if (chipModel.indexOf("ESP32-S3") < 0)  chipOk = false; // S3 firmware, ESP32 chip
  #endif
  if (!chipOk) {
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("!!! YANLIS FIRMWARE !!!");
    Serial.print("!!! Beklenen: "); Serial.println(BOARD_NAME);
    Serial.print("!!! Bulunan:  "); Serial.println(chipModel);
    Serial.println("!!! Sistem DURDURULUYOR !!!");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    // Sonsuz döngü - yanlış firmware çalışmasın
    while (true) { delay(5000); Serial.println("!!! YANLIS FIRMWARE - dogru board secin !!!"); }
  }

  // CPU kullanım izleme başlat
  g_loopWindowStartMs = millis();
  g_cpuWindowMs = millis();

  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  esp_reset_reason_t rr = esp_reset_reason();
  Serial.print("[BOOT] ResetReason=");
  Serial.println((int)rr);

  prefs.begin("cami", false);

  // Log buffer'ları NVS'ten yükle (restart'ta kaybolmasın)
  logLoadFromNvs(NVS_KEY_ULOG_BLOB, NVS_KEY_ULOG_META, g_userLog, g_userLogIdx, g_userLogCnt);
  logLoadFromNvs(NVS_KEY_SLOG_BLOB, NVS_KEY_SLOG_META, g_sysLog, g_sysLogIdx, g_sysLogCnt);

  // Ağ/İlçe ayarları (NVS)
  loadNetFromNvs();
  loadHttpPortFromNvs();
  loadIlceFromNvs();

  // WiFi & WEB_KEY (secrets boşsa NVS fallback)
  loadWiFiWebKeyFromNvsFallback();

  // Bot nesnesi oluştur (secrets.h token'ı ile, sonra NVS override edebilir)
  Serial.println("[BOOT] Bot init...");
  reinitBot(g_botToken);
  tgPrepare(12000);
  Serial.println("[BOOT] Bot OK");

  loadBotTokenChatIdFromNvs();

  g_hnyDay = prefs.getUChar(NVS_KEY_HNY_DAY, 1);
  g_hnyMon = prefs.getUChar(NVS_KEY_HNY_MON, 0);
  g_autoHicriYear = prefs.getBool(NVS_KEY_AUTO_HYR, false);
  g_hnyLastYear = prefs.getUShort(NVS_KEY_HNY_LAST, 0);
  if (g_hnyDay < 1 || g_hnyDay > 30) g_hnyDay = 1;
  if (g_hnyMon > 11) g_hnyMon = 0;  // Panelin hedef sohbetini (aktif chat) NVS'ten yükle
  loadActiveChatFromNvs();

#if FORCE_RESET_ADMINS
  resetAdminsToOwner();
#endif

  loadAdminsFromNvs();
  ensureOwnerIsAdmin();

  // Dini gün enable ayarlarını NVS'ten yükle
  loadSpecialEnableFromNvs();
  // Dini gün tarih override ayarlarını NVS'ten yükle
  loadSpecialOverrideFromNvs();

  loadOffsetsFromNvs();

  loadTimesFromNvs();
  tgLoadLastIdFromNvs();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Telegram gecikmesini azaltır (power-save kapalı)
  applyWiFiNetCfgIfNeeded();
  wifiBeginNow();
  logSerialAndTg("🚀 Sistem basladi. WiFi baglaniyor...", false, false);

  // Web panel
  webSetup();

  // Watchdog Timer (30sn - loop takılırsa otomatik restart)
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t wdtCfg = { .timeout_ms = 30000, .idle_core_mask = 0, .trigger_panic = true };
    esp_task_wdt_reconfigure(&wdtCfg);
  #else
    esp_task_wdt_init(30, true);
  #endif
  esp_task_wdt_add(NULL);
  Serial.println("[BOOT] Watchdog 30sn aktif");

  configTime(3 * 3600, 0, "pool.ntp.org", "time.google.com");

  // Boot log
  {
    esp_reset_reason_t reason = esp_reset_reason();
    String reasonStr = "boot";
    if (reason == ESP_RST_TASK_WDT) reasonStr = "Watchdog restart";
    else if (reason == ESP_RST_PANIC) reasonStr = "Panic restart";
    else if (reason == ESP_RST_SW) reasonStr = "Software restart";
    else if (reason == ESP_RST_POWERON) reasonStr = "Power-on";
    logSys("Boot: " + reasonStr);
  }

  // Not: Boot'ta uzun beklemeler (WiFi/NTP bekleme + vakit indirme) Telegram komutlarına geç tepkiye neden oluyordu.
  // Artık setup'ta beklemiyoruz; loop içinde arka planda tamamlanacak.

  applyRelayLogic();

  // Her reboot'ta otomatik menü
  g_autoMenuPending = true;
}

void loop() {
  esp_task_wdt_reset(); // Watchdog besle
  uint32_t _loopStartUs = micros();
  uint32_t _ioThisLoopUs = 0;
  g_loopCount++;

  wifiKeepAlive();
  if (g_web) g_web->handleClient();

  // Planlı restart (örn. statik IP değişimi)
  if (millisPassed(g_restartAtMs)) {
    Serial.print("[RESTART] ");
    Serial.println(g_restartWhy);
    delay(150);
    ESP.restart();
  }


  

  // Web OTA upload sırasında diğer işleri durdur (stabilite için)
  if (g_webOtaInProgress) {
    delay(10);
    return;
  }

// /guncelle worker önce (blocking I/O)
  { uint32_t _t0 = micros(); updateWorkerTick(); _ioThisLoopUs += (micros() - _t0); }

  { uint32_t _t0 = micros(); handleTelegram(); _ioThisLoopUs += (micros() - _t0); }
  uiRefreshTick();
  // Menü/Panel gönderimi (boot sonrası otomatik menü dahil) Telegram komutundan sonra çalışsın
  // (ilk mesajda komut gecikmesin).
  autoMenuTick();
  handleButton();
  weeklyUpdateTick();
  weeklyRestartTick();
  wifiTestTick();

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

  // Heap durumu izleme (5dk arayla serial log)
  // Hicri yıl otomatik güncelleme (saatlik kontrol)
  {
    static uint32_t _hnyMs = 0;
    if (g_autoHicriYear && isTimeValid() && g_dayCount > 0 && millis() - _hnyMs > 3600000UL) {
      _hnyMs = millis();
      int ti = findIdx(ymdToday());
      if (ti >= 0) {
        int hd=0, hy=0; String hm;
        if (parseHicri(g_days[ti].hicriUzun, hd, hm, hy)) {
          int mi = monthIndexFromKey(hm);
          bool pastNY = (mi > (int)g_hnyMon) || (mi == (int)g_hnyMon && hd >= (int)g_hnyDay);
          if (pastNY && hy > 0 && (uint16_t)hy != g_hnyLastYear) {
            for (uint8_t si = 0; si < SPECIAL_COUNT; si++) {
              if (!g_spOv[si].useDefault && g_spOv[si].year > 0 && g_spOv[si].year < (uint16_t)hy) {
                g_spOv[si].year = (uint16_t)hy;
              }
            }
            saveSpecialOverrideToNvs();
            g_hnyLastYear = (uint16_t)hy;
            prefs.putUShort(NVS_KEY_HNY_LAST, g_hnyLastYear);
            recomputeAllSchedules();
            logSys("Hicri yil guncellendi: " + String(hy));
          }
        }
      }
    }
  }

  // CPU ölçüm: bu loop'un iş ve I/O süresini hesapla
  uint32_t _totalThisLoop = (micros() - _loopStartUs);
  g_cpuIoUs += _ioThisLoopUs;
  g_cpuWorkUs += (_totalThisLoop > _ioThisLoopUs) ? (_totalThisLoop - _ioThisLoopUs) : 0;

  cpuWindowUpdate();
  logHeapIfNeeded();

  // Özellik 6: OTA Rollback - 60sn stabil çalışma sonrası firmware'i onayla
  {
    static bool _otaValidated = false;
    if (!_otaValidated && millis() > 60000) {
      _otaValidated = true;
      esp_ota_img_states_t state;
      const esp_partition_t* running = esp_ota_get_running_partition();
      if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
          esp_ota_mark_app_valid_cancel_rollback();
          Serial.println("[OTA] Firmware onaylandi (rollback iptal)");
          logSys("Firmware onaylandi (60sn stabil)");
        }
      }
    }
  }

  uint32_t _delayStart = micros();
  delay(50);
  g_cpuDelayUs += (micros() - _delayStart);
}