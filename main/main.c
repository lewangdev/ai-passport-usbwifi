// main/main.c —— AI Passport USB 网卡固件入口。
//
// 设备经 Wi-Fi(STA)连路由器,经 USB(USB-Serial-JTAG + PPP)连电脑,
// 在两者之间做 NAT 转发,让没有无线网卡的电脑上网。
//
// 按键:UP/DOWN 调节屏幕亮度;OK 单击开关屏幕。
#include "app_status.h"
#include "log_ring.h"
#include "ui.h"
#include "usb_link.h"
#include "web_cfg.h"
#include "wifi_sta.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "main";

#define BRIGHTNESS_STEP     20
#define BRIGHTNESS_MIN      10
#define BRIGHTNESS_DEFAULT  60

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} key_event_t;

static QueueHandle_t s_keys;

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (ev != BSP_BTN_CLICK) return;
    const key_event_t k = { .btn = btn, .ev = ev };
    (void)xQueueSend(s_keys, &k, 0);
}

static void key_task(void *arg)
{
    (void)arg;
    int brightness = BRIGHTNESS_DEFAULT;
    bool screen_on = true;
    key_event_t k;
    for (;;) {
        if (xQueueReceive(s_keys, &k, portMAX_DELAY) != pdTRUE) continue;
        if (k.btn == BSP_BTN_OK) {
            screen_on = !screen_on;
        } else if (screen_on) {
            brightness += k.btn == BSP_BTN_UP ? BRIGHTNESS_STEP : -BRIGHTNESS_STEP;
            if (brightness > 100) brightness = 100;
            if (brightness < BRIGHTNESS_MIN) brightness = BRIGHTNESS_MIN;
        } else {
            screen_on = true;   // 熄屏时按 UP/DOWN 先点亮
        }
        bsp_display_backlight(screen_on ? (uint8_t)brightness : 0);
    }
}

static esp_err_t nvs_prepare(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 分区已不可用(满或格式版本不符),只能擦除后重建;仅影响 NVS 分区。
        ESP_LOGW(TAG, "NVS 不可用(%s),擦除重建", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    return err;
}

static void start_display(void)
{
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示初始化失败,继续以无屏模式运行网卡功能");
        return;
    }
    bsp_display_backlight(BRIGHTNESS_DEFAULT);
    if (bsp_lvgl_lock(1000)) {
        ui_create();
        bsp_lvgl_unlock();
    }

    s_keys = xQueueCreate(4, sizeof(key_event_t));
    if (s_keys && xTaskCreate(key_task, "keys", 2560, NULL, 4, NULL) == pdPASS) {
        esp_err_t err = bsp_button_init(on_key, NULL);
        if (err != ESP_OK) ESP_LOGW(TAG, "按键初始化失败: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    // USB-Serial-JTAG 将被 PPP 独占,日志改存内存,通过 http://10.77.0.1/ 查看。
    log_ring_install();
    ESP_LOGI(TAG, "AI Passport USB WiFi 启动");

    ESP_ERROR_CHECK(nvs_prepare());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    bsp_i2c_init();
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计不可用");
    start_display();

    // 网络部分是本固件的核心功能,失败时重启重试比"带病运行"更好。
    ESP_ERROR_CHECK(usb_link_start());
    ESP_ERROR_CHECK(wifi_sta_start());
    ESP_ERROR_CHECK(web_cfg_start());

    ESP_LOGI(TAG, "就绪,空闲堆 %lu 字节,最大块 %u 字节",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
