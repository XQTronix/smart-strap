#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <TinyGPS++.h>
#include <Preferences.h>

// ==================== 1. BURANI ÖZ MƏLUMATLARINIZLA DƏYİŞİN ====================
const char* WIFI_SSID     = "";       // Wi-Fi adı
const char* WIFI_PASSWORD = "";    // Wi-Fi şifrəsi
#define BOT_TOKEN           "" // Bot Token
// ===============================================================================

#define BUZZER_PIN 26       // Buzzerin (+) ayağı -> ESP32 GPIO 26 (D26)

// Təkrar həyəcan vaxtı: 120000 ms = 2 dəqiqə
const unsigned long TEKRAR_HEYECAN_VAXTI = 120000;

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
TinyGPSPlus gps;
HardwareSerial gpsSerial(2);
Preferences yaddas;

// GPS RX pin axtarışı üçün
const int mumkunPinler[] = {16, 17, 4, 5, 18, 19, 21, 22, 23, 25, 27, 32, 33, 34, 35};
const int pinSayi = sizeof(mumkunPinler) / sizeof(mumkunPinler[0]);
int aktivGpsPin = 16;
bool gpsPinTapildi = false;

// 3 Nöqtəli Sərhəd Koordinatları
double serhedLat[3] = {0.0, 0.0, 0.0};
double serhedLng[3] = {0.0, 0.0, 0.0};
bool serhedAktivdir = false;

bool serhedYigmaRejimi = false;
int yigilanNoqteSayi = 0;
String aktivChatId = "";

// Real GPS Koordinatları
double cariLat = 0.0;
double cariLng = 0.0;
bool gpsFixVar = false;
bool ilkGpsBildirisiGonderildi = false;

// Canlı Xəritə (Live Location) dəyişənləri
long canliMesajId = 0;
unsigned long sonCanliYenileme = 0;
const unsigned long CANLI_YENILEME_VAXTI = 8000; // Hər 8 saniyədən bir canlı xəritədəki nöqtəni hərəkət etdir!

// Həyəcan idarəetmə dəyişənləri
bool testColdeRejimi = false;
bool siqnalSusdurulub = false;
bool buzzerAktivdir = false;
unsigned long sonHeyecanVaxti = 0;

unsigned long sonMesajYoxlama = 0;
const unsigned long MESAJ_INTERVALI = 1500;
unsigned long sonBuzzerVaxti = 0;

// -------------------- YADDAŞ FUNKSİYALARI --------------------
void chatIdYaddasaYaz(String id) {
  yaddas.begin("coban", false);
  yaddas.putString("chatId", id);
  yaddas.end();
}

void serhediYaddasaYaz() {
  yaddas.begin("coban", false);
  yaddas.putBool("aktiv", true);
  yaddas.putString("chatId", aktivChatId);
  for (int i = 0; i < 3; i++) {
    yaddas.putDouble(("lat" + String(i)).c_str(), serhedLat[i]);
    yaddas.putDouble(("lng" + String(i)).c_str(), serhedLng[i]);
  }
  yaddas.end();
}

void serhediYaddasdanOxu() {
  yaddas.begin("coban", true);
  serhedAktivdir = yaddas.getBool("aktiv", false);
  aktivChatId = yaddas.getString("chatId", "");
  if (serhedAktivdir) {
    for (int i = 0; i < 3; i++) {
      serhedLat[i] = yaddas.getDouble(("lat" + String(i)).c_str(), 0.0);
      serhedLng[i] = yaddas.getDouble(("lng" + String(i)).c_str(), 0.0);
    }
  }
  yaddas.end();
}

