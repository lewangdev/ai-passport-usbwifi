idf_component_register(
    SRCS "main.c"
         "app_status.c"
         "form_parse.c"
         "link_cmd.c"
         "log_ring.c"
         "ui.c"
         "usb_link.c"
         "web_cfg.c"
         "wifi_sta.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_driver_usb_serial_jtag esp_event esp_http_server esp_netif
             esp_wifi lwip nvs_flash
)
