idf_component_register(
    SRCS
        "main.cpp"
        "voice_state.cpp"
        "expression_controller.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_client esp_http_server esp_netif esp_event nvs_flash json app_update mbedtls tcp_transport esp_audio_codec esp32-camera esp_driver_uart
    EMBED_FILES
        "expressions/calm_face.png"
        "expressions/sleep_dark_face.png"
        "expressions/shy_face.png"
        "expressions/thinking_face.png"
        "expressions/relaxed_face.png"
        "expressions/smile_blink_face.png"
        "expressions/blink_half_face.png"
        "expressions/blink_closed_face.png"
        "expressions/wink_half_face.png"
        "expressions/wink_closed_face.png"
        "expressions/heart_small_face.png"
        "expressions/heart_face.png"
        "expressions/nod_soft_face.png"
        "expressions/nod_down_face.png"
        "expressions/happy_squint_face.png"
        "expressions/happy_squint_soft_face.png"
)
