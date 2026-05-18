/*
 * ai_pan_pc.ino  —  Version B
 * 使用者在 PAA 感測器上手繪 → 雙擊按鈕 →
 * JPEG 透過 COM port（USB Serial）傳送至電腦 →
 * 由 serial_receiver.py 串接 Gemini 生成精美圖片。
 *
 * 傳輸格式（純文字 framing + raw binary）：
 *   IMG_START:<byte_count>\r\n
 *   <byte_count 個 raw JPEG bytes>
 *   \r\nIMG_END\r\n
 */

#include <Wire.h>
#include <WiFi.h>
#include "OledDisplay.h"
#include <JPEGENC.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "vfs.h"
#include "sys_api.h"
#ifdef __cplusplus
}
#endif

// ── WiFi / Gemini 備援設定（無電腦時使用） ───────────────────
char wifi_ssid[] = "YOUR_SSID";
char wifi_pass[] = "YOUR_PASSWORD";
String GEMINI_KEY = "YOUR_GEMINI_API_KEY";
// 回應限制在一句話內，方便 OLED 顯示
static const char GEMINI_PROMPT[] =
    "This is a white-line sketch on a black background. "
    "In one sentence under 15 words, describe only what is drawn. "
    "Do not mention the line color or background.";
// ─────────────────────────────────────────────────────────────

#define BTN              15
#define LONG_PRESS_MS   300
#define DOUBLE_CLICK_MS 350
#define PAA_SERIAL      Serial3
#define PAA_BAUD        115200

#define SD_JPG_PATH    "sd:/char-0001.jpg"

#define MAX_PTS        800
#define PC_HELLO_WAIT  3000   // 等待 PC:READY 的毫秒上限
#define ACK_TIMEOUT    60000  // 等待 ACK（含 PC 端生圖時間）

OledDisplay oled;
WiFiSSLClient wifi_client;
static bool wifi_ready = false;

Pt   raw_pts[MAX_PTS];
bool stroke_start[MAX_PTS];
int  pt_count  = 0;
int  stroke_cnt = 0;
char wifi_result[128] = {};

uint8_t bmp[BMP_SIZE][BMP_SIZE];

struct FitCtx { int px[MAX_PTS]; int py[MAX_PTS]; float scale; };
static FitCtx  fit_ctx;
static float   fit_work_x[MAX_PTS];
static float   fit_work_y[MAX_PTS];

static bool     is_writing         = false;
static bool     first_pt_of_stroke = false;
static bool     recognize_pending  = false;

enum BtnEvt { BTN_NONE, BTN_LONG_START, BTN_LONG_END,
              BTN_SINGLE, BTN_DOUBLE, BTN_TRIPLE, BTN_QUAD };
static bool     _prev_btn    = HIGH;
static bool     _in_long     = false;
static uint32_t _press_t     = 0;
static uint32_t _last_click_t = 0;
static uint8_t  _click_count  = 0;

enum RunState { RUN_IDLE, RUN_CAPTURING, RUN_SENDING };
static RunState run_state = RUN_IDLE;

char uart_buf[128]; int uart_len = 0;
char cmd_buf[128];  int cmd_len  = 0;

