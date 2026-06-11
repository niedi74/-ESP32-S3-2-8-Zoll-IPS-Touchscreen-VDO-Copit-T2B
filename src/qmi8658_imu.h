#pragma once
#include <Arduino.h>
#include <Wire.h>

#define QMI8658_ADDR         0x6B
#define QMI8658_WHO_AM_I     0x00
#define QMI8658_CTRL1        0x02
#define QMI8658_CTRL2        0x03
#define QMI8658_CTRL3        0x04
#define QMI8658_CTRL5        0x06
#define QMI8658_CTRL7        0x08
#define QMI8658_STATUSINT    0x2D
#define QMI8658_AX_L         0x35
#define QMI8658_AY_L         0x37
#define QMI8658_AZ_L         0x39
#define QMI8658_GX_L         0x3B
#define QMI8658_GY_L         0x3D
#define QMI8658_GZ_L         0x3F

struct IMUData { float x, y, z; };

extern IMUData g_imuAccel;
extern IMUData g_imuGyro;
extern float g_imuPitch;
extern float g_imuRoll;
extern float g_imuGForce;
extern bool g_imuPresent;
extern float g_imuOffPitch;
extern float g_imuOffRoll;
extern bool g_imuTrimmed;

bool qmi8658Detect(void);
void qmi8658Init(void);
void qmi8658Read(void);
bool qmi8658ShakeDetected(float threshold);
void qmi8658SetTrim(float pitchOff, float rollOff, bool active);
bool qmi8658Zero(void);
