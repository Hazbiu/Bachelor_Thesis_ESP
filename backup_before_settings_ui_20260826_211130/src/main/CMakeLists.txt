idf_component_register(
    SRCS
        # Application orchestration
        "app/app_main.c"
        "app/app_boot.c"

        # User interface
        "ui/app_ui.c"
        "ui/settings_screen.c"
        "ui/pin_screen.c"

        # Persistent application settings
        "settings/app_settings.c"

        # Camera platform
        "platform/camera/video_capture.c"

        # Vision public dispatchers
        "services/vision/face_detection/face_detector.cpp"
        "services/vision/face_recognition/face_recognizer.cpp"

        # ESP-DL reference backend
        "services/vision/face_detection/face_detector_espdl.cpp"
        "services/vision/face_recognition/face_recognizer_espdl.cpp"

        # TensorFlow Lite Micro backends. FP32 is the software baseline;
        # full-INT8 uses the same TFLM runner and the Espressif ESP-NN kernels.
        "services/vision/tflm/tflm_model_runner.cpp"
        "services/vision/face_detection/face_detector_tflm_fp32.cpp"
        "services/vision/face_recognition/face_recognizer_tflm_fp32.cpp"
        "services/vision/face_detection/face_detector_tflm_int8.cpp"
        "services/vision/face_recognition/face_recognizer_tflm_int8.cpp"

        # Power management and sleep modes
        "power/app_sleep.c"
        "power/light_sleep.c"
        "power/deep_sleep.c"
        "power/wake_up.c"
        "power/cpu_power.c"
        "power/component_wifi.c"
        "power/component_ethernet.c"
        "power/component_sdcard.c"
        "power/component_audio.c"
        "power/component_display.c"

        # Diagnostics
        "diagnostics/app_logging.c"
        "diagnostics/cpu_stats.c"
        "diagnostics/ai_pipeline_status.c"

    INCLUDE_DIRS
        "include"
        "ui"
        "power"

    PRIV_REQUIRES
        nvs_flash
        spiffs
)
