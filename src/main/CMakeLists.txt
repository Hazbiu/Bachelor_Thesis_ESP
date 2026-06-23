idf_component_register(
    SRCS
        "app/app_main.c"
        "app/app_boot.c"
        "platform/camera/video_capture.c"
        "services/vision/face_detection/face_detector.cpp"
        "services/vision/face_recognition/face_recognizer.cpp"
    INCLUDE_DIRS "include"
)