#include <Arduino.h>
#include <Wire.h>
#include <MPU6050.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>

// ================== PINS (same as your working setup) ==================
#define SDA_PIN 21
#define SCL_PIN 22

#define PWMA_PIN 25
#define PWMB_PIN 26
#define AIN1_PIN 27
#define AIN2_PIN 14
#define BIN1_PIN 32
#define BIN2_PIN 33
#define STBY_PIN 13

// ================== PWM ==================
#define PWM_FREQ 20000
#define PWM_RESOLUTION 8
#define PWM_CHANNEL_A 0
#define PWM_CHANNEL_B 1

// ================== CONTROL LOOP ==================
#define LOOP_PERIOD_US 5000      // 200 Hz control loop
#define FALL_ANGLE 35.0          // give up beyond this tilt
#define GYRO_CAL_SAMPLES 500

MPU6050 mpu;

// IMU
int16_t ax, ay, az, gx, gy, gz;
float gxOffset = 0, gyOffset = 0, gzOffset = 0;
float pitch = 0;                 // filtered angle (deg)
float gyroRate = 0;              // pitch rate (deg/s) from gyro

// PID (tunable via BLE, persisted in NVS)
float Kp = 25.0;
float Ki = 80.0;                 // note: Ki here works on deg*s, tuned for real dt
float Kd = 0.8;                  // acts on gyro rate (derivative-on-measurement)
float targetAngle = 0.0;         // mechanical balance offset, find experimentally
float integral = 0.0;

bool balanceMode = false;
bool fallen = false;
int manualSpeedA = 0, manualSpeedB = 0;
int maxSpeed = 255;
int minSpeed = 30;               // stiction feed-forward, NOT a cutoff

int motorSpeedA = 0, motorSpeedB = 0;
uint32_t lastLoopUs = 0;

Preferences prefs;

// ================== BLE ==================
BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
BLECharacteristic* pControlCharacteristic = nullptr;
bool deviceConnected = false;

#define SERVICE_UUID        "19b10000-e8f2-537e-4f6c-d104768a1214"
#define CHAR_UUID           "19b10001-e8f2-537e-4f6c-d104768a1214"
#define CONTROL_CHAR_UUID   "19b10002-e8f2-537e-4f6c-d104768a1214"

void setMotorSpeed(int speedA, int speedB);

// ================== SETTINGS ==================
void loadSettings() {
  prefs.begin("balance", true);
  Kp = prefs.getFloat("Kp", Kp);
  Ki = prefs.getFloat("Ki", Ki);
  Kd = prefs.getFloat("Kd", Kd);
  targetAngle = prefs.getFloat("target", targetAngle);
  maxSpeed = prefs.getInt("maxSpeed", maxSpeed);
  minSpeed = prefs.getInt("minSpeed", minSpeed);
  prefs.end();
  // Never auto-load balanceMode=true: robot should always boot in safe mode
  Serial.println("Settings loaded from NVS");
}

void saveSettings() {
  prefs.begin("balance", false);
  prefs.putFloat("Kp", Kp);
  prefs.putFloat("Ki", Ki);
  prefs.putFloat("Kd", Kd);
  prefs.putFloat("target", targetAngle);
  prefs.putInt("maxSpeed", maxSpeed);
  prefs.putInt("minSpeed", minSpeed);
  prefs.end();
}

// ================== BLE CALLBACKS ==================
class MyCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) {
    String command = pChar->getValue();  // ESP32 core 3.x returns String
    if (command.length() == 0) return;

    if (command.startsWith("KP:")) {
      Kp = command.substring(3).toFloat();
      saveSettings();
    } else if (command.startsWith("KI:")) {
      Ki = command.substring(3).toFloat();
      integral = 0;
      saveSettings();
    } else if (command.startsWith("KD:")) {
      Kd = command.substring(3).toFloat();
      saveSettings();
    } else if (command.startsWith("TARGET:")) {
      targetAngle = command.substring(7).toFloat();
      saveSettings();
    } else if (command.startsWith("SPEED:")) {
      maxSpeed = constrain(command.substring(6).toInt(), 50, 255);
      saveSettings();
    } else if (command.startsWith("MIN:")) {
      minSpeed = constrain(command.substring(4).toInt(), 0, 100);
      saveSettings();
    } else if (command.startsWith("BALANCE:")) {
      balanceMode = command.substring(8).toInt();
      integral = 0;
      fallen = false;
      manualSpeedA = manualSpeedB = 0;
      Serial.print("Balance mode: ");
      Serial.println(balanceMode ? "ON" : "OFF");
    } else {
      // "A,B" manual motor command
      int commaIndex = command.indexOf(',');
      if (commaIndex > 0) {
        manualSpeedA = command.substring(0, commaIndex).toInt();
        manualSpeedB = command.substring(commaIndex + 1).toInt();
        balanceMode = false;
      }
    }
  }
};

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) { deviceConnected = true; }
  void onDisconnect(BLEServer*) {
    deviceConnected = false;
    // Safety: stop everything if controller disconnects
    balanceMode = false;
    manualSpeedA = manualSpeedB = 0;
    setMotorSpeed(0, 0);
    BLEDevice::startAdvertising();
  }
};

