#include <Arduino.h>
#include <M5Unified.h>
#include <M5GFX.h>
#include <vector>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "EpubReader.h"
#include "Paginator.h"


// --- Constants ---
#define COLOR_BG TFT_WHITE
#define COLOR_TEXT TFT_BLACK

// --- Globals ---
EpubReader reader;
std::vector<String> epubFiles;
int currentFileIndex = 0;
int currentChapterIndex = 0;

// State Machine
enum AppState {
    STATE_HOME,
    STATE_LOADING,
    STATE_READING,
    STATE_MENU,
    STATE_SKIP_PAGE,
    STATE_ERROR
};


AppState currentState = STATE_HOME;

// Text Buffer & Pagination
String currentTextBuffer = "";
std::vector<PageInfo> currentPages;
int textScrollOffset = 0; 
bool textRedrawNeeded = false;
float currentTextSize = 4.0; // Default Size (Medium)

// Async Task Globals
enum AsyncOp { OP_OPEN, OP_LOAD_CHAPTER };
AsyncOp currentOp;
String targetOpenFile = "";
int targetLoadChapterIndex = -1;
volatile bool operationSuccess = false;
volatile bool operationComplete = false;

// Helpers
void saveBookmark() {
    if (epubFiles.empty() || currentFileIndex >= epubFiles.size()) return;
    
    String filename = epubFiles[currentFileIndex];
    JsonDocument doc;
    
    File f = LittleFS.open("/bookmarks.json", "r");
    if (f) {
        deserializeJson(doc, f);
        f.close();
    }
    
    doc[filename]["chapter"] = currentChapterIndex;
    doc[filename]["page"] = textScrollOffset;
    doc[filename]["size"] = currentTextSize;
    
    f = LittleFS.open("/bookmarks.json", "w");
    if (f) {
        serializeJson(doc, f);
        f.close();
        Serial.printf("DEBUG: Save Bookmark [%s] -> Ch:%d, Pg:%d, Sz:%.1f\n", filename.c_str(), currentChapterIndex, textScrollOffset, currentTextSize);
    } else {
        Serial.println("DEBUG: Failed to open bookmarks.json for writing!");
    }
}


void loadBookmark(String filename, int& chapter, int& page, float& size) {
    File f = LittleFS.open("/bookmarks.json", "r");
    if (!f) {
        Serial.println("DEBUG: No bookmarks.json found");
        return;
    }
    
    JsonDocument doc;
    deserializeJson(doc, f);
    f.close();
    
    if (doc.containsKey(filename)) {
        chapter = doc[filename]["chapter"] | 0;
        page = doc[filename]["page"] | 0;
        size = doc[filename]["size"] | 4.0;
        Serial.printf("DEBUG: Load Bookmark [%s] -> Ch:%d, Pg:%d, Sz:%.1f\n", filename.c_str(), chapter, page, size);
    } else {
        Serial.printf("DEBUG: No bookmark for [%s]\n", filename.c_str());
    }
}


void recalculatePages() {

    int margin = 10;
    int w = M5.Display.width() - (margin * 2);
    int h = M5.Display.height() - 60; // Space for header
    currentPages = Paginator::paginate(currentTextBuffer, 0, 0, w, h, currentTextSize);
}

