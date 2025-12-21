#include <Arduino.h>
#include <M5Unified.h>
#include <M5GFX.h>
#include <vector>
#include <LittleFS.h>
#include "EpubReader.h"

// --- Constants ---
#define COLOR_BG TFT_WHITE
#define COLOR_TEXT TFT_BLACK
#define FONT_SIZE_DEFAULT 2

// --- Globals ---
EpubReader reader;
std::vector<String> epubFiles;
int currentFileIndex = 0;
int currentChapterIndex = 0;
int currentPageInChapter = 0; 

// State Machine
enum AppState {
    STATE_HOME,
    STATE_LOADING,
    STATE_READING,
    STATE_ERROR
};

AppState currentState = STATE_HOME;

// Text Buffer
String currentTextBuffer = "";
int textScrollOffset = 0;
bool textRedrawNeeded = false;

// Async Task Globals
enum AsyncOp { OP_OPEN, OP_LOAD_CHAPTER };
AsyncOp currentOp;
String targetOpenFile = "";
int targetLoadChapterIndex = -1;
volatile bool operationSuccess = false;
volatile bool operationComplete = false;

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
        }
        
    } else if (currentOp == OP_LOAD_CHAPTER) {
        Serial.printf("Task: Loading Chapter %d\n", targetLoadChapterIndex);
        currentChapterIndex = targetLoadChapterIndex;
        currentTextBuffer = reader.getChapterContent(currentChapterIndex);
        operationSuccess = true; // minimal error checking on getChapterContent for now
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
    M5.Display.println("Library");
    M5.Display.drawFastHLine(0, 40, M5.Display.width(), TFT_BLACK);

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
    
    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setTextSize(3); // Match Setup
    M5.Display.setTextColor(COLOR_TEXT, COLOR_BG);
    M5.Display.setCursor(10, 40); // Move down a bit for header
    
    int charsPerPage = 400; 
    int startChar = textScrollOffset * charsPerPage;
    if (startChar >= currentTextBuffer.length()) startChar = currentTextBuffer.length();
    
    // Safety check
    if (currentTextBuffer.length() == 0) {
        M5.Display.println("(Empty Chapter Content)");
    } else {
        String pageText = currentTextBuffer.substring(startChar, startChar + charsPerPage);
        M5.Display.println(pageText);
    }
    
    // Draw Header
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_BLUE, COLOR_BG);
    M5.Display.setCursor(5, 5);
    M5.Display.printf("Ch %d | Pg %d", currentChapterIndex + 1, textScrollOffset + 1);
    
    textRedrawNeeded = false;
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
                Serial.printf("Reader Touch: x=%d, y=%d (w=%d, h=%d)\n", t.x, t.y, width, height);
                
                if (t.x > width * 0.66) {
                    Serial.println("Action: NEXT PAGE");
                    // Next Page
                    int charsPerPage = 400; 
                    int maxPages = (currentTextBuffer.length() / charsPerPage) + 1;
                     textScrollOffset++;
                    if (textScrollOffset >= maxPages) {
                        // Next Chapter
                        Serial.printf("Action: NEXT CH (Current: %d, Total: %d)\n", currentChapterIndex, reader.getChapters().size());
                        if (currentChapterIndex < reader.getChapters().size() - 1) {
                            targetLoadChapterIndex = currentChapterIndex + 1;
                            startAsyncOp(OP_LOAD_CHAPTER);
                        } else {
                            textScrollOffset--;
                        }
                    } else {
                        textRedrawNeeded = true;
                    }
                } else if (t.x < width * 0.33) {
                    Serial.println("Action: PREV PAGE");
                    // Prev Page
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
                    Serial.println("Action: HOME/MENU");
                    // Center -> Back to Menu
                    reader.close();
                    currentState = STATE_HOME;
                    drawHome();
                }
            }
        }
    }
    
    delay(10);
}