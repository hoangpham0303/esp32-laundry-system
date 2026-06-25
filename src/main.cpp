#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <FirebaseESP32.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <time.h>
#include <PZEM004Tv30.h>
#include <HardwareSerial.h>
#include <Adafruit_PCF8574.h>

// ================= CẤU HÌNH WIFI & FIREBASE =================
#define WIFI_SSID "Wifi HHH"
#define WIFI_PASSWORD "hoang123090"
#define FIREBASE_HOST "doan-maygiat-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "j5yWpy3Oi4j43gCmNhJ6WuOgdvPeNDxbo6HB5ZCw"

// ================= CẤU HÌNH CHÂN PHẦN CỨNG =================
#define SS_1_PIN  5
#define SS_2_PIN  4
#define RST_PIN   15 
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 26 
#define OLED_SCL 27 
#define PZEM1_RX 16
#define PZEM1_TX 17
#define PZEM2_RX 32
#define PZEM2_TX 14
#define CAM_BIEN_NGOAI  35  
#define CAM_BIEN_TRONG  36  
#define RELAY_QUAT_PIN  22  
#define RELAY_MAY1_PIN  0  
#define RELAY_MAY2_PIN  1  
#define RELAY_DEN_PIN   2  
#define BUZZER_PIN      4  

// ================= KHỞI TẠO ĐỐI TƯỢNG =================
MFRC522 reader1(SS_1_PIN, RST_PIN);
MFRC522 reader2(SS_2_PIN, RST_PIN);
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_PCF8574 pcf;

PZEM004Tv30 pzem1(Serial2, PZEM1_RX, PZEM1_TX);
PZEM004Tv30 pzem2(Serial1, PZEM2_RX, PZEM2_TX); 

FirebaseData fbdo;         // Dành cho luồng chính (Quét thẻ)
FirebaseData fbdo_pzem;    // Dành riêng cho luồng phụ (Core 0) để tránh xung đột dữ liệu
FirebaseAuth auth;
FirebaseConfig config;

// Khai báo Task chạy ngầm
TaskHandle_t TaskPZEM;

// ================= BIẾN TRẠNG THÁI TOÀN CỤC =================
unsigned long tCapNhatOLED = 0;
unsigned long tBuzzerBatDau = 0;
int thoiGianKeuBuzzer = 0;

bool quatDangChay = false;
unsigned long tCa2MayTat = 0; 
int soNguoiTrongPhong = 0;
int trangThaiDemNguoi = 0; 

// Các biến được dùng chung giữa 2 Core
volatile float dongDienM1 = 0.0; 
volatile float dongDienM2 = 0.0; 
unsigned long tDuoiNguongM1 = 0;
unsigned long tDuoiNguongM2 = 0;

int trangThaiMay1 = 0;
int trangThaiMay2 = 0;
String phongMay1 = "";
String phongMay2 = "";

String uidCurrentM1 = "";
String uidCurrentM2 = "";
String thangHienTaiM1 = "";
String thangHienTaiM2 = "";
volatile float energyStartM1 = -1.0; 
volatile float energyStartM2 = -1.0;
float userBaseKwhM1 = 0.0;
float userBaseKwhM2 = 0.0;

// ================= CÁC HÀM XỬ LÝ (CHẠY TRÊN CORE 1) =================
void batBuzzer(int thoiGian) {
  pcf.digitalWrite(BUZZER_PIN, LOW); 
  tBuzzerBatDau = millis();          
  thoiGianKeuBuzzer = thoiGian;      
}

String getUIDString(byte *uidByte, byte uidSize) {
  String uidStr = "";
  for (byte i = 0; i < uidSize; i++) {
    if (uidByte[i] < 0x10) uidStr += "0";
    uidStr += String(uidByte[i], HEX);
  }
  return uidStr;
}