// -------------------- 3 NÖQTƏLİ ÜÇBUCAQ YOXLANIŞI --------------------
bool noqteUcbucaginIcindedir(double testLat, double testLng) {
  bool iceridedir = false;
  int n = 3;
  for (int i = 0, j = n - 1; i < n; j = i++) {
    if (((serhedLng[i] > testLng) != (serhedLng[j] > testLng)) &&
        (testLat < (serhedLat[j] - serhedLat[i]) * (testLng - serhedLng[i]) / (serhedLng[j] - serhedLng[i]) + serhedLat[i])) {
      iceridedir = !iceridedir;
    }
  }
  return iceridedir;
}

// -------------------- BUZZER FUNKSİYALARI --------------------
void buzzerQisaSes(int muddetMs) {
  pinMode(BUZZER_PIN, OUTPUT);
  unsigned long baslangic = millis();
  while (millis() - baslangic < (unsigned long)muddetMs) {
    digitalWrite(BUZZER_PIN, HIGH);
    delayMicroseconds(250);
    digitalWrite(BUZZER_PIN, LOW);
    delayMicroseconds(250);
  }
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BUZZER_PIN, INPUT);
}

void buzzerSusdur() {
  buzzerAktivdir = false;
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BUZZER_PIN, INPUT);
}

// -------------------- TELEGRAM CANLI XƏRİTƏ (LIVE LOCATION) FUNKSİYASI --------------------
void canliXeriteGonder(String chat_id, double lat, double lng) {
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendLocation?chat_id=" + chat_id +
               "&latitude=" + String(lat, 7) + "&longitude=" + String(lng, 7) + "&live_period=86400";
  http.begin(secured_client, url);
  int httpCode = http.GET();
  if (httpCode > 0) {
    String cavab = http.getString();
    int idx = cavab.indexOf("\"message_id\":");
    if (idx != -1) {
      int bas = idx + 13;
      int son = cavab.indexOf(",", bas);
      canliMesajId = cavab.substring(bas, son).toInt();
      sonCanliYenileme = millis();
    }
  }
  http.end();
}

// Heyvan yeridikcə Telegram-dakı Canlı Xəritəni hərəkət etdirən funksiya:
void canliXeriteYenile(String chat_id, double lat, double lng) {
  if (canliMesajId == 0 || chat_id == "") return;
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/editMessageLiveLocation?chat_id=" + chat_id +
               "&message_id=" + String(canliMesajId) +
               "&latitude=" + String(lat, 7) + "&longitude=" + String(lng, 7);
  http.begin(secured_client, url);
  http.GET();
  http.end();
}

// -------------------- HƏM SADƏ XƏRİTƏ, HƏM DƏ CANLI XƏRİTƏ GÖNDƏRMƏ --------------------
void konumGonder(String chat_id, double lat, double lng, String basliq, bool canliAt = true) {
  String latStr = String(lat, 7);
  String lngStr = String(lng, 7);

  // 1. Sadə yazılı mesaj + Google Maps Linki:
  String mesaj = basliq + "\n";
  mesaj += "📍 Dəqiq Koordinat: `" + latStr + ", " + lngStr + "`\n";
  mesaj += "🗺️ [Sadə Xəritədə Aç (Google Maps)](https://www.google.com/maps?q=" + latStr + "," + lngStr + ")";
  bot.sendMessage(chat_id, mesaj, "Markdown");

  // 2. Telegram Daxili Canlı Xəritəsi (Live Map):
  if (canliAt) {
    canliXeriteGonder(chat_id, lat, lng);
  }
}

// -------------------- FASİLƏSİZ GPS OXUMA --------------------
void gpsOxu() {
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    gps.encode(c);
    if (gps.charsProcessed() > 30) {
      gpsPinTapildi = true;
    }
    if (gps.location.isValid() && gps.location.lat() != 0.0) {
      if (!testColdeRejimi) {
        cariLat = gps.location.lat();
        cariLng = gps.location.lng();
        gpsFixVar = true;
      }
    }
  }
}

void aktivGpsDinle(unsigned long muddetMs) {
  unsigned long baslangic = millis();
  while (millis() - baslangic < muddetMs) {
    gpsOxu();
    delay(2);
  }
}

