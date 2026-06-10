#include <Arduino.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>

// ============================================================
// Clawd Tank — Pixel Art Crab for ESP32-C3 + GC9A01 (240x240)
// Based on marciogranzotto/clawd-tank design
// WiFi + HTTP POST /event endpoint
// ============================================================

// --- WiFi Config (multi-network) ---
#include "wifi_credentials.h"
WiFiMulti wifiMulti;

WebServer server(80);
bool wifiConnected = false;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite canvas = TFT_eSprite(&tft);

// --- Clawd Colors (from official SVG) ---
#define C_CRAB      0xDC8D  // #DE886D salmon pink body
#define C_EYE       0x0000  // black eyes
#define C_BG        0x1082  // #10101A dark background
#define C_SHADOW    0x0000  // shadow

// Activity accent colors
#define C_ACCENT    0x6BFF  // #6B7FD7 blue
#define C_GOOD      0x4E69  // green
#define C_WARN      0xFE20  // yellow/orange
#define C_ERR       0xF886  // red
#define C_MAGIC     0xB33F  // purple
#define C_FLASH     0xFFE0  // bright
#define C_LABEL     0x8C71  // gray text
#define C_INACTIVE  0x4208  // dim

// --- Screen ---
#define SW 240
#define SH 240
#define CX 120
#define CY 120

// Pixel scale: each "pixel" of the 15x16 crab = PX screen pixels
#define PX 10

// --- Activity States ---
enum Activity : uint8_t {
    ACT_IDLE = 0,
    ACT_READING,
    ACT_EDITING,
    ACT_BASH,
    ACT_SEARCH,
    ACT_MCP,
    ACT_THINKING,
    ACT_HAPPY,
    ACT_ERROR,
    ACT_SLEEPING,
    ACT_DISCONNECTED,
    ACT_COUNT
};

const char* activityNames[] = {
    "Idle", "Reading", "Editing", "Running",
    "Searching", "MCP", "Thinking", "Happy!",
    "Error", "Zzz...", "No WiFi"
};

uint16_t activityColors[ACT_COUNT];

void initColors() {
    activityColors[ACT_IDLE]    = C_LABEL;
    activityColors[ACT_READING] = C_ACCENT;
    activityColors[ACT_EDITING] = C_GOOD;
    activityColors[ACT_BASH]    = C_WARN;
    activityColors[ACT_SEARCH]  = C_MAGIC;
    activityColors[ACT_MCP]     = C_ERR;
    activityColors[ACT_THINKING]= C_FLASH;
    activityColors[ACT_HAPPY]   = 0xFFE0;  // bright yellow
    activityColors[ACT_ERROR]   = C_ERR;
    activityColors[ACT_SLEEPING]= C_INACTIVE;
    activityColors[ACT_DISCONNECTED] = C_ERR;
}

// --- State ---
Activity currentActivity = ACT_IDLE;
unsigned long lastEventTime = 0;
char lastTool[24] = "";
int eventCount = 0;

// --- Session tracking ---
char sessionId[20] = "";
unsigned long sessionStartMs = 0;
int sessionEvents = 0;
unsigned long sessionTokens = 0;
#define SESSION_WINDOW_SEC 18000   // 5 hours (Pro plan reset window)
#define PLAN_TOKEN_LIMIT   800000  // ~800k tokens per window (Pro estimate)

// --- Battery keepalive ---
unsigned long lastKeepalive = 0;
#define KEEPALIVE_INTERVAL 25000  // 25 seconds

// --- Animation ---
float phase = 0;
unsigned long frameCount = 0;
bool isBlinking = false;
int blinkCountdown = 80;
int blinkFrame = 0;

// --- Serial ---
char serialBuf[512];
int serialPos = 0;

// --- Sparkles ---
#define MAX_SPARKLES 6
struct Sparkle {
    int x, y;
    int frame;
    int maxFrames;
    uint16_t color;
};
Sparkle sparkles[MAX_SPARKLES];

void spawnSparkle(uint16_t color) {
    for (int i = 0; i < MAX_SPARKLES; i++) {
        if (sparkles[i].frame <= 0) {
            sparkles[i].x = random(30, 210);
            sparkles[i].y = random(20, 140);
            sparkles[i].frame = 1;
            sparkles[i].maxFrames = random(8, 16);
            sparkles[i].color = color;
            return;
        }
    }
}

