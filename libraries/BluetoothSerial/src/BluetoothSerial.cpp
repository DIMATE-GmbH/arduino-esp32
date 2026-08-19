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

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <esp_spp_api.h>

#include <esp32-hal-bt-mem.h>

#include <Arduino.h>

#include "BluetoothSerial.h"

#include "Debug.h"

// This is also added to the device file name when connected to by machines
// running macOS / Linux. Since this is the server name and not the device name,
// this is not visible inside the Bluetooth-related settings.
// Don't change this as this is persisted in the memory of the Bluetooth
// controller itself not the Flash / RAM / ROM of the ESP32, reading the old
// value from memory after every restart. Revoking any potential issues is very
// cumbersome and not very well documented.
constexpr PROGMEM const char *SERVER_NAME = "ESP32SPP";

static uint32_t _spp_client = 0; // NOSONAR
static QueueHandle_t _spp_rx_queue = nullptr; // NOSONAR
static QueueHandle_t _spp_tx_queue = nullptr; // NOSONAR
static SemaphoreHandle_t _spp_tx_done = nullptr; // NOSONAR
static TaskHandle_t _spp_task_handle = nullptr; // NOSONAR
static EventGroupHandle_t _spp_event_group = nullptr; // NOSONAR

// This is only used to persist the information between the two events
// "ESP_SPP_SRV_OPEN_EVT" and "ESP_SPP_SRV_CLOSE_EVT" that a connecting device
// required a second attempt to fully connect and that when the latter event is
// triggered (intentionally) there is no need to forget about that device!
static bool secondConnectionAttempt; // NOSONAR

// This is used alongside the FreeRTOS event group logic to save the information
// whether or not the communication is currently congested - meaning that it is
// overloaded and cannot handle more stuff currently.
//
// This could theoretically also be handled via a "volatile bool" variable and
// some logic related to waiting (mimicking "xEventGroupWaitBits") but I'm too
// lazy for that for now.
constexpr PROGMEM uint32_t NOT_OVERLOADED = 0x01;

// Maybe we have to maintain our own "queue" with packages with the size of the
// biggest message ("MAX_MESSAGE_SIZE"). The actual one ("_spp_tx_queue") only
// contains the pointer to elements created "on the fly". Issue with that is
// that the memory is memory is allocated on the fly and on errors not cleared
// correctly leaving the memory too fragmented.
// Our own "queue" would be more likely a "pool" that is somehow correctly
// linked to the actual "_spp_tx_queue" from FreeRTOS.
typedef struct { // NOSONAR
  size_t len;
  uint8_t data[]; // NOSONAR
} spp_packet_t;

/**
 *  This is invoked by the "write" method to move the data from the top-level
 *  Bluetooth Serial implementation, down to the actual Serial Port Profile
 *  (SPP) and then let it be handled by the underlying FreeRTOS implementation.
 */
static esp_err_t _spp_queue_packet(const uint8_t *data, size_t len) {
  // i) If there is no data or the length is not valid, inform about failure! By
  //    default this should not really be a problem but we will use it to find
  //    any issues with our Firmware implementation.
  if (data == nullptr || len == 0) {
    return ESP_FAIL;
  }

  // ii) Copy the actual data into the buffer.
  auto packet = (spp_packet_t*) malloc(sizeof(spp_packet_t) + len);
  if (!packet) {
    DEBUG_SERIAL("DEBUG BluetoothSerial::_spp_queue_packet malloc failed\n")
    return ESP_FAIL;
  }
  packet->len = len;
  memcpy(packet->data, data, len);

  // iii) Write the actual data, everything is handled via the underlying
  //      implementation.
  if (!_spp_tx_queue || xQueueSend(_spp_tx_queue, &packet, 1000) != pdPASS) {
    DEBUG_SERIAL("DEBUG BluetoothSerial::_spp_queue_packet queue send failed\n")
    free(packet);
    return ESP_FAIL;
  }
  return ESP_OK;
}

// This is related to the actual Bluetooth packages (being of 330 Bytes in size)
// and how to split the actual data into relevant packages.
const uint16_t SPP_TX_MAX = 330;
static uint8_t _spp_tx_buffer[SPP_TX_MAX]; // NOSONAR
static uint16_t _spp_tx_buffer_len = 0; // NOSONAR

