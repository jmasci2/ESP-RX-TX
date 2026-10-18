idf_component_register(
    SRCS
        "main.cpp"
        "app_state.cpp"
        "esp_now_link.cpp"
        "touchscreen.cpp"
        "display.cpp"
        "buttons.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_i2c
        esp_driver_ledc
        esp_event
        esp_netif
        esp_psram
        esp_timer
        esp_wifi
        esp32_display_panel
        lvgl
        nvs_flash
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

idf_build_get_property(BUILD_COMPONENTS BUILD_COMPONENTS)
foreach(COMPONENT_NAME IN LISTS BUILD_COMPONENTS)
    if(COMPONENT_NAME MATCHES "lvgl")
        idf_component_get_property(LVGL_LIB ${COMPONENT_NAME} COMPONENT_LIB)
        target_compile_definitions(${LVGL_LIB} PUBLIC LV_CONF_INCLUDE_SIMPLE)
        target_include_directories(${LVGL_LIB} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
    endif()
endforeach()
