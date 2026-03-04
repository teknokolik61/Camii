# 🕌 Cami Otomasyon Sistemi

ESP32 tabanlı cami şerefesi (minare ışıkları) otomasyon sistemi. Namaz vakitlerine göre otomatik açma/kapama, Telegram bot kontrolü, web panel ve dini gün desteği.

**Geliştirici:** Miraç Bahadır ÖZTÜRK  
**Versiyon:** v12.040  
**Lisans:** MIT

---

## Özellikler

### Namaz Vakti Otomasyonu
- Diyanet İşleri Başkanlığı API'sinden namaz vakitlerini otomatik çeker (30 günlük cache)
- Akşam ezanında şerefeler açılır, sabah ezanında kapanır
- Tolerans ayarı: açılma ve kapanma dakikası öne/arkaya kaydırılabilir (0-30 dk)
- İmsak saatinde zorunlu kapanma (Sabah'a kadar açılamaz)
- Perşembe-Cuma gecesi otomatik açılma desteği
- Haftalık otomatik güncelleme (Pazartesi 03:05)

### Dini Gün Desteği
14 özel gün için otomatik şerefe açma:
- Kandil geceleri: Regaib, Miraç, Berat, Kadir, Mevlid
- Üç Ayların başlangıcı
- Ramazan Bayramı (3 gün)
- Kurban Bayramı (Arefe + 4 gün)
- Hicri takvim hesaplama ile otomatik tarih tespiti
- Her gün ayrı ayrı açılıp kapatılabilir
- Tarih override: web panelden özel tarih girebilme
- Hicri yıl otomatik güncelleme

### Telegram Bot Kontrolü
Uzaktan erişim için Telegram bot entegrasyonu:

| Komut | Açıklama |
|-------|----------|
| `/on` | Şerefeleri aç |
| `/off` | Şerefeleri kapat |
| `/durum` | Sistem durumunu görüntüle |
| `/guncelle` | Namaz vakitlerini güncelle |
| `/dinigunler` | Dini gün listesini göster |
| `/admin_list` | Admin listesi |
| `/admin_add <id>` | Admin ekle |
| `/admin_del <id>` | Admin sil |
| `/myid` | Telegram ID'ni öğren |
| `/menu` | İnteraktif panel |

- Çoklu admin desteği (Owner + Admin yetki sistemi)
- İnteraktif buton paneli (ON/OFF/Durum)
- Boot bildirimi (LAN IP, Dış IP, versiyon, reset nedeni)
- Güç kesintisi bildirimi

### Web Panel
Tam özellikli responsive web arayüzü (7 sekme):

| Sekme | İçerik |
|-------|--------|
| Durum | Canlı saat, namaz vakitleri, şerefe durumu, ON/OFF kontrolü |
| Sistem | CPU, RAM, Flash, WiFi, sıcaklık, uptime gaugeları |
| Toleranslar | Açılma/kapanma tolerans ayarları |
| Dini Günler | 14 özel gün açma/kapama, tarih override |
| Komutlar | Telegram komut listesi |
| Log | Kullanıcı + sistem işlem logları (son 20'şer) |
| Ayarlar | Ağ, WiFi, ilçe, Hicri yıl, Bot Token, OTA, fabrika sıfırlama |

- Dark mode desteği
- Canlı veri (auto-refresh)
- Şifre korumalı (API key)
- Reboot butonu

### Güvenlik ve Koruma

**OTA Board Doğrulama:** Firmware'e gömülü board parmak izi ile yanlış cihaza yükleme engellenir. JavaScript tarafında .bin dosyası upload'dan önce kontrol edilir, sunucu tarafında header ile ikinci doğrulama yapılır.

**OTA Versiyon Kontrolü:** Firmware'deki sayısal versiyon numarası ile eski/aynı versiyonun yüklenmesi engellenir.

**Firmware Rollback:** OTA ile yüklenen firmware 60 saniye boyunca izlenir. Bu süre içinde çökerse ESP32 otomatik olarak önceki çalışan firmware'e döner.

**WiFi Parola Doğrulama:** WiFi bilgileri değiştirilirken önce bağlantı test edilir, başarısız olursa eski WiFi'ye geri dönülür. Cihaz asla erişilemez kalmaz.

**Watchdog Timer:** 30 saniye içinde loop tamamlanmazsa otomatik restart. Boot'ta restart nedeni loglanır ve Telegram'a bildirilir.

**Heap Koruma:** 5 dakikada bir heap kontrolü, 10KB altında otomatik restart ile bellek sızıntısı koruması.

**Güç Kesintisi Tespiti:** Son çalışma zamanı NVS'e periyodik kaydedilir, boot'ta kesinti süresi hesaplanarak Telegram'a bildirilir.

**Haftalık Otomatik Restart:** Her Salı 04:00'da proaktif restart ile heap fragmentasyonu temizlenir.

**Runtime Chip Doğrulama:** Boot'ta ESP32 chip modeli kontrol edilir, yanlış firmware yüklenmiş ise sistem durur.

---

## Desteklenen Donanım

| Board | BOARD_TYPE | Relay GPIO | Button GPIO | Flash |
|-------|-----------|------------|-------------|-------|
| ESP32 DevKit v1 | 1 | 23 | 27 | 4MB |
| ESP32-S3-N16R8 | 2 | 4 | 5 | 16MB |

Tek firmware dosyası tüm boardlarda çalışır. Board farkları `platformio.ini`'den derleme zamanında belirlenir.

---

## Kurulum

### Gereksinimler
- PlatformIO (VS Code eklentisi veya CLI)
- ESP32 DevKit v1 veya ESP32-S3 geliştirme kartı
- 5V röle modülü
- Buton (opsiyonel, fiziksel kontrol için)

### 1. Depoyu Klonla
```bash
git clone https://github.com/KULLANICI/cami-otomasyon.git
cd cami-otomasyon
```

### 2. secrets.h Dosyasını Oluştur
`src/secrets.h` dosyasını aşağıdaki şablonla oluştur:
```cpp
#pragma once

#define SECRET_BOT_TOKEN  "123456:ABC-DEF..."   // Telegram Bot Token
#define CHAT_ID           "-100123456789"        // Telegram Chat/Grup ID
#define WEB_KEY           "gizli_sifre"          // Web panel şifresi
#define SECRET_WIFI_SSID  "WiFi_Adi"             // WiFi SSID
#define SECRET_WIFI_PASS  "WiFi_Sifresi"         // WiFi Parolası
```

> **Not:** `secrets.h` dosyası `.gitignore`'a eklenmelidir. Boş bırakılan alanlar NVS üzerinden web panelden ayarlanabilir.

### 3. Board Seç ve Derle
```bash
# ESP32 DevKit için
pio run -e esp32dev

# ESP32-S3 için
pio run -e esp32s3
```

### 4. Yükle
```bash
# USB ile yükle
pio run -e esp32s3 -t upload

# İlk yükleme (flash tamamen sil)
pio run -e esp32s3 -t erase -t upload
```

### 5. Web Panele Bağlan
Serial monitörden IP adresini öğren ve tarayıcıda aç:
```
http://192.168.1.x
```

---

## Proje Yapısı

```
cami-otomasyon/
├── src/
│   ├── main.cpp          # Ana firmware (tek dosya, ~4500 satır)
│   └── secrets.h         # Gizli bilgiler (gitignore)
├── platformio.ini        # Board konfigürasyonları
├── partitions_16mb.csv   # ESP32-S3 partition table
└── README.md
```

---

## OTA Güncelleme (Web Üzerinden)

1. `pio run -e esp32s3` ile yeni firmware'i derle
2. Web panelde Ayarlar sekmesine git
3. `.pio/build/esp32s3/firmware.bin` dosyasını seç
4. Sistem otomatik olarak board uyumluluğunu ve versiyon numarasını kontrol eder
5. Uygunsa yüklenir, 60 saniye stabil çalışma sonrası onaylanır

---

## API Endpoint'leri

| Endpoint | Metod | Açıklama |
|----------|-------|----------|
| `/api/public` | GET | Versiyon, saat, namaz vakitleri |
| `/api/system` | GET | CPU, RAM, WiFi, uptime detayları |
| `/api/settings` | GET/POST | Tolerans ve dini gün ayarları |
| `/api/admincfg` | GET/POST | Ağ, WiFi, Bot Token, admin yönetimi |
| `/api/action` | POST | Röle kontrol, güncelleme, reboot |
| `/api/logs` | GET | Kullanıcı ve sistem logları |
| `/api/wifiscan` | GET | Çevredeki WiFi ağlarını tara |
| `/api/wifitest` | POST | WiFi bağlantı testi ve kaydetme |
| `/api/dinigunler` | GET | Dini gün listesi |
| `/api/factory_reset` | POST | Fabrika ayarlarına sıfırlama |
| `/update` | POST | OTA firmware yükleme |

Tüm endpoint'ler (public hariç) `X-API-KEY` header'ı ile korunmaktadır.

---

## Log Sistemi

RAM ring buffer + NVS kalıcı depolama. Restart sonrası loglar kaybolmaz.

**Kullanıcı logları (28 kayıt noktası):** Web panel, Telegram, fiziksel buton ve inline panel üzerinden yapılan tüm röle, güncelleme, ayar değişikliği ve admin işlemleri.

**Sistem logları (14 kayıt noktası):** Boot, WiFi bağlantı/kopma, NTP senkronizasyon, otomatik röle, imsak kapanma, vakit güncelleme, güç kesintisi, hicri yıl, firmware ve fabrika sıfırlama olayları.

---

## Konfigürasyon Öncelik Sırası

```
1. secrets.h    → Derleme zamanı sabit değerler
2. NVS          → Web panel / Telegram üzerinden değiştirilen değerler
3. Varsayılan   → Kod içi default değerler
```

`secrets.h`'te bir alan dolu ise NVS değeri göz ardı edilir. Boş bırakılan alanlar NVS üzerinden yönetilir.

---

## Elektrik Bağlantısı

```
ESP32          Röle Modülü
─────          ───────────
GPIO (RELAY)──→ IN
GND ──────────→ GND
VIN (5V) ─────→ VCC

ESP32          Buton
─────          ─────
GPIO (BUTTON)─→ Bir uç
GND ──────────→ Diğer uç
(dahili pull-up aktif)
```

---

## Lisans

MIT License — Detaylar için [LICENSE](LICENSE) dosyasına bakın.

---

## Teşekkürler

- [Diyanet İşleri Başkanlığı](https://namazvakitleri.diyanet.gov.tr/) — Namaz vakti verileri
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) — JSON kütüphanesi
- [UniversalTelegramBot](https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot) — Telegram entegrasyonu
- [Espressif](https://www.espressif.com/) — ESP32 platformu