void drawSparkles() {
    for (int i = 0; i < MAX_SPARKLES; i++) {
        if (sparkles[i].frame > 0) {
            int x = sparkles[i].x;
            int y = sparkles[i].y;
            int f = sparkles[i].frame;
            int half = sparkles[i].maxFrames / 2;
            int size = (f <= half) ? f : (sparkles[i].maxFrames - f);
            size = size * 2 + 1;

            uint16_t c = sparkles[i].color;
            // Cross shape sparkle (pixel art style)
            canvas.drawFastHLine(x - size, y, size * 2 + 1, c);
            canvas.drawFastVLine(x, y - size, size * 2 + 1, c);
            // Corner dots
            if (size > 2) {
                canvas.drawPixel(x - size/2, y - size/2, c);
                canvas.drawPixel(x + size/2, y - size/2, c);
                canvas.drawPixel(x - size/2, y + size/2, c);
                canvas.drawPixel(x + size/2, y + size/2, c);
            }

            sparkles[i].frame++;
            if (sparkles[i].frame > sparkles[i].maxFrames) {
                sparkles[i].frame = 0;
            }
        }
    }
}

// ============================================================
//  Pixel Art Clawd — based on the official 15x16 grid
// ============================================================
//  Grid (15 wide x 16 tall):
//  Row 0-5:  empty (space for eyes to pop up)
//  Row 6-12: torso (x=2..12, y=6..12) in C_CRAB
//  Row 9-10: left arm (x=0..1), right arm (x=13..14)
//  Row 13-14: legs at x=3,5,9,11
//  Row 15: shadow

void drawPixelBlock(int gx, int gy, int gw, int gh, uint16_t color, int ox, int oy) {
    canvas.fillRect(ox + gx * PX, oy + gy * PX, gw * PX, gh * PX, color);
}