// Unified Task for heavy stack operations
void asyncLoaderTask(void * parameter) {
    Serial.println(">>> asyncLoaderTask: Started");
    operationSuccess = false;
    
    if (currentOp == OP_OPEN) {
        // 1. Try normal path
        Serial.printf("Task: Opening %s\n", targetOpenFile.c_str());
        if (reader.open(targetOpenFile.c_str())) {
            operationSuccess = true;
        } else {
            // 2. Try prefix
            String alt = "/littlefs/" + targetOpenFile;
            if (targetOpenFile.startsWith("/")) alt = "/littlefs" + targetOpenFile;
            Serial.printf("Task: Retrying %s\n", alt.c_str());
            if (reader.open(alt.c_str())) {
                operationSuccess = true;
            }
        }
        
        if (operationSuccess) {
            Serial.println("Task: Open Success. Checking Bookmark.");
            
            // Load bookmark
            int savedCh = 0;
            int savedPg = 0;
            float savedSize = currentTextSize;
            loadBookmark(epubFiles[currentFileIndex], savedCh, savedPg, savedSize);
            
            currentChapterIndex = savedCh;
            currentTextSize = savedSize;
            
            Serial.printf("Task: Loading Ch %d from Bookmark\n", currentChapterIndex);
            currentTextBuffer = reader.getChapterContent(currentChapterIndex);
            recalculatePages(); 
            textScrollOffset = savedPg;
            Serial.printf("Task: Repaginated. Total Pages: %d, Restoring Pg: %d\n", currentPages.size(), textScrollOffset);
            if (textScrollOffset >= currentPages.size()) {
                Serial.println("Task: Restored Page out of bounds, resetting to 0");
                textScrollOffset = 0;
            }
        }


        
    } else if (currentOp == OP_LOAD_CHAPTER) {
        currentChapterIndex = targetLoadChapterIndex;
        currentTextBuffer = reader.getChapterContent(currentChapterIndex);
        recalculatePages();
        textScrollOffset = 0; // Reset to start of new chapter
        operationSuccess = true; 
    }


    if (operationSuccess) {
        textRedrawNeeded = true;
    } else {

        Serial.println("Task: Operation Failed.");
    }
    
    operationComplete = true;
    Serial.println(">>> asyncLoaderTask: Done.");
    vTaskDelete(NULL);
}

void startAsyncOp(AsyncOp op) {
    currentOp = op;
    operationComplete = false;
    operationSuccess = false;
    currentState = STATE_LOADING;
    
    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setCursor(M5.Display.width()/2, M5.Display.height()/2);
    M5.Display.setTextSize(3);
    if (op == OP_OPEN) M5.Display.drawCenterString("Opening...", M5.Display.width()/2, M5.Display.height()/2, &fonts::FreeSansBold9pt7b);
    else M5.Display.drawCenterString("Loading...", M5.Display.width()/2, M5.Display.height()/2, &fonts::FreeSansBold9pt7b);

    xTaskCreate(asyncLoaderTask, "Loader", 65536, NULL, 1, NULL);
}

// --- Helper Functions ---

void listEpubFiles(File dir, std::vector<String>& list) {
    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        if (!entry.isDirectory()) {
            String fname = String(entry.name());
            if (fname.endsWith(".epub") || fname.endsWith(".EPUB")) {
                list.push_back(fname); 
            }
        }
        entry.close();
    }
}

void drawHome() {
    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(COLOR_TEXT, COLOR_BG);
    M5.Display.setCursor(10, 10);
    M5.Display.print("Library");
    
    // Battery Status
    int bat = M5.Power.getBatteryLevel();
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_DARKGRAY, COLOR_BG);
    M5.Display.drawRightString(String(bat) + "%", M5.Display.width() - 10, 12, &fonts::FreeSansBold9pt7b);

    M5.Display.drawFastHLine(0, 42, M5.Display.width(), TFT_BLACK);

    int y = 50;
    if (epubFiles.empty()) {
        M5.Display.setCursor(10, y);
        M5.Display.println("No .epub files found!");
        M5.Display.setTextSize(2);
        M5.Display.setCursor(10, y + 40);
        M5.Display.println("Please upload files to LittleFS:");
        M5.Display.println("1. Put .epub in 'data'");
        M5.Display.println("2. pio run -t uploadfs");
        return;
    }

    for (int i = 0; i < epubFiles.size(); i++) {
        if (i == currentFileIndex) {
            M5.Display.fillRect(0, y, M5.Display.width(), 40, TFT_BLACK);
            M5.Display.setTextColor(TFT_WHITE, TFT_BLACK); // Inverted
        } else {
            M5.Display.setTextColor(COLOR_TEXT, COLOR_BG);
        }
        M5.Display.setCursor(10, y + 5);
        M5.Display.printf("%d. %s", i + 1, epubFiles[i].c_str());
        
        y += 45;
        if (y > M5.Display.height() - 40) break; 
    }
    
    // Instructions
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_DARKGRAY, COLOR_BG);
    M5.Display.drawCenterString("UP/DN | SELECT", M5.Display.width()/2, M5.Display.height()-30, &fonts::FreeSansBold9pt7b);
    
    // Power Button
    M5.Display.drawRightString("[ POWER OFF ]", M5.Display.width() - 10, M5.Display.height() - 30, &fonts::FreeSansBold9pt7b);
}


