#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"

#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

#define WIFI_SSID       "Lâm Hoàng"
#define WIFI_PASSWORD   "123456789"

#define API_KEY         "AIzaSyAk_i0lg7aTZK9cDKpPQfQTRQ712IC2v0A"
#define DATABASE_URL    "https://iot-project-ec071-default-rtdb.asia-southeast1.firebasedatabase.app/"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;
unsigned long sendDataPrevMillis = 0;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

MAX30105 particleSensor;

#define SDA_PIN     8
#define SCL_PIN     9
#define BUZZER_PIN  5

#define SAMPLE_RATE   200
#define SPO2_LOW      94
#define HR_LOW 40
#define HR_HIGH 120
#define BUFFER_SIZE   100
#define IR_FINGER_THRESHOLD 20000
#define SKIP_SAMPLE_COUNT   20

uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

int32_t spo2;
int8_t validSpO2;
int32_t heartRate;
int8_t validHeartRate;

#define HR_ALPHA    0.4
#define SPO2_ALPHA  0.3

float filteredHR = 70;
float filteredSpO2 = 95;

uint8_t skipCount = 0;

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(300);

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  Firebase.signUp(&config, &auth, "", "");
  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  signupOK = true;

  Wire.begin(SDA_PIN, SCL_PIN);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setTextColor(SSD1306_WHITE);
  showMessage("Put finger");

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    showMessage("Sensor error");
    while (1);
  }

  particleSensor.setup(
    0x3F,
    4,
    2,
    SAMPLE_RATE,
    411,
    16384
  );
}

void loop() {
  uint32_t ir = particleSensor.getIR();

  if (ir < IR_FINGER_THRESHOLD) {
    skipCount = 0;
    filteredHR = 0;
    filteredSpO2 = 0;
    digitalWrite(BUZZER_PIN, HIGH);
    showMessage("NO FINGER");
    delay(200);
    return;
  }

  if (skipCount < SKIP_SAMPLE_COUNT) {
    skipCount++;
    showMessage("Measuring...");
    delay(100);
    return;
  }

  for (int i = 0; i < BUFFER_SIZE; i++) {
    while (!particleSensor.available()) particleSensor.check();
    redBuffer[i] = particleSensor.getRed();
    irBuffer[i]  = particleSensor.getIR();
    particleSensor.nextSample();
  }

  maxim_heart_rate_and_oxygen_saturation(
    irBuffer, BUFFER_SIZE,
    redBuffer,
    &spo2, &validSpO2,
    &heartRate, &validHeartRate
  );

  if (validHeartRate && heartRate < 150) {
    filteredHR = (filteredHR == 0) ? heartRate : HR_ALPHA * heartRate + (1 - HR_ALPHA) * filteredHR;
  }

  if (validSpO2 && spo2 > 70 && spo2 <= 100) {
    filteredSpO2 = (filteredSpO2 == 0) ? spo2 : SPO2_ALPHA * spo2 + (1 - SPO2_ALPHA) * filteredSpO2;
  }

  if (filteredSpO2 == 0 || filteredHR == 0) {
    showMessage("Stabilizing");
    delay(100);
    return;
  }

  displayResult();

  digitalWrite(BUZZER_PIN, (filteredSpO2 < SPO2_LOW || filteredHR < HR_LOW || filteredHR > HR_HIGH) ? LOW : HIGH);

  if (Firebase.ready() && signupOK && millis() - sendDataPrevMillis > 5000) {
    pushDataToFirebase();
    sendDataPrevMillis = millis();
  }
}

void pushDataToFirebase() {
  FirebaseJson json;
  json.set("Hr", (int)filteredHR);
  json.set("SpO2", filteredSpO2);
  json.set("Ts/.sv", "timestamp");
  Firebase.RTDB.pushJSON(&fbdo, "/test8/push", &json);
}

void displayResult() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("SpO2 & HR");

  display.setTextSize(2);
  display.setCursor(0, 20);
  display.printf("SpO2:%d%%", (int)filteredSpO2);

  display.setCursor(0, 45);
  display.printf("HR:%d", (int)filteredHR);

  display.display();
}

void showMessage(const char* msg) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 22);
  display.println(msg);
  display.display();
}
