// UDP datagrams on port 4210 or newline-terminated USB serial lines: "<4-char session key>:<command bytes>".
// 'W' working, 'I' input needed, 'D' done (the session is idle; the dino dances for a moment), 'E' tool failed
// (flash), 'S' / 's' + 4-char agent id: subagent started / stopped, 'O' session gone.
// One busy session (working, asking, dancing) gets the whole matrix as the dino; with none the dino stands idle.
// With several busy sessions each gets a 2-column lane (bouncing block, blinking "!", dim dot when idle).
// The on-board LED blinks while any session needs input. The matrix goes dark after an hour without activity.
#include <Arduino_LED_Matrix.h>
#include <WiFiS3.h>
#include "arduino_secrets.h"  // defines SECRET_SSID and SECRET_PASS

constexpr int ROWS = 8, COLS = 12, LANE_WIDTH = 2, LANES = COLS / LANE_WIDTH, KEY_LENGTH = 4;
constexpr int MAX_SUBAGENT_DOTS = 4, MAX_AGENTS = 8;
constexpr unsigned long STEP_MS = 120, BLINK_MS = 400, HOP_MS = 2000;
constexpr unsigned long ERROR_MS = 800, ERROR_BLINK_MS = 100;
constexpr unsigned long DINO_BLINK_PERIOD_MS = 4000, DINO_BLINK_MS = 150, DINO_SLEEP_MS = 600000, DISPLAY_OFF_MS = 3600000, STRAY_PIXELS = 8, IDLE_PERIOD_MS = 20000, IDLE_ACT_AT_MS = 5000, SHAKE_MS = 60, SWEAT_MS = 150, SNORE_MS = 1000, WALK_STEP_MS = 250, EXCITED_STEP_MS = 120;
constexpr unsigned long WORK_TIMEOUT_MS = 600000;   // no hook heartbeat for this long => assume interrupted
constexpr unsigned long IDLE_TIMEOUT_MS = 3600000;  // finished lane nobody came back to, or a lost "O"
constexpr unsigned long INPUT_TIMEOUT_MS = 600000;  // "!" left unanswered, e.g. Esc at a permission prompt
constexpr unsigned long WIFI_RETRY_MS = 5000, WIFI_CHECK_MS = 1000;
constexpr unsigned long DEBUG_LEASE_MS = 5000, DEBUG_LANES_MS = 1000, DEBUG_FRAME_MS = 100;
constexpr unsigned int PORT = 4210;
constexpr int PACKET_MAX = 32;

enum class Mode { FREE, WORKING, IDLE, INPUT_NEEDED };  // IDLE: finished, waiting for the next prompt

struct Lane {
  char key[KEY_LENGTH];
  Mode mode;
  unsigned long modeStart;
  unsigned long lastCommand;
  unsigned long errorUntil;  // 0 = no flash running
  unsigned long hopUntil;    // 0 = no dance running
  char agentIds[MAX_AGENTS][KEY_LENGTH];  // running subagents; the first `agents` entries are valid
  int agents;
};

ArduinoLEDMatrix matrix;
WiFiUDP udp;
Lane lanes[LANES] = {};
uint8_t shown[ROWS][COLS];
unsigned long activityAt = 0;  // the last command received or the last time a session was busy
bool debugAsked = false;  // a host sent "?" ...
unsigned long debugAskedAt = 0;  // ... at this time, and wants the debug lines below
uint8_t debugShown[ROWS][COLS];  // last frame sent to the host

bool leaseActive() { return debugAsked && millis() - debugAskedAt < DEBUG_LEASE_MS; }

// Serial is true only while a host has the port open; availableForWrite() is always 0 on this core.
bool debugging() { return leaseActive() && Serial; }

// A new host starts with the current frame, even if it has not changed for a while.
void debugAsk() {
  if (!leaseActive()) memset(debugShown, 0xFF, sizeof(debugShown));
  debugAsked = true;
  debugAskedAt = millis();
}

// Packets come from anyone on the network: keep the debug lines one line of printable characters.
char printable(char c) { return c > ' ' && c <= '~' ? c : '?'; }

char modeCode(Mode mode) {
  switch (mode) {
    case Mode::WORKING: return 'W';
    case Mode::IDLE: return 'P';
    case Mode::INPUT_NEEDED: return 'I';
    default: return '-';
  }
}

// "rx <packet>": every command packet received, from UDP or serial.
void debugPacket(const char* packet, int length) {
  if (!debugging()) return;
  Serial.print("rx ");
  for (int i = 0; i < length; i++) Serial.print(printable(packet[i]));
  Serial.println();
}

