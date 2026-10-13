// UDP datagrams on port 4210 or newline-terminated USB serial lines: "<4-char session key>:<command bytes>". Each session gets a 2-column lane:
// 'W' working (bouncing block), 'S' / 's' + 4-char agent id: subagent started / stopped (a dot each, bottom rows),
// 'D' done (solid lane for 3 s, then a slow-blinking block until the next prompt), 'I' input needed (blinking "!"),
// 'E' tool failed (lane flashes), the on-board LED blinks while any lane needs input, a single working or asking session shows the dino, 'O' session gone (lane freed). With no active lane the dino mascot idles on the matrix.
#include <Arduino_LED_Matrix.h>
#include <WiFiS3.h>
#include "arduino_secrets.h"  // defines SECRET_SSID and SECRET_PASS

constexpr int ROWS = 8, COLS = 12, LANE_WIDTH = 2, LANES = COLS / LANE_WIDTH, KEY_LENGTH = 4;
constexpr int MAX_SUBAGENT_DOTS = 4, MAX_AGENTS = 8;
constexpr unsigned long STEP_MS = 120, BLINK_MS = 400, DONE_MS = 3000, WAITING_BLINK_MS = 1000;
constexpr unsigned long ERROR_MS = 800, ERROR_BLINK_MS = 100;
constexpr unsigned long DINO_BLINK_PERIOD_MS = 4000, DINO_BLINK_MS = 150, DINO_SLEEP_MS = 600000, SNORE_MS = 1000, WALK_STEP_MS = 250, EXCITED_STEP_MS = 120;
constexpr unsigned long WORK_TIMEOUT_MS = 600000;   // no hook heartbeat for this long => assume interrupted
constexpr unsigned long WAITING_TIMEOUT_MS = 3600000;  // finished lane nobody came back to, or a lost "O"
constexpr unsigned long INPUT_TIMEOUT_MS = 600000;  // "!" left unanswered, e.g. Esc at a permission prompt
constexpr unsigned long WIFI_RETRY_MS = 5000, WIFI_CHECK_MS = 1000;
constexpr unsigned long DEBUG_LEASE_MS = 5000, DEBUG_LANES_MS = 1000;
constexpr unsigned int PORT = 4210;
constexpr int PACKET_MAX = 32;

enum class Mode { FREE, WORKING, DONE, WAITING, INPUT_NEEDED };  // WAITING: finished, waiting for the next prompt

struct Lane {
  char key[KEY_LENGTH];
  Mode mode;
  unsigned long modeStart;
  unsigned long lastCommand;
  unsigned long errorUntil;  // 0 = no flash running
  char agentIds[MAX_AGENTS][KEY_LENGTH];  // running subagents; the first `agents` entries are valid
  int agents;
};

ArduinoLEDMatrix matrix;
WiFiUDP udp;
Lane lanes[LANES] = {};
uint8_t shown[ROWS][COLS];
unsigned long debugUntil = 0;  // a host sent "?" recently and wants the debug lines below

// Serial is true only while a host has the port open; availableForWrite() is always 0 on this core.
bool debugging() { return (long)(debugUntil - millis()) > 0 && Serial; }

char modeCode(Mode mode) {
  switch (mode) {
    case Mode::WORKING: return 'W';
    case Mode::DONE: return 'D';
    case Mode::WAITING: return 'P';
    case Mode::INPUT_NEEDED: return 'I';
    default: return '-';
  }
}

// "rx <packet>": every command packet received, from UDP or serial.
void debugPacket(const char* packet, int length) {
  if (!debugging()) return;
  Serial.print("rx ");
  Serial.write(packet, length);
  Serial.println();
}

// "F <8 rows as 3 hex digits>": the frame on the matrix, bit 11 is column 0.
void debugFrame(const uint8_t frame[ROWS][COLS]) {
  if (!debugging()) return;
  char line[1 + ROWS * 4 + 1] = "F";
  for (int r = 0; r < ROWS; r++) {
    unsigned bits = 0;
    for (int c = 0; c < COLS; c++) bits = (bits << 1) | frame[r][c];
    snprintf(line + strlen(line), 5, " %03X", bits);
  }
  Serial.println(line);
}

