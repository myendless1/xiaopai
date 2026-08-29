idf_component_register(
    SRCS
        "main.cpp"
        "power_manager.cpp"
        "audio/xiaopai_audio_service.cpp"
        "codec_audio_output.cpp"
        "voice_state.cpp"
        "expression_controller.cpp"
        "xiaopai_state.cpp"
        "expression_state.cpp"
        "audio/dji_mic_receiver_input.cpp"
        "audio/dji_mic_usb_power.cpp"
        "audio/dji_mic_uac_recorder.cpp"
    INCLUDE_DIRS "." "audio"
    REQUIRES esp_wifi esp_http_client esp_http_server esp_netif esp_event nvs_flash json app_update mbedtls tcp_transport esp_audio_codec esp_codec_dev esp32-camera esp_driver_i2s esp_driver_uart esp_driver_usb_serial_jtag esp_driver_gpio esp_audio_effects esp-sr usb usb_stream
    PRIV_REQUIRES spi_flash
)
