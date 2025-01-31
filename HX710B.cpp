// MIT License
// Original code by Roland Pelayo
// https://github.com/kurimawxx00/hx710B_pressure_sensor

// Copyright 2023 陈桂鑫 (Juan Chan) <josedelinux@hotmail.com>
// All rights reserved.

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the “Software”), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// library for Arduino HX710B pressure sensor support

// Based on HX711 library https://github.com/bogde/HX711

#include "HX710B.h"

#include <Arduino.h>

// TEENSYDUINO has a port of Dean Camera's ATOMIC_BLOCK macros for AVR to ARM
// Cortex M3.
#define HAS_ATOMIC_BLOCK (defined(ARDUINO_ARCH_AVR) || defined(TEENSYDUINO))

// Whether we are running on either the ESP8266 or the ESP32.
#define ARCH_ESPRESSIF \
  (defined(ARDUINO_ARCH_ESP8266) || defined(ARDUINO_ARCH_ESP32))

// Whether we are actually running on FreeRTOS.
#define IS_FREE_RTOS defined(ARDUINO_ARCH_ESP32)

// Define macro designating whether we're running on a reasonable
// fast CPU and so should slow down sampling from GPIO.
#define FAST_CPU                                                \
  (ARCH_ESPRESSIF || defined(ARDUINO_ARCH_SAM) ||               \
   defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_STM32) || \
   defined(TEENSYDUINO))

#if HAS_ATOMIC_BLOCK
// Acquire AVR-specific ATOMIC_BLOCK(ATOMIC_RESTORESTATE) macro.
#include <util/atomic.h>
#endif

#if FAST_CPU
// Make shiftIn() be aware of clockspeed for
// faster CPUs like ESP32, Teensy 3.x and friends.
// See also:
// - https://github.com/bogde/HX710B/issues/75
// - https://github.com/arduino/Arduino/issues/6561
// -
// https://community.hiveeyes.org/t/using-bogdans-canonical-HX710B-library-on-the-esp32/539
uint8_t shiftInSlow(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder) {
  uint8_t value = 0;
  uint8_t i;

  for (i = 0; i < 8; ++i) {
    digitalWrite(clockPin, HIGH);
    delayMicroseconds(1);
    if (bitOrder == LSBFIRST)
      value |= digitalRead(dataPin) << i;
    else
      value |= digitalRead(dataPin) << (7 - i);
    digitalWrite(clockPin, LOW);
    delayMicroseconds(1);
  }
  return value;
}
#define SHIFTIN_WITH_SPEED_SUPPORT(data, clock, order) \
  shiftInSlow(data, clock, order)
#else
#define SHIFTIN_WITH_SPEED_SUPPORT(data, clock, order) \
  shiftIn(data, clock, order)
#endif

HX710B::HX710B() {}

HX710B::~HX710B() {}

void HX710B::begin(byte dout, byte pd_sck) {
  PD_SCK = pd_sck;
  DOUT = dout;

  pinMode(PD_SCK, OUTPUT);
  pinMode(DOUT, INPUT_PULLUP);
}

bool HX710B::is_ready() { return digitalRead(DOUT) == LOW; }

long HX710B::read() {
  // Wait for the chip to become ready.
  wait_ready();

  // Define structures for reading data into.
  unsigned long value = 0;
  uint8_t data[3] = {0};
  uint8_t filler = 0x00;

// Protect the read sequence from system interrupts.  If an interrupt occurs
// during the time the PD_SCK signal is high it will stretch the length of the
// clock pulse. If the total pulse time exceeds 60 uSec this will cause the
// HX710B to enter power down mode during the middle of the read sequence. While
// the device will wake up when PD_SCK goes low again, the reset starts a new
// conversion cycle which forces DOUT high until that cycle is completed.
//
// The result is that all subsequent bits read by shiftIn() will read back as 1,
// corrupting the value returned by read().  The ATOMIC_BLOCK macro disables
// interrupts during the sequence and then restores the interrupt mask to its
// previous state after the sequence completes, insuring that the entire
// read-and-gain-set sequence is not interrupted.  The macro has a few minor
// advantages over bracketing the sequence between `noInterrupts()` and
// `interrupts()` calls.
#if HAS_ATOMIC_BLOCK
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
#elif IS_FREE_RTOS
  // Begin of critical section.
  // Critical sections are used as a valid protection method
  // against simultaneous access in vanilla FreeRTOS.
  // Disable the scheduler and call portDISABLE_INTERRUPTS. This prevents
  // context switches and servicing of ISRs during a critical section.
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&mux);

