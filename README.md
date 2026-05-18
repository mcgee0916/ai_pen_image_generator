# AI 智慧繪圖筆

一款 AI 繪圖筆裝置，透過光學追蹤感測器捕捉手繪筆跡，將其編碼為 JPEG 影像後送交 Google Gemini，生成精美的 AI 圖片或文字描述。

**開發單位：瑞創框架科技股份有限公司 (RECSFRAME TECHNOLOGY CO., LTD.)**
<img width="1916" height="1054" alt="螢幕擷取畫面 2026-05-19 001127" src="https://github.com/user-attachments/assets/6f2d3ef5-9b6b-46c6-9e11-babe87352d5c" />

\---

## 系統架構

```
PAA5163D 感測器 ──Serial3──►     HUB8735Ultra ──USB Serial──► PC（AI\_Pen.exe）
（光學定位）                   （韌體）                       └──► Gemini API ──► 生成圖片
                                    │
                                    └──► WiFi（備援）──► Gemini API ──► OLED 顯示結果
```

裝置支援兩種工作模式：

* **連接電腦模式**：筆跡透過 USB Serial 傳至 `AI\_Pen.exe`，由 PC 呼叫 Gemini 生成圖片
* **獨立模式**：偵測不到電腦時，裝置自行連接 WiFi 呼叫 Gemini，將文字結果顯示在 OLED 螢幕

\---

## 硬體規格

|元件|功能|
|-|-|
|HUB8735Ultra|主控 SoC，執行韌體|
|PAA5163D 光學感測器|XY 座標追蹤，Serial3（115200 baud）|
|SSD1306 OLED（128×32）|即時筆跡預覽與狀態顯示|
|KY-004 微動開關（單擊 / 雙擊 / 三擊 / 長按）|
|SD 卡|JPEG 暫存緩衝（`sd:/char-0001.jpg`）|
|3D 列印外殼|`ai\_pan\_frame.stl`|

\---

## 檔案結構

```
ai\_pan\_pc/
  ai\_pan\_pc.ino       — 主韌體（Ameba RTL8720，Arduino）
  OledDisplay.h       — OLED 驅動封裝（SSD1306 + 版面輔助函式）

Unit\_testing/
  ALL/ALL.ino         — 整合測試：OLED + PAA5163D + 按鈕（飛機射擊小遊戲）
  BTN/BTN.ino         — 按鈕單元測試
  OLED/OLED.ino       — OLED 單元測試
  OLED\_BTN/           — OLED + 按鈕組合測試
  PAA5163D/PAA5163D.ino — PAA5163D 感測器單元測試
  HUB-PAA5163D\_Tracker.exe        — Windows 路徑視覺化工具
  HUB-PAA5163D\_路徑測試程式使用說明.pdf — 路徑工具使用說明

ai\_pan\_frame.stl      — 3D 列印筆身外殼
AI\_Pen.exe            — PC 端接收程式 + Gemini 圖片生成器（Windows）
AI\_Image\_Pen.pptx     — 專案簡報
```

\---

## 按鈕手勢

|手勢|動作|
|-|-|
|雙擊|擷取當前筆跡 → 送出 AI 生圖|
|長按|開始 / 結束筆畫描繪|
|單擊|清除畫板）|

\---

## 韌體設定

### 開發環境

* Arduino IDE，安裝 **Ameba RTL8720（AmebaProII）** 開發板支援
* 需安裝函式庫：`JPEGENC`、`Adafruit\_SSD1306`、`Adafruit\_GFX`

### 設定金鑰（`ai\_pan\_pc.ino`）

```cpp
char wifi\_ssid\[] = "YOUR\_SSID";
char wifi\_pass\[] = "YOUR\_PASSWORD";
String GEMINI\_KEY = "YOUR\_GEMINI\_API\_KEY";
```

燒錄前請填入自己的 WiFi 帳密與 Gemini API Key。韌體預設以連接電腦模式運作，WiFi 僅作為備援。

### 燒錄步驟

1. 以 Arduino IDE 開啟 `ai\_pan\_pc/ai\_pan\_pc.ino`
2. 選擇開發板：HUB8735Ultra
3. 透過 USB Serial 燒錄

\---

## PC 端程式（`AI\_Pen.exe`）

Windows 執行檔。將繪圖筆以 USB 連接電腦後執行，流程如下：

1. 監聽裝置發送的握手訊號 `PC:HELLO`，回應 `PC:READY`
2. 接收以下格式的 JPEG 影像資料：

```
   IMG\_START:<byte\_count>\\r\\n
   <raw JPEG bytes>
   \\r\\nIMG\_END\\r\\n
   ```

3. 呼叫 Gemini API 依筆跡生成精美圖片
4. 生成完成後回傳 `ACK` 給裝置（逾時上限：60 秒）

\---

## 通訊協定（裝置 ↔ PC）

|訊息|方向|說明|
|-|-|-|
|`PC:HELLO`|裝置 → PC|握手發起|
|`PC:READY`|PC → 裝置|PC 就緒|
|`PT:NEW`|裝置 → PC|新筆畫開始|
|`PT:<x>,<y>`|裝置 → PC|座標點資料|
|`IMG\_START:<n>`|裝置 → PC|JPEG 傳輸開始|
|`IMG\_END`|裝置 → PC|JPEG 傳輸結束|
|`ACK`|PC → 裝置|圖片生成完成|

\---

## 單元測試

`Unit\_testing/` 內各草稿分別驗證單一子系統：

|草稿|測試內容|
|-|-|
|`BTN.ino`|按鈕防彈跳、單擊 / 雙擊 / 長按偵測|
|`OLED.ino`|SSD1306 初始化、文字渲染、點陣圖顯示|
|`OLED\_BTN/`|按鈕與 OLED 互動|
|`PAA5163D.ino`|感測器 AT 握手、XY 資料串流|
|`ALL.ino`|全整合測試——以飛機射擊小遊戲同時驗證三個子系統|

可使用 `HUB-PAA5163D\_Tracker.exe` 在 Windows 上視覺化 PAA5163D 的 XY 路徑，詳細操作請參閱隨附 PDF。

\---

## 3D 外殼

`ai\_pan\_frame.stl` — 筆身外殼，設計用於容納 Ameba 開發板、PAA5163D 感測器與按鈕。建議以 PLA 材料、標準 FDM 設定列印。

\---

