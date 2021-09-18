#ifndef BMP_FUNCTIONS
#define BMP_FUNCTIONS

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <LittleFS.h>

bool drawBmp(TFT_eSPI* tft, const char *filename, int16_t x, int16_t y);
uint16_t read16(fs::File &f);
uint32_t read32(fs::File &f);

#endif