void drawClawd(int ox, int oy, float ph) {
    // Animation offsets
    int bobY = 0;
    float armAngleL = 0, armAngleR = 0;
    int eyeOx = 0, eyeOy = 0;
    bool legsAlt = false; // alternate leg position

    switch (currentActivity) {
        case ACT_IDLE: {
            // Gentle breathing: subtle Y scale simulation via bob
            bobY = (int)(1.5f * sinf(ph * 0.8f));
            // Occasional look around
            eyeOx = (int)(2 * sinf(ph * 0.3f));
            legsAlt = ((int)(ph * 0.5f) % 2) == 0;
            break;
        }
        case ACT_READING: {
            bobY = (int)(sinf(ph * 0.6f));
            // Eyes scan left-right quickly
            eyeOx = (int)(3 * sinf(ph * 3.0f));
            legsAlt = ((int)(ph) % 2) == 0;
            break;
        }
        case ACT_EDITING: {
            // Jitter body slightly (typing furiously)
            bobY = (frameCount % 2 == 0) ? 0 : 1;
            eyeOx = (int)(2 * sinf(ph * 2.0f));
            legsAlt = (frameCount % 4 < 2);
            break;
        }
        case ACT_BASH: {
            // Bounce
            bobY = (int)(3.0f * fabsf(sinf(ph * 3.0f)));
            eyeOy = -1;
            legsAlt = (frameCount % 3 < 2);
            break;
        }
        case ACT_SEARCH: {
            bobY = (int)(2.0f * sinf(ph * 1.2f));
            // Eyes go circular
            eyeOx = (int)(2 * cosf(ph * 1.5f));
            eyeOy = (int)(1 * sinf(ph * 1.5f));
            legsAlt = ((int)(ph * 0.8f) % 2) == 0;
            break;
        }
        case ACT_MCP: {
            // Excited bouncing
            bobY = (int)(4.0f * fabsf(sinf(ph * 4.0f)));
            eyeOx = (frameCount % 6 < 3) ? 1 : -1;
            legsAlt = (frameCount % 2 == 0);
            break;
        }
        case ACT_THINKING: {
            bobY = (int)(sinf(ph * 0.4f));
            eyeOy = -1; // looking up
            eyeOx = (int)(sinf(ph * 0.6f));
            legsAlt = false;
            break;
        }
        case ACT_HAPPY: {
            // Bouncing with squash/stretch
            bobY = (int)(5.0f * fabsf(sinf(ph * 3.5f)));
            eyeOx = (frameCount % 8 < 4) ? 1 : -1;
            legsAlt = (frameCount % 2 == 0);
            break;
        }
        case ACT_ERROR: {
            // Swaying dizzy
            bobY = (int)(2 * sinf(ph * 2.0f));
            eyeOx = (int)(2 * sinf(ph * 3.0f));
            legsAlt = (frameCount % 3 == 0);
            break;
        }
        case ACT_SLEEPING: {
            // Melted down, barely moving
            bobY = 3 + (int)(sinf(ph * 0.3f));
            legsAlt = false;
            break;
        }
        case ACT_DISCONNECTED: {
            bobY = (int)(sinf(ph * 0.5f));
            eyeOx = (int)(3 * sinf(ph * 1.5f));
            eyeOy = (int)(sinf(ph * 0.8f));
            legsAlt = ((int)(ph) % 2) == 0;
            break;
        }
    }

    int by = oy + bobY;

    // --- Shadow (row 15) ---
    int shadowW = 9;
    if (currentActivity == ACT_BASH || currentActivity == ACT_MCP) {
        // Shadow shrinks when jumping
        shadowW = 9 - bobY;
        if (shadowW < 5) shadowW = 5;
    }
    int shadowX = ox + (int)((15 - shadowW) / 2.0f * PX);
    canvas.fillRect(shadowX, oy + 15 * PX, shadowW * PX, PX, C_SHADOW);

    // --- Legs (row 13-14) ---
    int legShift = legsAlt ? 1 : 0;
    drawPixelBlock(3, 13 + legShift, 1, 2 - legShift, C_CRAB, ox, by);
    drawPixelBlock(5, 13 + (1-legShift), 1, 2 - (1-legShift), C_CRAB, ox, by);
    drawPixelBlock(9, 13 + legShift, 1, 2 - legShift, C_CRAB, ox, by);
    drawPixelBlock(11, 13 + (1-legShift), 1, 2 - (1-legShift), C_CRAB, ox, by);

    // --- Torso (rows 6-12, cols 2-12) ---
    drawPixelBlock(2, 6, 11, 7, C_CRAB, ox, by);

    // --- Arms (row 9-10) ---
    int armLX = 0;
    int armRX = 13;
    int armY = 9;

    if (currentActivity == ACT_EDITING) {
        // Arms alternate up/down rapidly (typing)
        int armFrame = frameCount % 4;
        int armLOff = (armFrame < 2) ? 0 : -1;
        int armROff = (armFrame < 2) ? -1 : 0;
        drawPixelBlock(armLX, armY + armLOff, 2, 2, C_CRAB, ox, by);
        drawPixelBlock(armRX, armY + armROff, 2, 2, C_CRAB, ox, by);
    } else if (currentActivity == ACT_HAPPY) {
        // Arms waving alternately high
        int wL = (frameCount % 6 < 3) ? -3 : -2;
        int wR = (frameCount % 6 < 3) ? -2 : -3;
        drawPixelBlock(armLX, armY + wL, 2, 2, C_CRAB, ox, by);
        drawPixelBlock(armRX, armY + wR, 2, 2, C_CRAB, ox, by);
    } else if (currentActivity == ACT_ERROR) {
        // Arms drooping down
        drawPixelBlock(armLX, armY + 2, 2, 2, C_CRAB, ox, by);
        drawPixelBlock(armRX, armY + 2, 2, 2, C_CRAB, ox, by);
    } else if (currentActivity == ACT_SLEEPING) {
        // Arms splayed out flat
        drawPixelBlock(armLX - 1, armY + 1, 3, 1, C_CRAB, ox, by);
        drawPixelBlock(armRX, armY + 1, 3, 1, C_CRAB, ox, by);
    } else if (currentActivity == ACT_DISCONNECTED) {
        drawPixelBlock(armLX, armY - 3, 2, 2, C_CRAB, ox, by);
        drawPixelBlock(armRX, armY, 2, 2, C_CRAB, ox, by);
    } else if (currentActivity == ACT_MCP || currentActivity == ACT_BASH) {
        // Arms waving up
        int wave = (frameCount % 4 < 2) ? -2 : -1;
        drawPixelBlock(armLX, armY + wave, 2, 2, C_CRAB, ox, by);
        drawPixelBlock(armRX, armY + wave, 2, 2, C_CRAB, ox, by);
    } else {
        drawPixelBlock(armLX, armY, 2, 2, C_CRAB, ox, by);
        drawPixelBlock(armRX, armY, 2, 2, C_CRAB, ox, by);
    }

    // --- Eyes ---
    if (currentActivity == ACT_ERROR) {
        // X-shaped eyes (dizzy/KO)
        int ex1 = ox + 4 * PX, ex2 = ox + 10 * PX, ey = by + 8 * PX;
        canvas.drawLine(ex1, ey, ex1 + PX, ey + PX*2, C_EYE);
        canvas.drawLine(ex1 + PX, ey, ex1, ey + PX*2, C_EYE);
        canvas.drawLine(ex2, ey, ex2 + PX, ey + PX*2, C_EYE);
        canvas.drawLine(ex2 + PX, ey, ex2, ey + PX*2, C_EYE);
    } else if (currentActivity == ACT_SLEEPING) {
        // Closed eyes — horizontal slits
        canvas.fillRect(ox + 3 * PX, by + 9 * PX + PX/3, PX * 2, PX/4, C_EYE);
        canvas.fillRect(ox + 9 * PX, by + 9 * PX + PX/3, PX * 2, PX/4, C_EYE);
    } else if (currentActivity == ACT_HAPPY) {
        // Happy squint arcs (^ ^)
        int ey = by + 9 * PX;
        for (int i = 0; i < 3; i++) {
            int dy = (i == 1) ? 0 : 1;
            canvas.fillRect(ox + (3 + i) * PX, ey + dy * PX/3, PX, PX/4, C_EYE);
            canvas.fillRect(ox + (9 + i) * PX, ey + dy * PX/3, PX, PX/4, C_EYE);
        }
    } else if (currentActivity == ACT_EDITING) {
        // Squinting eyes (concentrating) — 1x1 instead of 1x2
        drawPixelBlock(4 + eyeOx, 9 + eyeOy, 1, 1, C_EYE, ox, by);
        drawPixelBlock(10 + eyeOx, 9 + eyeOy, 1, 1, C_EYE, ox, by);
    } else if (isBlinking) {
        canvas.fillRect(ox + (4 + eyeOx) * PX, by + 9 * PX, PX, PX/3, C_EYE);
        canvas.fillRect(ox + (10 + eyeOx) * PX, by + 9 * PX, PX, PX/3, C_EYE);
    } else {
        drawPixelBlock(4 + eyeOx, 8 + eyeOy, 1, 2, C_EYE, ox, by);
        drawPixelBlock(10 + eyeOx, 8 + eyeOy, 1, 2, C_EYE, ox, by);
    }

    // --- Mouth & Effects ---
    if (currentActivity == ACT_BASH || currentActivity == ACT_MCP) {
        // Open mouth
        canvas.fillRect(ox + 7 * PX, by + 11 * PX, PX, PX/2, C_EYE);
    } else if (currentActivity == ACT_HAPPY) {
        // Wide smile (curved up)
        canvas.fillRect(ox + 6 * PX, by + 11 * PX, PX * 3, PX/3, C_EYE);
        canvas.fillRect(ox + 5 * PX + PX/2, by + 11 * PX - PX/4, PX/2, PX/3, C_EYE);
        canvas.fillRect(ox + 9 * PX, by + 11 * PX - PX/4, PX/2, PX/3, C_EYE);
        // Gold sparkles
        if (frameCount % 5 == 0) spawnSparkle(0xFFE0);
    } else if (currentActivity == ACT_ERROR) {
        // Wavy distressed mouth
        for (int i = 0; i < 3; i++) {
            int my = (i == 1) ? -1 : 1;
            canvas.fillRect(ox + (6 + i) * PX, by + 11 * PX + my, PX, PX/3, C_EYE);
        }
        // Orbiting stars
        for (int i = 0; i < 3; i++) {
            float a = phase * 2.0f + i * 2.094f;
            int sx = ox + 7 * PX + (int)(5 * PX * cosf(a));
            int sy = by + 4 * PX + (int)(2 * PX * sinf(a));
            canvas.fillRect(sx, sy, PX/3, PX/3, C_FLASH);
            canvas.fillRect(sx - PX/4, sy + PX/6, PX/6, PX/6, C_FLASH);
            canvas.fillRect(sx + PX/3, sy + PX/6, PX/6, PX/6, C_FLASH);
        }
    } else if (currentActivity == ACT_SLEEPING) {
        // Floating Z's
        for (int i = 0; i < 3; i++) {
            float zphase = phase * 0.5f + i * 1.2f;
            float zy = fmodf(zphase, 4.0f);
            if (zy > 3.0f) continue;
            int zx = ox + (12 + i) * PX;
            int zyy = by + (int)((5 - zy * 2) * PX);
            int zs = PX/3 + i * PX/6;
            uint16_t zc = (i == 0) ? C_LABEL : (i == 1) ? C_INACTIVE : 0x4A49;
            canvas.fillRect(zx, zyy, zs, PX/5, zc);
            canvas.drawLine(zx + zs, zyy, zx, zyy + zs, zc);
            canvas.fillRect(zx, zyy + zs, zs, PX/5, zc);
        }
    } else if (currentActivity == ACT_THINKING) {
        // Thinking dots "..."
        int dotPhase = (frameCount / 10) % 4;
        for (int i = 0; i < 3; i++) {
            uint16_t dc = (i < dotPhase) ? C_EYE : C_CRAB;
            canvas.fillRect(ox + (6 + i) * PX + PX/3, by + 11 * PX + PX/3, PX/3, PX/3, dc);
        }
        // Thought bubble
        int bx = ox + 13 * PX;
        int bby = by + 3 * PX + (int)(2 * sinf(phase * 0.8f));
        canvas.fillRoundRect(bx, bby, PX * 3, PX * 2, 3, C_LABEL);
        canvas.fillCircle(bx - 2, bby + PX * 2, 2, C_LABEL);
        canvas.fillCircle(bx - 5, bby + PX * 2 + 4, 1, C_LABEL);
    } else if (currentActivity == ACT_DISCONNECTED) {
        // Wavy mouth
        for (int i = 0; i < 3; i++) {
            int my = (i == 1) ? 0 : 2;
            canvas.fillRect(ox + (6 + i) * PX + PX/4, by + 11 * PX + my, PX/2, PX/3, C_EYE);
        }
        // Floating "?"
        int qx = ox + 14 * PX;
        int qy = by + 4 * PX + (int)(3 * sinf(phase * 1.5f));
        uint16_t qc = ((frameCount / 15) % 2 == 0) ? C_WARN : C_ERR;
        canvas.fillRect(qx, qy, 3*PX/4, PX/4, qc);
        canvas.fillRect(qx + PX/2, qy + PX/4, PX/4, PX/4, qc);
        canvas.fillRect(qx + PX/4, qy + PX/2, PX/4, PX/4, qc);
        canvas.fillRect(qx + PX/4, qy + PX, PX/4, PX/4, qc);
    } else if (currentActivity == ACT_READING) {
        // Reading: small book under crab
        canvas.fillRect(ox + 5 * PX, by + 13 * PX + PX/2, PX * 5, PX/3, C_ACCENT);
        canvas.drawFastVLine(ox + 7 * PX + PX/2, by + 13 * PX + PX/2, PX/3, C_BG);
    }
}

