#include "shell.h"
#include "main.h"
#include "app_version.h"
#include "config_store.h"
#include "cmsis_os.h"
#include "usb_device.h"
#include "FreeRTOS.h"
#include "portable.h"
#include "task.h"
#include "stream_buffer.h"
#include "usbd_cdc_if.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SHELL_RX_BUFFER_SIZE 256U
#define SHELL_LINE_SIZE 128U
#define SHELL_HISTORY_COUNT 8U
#define SHELL_RTOS_TASK_LIMIT 8U

typedef struct {
  const char *name;
  GPIO_TypeDef *port;
  uint16_t pin;
  uint8_t adc_channel;
  uint8_t config_index;
} ShellPin;

static const ShellPin shell_pins[] = {
  {"PA0", GPIOA, GPIO_PIN_0, ADC_CHANNEL_0, 0},
  {"PA1", GPIOA, GPIO_PIN_1, ADC_CHANNEL_1, 1},
  {"PA2", GPIOA, GPIO_PIN_2, ADC_CHANNEL_2, 2},
  {"PB0", GPIOB, GPIO_PIN_0, ADC_CHANNEL_8, 3},
  {"PB1", GPIOB, GPIO_PIN_1, ADC_CHANNEL_9, 4},
  {"PC13", GPIOC, GPIO_PIN_13, 0xFF, 5},
};

static StaticStreamBuffer_t shell_stream_struct;
static uint8_t shell_stream_storage[SHELL_RX_BUFFER_SIZE];
static StreamBufferHandle_t shell_stream;
static AppConfig app_config;
static char shell_history[SHELL_HISTORY_COUNT][SHELL_LINE_SIZE];
static char shell_history_draft[SHELL_LINE_SIZE];
static size_t shell_history_count;
static size_t shell_history_cursor;
static TaskStatus_t shell_rtos_tasks[SHELL_RTOS_TASK_LIMIT];

extern ADC_HandleTypeDef hadc1;
extern volatile uint32_t g_usb_stage;

static const ShellPin *find_pin(const char *name)
{
  for (size_t i = 0; i < sizeof(shell_pins) / sizeof(shell_pins[0]); ++i) {
    if (strcasecmp(name, shell_pins[i].name) == 0) {
      return &shell_pins[i];
    }
  }
  return NULL;
}

static void shell_write_bytes(const uint8_t *data, size_t length)
{
  while (length > 0U) {
    uint16_t chunk = (length > APP_TX_DATA_SIZE) ? APP_TX_DATA_SIZE : (uint16_t)length;
    uint32_t start = osKernelGetTickCount();

    for (;;) {
      uint8_t status = CDC_Transmit_FS((uint8_t *)data, chunk);
      if (status == USBD_OK) {
        break;
      }
      if (status != USBD_BUSY || osKernelGetTickCount() - start > 200U) {
        return;
      }
      osDelay(1);
    }

    data += chunk;
    length -= chunk;
  }
}

static void shell_write(const char *text)
{
  shell_write_bytes((const uint8_t *)text, strlen(text));
}

static void shell_prompt(void)
{
  shell_write("stm32> ");
}

static void shell_welcome(void)
{
  shell_write("\r\nSTM32F103C8T6 console\r\nType 'help' for commands.\r\n");
  shell_prompt();
}

static void shell_history_reset_cursor(const char *line)
{
  shell_history_cursor = shell_history_count;
  strncpy(shell_history_draft, line, SHELL_LINE_SIZE - 1U);
  shell_history_draft[SHELL_LINE_SIZE - 1U] = '\0';
}

static void shell_history_add(const char *line)
{
  if (line[0] == '\0') {
    shell_history_reset_cursor("");
    return;
  }
  if (shell_history_count > 0U &&
      strcmp(shell_history[shell_history_count - 1U], line) == 0) {
    shell_history_reset_cursor("");
    return;
  }
  if (shell_history_count == SHELL_HISTORY_COUNT) {
    memmove(shell_history[0], shell_history[1],
            (SHELL_HISTORY_COUNT - 1U) * sizeof(shell_history[0]));
    shell_history_count--;
  }
  strncpy(shell_history[shell_history_count], line, SHELL_LINE_SIZE - 1U);
  shell_history[shell_history_count][SHELL_LINE_SIZE - 1U] = '\0';
  shell_history_count++;
  shell_history_reset_cursor("");
}