/**
 *  This is used to write the actual buffer of outgoing data over the Serial
 *  Port Profile (SPP), called by the underlying FreeRTOS implementation -
 *  directly calling the ESP32 logic to do so.
 */
static bool _spp_send_buffer() {
  if (!_spp_client) {
    return false;
  }

  // i) Wait until for the buffer to not be "overloaded" anymore
  if ((xEventGroupWaitBits(_spp_event_group, NOT_OVERLOADED, pdFALSE, pdTRUE,
      1000) & NOT_OVERLOADED) == 0) {
    DEBUG_SERIAL("DEBUG BluetoothSerial::_spp_send_buffer overloaded\n")
    return false;
  }

  // ii) Write the buffer based on the Bluetooth package size (SPP_TX_MAX / 330
  //     Bytes)
  esp_err_t err = esp_spp_write(_spp_client, _spp_tx_buffer_len,
      _spp_tx_buffer);
  if (err != ESP_OK) {
    DEBUG_SERIAL("DEBUG BluetoothSerial::_spp_send_buffer write failed\n")
    return false;
  }

  // iii) Unblock the semaphore to not block future communication anymore
  _spp_tx_buffer_len = 0;
  if (xSemaphoreTake(_spp_tx_done, 1000) != pdTRUE) {
    DEBUG_SERIAL("DEBUG BluetoothSerial::_spp_send_buffer ACK failed\n")
    return false;
  }
  return true;
}

/**
 *  This is the actual task that will be used for writing outgoing data using
 *  the underlying FreeRTOS implementation.
 *
 *  It will be triggered internally via "xQueueSend" as the queue
 *  "_spp_tx_queue" is linked to the actual task.
 */
static void _spp_tx_task(void *arg) {
  spp_packet_t *packet = nullptr;
  size_t len = 0;
  size_t to_send = 0;
  uint8_t *data = nullptr;

  for (;;) {
    if (_spp_tx_queue
        && xQueueReceive(_spp_tx_queue, &packet, portMAX_DELAY) == pdTRUE
        && packet) {
      // The data is not bigger than a normal Bluetooth package, it does not
      // have to be split any further.
      if (packet->len <= (SPP_TX_MAX - _spp_tx_buffer_len)) {
        memcpy(_spp_tx_buffer + _spp_tx_buffer_len, packet->data, packet->len);
        _spp_tx_buffer_len += packet->len;
        free(packet);
        packet = nullptr;

        if (SPP_TX_MAX == _spp_tx_buffer_len
            || uxQueueMessagesWaiting(_spp_tx_queue) == 0) {
          // This can fail and would therefore lead to no data to be send to the
          // client actually. But as this is a task not linked to the initial
          // "write(...)" request we cannot really hand back the information.
          _spp_send_buffer();
        }
      } else {
        len = packet->len;
        data = packet->data;
        to_send = SPP_TX_MAX - _spp_tx_buffer_len;
        memcpy(_spp_tx_buffer + _spp_tx_buffer_len, data, to_send);
        _spp_tx_buffer_len = SPP_TX_MAX;
        data += to_send;
        len -= to_send;

        if (!_spp_send_buffer()) {
          len = 0;
        }

        while (len >= SPP_TX_MAX) {
          memcpy(_spp_tx_buffer, data, SPP_TX_MAX);
          _spp_tx_buffer_len = SPP_TX_MAX;
          data += SPP_TX_MAX;
          len -= SPP_TX_MAX;
          if (!_spp_send_buffer()) {
            len = 0;
            break;
          }
        }

        if (len) {
          memcpy(_spp_tx_buffer, data, len);
          _spp_tx_buffer_len += len;
          if (uxQueueMessagesWaiting(_spp_tx_queue) == 0) {
            _spp_send_buffer();
          }
        }

        free(packet);
        packet = nullptr;
      }
    }
  }

  vTaskDelete(nullptr);
  _spp_task_handle = nullptr;
}