void gpsPininiAvtomatikTap() {
  if (gpsPinTapildi) return;
  for (int i = 0; i < pinSayi; i++) {
    int yoxlananPin = mumkunPinler[i];
    gpsSerial.end();
    gpsSerial.setRxBufferSize(2048);
    gpsSerial.begin(9600, SERIAL_8N1, yoxlananPin, -1);

    unsigned long EvvelkiBayt = gps.charsProcessed();
    aktivGpsDinle(1200);

    if (gps.charsProcessed() - EvvelkiBayt > 20) {
      aktivGpsPin = yoxlananPin;
      gpsPinTapildi = true;
      return;
    }
  }
  gpsSerial.end();
  gpsSerial.setRxBufferSize(2048);
  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);
  aktivGpsPin = 16;
}

// -------------------- 5 DƏFƏ DALBADAL HƏYƏCAN GÖNDƏRƏN FUNKSİYA --------------------
void besDefeHeyecanGonder(String chat_id, double lat, double lng) {
  buzzerAktivdir = true;

  for (int m = 1; m <= 5; m++) {
    aktivGpsDinle(300);
    if (!testColdeRejimi && gpsFixVar) {
      lat = cariLat;
      lng = cariLng;
    }

    buzzerQisaSes(300);

    String basliq = "🚨 *HƏYƏCAN SİQNALI! (" + String(m) + "/5)* 🚨\n";
    basliq += "🐾 *Xalta №1* təyin olunmuş 3 nöqtəli sərhəddən KƏNARA ÇIXDI!";
    // 5 mesajın hamısında sadə xəritə, sonuncusunda (5/5) isə əlavə olaraq Canlı Xəritə açılır:
    konumGonder(chat_id, lat, lng, basliq, (m == 5));
  }
}

// -------------------- MENYU DÜYMƏLƏRİ --------------------
void menyuGonder(String chat_id, String metn) {
  String klaviatura = "[[\"📐 Yeni Sərhəd (3 Nöqtə)\", \"📍 Heyvan Haradadır?\"], [\"🧪 Test: Çölə Çıxart\", \"🔕 Siqnalı Dayandır\"], [\"🗺️ Mövcud Sərhədə Bax\"]]";
  bot.sendMessageWithReplyKeyboard(chat_id, metn, "Markdown", klaviatura, true);
}