static void shell_replace_line(char *line, size_t *length, const char *replacement)
{
  size_t replacement_length = strlen(replacement);

  while (*length > 0U) {
    shell_write("\b \b");
    (*length)--;
  }
  if (replacement_length >= SHELL_LINE_SIZE) {
    replacement_length = SHELL_LINE_SIZE - 1U;
  }
  memcpy(line, replacement, replacement_length);
  line[replacement_length] = '\0';
  *length = replacement_length;
  shell_write_bytes((const uint8_t *)line, replacement_length);
}

static void shell_history_up(char *line, size_t *length)
{
  if (shell_history_count == 0U) return;
  if (shell_history_cursor == shell_history_count) {
    strncpy(shell_history_draft, line, SHELL_LINE_SIZE - 1U);
    shell_history_draft[SHELL_LINE_SIZE - 1U] = '\0';
  }
  if (shell_history_cursor > 0U) shell_history_cursor--;
  shell_replace_line(line, length, shell_history[shell_history_cursor]);
}

static void shell_history_down(char *line, size_t *length)
{
  if (shell_history_count == 0U || shell_history_cursor >= shell_history_count) return;
  shell_history_cursor++;
  if (shell_history_cursor == shell_history_count) {
    shell_replace_line(line, length, shell_history_draft);
  } else {
    shell_replace_line(line, length, shell_history[shell_history_cursor]);
  }
}

static void trim_line(char *line)
{
  size_t length = strlen(line);
  while (length > 0 && isspace((unsigned char)line[length - 1])) {
    line[--length] = '\0';
  }
}

static LedMode config_led_mode(void)
{
  if (app_config.led_mode <= CONFIG_LED_MODE_BLINK) {
    return (LedMode)app_config.led_mode;
  }
  app_config.led_mode = CONFIG_LED_MODE_BLINK;
  return LED_MODE_BLINK;
}

