#include <Arduino.h>
#include <Wire.h>
#include <FS.h>
#include <SPIFFS.h>
#include <driver/i2s.h>
#include <Seeed_Arduino_SSCMA.h>

// ============================================================
// 硬體設定
// ============================================================
#define I2S_LRC       1   // XIAO D0 -> MAX98357A LRC
#define I2S_BCLK      2   // XIAO D1 -> MAX98357A BCLK
#define I2S_DOUT      3   // XIAO D2 -> MAX98357A DIN
#define BAT_ADC_PIN   D3

// 必須和所有 WAV 音檔一致
#define AUDIO_SAMPLE_RATE 8000

#define CAMERA_ADDRESS  0x62
#define MIN_SCORE       25

SSCMA AI;

// ============================================================
// 系統狀態
// ============================================================
enum SystemState {
    SYS_INIT,
    VISION_LOOP,
    VOICE_PLAYBACK,
    BATTERY_CHECK
};

enum TrafficLabel {
    LIGHT_NULL  = -1,
    LIGHT_GREEN = 0,
    LIGHT_RED   = 1
};

enum AudioPrompt {
    PROMPT_NONE,
    PROMPT_GREEN,
    PROMPT_RED,
    PROMPT_BAT_MID,
    PROMPT_BAT_LOW
};

SystemState currentState = SYS_INIT;
TrafficLabel currentLabel = LIGHT_NULL;
TrafficLabel candidateLabel = LIGHT_NULL;
AudioPrompt pendingPrompt = PROMPT_NONE;

bool spiffsReady = false;
bool audioReady = false;
bool cameraReady = false;

int debounceCount = 0;
const int TH_DEBOUNCE = 5;

unsigned long lastBatCheck = 0;
const unsigned long BAT_INTERVAL = 900000;

unsigned long lastVoiceTime = 0;
const unsigned long VOICE_REPEAT_INTERVAL = 5000;

// ============================================================
// I2C 掃描
// ============================================================
bool scanI2C() {
    int count = 0;
    bool cameraFound = false;

    Serial.println("[I2C] 開始掃描...");

    for (uint8_t address = 1; address < 127; address++) {
        Wire.beginTransmission(address);
        uint8_t error = Wire.endTransmission();

        if (error == 0) {
            Serial.printf("[I2C] 發現裝置: 0x%02X\n", address);
            count++;

            if (address == CAMERA_ADDRESS) {
                cameraFound = true;
            }
        }
    }

    Serial.printf("[I2C] 共發現 %d 個裝置\n", count);

    if (!cameraFound) {
        Serial.println("[I2C] 警告：沒有找到 Grove 0x62");
    }

    return cameraFound;
}

// ============================================================
// I2S 初始化
// ============================================================
bool initI2S() {
    i2s_config_t config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };

    i2s_pin_config_t pins = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRC,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    esp_err_t error =
        i2s_driver_install(I2S_NUM_0, &config, 0, nullptr);

    Serial.printf(
        "[I2S] driver install: %s (%d)\n",
        esp_err_to_name(error),
        error
    );

    if (error != ESP_OK) {
        return false;
    }

    error = i2s_set_pin(I2S_NUM_0, &pins);

    Serial.printf(
        "[I2S] set pin: %s (%d)\n",
        esp_err_to_name(error),
        error
    );

    if (error != ESP_OK) {
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);

    Serial.println("[I2S] 初始化成功");
    return true;
}