void yeniMesajlariIdareEt(int yeniMesajSayi) {
  for (int i = 0; i < yeniMesajSayi; i++) {
    gpsOxu();
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    if (aktivChatId != chat_id) {
      aktivChatId = chat_id;
      chatIdYaddasaYaz(aktivChatId);
    }

    // 1. Xəritədən Konum (Location) göndərilibsə:
    if (bot.messages[i].longitude != 0 || bot.messages[i].latitude != 0) {
      if (serhedYigmaRejimi) {
        serhedLat[yigilanNoqteSayi] = bot.messages[i].latitude;
        serhedLng[yigilanNoqteSayi] = bot.messages[i].longitude;
        yigilanNoqteSayi++;

        if (yigilanNoqteSayi < 3) {
          String cavab = "✅ *" + String(yigilanNoqteSayi) + "-ci künc nöqtəsi qəbul edildi!*\n\n";
          cavab += "İndi xəritədən (📎 -> Konum/Location) barmağınızla *" + String(yigilanNoqteSayi + 1) + "-ci küncü* seçib göndərin.";
          bot.sendMessage(chat_id, cavab, "Markdown");
        } else {
          serhedYigmaRejimi = false;
          serhedAktivdir = true;
          testColdeRejimi = false;
          siqnalSusdurulub = false;
          sonHeyecanVaxti = 0;
          buzzerSusdur();
          serhediYaddasaYaz();

          String bitisMesaji = "🎉 *Təbriklər! 3 Nöqtəli (Üçbucaq) Otlaq Sərhədi Aktiv Edildi!*\n\n";
          bitisMesaji += "🛡️ Xalta hazırda GPS ilə bu ərazini avtomatik qoruyur.";
          menyuGonder(chat_id, bitisMesaji);
        }
      } else {
        bot.sendMessage(chat_id, "ℹ️ Yeni sərhəd təyin etmək üçün əvvəlcə *\"📐 Yeni Sərhəd (3 Nöqtə)\"* düyməsinə basın.", "Markdown");
      }
      continue;
    }

    // 2. Düymələr və Əmrlər:
    if (text == "/start") {
      menyuGonder(chat_id, "🐾 *Ağıllı Çoban Xaltası Botuna Xoş Gəlmisiniz!*\nAşağıdakı düymələrdən istifadə edin 👇");
    }
    else if (text.indexOf("Yeni Sərhəd") != -1 || text == "/yeni_serhed") {
      serhedYigmaRejimi = true;
      yigilanNoqteSayi = 0;
      buzzerSusdur();
      bot.sendMessage(chat_id, "🗺️ *3 Nöqtəli Yeni Sərhəd Rejimi Başladı!*\n\n1️⃣ *📎 (Sancaq)* işarəsinə basın.\n2️⃣ *Konum (Location)* seçin.\n3️⃣ Xəritədə *1-ci küncü* seçib göndərin!", "Markdown");
    }
    else if (text.indexOf("Dayandır") != -1 || text.indexOf("dayandir") != -1 || text == "/dayandir") {
      testColdeRejimi = false;
      buzzerSusdur();
      sonHeyecanVaxti = millis();
      siqnalSusdurulub = false;
      menyuGonder(chat_id, "🔕 Siqnal dayandı");
    }
    else if (text.indexOf("Çölə Çıxart") != -1) {
      if (!serhedAktivdir) {
        bot.sendMessage(chat_id, "⚠️ Əvvəlcə *\"📐 Yeni Sərhəd (3 Nöqtə)\"* ilə sərhəd təyin edin!", "Markdown");
      } else {
        double maxLat = max(serhedLat[0], max(serhedLat[1], serhedLat[2]));
        double maxLng = max(serhedLng[0], max(serhedLng[1], serhedLng[2]));
        cariLat = maxLat + 0.01;
        cariLng = maxLng + 0.01;

        testColdeRejimi = true;
        siqnalSusdurulub = false;

        besDefeHeyecanGonder(chat_id, cariLat, cariLng);
        sonHeyecanVaxti = millis();
      }
    }
    else if (text.indexOf("Haradadır") != -1 || text == "/haradadir") {
      aktivGpsDinle(1200);

      if (!gpsPinTapildi) {
        gpsPininiAvtomatikTap();
        aktivGpsDinle(1200);
      }

      if (gpsFixVar && !testColdeRejimi) {
        bool iceride = serhedAktivdir ? noqteUcbucaginIcindedir(cariLat, cariLng) : true;
        String status = iceride ? "🐾 *Xalta №1:* ✅ Təhlükəsiz (Sərhədin içindədir)" : "🐾 *Xalta №1:* 🚨 TƏHLÜKƏ! (Sərhəddən kənardadır!)";
        status += "\n🛰️ *Peyk sayı:* " + String(gps.satellites.value()) + " | 🔴 *Canlı İzləmə Aktivdir*";
        // Həm sadə linki, həm də Canlı Xəritəni göndər:
        konumGonder(chat_id, cariLat, cariLng, status, true);
      } else if (testColdeRejimi) {
        konumGonder(chat_id, cariLat, cariLng, "🧪 *Test Rejimi:* 🚨 Sərhəddən kənardadır!", true);
      } else {
        String Melumat = "⏳ *GPS peykləri dəqiqləşdirir...*\n";
        Melumat += "📡 *Oxunan məlumat (Bayt):* `" + String(gps.charsProcessed()) + "`\n";
        bot.sendMessage(chat_id, Melumat, "Markdown");
      }
    }
    else if (text.indexOf("Mövcud Sərhədə") != -1) {
      if (!serhedAktivdir) {
        bot.sendMessage(chat_id, "⚠️ Hələ sərhəd təyin edilməyib.", "Markdown");
      } else {
        for (int k = 0; k < 3; k++) {
          konumGonder(chat_id, serhedLat[k], serhedLng[k], "📐 *Sərhədin " + String(k + 1) + "-ci küncü:*", true);
        }
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  gpsSerial.setRxBufferSize(2048);
  gpsSerial.begin(9600, SERIAL_8N1, aktivGpsPin, -1);

  buzzerSusdur();
  serhediYaddasdanOxu();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  secured_client.setInsecure();
  while (WiFi.status() != WL_CONNECTED) {
    gpsOxu();
    delay(500);
  }

  gpsPininiAvtomatikTap();

  if (aktivChatId != "") {
    menyuGonder(aktivChatId, "🔌 *Xalta işə düşdü!* Həm Sadə, həm də Canlı Xəritə rejimi aktivdir ✅");
  }
}

void loop() {
  // 1. Hər dövrdə 500 ms fasiləsiz GPS-i dinlə
  aktivGpsDinle(500);

  // 2. GPS ilk dəfə peyk tutanda avtomatik həm sadə, həm də Canlı Xəritə göndər:
  if (gpsFixVar && !ilkGpsBildirisiGonderildi && aktivChatId != "") {
    ilkGpsBildirisiGonderildi = true;
    konumGonder(aktivChatId, cariLat, cariLng, "🛰️ *GPS peykləri tutdu!*\n🔴 Canlı Xəritə və avtomatik sərhəd nəzarəti başladı ✅", true);
  }

  // 3. Canlı Xəritəni (Live Location) hər 8 saniyədən bir yenilə ki, xəritədəki nöqtə canlı yerisin!
  if (gpsFixVar && canliMesajId != 0 && !testColdeRejimi) {
    if (millis() - sonCanliYenileme >= CANLI_YENILEME_VAXTI) {
      canliXeriteYenile(aktivChatId, cariLat, cariLng);
      sonCanliYenileme = millis();
    }
  }

  // 4. Əgər həyəcan aktivdirsə, hər 1 saniyədən bir qısa "BİİP" et
  if (buzzerAktivdir) {
    if (millis() - sonBuzzerVaxti > 1000) {
      buzzerQisaSes(250);
      sonBuzzerVaxti = millis();
    }
  }

  // 5. TAM AVTOMATİK SƏRHƏDDƏN ÇIXMA YOXLANIŞI
  if (serhedAktivdir && (gpsFixVar || testColdeRejimi)) {
    bool coldedir = testColdeRejimi || !noqteUcbucaginIcindedir(cariLat, cariLng);

    if (coldedir) {
      if (!siqnalSusdurulub) {
        if (sonHeyecanVaxti == 0 || (millis() - sonHeyecanVaxti >= TEKRAR_HEYECAN_VAXTI)) {
          if (aktivChatId != "") {
            besDefeHeyecanGonder(aktivChatId, cariLat, cariLng);
            sonHeyecanVaxti = millis();
          }
        }
      }
    } else {
      sonHeyecanVaxti = 0;
      siqnalSusdurulub = false;
      buzzerSusdur();
    }
  }

  // 6. Telegram mesajlarını yoxla
  if (millis() - sonMesajYoxlama > MESAJ_INTERVALI) {
    int yeniMesajSayi = bot.getUpdates(bot.last_message_received + 1);
    while (yeniMesajSayi) {
      gpsOxu();
      yeniMesajlariIdareEt(yeniMesajSayi);
      yeniMesajSayi = bot.getUpdates(bot.last_message_received + 1);
    }
    sonMesajYoxlama = millis();
  }
}
