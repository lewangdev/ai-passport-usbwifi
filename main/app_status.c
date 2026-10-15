// main/app_status.c —— 见 app_status.h。
#include "app_status.h"

#include "freertos/FreeRTOS.h"

static app_status_t s_status;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void app_status_get(app_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}

app_status_t *app_status_lock(void)
{
    taskENTER_CRITICAL(&s_lock);
    return &s_status;
}

void app_status_unlock(void)
{
    taskEXIT_CRITICAL(&s_lock);
}

const char *app_wifi_state_name(app_wifi_state_t s)
{
    switch (s) {
    case APP_WIFI_NO_CONFIG:  return "no_config";
    case APP_WIFI_CONNECTING: return "connecting";
    case APP_WIFI_CONNECTED:  return "connected";
    case APP_WIFI_RETRYING:   return "retrying";
    }
    return "unknown";
}

const char *app_link_state_name(app_link_state_t s)
{
    switch (s) {
    case APP_LINK_NO_USB:      return "no_usb";
    case APP_LINK_WAITING:     return "waiting";
    case APP_LINK_NEGOTIATING: return "negotiating";
    case APP_LINK_UP:          return "up";
    }
    return "unknown";
}
