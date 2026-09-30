// ============================================================================
// Camera Sync + Synchronous USART1 TX (STM32H7 + Arduino-style API)   -- v2
// ----------------------------------------------------------------------------
// This firmware generates a TTL sync pulse train on user-selected digital pins
// at a specified frame rate, and sends a 16-bit frame counter over USART1 in
// synchronous mode (TX + external CLK).
//
// Serial control protocol (USB/Serial Monitor @ baudrate):
//   Input format:    #pins, <pin1>, <pin2>, ..., <frame_rate_fps>
//   Example START:   1,39,30     -> 1 pin, use "pin 39" (mapped to PH15), 30 fps
//   Example STOP:    1,39,0      -> same pin, 0 fps (stops pulsing)
//   Example LED ON:  1,39,1      -> reserved rate: sends LED_ON packet only
//   Example LED OFF: 1,39,2      -> reserved rate: sends LED_OFF packet only
//
// Reserved frame-rate band:
//   Rates in [0.5, 2.5) are NOT usable as frame rates. They are commands:
//     round(rate) == 1  -> LED ON packet
//     round(rate) == 2  -> LED OFF packet
//   These commands never change frame_period and never start or stop the pulse
//   train; an ongoing acquisition keeps running and the frame counter is kept.
//   Real frame rates must be >= 2.5 fps. 0 (or negative) means STOP.
//
// Notes:
// - Pin 39 is treated specially and mapped to STM32 port PH15 via HAL.
// - Other pins use Arduino-style pinMode/digitalWrite.
// - USART1 is set up for synchronous transmission with CLK on PA8 and TX on PA9.
// - The pulse is LOW for the first half period, then HIGH for the second half.
// - A 16-bit frame counter is sent each frame: high byte then low byte.
//
// ---------------------------------------------------------------------------
// The USART1 synchronous section is VERIFIED AGAINST THE EXTERNAL CIRCUIT.
// setupUSART1_Sync(), the BRR divisor (1560), CR1/CR2/CR3, send16BitUSART1(),
// the Start/Stop/LED byte arrays and the TX loop inside
// SendStartStopLEDOnOff() are unchanged from v1 -- DO NOT MODIFY.
// ---------------------------------------------------------------------------
//
// Changes vs v1 (control logic only):
//  1. Fixed ~36 min hang: a reserved-rate command (1 or 2) issued while the
//     system was stopped left frame_rate_out > 0 with frame_period = 0xFFFFFFFF,
//     so the loop busy-waited on frame_period/2. Run state is now a separate
//     'pulsing' flag instead of being inferred from frame_rate_out, and the
//     0xFFFFFFFF sentinel is gone.
//  2. Frame rates are floats end to end. SetFrameRate()/SetFramePeriod() used
//     to return/take unsigned long, which truncated 29.97 -> 29 and turned
//     0.5 into a STOP while still printing "0.50 fps".
//  3. Pin count from serial is clamped to MAX_SYNC_PINS (was an unchecked
//     write into a 10-element array).
//  4. The START packet is now also sent for the first configuration after
//     power-on: setup() only initialises, and the first serial command is
//     handled by the same path as every later one.
//  5. Rate == 2.5 exactly no longer falls through every branch (it is a real
//     rate now); the real-rate test is >= 2.5.
//  6. The frame counter is reset on START/STOP, but preserved across reserved
//     rate (LED) commands.
//  6b. START sequence is now explicit: START packet out on USART1 -> wait
//     START_SETTLE_MS with the sync pins LOW and USART1 idle -> only
//     then the frame loop with the 16-bit counter begins. No frame trigger and
//     no counter word can be emitted before the START packet has been sent.
//  7. Dead code removed (time_zero, frame_count, unused global Mode);
//     FlushSerialBuffer() drains with read() instead of parseFloat() timeouts;
//     the GPIO critical sections restore the previous interrupt state instead
//     of unconditionally enabling interrupts; stale comments corrected
//     (PH6 -> PH15, "9600 baud" removed, delay values match the code).

#include "stm32h7xx_hal.h"

