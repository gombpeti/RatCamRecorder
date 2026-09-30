// ============================================================================
// Camera Sync + Synchronous USART1 TX (STM32H7 + Arduino-style API)   -- v3
// written by Peter Gombkoto (pgombkoto@ethz.ch) and Alexei Vyssotski <alexei@ini.uzh.ch>
// ----------------------------------------------------------------------------
// Generates a TTL sync pulse train on user-selected digital pins at a specified
// frame rate, and sends a 16-bit frame counter over USART1 in synchronous mode
// (TX + external CLK).
//
// ---------------------------------------------------------------------------
// The USART1 synchronous section is VERIFIED AGAINST THE EXTERNAL CIRCUIT.
// setupUSART1_Sync(), the BRR divisor (1560), CR1/CR2/CR3, send16BitUSART1(),
// the Start/Stop/LED byte arrays and the TX loop inside
// SendStartStopLEDOnOff() are unchanged from v1/v2 -- DO NOT MODIFY.
// ---------------------------------------------------------------------------
//
// Serial control protocol (USB CDC @ baudrate). Every command is one line
// terminated by '\n', and every command answers with a final "OK" or
// "ERR <reason>" line.
//
//   Start / stop / LED (unchanged from v2, so existing hosts keep working):
//     1,39,30      -> 1 pin, "pin 39" (PH15), 30 fps. START.
//     1,39,0       -> STOP.
//     1,39,1       -> LED ON packet only    (reserved rate, see below)
//     1,39,2       -> LED OFF packet only   (reserved rate)
//
//   New in v3 -- timing configured in advance by the host:
//     DELAY <start_ms> [stop_ms]   set the settle windows, change nothing else
//     STATUS                       report state without disturbing it
//     VERSION                      report firmware version
//
//   Also accepted: the settle may ride along with a start command,
//     1,39,30,500        -> start, with a 500 ms start settle
//     1,39,30,500,1000   -> ... and a 1000 ms stop settle
//
// Reserved frame-rate band (unchanged):
//   Rates in [0.5, 2.5) are NOT frame rates. round(rate)==1 -> LED ON,
//   round(rate)==2 -> LED OFF. They never change frame_period and never start
//   or stop the pulse train. Real frame rates must be >= 2.5. 0 means STOP.
//
// Changes vs v2:
//  1. The settle windows are no longer compile-time constants. The host sets
//     them with DELAY (or as trailing fields on a start command) and they are
//     used by the next START/STOP. Nothing has to be reflashed to change them.
//  2. Line-based parsing. v2 used Serial.parseFloat() with no framing, so one
//     stray token shifted every field: "1,39,30" could be read as
//     num_pins=0, rate=1, which lands in the reserved band and was executed as
//     LED ON -- the board answered plausibly and no pin was ever driven. A
//     whole line is now read, parsed and validated before anything is applied,
//     and a malformed line is rejected entirely with ERR.
//  3. No unbounded blocking. v2 sat in `while (Serial.available() == 0) {}`,
//     so a truncated command wedged the board until a power cycle. Reading is
//     now non-blocking with a partial-line timeout.
//  4. The settle is a non-blocking state, not delay(). v2 called
//     FlushSerialBuffer() immediately AFTER its delay, so any command that
//     arrived during the 9 s window was silently discarded -- a start sent soon
//     after a stop was eaten and had to be retried. The pins stay LOW and
//     USART1 stays idle exactly as before, but serial keeps working throughout.
//  5. Explicit OK / ERR acknowledgement so the host can tell "applied" from
//     "ignored" without inferring it from echoed text.
//
// Preserved deliberately: the echo lines ("Number of digital pins: ",
// "Digital pins: ", "Frame rate set to: X.XX fps.", "Frame period: N us.") are
// byte-for-byte as v2, because existing hosts match on them.

#include "stm32h7xx_hal.h"

