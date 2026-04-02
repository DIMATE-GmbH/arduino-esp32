// Copyright 2018 Evandro Luis Copercini
// Copyright 2026 Tobias Hahnen, DIMATE GmbH
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _BLUETOOTH_SERIAL_H_
#define _BLUETOOTH_SERIAL_H_

#include <Stream.h>

class BluetoothSerial : public Stream {
public:
  BluetoothSerial(void) = default;
  bool begin(const char *localName);

  int available(void) override;
  int peek(void) override;
  int read(void) override;
  size_t write(const uint8_t *buffer, size_t size) override;
  size_t write(uint8_t c) override {
    return write(&c, 1);
  }
  void flush() override;

  void end(void);
  ~BluetoothSerial(void);

  void setTimeout(int timeoutMS);
  explicit operator bool() const {
    return true;
  }
private:
  int timeoutTicks = 0;
};

#endif
