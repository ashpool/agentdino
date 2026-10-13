// UDP datagrams on port 4210 or newline-terminated USB serial lines: "<4-char session key>:<command bytes>". Each session gets a 2-column lane:
// 'W' working (bouncing block), 'S' / 's' + 4-char agent id: subagent started / stopped (a dot each, bottom rows),
// 'D' done (solid lane for 3 s, then freed), 'I' input needed (blinking "!"), 'O' session gone (lane freed).
#include <Arduino_LED_Matrix.h>
#include <WiFiS3.h>
#include "arduino_secrets.h"  // defines SECRET_SSID and SECRET_PASS

constexpr int ROWS = 8, COLS = 12, LANE_WIDTH = 2, LANES = COLS / LANE_WIDTH, KEY_LENGTH = 4;
constexpr int MAX_SUBAGENT_DOTS = 4, MAX_AGENTS = 8;
constexpr unsigned long STEP_MS = 120, BLINK_MS = 400, DONE_MS = 3000;
constexpr unsigned long WORK_TIMEOUT_MS = 600000;   // no hook heartbeat for this long => assume interrupted
constexpr unsigned long INPUT_TIMEOUT_MS = 600000;  // "!" left unanswered, e.g. Esc at a permission prompt
constexpr unsigned long WIFI_RETRY_MS = 5000, WIFI_CHECK_MS = 1000;
constexpr unsigned int PORT = 4210;
constexpr int PACKET_MAX = 32;

enum class Mode { FREE, WORKING, DONE, INPUT_NEEDED };

struct Lane {
  char key[KEY_LENGTH];
  Mode mode;
  unsigned long modeStart;
  unsigned long lastCommand;
  char agentIds[MAX_AGENTS][KEY_LENGTH];  // running subagents; the first `agents` entries are valid
  int agents;
};

ArduinoLEDMatrix matrix;
WiFiUDP udp;
Lane lanes[LANES] = {};
uint8_t shown[ROWS][COLS];

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
  if (mode == Mode::DONE || mode == Mode::FREE) lane.agents = 0;
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
    case 'O': if (lane) setMode(*lane, Mode::FREE); break;
  }
}

void dispatch(const char* packet, int length) {
  if (length <= KEY_LENGTH || packet[KEY_LENGTH] != ':') return;
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
      if (!tooLong) dispatch(line, length);
      length = 0;
      tooLong = false;
    } else if (c != '\r') {
      if (length < PACKET_MAX) line[length++] = c;
      else tooLong = true;
    }
  }
}

// Frees lanes whose DONE display is over or whose session went silent. The key is kept so the
// session gets the same lane back.
void expire(Lane& lane, unsigned long now) {
  bool doneShown = lane.mode == Mode::DONE && now - lane.modeStart >= DONE_MS;
  // Subagent tool calls send no heartbeat, so a lane with running subagents is never considered stalled.
  bool workStalled = lane.mode == Mode::WORKING && lane.agents == 0 && now - lane.lastCommand > WORK_TIMEOUT_MS;
  bool inputStale = lane.mode == Mode::INPUT_NEEDED && now - lane.lastCommand > INPUT_TIMEOUT_MS;
  if (doneShown || workStalled || inputStale) setMode(lane, Mode::FREE);
}

void fill(uint8_t frame[ROWS][COLS], int x, int row, int rows) {
  for (int r = row; r < row + rows; r++)
    for (int c = x; c < x + LANE_WIDTH; c++) frame[r][c] = 1;
}

void drawLane(uint8_t frame[ROWS][COLS], int index, const Lane& lane, unsigned long now) {
  int x = index * LANE_WIDTH;
  unsigned long elapsed = now - lane.modeStart;
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

void setup() {
  matrix.begin();
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
  for (int i = 0; i < LANES; i++) {
    expire(lanes[i], now);
    drawLane(frame, i, lanes[i], now);
  }
  if (memcmp(frame, shown, sizeof(frame)) != 0) {
    matrix.loadPixels(&frame[0][0], ROWS * COLS);
    memcpy(shown, frame, sizeof(frame));
  }
}