String layThangHienTai() {
  struct tm timeinfo;
  if(getLocalTime(&timeinfo, 10)) {
    char chuoiThang[8];
    strftime(chuoiThang, sizeof(chuoiThang), "%Y-%m", &timeinfo);
    return String(chuoiThang);
  }
  return "unknown";
}

bool xuLyQuetThe(String uid, int mayGiat) {
  String duongDanUser = "/users/" + uid;
  
  if (Firebase.getString(fbdo, duongDanUser + "/phong")) {
    String phong = fbdo.stringData();
    if (phong == "") phong = "N/A";
    
    if(mayGiat == 1) phongMay1 = phong;
    else phongMay2 = phong;

    int daGiat = 0;
    if (Firebase.getInt(fbdo, duongDanUser + "/da_giat")) daGiat = fbdo.intData();
    Firebase.setIntAsync(fbdo, duongDanUser + "/da_giat", daGiat + 1);

    String thangStr = layThangHienTai();
    float kwhThangNay = 0.0;
    if (thangStr != "unknown" && Firebase.getFloat(fbdo, duongDanUser + "/history/" + thangStr)) {
      kwhThangNay = fbdo.floatData();
    }
    
    if (mayGiat == 1) {
      userBaseKwhM1 = kwhThangNay;
      uidCurrentM1 = uid;
      thangHienTaiM1 = thangStr;
    } else {
      userBaseKwhM2 = kwhThangNay;
      uidCurrentM2 = uid;
      thangHienTaiM2 = thangStr;
    }

    Firebase.setStringAsync(fbdo, "/machines/" + String(mayGiat), "Dang giat...");
    Firebase.setStringAsync(fbdo, "/machines/" + String(mayGiat) + "_phong", phong);
    
    return true; 
  }
  return false; 
}

void veGiaoDienDangBan(int may, String phong, float dongDien) {
  display.setTextSize(1);
  display.setCursor(0, 15); display.print(">> MAY: "); display.print(may);
  display.print(" | "); display.print(phong);
  
  display.setCursor(0, 30); display.print("Dong: ");
  if (dongDien >= 0.0) {
    display.print(dongDien, 2); display.print(" A");
  } else { display.print("-- A"); }

  display.setTextSize(2); display.setCursor(10, 45); display.print("DANG GIAT");
}