// ---------------------- Global configuration/state --------------------------
const int MAX_SYNC_PINS = 10;         // Size of DIG_out_pins[]
// Settle window between the START packet leaving USART1 and the first frame
// pulse / first 16-bit counter word. Nothing is emitted during this window:
// the sync pins stay LOW and USART1 stays idle. Increase this to give the
// downstream device more time to initialise after it receives START.
// Milliseconds; uint32_t so values above 65535 ms are safe.
const uint32_t START_SETTLE_MS = 9000;   // <-- increase this for a longer arm delay
const uint32_t STOP_SETTLE_MS  = 4000;   // Host settle window after STOP

uint16_t counter = 0;                 // 16-bit frame counter sent on USART1
uint32_t baudrate = 115200;           // USB/Serial monitor baud (not USART1)
int DIG_out_pins[MAX_SYNC_PINS];      // List of user-selected digital pins
int n_sync = 0;                       // Number of sync output pins
float frame_rate_in = 0;              // Parsed frame rate from serial
float frame_rate_out = 0;             // Validated frame rate used by system
bool pulsing = false;                 // True while the pulse train is running
unsigned long frame_start = 0;        // Start time (micros) of current frame
unsigned long frame_period = 0;       // Frame period in microseconds

//for Start/Stop
const uint8_t StartStopArrayLength = 8;
const uint8_t StartArray[] = {1,1,17,17,33,33,46,46};  //0010 1110; Mode 0
const uint8_t StopArray[] = {1,1,17,17,33,33,49,49};   //0011 0001; Mode 1
const uint8_t LED_ONArray[] = {1,1,17,17,33,33,206,206}; //1100 1110; sent when frame rate is 1, but this frame rate is not executed; Mode 2
const uint8_t LED_OFFArray[] = {1,1,17,17,33,33,209,209};//1101 0001; sent wehen frame rate is 2, but this frame rate is not executed; Mode 3

// Force PH15 to a known LOW state as early as possible at boot so it does not
// float before any serial command configures sync outputs.
void InitPH15Low() {
  __HAL_RCC_GPIOH_CLK_ENABLE();

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOH, &GPIO_InitStruct);
  HAL_GPIO_WritePin(GPIOH, GPIO_PIN_15, GPIO_PIN_RESET);
}

// ================= USART1 CONFIGURATION (Synchronous TX + CLK) ===============
// VERIFIED AGAINST EXTERNAL HARDWARE -- unchanged from v1, do not modify.
void setupUSART1_Sync(uint32_t periph_clk) {
  // Enable clocks for GPIOA and USART1 peripheral
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_USART1_CLK_ENABLE();

  // Configure PA9 (TX) and PA8 (SCLK) to AF7, push-pull, very high speed
  // NOTE: PA9 is reassigned to USART1 AF here. This is harmless when Serial is
  // the USB CDC port (e.g. Portenta H7). On a board where Serial is routed to
  // USART1/PA9, Serial would stop working from this point on.
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_9 | GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  // Disable USART before configuration
  USART1->CR1 &= ~USART_CR1_UE;

  // Set baud/BRR: using APB2 clock divided by desired baud
  // periph_clk is passed in from setup() so it can be printed before PA9 is reassigned
  USART1->BRR = periph_clk / 1560;     // Fixed baud for synchronous TX
  // Enable transmitter, synchronous clock output; no extras in CR3
  USART1->CR1 = USART_CR1_TE;          // TX enable
  USART1->CR2 = USART_CR2_CLKEN;       // Enable synchronous clock output
  USART1->CR3 = 0;

  // Enable USART
  USART1->CR1 |= USART_CR1_UE;
}

// Send a 16-bit value over USART1, high byte first, then low byte.
// VERIFIED AGAINST EXTERNAL HARDWARE -- unchanged from v1, do not modify.
void send16BitUSART1(uint16_t value) {
  uint8_t high = (value >> 8) & 0xFF;
  uint8_t low  = value & 0xFF;

  // Wait for TX register empty, send high byte
  while (!(USART1->ISR & USART_ISR_TXE_TXFNF));
  USART1->TDR = high;

  // Wait again, send low byte
  while (!(USART1->ISR & USART_ISR_TXE_TXFNF));
  USART1->TDR = low;
}