// ============================================================
//  Activity Ring (around circular display edge)
// ============================================================

void drawActivityRing(float ph) {
    if (currentActivity == ACT_IDLE) return;

    uint16_t color = activityColors[currentActivity];
    float speed = 1.5f;
    if (currentActivity == ACT_BASH) speed = 4.0f;
    if (currentActivity == ACT_THINKING) speed = 0.5f;
    if (currentActivity == ACT_EDITING) speed = 3.0f;

    int numDots = 12;
    float arcSpacing = 6.2832f / numDots;

    for (int i = 0; i < numDots; i++) {
        float a = ph * speed + i * arcSpacing;
        int x = CX + (int)(113 * cosf(a));
        int y = CY + (int)(113 * sinf(a));
        // Fade based on position
        bool bright = (i % 3 == 0);
        if (bright) {
            canvas.fillRect(x - 2, y - 2, 5, 5, color);
        } else {
            canvas.fillRect(x - 1, y - 1, 3, 3, color);
        }
    }
}

// ============================================================
//  Usage Ring (inner circle showing event count)
// ============================================================

void drawUsageRing() {
    if (sessionTokens == 0 && sessionStartMs == 0) return;

    float fillRatio = (float)sessionTokens / PLAN_TOKEN_LIMIT;
    if (fillRatio > 1.0f) fillRatio = 1.0f;

    uint16_t ringColor;
    if (fillRatio < 0.5f) ringColor = C_ACCENT;
    else if (fillRatio < 0.75f) ringColor = C_GOOD;
    else if (fillRatio < 0.9f) ringColor = C_WARN;
    else ringColor = C_ERR;

    float startAngle = -1.5708f;
    float endAngle = startAngle + fillRatio * 6.2832f;
    int radius = 100;

    // Background ring (dim track)
    for (float a = startAngle; a < startAngle + 6.2832f; a += 0.05f) {
        int x = CX + (int)(radius * cosf(a));
        int y = CY + (int)(radius * sinf(a));
        canvas.drawPixel(x, y, C_INACTIVE);
    }

    // Filled arc
    for (float a = startAngle; a < endAngle; a += 0.03f) {
        int x = CX + (int)(radius * cosf(a));
        int y = CY + (int)(radius * sinf(a));
        canvas.fillRect(x - 1, y - 1, 3, 3, ringColor);
    }

    // Tick marks at 25%
    for (int i = 0; i < 4; i++) {
        float a = startAngle + i * 1.5708f;
        int x1 = CX + (int)(96 * cosf(a));
        int y1 = CY + (int)(96 * sinf(a));
        int x2 = CX + (int)(104 * cosf(a));
        int y2 = CY + (int)(104 * sinf(a));
        canvas.drawLine(x1, y1, x2, y2, C_INACTIVE);
    }
}