// ================== MOTORS ==================
void setMotorA(int speed) {
  if (speed > 0)      { digitalWrite(AIN1_PIN, HIGH); digitalWrite(AIN2_PIN, LOW);  ledcWrite(PWMA_PIN, speed); }
  else if (speed < 0) { digitalWrite(AIN1_PIN, LOW);  digitalWrite(AIN2_PIN, HIGH); ledcWrite(PWMA_PIN, -speed); }
  else                { digitalWrite(AIN1_PIN, LOW);  digitalWrite(AIN2_PIN, LOW);  ledcWrite(PWMA_PIN, 0); }
}

void setMotorB(int speed) {
  if (speed > 0)      { digitalWrite(BIN1_PIN, HIGH); digitalWrite(BIN2_PIN, LOW);  ledcWrite(PWMB_PIN, speed); }
  else if (speed < 0) { digitalWrite(BIN1_PIN, LOW);  digitalWrite(BIN2_PIN, HIGH); ledcWrite(PWMB_PIN, -speed); }
  else                { digitalWrite(BIN1_PIN, LOW);  digitalWrite(BIN2_PIN, LOW);  ledcWrite(PWMB_PIN, 0); }
}

void setMotorSpeed(int speedA, int speedB) {
  motorSpeedA = constrain(speedA, -255, 255);
  motorSpeedB = constrain(speedB, -255, 255);
  setMotorA(motorSpeedA);
  setMotorB(motorSpeedB);
}

// ================== IMU ==================
void calibrateGyro() {
  Serial.println("Calibrating gyro... keep robot STILL");
  long sx = 0, sy = 0, sz = 0;
  for (int i = 0; i < GYRO_CAL_SAMPLES; i++) {
    int16_t tx, ty, tz;
    mpu.getRotation(&tx, &ty, &tz);
    sx += tx; sy += ty; sz += tz;
    delay(3);
  }
  gxOffset = (float)sx / GYRO_CAL_SAMPLES;
  gyOffset = (float)sy / GYRO_CAL_SAMPLES;
  gzOffset = (float)sz / GYRO_CAL_SAMPLES;
  Serial.println("Gyro calibrated");
}

// Complementary filter with REAL measured dt
void updateAngle(float dt) {
  mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);

  // Pitch from accelerometer (deg). Adjust axes if your MPU is mounted differently.
  float accPitch = atan2(-(float)ax, sqrt((float)ay * ay + (float)az * az)) * 180.0 / PI;

  // Gyro rate in deg/s (FS_250 -> 131 LSB per deg/s)
  gyroRate = ((float)gy - gyOffset) / 131.0;

  const float alpha = 0.996;  // 200Hz: ~1.25s time constant
  pitch = alpha * (pitch + gyroRate * dt) + (1.0 - alpha) * accPitch;
}

// ================== PID ==================
float calculatePID(float dt) {
  float error = targetAngle - pitch;

  float P = Kp * error;

  // Integral with conditional anti-windup (don't integrate when saturated)
  float I = Ki * integral;

  // Derivative on measurement: gyro rate IS d(pitch)/dt, noise-free vs differencing
  float D = Kd * (-gyroRate);   // error derivative = -pitch derivative

  float output = P + I + D;

  // Anti-windup: only integrate if output isn't saturated (or error pushes it back)
  bool saturated = (output > maxSpeed && error > 0) || (output < -maxSpeed && error < 0);
  if (!saturated) {
    integral += error * dt;
    integral = constrain(integral, -maxSpeed / max(Ki, 0.001f), maxSpeed / max(Ki, 0.001f));
  }

  return constrain(output, (float)-maxSpeed, (float)maxSpeed);
}