/**
 *  This is the callback method invoked for every Bluetooth GAP event. It covers
 *  only the PIN request event as this might be called from the macOS Bluetooth
 *  stack when connecting to the ESP32 Bluetooth Classic interface. The PIN is
 *  hardcoded to "1234" as this is a legacy pairing method in contrary to SPP.
 *  Otherwise, macOS would hang in the authentication timeout.
 */
static void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
  case ESP_BT_GAP_PIN_REQ_EVT:
    DEBUG_SERIAL("DEBUG BluetoothSerial::esp_bt_gap_cb received ESP_BT_GAP_PIN_REQ_EVT\n")
    if (!param->pin_req.min_16_digit) {
      esp_bt_pin_code_t pin_code = {'1', '2', '3', '4'};
      esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
    } else {
      esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, nullptr);
    }
    break;

  default:
    break;
  }
}

/**
 *  This is the callback method invoked for every Serial Port Profile (SPP). It
 *  covers only the necessary events used for our use cases, being a slave
 *  device.
 */
static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) { // NOSONAR
  switch (event) {
  case ESP_SPP_INIT_EVT:
    // The Serial Port Profile (SPP) is initialized, time to set up the
    // Bluetooth server.
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, SERVER_NAME);
    break;

  case ESP_SPP_SRV_OPEN_EVT:
    // A connection towards the ESP32 Bluetooth Classic interface is opened.
    DEBUG_SERIAL("DEBUG BluetoothSerial::esp_spp_cb received ESP_SPP_SRV_OPEN_EVT\n")
    DEBUG_SERIAL("ESP_SPP_SRV_OPEN_EVT: status=%d handle=%lu current_client=%lu\n",
      param->srv_open.status, (unsigned long)param->srv_open.handle,
      (unsigned long)_spp_client);
    if (param->srv_open.status == ESP_SPP_SUCCESS) {
      if (!_spp_client) {
        _spp_client = param->srv_open.handle;
        _spp_tx_buffer_len = 0;
      } else {
        secondConnectionAttempt = true;
        esp_spp_disconnect(param->srv_open.handle);
      }
    }
    break;

  case ESP_SPP_CLOSE_EVT:
    // A connection towards (or from, not happening in our case) the ESP32
    // Bluetooth Classic interface is closed.
    DEBUG_SERIAL("DEBUG BluetoothSerial::esp_spp_cb received ESP_SPP_CLOSE_EVT\n")
    DEBUG_SERIAL("ESP_SPP_CLOSE_EVT: status=%d handle=%lu async=%d second_attempt=%d\n",
      param->close.status, (unsigned long)param->close.handle,
      param->close.async, secondConnectionAttempt);
    if ((param->close.async == false && param->close.status == ESP_SPP_SUCCESS)
        || param->close.async) {
      if (secondConnectionAttempt) {
        secondConnectionAttempt = false;
      } else {
        _spp_client = 0;
        xEventGroupSetBits(_spp_event_group, NOT_OVERLOADED);
      }
    }
    break;

  case ESP_SPP_DATA_IND_EVT:
    // Another device is sending data towards the ESP32 Bluetooth Classic
    // interface, this event is triggered when the data is received on this side.
    DEBUG_SERIAL("DEBUG BluetoothSerial::esp_spp_cb received ESP_SPP_DATA_IND_EVT\n")
    if (_spp_rx_queue != nullptr) {
      for (int i = 0; i < param->data_ind.len; i++) {
        if (xQueueSend(_spp_rx_queue, param->data_ind.data + i,
            (TickType_t )0) != pdTRUE) {
          break;
        }
      }
    }
    break;

  case ESP_SPP_WRITE_EVT:
    // Another device received data from the ESP32 Bluetooth Classic interface,
    // this event is triggered when the data was completely written.
    DEBUG_SERIAL("DEBUG BluetoothSerial::esp_spp_cb received ESP_SPP_WRITE_EVT\n")
    if (param->write.status == ESP_SPP_SUCCESS && param->write.cong) {
      // Writing was successful, the buffer is meant not to be overloaded
      // anymore (congested).
      xEventGroupClearBits(_spp_event_group, NOT_OVERLOADED);
    }
    xSemaphoreGive(_spp_tx_done);
    break;

  case ESP_SPP_CONG_EVT:
    // The "overload" information is changed based on the back and forth of the
    // data sent and received. Based on that, either clear or save that
    // information.
    DEBUG_SERIAL("DEBUG BluetoothSerial::esp_spp_cb received ESP_SPP_CONG_EVT\n")
    if (param->cong.cong) {
      xEventGroupClearBits(_spp_event_group, NOT_OVERLOADED);
    } else {
      xEventGroupSetBits(_spp_event_group, NOT_OVERLOADED);
    }
    break;

  default:
    break;
  }
}