// ---------------------- Global configuration/state --------------------------
const int MAX_SYNC_PINS = 10;         // Size of DIG_out_pins[]

// Settle windows. Defaults match v2 so behaviour is identical until the host
// changes them; DELAY overrides both at run time.
uint32_t start_settle_ms = 9000;      // START packet -> first pulse
uint32_t stop_settle_ms  = 4000;      // after STOP

uint16_t counter = 0;                 // 16-bit frame counter sent on USART1
uint32_t baudrate = 115200;           // USB/Serial monitor baud (not USART1)
int DIG_out_pins[MAX_SYNC_PINS];      // List of user-selected digital pins
int n_sync = 0;                       // Number of sync output pins
float frame_rate_in = 0;              // Parsed frame rate from serial
float frame_rate_out = 0;             // Validated frame rate used by system
unsigned long frame_start = 0;        // Start time (micros) of current frame
unsigned long frame_period = 0;       // Frame period in microseconds

// Run state. v2 used a bool 'pulsing'; v3 adds the settle as a real state so
// the wait does not have to block serial.
enum RunState { STATE_IDLE, STATE_SETTLING, STATE_PULSING };
RunState run_state = STATE_IDLE;
unsigned long settle_started_ms = 0;
uint32_t settle_target_ms = 0;

//for Start/Stop
// VERIFIED AGAINST EXTERNAL HARDWARE -- unchanged, do not modify.
const uint8_t StartStopArrayLength = 8;
const uint8_t StartArray[] = {1,1,17,17,33,33,46,46};  //0010 1110; Mode 0
const uint8_t StopArray[] = {1,1,17,17,33,33,49,49};   //0011 0001; Mode 1
const uint8_t LED_ONArray[] = {1,1,17,17,33,33,206,206}; //1100 1110; Mode 2
const uint8_t LED_OFFArray[] = {1,1,17,17,33,33,209,209};//1101 0001; Mode 3

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
// VERIFIED AGAINST EXTERNAL HARDWARE -- unchanged from v1/v2, do not modify.
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
// VERIFIED AGAINST EXTERNAL HARDWARE -- unchanged, do not modify.
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
// condition unchanged from v1/v2.
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

// Configure the requested pins as outputs and force them LOW.
void ApplyPins(int num_pins) {
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
}

// Echo lines kept byte-for-byte identical to v2: hosts match on this text.
void ReportPins(int num_pins) {
  Serial.println();
  Serial.print("Number of digital pins: ");
  Serial.println(num_pins);

  Serial.print("Digital pins: ");
  for (int i = 0; i < num_pins; i++) {
    Serial.print(DIG_out_pins[i]);
    if (i + 1 < num_pins) Serial.print(",");
  }
  Serial.println();
}

void ReportFrameRate() {
  Serial.println();
  Serial.print("Frame rate set to: ");
  Serial.print(frame_rate_out);
  Serial.println(" fps.");
}

unsigned long SetFramePeriod(float fr) {
  if (fr <= 0.0f) return frame_period;   // Should not happen; keep old period
  frame_period = (unsigned long)(1e6 / fr);

  Serial.print("Frame period: ");
  Serial.print(frame_period);
  Serial.println(" us.");
  return frame_period;
}

// ============================== Line reader ==================================
// Non-blocking, with a timeout on a partial line. v2 blocked forever in
// `while (Serial.available() == 0) {}`, so a truncated command wedged the board
// until it was power cycled.
const size_t LINE_MAX = 96;
const unsigned long LINE_TIMEOUT_MS = 2000;
char line_buf[LINE_MAX];
size_t line_len = 0;
unsigned long line_started_ms = 0;

