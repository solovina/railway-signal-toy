/*
  铁路信号灯玩具（Arduino Nano）

  状态：
  1. 等待：白灯常亮，持续 10 秒
  2. 火车通过：两颗红灯每 1 秒交替闪烁，DFPlayer 完整播放 01/001.mp3 一次，状态持续 10 秒
  3. 检修：两颗红灯常亮，声音停止，白灯熄灭

  使用前：在 Arduino IDE 的“库管理器”安装 DFRobotDFPlayerMini。
  舵机已纳入第二阶段控制。若暂时没有安装舵机，可把 ENABLE_SERVO 改为 0。
*/

#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <Servo.h>

// ---- 引脚定义（与讲义接线表一致）----
const byte RED_LED_1_PIN = 2;
const byte RED_LED_2_PIN = 3;
const byte WHITE_LED_PIN = 4;
const byte MAINTENANCE_SWITCH_PIN = 5;  // 拨到 GND 时进入检修
const byte SERVO_PIN = 6;               // SG90 黄色/橙色信号线
const byte DFPLAYER_RX_PIN = 10;        // Nano 接收 ← DFPlayer TX
const byte DFPLAYER_TX_PIN = 11;        // Nano 发送 → 1 kΩ → DFPlayer RX

// 已安装 SG90 时保持为 1；暂时不装舵机时改为 0。
#define ENABLE_SERVO 1

// 根据道闸的机械方向微调。若开、关方向相反，可交换这两个数值。
const byte SERVO_OPEN_ANGLE = 0;       // 等待/检修：道闸抬起
const byte SERVO_CLOSED_ANGLE = 90;    // 火车通过：道闸落下
const unsigned long SERVO_TRAVEL_TIME_MS = 3000UL;  // 0°↔90°约 3 秒

const unsigned long WAIT_TIME_MS = 10000UL;
const unsigned long TRAIN_TIME_MS = 10000UL;
const unsigned long FLASH_INTERVAL_MS = 1000UL;

enum SignalState { WAITING, TRAIN_PASSING, MAINTENANCE };
SignalState state = WAITING;