// ============================================================
//  Status text
// ============================================================

void drawStatus() {
    uint16_t actColor = activityColors[currentActivity];

    // Top: countdown timer + token usage
    if (sessionStartMs > 0) {
        unsigned long elapsedSec = (millis() - sessionStartMs) / 1000;
        long remainSec = SESSION_WINDOW_SEC - (long)elapsedSec;
        if (remainSec < 0) remainSec = 0;

        int h = remainSec / 3600;
        int m = (remainSec % 3600) / 60;
        int s = remainSec % 60;

        // Countdown H:MM:SS
        char timeBuf[16];
        snprintf(timeBuf, sizeof(timeBuf), "%d:%02d:%02d", h, m, s);
        canvas.setTextDatum(TC_DATUM);
        uint16_t timeColor = (remainSec < 600) ? C_ERR : (remainSec < 1800) ? C_WARN : C_LABEL;
        canvas.setTextColor(timeColor, C_BG);
        canvas.setTextFont(2);
        canvas.drawString(timeBuf, CX, 15);

        // Token usage below (e.g. "123.4k / 800k")
        char tokBuf[24];
        if (sessionTokens >= 1000) {
            snprintf(tokBuf, sizeof(tokBuf), "%.1fk / %dk",
                sessionTokens / 1000.0f, PLAN_TOKEN_LIMIT / 1000);
        } else {
            snprintf(tokBuf, sizeof(tokBuf), "%lu / %dk",
                sessionTokens, PLAN_TOKEN_LIMIT / 1000);
        }
        canvas.setTextFont(1);
        canvas.setTextColor(C_INACTIVE, C_BG);
        canvas.drawString(tokBuf, CX, 32);
    }

    // Activity name at bottom
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextColor(actColor, C_BG);
    canvas.setTextFont(4);
    canvas.drawString(activityNames[currentActivity], CX, 195);

    // Tool name
    if (lastTool[0] != '\0') {
        canvas.setTextFont(2);
        canvas.setTextColor(C_LABEL, C_BG);
        canvas.drawString(lastTool, CX, 220);
    }

    // WiFi indicator (top-right dot)
    if (wifiConnected) {
        canvas.fillCircle(215, 25, 4, C_GOOD);
    } else {
        canvas.fillCircle(215, 25, 4, C_ERR);
    }
}