static void apply_pin_config(const ShellPin *pin)
{
  const ConfigPinState *state = &app_config.pins[pin->config_index];
  GPIO_InitTypeDef init = {0};
  init.Pin = pin->pin;

  /* PC13 is the onboard LED.  Always keep it as a push-pull output so a
     persisted config cannot turn off the heartbeat by changing the mode. */
  if (pin->port == LED_GPIO_Port && pin->pin == LED_Pin) {
    init.Mode = GPIO_MODE_OUTPUT_PP;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(pin->port, &init);
    HAL_GPIO_WritePin(pin->port, pin->pin,
                     state->level ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return;
  }

  if (state->mode == 1U) {
    init.Mode = GPIO_MODE_OUTPUT_PP;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(pin->port, &init);
    HAL_GPIO_WritePin(pin->port, pin->pin,
                     state->level ? GPIO_PIN_SET : GPIO_PIN_RESET);
  } else if (state->mode == 2U) {
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(pin->port, &init);
  } else if (state->mode == 3U) {
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(pin->port, &init);
  } else {
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(pin->port, &init);
  }
}

static int parse_mode(const char *mode)
{
  if (strcasecmp(mode, "input") == 0) return 0;
  if (strcasecmp(mode, "output") == 0) return 1;
  if (strcasecmp(mode, "pullup") == 0) return 2;
  if (strcasecmp(mode, "pulldown") == 0) return 3;
  return -1;
}

static void command_help(void)
{
  shell_write("Commands:\r\n");
  shell_write("  help\r\n");
  shell_write("  version [info]\r\n");
  shell_write("  status\r\n");
  shell_write("  rtos [status|tasks]\r\n");
  shell_write("  led on|off|blink|toggle|status\r\n");
  shell_write("  gpio mode <PA0|PA1|PA2|PB0|PB1|PC13> <input|output|pullup|pulldown>\r\n");
  shell_write("  gpio write <pin> <0|1>\r\n");
  shell_write("  gpio read <pin>\r\n");
  shell_write("  adc read <PA0|PA1|PA2|PB0|PB1>\r\n");
  shell_write("  config save|load|reset\r\n");
  shell_write("  disconnect  USB detach and re-enumerate\r\n");
  shell_write("  exit        close shell session\r\n");
}

static void command_version(void)
{
  shell_write("STM32F103 console version " APP_VERSION_STRING "\r\n");
}

static const char *rtos_state_name(eTaskState state)
{
  switch (state) {
    case eRunning: return "running";
    case eReady: return "ready";
    case eBlocked: return "blocked";
    case eSuspended: return "suspend";
    case eDeleted: return "deleted";
    default: return "invalid";
  }
}

static void command_rtos(void)
{
  char response[128];
  size_t heap_free = xPortGetFreeHeapSize();
  size_t heap_min = xPortGetMinimumEverFreeHeapSize();
  UBaseType_t total_tasks = uxTaskGetNumberOfTasks();
  UBaseType_t task_count = uxTaskGetSystemState(shell_rtos_tasks,
                                                 SHELL_RTOS_TASK_LIMIT,
                                                 NULL);

  if (heap_free == 0U && heap_min == 0U) {
    snprintf(response, sizeof(response),
             "RTOS heap free=uninit total=%lu min=uninit tasks=%lu/%lu\r\n",
             (unsigned long)configTOTAL_HEAP_SIZE,
             (unsigned long)task_count,
             (unsigned long)total_tasks);
  } else {
    snprintf(response, sizeof(response),
             "RTOS heap free=%lu total=%lu min=%lu tasks=%lu/%lu\r\n",
             (unsigned long)heap_free,
             (unsigned long)configTOTAL_HEAP_SIZE,
             (unsigned long)heap_min,
             (unsigned long)task_count,
             (unsigned long)total_tasks);
  }
  shell_write(response);
  shell_write("name state priority stack_min(words)\r\n");
  for (UBaseType_t i = 0; i < task_count; ++i) {
    snprintf(response, sizeof(response), "%s %s %lu %lu\r\n",
             shell_rtos_tasks[i].pcTaskName,
             rtos_state_name(shell_rtos_tasks[i].eCurrentState),
             (unsigned long)shell_rtos_tasks[i].uxCurrentPriority,
             (unsigned long)shell_rtos_tasks[i].usStackHighWaterMark);
    shell_write(response);
  }
}

static void command_gpio(char **argv, int argc)
{
  const ShellPin *pin;

  if (argc < 3) {
    shell_write("ERR: gpio mode|write|read ...\r\n");
    return;
  }
  pin = find_pin(argv[2]);
  if (pin == NULL) {
    shell_write("ERR: unsupported pin\r\n");
    return;
  }

  if (pin->port == LED_GPIO_Port && pin->pin == LED_Pin &&
      strcasecmp(argv[1], "mode") == 0 && argc >= 4 &&
      strcasecmp(argv[3], "output") != 0) {
    shell_write("ERR: PC13 is reserved for LED; use led on|off|blink|toggle\r\n");
    return;
  }

  if (strcasecmp(argv[1], "mode") == 0 && argc >= 4) {
    int mode = parse_mode(argv[3]);
    if (mode < 0) {
      shell_write("ERR: invalid mode\r\n");
      return;
    }
    app_config.pins[pin->config_index].mode = (uint8_t)mode;
    apply_pin_config(pin);
    shell_write("OK\r\n");
  } else if (strcasecmp(argv[1], "write") == 0 && argc >= 4) {
    char *end = NULL;
    long value = strtol(argv[3], &end, 10);
    if (*argv[3] == '\0' || *end != '\0' || (value != 0 && value != 1)) {
      shell_write("ERR: value must be 0 or 1\r\n");
      return;
    }
    app_config.pins[pin->config_index].mode = 1U;
    app_config.pins[pin->config_index].level = (uint8_t)value;
    apply_pin_config(pin);
    shell_write("OK\r\n");
  } else if (strcasecmp(argv[1], "read") == 0) {
    char response[32];
    int value = HAL_GPIO_ReadPin(pin->port, pin->pin) == GPIO_PIN_SET;
    snprintf(response, sizeof(response), "%s=%d\r\n", pin->name, value);
    shell_write(response);
  } else {
    shell_write("ERR: invalid gpio command\r\n");
  }
}

static void command_led(const char *action)
{
  GPIO_PinState state = HAL_GPIO_ReadPin(LED_GPIO_Port, LED_Pin);
  LedMode mode = LED_GetMode();

  if (strcasecmp(action, "on") == 0) {
    mode = LED_MODE_ON;
    state = GPIO_PIN_RESET;
  } else if (strcasecmp(action, "off") == 0) {
    mode = LED_MODE_OFF;
    state = GPIO_PIN_SET;
  } else if (strcasecmp(action, "blink") == 0) {
    app_config.led_mode = CONFIG_LED_MODE_BLINK;
    LED_SetMode(LED_MODE_BLINK);
    shell_write("LED=blink\r\n");
    return;
  } else if (strcasecmp(action, "toggle") == 0) {
    mode = (mode == LED_MODE_ON) ? LED_MODE_OFF : LED_MODE_ON;
    state = (mode == LED_MODE_ON) ? GPIO_PIN_RESET : GPIO_PIN_SET;
  }
  else if (strcasecmp(action, "status") != 0) {
    shell_write("ERR: led on|off|blink|toggle|status\r\n");
    return;
  }

  if (strcasecmp(action, "status") != 0) {
    app_config.led_mode = (uint8_t)mode;
    LED_SetMode(mode);
    app_config.pins[5].mode = 1U;
    app_config.pins[5].level = (state == GPIO_PIN_SET) ? 1U : 0U;
    shell_write((mode == LED_MODE_ON) ? "LED=on\r\n" : "LED=off\r\n");
    return;
  }

  if (mode == LED_MODE_BLINK) {
    shell_write("LED=blink\r\n");
  } else {
    shell_write((state == GPIO_PIN_SET) ? "LED=off\r\n" : "LED=on\r\n");
  }
}

static void command_adc(const char *pin_name)
{
  const ShellPin *pin = find_pin(pin_name);
  ADC_ChannelConfTypeDef channel = {0};
  char response[64];
  uint32_t value;

  if (pin == NULL || pin->adc_channel == 0xFF) {
    shell_write("ERR: ADC pin must be PA0, PA1, PA2, PB0 or PB1\r\n");
    return;
  }
  channel.Channel = pin->adc_channel;
  channel.Rank = ADC_REGULAR_RANK_1;
  channel.SamplingTime = ADC_SAMPLETIME_55CYCLES_5;
  {
    GPIO_InitTypeDef init = {0};
    init.Pin = pin->pin;
    init.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_Init(pin->port, &init);
  }
  if (HAL_ADC_ConfigChannel(&hadc1, &channel) != HAL_OK ||
      HAL_ADC_Start(&hadc1) != HAL_OK ||
      HAL_ADC_PollForConversion(&hadc1, 20) != HAL_OK) {
    shell_write("ERR: ADC conversion failed\r\n");
    HAL_ADC_Stop(&hadc1);
    return;
  }
  value = HAL_ADC_GetValue(&hadc1);
  HAL_ADC_Stop(&hadc1);
  snprintf(response, sizeof(response), "%s raw=%lu mv=%lu\r\n", pin->name,
           (unsigned long)value, (unsigned long)((value * 3300UL) / 4095UL));
  shell_write(response);
}

static void command_config(const char *action)
{
  if (strcasecmp(action, "save") == 0) {
    shell_write(ConfigStore_Save(&app_config) == HAL_OK ? "OK: saved\r\n" : "ERR: save failed\r\n");
  } else if (strcasecmp(action, "load") == 0) {
    HAL_StatusTypeDef status = ConfigStore_Load(&app_config);
    for (size_t i = 0; i < sizeof(shell_pins) / sizeof(shell_pins[0]); ++i) apply_pin_config(&shell_pins[i]);
    LED_SetMode(config_led_mode());
    if (status == HAL_OK) {
      shell_write("OK: loaded\r\n");
    } else {
      shell_write("OK: defaults (no valid config)\r\n");
    }
  } else if (strcasecmp(action, "reset") == 0) {
    ConfigStore_LoadDefaults(&app_config);
    for (size_t i = 0; i < sizeof(shell_pins) / sizeof(shell_pins[0]); ++i) apply_pin_config(&shell_pins[i]);
    LED_SetMode(config_led_mode());
    shell_write("OK: defaults\r\n");
  } else {
    shell_write("ERR: config save|load|reset\r\n");
  }
}

static uint8_t execute_line(char *line)
{
  char *argv[5] = {0};
  int argc = 0;
  char *token = strtok(line, " \t");

  while (token != NULL && argc < 5) {
    argv[argc++] = token;
    token = strtok(NULL, " \t");
  }
  if (argc == 0) return 1U;

  if (strcasecmp(argv[0], "help") == 0) command_help();
  else if (strcasecmp(argv[0], "version") == 0 &&
           (argc == 1 || (argc == 2 && strcasecmp(argv[1], "info") == 0))) command_version();
  else if (strcasecmp(argv[0], "status") == 0) shell_write("OK: stm32f103c8t6 HAL+FreeRTOS USB-CDC version=" APP_VERSION_STRING "\r\n");
  else if (strcasecmp(argv[0], "rtos") == 0 &&
           (argc == 1 || (argc == 2 &&
                          (strcasecmp(argv[1], "status") == 0 ||
                           strcasecmp(argv[1], "tasks") == 0)))) command_rtos();
  else if (strcasecmp(argv[0], "led") == 0 && argc >= 2) command_led(argv[1]);
  else if (strcasecmp(argv[0], "gpio") == 0) command_gpio(argv, argc);
  else if (strcasecmp(argv[0], "adc") == 0 && argc >= 3 && strcasecmp(argv[1], "read") == 0) command_adc(argv[2]);
  else if (strcasecmp(argv[0], "config") == 0 && argc >= 2) command_config(argv[1]);
  else if (strcasecmp(argv[0], "disconnect") == 0) {
    shell_write("OK: USB disconnecting; reconnecting shortly\r\n");
    osDelay(50);
    USB_Device_Disconnect();
    osDelay(300);
    MX_USB_DEVICE_Init();
    (void)xStreamBufferReset(shell_stream);
    return 1U;
  } else if (strcasecmp(argv[0], "exit") == 0) {
    shell_write("OK: shell session closed; type a key to reopen\r\n");
    return 0U;
  }
  else shell_write("ERR: unknown command; type help\r\n");
  return 1U;
}

void Shell_Init(void)
{
  shell_stream = xStreamBufferCreateStatic(SHELL_RX_BUFFER_SIZE, 1,
                                            shell_stream_storage, &shell_stream_struct);
  ConfigStore_Load(&app_config);
  for (size_t i = 0; i < sizeof(shell_pins) / sizeof(shell_pins[0]); ++i) apply_pin_config(&shell_pins[i]);
  LED_SetMode(config_led_mode());
  shell_history_count = 0U;
  shell_history_cursor = 0U;
  shell_history_draft[0] = '\0';
}

void Shell_RxFromIsr(const uint8_t *data, uint32_t length)
{
  BaseType_t higher_priority_task_woken = pdFALSE;
  if (shell_stream != NULL) {
    xStreamBufferSendFromISR(shell_stream, data, length, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
  }
}

void Shell_Task(void *argument)
{
  char line[SHELL_LINE_SIZE];
  size_t length = 0;
  uint8_t ch;
  uint8_t escape_state = 0U;
  uint8_t session_active = 1U;
  uint8_t welcome_sent = 0U;
  uint8_t last_host_connected = 0U;
  (void)argument;

  g_usb_stage = 1U;
  osDelay(1000);
#ifndef STM32_DIAG_NO_USB
  /* Start USB after the scheduler and heartbeat are already running.  This
     keeps a USB clock/attach fault from hiding the LED/RTOS bring-up. */
  g_usb_stage = 2U;
  MX_USB_DEVICE_Init();
  g_usb_stage = 3U;
#endif

  for (;;) {
    uint8_t host_connected = CDC_HostConnected();
    if (host_connected != 0U && last_host_connected == 0U) {
      /* A terminal opened the CDC ACM port.  Send the banner again even if
       * the USB device had already enumerated before the terminal opened. */
      welcome_sent = 0U;
    }
    last_host_connected = host_connected;
    if (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) {
      if (!welcome_sent) {
        shell_welcome();
        welcome_sent = 1U;
      }
    } else {
      welcome_sent = 0U;
    }
    if (xStreamBufferReceive(shell_stream, &ch, 1, pdMS_TO_TICKS(50)) != 1) continue;
    if (!session_active) {
      session_active = 1U;
      length = 0U;
      line[0] = '\0';
      shell_history_reset_cursor("");
      shell_write("\r\n");
      shell_prompt();
    }
    if (escape_state == 1U) {
      escape_state = (ch == '[' || ch == 'O') ? 2U : 0U;
      continue;
    }
    if (escape_state == 2U) {
      escape_state = 0U;
      if (ch == 'A') shell_history_up(line, &length);
      else if (ch == 'B') shell_history_down(line, &length);
      continue;
    }
    if (ch == 0x1BU) {
      escape_state = 1U;
      continue;
    }
    if (ch == '\r' || ch == '\n') {
      shell_write("\r\n");
      line[length] = '\0';
      trim_line(line);
      shell_history_add(line);
      session_active = execute_line(line);
      length = 0;
      line[0] = '\0';
      escape_state = 0U;
      if (session_active) shell_prompt();
    } else if (ch == '\b' || ch == 0x7FU) {
      if (length > 0) {
        --length;
        line[length] = '\0';
        shell_write("\b \b");
        shell_history_reset_cursor(line);
      }
    } else if (isprint(ch) && length < sizeof(line) - 1U) {
      line[length++] = (char)ch;
      line[length] = '\0';
      shell_history_reset_cursor(line);
      shell_write_bytes(&ch, 1U);
    }
  }
}