void hienThiOLED() {
  display.clearDisplay(); display.setTextColor(SH110X_WHITE);
  struct tm timeinfo;
  if(getLocalTime(&timeinfo, 10)) {
    display.setTextSize(1); display.setCursor(20, 0); display.printf("Time: %02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }
  display.drawLine(0, 11, 128, 11, SH110X_WHITE);

  bool m1HoatDong = (trangThaiMay1 == 1); bool m2HoatDong = (trangThaiMay2 == 1);

  if (!m1HoatDong && !m2HoatDong) {
    display.setTextSize(2); display.setCursor(20, 25); display.print("XIN MOI"); display.setCursor(15, 45); display.print("QUET THE!");
  } else {
    int mayHienThi = 1;
    if (m1HoatDong && !m2HoatDong) mayHienThi = 1;
    else if (!m1HoatDong && m2HoatDong) mayHienThi = 2;
    else mayHienThi = ((millis() / 3000) % 2 == 0) ? 1 : 2; 

    if (mayHienThi == 1) veGiaoDienDangBan(1, phongMay1, dongDienM1);
    else veGiaoDienDangBan(2, phongMay2, dongDienM2);
  }
  display.display(); 
}

// ================= TASK CHẠY NGẦM TRÊN CORE 0 (Đo PZEM mỗi 1s) =================
void TaskPZEMcode(void * pvParameters) {
  for(;;) {
    // 1. Gửi và cập nhật dữ liệu máy 1
    float V1 = pzem1.voltage(); 
    if (!isnan(V1)) {
      dongDienM1 = pzem1.current(); 
      float P1 = pzem1.power();
      if (isnan(P1)) P1 = 0.0;

      // ÁP DỤNG Ý TƯỞNG CỦA BẠN: Gom nhóm dữ liệu gửi JSON 1 lần chống lag mạng
      FirebaseJson jsonLive1;
      jsonLive1.set("dien_ap", V1);
      jsonLive1.set("dong_dien", dongDienM1);
      jsonLive1.set("cong_suat", P1);
      Firebase.updateNodeAsync(fbdo_pzem, "/pzem/may1/live", jsonLive1);
      
      // Tích lũy điện năng khi đang giặt
      if (trangThaiMay1 == 1 && uidCurrentM1 != "" && thangHienTaiM1 != "unknown") {
        float eNow = pzem1.energy();
        if (!isnan(eNow)) {
          if (energyStartM1 < 0) energyStartM1 = eNow;
          else {
            float delta = eNow - energyStartM1;
            if (delta > 0) Firebase.setFloatAsync(fbdo_pzem, "/users/" + uidCurrentM1 + "/history/" + thangHienTaiM1, userBaseKwhM1 + delta);
          }
        }
      }
    } else { dongDienM1 = -1.0; } // Báo hiệu chưa kết nối được mạch

    // 2. Gửi và cập nhật dữ liệu máy 2
    float V2 = pzem2.voltage(); 
    if (!isnan(V2)) {
      dongDienM2 = pzem2.current(); 
      float P2 = pzem2.power();
      if (isnan(P2)) P2 = 0.0;

      FirebaseJson jsonLive2;
      jsonLive2.set("dien_ap", V2);
      jsonLive2.set("dong_dien", dongDienM2);
      jsonLive2.set("cong_suat", P2);
      Firebase.updateNodeAsync(fbdo_pzem, "/pzem/may2/live", jsonLive2);
      
      if (trangThaiMay2 == 1 && uidCurrentM2 != "" && thangHienTaiM2 != "unknown") {
        float eNow = pzem2.energy();
        if (!isnan(eNow)) {
          if (energyStartM2 < 0) energyStartM2 = eNow;
          else {
            float delta = eNow - energyStartM2;
            if (delta > 0) Firebase.setFloatAsync(fbdo_pzem, "/users/" + uidCurrentM2 + "/history/" + thangHienTaiM2, userBaseKwhM2 + delta);
          }
        }
      }
    } else { dongDienM2 = -1.0; }

    // Nghỉ đúng 1 giây rồi vòng lại đo tiếp, KHÔNG ẢNH HƯỞNG ĐẾN QUÉT THẺ
    vTaskDelay(1000 / portTICK_PERIOD_MS); 
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(CAM_BIEN_NGOAI, INPUT); pinMode(CAM_BIEN_TRONG, INPUT);
  pinMode(RELAY_QUAT_PIN, OUTPUT); digitalWrite(RELAY_QUAT_PIN, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);     
  
  if (!pcf.begin(0x27, &Wire)) Serial.println("Loi PCF8574!");
  else {
    pcf.pinMode(RELAY_MAY1_PIN, OUTPUT); pcf.digitalWrite(RELAY_MAY1_PIN, HIGH);
    pcf.pinMode(RELAY_MAY2_PIN, OUTPUT); pcf.digitalWrite(RELAY_MAY2_PIN, HIGH);
    pcf.pinMode(RELAY_DEN_PIN, OUTPUT);  pcf.digitalWrite(RELAY_DEN_PIN, HIGH);
    pcf.pinMode(BUZZER_PIN, OUTPUT);     pcf.digitalWrite(BUZZER_PIN, HIGH);
  }

  if(!display.begin(0x3C, true)) Serial.println("Loi OLED!");
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(10, 30); display.print("Dang ket noi WiFi..."); display.display();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }

  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov", "time.windows.com");
  
  config.host = FIREBASE_HOST; config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth); 
  Firebase.reconnectWiFi(true);
  fbdo.setBSSLBufferSize(1024, 1024);
  fbdo_pzem.setBSSLBufferSize(1024, 1024);
  config.timeout.socketConnection = 2000; 

  SPI.begin();
  reader1.PCD_Init(); delay(10); reader2.PCD_Init();
  
  Firebase.setStringAsync(fbdo, "/machines/1", "San sang"); Firebase.setStringAsync(fbdo, "/machines/1_phong", "");
  Firebase.setStringAsync(fbdo, "/machines/2", "San sang"); Firebase.setStringAsync(fbdo, "/machines/2_phong", "");

  // KHỞI TẠO LUỒNG CHẠY NGẦM CHO PZEM TRÊN CORE 0
  xTaskCreatePinnedToCore(
    TaskPZEMcode,   /* Tên hàm chứa luồng */
    "TaskPZEM",     /* Tên định danh */
    10000,          /* Dung lượng RAM cấp phát (Word) */
    NULL,           /* Tham số đầu vào */
    1,              /* Độ ưu tiên */
    &TaskPZEM,      /* Task handle */
    0);             /* Gắn cố định vào Core 0 */
}

// ================= VÒNG LẶP CHÍNH (CHẠY TRÊN CORE 1 - CHỈ LO QUÉT THẺ) =================
void loop() {
  unsigned long thoiGianHienTai = millis();

  // 1. TẮT CÒI TỰ ĐỘNG
  if (tBuzzerBatDau > 0 && (thoiGianHienTai - tBuzzerBatDau >= thoiGianKeuBuzzer)) {
    pcf.digitalWrite(BUZZER_PIN, HIGH); tBuzzerBatDau = 0; 
  }

  // 2. ĐẾM NGƯỜI & BẬT ĐÈN
  bool coNguoiNgoai = (digitalRead(CAM_BIEN_NGOAI) == LOW);
  bool coNguoiTrong = (digitalRead(CAM_BIEN_TRONG) == LOW);
  if (trangThaiDemNguoi == 0) {
    if (coNguoiNgoai && !coNguoiTrong) trangThaiDemNguoi = 1; else if (coNguoiTrong && !coNguoiNgoai) trangThaiDemNguoi = 2; 
  } else if (trangThaiDemNguoi == 1) { 
    if (coNguoiTrong) { soNguoiTrongPhong++; trangThaiDemNguoi = 3;} else if (!coNguoiNgoai && !coNguoiTrong) trangThaiDemNguoi = 0; 
  } else if (trangThaiDemNguoi == 2) { 
    if (coNguoiNgoai) { if (soNguoiTrongPhong > 0) soNguoiTrongPhong--; trangThaiDemNguoi = 3;} else if (!coNguoiNgoai && !coNguoiTrong) trangThaiDemNguoi = 0; 
  } else if (trangThaiDemNguoi == 3) { 
    if (!coNguoiNgoai && !coNguoiTrong) trangThaiDemNguoi = 0; 
  }
  pcf.digitalWrite(RELAY_DEN_PIN, (soNguoiTrongPhong > 0) ? LOW : HIGH);

  // 3. QUẠT
  if (trangThaiMay1 == 1 || trangThaiMay2 == 1) {
    if (!quatDangChay) { digitalWrite(RELAY_QUAT_PIN, HIGH); quatDangChay = true; } tCa2MayTat = 0; 
  } else {
    if (quatDangChay) {
      if (tCa2MayTat == 0) tCa2MayTat = thoiGianHienTai; 
      if (thoiGianHienTai - tCa2MayTat >= 5000) { digitalWrite(RELAY_QUAT_PIN, LOW); quatDangChay = false; tCa2MayTat = 0; }
    }
  }

  // 4. QUẢN LÝ QUÉT THẺ MÁY 1 (Cực kỳ nhạy vì chạy độc lập)
  if (trangThaiMay1 == 0) { 
    reader1.PCD_Init(); 
    if (reader1.PICC_IsNewCardPresent() && reader1.PICC_ReadCardSerial()) {
      pcf.digitalWrite(BUZZER_PIN, LOW); delay(80); pcf.digitalWrite(BUZZER_PIN, HIGH);
      
      display.clearDisplay(); display.setCursor(20, 25); display.setTextSize(2); 
      display.print("DANG CHECK"); display.display();

      String uid = getUIDString(reader1.uid.uidByte, reader1.uid.size);
      if (xuLyQuetThe(uid, 1)) { 
        trangThaiMay1 = 1; tDuoiNguongM1 = thoiGianHienTai;
        energyStartM1 = -1.0; 
        pcf.digitalWrite(RELAY_MAY1_PIN, LOW); 
        batBuzzer(200); 
      } else {
        for(int i=0; i<3; i++) { pcf.digitalWrite(BUZZER_PIN, LOW); delay(80); pcf.digitalWrite(BUZZER_PIN, HIGH); delay(80); }
      }
      reader1.PICC_HaltA(); reader1.PCD_StopCrypto1(); 
    }
  } else if (trangThaiMay1 == 1) { 
    // Theo dõi dòng điện để ngắt rơ-le (dongDienM1 do Core 0 cập nhật ngầm)
    if (dongDienM1 > 0.0) { tDuoiNguongM1 = thoiGianHienTai; } 
    else if (dongDienM1 == 0.0) {
      if (thoiGianHienTai - tDuoiNguongM1 >= 15000) { 
        trangThaiMay1 = 0; pcf.digitalWrite(RELAY_MAY1_PIN, HIGH); 
        Firebase.setStringAsync(fbdo, "/machines/1", "San sang"); Firebase.setStringAsync(fbdo, "/machines/1_phong", "");
        uidCurrentM1 = ""; batBuzzer(200); 
      }
    }
  }

  // 5. QUẢN LÝ QUÉT THẺ MÁY 2
  if (trangThaiMay2 == 0) {
    reader2.PCD_Init(); 
    if (reader2.PICC_IsNewCardPresent() && reader2.PICC_ReadCardSerial()) {
      pcf.digitalWrite(BUZZER_PIN, LOW); delay(80); pcf.digitalWrite(BUZZER_PIN, HIGH);
      
      display.clearDisplay(); display.setCursor(20, 25); display.setTextSize(2); 
      display.print("DANG CHECK"); display.display();

      String uid = getUIDString(reader2.uid.uidByte, reader2.uid.size);
      if (xuLyQuetThe(uid, 2)) { 
        trangThaiMay2 = 1; tDuoiNguongM2 = thoiGianHienTai;
        energyStartM2 = -1.0; 
        pcf.digitalWrite(RELAY_MAY2_PIN, LOW); 
        batBuzzer(200); 
      } else {
        for(int i=0; i<3; i++) { pcf.digitalWrite(BUZZER_PIN, LOW); delay(80); pcf.digitalWrite(BUZZER_PIN, HIGH); delay(80); }
      }
      reader2.PICC_HaltA(); reader2.PCD_StopCrypto1();
    }
  } else if (trangThaiMay2 == 1) { 
    if (dongDienM2 > 0.0) { tDuoiNguongM2 = thoiGianHienTai; } 
    else if (dongDienM2 == 0.0) {
      if (thoiGianHienTai - tDuoiNguongM2 >= 15000) {
        trangThaiMay2 = 0; pcf.digitalWrite(RELAY_MAY2_PIN, HIGH); 
        Firebase.setStringAsync(fbdo, "/machines/2", "San sang"); Firebase.setStringAsync(fbdo, "/machines/2_phong", "");
        uidCurrentM2 = ""; batBuzzer(200); 
      }
    }
  }

  // 6. CẬP NHẬT OLED MƯỢT MÀ TỪNG 200ms
  if(thoiGianHienTai - tCapNhatOLED >= 200) { hienThiOLED(); tCapNhatOLED = thoiGianHienTai; }
}