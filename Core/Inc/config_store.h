#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

#define CONFIG_STORE_MAGIC   0x43464731UL
#define CONFIG_STORE_VERSION 1U
#define CONFIG_STORE_PIN_COUNT 6U

typedef struct {
  uint8_t mode;
  uint8_t level;
} ConfigPinState;

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  ConfigPinState pins[CONFIG_STORE_PIN_COUNT];
  uint32_t checksum;
} AppConfig;

void ConfigStore_LoadDefaults(AppConfig *config);
HAL_StatusTypeDef ConfigStore_Load(AppConfig *config);
HAL_StatusTypeDef ConfigStore_Save(const AppConfig *config);

#endif