// Returns true when a complete line is ready in line_buf.
bool ReadLine() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (line_len == 0) line_started_ms = millis();
    if (c == '\r') continue;
    if (c == '\n') {
      line_buf[line_len] = '\0';
      size_t n = line_len;
      line_len = 0;
      if (n == 0) return false;       // bare newline: ignore, do not ERR
      return true;
    }
    if (line_len < LINE_MAX - 1) {
      line_buf[line_len++] = c;
    } else {
      // Overlong line: drop it rather than half-apply a truncated command.
      line_len = 0;
      Serial.println("ERR line too long");
    }
  }

  // A partial line that stops arriving must not block the next command.
  if (line_len > 0 && (millis() - line_started_ms) > LINE_TIMEOUT_MS) {
    line_len = 0;
    Serial.println("ERR partial line timed out");
  }
  return false;
}

// Split on commas / whitespace. Returns how many fields were parsed.
int SplitFloats(char* s, float* out, int maxOut) {
  int n = 0;
  char* tok = strtok(s, ", \t");
  while (tok && n < maxOut) {
    out[n++] = atof(tok);
    tok = strtok(NULL, ", \t");
  }
  return n;
}

bool StartsWithIgnoreCase(const char* s, const char* prefix) {
  while (*prefix) {
    if (toupper((unsigned char)*s++) != toupper((unsigned char)*prefix++))
      return false;
  }
  return true;
}

void PrintStatus() {
  Serial.print("STATE ");
  if (run_state == STATE_IDLE) Serial.print("IDLE");
  else if (run_state == STATE_SETTLING) Serial.print("SETTLING");
  else Serial.print("PULSING");
  Serial.print(" rate="); Serial.print(frame_rate_out);
  Serial.print(" period="); Serial.print(frame_period);
  Serial.print(" counter="); Serial.print(counter);
  Serial.print(" pins=");
  for (int i = 0; i < n_sync; i++) {
    Serial.print(DIG_out_pins[i]);
    if (i + 1 < n_sync) Serial.print(",");
  }
  Serial.print(" start_settle="); Serial.print(start_settle_ms);
  Serial.print(" stop_settle="); Serial.println(stop_settle_ms);
}

// Enter the settle window: pins LOW, USART1 idle, no counter output.
// Non-blocking, unlike v2's delay(), so serial keeps working throughout.
void BeginSettle(uint32_t ms) {
  SetPinsLow();
  settle_started_ms = millis();
  settle_target_ms = ms;
  run_state = STATE_SETTLING;
}