SoftwareSerial dfSerial(DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
DFRobotDFPlayerMini player;
bool playerReady = false;
bool soundStarted = false;
unsigned long stateStartedAt = 0;
unsigned long lastFlashAt = 0;
bool red1IsOn = true;

#if ENABLE_SERVO
Servo gateServo;
int servoCurrentAngle = SERVO_OPEN_ANGLE;
int servoStartAngle = SERVO_OPEN_ANGLE;
int servoTargetAngle = SERVO_OPEN_ANGLE;
unsigned long servoMoveStartedAt = 0;
#endif

void setLights(bool red1, bool red2, bool white) {
  digitalWrite(RED_LED_1_PIN, red1 ? HIGH : LOW);
  digitalWrite(RED_LED_2_PIN, red2 ? HIGH : LOW);
  digitalWrite(WHITE_LED_PIN, white ? HIGH : LOW);
}

void requestGateAngle(int targetAngle) {
#if ENABLE_SERVO
  targetAngle = constrain(targetAngle, 0, 180);
  // 从当前已发送的角度重新起算（没有实际位置反馈），途中可切换方向。
  servoStartAngle = servoCurrentAngle;
  servoTargetAngle = targetAngle;
  servoMoveStartedAt = millis();
#endif
}

void openGate() {
  requestGateAngle(SERVO_OPEN_ANGLE);
}

void closeGate() {
  requestGateAngle(SERVO_CLOSED_ANGLE);
}

void serviceGate() {
#if ENABLE_SERVO
  if (servoCurrentAngle == servoTargetAngle) return;

  const unsigned long elapsed = millis() - servoMoveStartedAt;
  int nextAngle;
  if (elapsed >= SERVO_TRAVEL_TIME_MS) {
    nextAngle = servoTargetAngle;
  } else {
    const long delta = (long)servoTargetAngle - servoStartAngle;
    nextAngle = servoStartAngle + (int)(delta * (long)elapsed / (long)SERVO_TRAVEL_TIME_MS);
  }
  if (nextAngle != servoCurrentAngle) {
    servoCurrentAngle = nextAngle;
    gateServo.write(servoCurrentAngle);
  }
#endif
}

void stopSound() {
  // 只在声音确实仍在播放时发送 stop()。
  // 对已经自然结束的曲目重复发送 stop()，会使部分兼容模块下一轮异常。
  if (playerReady && soundStarted) player.stop();
  soundStarted = false;
}

void servicePlayer() {
  if (!playerReady) return;

  // 必须及时读取 DFPlayer 的异步事件，避免上一轮的 PlayFinished
  // 留在串口缓冲区中影响下一轮命令。
  while (player.available()) {
    const uint8_t eventType = player.readType();
    player.read();

    if (eventType == DFPlayerPlayFinished) {
      soundStarted = false;
    }
  }
}

void startWarningSound() {
  if (playerReady && !soundStarted) {
    // 当前声音约 8 秒，而火车通过状态持续 10 秒。
    // 不能使用 loopFolder(1)：第二轮刚播放约 2 秒就会被状态结束时的 stop() 截断。
    // 单次播放可确保 001.mp3 自然、完整地结束。
    player.playFolder(1, 1);
    soundStarted = true;
  }
}

void enterWaiting() {
  state = WAITING;
  stateStartedAt = millis();
  stopSound();
  openGate();
  setLights(false, false, true);
}

void enterTrainPassing() {
  state = TRAIN_PASSING;
  stateStartedAt = millis();
  lastFlashAt = millis();
  red1IsOn = true;
  setLights(true, false, false);
  closeGate();
  startWarningSound();
}

void enterMaintenance() {
  state = MAINTENANCE;
  stopSound();
  openGate();
  setLights(true, true, false);
}

void setup() {
  pinMode(RED_LED_1_PIN, OUTPUT);
  pinMode(RED_LED_2_PIN, OUTPUT);
  pinMode(WHITE_LED_PIN, OUTPUT);
  pinMode(MAINTENANCE_SWITCH_PIN, INPUT_PULLUP);

#if ENABLE_SERVO
  gateServo.attach(SERVO_PIN);
  // 上电时无法读到 SG90 的真实角度，先定位到打开位置。
  // 正常运行中的开、关动作由 serviceGate() 缓慢完成。
  gateServo.write(SERVO_OPEN_ANGLE);
#endif

  // 上电后先保持安全、安静的等待状态。
  setLights(false, false, false);
  openGate();

  dfSerial.begin(9600);
  if (player.begin(dfSerial, true, true)) {
    playerReady = true;
    // 初始化阶段确认模块真实在线；随后切换为无 ACK 兼容模式，
    // 与已经连续通过三轮播放的诊断固件保持一致。
    player.begin(dfSerial, false, false);
    player.outputDevice(DFPLAYER_DEVICE_SD);
    player.volume(30);  // 音量范围为 0～30
    player.enableDAC();
  }

  enterWaiting();
}

void loop() {
  servicePlayer();
  serviceGate();

  const bool maintenanceSelected = (digitalRead(MAINTENANCE_SWITCH_PIN) == LOW);
  const unsigned long now = millis();

  // 检修开关优先级最高；退出检修后重新从等待的 10 秒开始计时。
  if (maintenanceSelected) {
    if (state != MAINTENANCE) enterMaintenance();
    return;
  }
  if (state == MAINTENANCE) {
    enterWaiting(); 
    return;
  }

  if (state == WAITING) {
    setLights(false, false, true);
    if (now - stateStartedAt >= WAIT_TIME_MS) enterTrainPassing();
    return;
  }

  // 火车通过：红灯每秒交替一次，不用 delay()，检修开关可以随时响应。
  if (now - lastFlashAt >= FLASH_INTERVAL_MS) {
    lastFlashAt = now;
    red1IsOn = !red1IsOn;
    setLights(red1IsOn, !red1IsOn, false);
  }

  if (now - stateStartedAt >= TRAIN_TIME_MS) enterWaiting();
}

