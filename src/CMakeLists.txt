idf_component_register(
    SRCS
        "app_main.cpp"
        "audio_capture.cpp"
        "dsp_features.cpp"
        "kws_model.cpp"
        "vad.cpp"
        "net_task.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_wifi
        esp_netif
        nvs_flash
        mbedtls
        lwip
        json
    # Add these via idf.py add-dependency once vendored/available:
    #   espressif/esp-dsp        (FFT + Mel helpers)
    #   espressif/esp-tflite-micro
    #   third-party libopus (fixed-point build) as a component
)
