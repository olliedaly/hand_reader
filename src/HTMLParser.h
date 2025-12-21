#ifndef HTML_PARSER_H
#define HTML_PARSER_H

#include <Arduino.h>

class HTMLParser {
public:
    static String stripTags(const String& html) {
        String script = "";
        bool insideTag = false;
        bool ignoreContent = false; 
        
        script.reserve(html.length());
        
        for (int i = 0; i < html.length(); i++) {
            char c = html[i];
            
            if (c == '<') {
                insideTag = true;
                
                // Check for start of style or script to enable ignore mode
                if (html.substring(i, i+6).equalsIgnoreCase("<style")) ignoreContent = true;
                if (html.substring(i, i+7).equalsIgnoreCase("<script")) ignoreContent = true;
                if (html.substring(i, i+5).equalsIgnoreCase("<head")) ignoreContent = true;
                
                // Block tags that imply newlines
                if (html.substring(i, i+3).equalsIgnoreCase("<p>") || 
                    html.substring(i, i+3).equalsIgnoreCase("<p ") ||
                    html.substring(i, i+4).equalsIgnoreCase("<div") ||
                    html.substring(i, i+4).equalsIgnoreCase("<br")) {
                    if (script.length() > 0 && script[script.length()-1] != '\n') script += '\n';
                }

                // Check for closing tags to disable ignore mode
                if (html.substring(i, i+8).equalsIgnoreCase("</style>")) ignoreContent = false;
                if (html.substring(i, i+9).equalsIgnoreCase("</script>")) ignoreContent = false;
                if (html.substring(i, i+7).equalsIgnoreCase("</head>")) ignoreContent = false;
                
                // Closing block tags
                if (html.substring(i, i+4).equalsIgnoreCase("</p>") || 
                    html.substring(i, i+6).equalsIgnoreCase("</div>")) {
                     if (script.length() > 0 && script[script.length()-1] != '\n') script += '\n';
                }

                continue;
            }
            
            if (c == '>') {
                insideTag = false;
                continue;
            }
            
            if (!insideTag && !ignoreContent) {
                // Collapse whitespace: If we encounter a newline/tab/space, convert to space
                // ONLY if the previous char wasn't usually a newline or space.
                // But simplified: just append, we clean up later.
                // Actually, let's just treat standard newlines in HTML as spaces (standard variable width rule),
                // UNLESS we just inserted a logical newline from a tag.
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                
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
        script.replace("&#8217;", "'");
        script.replace("&#8220;", "\"");
        script.replace("&#8221;", "\"");
        
        // Collapse multiple spaces
        while (script.indexOf("  ") != -1) {
            script.replace("  ", " ");
        }
        
        // Prepare for Paginator:
        // We want paragraphs to be distinguished.
        // We added '\n' for p/br/div. 
        // Let's ensure we don't have " \n "
        script.replace(" \n", "\n");
        script.replace("\n ", "\n");
        
        // Multiple newlines -> Double Newline max?
        while (script.indexOf("\n\n\n") != -1) {
            script.replace("\n\n\n", "\n\n");
        }
        
        return script;
    }
};

#endif
