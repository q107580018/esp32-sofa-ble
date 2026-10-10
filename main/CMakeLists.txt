idf_component_register(SRCS "main.c"
                            "http_server.c"
                            "sofa_ble.c"
                            "sofa_console.c"
                            "sofa_control.c"
                            "sofa_proto.c"
                            "status_led.c"
                            "wifi_sta.c"
                       INCLUDE_DIRS "."
                       # v5.5 splits the drivers into esp_driver_* components and
                       # none of these are common requirements: a missing entry
                       # here is the #1 build error in this workspace.
                       # `bt` is the umbrella that exposes the NimBLE headers.
                       PRIV_REQUIRES bt nvs_flash esp_timer esp_driver_gpio
                                     console esp_driver_uart
                                     esp_wifi esp_netif esp_event esp_http_server
                       # index.html is linked into the binary and referenced as
                       # _binary_index_html_start / _binary_index_html_end.
                       EMBED_FILES "index.html")
