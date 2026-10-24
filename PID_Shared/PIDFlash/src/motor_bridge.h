#pragma once
#include "motor_protocol.h"
namespace pidflash { namespace motorbridge {
void pause(uint32_t now);
void poll(uint32_t now,bool available);
bool owned();
bool inhibited();
bool status(MotorStatus& value,uint32_t& age,uint32_t now);
bool targets(uint32_t left100,uint32_t right100);
} }