// ============================================================
// WAV 播放
// ============================================================
bool playWav(const char *filename) {
    if (!audioReady) {
        Serial.println("[VOICE] I2S 尚未初始化，取消播放");
        return false;
    }

    if (!spiffsReady) {
        Serial.println("[VOICE] SPIFFS 尚未掛載，取消播放");
        return false;
    }

    if (!SPIFFS.exists(filename)) {
        Serial.printf("[VOICE] 找不到檔案: %s\n", filename);
        return false;
    }

    File file = SPIFFS.open(filename, "r");

    if (!file) {
        Serial.printf("[VOICE] 無法開啟檔案: %s\n", filename);
        return false;
    }

    if (file.size() <= 44) {
        Serial.printf("[VOICE] WAV 格式錯誤: %s\n", filename);
        file.close();
        return false;
    }

    Serial.printf(
        "[VOICE] 開始播放: %s，大小: %u bytes\n",
        filename,
        (unsigned int)file.size()
    );

    // 標準 PCM WAV header
    file.seek(44);

    uint8_t buffer[512];
    size_t bytesWritten = 0;
    bool success = true;

    while (file.available()) {
        int bytesRead = file.read(buffer, sizeof(buffer));

        if (bytesRead <= 0) {
            break;
        }

        esp_err_t error = i2s_write(
            I2S_NUM_0,
            buffer,
            bytesRead,
            &bytesWritten,
            portMAX_DELAY
        );

        if (error != ESP_OK ||
            bytesWritten != (size_t)bytesRead) {

            Serial.printf(
                "[VOICE] I2S 寫入失敗: %s，讀取=%d，寫入=%u\n",
                esp_err_to_name(error),
                bytesRead,
                (unsigned int)bytesWritten
            );

            success = false;
            break;
        }
    }

    file.close();

    // 送出靜音，清除尾音
    uint8_t zeroBuffer[512] = {0};

    for (int i = 0; i < 8; i++) {
        esp_err_t error = i2s_write(
            I2S_NUM_0,
            zeroBuffer,
            sizeof(zeroBuffer),
            &bytesWritten,
            portMAX_DELAY
        );

        if (error != ESP_OK) {
            Serial.printf(
                "[VOICE] 靜音資料寫入失敗: %s\n",
                esp_err_to_name(error)
            );

            success = false;
            break;
        }
    }

    i2s_zero_dma_buffer(I2S_NUM_0);

    Serial.printf(
        "[VOICE] 播放結果: %s\n",
        success ? "成功" : "失敗"
    );

    return success;
}

// ============================================================
// 視覺辨識
//
// 模型標籤：
// 0 = 綠燈
// 1 = 紅燈
// 2 = 黃燈，當成紅燈
// ============================================================
TrafficLabel getVisionResultFromI2C() {
    if (!cameraReady) {
        return LIGHT_NULL;
    }

    int result = AI.invoke();

    if (result != 0) {
        static unsigned long lastErrorTime = 0;

        if (millis() - lastErrorTime >= 3000) {
            Serial.printf(
                "[CAMERA] 推論失敗，錯誤碼: %d\n",
                result
            );

            lastErrorTime = millis();
        }

        return LIGHT_NULL;
    }

    const auto &boxes = AI.boxes();

    int bestGreenScore = 0;
    int bestRedLikeScore = 0;

    for (size_t i = 0; i < boxes.size(); i++) {
        int target = boxes[i].target;
        int score = boxes[i].score;

        // 0 = 綠燈
        if (target == 0 && score > bestGreenScore) {
            bestGreenScore = score;
        }

        // 1 = 紅燈，2 = 黃燈
        if ((target == 1 || target == 2) &&
            score > bestRedLikeScore) {
            bestRedLikeScore = score;
        }
    }

    // 安全優先：紅燈或黃燈優先
    if (bestRedLikeScore >= MIN_SCORE) {
        return LIGHT_RED;
    }

    if (bestGreenScore >= MIN_SCORE) {
        return LIGHT_GREEN;
    }

    return LIGHT_NULL;
}

// ============================================================
// 視覺去抖動
// ============================================================
void updateVisionPerception() {
    TrafficLabel detectedLabel =
        getVisionResultFromI2C();

    static unsigned long lastPrintTime = 0;

    if (millis() - lastPrintTime >= 1000) {
        if (!cameraReady) {
            Serial.println("[VISION] 相機尚未連線");
        } else if (detectedLabel == LIGHT_GREEN) {
            Serial.println("[VISION] 0 綠燈通行");
        } else if (detectedLabel == LIGHT_RED) {
            Serial.println("[VISION] 1/2 紅黃燈停止");
        } else {
            Serial.println("[VISION] 未偵測到有效號誌");
        }

        lastPrintTime = millis();
    }

    if (detectedLabel == LIGHT_NULL) {
        candidateLabel = LIGHT_NULL;
        debounceCount = 0;
        return;
    }

    // 新的候選燈號
    if (detectedLabel != currentLabel) {
        if (detectedLabel != candidateLabel) {
            candidateLabel = detectedLabel;
            debounceCount = 1;
        } else {
            debounceCount++;
        }

        if (debounceCount >= TH_DEBOUNCE) {
            currentLabel = candidateLabel;
            candidateLabel = LIGHT_NULL;
            debounceCount = 0;

            pendingPrompt =
                (currentLabel == LIGHT_GREEN)
                    ? PROMPT_GREEN
                    : PROMPT_RED;

            currentState = VOICE_PLAYBACK;
        }
    } else {
        candidateLabel = LIGHT_NULL;
        debounceCount = 0;

        if (millis() - lastVoiceTime >=
            VOICE_REPEAT_INTERVAL) {

            pendingPrompt =
                (currentLabel == LIGHT_GREEN)
                    ? PROMPT_GREEN
                    : PROMPT_RED;

            currentState = VOICE_PLAYBACK;
        }
    }
}