// ============================================================
//  JSON Parser
// ============================================================

void setActivity(Activity act) {
    if (act != currentActivity) {
        currentActivity = act;
        // Spawn sparkles on change
        uint16_t c = activityColors[act];
        for (int i = 0; i < 3; i++) spawnSparkle(c);
    }
    lastEventTime = millis();
    eventCount++;
}

Activity toolToActivity(const char* tool) {
    if (!tool) return ACT_IDLE;
    if (strstr(tool, "Read") || strstr(tool, "Grep") || strstr(tool, "Glob") ||
        strstr(tool, "read") || strstr(tool, "grep") || strstr(tool, "glob"))
        return ACT_READING;
    if (strstr(tool, "Edit") || strstr(tool, "Write") || strstr(tool, "edit") || strstr(tool, "write"))
        return ACT_EDITING;
    if (strstr(tool, "Bash") || strstr(tool, "bash") || strstr(tool, "PowerShell") || strstr(tool, "shell"))
        return ACT_BASH;
    if (strstr(tool, "WebSearch") || strstr(tool, "WebFetch") || strstr(tool, "search") || strstr(tool, "fetch"))
        return ACT_SEARCH;
    if (strstr(tool, "mcp") || strstr(tool, "MCP") || strstr(tool, "Agent") || strstr(tool, "agent"))
        return ACT_MCP;
    if (strstr(tool, "Think") || strstr(tool, "think"))
        return ACT_THINKING;
    if (strstr(tool, "Workflow") || strstr(tool, "Skill"))
        return ACT_HAPPY;
    return ACT_IDLE;
}

