#include "qmi8658_imu.h"

IMUData g_imuAccel = {0, 0, 0};
IMUData g_imuGyro = {0, 0, 0};
float g_imuPitch = 0;
float g_imuRoll = 0;
float g_imuGForce = 0;
bool g_imuPresent = false;
float g_imuOffPitch = 0;
float g_imuOffRoll = 0;
bool g_imuTrimmed = false;

static float accelScale = 4.0f / 32768.0f;   // 4G range
static float gyroScale = 512.0f / 32768.0f; // 512 DPS range

static bool qmiWriteReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(QMI8658_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool qmiReadRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
    Wire.beginTransmission(QMI8658_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    uint8_t got = Wire.requestFrom((int)QMI8658_ADDR, (int)len);
    if (got != len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

bool qmi8658Detect(void) {
    uint8_t id = 0;
    if (!qmiReadRegs(QMI8658_WHO_AM_I, &id, 1)) return false;
    return (id == 0x05);
}

void qmi8658Init(void) {
    // Auto address increment, I2C mode
    qmiWriteReg(QMI8658_CTRL1, 0x60);
    delay(10);
    // Accel: 4G, 250Hz ODR
    qmiWriteReg(QMI8658_CTRL2, 0x24);
    // Gyro: 512DPS, 250Hz ODR
    qmiWriteReg(QMI8658_CTRL3, 0x64);
    // LPF on for both
    qmiWriteReg(QMI8658_CTRL5, 0x11);
    // Enable accel + gyro
    qmiWriteReg(QMI8658_CTRL7, 0x03);
    delay(10);
}

static bool qmi8658ReadSample(float* pitch, float* roll) {
    uint8_t buf[12];
    if (!qmiReadRegs(QMI8658_AX_L, buf, 12)) return false;

    int16_t ax = (buf[1] << 8) | buf[0];
    int16_t ay = (buf[3] << 8) | buf[2];
    int16_t az = (buf[5] << 8) | buf[4];
    int16_t gx = (buf[7] << 8) | buf[6];
    int16_t gy = (buf[9] << 8) | buf[8];
    int16_t gz = (buf[11] << 8) | buf[10];

    g_imuAccel.x = ax * accelScale;
    g_imuAccel.y = ay * accelScale;
    g_imuAccel.z = az * accelScale;
    g_imuGyro.x = gx * gyroScale;
    g_imuGyro.y = gy * gyroScale;
    g_imuGyro.z = gz * gyroScale;

    const float rawPitch = atan2(g_imuAccel.y, sqrt(g_imuAccel.x*g_imuAccel.x + g_imuAccel.z*g_imuAccel.z)) * 180.0 / PI;
    const float rawRoll  = atan2(-g_imuAccel.x, g_imuAccel.z) * 180.0 / PI;
    if (pitch) *pitch = rawPitch;
    if (roll)  *roll  = rawRoll;
    g_imuGForce = sqrt(g_imuAccel.x*g_imuAccel.x + g_imuAccel.y*g_imuAccel.y + g_imuAccel.z*g_imuAccel.z);
    return true;
}

void qmi8658SetTrim(float pitchOff, float rollOff, bool active) {
    g_imuOffPitch = pitchOff;
    g_imuOffRoll  = rollOff;
    g_imuTrimmed  = active;
}

void qmi8658Read(void) {
    float rawPitch = 0;
    float rawRoll = 0;
    if (!qmi8658ReadSample(&rawPitch, &rawRoll)) return;
    g_imuPitch = rawPitch - g_imuOffPitch;
    g_imuRoll  = rawRoll  - g_imuOffRoll;
}

bool qmi8658Zero(void) {
    float rawPitch = 0;
    float rawRoll = 0;
    if (!qmi8658ReadSample(&rawPitch, &rawRoll)) return false;
    g_imuOffPitch = rawPitch;
    g_imuOffRoll  = rawRoll;
    g_imuTrimmed  = true;
    g_imuPitch = 0;
    g_imuRoll  = 0;
    return true;
}

bool qmi8658ShakeDetected(float threshold) {
    return g_imuGForce > threshold;
}