static bool _init_bt(const char *deviceName) {
  // i) Create the event group for the normal Serial Port Profile (SPP), this is
  //    done via the underlying FreeRTOS implementation.
  if (!_spp_event_group) {
    _spp_event_group = xEventGroupCreate();
    if (!_spp_event_group) {
      return false;
    }
    xEventGroupSetBits(_spp_event_group, NOT_OVERLOADED);
  }

  // ii) Create the receive queue for incoming data, by default this is 512
  //     Bytes in size, this is done via the underlying FreeRTOS implementation.
  //     512 means the queue has space for this many elements.
  if (_spp_rx_queue == nullptr) {
    _spp_rx_queue = xQueueCreate(512, sizeof(uint8_t));
    if (_spp_rx_queue == nullptr) {
      return false;
    }
  }

  // iii) Create the send queue for outgoing data, by default this is
  //      32 * platform-specific pointer size (ESP32 should be 32-Bit, ergo 4
  //      Byte -> 32 * 4 = 128 Bytes), this is done via the underlying FreeRTOS
  //      implementation.
  //      32 means the queue has space for this many elements - pointers.
  if (_spp_tx_queue == nullptr) {
    _spp_tx_queue = xQueueCreate(32, sizeof(spp_packet_t*));
    if (_spp_tx_queue == nullptr) {
      return false;
    }
  }

  // iv) Create a semaphore for the task to the outgoing data, this is done via
  //     the underlying FreeRTOS implementation.
  if (_spp_tx_done == nullptr) {
    _spp_tx_done = xSemaphoreCreateBinary();
    if (_spp_tx_done == nullptr) {
      return false;
    }
    xSemaphoreTake(_spp_tx_done, 0);
  }

  // v) Create the actual task for the outgoing data, this is done via the
  //    underlying FreeRTOS implementation. The parameter "ulStackDepth" is set
  //    to 8192 Bytes as the task probably requires that much space on the stack
  //    for the underlying Bluetooth communication.
  if (!_spp_task_handle) {
    xTaskCreatePinnedToCore(_spp_tx_task, "spp_tx", 8192, nullptr,
    configMAX_PRIORITIES - 1, &_spp_task_handle, 0);
    if (!_spp_task_handle) {
      return false;
    }
  }

  // vi) Start the actual Bluetooth controller.
  if (!btStarted() && !btStartMode(BT_MODE_CLASSIC_BT)) {
    return false;
  }

  // vii) Start and enable the Bluedroid stack.
  esp_bluedroid_status_t bt_state = esp_bluedroid_get_status();
  if (bt_state
      == ESP_BLUEDROID_STATUS_UNINITIALIZED&& esp_bluedroid_init() != ESP_OK) {
    return false;
  }
  if (bt_state
      != ESP_BLUEDROID_STATUS_ENABLED&& esp_bluedroid_enable() != ESP_OK) {
    return false;
  }

  // viii) Register the callbacks for the Serial Port Profile (SPP) events and
  //       the legacy Bluetooth GAP PIN event.
  if (esp_spp_register_callback(esp_spp_cb) != ESP_OK) {
    return false;
  }

  if (esp_bt_gap_register_callback(esp_bt_gap_cb) != ESP_OK) {
    return false;
  }

  // ix) Apply the Serial Port Profile (SPP) enhanced configuration.
  esp_spp_cfg_t cfg = BT_SPP_DEFAULT_CONFIG();
  cfg.mode = ESP_SPP_MODE_CB;
  if (esp_spp_enhanced_init(&cfg) != ESP_OK) {
    return false;
  }

  // x) We set the device name.
  esp_bt_gap_set_device_name(deviceName);

  // xi) We have to set cod as the default mode does not work with the macOS BT stack!
  esp_bt_cod_t cod;
  cod.major = 0b00001; // Major Device Class: Computer
  cod.minor = 0b000100; // Minor Device Class: Laptop
  cod.service = 0b00000010110; // Service Class: Rendering, Capturing, Object Transfer
  if (esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD) != ESP_OK) {
    return false;
  }

  return true;
}

