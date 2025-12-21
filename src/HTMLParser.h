#ifndef HTML_PARSER_H
#define HTML_PARSER_H

#include <Arduino.h>

class HTMLParser {
public:
    static String stripTags(const String& html) {
        String script = "";
        bool insideTag = false;
        bool ignoreContent = false; // To ignore content of style/script
        
        script.reserve(html.length());
        
        // Simple state machine
        for (int i = 0; i < html.length(); i++) {
            char c = html[i];
            
            if (c == '<') {
                insideTag = true;
                
                // Check for start of style or script to enable ignore mode
                if (html.substring(i, i+6).equalsIgnoreCase("<style")) ignoreContent = true;
                if (html.substring(i, i+7).equalsIgnoreCase("<script")) ignoreContent = true;
                if (html.substring(i, i+5).equalsIgnoreCase("<head")) ignoreContent = true;
                
                // Check for closing tags to disable ignore mode
                // We check at the start of the tag, e.g., </style>
                if (html.substring(i, i+8).equalsIgnoreCase("</style>")) ignoreContent = false;
                if (html.substring(i, i+9).equalsIgnoreCase("</script>")) ignoreContent = false;
                if (html.substring(i, i+7).equalsIgnoreCase("</head>")) ignoreContent = false;

                continue;
            }
            
            if (c == '>') {
                insideTag = false;
                // Add newline for block tags for better formatting
                // simplistic check
                if (script.length() > 0 && script[script.length()-1] != ' ' && !ignoreContent) {
                   script += ' ';
                }
                continue;
            }
            
            if (!insideTag && !ignoreContent) {
                script += c;
            }
        }
        
        // Basic entity replacement
        script.replace("&nbsp;", " ");
        script.replace("&amp;", "&");
        script.replace("&lt;", "<");
        script.replace("&gt;", ">");
        script.replace("&quot;", "\"");
        script.replace("&#39;", "'");
        
        // Remove excessive whitespace
        while (script.indexOf("  ") != -1) {
            script.replace("  ", " ");
        }
        
        // Add newlines? The current parser makes everything one line.
        // For wrapping on screen, M5GFX handles some wrapping if we use print.
        // But double spaces or specific chars might help.
        
        return script;
    }
};

#endif
