

#pragma once

#include <cstdint>
#include <string>
#include "movement-sequences.h"

bool sesameControllerInit();

void setServoAngle(uint8_t channel, int angle);

void setFace(const String& faceName);
void setFaceMode(FaceAnimMode mode);
void setFaceWithMode(const String& faceName, FaceAnimMode mode);

void delayWithFace(unsigned long ms);
void enterIdle();
bool pressingCheck(String cmd, int ms);

void sesameStopMovement();
void sesameRunCommand(const String& command);