#else
  // Disable interrupts.
  noInterrupts();
#endif

    // Pulse the clock pin 24 times to read the data.
    data[2] = SHIFTIN_WITH_SPEED_SUPPORT(DOUT, PD_SCK, MSBFIRST);
    data[1] = SHIFTIN_WITH_SPEED_SUPPORT(DOUT, PD_SCK, MSBFIRST);
    data[0] = SHIFTIN_WITH_SPEED_SUPPORT(DOUT, PD_SCK, MSBFIRST);

    // Set the channel and the gain factor for the next reading using the clock
    // pin.
    for (unsigned int i = 0; i < 128; i++) {
      digitalWrite(PD_SCK, HIGH);
#if ARCH_ESPRESSIF
      delayMicroseconds(1);
#endif
      digitalWrite(PD_SCK, LOW);
#if ARCH_ESPRESSIF
      delayMicroseconds(1);
#endif
    }

#if IS_FREE_RTOS
    // End of critical section.
    portEXIT_CRITICAL(&mux);

#elif HAS_ATOMIC_BLOCK
}

#else
  // Enable interrupts again.
  interrupts();
#endif

    // Replicate the most significant bit to pad out a 32-bit signed integer
    if (data[2] & 0x80) {
      filler = 0xFF;
    } else {
      filler = 0x00;
    }

    // Construct a 32-bit signed integer
    value = (static_cast<unsigned long>(filler) << 24 |
             static_cast<unsigned long>(data[2]) << 16 |
             static_cast<unsigned long>(data[1]) << 8 |
             static_cast<unsigned long>(data[0]));

    return static_cast<long>(value);
  }

  void HX710B::wait_ready(unsigned long delay_ms) {
    // Wait for the chip to become ready.
    // This is a blocking implementation and will
    // halt the sketch until a load cell is connected.
    while (!is_ready()) {
      // Probably will do no harm on AVR but will feed the Watchdog Timer (WDT)
      // on ESP. https://github.com/bogde/HX710B/issues/73
      delay(delay_ms);
    }
  }

  bool HX710B::wait_ready_retry(int retries, unsigned long delay_ms) {
    // Wait for the chip to become ready by
    // retrying for a specified amount of attempts.
    // https://github.com/bogde/HX710B/issues/76
    int count = 0;
    while (count < retries) {
      if (is_ready()) {
        return true;
      }
      delay(delay_ms);
      count++;
    }
    return false;
  }

  bool HX710B::wait_ready_timeout(unsigned long timeout,
                                  unsigned long delay_ms) {
    // Wait for the chip to become ready until timeout.
    // https://github.com/bogde/HX710B/pull/96
    unsigned long millisStarted = millis();
    while (millis() - millisStarted < timeout) {
      if (is_ready()) {
        return true;
      }
      delay(delay_ms);
    }
    return false;
  }

  long HX710B::read_average(byte times) {
    long sum = 0;
    for (byte i = 0; i < times; i++) {
      sum += read();
      // Probably will do no harm on AVR but will feed the Watchdog Timer (WDT)
      // on ESP. https://github.com/bogde/HX710B/issues/73
      delay(0);
    }
    return sum / times;
  }

  float HX710B::pascal() {
    float value = (read_average() * RES) * 200 + 500;
    return value;
  }

  float HX710B::atm() {
    float value = pascal() * 9.86923E-6;
    return value;
  }

  float HX710B::mmHg() {
    float value = pascal() * 0.00750062;
    return value;
  }

  float HX710B::psi() {
    float value = pascal() * 0.000145038;
    return value;
  }

  double HX710B::get_value(byte times) { return read_average(times) - OFFSET; }

  float HX710B::get_units(byte times) { return get_value(times) / SCALE; }

  void HX710B::tare(byte times) {
    double sum = read_average(times);
    set_offset(sum);
  }

  void HX710B::set_scale(float scale) { SCALE = scale; }

  float HX710B::get_scale() { return SCALE; }

  void HX710B::set_offset(long offset) { OFFSET = offset; }

  long HX710B::get_offset() { return OFFSET; }

  void HX710B::power_down() {
    digitalWrite(PD_SCK, LOW);
    digitalWrite(PD_SCK, HIGH);
  }

  void HX710B::power_up() { digitalWrite(PD_SCK, LOW); }