// ── WiFi ─────────────────────────────────────────────────────
void ensureWiFi() {
    if (wifi_ready) return;
    WiFi.begin(wifi_ssid, wifi_pass);
    uint32_t t0=millis(), last_anim=0; int frame=0;
    while (WiFi.status() != WL_CONNECTED && millis()-t0 < 15000) {
        if (millis()-last_anim >= 250) {
            oled.animWiFi(frame++, wifi_ssid);
            last_anim = millis();
        }
        delay(50);
    }
    wifi_ready = (WiFi.status() == WL_CONNECTED);
    if (wifi_ready) {
        IPAddress ip = WiFi.localIP(); char s[16];
        snprintf(s, sizeof(s), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        oled.showFullStatus("WiFi OK", s); delay(800);
    } else {
        oled.showFullStatus("WiFi FAIL", "no network"); delay(2000);
    }
}

// ── Gemini 備援（base64 vision） ─────────────────────────────
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64Chunk(const uint8_t *src, int n, char *dst) {
    uint32_t b = ((uint32_t)src[0]<<16)|(n>1?(uint32_t)src[1]<<8:0)|(n>2?(uint32_t)src[2]:0);
    dst[0]=B64[(b>>18)&0x3F]; dst[1]=B64[(b>>12)&0x3F];
    dst[2]=(n>1)?B64[(b>>6)&0x3F]:'='; dst[3]=(n>2)?B64[b&0x3F]:'=';
}

String callGemini(uint8_t *img_buf, uint32_t img_len) {
    const char *HOST = "generativelanguage.googleapis.com";
    if (!wifi_client.connect(HOST, 443)) return "";
    String pre = String("{\"contents\":[{\"parts\":["
                 "{\"text\":\"") + GEMINI_PROMPT + "\"},"
                 "{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"";
    const char *post = "\"}}]}]}";
    uint32_t b64_len  = ((img_len+2)/3)*4;
    uint32_t body_len = (uint32_t)pre.length() + b64_len + (uint32_t)strlen(post);
    String path = String("/v1beta/models/gemini-2.5-flash:generateContent?key=") + GEMINI_KEY;
    wifi_client.println(String("POST ") + path + " HTTP/1.0");
    wifi_client.println(String("Host: ") + HOST);
    wifi_client.println("Content-Type: application/json");
    wifi_client.println(String("Content-Length: ") + String(body_len));
    wifi_client.println("Connection: close");
    wifi_client.println();
    wifi_client.print(pre);
    char b64_out[84];
    for (uint32_t i=0; i<img_len; i+=60) {
        uint32_t chunk=(img_len-i<60)?(img_len-i):60, out_pos=0;
        for (uint32_t j=0; j<chunk; j+=3) { int n=(int)(chunk-j); if(n>3)n=3; b64Chunk(img_buf+i+j,n,b64_out+out_pos); out_pos+=4; }
        wifi_client.write((uint8_t*)b64_out, out_pos);
    }
    wifi_client.print(post);
    String resp=""; bool body_mode=false; String hdr_buf="";
    uint32_t t0=millis();
    while (millis()-t0<15000) {
        while (wifi_client.available()) {
            char c=(char)wifi_client.read();
            if (!body_mode) { hdr_buf+=c; if(hdr_buf.endsWith("\r\n\r\n"))body_mode=true; }
            else resp+=c;
        }
        if (!wifi_client.connected()&&!wifi_client.available()) break;
    }
    wifi_client.stop();
    int idx=resp.indexOf("\"text\":"); if(idx<0) return "";
    idx=resp.indexOf('"',idx+7)+1; int end=idx;
    while (end<(int)resp.length()) { if(resp[end]=='"'&&resp[end-1]!='\\')break; end++; }
    String result=resp.substring(idx,end);
    result.replace("\\n"," "); result.replace("\\\"","\"");
    return result;
}

// ── 偵測 PC 是否就緒（握手） ─────────────────────────────────
// 送 PC:HELLO，等 PC:READY 回應；timeout 內沒有回應 → 無電腦
bool checkPCReady() {
    while (Serial.available()) Serial.read();
    Serial.println("PC:HELLO");
    char buf[16]; int len=0;
    uint32_t t0=millis(), last_anim=0; int frame=0;
    while (millis()-t0 < PC_HELLO_WAIT) {
        if (millis()-last_anim >= 150) {
            oled.animPC(frame++, "Check PC...");
            last_anim = millis();
        }
        if (Serial.available()) {
            char c=(char)Serial.read();
            if (c=='\n'||c=='\r') {
                if (len>0) { buf[len]='\0'; if(strstr(buf,"PC:READY"))return true; len=0; }
            } else if (len<15) buf[len++]=c;
        }
    }
    return false;
}

// ── PAA ──────────────────────────────────────────────────────
static bool containsOK(const char *s) {
    if (!s) return false;
    for (int i=0; s[i]&&s[i+1]; i++) {
        char a=s[i],b=s[i+1];
        if ((a=='O'||a=='o')&&(b=='K'||b=='k')) return true;
    }
    return false;
}

static bool waitForPAAOK(uint32_t timeout_ms) {
    char buf[64]; int len=0;
    uint32_t t0=millis();
    while (millis()-t0<timeout_ms) {
        if (PAA_SERIAL.available()) {
            char c=(char)PAA_SERIAL.read();
            if (c=='\n'||c=='\r') { if(len>0){buf[len]='\0';if(containsOK(buf))return true;len=0;} }
            else if (len<63) buf[len++]=c;
        }
    }
    return false;
}

// 回傳值：0=成功, 1=no AT OK（感測器無回應）, 2=no data（初始化後無 X/Y 資料）
int initPAA() {
    static bool begun=false;
    if (!begun) { PAA_SERIAL.begin(PAA_BAUD); begun=true; delay(500); }
    PAA_SERIAL.println("at-"); delay(200);
    while (PAA_SERIAL.available()) PAA_SERIAL.read();
    PAA_SERIAL.println("at");
    if (!waitForPAAOK(1000)) return 1;   // 無 AT OK
    PAA_SERIAL.println("at0g+"); waitForPAAOK(1000);
    PAA_SERIAL.println("atrpt+50"); delay(200);
    PAA_SERIAL.println("atx+");    delay(200);
    // 確認有資料輸出（等 500ms 看有無 X= 資料）
    uint32_t t0=millis();
    while (millis()-t0 < 500) {
        if (PAA_SERIAL.available()) {
            char c=(char)PAA_SERIAL.read();
            if (c=='X') return 0;   // 有資料，成功
        }
    }
    return 2;   // 無資料路徑
}

// ── Stroke / BMP ─────────────────────────────────────────────
static int compareFloatAsc(const void *a, const void *b) {
    float fa=*(const float*)a, fb=*(const float*)b;
    return (fa<fb)?-1:(fa>fb)?1:0;
}

bool autoFit(int sz, int pad, FitCtx &out) {
    if (pt_count<2) return false;
    float rmx=raw_pts[0].x,rmn=raw_pts[0].x,rmy=raw_pts[0].y,rmny=raw_pts[0].y;
    for (int i=0;i<pt_count;i++) {
        fit_work_x[i]=raw_pts[i].x; fit_work_y[i]=raw_pts[i].y;
        if(raw_pts[i].x<rmn) rmn=raw_pts[i].x; if(raw_pts[i].x>rmx) rmx=raw_pts[i].x;
        if(raw_pts[i].y<rmny)rmny=raw_pts[i].y;if(raw_pts[i].y>rmy) rmy=raw_pts[i].y;
    }
    qsort(fit_work_x,pt_count,sizeof(float),compareFloatAsc);
    qsort(fit_work_y,pt_count,sizeof(float),compareFloatAsc);
    int trim=(pt_count>=20)?pt_count/20:0; if(trim*2>=pt_count)trim=0;
    float mn_x=fit_work_x[trim],mx_x=fit_work_x[pt_count-1-trim];
    float mn_y=fit_work_y[trim],mx_y=fit_work_y[pt_count-1-trim];
    if(mx_x<=mn_x){mn_x=rmn;mx_x=rmx;} if(mx_y<=mn_y){mn_y=rmny;mx_y=rmy;}
    float w=mx_x-mn_x,h=mx_y-mn_y,fit=(float)(sz-pad*2);
    float sc=(w==0&&h==0)?1.0f:(w==0)?fit/h:(h==0)?fit/w:(fit/w<fit/h?fit/w:fit/h);
    float cx=(mn_x+mx_x)*0.5f,cy=(mn_y+mx_y)*0.5f,c=sz*0.5f;
    for (int i=0;i<pt_count;i++) {
        out.px[i]=(int)(c+(raw_pts[i].x-cx)*sc+0.5f);
        out.py[i]=(int)(c-(raw_pts[i].y-cy)*sc+0.5f);
    }
    out.scale=sc; return true;
}

static void plotBmp3px(int x, int y) {
    for (int dy=-1;dy<=1;dy++) for (int dx=-1;dx<=1;dx++) {
        int py=y+dy,px=x+dx;
        if(py>=0&&py<BMP_SIZE&&px>=0&&px<BMP_SIZE) bmp[py][px]=1;
    }
}

void bmpLine(int x0,int y0,int x1,int y1) {
    int dx=abs(x1-x0),dy=abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx-dy;
    while(true) {
        if(x0>=0&&x0<BMP_SIZE&&y0>=0&&y0<BMP_SIZE) plotBmp3px(x0,y0);
        if(x0==x1&&y0==y1) break;
        int e2=2*err; if(e2>-dy){err-=dy;x0+=sx;} if(e2<dx){err+=dx;y0+=sy;}
    }
}

void renderToBmp(const FitCtx &fc) {
    memset(bmp,0,sizeof(bmp));
    for (int i=1;i<pt_count;i++) {
        if(stroke_start[i]) continue;
        bmpLine(fc.px[i-1],fc.py[i-1],fc.px[i],fc.py[i]);
    }
}

void clearStrokes() {
    pt_count=0; stroke_cnt=0;
    memset(raw_pts,0,sizeof(raw_pts)); memset(stroke_start,0,sizeof(stroke_start));
    memset(bmp,0,sizeof(bmp)); memset(&fit_ctx,0,sizeof(fit_ctx));
    memset(fit_work_x,0,sizeof(fit_work_x)); memset(fit_work_y,0,sizeof(fit_work_y));
}

// ── JPEG to SD ───────────────────────────────────────────────
static void *jpegOpen(const char *p)                        { return fopen(p,"w+b"); }
static void  jpegClose(JPEGE_FILE *f)                       { if(f->fHandle)fclose((FILE*)f->fHandle); }
static int32_t jpegRead(JPEGE_FILE *f,uint8_t *b,int32_t l) { return (int32_t)fread(b,1,l,(FILE*)f->fHandle); }
static int32_t jpegWrite(JPEGE_FILE *f,uint8_t *b,int32_t l){ return (int32_t)fwrite(b,1,l,(FILE*)f->fHandle); }
static int32_t jpegSeek(JPEGE_FILE *f,int32_t pos)          { fseek((FILE*)f->fHandle,pos,SEEK_SET); return (int32_t)ftell((FILE*)f->fHandle); }

bool writeJpegImageToSD(const char *path) {
    JPEGENC jpg; JPEGENCODE enc; uint8_t mcu[16*16*3];
    if (jpg.open(path,jpegOpen,jpegClose,jpegRead,jpegWrite,jpegSeek)!=JPEGE_SUCCESS) return false;
    if (jpg.encodeBegin(&enc,BMP_SIZE,BMP_SIZE,JPEGE_PIXEL_RGB888,JPEGE_SUBSAMPLE_420,JPEGE_Q_HIGH)!=JPEGE_SUCCESS)
        { jpg.close(); return false; }
    for (int y=0;y<BMP_SIZE;y+=enc.cy) for (int x=0;x<BMP_SIZE;x+=enc.cx) {
        for (int yy=0;yy<enc.cy;yy++) for (int xx=0;xx<enc.cx;xx++) {
            uint8_t v=bmp[y+yy][x+xx]?255:0;
            int idx=(yy*enc.cx+xx)*3; mcu[idx]=mcu[idx+1]=mcu[idx+2]=v;
        }
        if (jpg.addMCU(&enc,mcu,enc.cx*3)!=JPEGE_SUCCESS){jpg.close();return false;}
    }
    return jpg.close()>0;
}

// ── Serial 傳圖 ──────────────────────────────────────────────
bool sendImageViaSerial() {
    FILE *fp = fopen(SD_JPG_PATH, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    uint32_t size = (uint32_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // header
    Serial.print("IMG_START:");
    Serial.print(size);
    Serial.print("\r\n");

    // raw bytes in chunks
    uint8_t buf[256];
    uint32_t remaining = size;
    while (remaining > 0) {
        uint32_t chunk = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        fread(buf, 1, chunk, fp);
        Serial.write(buf, chunk);
        remaining -= chunk;
    }
    fclose(fp);

    Serial.print("\r\nIMG_END\r\n");
    return true;
}

// 等待 PC 回傳 "ACK"（不區分大小寫）
bool waitForACK(uint32_t timeout_ms) {
    char buf[16]; int len = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        if (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                if (len > 0) {
                    buf[len] = '\0';
                    if (strstr(buf, "ACK") || strstr(buf, "ack")) return true;
                    len = 0;
                }
            } else if (len < 15) {
                buf[len++] = c;
            }
        }
    }
    return false;
}

// ── Button ───────────────────────────────────────────────────
BtnEvt pollButton() {
    bool cur=digitalRead(BTN); uint32_t now=millis(); BtnEvt evt=BTN_NONE;
    if(cur==LOW&&_prev_btn==HIGH){_press_t=now;}
    else if(cur==HIGH&&_prev_btn==LOW){
        if(_in_long){_in_long=false;evt=BTN_LONG_END;}
        else if(now-_press_t<LONG_PRESS_MS){if(_click_count<4)_click_count++;_last_click_t=now;}
    } else if(cur==LOW){
        if(!_in_long&&(now-_press_t>=LONG_PRESS_MS)){_in_long=true;_click_count=0;evt=BTN_LONG_START;}
    } else if(_click_count>0&&(now-_last_click_t>=DOUBLE_CLICK_MS)){
        if(_click_count==1)evt=BTN_SINGLE;
        else if(_click_count==2)evt=BTN_DOUBLE;
        else if(_click_count==3)evt=BTN_TRIPLE;
        else evt=BTN_QUAD;
        _click_count=0;
    }
    _prev_btn=cur; return evt;
}

void redrawIdle() {
    oled.dev.clearDisplay();
    oled.drawLeftPreview(raw_pts, stroke_start, pt_count);
    oled.drawDividers();
    if (wifi_result[0]) oled.drawRightText(wifi_result);
    oled.dev.display();
}

void handleButtonEvent(BtnEvt evt) {
    switch (evt) {
        case BTN_LONG_START:
            if (run_state==RUN_SENDING||recognize_pending) break;
            is_writing=true; first_pt_of_stroke=true; stroke_cnt++; run_state=RUN_CAPTURING;
            break;
        case BTN_LONG_END:
            if (!is_writing) break;
            is_writing=false; if(run_state!=RUN_SENDING) run_state=RUN_IDLE;
            Serial.println("PT:END");
            break;
        case BTN_SINGLE:
            if (run_state==RUN_SENDING) break;
            clearStrokes(); redrawIdle();
            Serial.println("PT:CLEAR");
            break;
        case BTN_DOUBLE:
            if (run_state==RUN_SENDING||recognize_pending) break;
            recognize_pending=true; is_writing=false; first_pt_of_stroke=false;
            break;
        case BTN_TRIPLE:
            if (run_state==RUN_SENDING||is_writing) break;
            clearStrokes();
            wifi_result[0] = '\0';
            redrawIdle();
            Serial.println("PT:CLEAR");
            break;
        case BTN_QUAD:
            if (run_state==RUN_SENDING||is_writing) break;
            oled.showFullStatus("PAA REINIT","...");
            { int r=initPAA();
              if      (r==1) oled.showFullStatus("PAA ERROR","no AT OK");
              else if (r==2) oled.showFullStatus("PAA ERROR","no data");
              else redrawIdle(); }
            break;
        default: break;
    }
}

// ── 傳圖主流程 ───────────────────────────────────────────────
void onSend() {
    if (run_state==RUN_SENDING) return;
    recognize_pending=false; run_state=RUN_SENDING; is_writing=false;

    if (pt_count<2) { run_state=RUN_IDLE; return; }
    if (!autoFit(BMP_SIZE, BMP_PAD, fit_ctx)) { run_state=RUN_IDLE; return; }

    renderToBmp(fit_ctx);
    oled.dev.clearDisplay();
    oled.drawLeftBitmap(bmp);
    oled.drawDividers();
    oled.drawRightText("Saving...");
    oled.dev.display();

    if (!writeJpegImageToSD(SD_JPG_PATH)) {
        oled.dev.clearDisplay(); oled.drawLeftBitmap(bmp); oled.drawDividers();
        oled.drawRightText("SD ERROR"); oled.dev.display();
        run_state=RUN_IDLE; return;
    }

    // ── 偵測 PC ──────────────────────────────────────────────
    oled.dev.clearDisplay(); oled.drawLeftBitmap(bmp); oled.drawDividers();
    oled.drawRightText("Check PC..."); oled.dev.display();

    if (checkPCReady()) {
        // ── PC 模式：傳圖 → 等 ACK ───────────────────────────
        oled.dev.clearDisplay(); oled.drawLeftBitmap(bmp); oled.drawDividers();
        oled.drawRightText("Sending..."); oled.dev.display();

        Serial.println("[SEND] img ready, sending...");
        if (!sendImageViaSerial()) {
            oled.dev.clearDisplay(); oled.drawLeftBitmap(bmp); oled.drawDividers();
            oled.drawRightText("SEND ERROR"); oled.dev.display();
            delay(2000); run_state=RUN_IDLE; return;
        }

        oled.dev.clearDisplay(); oled.drawLeftBitmap(bmp); oled.drawDividers();
        oled.drawRightText("Wait ACK..."); oled.dev.display();

        if (waitForACK(ACK_TIMEOUT)) {
            oled.dev.clearDisplay(); oled.drawLeftBitmap(bmp); oled.drawDividers();
            oled.drawRightText("PC generating"); oled.dev.display();
            Serial.println("[SEND] ACK received");
        } else {
            oled.dev.clearDisplay(); oled.drawLeftBitmap(bmp); oled.drawDividers();
            oled.drawRightText("ACK timeout"); oled.dev.display();
            Serial.println("[SEND] ACK timeout");
        }
    } else {
        // ── WiFi 備援模式：直接問 Gemini 顯示 OLED ──────────
        Serial.println("[SEND] no PC, using WiFi");
        oled.dev.clearDisplay(); oled.drawLeftBitmap(bmp); oled.drawDividers();
        oled.drawRightText("No PC WiFi..."); oled.dev.display();

        ensureWiFi();
        if (!wifi_ready) { run_state=RUN_IDLE; return; }

        // 讀 JPEG 進 RAM
        FILE *fp = fopen(SD_JPG_PATH, "rb");
        if (!fp) {
            oled.dev.clearDisplay(); oled.drawLeftBitmap(bmp); oled.drawDividers();
            oled.drawRightText("SD open fail"); oled.dev.display();
            Serial.println("[ERR] SD open fail");
            delay(2000); run_state=RUN_IDLE; return;
        }
        fseek(fp, 0, SEEK_END); uint32_t img_len=(uint32_t)ftell(fp); fseek(fp,0,SEEK_SET);
        uint8_t *img_buf=(uint8_t*)malloc(img_len);
        if (!img_buf) {
            fclose(fp);
            oled.dev.clearDisplay(); oled.drawLeftBitmap(bmp); oled.drawDividers();
            oled.drawRightText("RAM fail"); oled.dev.display();
            Serial.println("[ERR] malloc fail");
            delay(2000); run_state=RUN_IDLE; return;
        }
        fread(img_buf,1,img_len,fp); fclose(fp);

        oled.dev.clearDisplay(); oled.drawLeftBitmap(bmp); oled.drawDividers();
        oled.drawRightText("Asking AI..."); oled.dev.display();

        String response = callGemini(img_buf, img_len);
        free(img_buf);

        const char *txt = response.length()>0 ? response.c_str() : "No result";
        strncpy(wifi_result, txt, sizeof(wifi_result)-1);
        wifi_result[sizeof(wifi_result)-1] = '\0';

        oled.dev.clearDisplay(); oled.drawLeftBitmap(bmp); oled.drawDividers();
        oled.drawRightText(wifi_result);
        oled.dev.display();
        run_state=RUN_IDLE; return;
    }

    delay(2000);
    run_state=RUN_IDLE;
}

// ── PAA 資料行處理 ───────────────────────────────────────────
void processLine(const char *line) {
    if (!strstr(line, "X=")) return;
    auto parseField=[](const char *l,const char *key)->float {
        const char *p=strstr(l,key); return p?(float)atof(p+strlen(key)):0.0f;
    };
    float x=parseField(line,"X="), y=parseField(line,"Y=");
    if (is_writing&&pt_count<MAX_PTS) {
        raw_pts[pt_count]={x,y}; stroke_start[pt_count]=first_pt_of_stroke;
        if (first_pt_of_stroke) Serial.println("PT:NEW");
        Serial.print("PT:"); Serial.print(x,1); Serial.print(","); Serial.println(y,1);
        pt_count++; first_pt_of_stroke=false;
    }
    static uint32_t _oled_t=0;
    if (millis()-_oled_t<150) return; _oled_t=millis();
    oled.dev.clearDisplay();
    oled.drawLeftPreview(raw_pts,stroke_start,pt_count);
    oled.drawDividers();
    if (wifi_result[0]) oled.drawRightText(wifi_result);
    oled.dev.display();
}

// ── Setup / Loop ──────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(BTN, INPUT_PULLUP);
    if (!oled.begin()) { Serial.println("[OLED] fail"); for(;;); }
    oled.showSplash();

    vfs_init(NULL);
    if (vfs_user_register("sd",VFS_FATFS,VFS_INF_SD)!=0)
        Serial.println("[VFS] SD mount fail");

    if (access(SD_JPG_PATH,F_OK)!=0) { FILE *f=fopen(SD_JPG_PATH,"wb"); if(f)fclose(f); }

    { int r=initPAA();
      if      (r==1) { oled.showFullStatus("PAA ERROR","no AT OK");  Serial.println("[PAA] no AT OK"); }
      else if (r==2) { oled.showFullStatus("PAA ERROR","no data");   Serial.println("[PAA] no data"); }
    }
    clearStrokes();
    redrawIdle();
    Serial.println("[READY] draw and double-click to send");
}

