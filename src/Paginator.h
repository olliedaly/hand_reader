#ifndef PAGINATOR_H
#define PAGINATOR_H

#include <Arduino.h>
#include <M5Unified.h>
#include <vector>

struct PageInfo {
    int start;
    int length;
};

class Paginator {
public:
    // Splits text into pages based on dimensions and font size
    static std::vector<PageInfo> paginate(const String& text, int x, int y, int width, int height, float textSize) {
        std::vector<PageInfo> pages;
        if (text.length() == 0) return pages;

        M5.Display.setTextSize(textSize);
        int spaceWidth = M5.Display.textWidth(" ");
        int lineHeight = M5.Display.fontHeight();
        
        int cursorX = 0;
        int cursorY = 0;
        
        int pageStart = 0;
        int i = 0;
        int len = text.length();
        
        while (i < len) {
            // Check for explicit newline
            if (text[i] == '\n') {
                cursorX = 0;
                cursorY += lineHeight;
                i++;
                
                // Page Break Check
                if (cursorY + lineHeight > height) {
                    pages.push_back({pageStart, i - pageStart});
                    pageStart = i;
                    cursorY = 0;
                    cursorX = 0;
                }
                continue;
            }
            
            // Identify word
            int wordStart = i;
            int wordEnd = i;
            while (wordEnd < len && text[wordEnd] != ' ' && text[wordEnd] != '\n') {
                wordEnd++;
            }
            
            // Extract word
            String word = text.substring(wordStart, wordEnd);
            int wordWidth = M5.Display.textWidth(word);
            
            // Logic: Does word fit on current line?
            bool wordFit = (cursorX + wordWidth <= width);
            
            if (!wordFit) {
                // If cursor is not at start of line, wrap to next line
                if (cursorX > 0) {
                    cursorX = 0;
                    cursorY += lineHeight;
                    
                    // Page Break Check on wrap
                    if (cursorY + lineHeight > height) {
                         pages.push_back({pageStart, wordStart - pageStart});
                         pageStart = wordStart;
                         cursorY = 0;
                         cursorX = 0;
                    }
                }
                // If word is longer than entire width, we force it to print (char wrap implicitly by drawer? 
                // or we accept it overflows X. Better to overflow X than break logic).
                // For this simplistic engine, we assume words are < screen width.
            }
            
            // Add word width
            cursorX += wordWidth;
            
            // Handle trailing space
            if (wordEnd < len && text[wordEnd] == ' ') {
                cursorX += spaceWidth;
                wordEnd++; // Consume space
            }
            
            // Update iterator
            i = wordEnd;
            
             // Double check width overflow (if we just added a word that pushed exactly to edge, fine. If over?)
             if (cursorX > width) {
                 // Should have wrapped. The logic above handles wrapping BEFORE printing.
                 // So here we are just updating X.
             }
        }
        
        // Final page
        if (pageStart < len) {
            pages.push_back({pageStart, len - pageStart});
        }
        
        return pages;
    }

    // Draws a specific page content using the SAME logic
    static void drawPage(const String& text, int startIndex, int length, int x, int y, int width, int height, float textSize, uint32_t color) {
        if (startIndex >= text.length()) return;
        
        M5.Display.setTextSize(textSize);
        M5.Display.setTextColor(color);
        M5.Display.setCursor(x, y);
        
        int spaceWidth = M5.Display.textWidth(" ");
        int lineHeight = M5.Display.fontHeight();
        
        int end = startIndex + length;
        if (end > text.length()) end = text.length();
        
        int cursorX = 0;       // Relative to x
        int cursorY = 0;       // Relative to y
        
        int i = startIndex;
        
        while (i < end) {
             if (text[i] == '\n') {
                cursorX = 0;
                cursorY += lineHeight;
                i++;
                continue;
            }
            
            int wordStart = i;
            int wordEnd = i;
            while (wordEnd < end && text[wordEnd] != ' ' && text[wordEnd] != '\n') {
                wordEnd++;
            }
            
            String word = text.substring(wordStart, wordEnd);
            int wordWidth = M5.Display.textWidth(word);
            
            if (cursorX + wordWidth > width) {
                if (cursorX > 0) {
                    cursorX = 0;
                    cursorY += lineHeight;
                }
            }
            
            M5.Display.setCursor(x + cursorX, y + cursorY);
            M5.Display.print(word);
            cursorX += wordWidth;
            
            if (wordEnd < end && text[wordEnd] == ' ') {
                 // M5.Display.print(" "); // Avoid painting background for spaces if not needed
                 cursorX += spaceWidth;
                 wordEnd++;
            }
            
            i = wordEnd;
        }
    }
};

#endif
