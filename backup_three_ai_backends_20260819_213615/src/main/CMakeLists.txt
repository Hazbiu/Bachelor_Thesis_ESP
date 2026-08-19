idf_component_register(
    SRCS
        # Application orchestration
        "app/app_main.c"
        "app/app_boot.c"

        # User interface
        "ui/app_ui.c"
        "ui/pin_screen.c"

        # Camera platform
        "platform/camera/video_capture.c"

        # Vision public dispatchers
        "services/vision/face_detection/face_detector.cpp"
        "services/vision/face_recognition/face_recognizer.cpp"

        # ESP-DL reference backend
        "services/vision/face_detection/face_detector_espdl.cpp"
        "services/vision/face_recognition/face_recognizer_espdl.cpp"

        # TensorFlow Lite Micro FP32 comparison backend
        "services/vision/tflm/tflm_model_runner.cpp"
        "services/vision/face_detection/face_detector_tflm_fp32.cpp"
        "services/vision/face_recognition/face_recognizer_tflm_fp32.cpp"

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
)
