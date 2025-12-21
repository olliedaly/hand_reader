#include <Arduino.h>
#include <M5Unified.h>
#include <M5GFX.h>
#include <vector>
#include <LittleFS.h>
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
            Serial.println("Task: Open Success. Loading Ch 0.");
            currentChapterIndex = 0;
            currentTextBuffer = reader.getChapterContent(0);
            recalculatePages(); 
        }
        
    } else if (currentOp == OP_LOAD_CHAPTER) {
        Serial.printf("Task: Loading Chapter %d\n", targetLoadChapterIndex);
        currentChapterIndex = targetLoadChapterIndex;
        currentTextBuffer = reader.getChapterContent(currentChapterIndex);
        recalculatePages();
        operationSuccess = true; 
    }

    if (operationSuccess) {
        textScrollOffset = 0;
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
    // Top 1/4 screen
    int h = M5.Display.height() / 4;
    M5.Display.fillRect(0, 0, M5.Display.width(), h, TFT_LIGHTGREY);
    M5.Display.drawRect(0, 0, M5.Display.width(), h, TFT_BLACK);
    
    M5.Display.setTextColor(TFT_BLACK, TFT_LIGHTGREY);
    M5.Display.setTextSize(2);
    
    // Battery
    int bat = M5.Power.getBatteryLevel();
    M5.Display.drawRightString(String(bat) + "%", M5.Display.width() - 10, 10, &fonts::FreeSansBold9pt7b);

    // Buttons (Simple Text for now)
    // Left: HOME
    M5.Display.drawCenterString("[ HOME ]", M5.Display.width() * 0.2, h/2 - 10, &fonts::FreeSansBold9pt7b);
    
    // Right: SIZE
    M5.Display.drawCenterString("[ SIZE ]", M5.Display.width() * 0.8, h/2 - 10, &fonts::FreeSansBold9pt7b);
    
    M5.Display.drawCenterString("MENU", M5.Display.width() * 0.5, 10, &fonts::FreeSansBold9pt7b);
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
                    // Down/Next File
                    currentFileIndex++;
                    if (currentFileIndex >= epubFiles.size()) currentFileIndex = 0;
                    drawHome();
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
                            targetLoadChapterIndex = currentChapterIndex + 1;
                            startAsyncOp(OP_LOAD_CHAPTER);
                        } else {
                            textScrollOffset--; // End of book
                        }
                    } else {
                        textRedrawNeeded = true;
                    }
                } else if (t.x < width * 0.25) {
                    // PREV PAGE
                    textScrollOffset--;
                    if (textScrollOffset < 0) {
                        if (currentChapterIndex > 0) {
                            targetLoadChapterIndex = currentChapterIndex - 1;
                             startAsyncOp(OP_LOAD_CHAPTER);
                        } else {
                            textScrollOffset = 0;
                        }
                    } else {
                         textRedrawNeeded = true;
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
                // Top 1/4 is menu.
                int h = height / 4;
                if (t.y > h) {
                    // Click outside -> Close Menu
                    currentState = STATE_READING;
                    textRedrawNeeded = true; // Redraw reader
                } else {
                    // Inside Menu
                    // Left (Home)
                    if (t.x < width * 0.4) {
                        reader.close();
                        currentState = STATE_HOME;
                        drawHome();
                    }
                    // Right (Size)
                    else if (t.x > width * 0.6) {
                        // Toggle Size
                        if (currentTextSize <= 3.0) currentTextSize = 4.0;
                        else if (currentTextSize == 4.0) currentTextSize = 6.0;
                        else currentTextSize = 3.0;
                        
                        // Repaginate
                        M5.Display.fillScreen(COLOR_BG);
                        M5.Display.drawCenterString("Resizing...", width/2, height/2, &fonts::FreeSansBold9pt7b);
                        recalculatePages();
                        
                        currentState = STATE_READING;
                        textRedrawNeeded = true;
                    }
                }
            }
        }
    }
    
    delay(10);
}