// "L <lane>...": "-" for a free lane, else "<key>:<mode>:<agents>:<seconds since its last command>".
void debugLanes(unsigned long now) {
  static unsigned long last = 0;
  if (now - last < DEBUG_LANES_MS || !debugging()) return;
  last = now;
  Serial.print("L");
  for (const Lane& lane : lanes) {
    Serial.print(' ');
    if (lane.mode == Mode::FREE) { Serial.print('-'); continue; }
    Serial.write(lane.key, KEY_LENGTH);
    Serial.print(':');
    Serial.print(modeCode(lane.mode));
    Serial.print(':');
    Serial.print(lane.agents);
    Serial.print(':');
    Serial.print((now - lane.lastCommand) / 1000);
  }
  Serial.println();
}

// Connects (and reconnects) without blocking, so USB serial works with no WiFi. Checked once per WIFI_CHECK_MS;
// the UDP socket is opened once the link is up.
void maintainWiFi(unsigned long now) {
  static bool connected = false;
  static unsigned long lastCheck = 0, lastAttempt = 0;
  if (now - lastCheck < WIFI_CHECK_MS) return;
  lastCheck = now;
  if (WiFi.status() == WL_CONNECTED) {
    if (!connected) {
      udp.stop();
      connected = udp.begin(PORT) != 0;  // retried at the next check if the reopen fails
      if (connected) Serial.println(WiFi.localIP());
    }
    return;
  }
  connected = false;
  if (now - lastAttempt >= WIFI_RETRY_MS) {
    WiFi.begin(SECRET_SSID, SECRET_PASS);
    lastAttempt = now;
  }
}

Lane* findLane(const char* key) {
  for (Lane& lane : lanes) {
    if (lane.mode != Mode::FREE && memcmp(lane.key, key, KEY_LENGTH) == 0) return &lane;
  }
  return nullptr;
}

// Prefers the free lane this session held before (so it keeps its position), then any free lane,
// then evicts the least recently active one.
Lane& allocateLane(const char* key) {
  Lane* target = nullptr;
  for (Lane& lane : lanes) {
    if (lane.mode == Mode::FREE && memcmp(lane.key, key, KEY_LENGTH) == 0) { target = &lane; break; }
  }
  if (!target) {
    for (Lane& lane : lanes) {
      if (lane.mode == Mode::FREE) { target = &lane; break; }
    }
  }
  if (!target) {
    unsigned long now = millis();
    target = &lanes[0];
    for (Lane& lane : lanes) {
      if (now - lane.lastCommand > now - target->lastCommand) target = &lane;
    }
  }
  *target = Lane{};
  memcpy(target->key, key, KEY_LENGTH);
  return *target;
}

void setMode(Lane& lane, Mode mode) {
  if (mode == Mode::DONE || mode == Mode::WAITING || mode == Mode::FREE) lane.agents = 0;
  if (mode == lane.mode) return;
  lane.mode = mode;
  lane.modeStart = millis();
}

int findAgent(const Lane& lane, const char* id) {
  for (int i = 0; i < lane.agents; i++) {
    if (memcmp(lane.agentIds[i], id, KEY_LENGTH) == 0) return i;
  }
  return -1;
}

// Duplicate starts, and stops of agents we never saw start, are ignored.
void addAgent(Lane& lane, const char* id) {
  if (findAgent(lane, id) >= 0 || lane.agents == MAX_AGENTS) return;
  memcpy(lane.agentIds[lane.agents++], id, KEY_LENGTH);
}

void removeAgent(Lane& lane, const char* id) {
  int i = findAgent(lane, id);
  if (i < 0) return;
  memmove(lane.agentIds[i], lane.agentIds[i + 1], (lane.agents - i - 1) * KEY_LENGTH);
  lane.agents--;
}

// Returns the session's lane (allocating one if it has none) and records that it just sent a command.
Lane& touch(Lane* existing, const char* key) {
  Lane& lane = existing ? *existing : allocateLane(key);
  lane.lastCommand = millis();
  return lane;
}

// agentId is only set for 'S' / 's'.
void handle(const char* key, char command, const char* agentId) {
  Lane* lane = findLane(key);
  switch (command) {
    case 'W': setMode(touch(lane, key), Mode::WORKING); break;
    case 'D': setMode(touch(lane, key), Mode::DONE); break;
    case 'I': setMode(touch(lane, key), Mode::INPUT_NEEDED); break;
    case 'S': if (lane) { addAgent(*lane, agentId); lane->lastCommand = millis(); } break;
    case 's': if (lane) { removeAgent(*lane, agentId); lane->lastCommand = millis(); } break;
    case 'E': if (lane) { lane->errorUntil = millis() + ERROR_MS; lane->lastCommand = millis(); } break;
    case 'O': if (lane) setMode(*lane, Mode::FREE); break;
  }
}

