## Initial Project:

- examples/esp-idf/16_video_lcd_display
- Link: https://github.com/waveshareteam/ESP32-P4-Platform/tree/main/examples/esp-idf/16_video_lcd_display

## ESP-WHO-Human_Face_Rec
To understand the concept
- https://github.com/espressif/esp-who/tree/master/examples/human_face_recognition

## Face Detection Model
``` bash
idf.py add-dependency "espressif/human_face_detect"
``` 
- Link: https://components.espressif.com/components/espressif/human_face_detect/versions/0.4.2/readme

## Face Recogintion Model
``` bash
idf.py add-dependency "espressif/human_face_recognition"
```
- https://components.espressif.com/components/espressif/human_face_recognition/versions/0.1.1/readme?language=en
 
## Reconfigured the ESP-IDF project
``` bash
idf.py reconfigure
## Project Build
idf.py build
``` 


## AI Pipeline

```text
OV5647 Camera
    ↓
Camera frame
    ↓
Face detection
    ↓
Face crop
    ↓
Face recognition
    ↓
Compare with stored face data (Needs to be improved)
    ↓
Display recognition result