static void _stop_bt() {
  // i) Stop the Bluetooth controller and and shutdown the Bluedroid stack.
  if (btStarted()) {
    if (_spp_client) {
      esp_spp_disconnect(_spp_client);
    }
    esp_spp_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    btStop();
  }
  _spp_client = 0;

  // ii) Remove the task for outgoing data.
  if (_spp_task_handle) {
    vTaskDelete(_spp_task_handle);
    _spp_task_handle = nullptr;
  }

  // iii) Remove the event group for the normal Serial Port Profile (SPP).
  if (_spp_event_group) {
    vEventGroupDelete(_spp_event_group);
    _spp_event_group = nullptr;
  }

  // iv) Remove the queue for incoming data.
  if (_spp_rx_queue) {
    vQueueDelete(_spp_rx_queue);
    _spp_rx_queue = nullptr;
  }

  // v) Remove the queue for outgoing data by clearing it first if there are
  //    still elements in it that point to valid memory.
  if (_spp_tx_queue) {
    spp_packet_t *packet = nullptr;
    while (xQueueReceive(_spp_tx_queue, &packet, 0) == pdTRUE) {
      free(packet);
    }
    vQueueDelete(_spp_tx_queue);
    _spp_tx_queue = nullptr;
  }

  // vi) Remove the semaphore for the task for outgoing data.
  if (_spp_tx_done) {
    vSemaphoreDelete(_spp_tx_done);
    _spp_tx_done = nullptr;
  }
}

/** =========================================================================
 *  The actual implementation that is visible to the outside
 ** ========================================================================= */

bool BluetoothSerial::begin(const char *localName) {
  return _init_bt(localName);
}

int BluetoothSerial::available(void) {
  if (_spp_rx_queue == nullptr) {
    return 0;
  }
  return uxQueueMessagesWaiting(_spp_rx_queue);
}

int BluetoothSerial::peek(void) {
  uint8_t c = 0;
  if (_spp_rx_queue && xQueuePeek(_spp_rx_queue, &c, this->timeoutTicks)) { // NOSONAR
    return c;
  }
  return -1;
}

int BluetoothSerial::read() {
  uint8_t c = 0;
  if (_spp_rx_queue && xQueueReceive(_spp_rx_queue, &c, this->timeoutTicks)) { // NOSONAR
    return c;
  }
  return -1;
}

size_t BluetoothSerial::write(const uint8_t *buffer, size_t size) {
  if (!_spp_client) {
    return 0;
  }
  return (_spp_queue_packet(buffer, size) == ESP_OK) ? size : 0;
}

void BluetoothSerial::flush() {
  if (_spp_tx_queue != nullptr) {
    while (uxQueueMessagesWaiting(_spp_tx_queue) > 0) {
      delay(2);
    }
  }
}

void BluetoothSerial::end() {
  _stop_bt();
}

BluetoothSerial::~BluetoothSerial(void) {
  end();
}

void BluetoothSerial::setTimeout(int timeoutMS) {
  Stream::setTimeout(timeoutMS);
  this->timeoutTicks = timeoutMS / portTICK_PERIOD_MS;
}

// This is a compilation guard just in case the header is removed directly or
// transitively when included within other included headers! Otherwise there
// would be a failure in "btStartMode(...)" ("esp32-hal-bt.h") due to an error
// with "esp_bt_controller_init(...)" ("esp_bt.h") that is notoriously horrible
// to debug if you don't know that the issue is due to a missing header.
#ifndef ESP32_HAL_BT_MEM_H
#error "The necessary header 'esp32-hal-bt-mem.h' must be included!"
#endif
