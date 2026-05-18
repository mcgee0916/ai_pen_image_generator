/*
 * OledDisplay.h  (ai_pan_pc — 與 ai_pan 共用相同 header)
 * SSD1306 128×32
 *
 * 左半 [0–31]  × 32 — 手繪預覽 / bitmap 縮圖  (32×32)
 * 右半 [32–127] × 32 — 狀態文字                (96×32)
 */

#pragma once

#include <string.h>
#include <Wire.h>
#include <Adafruit_OLED_libraries/Adafruit_GFX.h>
#include <Adafruit_OLED_libraries/Adafruit_SSD1306.h>

#define OLED_W        128
#define OLED_H         32
#define OLED_ADDR     0x3C
#define OLED_DIVIDER   32

#define BMP_SIZE      192
#define BMP_PAD        20

struct Pt { float x, y; };

class OledDisplay {
public:
    Adafruit_SSD1306 dev;
    OledDisplay() : dev(OLED_W, OLED_H, &Wire, -1) {}

    bool begin();
    void showSplash();
    void drawDividers();
    void clearLeft();
    void clearRight();
    void drawLeftPreview(Pt *pts, bool *stroke_start, int pt_count);
    void drawLeftBitmap(uint8_t bmp[][BMP_SIZE]);
    void drawRightText(const char *text);
    void showFullStatus(const char *l1, const char *l2 = nullptr);

    // 動態圖示（左半 32×32）+ 右半文字，frame 每次 +1
    void animPC(int frame, const char *status);    // 電腦圖示
    void animWiFi(int frame, const char *status);  // WiFi 扇形圖示
};

inline bool OledDisplay::begin() {
    return dev.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
}

inline void OledDisplay::showSplash() {
    dev.clearDisplay();
    dev.setTextSize(1); dev.setTextColor(SSD1306_WHITE);
    dev.setCursor(16, 4);  dev.print(F("AI Image Pen"));
    dev.setCursor(16, 16); dev.print(F("PC mode ready"));
    dev.display(); delay(1500);
    dev.clearDisplay(); dev.display();
}

inline void OledDisplay::drawDividers() {
    dev.drawLine(OLED_DIVIDER, 0, OLED_DIVIDER, OLED_H-1, SSD1306_WHITE);
}

inline void OledDisplay::clearLeft() {
    dev.fillRect(0, 0, OLED_DIVIDER, OLED_H, SSD1306_BLACK);
}

inline void OledDisplay::clearRight() {
    dev.fillRect(OLED_DIVIDER+1, 0, OLED_W-OLED_DIVIDER-1, OLED_H, SSD1306_BLACK);
}

inline void OledDisplay::drawLeftPreview(Pt *pts, bool *stroke_start, int pt_count) {
    clearLeft();
    if (pt_count < 2) return;
    float mn_x=pts[0].x,mx_x=pts[0].x,mn_y=pts[0].y,mx_y=pts[0].y;
    for (int i=1;i<pt_count;i++) {
        if(pts[i].x<mn_x)mn_x=pts[i].x; if(pts[i].x>mx_x)mx_x=pts[i].x;
        if(pts[i].y<mn_y)mn_y=pts[i].y; if(pts[i].y>mx_y)mx_y=pts[i].y;
    }
    const float draw_sz=28.0f;
    float w=mx_x-mn_x,h=mx_y-mn_y;
    float sc=(w==0&&h==0)?1.0f:(w==0)?draw_sz/h:(h==0)?draw_sz/w:(draw_sz/w<draw_sz/h?draw_sz/w:draw_sz/h);
    float cx=(mn_x+mx_x)*0.5f,cy=(mn_y+mx_y)*0.5f;
    for (int i=1;i<pt_count;i++) {
        if(stroke_start[i]) continue;
        int x0=constrain((int)(16.0f+(pts[i-1].x-cx)*sc+0.5f),0,OLED_DIVIDER-1);
        int y0=constrain((int)(16.0f-(pts[i-1].y-cy)*sc+0.5f),0,OLED_H-1);
        int x1=constrain((int)(16.0f+(pts[i  ].x-cx)*sc+0.5f),0,OLED_DIVIDER-1);
        int y1=constrain((int)(16.0f-(pts[i  ].y-cy)*sc+0.5f),0,OLED_H-1);
        dev.drawLine(x0,y0,x1,y1,SSD1306_WHITE);
    }
}

