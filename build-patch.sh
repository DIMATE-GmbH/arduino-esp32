#!/usr/bin/env bash

# This script simply creates a ZIP archive resembling the one that can be
# downloaded inside the official repository and that is also used by Sloeber
# (the Eclipse-based Arduino IDE).

VERSION="3.3.7.2"
FOLDER="esp32-core-$VERSION"
ARCHIVE="$FOLDER.zip"

rm -rf $FOLDER
rm -f $ARCHIVE

mkdir $FOLDER
mkdir $FOLDER/libraries
mkdir $FOLDER/variants

cp -a cores $FOLDER
cp -a libraries/BluetoothSerial $FOLDER/libraries
cp -a libraries/FS $FOLDER/libraries
cp -a libraries/SD $FOLDER/libraries
cp -a libraries/SD_MMC $FOLDER/libraries
cp -a libraries/SPI $FOLDER/libraries
cp -a tools $FOLDER
cp -a variants/pico32 $FOLDER/variants
cp Kconfig.projbuild $FOLDER
cp package.json $FOLDER
cp boards.txt $FOLDER
cp CMakeLists.txt $FOLDER
cp platform.txt $FOLDER
cp programmers.txt $FOLDER
cp idf_component.yml $FOLDER

zip -r $ARCHIVE $FOLDER >/dev/null

sha256 $ARCHIVE
stat -f%z $ARCHIVE
