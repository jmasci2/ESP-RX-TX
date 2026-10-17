#pragma once

#include <cstdint>

void initializeEspNow();
bool sendInput(uint8_t type, uint8_t code, uint16_t value);
void sendKeepalive();
