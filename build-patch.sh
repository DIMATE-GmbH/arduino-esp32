#!/usr/bin/env bash

# This script simply creates a ZIP archive resembling the one that can be
# downloaded inside the official repository and that is also used by Sloeber
# (the Eclipse-based Arduino IDE).

VERSION="3.3.7.2"
VERSION_HEX="$(printf '0x%x\n' $(echo $VERSION | tr -d '.'))"
VERSION_UNDERSCORE="$(echo $VERSION | tr '.' '_')"

GIT_COMMIT_SHORT="$(git log --format="%h" -n 1)"

FOLDER="esp32-core-$VERSION"
ARCHIVE="$FOLDER.zip"

rm -rf $FOLDER
rm -f $ARCHIVE

mkdir $FOLDER
mkdir $FOLDER/libraries
mkdir $FOLDER/tools
mkdir $FOLDER/variants

cp -a cores $FOLDER
cp -a libraries/BluetoothSerial $FOLDER/libraries
cp -a libraries/FS $FOLDER/libraries
cp -a libraries/SD $FOLDER/libraries
cp -a libraries/SD_MMC $FOLDER/libraries
cp -a libraries/SPI $FOLDER/libraries
cp -a tools/ide-debug $FOLDER/tools
cp -a tools/partitions $FOLDER/tools
cp tools/espota.py $FOLDER/tools
cp tools/gen_esp32part.py $FOLDER/tools
cp tools/gen_insights_package.py $FOLDER/tools
cp tools/pioarduino-build.py $FOLDER/tools
cp -a variants/pico32 $FOLDER/variants
cp Kconfig.projbuild $FOLDER
cp package.json $FOLDER
cp boards.txt $FOLDER
cp CMakeLists.txt $FOLDER
cp platform.txt $FOLDER
cp programmers.txt $FOLDER
cp idf_component.yml $FOLDER

touch $FOLDER/cores/esp32/core_version.h
echo "#define ARDUINO_ESP32_GIT_VER $VERSION_HEX" >> $FOLDER/cores/esp32/core_version.h
echo "#define ARDUINO_ESP32_GIT_DESC $VERSION-$GIT_COMMIT_SHORT" >> $FOLDER/cores/esp32/core_version.h
echo "#define ARDUINO_ESP32_RELEASE_$VERSION_UNDERSCORE" >> $FOLDER/cores/esp32/core_version.h
echo "#define ARDUINO_ESP32_RELEASE \"$VERSION_UNDERSCORE\"" >> $FOLDER/cores/esp32/core_version.h

zip -r $ARCHIVE $FOLDER >/dev/null

sha256 $ARCHIVE
stat -f%z $ARCHIVE
