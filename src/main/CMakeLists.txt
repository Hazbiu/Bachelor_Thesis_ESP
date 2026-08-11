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

        # Vision services
        "services/vision/face_detection/face_detector.cpp"
        "services/vision/face_recognition/face_recognizer.cpp"

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

        # Diagnostics
        "diagnostics/cpu_stats.c"
        "diagnostics/ai_pipeline_status.c"

    INCLUDE_DIRS
        "include"
        "ui"
        "power"
)