// ============================================================
// 電池電壓
// ============================================================
float getBatteryVoltage() {
    uint32_t sum = 0;

    for (int i = 0; i < 10; i++) {
        sum += analogRead(BAT_ADC_PIN);
        delayMicroseconds(50);
    }

    float average = sum / 10.0f;

    // 假設電池分壓比例為 1:1
    return (average / 4095.0f) * 3.3f * 2.0f;
}

void checkBatteryRoutine() {
    float voltage = getBatteryVoltage();

    Serial.printf(
        "[BATTERY] 電池電壓: %.2f V\n",
        voltage
    );

    if (voltage < 3.50f) {
        pendingPrompt = PROMPT_BAT_LOW;
        currentState = VOICE_PLAYBACK;
    } else if (voltage < 3.70f) {
        pendingPrompt = PROMPT_BAT_MID;
        currentState = VOICE_PLAYBACK;
    } else {
        currentState = VISION_LOOP;
    }
}

// ============================================================
// 初始化
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("====================================");
    Serial.println("智慧導盲眼鏡 - 穩定修正版");
    Serial.println("====================================");

    Wire.begin(D4, D5);
    Wire.setClock(100000);

    bool addressFound = scanI2C();

    analogReadResolution(12);

    // 相機只初始化一次
    if (addressFound) {
        Serial.println("[CAMERA] 開始初始化...");

        cameraReady = AI.begin(
            &Wire,
            -1,
            CAMERA_ADDRESS,
            2,
            100000
        );
    }

    if (cameraReady) {
        Serial.println("[CAMERA] 初始化成功");
    } else {
        Serial.println("[CAMERA] 初始化失敗");
    }

    // 掛載 SPIFFS
    spiffsReady = SPIFFS.begin(true);

    if (spiffsReady) {
        Serial.println("[SPIFFS] 掛載成功");
    } else {
        Serial.println("[SPIFFS] 掛載失敗");
    }

    // 初始化 I2S
    audioReady = initI2S();

    if (!audioReady) {
        Serial.println("[I2S] 音訊功能停用");
    }

    // 開機電量播報
    float bootVoltage = getBatteryVoltage();

    Serial.printf(
        "[BOOT] 開機電壓: %.2f V\n",
        bootVoltage
    );

    if (audioReady && spiffsReady) {
        if (bootVoltage < 3.50f) {
            playWav("/low.wav");
        } else {
            playWav("/mid.wav");
        }
    }

    lastBatCheck = millis();
    lastVoiceTime = millis();

    currentState = VISION_LOOP;
}

// ============================================================
// 主迴圈
// ============================================================
void loop() {
    switch (currentState) {
        case VISION_LOOP:
            updateVisionPerception();

            if (
                debounceCount == 0 &&
                millis() - lastBatCheck >= BAT_INTERVAL
            ) {
                currentState = BATTERY_CHECK;
            }
            break;

        case VOICE_PLAYBACK:
            switch (pendingPrompt) {
                case PROMPT_GREEN:
                    playWav("/green.wav");
                    break;

                case PROMPT_RED:
                    playWav("/red.wav");
                    break;

                case PROMPT_BAT_MID:
                    playWav("/mid.wav");
                    break;

                case PROMPT_BAT_LOW:
                    playWav("/low.wav");
                    break;

                default:
                    break;
            }

            if (
                pendingPrompt == PROMPT_GREEN ||
                pendingPrompt == PROMPT_RED
            ) {
                lastVoiceTime = millis();
            }

            pendingPrompt = PROMPT_NONE;
            currentState = VISION_LOOP;
            break;

        case BATTERY_CHECK:
            checkBatteryRoutine();
            lastBatCheck = millis();
            break;

        case SYS_INIT:
        default:
            currentState = VISION_LOOP;
            break;
    }
}