void drawReader() {
    if (!textRedrawNeeded) return;
    
    // Check page validity
    if (currentTextBuffer.length() == 0) {
        M5.Display.fillScreen(COLOR_BG);
        M5.Display.setCursor(10, 40);
        M5.Display.setTextColor(COLOR_TEXT, COLOR_BG);
        M5.Display.println("(Empty Chapter Content)");
        textRedrawNeeded = false;
        return;
    }
    
    if (textScrollOffset >= currentPages.size()) {
       textScrollOffset = currentPages.size() - 1;
       if (textScrollOffset < 0) textScrollOffset = 0;
    }
    
    M5.Display.fillScreen(COLOR_BG);
    
    // Header
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_BLUE, COLOR_BG);
    M5.Display.setCursor(5, 5);
    // Page X of Y
    M5.Display.printf("Ch %d | Pg %d/%d", currentChapterIndex + 1, textScrollOffset + 1, currentPages.size());
    
    // Draw Text using Paginator
    if (currentPages.size() > 0) {
        PageInfo p = currentPages[textScrollOffset];
        int margin = 10;
        int w = M5.Display.width() - (margin * 2);
        int h = M5.Display.height() - 60;
        
        Paginator::drawPage(currentTextBuffer, p.start, p.length, margin, 40, w, h, currentTextSize, COLOR_TEXT);
    }
    
    textRedrawNeeded = false;
}

void drawMenu() {
    // Overlay menu
    // Top 1/3 screen for more buttons
    int h = M5.Display.height() / 3;
    M5.Display.fillRect(0, 0, M5.Display.width(), h, TFT_LIGHTGREY);
    M5.Display.drawRect(0, 0, M5.Display.width(), h, TFT_BLACK);
    
    M5.Display.setTextColor(TFT_BLACK, TFT_LIGHTGREY);
    M5.Display.setTextSize(2);
    
    // Battery
    int bat = M5.Power.getBatteryLevel();
    M5.Display.drawRightString(String(bat) + "%", M5.Display.width() - 10, 10, &fonts::FreeSansBold9pt7b);

    // Buttons
    // Left: HOME
    M5.Display.drawCenterString("[ HOME ]", M5.Display.width() * 0.15, 60, &fonts::FreeSansBold9pt7b);
    
    // PAGE
    M5.Display.drawCenterString("[ PAGE ]", M5.Display.width() * 0.38, 60, &fonts::FreeSansBold9pt7b);
    
    // SIZE
    M5.Display.drawCenterString("[ SIZE ]", M5.Display.width() * 0.62, 60, &fonts::FreeSansBold9pt7b);

    // Power
    M5.Display.drawCenterString("[ OFF ]", M5.Display.width() * 0.85, 60, &fonts::FreeSansBold9pt7b);
    
    M5.Display.drawCenterString("MENU", M5.Display.width() * 0.5, 10, &fonts::FreeSansBold9pt7b);
}