// Map PID output to PWM, adding stiction feed-forward instead of cutting off
int applyDeadband(float output) {
  if (fabs(output) < 0.5) return 0;  // truly zero command
  int pwm = (int)(output > 0 ? output + minSpeed : output - minSpeed);
  return constrain(pwm, -255, 255);
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  loadSettings();

  // Safety first: motor driver disabled
  pinMode(STBY_PIN, OUTPUT);
  digitalWrite(STBY_PIN, LOW);

  pinMode(AIN1_PIN, OUTPUT); pinMode(AIN2_PIN, OUTPUT);
  pinMode(BIN1_PIN, OUTPUT); pinMode(BIN2_PIN, OUTPUT);
  digitalWrite(AIN1_PIN, LOW); digitalWrite(AIN2_PIN, LOW);
  digitalWrite(BIN1_PIN, LOW); digitalWrite(BIN2_PIN, LOW);

  ledcAttach(PWMA_PIN, PWM_FREQ, PWM_RESOLUTION);  // ESP32 core 3.x API
  ledcAttach(PWMB_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcWrite(PWMA_PIN, 0);
  ledcWrite(PWMB_PIN, 0);

  // I2C at 400kHz — MPU6050 supports it, halves read time vs 100kHz
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);

  Serial.println("Initializing MPU6050...");
  mpu.initialize();
  if (!mpu.testConnection()) {
    Serial.println("MPU6050 connection FAILED — check wiring (SDA=21, SCL=22)");
    while (1) delay(1000);
  }

  mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_250);
  mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_2);
  mpu.setDLPFMode(MPU6050_DLPF_BW_42); // on-chip 42Hz low-pass: kills motor vibration noise

  calibrateGyro();

  // Seed the filter with the accelerometer angle so it doesn't start at 0
  mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
  pitch = atan2(-(float)ax, sqrt((float)ay * ay + (float)az * az)) * 180.0 / PI;

  // BLE
  BLEDevice::init("ESP32_BalanceBot");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
    CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristic->addDescriptor(new BLE2902());

  pControlCharacteristic = pService->createCharacteristic(
    CONTROL_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
  pControlCharacteristic->setCallbacks(new MyCharacteristicCallbacks());

  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0);
  BLEDevice::startAdvertising();
  Serial.println("BLE started: ESP32_BalanceBot");

  digitalWrite(STBY_PIN, HIGH);
  Serial.println("Motor driver enabled. Balance mode OFF — send BALANCE:1 to start.");

  lastLoopUs = micros();
}

// ================== MAIN LOOP (fixed 200Hz) ==================
void loop() {
  // Wait for next loop tick (precise, drift-free)
  uint32_t now = micros();
  if (now - lastLoopUs < LOOP_PERIOD_US) return;
  float dt = (now - lastLoopUs) / 1000000.0f;
  lastLoopUs = now;
  if (dt > 0.05f) dt = LOOP_PERIOD_US / 1000000.0f; // guard against BLE stalls

  updateAngle(dt);

  if (balanceMode) {
    // Fall detection with latch — must re-enable manually after a fall
    if (fabs(pitch - targetAngle) > FALL_ANGLE) {
      if (!fallen) Serial.println("FALLEN — motors stopped. Send BALANCE:1 to retry.");
      fallen = true;
      balanceMode = false;
      integral = 0;
      setMotorSpeed(0, 0);
    } else {
      float pidOutput = calculatePID(dt);
      int pwm = applyDeadband(pidOutput);
      setMotorSpeed(pwm, pwm);
    }
  } else {
    setMotorSpeed(manualSpeedA, manualSpeedB);
  }

  // BLE telemetry at 10Hz
  static uint32_t lastBLE = 0;
  if (deviceConnected && millis() - lastBLE > 100) {
    lastBLE = millis();
    char buf[256];
    snprintf(buf, sizeof(buf),
      "{\"pitch\":%.2f,\"rate\":%.1f,\"motorA\":%d,\"motorB\":%d,"
      "\"Kp\":%.2f,\"Ki\":%.2f,\"Kd\":%.3f,\"target\":%.2f,"
      "\"maxSpeed\":%d,\"minSpeed\":%d,\"balance\":%d,\"fallen\":%d}",
      pitch, gyroRate, motorSpeedA, motorSpeedB,
      Kp, Ki, Kd, targetAngle, maxSpeed, minSpeed,
      balanceMode ? 1 : 0, fallen ? 1 : 0);
    pCharacteristic->setValue(buf);
    pCharacteristic->notify();
  }
}
