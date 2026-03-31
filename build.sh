#!/usr/bin/env bash

# This script simply creates a ZIP archive resembling the one that can be
# downloaded inside the official repository and that is also used by Sloeber
# (the Eclipse-based Arduino IDE).

VERSION="3.3.7.1"
ARCHIVE="esp32-core-$VERSION.zip"

rm -f $ARCHIVE
zip -r $ARCHIVE \
  cores/ \
  libraries/BluetoothSerial/ \
  libraries/FS/ \
  libraries/SD/ \
  libraries/SD_MMC/ \
  libraries/SPI/ \
  tools/ \
  variants/pico32/ \
  Kconfig.projbuild \
  package.json \
  boards.txt \
  CMakeLists.txt \
  platform.txt \
  programmers.txt \
  idf_component.yml