void drawSkipPage() {
    int h = M5.Display.height() / 3;
    M5.Display.fillRect(0, 0, M5.Display.width(), h, TFT_WHITE);
    M5.Display.drawRect(0, 0, M5.Display.width(), h, TFT_BLACK);
    
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.drawCenterString("SKIP PAGE", M5.Display.width() * 0.5, 10, &fonts::FreeSansBold9pt7b);
    
    M5.Display.setTextSize(3);
    M5.Display.drawCenterString("Pg: " + String(textScrollOffset + 1), M5.Display.width() * 0.5, 50, &fonts::FreeSansBold9pt7b);
    
    M5.Display.setTextSize(2);
    M5.Display.drawCenterString("[ -10 ]", M5.Display.width() * 0.2, 110, &fonts::FreeSansBold9pt7b);
    M5.Display.drawCenterString("[ -1 ]", M5.Display.width() * 0.4, 110, &fonts::FreeSansBold9pt7b);
    M5.Display.drawCenterString("[ +1 ]", M5.Display.width() * 0.6, 110, &fonts::FreeSansBold9pt7b);
    M5.Display.drawCenterString("[ +10 ]", M5.Display.width() * 0.8, 110, &fonts::FreeSansBold9pt7b);
    
    M5.Display.drawCenterString("TAP OUTSIDE TO CLOSE", M5.Display.width() * 0.5, 160, &fonts::FreeSansBold9pt7b);
}


// --- Setup & Loop ---

void setup() {
    Serial.begin(115200);
    
    auto cfg = M5.config();
    cfg.clear_display = true;
    M5.begin(cfg);
    M5.Display.setRotation(0); 
    M5.Display.setTextSize(3); 

    // Initialize LittleFS
    M5.Display.println("Mounting LittleFS...");
    if (!LittleFS.begin(true)) { 
        M5.Display.println("LittleFS Mount Failed!");
        Serial.println("LittleFS Mount Failed");
        delay(2000);
    } else {
        M5.Display.println("LittleFS Mounted");
        Serial.println("LittleFS Mounted");
        delay(500);
    }

    File root = LittleFS.open("/");
    if (!root) {
        M5.Display.println("Failed to open root!");
    } else {
        listEpubFiles(root, epubFiles);
        root.close();
    }

    drawHome();
}

