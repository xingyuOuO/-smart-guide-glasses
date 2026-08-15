# 🕶️ Edge AI Smart Guide Goggles (純離線智慧視障導盲眼鏡)

![ESP32-S3](https://img.shields.io/badge/MCU-ESP32--S3-orange?style=flat-square)
![Grove Vision AI V2](https://img.shields.io/badge/NPU-Grove_Vision_AI_V2-blue?style=flat-square)
![C++](https://img.shields.io/badge/Language-C%2B%2B-00599C?style=flat-square)


> **「在極限硬體資源下，榨乾微控制器的每一滴效能。」**

本專案旨在解決視障者於戶外路口面臨的交通號誌辨識痛點。捨棄高延遲的雲端連線架構，實作 **100% 純離線 (Off-grid)** 的穿戴式邊緣運算系統。全機包含電池總重控制於 145g 內，具備低功耗、零網路依賴與極高的容錯安全性。

---

## ✨ 系統亮點 (Key Features)

- 🚫 **純離線邊緣推論**：無需 Wi-Fi/4G，由 NPU 獨立完成交通號誌物件偵測。
- 🛡️ **防禦性容錯演算法**：內建連續時域去顫濾波 (Debounce Logic)。
- 🎶 **語音指引**：非阻塞多工狀態機搭配 DMA 雙緩衝，透過骨傳導震動發聲。

---

## 🏗️ 硬體架構 (Hardware Architecture)

- **主控大腦 (Main MCU):** Seeed Studio XIAO ESP32S3
- **視覺邊緣節點 (Vision NPU):** Grove Vision AI V2 (運行 INT8 量化模型)
- **音訊功放晶片:** MAX98357A (I2S Class-D) + 骨傳導揚聲器
- **供電與機構:** 3.7V 鋰電池 +  3D 列印眼鏡

---

## 🛠️ 核心實作與開發指南 (Implementation Details & Developer Notes)

本專案在極限的 SRAM 與單執行緒環境下進行開發，若您有意 clone 本專案或參與貢獻，請務必詳閱以下技術實作細節與避坑指南。

### 1. ⚠️ 模型部署陷阱：請勿使用預設 Web 燒錄器

我們使用 Roboflow 訓練了 **Text-Image Training Model**。

- **Issue:** Grove Vision AI V2 官方提供的 Web 介面無法正確解析該模型結構的輸入/輸出張量 (Tensor) 維度，導致燒錄後無法輸出推論結果。
- **Solution:** 必須捨棄網頁介面，改用原廠專屬的 **Toolchain（工具鏈）**。在命令列手動將 `.tflite` 模型檔與韌體環境編譯打包為二進位檔後，再透過終端機強制寫入 Flash。

### 2. 底層通訊與非阻塞狀態機 (Non-blocking FSM)

傳統 `delay()` 會導致影像抓取與語音播放互相卡死 (Blocking)。 本專案導入了非阻塞多工有限狀態機 (FSM)，將「I2C 影像輪詢」與「I2S 音訊寫入」徹底解耦。利用 ESP32S3 的 DMA 背景搬運音訊，讓系統能一邊流暢播報語音，一邊持續透過 I2C (`0x62`) 接收 NPU 傳來的 2-byte 分類結果。

### 3. ⚠️ MAX98357A 音訊失真與腳位除錯

- **Issue:** 播放 WAV 語音時遭遇嚴重破音與電流聲。
- **Solution:** I2S 總線訊號排查，確認為時序錯位。請嚴格依照本專案原始碼中的腳位定義對齊：
  - `DOUT` (Data Out): 數位音訊數據傳輸。
  - `LRC` (Left/Right Clock): 左右聲道頻率選擇。
  - `BCLK` (Bit Clock): 資料位元時鐘。 
  *(註：同時在程式碼結尾加入了 8 組 512-byte 零電平補丁，強制 DAC 歸零以消除殘音。)*

### 4. 演算法優化：時域去顫投票機制 (Debounce Logic)

戶外環境的閃黃燈與車燈反光極易造成單幀影像誤判。為確保視障者絕對安全，決策層不信任單一畫面：

```cpp
#define TH_DEBOUNCE 6 
// 邏輯：必須在時間軸上連續 6 幀的信心度皆超越門檻，狀態機才會解鎖並觸發音訊。
```

此設計用些微的運算毫秒數，換取了戶外複雜光源下低誤判率。


