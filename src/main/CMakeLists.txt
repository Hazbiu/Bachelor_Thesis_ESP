idf_component_register(
    SRCS
        # Application orchestration
        "app/app_main.c"
        "app/app_runtime.c"
        "app/app_boot.c"
        "app/state/app_state_machine.c"
        "app/controller/app_controller.c"
        "app/authentication/authentication_flow.c"

        # User interface
        "ui/views/app_ui.c"
        "ui/views/settings_screen.c"
        "ui/views/pin_screen.c"
        "ui/renderers/face_overlay_renderer.c"

        # Authentication session
        "services/authentication/authentication_session.c"

        # Persistent application settings
        "services/settings/app_settings.c"

        # Camera platform
        "platform/camera/camera_runtime.c"
        "platform/camera/video_capture.c"

        # Display platform
        "platform/display/display_platform.c"

        # Storage platform
        "platform/storage/sd_card_storage.c"
        "platform/storage/spiffs_storage.c"

        # Vision public dispatchers
        "services/vision/face_detection/face_detector.cpp"
        "services/vision/face_recognition/face_recognizer.cpp"
        "services/vision/recognition_policy.c"
        "services/vision/face_geometry.c"
        "services/vision/face_detection_result_processor.c"
        "services/vision/face_recognition_result_processor.c"
        "services/vision/face_result_store.c"
        "services/vision/ai_snapshot.c"
        "services/vision/ai_snapshot_buffer.c"
        "services/vision/ai_snapshot_scheduler.c"
        "services/vision/ai_worker_state.c"
        "services/vision/ai_inference_guard.c"
        "services/vision/ai_worker_runtime.c"
        "services/vision/ai_worker_pipeline.c"

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
        "services/power/power_manager.c"
        "services/power/sleep/app_sleep.c"
        "services/power/sleep/light_sleep.c"
        "services/power/sleep/deep_sleep.c"
        "services/power/sleep/wake_up.c"
        "platform/power/cpu_power.c"
        "platform/power/component_wifi.c"
        "platform/power/component_ethernet.c"
        "services/power/component_runtime_policy.c"
        "platform/power/component_sdcard.c"
        "platform/power/component_audio.c"
        "platform/power/component_display.c"

        # Diagnostics
        "diagnostics/app_logging.c"
        "diagnostics/cpu_stats.c"
        "diagnostics/ai_pipeline_status.c"

    INCLUDE_DIRS
        "include"
        "ui"
        "ui/views"

    PRIV_REQUIRES
        nvs_flash
        spiffs
)