void processJson(const char* json) {
    StaticJsonDocument<384> doc;
    if (deserializeJson(doc, json)) return;

    // Track session
    const char* sess = doc["session"] | (const char*)nullptr;
    if (sess && strcmp(sess, sessionId) != 0) {
        strlcpy(sessionId, sess, sizeof(sessionId));
        sessionStartMs = millis();
        sessionEvents = 0;
        sessionTokens = 0;
    }
    sessionEvents++;

    // Update token count from hook
    unsigned long tokens = doc["tokens"] | 0UL;
    if (tokens > 0) sessionTokens = tokens;

    const char* tool = doc["tool"] | (const char*)nullptr;
    const char* activity = doc["activity"] | (const char*)nullptr;
    const char* hook = doc["hook"] | (const char*)nullptr;

    if (tool) {
        strlcpy(lastTool, tool, sizeof(lastTool));
        setActivity(toolToActivity(tool));
    } else if (activity) {
        if (strcmp(activity, "idle") == 0) setActivity(ACT_IDLE);
        else if (strcmp(activity, "reading") == 0) setActivity(ACT_READING);
        else if (strcmp(activity, "editing") == 0) setActivity(ACT_EDITING);
        else if (strcmp(activity, "bash") == 0) setActivity(ACT_BASH);
        else if (strcmp(activity, "search") == 0) setActivity(ACT_SEARCH);
        else if (strcmp(activity, "mcp") == 0) setActivity(ACT_MCP);
        else if (strcmp(activity, "thinking") == 0) setActivity(ACT_THINKING);
        else if (strcmp(activity, "happy") == 0) setActivity(ACT_HAPPY);
        else if (strcmp(activity, "error") == 0) setActivity(ACT_ERROR);
        else if (strcmp(activity, "sleep") == 0) setActivity(ACT_SLEEPING);
    } else if (hook) {
        const char* toolName = doc["tool_name"] | (const char*)nullptr;
        if (strcmp(hook, "PreToolUse") == 0 || strcmp(hook, "PostToolUse") == 0) {
            if (toolName) {
                strlcpy(lastTool, toolName, sizeof(lastTool));
                setActivity(toolToActivity(toolName));
            }
        } else if (strcmp(hook, "Stop") == 0) {
            setActivity(ACT_IDLE);
            lastTool[0] = '\0';
        }
    }
    Serial.println("OK");
}

void readSerial() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialPos > 0) {
                serialBuf[serialPos] = '\0';
                processJson(serialBuf);
                serialPos = 0;
            }
        } else if (serialPos < (int)sizeof(serialBuf) - 1) {
            serialBuf[serialPos++] = c;
        }
    }
}

// ============================================================
//  WiFi & HTTP Server
// ============================================================

void handleEvent() {
    if (server.hasArg("plain")) {
        String body = server.arg("plain");
        processJson(body.c_str());
        server.send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
        server.send(400, "application/json", "{\"error\":\"no body\"}");
    }
}

void handleStatus() {
    long remainSec = 0;
    if (sessionStartMs > 0) {
        unsigned long elapsed = (millis() - sessionStartMs) / 1000;
        remainSec = SESSION_WINDOW_SEC - (long)elapsed;
        if (remainSec < 0) remainSec = 0;
    }
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"activity\":\"%s\",\"events\":%d,\"tokens\":%lu,\"remain\":%ld,\"uptime\":%lu}",
        activityNames[currentActivity], eventCount, sessionTokens, remainSec, millis() / 1000);
    server.send(200, "application/json", buf);
}

void handleRoot() {
    server.send(200, "text/html",
        "<h2>Clawd Tank</h2>"
        "<p>POST JSON to <code>/event</code></p>"
        "<p>Example: <code>{\"tool\":\"Bash\"}</code></p>"
        "<p><a href='/status'>Status</a></p>");
}

void startHttpServer() {
    server.on("/", handleRoot);
    server.on("/event", HTTP_POST, handleEvent);
    server.on("/status", HTTP_GET, handleStatus);
    server.begin();
    Serial.println("HTTP server started on port 80");
}