inline void OledDisplay::drawLeftBitmap(uint8_t bmp[][BMP_SIZE]) {
    clearLeft();
    for (int r=0;r<OLED_H;r++) for (int c=0;c<OLED_DIVIDER;c++) {
        if (bmp[r*BMP_SIZE/OLED_H][c*BMP_SIZE/OLED_DIVIDER])
            dev.drawPixel(c,r,SSD1306_WHITE);
    }
}

inline void OledDisplay::drawRightText(const char *text) {
    clearRight();
    if (!text||!text[0]) return;
    dev.setTextSize(1); dev.setTextColor(SSD1306_WHITE);
    const int x0=OLED_DIVIDER+2, chars_line=(OLED_W-x0-1)/6, line_h=8;
    int y=0,pos=0,len=(int)strlen(text);
    while (pos<len&&y+line_h<=OLED_H) {
        int end=pos+chars_line; if(end>=len)end=len;
        else { int sp=end; while(sp>pos&&text[sp]!=' ')sp--; if(sp>pos)end=sp; }
        char line[18]={}; int lc=end-pos; if(lc>17)lc=17;
        strncpy(line,text+pos,lc); line[lc]='\0';
        dev.setCursor(x0,y); dev.print(line);
        pos=end; if(pos<len&&text[pos]==' ')pos++;
        y+=line_h;
    }
}

inline void OledDisplay::showFullStatus(const char *l1, const char *l2) {
    dev.clearDisplay(); dev.setTextSize(1); dev.setTextColor(SSD1306_WHITE);
    if (l1&&l1[0]) { dev.setCursor(2,  6); dev.print(l1); }
    if (l2&&l2[0]) { dev.setCursor(2, 20); dev.print(l2); }
    dev.display();
}

// ── 電腦圖示動畫（左半 32×32） ────────────────────────────────
// 螢幕框 + 底座，框內顯示旋轉 spinner
inline void OledDisplay::animPC(int frame, const char *status) {
    dev.clearDisplay();
    // 螢幕外框 (x=2..27, y=2..20)
    dev.drawRect(2, 2, 26, 19, SSD1306_WHITE);
    // 底座
    dev.drawLine(13, 21, 13, 25, SSD1306_WHITE);
    dev.drawLine(8,  26, 18, 26, SSD1306_WHITE);
    // 框內 spinner：8 方向旋轉短線，以螢幕中心 (15,11) 為圓心
    const int cx=15, cy=11;
    // 8 個 spinner 方向 (dx,dy) pairs
    static const int8_t spx[8] = { 0, 4, 5, 4, 0,-4,-5,-4};
    static const int8_t spy[8] = {-5,-4, 0, 4, 5, 4, 0,-4};
    int f = (frame & 7);
    // 畫 3 條相鄰的點（形成短尾巴）
    for (int i = 0; i < 3; i++) {
        int fi = (f + i) & 7;
        dev.drawPixel(cx + spx[fi], cy + spy[fi], SSD1306_WHITE);
    }
    // 右半狀態文字
    drawDividers();
    drawRightText(status);
    dev.display();
}

// ── WiFi 扇形動畫（左半 32×32） ───────────────────────────────
// 從底部中心向上發射的同心弧，frame 控制顯示幾層
inline void OledDisplay::animWiFi(int frame, const char *status) {
    dev.clearDisplay();
    const int cx = 16, cy = 29;   // 扇形圓心（左半底部中央）
    // 中心點
    dev.fillCircle(cx, cy, 2, SSD1306_WHITE);
    // 3 層弧，根據 frame%4 決定顯示幾層（1→2→3→全亮→1…）
    int show = (frame % 4);        // 0=僅點, 1,2,3 = 1/2/3 層弧
    // 繪製弧：只畫 210°~330° 的上半扇（相對 cy）
    static const int radii[3] = {7, 12, 17};
    for (int layer = 0; layer < 3; layer++) {
        if (show <= layer) continue;
        int r = radii[layer];
        // 用逐點方式畫弧（避免 float，用整數近似）
        for (int dx = -r; dx <= r; dx++) {
            int dy_sq = r*r - dx*dx;
            if (dy_sq < 0) continue;
            int dy = (int)sqrt((float)dy_sq);
            // 只畫上弧（y < cy）且在左半範圍內
            int px_ = cx + dx, py_ = cy - dy;
            if (px_>=1 && px_<=30 && py_>=1 && py_<=OLED_H-1)
                dev.drawPixel(px_, py_, SSD1306_WHITE);
        }
    }
    drawDividers();
    drawRightText(status);
    dev.display();
}