// "F <8 rows as 3 hex digits>": the frame on the matrix, bit 11 is column 0. At most one per DEBUG_FRAME_MS; a change
// that has to wait is sent on a later pass, because debugShown only follows what was sent.
void debugFrame(const uint8_t frame[ROWS][COLS], unsigned long now) {
  static unsigned long last = 0;
  if (!debugging() || memcmp(frame, debugShown, sizeof(debugShown)) == 0 || now - last < DEBUG_FRAME_MS) return;
  last = now;
  memcpy(debugShown, frame, sizeof(debugShown));
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
    for (char c : lane.key) Serial.print(printable(c));
    Serial.print(':');
    Serial.print(modeCode(lane.mode));
    Serial.print(':');
    Serial.print(lane.agents);
    Serial.print(':');
    Serial.print((now - lane.lastCommand) / 1000);
  }
  Serial.println();
}

// errorUntil and hopUntil are 0 when not running; a plain signed compare against 0 would break after 24.8 days of uptime.
bool timerRunning(unsigned long until, unsigned long now) { return until != 0 && (long)(until - now) > 0; }
bool flashing(const Lane& lane, unsigned long now) { return timerRunning(lane.errorUntil, now); }
bool dancing(const Lane& lane, unsigned long now) { return lane.mode == Mode::IDLE && timerRunning(lane.hopUntil, now); }
bool busy(const Lane& lane, unsigned long now) {
  return lane.mode == Mode::WORKING || lane.mode == Mode::INPUT_NEEDED || dancing(lane, now);
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

// `now` must not be later than the loop's `now`, or elapsed times underflow.
void setMode(Lane& lane, Mode mode, unsigned long now) {
  if (mode == Mode::IDLE || mode == Mode::FREE) lane.agents = 0;
  if (mode == lane.mode) return;
  lane.mode = mode;
  lane.modeStart = now;
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
    case 'W': setMode(touch(lane, key), Mode::WORKING, millis()); break;
    case 'D': { Lane& l = touch(lane, key); setMode(l, Mode::IDLE, millis()); l.hopUntil = millis() + HOP_MS; break; }
    case 'I': setMode(touch(lane, key), Mode::INPUT_NEEDED, millis()); break;
    // A late start on an idle lane would leave a phantom agent that pins the lane in WORKING for good.
    case 'S': if (lane && lane->mode != Mode::IDLE) { addAgent(*lane, agentId); lane->lastCommand = millis(); } break;
    case 's': if (lane && lane->mode != Mode::IDLE) { removeAgent(*lane, agentId); lane->lastCommand = millis(); } break;
    case 'E': if (lane && busy(*lane, millis())) { lane->errorUntil = millis() + ERROR_MS; lane->lastCommand = millis(); } break;
    case 'O': if (lane) setMode(*lane, Mode::FREE, millis()); break;
  }
}

void dispatch(const char* packet, int length) {
  if (length <= KEY_LENGTH || packet[KEY_LENGTH] != ':') return;
  debugPacket(packet, length);
  activityAt = millis();
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
      if (length == 1 && line[0] == '?') debugAsk();
      else if (!tooLong) dispatch(line, length);
      length = 0;
      tooLong = false;
    } else if (c != '\r') {
      if (length < PACKET_MAX) line[length++] = c;
      else tooLong = true;
    }
  }
}

// Frees lanes whose session went silent. The key is kept so the session gets the same lane back.
void expire(Lane& lane, unsigned long now) {
  if (lane.errorUntil != 0 && !flashing(lane, now)) lane.errorUntil = 0;
  if (lane.hopUntil != 0 && !timerRunning(lane.hopUntil, now)) lane.hopUntil = 0;
  // Subagent tool calls send no heartbeat, so a lane with running subagents is never considered stalled.
  bool workStalled = lane.mode == Mode::WORKING && lane.agents == 0 && now - lane.lastCommand > WORK_TIMEOUT_MS;
  bool inputStale = lane.mode == Mode::INPUT_NEEDED && now - lane.lastCommand > INPUT_TIMEOUT_MS;
  bool idleStale = lane.mode == Mode::IDLE && now - lane.lastCommand > IDLE_TIMEOUT_MS;
  if (workStalled || inputStale || idleStale) setMode(lane, Mode::FREE, now);
}

void fill(uint8_t frame[ROWS][COLS], int x, int row, int rows) {
  for (int r = row; r < row + rows; r++)
    for (int c = x; c < x + LANE_WIDTH; c++) frame[r][c] = 1;
}

