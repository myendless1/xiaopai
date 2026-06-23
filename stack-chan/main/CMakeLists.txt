idf_component_register(
    SRCS
        "main.cpp"
        "codec_audio_output.cpp"
        "voice_state.cpp"
        "expression_controller.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_client esp_http_server esp_netif esp_event nvs_flash json app_update mbedtls tcp_transport esp_audio_codec esp_codec_dev esp32-camera esp_driver_i2s esp_driver_uart
)
