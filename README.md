ESP32 Cami Röle Otomasyonu (Ezan Vakti + Telegram + Buton)

Bu proje, ESP32 ile bir röleyi ezan vakitlerine göre otomatik kontrol eder. Ayrıca Telegram üzerinden manuel kontrol, admin yönetimi ve NVS (kalıcı hafıza) ile vakit/cache saklama içerir.

İlçe: Çankaya (ILCE_ID=9206)
Röle: LOW aktif (aktif-low)

Özellikler

✅ Otomatik-1 (Perşembe→Cuma):
Perşembe Akşam +1 dk ile başlar, Cuma İmsak -1 dk ile biter → Röle ON

✅ Otomatik-2 (Dini Günler):
Dini günün önceki günü Akşam +1 dk → dini gün İmsak -1 dk arası → Röle ON

✅ Ramazan Günleri (opsiyonel):
Cache içinde Ramazan varsa, Ramazan Günü 1..30 otomatik “dini gün” gibi değerlendirilir.

✅ Manuel ON/OFF:

Telegram: /on ve /off

Buton: sadece ON

✅ ZORUNLU OFF (Her zaman geçerli):
“Bir sonraki İmsak - 1 dk” geldiğinde ne olursa olsun OFF (manuel ON dahil).
Ayrıca zorunlu OFF sonrası kısa süreli ON engeli (2 dk) vardır.

✅ Admin sistemi (NVS):

/admin_add, /admin_del, /admin_list, /myid

✅ Vakit Cache yönetimi (NVS):

Cache 30 günlük vakitleri indirip saklar (NVS)

Pazartesi 03:05’te otomatik günceller (başarısız olursa Pazartesi her saat :05’te tekrar dener)

Manuel güncelleme: /guncelle

✅ Telegram mesajları için spam/dedup koruması

✅ Telegram’da /dinigünler çıktısı en yakın tarihten en uzağa ve tek satır formatında

Donanım Bağlantıları
Röle

RELAY_PIN = 23

LOW aktif: Röle ON → GPIO23 LOW

Röle modülün “IN” pini GPIO23’e, VCC/GND uygun şekilde bağlanmalı.

Buton (Sadece ON)

BUTTON_PIN = 27

Buton bağlantısı: GPIO27 ↔ GND

İç pull-up aktif (INPUT_PULLUP)
Basınca LOW olur.

Yazılım Gereksinimleri
Arduino IDE ile

Kurulması gereken kütüphaneler:

WiFi (ESP32 core ile gelir)

WiFiClientSecure (ESP32 core)

HTTPClient (ESP32 core)

ArduinoJson

Preferences (ESP32 core)

UniversalTelegramBot

PlatformIO ile

lib_deps örneği:

bblanchon/ArduinoJson

witnessmenow/UniversalTelegramBot

secrets.h (Zorunlu)

Projede secrets.h dosyası olmalı (aynı klasörde veya include). Örnek:

#pragma once

#define SECRET_WIFI_SSID  "WIFI_ADI"
#define SECRET_WIFI_PASS  "WIFI_SIFRE"

#define SECRET_BOT_TOKEN  "123456:ABCDEF...."
#define SECRET_CHAT_ID    "123456789"   // grup veya kullanıcı chat id


Not: OWNER_ADMIN_ID kod içinde sabit. Telegram’da /myid ile kendi id’ni görüp oraya yazmalısın.

Telegram Komutları
Herkes

/myid → Telegram user_id bilgisini gösterir

/help → Komut listesini verir

/durum → Röle durumu + pencere zamanları + aktif dini gün bilgisi

/dinigünler → Cache içinde bulunan aktif/yaklaşan dini günleri listeler

Admin

/on → Manuel ON

/off → Manuel OFF

Eğer o anda otomatik pencere aktifse, pencere bitene kadar yeniden ON olmaz (override).

/guncelle → Cache güncelleme (vakitleri yeniden indirir)

Güvenli şekilde “kuyruğa alınır” ve loop içinde işlenir.

/admin_list veya /admins

/admin_add <id>

/admin_del <id>

/dinigünler Çıktı Formatı

Liste en yakın tarih en üstte olacak şekilde sıralanır. Format:

✅ 🟡 YAKLASAN - Ramazan Günü 23 Miladi: 13.03.2026
✅ 🟡 YAKLASAN - Ramazan Günü 24 Miladi: 14.03.2026
✅ 🟡 YAKLASAN - Ramazan Günü 25 Miladi: 15.03.2026


Eğer cache içinde aktif/yaklaşan dini gün yoksa:

Yakın tarihte dini gün yok

Çalışma Mantığı (Özet)
Röle ON olma şartları

Röle ON olur eğer:

Perşembe→Cuma otomatik penceresi aktifse veya

Dini gün penceresi aktifse (özel gün veya Ramazan günü) veya

Manuel ON latch aktifse

Röle OFF olma şartları

Manuel /off gelirse röle OFF olur. Eğer otomatik pencere aktifse, pencere bitene kadar tekrar ON olmaz.

ZORUNLU OFF zamanı gelirse: (Bir sonraki İmsak - 1 dk)

Röle kesin OFF

Manuel ON iptal edilir

Kısa süre ON engeli uygulanır

Cache Güncelleme (Önemli)
Otomatik Güncelleme

Pazartesi 03:05’te dener

Başarısızsa Pazartesi günü her saat :05’te tekrar dener

Başarılı olunca NVS’e lastUpdYmd yazılır ve aynı gün tekrar etmez

Manuel /guncelle

Telegram handler içinde ağır işlem yapılmaz

Komut sadece “pending” işaretler

Güncelleme loop içinde worker ile yapılır (stack taşmasını/yeniden başlamayı engeller)

60 saniyelik cooldown vardır

Sık Karşılaşılan Sorunlar
1) “Dini Gun: -” görünüyor

Cache içinde ilgili gün yoktur veya hicri parse eşleşmiyordur.

Çözüm: /guncelle ile cache’i tazele.

2) /guncelle sonrası restart / Guru Meditation

Eskiden Telegram handler içinde indirme+JSON parse yapılınca stack canary tetiklenebiliyordu.

Bu sürümde /guncelle “worker tick” üzerinden yapıldığı için stabil olmalı.

Hâlâ olursa: ArduinoJson bellek tüketimi nedeniyle heap yetersiz olabilir (özellikle PSRAM olmayan kartlarda).

3) WiFi/NTP zamanı yok

Zaman geçerli değilse (NTP gelmediyse), zaman temelli kurallar çalışmaz.

WiFi bağlantısını ve NTP sunucularını kontrol et.

Ayarlar / Özelleştirme

Kod içinde kolay ayarlanabilen yerler:

ILCE_ID_FIXED (ilçe id)

ON_OFFSET_SEC (Akşam + kaç sn)

OFF_OFFSET_SEC (İmsak - kaç sn)

SP_NOTIFY_BEFORE_SEC (dini gün bildirimi kaç sn önce)

EN_RAMAZAN_TUM_GUNLER (Ramazan günlerini dahil et)

Güvenlik Notu

WiFiClientSecure.setInsecure() kullanıldığı için TLS sertifika doğrulaması yapılmaz.

Bu pratikte çalışmayı kolaylaştırır ama güvenlik açısından daha zayıftır.







python tools/uart_push.py COM3 tools/config.txt