void drawLane(uint8_t frame[ROWS][COLS], int index, const Lane& lane, unsigned long now) {
  int x = index * LANE_WIDTH;
  unsigned long elapsed = now - lane.modeStart;
  if (lane.mode != Mode::FREE && flashing(lane, now)) {
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
    case Mode::IDLE:
      if (dancing(lane, now)) fill(frame, x, 0, ROWS);
      else fill(frame, x, ROWS - 1, 1);
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
constexpr uint16_t DINO_EYES = 0b000000100010;  // the eye holes of rows 1 and 2
constexpr uint16_t DINO_YAWN = 0b000000011100;  // the open mouth of rows 3 and 4
constexpr uint16_t SNORE[3] = {0b110000000000, 0b010000000000, 0b011000000000};  // z in the top left corner

void blit(uint8_t frame[ROWS][COLS], const uint16_t rows[ROWS]) {
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) frame[r][c] = (rows[r] >> (COLS - 1 - c)) & 1;
}

// The second frame of the walk: head one row down, tail up, legs apart. `rows` starts as a copy of `sprite`.
void walkPose(uint16_t rows[ROWS], const uint16_t sprite[ROWS], bool stepB) {
  if (!stepB) return;
  for (int r = 0; r < 5; r++) rows[r] &= ~DINO_HEAD;  // the body keeps its rows
  for (int r = 1; r <= 5; r++) rows[r] |= sprite[r - 1] & DINO_HEAD;
  rows[5] |= DINO_TAIL;
  rows[6] &= ~DINO_TAIL;
  rows[7] = 0b011000011000;
}

// Moves the picture right (dx > 0) or left; whatever leaves the matrix is gone.
void shiftRows(uint16_t rows[ROWS], int dx) {
  for (int r = 0; r < ROWS; r++) rows[r] = dx >= 0 ? rows[r] >> dx : (rows[r] << -dx) & 0xFFF;
}

int litPixels(const uint16_t rows[ROWS]) {
  int lit = 0;
  for (int r = 0; r < ROWS; r++) lit += __builtin_popcount(rows[r]);
  return lit;
}

// Dots on the feet row, moving left a step at a time: the dino is going somewhere. Pixels next to a foot stay dark so
// the dots do not merge with the legs.
void addGround(uint16_t rows[ROWS], unsigned long step) {
  uint16_t feet = rows[ROWS - 1];
  for (int c = 0; c < COLS; c++) {
    uint16_t pixel = 1 << (COLS - 1 - c);
    if ((c + step) % 3 == 0 && !(feet & ((pixel << 1) | pixel | (pixel >> 1)))) rows[ROWS - 1] |= pixel;
  }
}

// Standing, with a blink every few seconds. Once per IDLE_PERIOD_MS, after a pause, it does one of four things, never
// the same twice in a row: look around, wag the tail, yawn, or stroll off the right
// edge and back in from the left. Asleep (after DINO_SLEEP_MS) it only breathes out a z.
void drawIdleDino(uint8_t frame[ROWS][COLS], unsigned long now, unsigned long idleFor) {
  bool asleep = idleFor > DINO_SLEEP_MS;
  unsigned long phase = idleFor % IDLE_PERIOD_MS;  // counted from when it became idle: it always starts out standing
  bool acting = !asleep && phase >= IDLE_ACT_AT_MS;
  unsigned long t = acting ? phase - IDLE_ACT_AT_MS : 0;
  static unsigned long anchor = 0, period = ~0UL;
  static int variant = 0;
  if (anchor != activityAt || idleFor / IDLE_PERIOD_MS != period) {  // a new period: a pseudo-random pick that differs from the last one
    anchor = activityAt;
    period = idleFor / IDLE_PERIOD_MS;
    uint32_t mix = (activityAt + period) * 2654435761u;
    mix ^= mix >> 15;
    mix *= 2246822519u;
    mix ^= mix >> 13;
    int pick = (int)(mix % 4);
    variant = pick == variant ? (pick + 1) % 4 : pick;
  }
  uint16_t rows[ROWS];
  for (int r = 0; r < ROWS; r++) rows[r] = DINO[r];
  if (asleep || now % DINO_BLINK_PERIOD_MS < DINO_BLINK_MS) rows[1] = DINO[2];  // eyes shut
  if (asleep) {
    for (int r = 0; r < 3; r++)
      if ((now / SNORE_MS) % 2 == 0) rows[r] |= SNORE[r];
  } else if (acting && variant == 0 && t < 2400) {  // look left, ahead, right, ahead
    static const int look[4] = {-1, 0, 1, 0};
    int off = look[(t / 600) % 4];
    rows[1] = DINO_HEAD & ~((1 << (5 - off)) | (1 << (1 - off)));  // the eye holes move across the face
  } else if (acting && variant == 1 && t < 2400) {  // wag
    if ((t / 150) % 2 == 1) {
      rows[5] |= DINO_TAIL;
      rows[6] &= ~DINO_TAIL;
    }
  } else if (acting && variant == 2 && t < 1800) {  // yawn
    rows[1] = DINO[2];
    rows[3] &= ~DINO_YAWN;
    rows[4] &= ~DINO_YAWN;
  } else if (acting && variant == 3 && t < 6000) {  // stroll: 12 steps off the right edge, 12 in from the left
    int step = (int)(t / WALK_STEP_MS);
    walkPose(rows, DINO, step % 2 == 1);
    shiftRows(rows, step < COLS ? step : step - 2 * COLS);
    if (litPixels(rows) < STRAY_PIXELS)  // a lone bit of tail or head at the edge looks like a glitch: show nothing
      for (int r = 0; r < ROWS; r++) rows[r] = 0;
  }
  blit(frame, rows);
}

// The one busy session gets the whole matrix. Working: the dino walks in place (legs and tail swap, head bobs, ground
// scrolls underfoot) and its running subagents show as dots in the free top left corner. Input needed: the same, twice
// as fast, mouth open. Just finished: the fast steps with the normal smile, a short dance. A failed tool startles it:
// tall eyes, a shake and a falling sweat drop.
void drawDinoSession(uint8_t frame[ROWS][COLS], const Lane& lane, unsigned long now) {
  bool asking = lane.mode == Mode::INPUT_NEEDED, lively = asking || lane.mode == Mode::IDLE;
  bool startled = flashing(lane, now);
  uint16_t sprite[ROWS], rows[ROWS];
  for (int r = 0; r < ROWS; r++) sprite[r] = rows[r] = DINO[r];
  if (asking) sprite[3] = rows[3] = (DINO[3] & ~DINO_HEAD) | DINO_MOUTH_OPEN;
  bool stepB = (now / (lively ? EXCITED_STEP_MS : WALK_STEP_MS)) % 2 == 1;
  walkPose(rows, sprite, stepB);
  if (lane.mode == Mode::WORKING) addGround(rows, now / WALK_STEP_MS);
  if (startled) {
    rows[stepB ? 3 : 2] &= ~DINO_EYES;  // the eyes are two rows tall; the head is a row lower in the second step
    shiftRows(rows, (now / SHAKE_MS) % 2 == 1 ? 1 : -1);
  }
  blit(frame, rows);
  if (startled) {
    frame[1 + (now / SWEAT_MS) % 3][3] = 1;  // a sweat drop falls beside the head
    return;
  }
  if (lane.mode != Mode::WORKING) return;
  for (int dot = 0; dot < lane.agents && dot < MAX_SUBAGENT_DOTS; dot++)
    frame[1 + 2 * (dot / 2)][1 + 2 * (dot % 2)] = 1;
}

void setup() {
  matrix.begin();
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(9600);
  memset(shown, 0xFF, sizeof(shown));  // differs from any frame, so the first loop renders
  memset(debugShown, 0xFF, sizeof(debugShown));
  WiFi.setTimeout(0);  // begin() otherwise polls for 10 s, freezing the display and overflowing the serial buffer
  WiFi.begin(SECRET_SSID, SECRET_PASS);
}

void loop() {
  maintainWiFi(millis());
  receiveUdp();
  receiveSerial();
  unsigned long now = millis();  // read after receive() so now - modeStart cannot underflow
  uint8_t frame[ROWS][COLS] = {};
  int busyLanes = 0;
  const Lane* only = nullptr;
  bool needsInput = false;
  for (Lane& lane : lanes) {
    expire(lane, now);
    if (busy(lane, now)) { busyLanes++; only = &lane; }
    needsInput = needsInput || lane.mode == Mode::INPUT_NEEDED;
  }
  digitalWrite(LED_BUILTIN, needsInput && (now / BLINK_MS) % 2 == 0);  // visible even when not looking at the matrix
  if (busyLanes > 0) activityAt = now;
  if (busyLanes == 0) {
    if (now - activityAt <= DISPLAY_OFF_MS) drawIdleDino(frame, now, now - activityAt);  // dark after a long idle
  } else if (busyLanes == 1) {
    drawDinoSession(frame, *only, now);
  } else {
    for (int i = 0; i < LANES; i++) drawLane(frame, i, lanes[i], now);
  }
  if (memcmp(frame, shown, sizeof(frame)) != 0) {
    matrix.loadPixels(&frame[0][0], ROWS * COLS);
    memcpy(shown, frame, sizeof(frame));
  }
  debugFrame(frame, now);
  debugLanes(now);
}