//send start-stop
// TX loop VERIFIED AGAINST EXTERNAL HARDWARE -- byte sequence and wait
// condition unchanged from v1. Only the switch got an explicit break/default.
void SendStartStopLEDOnOff(uint8_t mode) {
  uint8_t i_t;
  for (i_t = 0; i_t < StartStopArrayLength; i_t++) {
  // Wait for TX register empty, send byte
  while (!(USART1->ISR & USART_ISR_TXE_TXFNF));
  switch (mode) {
  case 0: USART1->TDR = StartArray[i_t]; break;
  case 1: USART1->TDR = StopArray[i_t]; break;
  case 2: USART1->TDR = LED_ONArray[i_t]; break;
  case 3: USART1->TDR = LED_OFFArray[i_t]; break;
  default: break;}
  }
}

// =========================== TTL Pulse Functions =============================
// Drive all selected sync pins HIGH with interrupts masked while toggling.
// The previous interrupt state is restored, so these are safe to nest.
void SetPinsHigh() {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  for (int i = 0; i < n_sync; i++) {
    if (DIG_out_pins[i] == 39) {
      // Special handling: "pin 39" routed to PH15 via HAL
      HAL_GPIO_WritePin(GPIOH, GPIO_PIN_15, GPIO_PIN_SET); // PH15 HIGH
      digitalWrite(LED_BUILTIN, LOW);
    } else {
      digitalWrite(DIG_out_pins[i], HIGH);
    }
  }
  if (!primask) __enable_irq();
}

// Drive all selected sync pins LOW with interrupts masked while toggling.
void SetPinsLow() {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  for (int i = 0; i < n_sync; i++) {
    if (DIG_out_pins[i] == 39) {
      HAL_GPIO_WritePin(GPIOH, GPIO_PIN_15, GPIO_PIN_RESET); // PH15 LOW
      digitalWrite(LED_BUILTIN, HIGH);
    } else {
      digitalWrite(DIG_out_pins[i], LOW);
    }
  }
  if (!primask) __enable_irq();
}

// =============================== Timing Utils ================================
// Elapsed microseconds since the start of the current frame window. Unsigned
// arithmetic, so this stays correct across the micros() 32-bit wraparound.
unsigned long Timer() {
  return micros() - frame_start;
}

// Drain any leftover tokens in the serial RX buffer to keep parsing clean.
void FlushSerialBuffer() {
  while (Serial.available()) {
    Serial.read();          // discard leftover bytes (no parseFloat timeout)
  }
}

// Full timing reset: used on START and STOP. Resets the frame counter so every
// acquisition starts at frame 0.
void ResetTimer(uint32_t MyDelay) {
  delay(MyDelay);              // Settle window after config
  FlushSerialBuffer();         // Ensure buffer clean
  counter = 0;                 // Restart frame numbering
  frame_start = micros();      // Align start time
}

// Re-align the frame window to "now" without touching the frame counter.
// Used after a reserved-rate (LED) command so parsing/printing time does not
// leave the loop behind and produce a burst of catch-up frames, while an
// ongoing acquisition keeps its frame numbering.
void RealignTimer() {
  FlushSerialBuffer();
  frame_start = micros();
}


// ============================== Serial Config ================================
// Read user-selected digital pins from serial, configure them as outputs.
// Returns: number of pins configured (n_sync).
int SetDigPins() {
  while (Serial.available() == 0) {}
  int num_pins = int(Serial.parseFloat());

  // Guard against a malformed count overflowing DIG_out_pins[]
  if (num_pins < 0) num_pins = 0;
  if (num_pins > MAX_SYNC_PINS) num_pins = MAX_SYNC_PINS;

  for (int i = 0; i < num_pins; i++) {
    while (Serial.available() == 0) {}
    int input = int(Serial.parseFloat());
    DIG_out_pins[i] = input;
  }

  // Configure each requested output and force it LOW before any Serial prints,
  // so the selected sync pins never spend time floating during setup.
  for (int i = 0; i < num_pins; i++) {
    if (DIG_out_pins[i] == 39) {
      // Configure PWM7 PH15 manually for output (HAL-based)
      __HAL_RCC_GPIOH_CLK_ENABLE();
      GPIO_InitTypeDef GPIO_InitStruct = {0};
      GPIO_InitStruct.Pin = GPIO_PIN_15;
      GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
      GPIO_InitStruct.Pull = GPIO_NOPULL;
      GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
      HAL_GPIO_Init(GPIOH, &GPIO_InitStruct);
      HAL_GPIO_WritePin(GPIOH, GPIO_PIN_15, GPIO_PIN_RESET);
    } else {
      pinMode(DIG_out_pins[i], OUTPUT);
      digitalWrite(DIG_out_pins[i], LOW);
    }
  }

  Serial.println();
  Serial.print("Number of digital pins: ");
  Serial.println(num_pins);

  Serial.print("Digital pins: ");
  for (int i = 0; i < num_pins; i++) {
    Serial.print(DIG_out_pins[i]);
    if (i + 1 < num_pins) Serial.print(",");
  }
  Serial.println();

  return num_pins;
}