void loop() {
    M5.update();

    int width = M5.Display.width();
    int height = M5.Display.height();

    // Logic Dispatch
    if (currentState == STATE_LOADING) {
        // Spin while waiting for task
        if (operationComplete) {
            if (operationSuccess) {
                currentState = STATE_READING;
                drawReader();
            } else {
                M5.Display.fillScreen(COLOR_BG);
                M5.Display.setCursor(10, height/2);
                M5.Display.setTextColor(TFT_RED, COLOR_BG);
                M5.Display.println("Op Failed!");
                delay(2000);
                currentState = STATE_HOME; // Fallback to home on error
                drawHome();
            }
        }
        delay(100);
        return;
    }

    if (currentState == STATE_HOME) {
        if (M5.Touch.getCount() > 0) {
            auto t = M5.Touch.getDetail();
            if (t.wasPressed()) {
                if (t.y < height / 3) {
                    // Up/Previous File
                    currentFileIndex--;
                    if (currentFileIndex < 0) currentFileIndex = epubFiles.size() - 1;
                    drawHome();
                } else if (t.y > (height * 2) / 3) {
                    // Down or Power?
                    // Bottom Right region for Power
                    if (t.x > width * 0.6) {
                        M5.Display.fillScreen(COLOR_BG);
                        M5.Display.drawCenterString("Powering Off...", width/2, height/2, &fonts::FreeSansBold9pt7b);
                        delay(1000);
                        M5.Power.powerOff();
                    } else {
                        // Down/Next File
                        currentFileIndex++;
                        if (currentFileIndex >= epubFiles.size()) currentFileIndex = 0;
                        drawHome();
                    }
                } else {

                    // Select (Center)
                    if (epubFiles.size() > 0) {
                        targetOpenFile = "/" + epubFiles[currentFileIndex];
                        startAsyncOp(OP_OPEN);
                    }
                }
            }
        }
    } 
    else if (currentState == STATE_READING) {
        if (textRedrawNeeded) {
            drawReader();
        }
        
        if (M5.Touch.getCount() > 0) {
            auto t = M5.Touch.getDetail();
            if (t.wasPressed()) {
                // New Logic:
                // Left 25% -> PREV
                // Right 25% -> NEXT
                // Center 50% -> MENU
                
                if (t.x > width * 0.75) {
                    // NEXT PAGE
                    textScrollOffset++;
                    if (textScrollOffset >= currentPages.size()) {
                        // Next Chapter
                         if (currentChapterIndex < reader.getChapters().size() - 1) {
                            saveBookmark();
                            targetLoadChapterIndex = currentChapterIndex + 1;
                            startAsyncOp(OP_LOAD_CHAPTER);
                        } else {
                            textScrollOffset--; // End of book
                        }
                    } else {
                        textRedrawNeeded = true;
                        if (textScrollOffset % 5 == 0) saveBookmark(); // Periodic save
                    }
                } else if (t.x < width * 0.25) {
                    // PREV PAGE
                    textScrollOffset--;
                    if (textScrollOffset < 0) {
                        if (currentChapterIndex > 0) {
                            saveBookmark();
                            targetLoadChapterIndex = currentChapterIndex - 1;
                             startAsyncOp(OP_LOAD_CHAPTER);
                        } else {
                            textScrollOffset = 0;
                        }
                    } else {
                         textRedrawNeeded = true;
                         if (textScrollOffset % 5 == 0) saveBookmark();
                    }

                } else {
                    // CENTER -> OPEN MENU
                    currentState = STATE_MENU;
                    drawMenu();
                }
            }
        }
    }
    else if (currentState == STATE_MENU) {
        if (M5.Touch.getCount() > 0) {
            auto t = M5.Touch.getDetail();
            if (t.wasPressed()) {
                // Top 1/3 is menu.
                int h = height / 3;
                if (t.y > h) {
                    // Click outside -> Close Menu
                    currentState = STATE_READING;
                    textRedrawNeeded = true; // Redraw reader
                } else {
                    // Inside Menu
                    // Left (Home)
                    if (t.x < width * 0.25) {
                        saveBookmark();
                        reader.close();
                        currentState = STATE_HOME;
                        drawHome();
                    }
                    // Page Skip
                    else if (t.x < width * 0.5) {
                        currentState = STATE_SKIP_PAGE;
                        drawSkipPage();
                    }
                    // Size
                    else if (t.x < width * 0.75) {
                        // Toggle Size
                        if (currentTextSize <= 3.0) currentTextSize = 4.0;
                        else if (currentTextSize == 4.0) currentTextSize = 6.0;
                        else currentTextSize = 3.0;
                        
                        // Repaginate
                        M5.Display.fillScreen(COLOR_BG);
                        M5.Display.drawCenterString("Resizing...", width/2, height/2, &fonts::FreeSansBold9pt7b);
                        recalculatePages();
                        saveBookmark();
                        
                        currentState = STATE_READING;
                        textRedrawNeeded = true;
                    }
                    // Power Off
                    else {
                        saveBookmark();
                        M5.Display.fillScreen(COLOR_BG);
                        M5.Display.drawCenterString("Powering Off...", width/2, height/2, &fonts::FreeSansBold9pt7b);
                        delay(1000);
                        M5.Power.powerOff();
                    }
                }

            }
        }
    }
    else if (currentState == STATE_SKIP_PAGE) {
        if (M5.Touch.getCount() > 0) {
            auto t = M5.Touch.getDetail();
            if (t.wasPressed()) {
                int h = height / 3;
                if (t.y > h) {
                    currentState = STATE_READING;
                    textRedrawNeeded = true;
                } else {
                    // Inside skip menu
                    if (t.y > 100 && t.y < 150) {
                        if (t.x < width * 0.3) textScrollOffset -= 10;
                        else if (t.x < width * 0.5) textScrollOffset -= 1;
                        else if (t.x < width * 0.7) textScrollOffset += 1;
                        else textScrollOffset += 10;
                        
                        // Bounds check
                        if (textScrollOffset < 0) textScrollOffset = 0;
                        if (textScrollOffset >= currentPages.size()) textScrollOffset = currentPages.size() - 1;
                        
                        drawSkipPage();
                    }
                }
            }
        }
    }

    
    delay(10);
}