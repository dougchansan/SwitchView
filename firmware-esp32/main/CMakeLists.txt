idf_component_register(
  SRCS "main.cpp" "switch_usb.cpp" "switch_common.cpp"
  INCLUDE_DIRS "include" "${CMAKE_CURRENT_LIST_DIR}/../../proto"
  REQUIRES esp_tinyusb esp_wifi nvs_flash lwip esp_timer esp_event esp_netif)