// Returns the requested frame rate as a float. Fractional rates such as 29.97
// are preserved (v1 truncated them through an unsigned long return value).
float SetFrameRate() {
  while (Serial.available() == 0) {}
  frame_rate_in = Serial.parseFloat();

  if (frame_rate_in <= 0) frame_rate_out = 0;
  else frame_rate_out = frame_rate_in;

  Serial.println();
  Serial.print("Frame rate set to: ");
  Serial.print(frame_rate_out);
  Serial.println(" fps.");
  return frame_rate_out;
}

// Only ever called with a real frame rate (>= 2.5 fps), so no 0xFFFFFFFF
// sentinel is needed any more; the 'pulsing' flag gates the pulse generator.
unsigned long SetFramePeriod(float fr) {
  if (fr <= 0.0f) return frame_period;   // Should not happen; keep old period
  frame_period = (unsigned long)(1e6 / fr);

  Serial.print("Frame period: ");
  Serial.print(frame_period);
  Serial.println(" us.");
  return frame_period;
}

// Read one complete command from serial and apply it.
//   rate >= 2.5   -> real frame rate: (re)start the pulse train, counter -> 0
//   rate <  0.5   -> STOP: stop pulsing, counter -> 0
//   round == 1/2  -> reserved: send LED ON/OFF packet only, leave run state,
//                    frame_period and the frame counter untouched
void HandleSerialConfig() {
  SetPinsLow();                 // Park the currently selected pins first
  n_sync = SetDigPins();
  frame_rate_out = SetFrameRate();

  if (frame_rate_out >= 2.5f) {
    // Real frame rate -> START.
    // Order matters: the START packet goes out on USART1 first, then we wait
    // START_SETTLE_MS with the sync pins held LOW and nothing on USART1, and
    // only then does the pulse train / counter transmission begin. 'pulsing'
    // is armed before the wait, but ResetTimer() delays before setting
    // frame_start, so no frame can fire during the window.
    SetFramePeriod(frame_rate_out);
    SendStartStopLEDOnOff(0);
    pulsing = true;
    ResetTimer(START_SETTLE_MS);
  } else if (frame_rate_out < 0.5f) {
    // STOP
    SendStartStopLEDOnOff(1);
    pulsing = false;
    ResetTimer(STOP_SETTLE_MS);
  } else if (round(frame_rate_out) == 1) {
    // LEDs ON: command only, acquisition state is not changed
    SendStartStopLEDOnOff(2);
    RealignTimer();
  } else {
    // round(frame_rate_out) == 2 -> LEDs OFF: command only
    SendStartStopLEDOnOff(3);
    RealignTimer();
  }
}

// ==== Arduino Setup ====
// setup() only initialises. The first configuration command is handled by
// loop()/HandleSerialConfig(), so the START packet is sent for the first
// configuration after power-on too (v1 skipped it).
void setup() {
  InitPH15Low();
  Serial.begin(baudrate);
  delay(500);
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.print("Set baudrate to ");
  Serial.println(baudrate);

  // Read and print periph_clk BEFORE setupUSART1_Sync() reassigns PA9
  uint32_t periph_clk = HAL_RCC_GetPCLK2Freq();
  Serial.println(periph_clk / 1560);
  Serial.flush();

  setupUSART1_Sync(periph_clk);

  Serial.println("Enter comma-separated string: #pins, pin IDs..., frame rate:");
  Serial.println("Camera sync system ready.");
}

// =============================== Arduino Loop ================================
// Main loop: if new config arrives, apply it; otherwise generate pulses and
// send the 16-bit frame counter on each frame.
void loop() {
  if (Serial.available()) {
    HandleSerialConfig();
  }

  if (pulsing) {
    SetPinsLow();
    while (Timer() < frame_period / 2) {}

    SetPinsHigh();
    send16BitUSART1(counter);

    while (Timer() < frame_period) {}

    frame_start += frame_period;
    counter++;
  }
}
