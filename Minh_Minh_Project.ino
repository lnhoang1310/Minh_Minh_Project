#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

/* Wifi and Firebase */
#define WIFI_SSID "Lâm Hoàng"
#define WIFI_PASSWORD "123456789"

#define API_KEY "AIzaSyAk_i0lg7aTZK9cDKpPQfQTRQ712IC2v0A"
#define DATABASE_URL "https://iot-project-ec071-default-rtdb.asia-southeast1.firebasedatabase.app/"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long sendDataPrevMillis = 0;
bool signupOK = false;

/* OLED Display */
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

/* MAX30102 */
MAX30105 particleSensor;

/* PIN IO*/
#define SDA_PIN     8
#define SCL_PIN     9
#define BUZZER_PIN  5

/* Configuration */
#define BUFFER_SIZE   100
#define SAMPLE_RATE   200
#define SPO2_LOW      94

#define FINGER_ON_DC   20000
#define SKIP_FIRST_BUFFERS  1

/* Buffer */
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];
int bufferIndex = 0;
bool bufferFull = false;

/* Heart Rate */
const byte RATE_SIZE = 8;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;

float beatsPerMinute = 0;
int beatAvg = 0;
int filteredHR = 0;

/* SpO2 */
float spo2;
float filteredSpO2 = 0;
bool validSPO2 = false;

/* State */
bool fingerPresent = false;
byte stableCount = 0;
bool dataJustSent = false;

/* === Bộ lọc EMA (Exponential Moving Average) === */
#define HR_ALPHA    0.4
#define SPO2_ALPHA  0.3

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase signup OK");
    signupOK = true;
  }

  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    while(1);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  showMessage("Put finger\non sensor");

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    showMessage("Sensor\nnot found");
    while (1);
  }

  particleSensor.setup(
    0x3F,    // LED brightness
    4,       // Sample average
    2,       // Mode: Red + IR
    SAMPLE_RATE,
    411,     // Pulse width
    16384    // ADC range
  );

  memset(irBuffer, 0, sizeof(irBuffer));
  memset(redBuffer, 0, sizeof(redBuffer));
  memset(rates, 0, sizeof(rates));

  // Khởi tạo giá trị lọc ban đầu
  filteredHR = 70;
  filteredSpO2 = 100;
}

void loop() {
  particleSensor.check();

  while (particleSensor.available()) {
    uint32_t ir  = particleSensor.getFIFOIR();
    uint32_t red = particleSensor.getFIFORed();
    particleSensor.nextSample();

    irBuffer[bufferIndex]  = ir;
    redBuffer[bufferIndex] = red;
    bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;

    if (bufferIndex == 0) bufferFull = true;

    // Phát hiện nhịp tim
    if (checkForBeat(ir)) {
      long delta = millis() - lastBeat;
      lastBeat = millis();

      beatsPerMinute = 60 / (delta / 1000.0);

      if (beatsPerMinute > 40 && beatsPerMinute < 200) {
        rates[rateSpot++] = (byte)beatsPerMinute;
        rateSpot %= RATE_SIZE;

        long total = 0;
        byte validCount = 0;
        for (byte i = 0; i < RATE_SIZE; i++) {
          if (rates[i] > 0) {
            total += rates[i];
            validCount++;
          }
        }
        if (validCount > 0) {
          beatAvg = total / validCount;

          // Áp dụng bộ lọc EMA cho HR
          filteredHR = (int)((HR_ALPHA * beatAvg) + (1.0 - HR_ALPHA) * filteredHR);
        }
      }
    }

    // Tính SpO2 khi buffer đầy
    if (bufferFull && bufferIndex % BUFFER_SIZE == 0) {
      calculateSpO2();
    }
  }
}

void calculateSpO2() {
  long sumIR = 0, sumRed = 0;
  double sumRedSq = 0, sumIrSq = 0;

  for (int i = 0; i < BUFFER_SIZE; i++) {
    sumIR  += irBuffer[i];
    sumRed += redBuffer[i];
  }
  double dcIR  = sumIR  / BUFFER_SIZE;
  double dcRed = sumRed / BUFFER_SIZE;

  fingerPresent = (dcIR > FINGER_ON_DC);

  if (!fingerPresent || beatAvg < 40) {
    beatAvg = 0;
    filteredHR = 70;
    spo2 = 0;
    filteredSpO2 = 95;
    validSPO2 = false;
    stableCount = 0;
    dataJustSent = false;
    showMessage("Put finger\non sensor");
    digitalWrite(BUZZER_PIN, HIGH);
    return;
  }

  if (stableCount < SKIP_FIRST_BUFFERS) {
    stableCount++;
    showMessage("Measuring...");
    digitalWrite(BUZZER_PIN, HIGH);
    return;
  }

  for (int i = 0; i < BUFFER_SIZE; i++) {
    double acRed = redBuffer[i] - dcRed;
    double acIr  = irBuffer[i]  - dcIR;
    sumRedSq += acRed * acRed;
    sumIrSq  += acIr  * acIr;
  }

  double rmsRed = sqrt(sumRedSq / BUFFER_SIZE);
  double rmsIr  = sqrt(sumIrSq  / BUFFER_SIZE);

  if (rmsRed < 50 || rmsIr < 50) {
    validSPO2 = false;
    return;
  }

  double R = (rmsRed / dcRed) / (rmsIr / dcIR);
  spo2 = constrain((-25.0 * R + 110.0), 0, 100);
  validSPO2 = (R >= 0.4 && R <= 1.2);

  // Áp dụng bộ lọc EMA cho SpO2
  filteredSpO2 = (SPO2_ALPHA * spo2) + (1.0 - SPO2_ALPHA) * filteredSpO2;

  // Hiển thị giá trị đã lọc
  displayResult();

  digitalWrite(BUZZER_PIN, (filteredSpO2 < SPO2_LOW) ? LOW : HIGH);

  // Gửi dữ liệu đã lọc lên Firebase
  if (validSPO2 && filteredHR > 0 && !dataJustSent &&
      (millis() - sendDataPrevMillis > 10000 || sendDataPrevMillis == 0)) {
    pushDataToFirebase();
    dataJustSent = true;
    sendDataPrevMillis = millis();
  }
}

void pushDataToFirebase() {
  if (!Firebase.ready() || !signupOK) return;

  FirebaseJson json;
  json.set("Hr", filteredHR);
  json.set("SpO2", filteredSpO2);
  json.set("Ts/.sv", "timestamp");

  String path = "/test8/push";
  Firebase.RTDB.pushJSON(&fbdo, path, &json);
}

void displayResult() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("SpO2 & HR Monitor");

  display.setTextSize(2);
  display.setCursor(0, 20);

  if (validSPO2) {
    display.print("SpO2:");
    display.print(filteredSpO2, 1);
    display.println("%");
  } else {
    display.println("SpO2: --%");
  }

  display.setCursor(0, 45);
  if (filteredHR > 0) {
    display.print("HR:");
    display.print(filteredHR);
    display.println(" bpm");
  } else {
    display.println("HR: -- bpm");
  }

  display.display();
}

void showMessage(const char* msg) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(5, 20);
  display.println(msg);
  display.display();
}