void dispatch(const char* packet, int length) {
  if (length <= KEY_LENGTH || packet[KEY_LENGTH] != ':') return;
  debugPacket(packet, length);
  for (int i = KEY_LENGTH + 1; i < length; i++) {
    char command = packet[i];
    const char* agentId = nullptr;
    if (command == 'S' || command == 's') {
      if (i + KEY_LENGTH >= length) return;  // truncated agent id
      agentId = &packet[i + 1];
      i += KEY_LENGTH;
    }
    handle(packet, command, agentId);
  }
}

void receiveUdp() {
  if (udp.parsePacket() == 0) return;
  char packet[PACKET_MAX];
  dispatch(packet, udp.read(packet, sizeof(packet)));
}

void receiveSerial() {
  static char line[PACKET_MAX];
  static int length = 0;
  static bool tooLong = false;  // a truncated line must not run its prefix
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n') {
      if (length == 1 && line[0] == '?') debugUntil = millis() + DEBUG_LEASE_MS;
      else if (!tooLong) dispatch(line, length);
      length = 0;
      tooLong = false;
    } else if (c != '\r') {
      if (length < PACKET_MAX) line[length++] = c;
      else tooLong = true;
    }
  }
}

// Moves finished lanes on and frees lanes whose session went silent. The key is kept so the
// session gets the same lane back.
void expire(Lane& lane, unsigned long now) {
  if (lane.mode == Mode::DONE && now - lane.modeStart >= DONE_MS) setMode(lane, Mode::WAITING);
  // Subagent tool calls send no heartbeat, so a lane with running subagents is never considered stalled.
  bool workStalled = lane.mode == Mode::WORKING && lane.agents == 0 && now - lane.lastCommand > WORK_TIMEOUT_MS;
  bool inputStale = lane.mode == Mode::INPUT_NEEDED && now - lane.lastCommand > INPUT_TIMEOUT_MS;
  bool waitingStale = lane.mode == Mode::WAITING && now - lane.lastCommand > WAITING_TIMEOUT_MS;
  if (workStalled || inputStale || waitingStale) setMode(lane, Mode::FREE);
}

void fill(uint8_t frame[ROWS][COLS], int x, int row, int rows) {
  for (int r = row; r < row + rows; r++)
    for (int c = x; c < x + LANE_WIDTH; c++) frame[r][c] = 1;
}

void drawLane(uint8_t frame[ROWS][COLS], int index, const Lane& lane, unsigned long now) {
  int x = index * LANE_WIDTH;
  unsigned long elapsed = now - lane.modeStart;
  if (lane.mode != Mode::FREE && (long)(lane.errorUntil - now) > 0) {  // signed: errorUntil is 0 when unset
    if ((now / ERROR_BLINK_MS) % 2 == 0) fill(frame, x, 0, ROWS);
    return;
  }
  switch (lane.mode) {
    case Mode::WORKING: {
      int step = (elapsed / STEP_MS) % 8;  // block rows bounce 0,1,2,3,4,3,2,1
      fill(frame, x, step <= 4 ? step : 8 - step, 2);
      for (int dot = 0; dot < lane.agents && dot < MAX_SUBAGENT_DOTS; dot++)
        frame[6 + dot / LANE_WIDTH][x + dot % LANE_WIDTH] = 1;
      break;
    }
    case Mode::DONE:
      fill(frame, x, 0, ROWS);
      break;
    case Mode::WAITING:
      if ((elapsed / WAITING_BLINK_MS) % 2 == 0) fill(frame, x, 6, 2);
      break;
    case Mode::INPUT_NEEDED:
      if ((elapsed / BLINK_MS) % 2 == 0) {
        fill(frame, x, 0, 5);
        fill(frame, x, 6, 2);
      }
      break;
    case Mode::FREE:
      break;
  }
}

// Idle mascot, from the logo: a screen head with a face, spikes on the back. One row per entry, bit 11 is column 0.
constexpr uint16_t DINO[ROWS] = {
  0b000001111111,
  0b000001011101,  // eyes
  0b000001111111,
  0b000001101011,  // smile corners, inset under the eyes
  0b010101110111,  // spikes, smile
  0b011111111000,
  0b111111111000,
  0b001100110000,  // legs
};
constexpr uint16_t DINO_HEAD = 0b000001111111;  // columns 5-11
constexpr uint16_t DINO_TAIL = 0b100000000000;  // column 0
constexpr uint16_t DINO_MOUTH_OPEN = 0b1110111;  // row 3 head columns: one hole instead of the smile corners
constexpr uint16_t SNORE[3] = {0b110000000000, 0b010000000000, 0b011000000000};  // z in the top left corner

