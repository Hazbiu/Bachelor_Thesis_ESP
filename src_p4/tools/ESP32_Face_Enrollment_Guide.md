# ESP32-P4 Face Enrollment Guide

This guide explains how to add a new person to the face-recognition database using the SD card. The example person is named `Keti`; replace `Keti` with the actual person's name.

## Important behavior

- Use one folder per person inside `/enroll`.
- The folder name becomes the recognized person's name.
- Use only clear, full-face images.
- Every generated RGB file must be exactly `230400` bytes (`320 × 240 × 3`).
- The firmware automatically rebuilds `/sdcard/FACE.DB` when the enrollment files change.
- You do not need to modify the firmware, delete `FACE.DB`, or flash the board again.

## Step 1: Insert the SD card into the computer

Insert the ESP32-P4 SD card into the computer and confirm that Ubuntu mounted it:

```bash
ls /media/hazbiu/SDCARD
```

The SD card should contain an `enroll` directory.

## Step 2: Create a folder for the new person

For a person named `Keti`, run:

```bash
mkdir -p /media/hazbiu/SDCARD/enroll/Keti
```

The enrollment structure will be:

```text
/media/hazbiu/SDCARD/enroll/
├── Keti/
├── Muazzam/
```

Use a short, unique folder name. This name will appear in the recognition result.

## Step 3: Add suitable photographs

Copy approximately 5–8 photographs into the new folder:

```bash
cp ~/Pictures/Keti/*.jpg /media/hazbiu/SDCARD/enroll/Keti/
```

Each photograph should have:

- Exactly one person.
- A complete and clearly visible face.
- Both eyes, the nose, and the mouth visible.
- A mostly frontal pose.
- Good lighting and sharp focus.
- Slight variations in expression, lighting, and head angle.

Do not use:

- Strong side-profile photographs.
- Blurred or dark photographs.
- Photographs with sunglasses, masks, or covered facial features.
- Photographs where the head, eyes, nose, or mouth is cropped.
- Photographs containing multiple people.

## Step 4: Convert the photographs to RGB files

Open the corrected converter directory and run it:

```bash
cd ~/Downloads/face_multi_person_update
python3 prepare_all_faces.py
```

The converter processes every person folder under:

```text
/media/hazbiu/SDCARD/enroll/
```

For every source photograph, use only the full-frame/letterboxed output. Do not use an `upper` crop because it can remove the nose or mouth and prevent detection of all five facial landmarks.

## Step 5: Inspect the generated previews

Open the new person's preview directory:

```bash
xdg-open /media/hazbiu/SDCARD/enroll/Keti/.face_previews
```

Check every preview. Keep it only if the complete face, both eyes, nose, and mouth are visible.

If a preview is unsuitable:

1. Remove or move the corresponding original photograph out of the person's enrollment folder.
2. Run the converter again.
3. Inspect the newly generated previews again.

Do not continue until every RGB candidate is based on a suitable preview.

## Step 6: Verify the RGB files

List the RGB files and their sizes:

```bash
find /media/hazbiu/SDCARD/enroll/Keti \
  -maxdepth 1 -type f -iname '*.rgb' \
  -printf '%f %s bytes\n'
```
ali
Every file must report:

```text
230400 bytes
```

Example valid output:

```text
001.rgb 230400 bytes
002.rgb 230400 bytes
003.rgb 230400 bytes
004.rgb 230400 bytes
005.rgb 230400 bytes
```

If an RGB file has a different size, do not use it. Regenerate the files with the converter.

## Step 7: Safely eject the SD card

Flush all pending writes:

```bash
sync
```

Then use Ubuntu's **Eject** button. Do not remove the card while a write operation is active.

## Step 8: Insert the card into the ESP32-P4

1. Power off the ESP32-P4 board.
2. Insert the SD card into the board.
3. Power on the board.

## Step 9: Start the serial monitor

Run:

```bash
cd ~/Programming/Bachelor_Thesis_ESP/src
idf.py -p /dev/ttyACM0 monitor 2>&1 | tee ~/face-db-rebuild.log
```

If the serial device is different, identify it before changing `/dev/ttyACM0`.

## Step 10: Start enrollment

On the ESP32-P4 touchscreen, press **Start Camera**.

The face system initializes at this point. The firmware scans all person folders and automatically rebuilds the shared database when it detects changed enrollment files.

Do not reset or power off the board while the database is being rebuilt.

## Step 11: Verify that the new person was enrolled

Look for messages similar to:

```text
Enrollment scan: people=3
Rebuilding one shared /sdcard/FACE.DB
Enrolled person=Keti file=001.rgb feature_id=...
FACE.DB rebuild complete
FACE.DB map: feature_id=... person=Keti
```

After leaving the monitor with `Ctrl+]`, filter the saved log:

```bash
grep -Ei \
  'Enrollment scan|Rebuilding|Enrolled person=Keti|rejected|FACE.DB rebuild complete|FACE.DB map' \
  ~/face-db-rebuild.log
```

Replace `Keti` in the command with the person's folder name.

Enrollment is successful only if at least one line contains:

```text
Enrolled person=Keti
```

Preferably, several valid photographs should be enrolled for the person.

## Step 12: Test live recognition

Stand in front of the camera with the face clearly visible and mostly frontal. Verify that the log reports the new person's name with a similarity score above the configured recognition threshold.

Occasional `Unknown` results can occur because of pose, distance, motion blur, or lighting. Test several frames before changing any recognition threshold.

## Troubleshooting

### The new person is always `Unknown`

Check whether the person was actually enrolled:

```bash
grep -Ei 'Enrolled person=Keti|Enrollment rejected|FACE.DB map' ~/face-db-rebuild.log
```

If every photograph was rejected, replace the photographs with clearer, frontal full-face images and repeat the conversion.

### The log says `no face with 5 landmarks`

The detector could not reliably find both eyes, the nose, and the mouth. Remove cropped, blurry, dark, or side-profile photographs and regenerate the RGB files.

### The database does not rebuild

Confirm that:

- The new folder is directly inside `/sdcard/enroll/`.
- The folder contains valid `.rgb` files.
- Every RGB file is `230400` bytes.
- The SD card is inserted before powering on the board.
- **Start Camera** was pressed.

### The converter produces `upper` variants

Do not enroll them. Use the corrected converter that creates only full-frame/letterboxed candidates. An `upper` crop can remove required facial landmarks.

## Final checklist

- [ ] A unique folder exists at `/enroll/<PersonName>/`.
- [ ] The folder contains 5–8 clear photographs.
- [ ] Every preview shows one complete, mostly frontal face.
- [ ] No side profiles or cropped `upper` candidates are used.
- [ ] Every `.rgb` file is exactly `230400` bytes.
- [ ] `sync` completed before ejecting the SD card.
- [ ] The SD card was inserted while the board was powered off.
- [ ] **Start Camera** was pressed.
- [ ] The log contains `Enrolled person=<PERSON>`.
- [ ] The database map contains the new person's name.