void loop() {
    handleButtonEvent(pollButton());

    while (Serial.available()) {
        char c=(char)Serial.read();
        if (run_state==RUN_SENDING) break;  // 傳圖中不接受 passthrough
        if(c=='\n'||c=='\r'){if(cmd_len>0){cmd_buf[cmd_len]='\0';PAA_SERIAL.println(cmd_buf);cmd_len=0;}}
        else if(cmd_len<127) cmd_buf[cmd_len++]=c;
    }

    static uint32_t _paa_last=0;
    if (_paa_last==0) _paa_last=millis();

    while (PAA_SERIAL.available()) {
        handleButtonEvent(pollButton());
        _paa_last=millis();
        char c=(char)PAA_SERIAL.read();
        if(c=='\n'||c=='\r'){if(uart_len>0){uart_buf[uart_len]='\0';processLine(uart_buf);uart_len=0;}}
        else if(uart_len<127) uart_buf[uart_len++]=c;
    }

    if (millis()-_paa_last>1000) {
        while(PAA_SERIAL.available())PAA_SERIAL.read(); uart_len=0;
        int r=initPAA();
        if (r==0) { redrawIdle(); }
        else {
            const char *reason = (r==1)?"no AT OK":"no data";
            oled.showFullStatus("PAA RETRY", reason);
            Serial.print("[PAA] retry fail: "); Serial.println(reason);
        }
        _paa_last=millis();
    }

    if (recognize_pending&&run_state!=RUN_SENDING) onSend();
}