void blit(uint8_t frame[ROWS][COLS], const uint16_t rows[ROWS]) {
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) frame[r][c] = (rows[r] >> (COLS - 1 - c)) & 1;
}

void drawIdleDino(uint8_t frame[ROWS][COLS], unsigned long now, unsigned long idleFor) {
  bool asleep = idleFor > DINO_SLEEP_MS;
  bool eyesShut = asleep || now % DINO_BLINK_PERIOD_MS < DINO_BLINK_MS;
  uint16_t rows[ROWS];
  for (int r = 0; r < ROWS; r++) {
    rows[r] = (r == 1 && eyesShut) ? DINO[2] : DINO[r];
    if (asleep && r < 3 && (now / SNORE_MS) % 2 == 0) rows[r] |= SNORE[r];
  }
  blit(frame, rows);
}

// A single active session gets the whole matrix. Working: the dino walks in place (legs and tail swap, head bobs) and
// its running subagents show as dots in the free top left corner. Input needed: the same, twice as fast, mouth open,
// with a blinking "!" instead of the dots.
void drawDinoSession(uint8_t frame[ROWS][COLS], const Lane& lane, unsigned long now) {
  bool asking = lane.mode == Mode::INPUT_NEEDED;
  uint16_t sprite[ROWS], rows[ROWS];
  for (int r = 0; r < ROWS; r++) sprite[r] = rows[r] = DINO[r];
  if (asking) sprite[3] = rows[3] = (DINO[3] & ~DINO_HEAD) | DINO_MOUTH_OPEN;
  if ((now / (asking ? EXCITED_STEP_MS : WALK_STEP_MS)) % 2 == 1) {
    for (int r = 0; r < 5; r++) rows[r] &= ~DINO_HEAD;  // head one row down; the body keeps its rows
    for (int r = 1; r <= 5; r++) rows[r] |= sprite[r - 1] & DINO_HEAD;
    rows[5] |= DINO_TAIL;  // tail up
    rows[6] &= ~DINO_TAIL;
    rows[7] = 0b011000011000;  // legs apart
  }
  blit(frame, rows);
  if (asking) {
    if ((now - lane.modeStart) / BLINK_MS % 2 == 0) {
      fill(frame, 1, 0, 2);
      fill(frame, 1, 3, 1);
    }
    return;
  }
  for (int dot = 0; dot < lane.agents && dot < MAX_SUBAGENT_DOTS; dot++)
    frame[1 + 2 * (dot / 2)][1 + 2 * (dot % 2)] = 1;
}

void setup() {
  matrix.begin();
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(9600);
  memset(shown, 0xFF, sizeof(shown));  // differs from any frame, so the first loop renders
  WiFi.setTimeout(0);  // begin() otherwise polls for 10 s, freezing the display and overflowing the serial buffer
  WiFi.begin(SECRET_SSID, SECRET_PASS);
}

void loop() {
  maintainWiFi(millis());
  receiveUdp();
  receiveSerial();
  unsigned long now = millis();  // read after receive() so now - modeStart cannot underflow
  uint8_t frame[ROWS][COLS] = {};
  int active = 0;
  const Lane* only = nullptr;
  bool needsInput = false;
  for (Lane& lane : lanes) {
    expire(lane, now);
    if (lane.mode != Mode::FREE) { active++; only = &lane; }
    needsInput = needsInput || lane.mode == Mode::INPUT_NEEDED;
  }
  digitalWrite(LED_BUILTIN, needsInput && (now / BLINK_MS) % 2 == 0);  // visible even when not looking at the matrix
  static unsigned long idleSince = 0;  // the last time a lane was active
  if (active > 0) idleSince = now;
  if (active == 0) {
    drawIdleDino(frame, now, now - idleSince);
  } else if (active == 1 && ((only->mode == Mode::WORKING && (long)(only->errorUntil - now) <= 0) || only->mode == Mode::INPUT_NEEDED)) {
    drawDinoSession(frame, *only, now);
  } else {
    for (int i = 0; i < LANES; i++) drawLane(frame, i, lanes[i], now);
  }
  if (memcmp(frame, shown, sizeof(frame)) != 0) {
    matrix.loadPixels(&frame[0][0], ROWS * COLS);
    memcpy(shown, frame, sizeof(frame));
    debugFrame(frame);
  }
  debugLanes(now);
}