void setupWiFi() {
    WiFi.mode(WIFI_STA);

    wifiMulti.addAP(WIFI_CRED_1_SSID, WIFI_CRED_1_PASS);
    wifiMulti.addAP(WIFI_CRED_2_SSID, WIFI_CRED_2_PASS);

    Serial.print("WiFi: scanning networks");
    int attempts = 0;
    while (wifiMulti.run() != WL_CONNECTED && attempts < 40) {
        delay(250);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.printf("\nWiFi: connected to %s IP: %s\n",
            WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        startHttpServer();
    } else {
        Serial.println("\nWiFi: connection failed");
        currentActivity = ACT_DISCONNECTED;
    }
}

// ============================================================
//  Setup & Loop
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(300);

    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);

    pinMode(3, OUTPUT);
    digitalWrite(3, HIGH);

    initColors();
    canvas.createSprite(SW, SH);
    memset(sparkles, 0, sizeof(sparkles));

    // Splash — draw a big Clawd
    canvas.fillSprite(C_BG);
    drawClawd((SW - 15 * PX) / 2, (SH - 16 * PX) / 2 - 15, 0);
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextColor(C_ACCENT, C_BG);
    canvas.setTextFont(4);
    canvas.drawString("CLAWD", CX, 195);
    canvas.setTextFont(2);
    canvas.setTextColor(C_LABEL, C_BG);
    canvas.drawString("TANK", CX, 220);
    canvas.pushSprite(0, 0);

    setupWiFi();

    // Show IP on splash if connected
    if (wifiConnected) {
        canvas.setTextColor(C_GOOD, C_BG);
        canvas.setTextFont(2);
        canvas.drawString(WiFi.SSID().c_str(), CX, 5);
        canvas.setTextFont(1);
        canvas.drawString(WiFi.localIP().toString().c_str(), CX, 20);
        canvas.pushSprite(0, 0);
    }
    delay(2000);

    Serial.println("{\"device\":\"clawd_tank\",\"status\":\"ready\",\"wifi\":" +
                   String(wifiConnected ? "true" : "false") + "}");
}

void loop() {
    readSerial();
    if (wifiConnected) server.handleClient();

    // Auto-idle → sleep progression
    if (lastEventTime > 0 && currentActivity != ACT_DISCONNECTED) {
        unsigned long idle = millis() - lastEventTime;
        if (idle > 120000 && currentActivity != ACT_SLEEPING) {
            currentActivity = ACT_SLEEPING;
            lastTool[0] = '\0';
        } else if (idle > 15000 && currentActivity != ACT_IDLE && currentActivity != ACT_SLEEPING) {
            currentActivity = ACT_IDLE;
            lastTool[0] = '\0';
        }
    }

    // WiFi reconnect every ~30s if disconnected
    if (!wifiConnected && frameCount % 900 == 0) {
        if (wifiMulti.run() == WL_CONNECTED) {
            wifiConnected = true;
            startHttpServer();
            currentActivity = ACT_IDLE;
            Serial.printf("WiFi: reconnected to %s\n", WiFi.SSID().c_str());
        }
    }

    phase += 0.06f;
    if (phase > 6.2832f) phase -= 6.2832f;
    frameCount++;

    // Blink logic
    blinkCountdown--;
    if (blinkCountdown <= 0 && !isBlinking) {
        isBlinking = true;
        blinkFrame = 4;
    }
    if (isBlinking) {
        blinkFrame--;
        if (blinkFrame <= 0) {
            isBlinking = false;
            blinkCountdown = random(60, 160);
        }
    }

    // --- Draw ---
    canvas.fillSprite(C_BG);

    drawUsageRing();
    drawActivityRing(phase);
    drawSparkles();

    // Center the 15x16 pixel crab on screen
    int crabOx = (SW - 15 * PX) / 2;
    int crabOy = (SH - 16 * PX) / 2 - 20;
    drawClawd(crabOx, crabOy, phase);

    drawStatus();

    canvas.pushSprite(0, 0);

    // Battery keepalive: briefly pulse backlight to spike current draw
    if (millis() - lastKeepalive > KEEPALIVE_INTERVAL) {
        lastKeepalive = millis();
        for (int i = 0; i < 3; i++) {
            digitalWrite(3, LOW);
            delay(1);
            digitalWrite(3, HIGH);
            delay(1);
        }
    }

    delay(33);
}
