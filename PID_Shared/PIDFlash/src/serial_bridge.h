#pragma once
#include "flash_core.h"
namespace pidflash { namespace serialbridge {
bool receive(const Packet& packet,uint32_t now,bool blocked);
void poll(uint32_t now,bool blocked);
bool ownsPort();bool blocksMotor();bool waitingTx();bool readyForFlash();
void selectMotor();
} }
