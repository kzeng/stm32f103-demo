#include "config_store.h"
#include <stddef.h>
#include <string.h>

/* Last 1 KiB page of the 64 KiB STM32F103C8T6 flash. */
#define CONFIG_FLASH_ADDRESS 0x0800FC00UL

static uint32_t config_checksum(const AppConfig *config)
{
  const uint8_t *bytes = (const uint8_t *)config;
  uint32_t checksum = 0xA5A55A5AUL;

  for (size_t i = 0; i < offsetof(AppConfig, checksum); ++i) {
    checksum = (checksum << 5) ^ (checksum >> 27) ^ bytes[i];
  }
  return checksum;
}

void ConfigStore_LoadDefaults(AppConfig *config)
{
  memset(config, 0, sizeof(*config));
  config->magic = CONFIG_STORE_MAGIC;
  config->version = CONFIG_STORE_VERSION;
  config->length = sizeof(*config);
  /* PC13 is the onboard LED and is active-low on the usual Blue Pill. */
  config->pins[5].mode = 1U;
  config->pins[5].level = 1U;
  config->led_mode = CONFIG_LED_MODE_BLINK;
}

HAL_StatusTypeDef ConfigStore_Load(AppConfig *config)
{
  const AppConfig *stored = (const AppConfig *)CONFIG_FLASH_ADDRESS;

  if (stored->magic != CONFIG_STORE_MAGIC ||
      stored->version != CONFIG_STORE_VERSION ||
      stored->length != sizeof(AppConfig) ||
      stored->checksum != config_checksum(stored)) {
    ConfigStore_LoadDefaults(config);
    return HAL_ERROR;
  }

  memcpy(config, stored, sizeof(*config));
  return HAL_OK;
}

HAL_StatusTypeDef ConfigStore_Save(const AppConfig *config)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t page_error = 0;
  AppConfig copy = *config;
  HAL_StatusTypeDef status;

  copy.magic = CONFIG_STORE_MAGIC;
  copy.version = CONFIG_STORE_VERSION;
  copy.length = sizeof(copy);
  copy.checksum = config_checksum(&copy);

  HAL_FLASH_Unlock();

  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.PageAddress = CONFIG_FLASH_ADDRESS;
  erase.NbPages = 1;
  status = HAL_FLASHEx_Erase(&erase, &page_error);

  if (status == HAL_OK) {
    const uint32_t *words = (const uint32_t *)&copy;
    for (size_t i = 0; i < sizeof(copy) / sizeof(uint32_t); ++i) {
      status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                  CONFIG_FLASH_ADDRESS + (uint32_t)(i * 4U),
                                  words[i]);
      if (status != HAL_OK) {
        break;
      }
    }
  }

  HAL_FLASH_Lock();
  return status;
}