// ============================== Command handling =============================
void HandleLine(char* line) {
  // --- text commands ---
  if (StartsWithIgnoreCase(line, "VERSION")) {
    Serial.println("trigger_h7_CAM_Sync_Alexei v3");
    Serial.println("OK");
    return;
  }
  if (StartsWithIgnoreCase(line, "STATUS")) {
    PrintStatus();
    Serial.println("OK");
    return;
  }
  if (StartsWithIgnoreCase(line, "DELAY")) {
    float v[4];
    int n = SplitFloats(line + 5, v, 4);   // skip "DELAY"
    if (n < 1 || v[0] < 0) { Serial.println("ERR DELAY <start_ms> [stop_ms]"); return; }
    start_settle_ms = (uint32_t)v[0];
    if (n >= 2 && v[1] >= 0) stop_settle_ms = (uint32_t)v[1];
    Serial.print("Start settle: "); Serial.print(start_settle_ms); Serial.println(" ms.");
    Serial.print("Stop settle: ");  Serial.print(stop_settle_ms);  Serial.println(" ms.");
    Serial.println("OK");
    return;
  }

  // --- numeric command: <n>,<pins...>,<rate>[,<startMs>[,<stopMs>]] ---
  float v[MAX_SYNC_PINS + 4];
  int n = SplitFloats(line, v, MAX_SYNC_PINS + 4);
  if (n < 2) { Serial.println("ERR expected <n>,<pins...>,<rate>"); return; }

  int num_pins = (int)v[0];
  if (num_pins < 1 || num_pins > MAX_SYNC_PINS) {
    Serial.println("ERR pin count out of range");
    return;
  }
  if (n < num_pins + 2) {
    // Reject the whole line rather than applying half of it. This is what
    // stops one stray token from shifting every field, which in v2 turned
    // "1,39,30" into num_pins=0, rate=1 -- an LED command that drove no pin.
    Serial.println("ERR too few fields for that pin count");
    return;
  }

  int pins[MAX_SYNC_PINS];
  for (int i = 0; i < num_pins; i++) pins[i] = (int)v[1 + i];
  float rate = v[1 + num_pins];

  // Optional trailing settle values, so timing can ride along with the start.
  if (n >= num_pins + 3 && v[num_pins + 2] >= 0)
    start_settle_ms = (uint32_t)v[num_pins + 2];
  if (n >= num_pins + 4 && v[num_pins + 3] >= 0)
    stop_settle_ms = (uint32_t)v[num_pins + 3];

  // Everything validated -- now apply.
  SetPinsLow();                       // park whatever is currently selected
  n_sync = num_pins;
  for (int i = 0; i < num_pins; i++) DIG_out_pins[i] = pins[i];
  ApplyPins(num_pins);
  ReportPins(num_pins);

  frame_rate_in = rate;
  frame_rate_out = (rate <= 0) ? 0 : rate;
  ReportFrameRate();

  if (frame_rate_out >= 2.5f) {
    // Real frame rate -> START.
    // Order matters, exactly as in v2: the START packet goes out on USART1
    // first, then the settle window passes with the sync pins held LOW and
    // nothing on USART1, and only then does the pulse train / counter
    // transmission begin.
    SetFramePeriod(frame_rate_out);
    SendStartStopLEDOnOff(0);
    counter = 0;                      // every acquisition starts at frame 0
    BeginSettle(start_settle_ms);
    Serial.print("Settling "); Serial.print(start_settle_ms);
    Serial.println(" ms before the first pulse.");
    Serial.println("OK");
  } else if (frame_rate_out < 0.5f) {
    // STOP
    SendStartStopLEDOnOff(1);
    counter = 0;
    BeginSettle(stop_settle_ms);
    run_state = STATE_SETTLING;       // settle, then fall back to IDLE
    frame_rate_out = 0;
    Serial.println("OK");
  } else if ((int)(frame_rate_out + 0.5f) == 1) {
    // LEDs ON: command only, acquisition state and frame counter untouched
    SendStartStopLEDOnOff(2);
    Serial.println("OK");
  } else {
    // round(rate) == 2 -> LEDs OFF: command only
    SendStartStopLEDOnOff(3);
    Serial.println("OK");
  }
}

// ==== Arduino Setup ====
// setup() only initialises. The first configuration command is handled by
// loop()/HandleLine(), so the START packet is sent for the first configuration
// after power-on too.
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
  Serial.println("v3: also DELAY <start_ms> [stop_ms] | STATUS | VERSION");
  Serial.println("Camera sync system ready.");
}

// =============================== Arduino Loop ================================
void loop() {
  // Serial is serviced in every state, including during the settle window.
  // v2 sat inside delay() here and then flushed, so a command arriving in that
  // window was silently thrown away.
  if (ReadLine()) HandleLine(line_buf);

  if (run_state == STATE_SETTLING) {
    if ((millis() - settle_started_ms) >= settle_target_ms) {
      if (frame_rate_out >= 2.5f) {
        frame_start = micros();       // align the first frame window
        run_state = STATE_PULSING;
      } else {
        run_state = STATE_IDLE;       // settle after STOP finished
      }
    }
    return;                           // pins stay LOW, USART1 idle
  }

  if (run_state == STATE_PULSING) {
    SetPinsLow();
    while (Timer() < frame_period / 2) {}

    SetPinsHigh();
    send16BitUSART1(counter);

    while (Timer() < frame_period) {}

    frame_start += frame_period;
    counter++;
  }
}
