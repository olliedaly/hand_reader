#include "EpubReader.h"
#include "HTMLParser.h"

// Define M5 and miniz logic
EpubReader::EpubReader() {
    isOpen = false;
    fileBuffer = nullptr;
    memset(&zip_archive, 0, sizeof(zip_archive));
}

EpubReader::~EpubReader() {
    close();
}

void EpubReader::close() {
    if (isOpen) {
        mz_zip_reader_end(&zip_archive);
        isOpen = false;
        chapters.clear();
        opfPath = "";
        
        // Free fallback buffer if used
        if (fileBuffer) {
            free(fileBuffer);
            fileBuffer = nullptr;
        }
    }
}

// Add LittleFS for fallback
#include <LittleFS.h>

bool EpubReader::open(const char* filepath) {
    if (isOpen) close();
    
    Serial.printf("EpubReader::open(%s)\n", filepath);
    
    // Initialize zip reader
    memset(&zip_archive, 0, sizeof(zip_archive));
    
    // METHOD 1: Try Standard Miniz File Init (uses fopen)
    // Note: If filepath is just /filename.epub, standard fopen might fail if VFS not mounted at root?
    // LittleFS mount point default is /littlefs? No, M5Unified/Arduino usually mounts at /littlefs or /.
    bool initSuccess = mz_zip_reader_init_file(&zip_archive, filepath, 0);
    
    if (!initSuccess) {
        Serial.printf("mz_zip_reader_init_file failed for %s. Trying Memory Load Fallback...\n", filepath);
        
        String fsPath = String(filepath);
        // Fix path for LittleFS object
        if (fsPath.startsWith("/littlefs")) {
             fsPath = fsPath.substring(9); // remove /littlefs prefix (length 9)
        }
        
        if (!LittleFS.exists(fsPath)) {
            Serial.printf("LittleFS says file does not exist: %s\n", fsPath.c_str());
             if (fsPath.startsWith("/")) {
                 if (LittleFS.exists(fsPath.substring(1))) {
                     fsPath = fsPath.substring(1);
                     Serial.printf("Found it at: %s\n", fsPath.c_str());
                 }
             }
        }
        
        File f = LittleFS.open(fsPath, "r");
        if (!f) {
            Serial.println("Fallback: Failed to open file with LittleFS object.");
            return false;
        }
        
        size_t size = f.size();
        Serial.printf("File size: %d bytes\n", size);
        
        // Allocate buffer in PSRAM
        fileBuffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (!fileBuffer) {
            Serial.println("Fallback: Failed to allocate PSRAM buffer.");
            f.close();
            return false;
        }
        
        // Read file
        size_t readBytes = f.read((uint8_t*)fileBuffer, size);
        f.close();
        
        if (readBytes != size) {
            Serial.println("Fallback: Failed to read entire file.");
            free(fileBuffer);
            fileBuffer = nullptr;
            return false;
        }
        
        Serial.println("File loaded to PSRAM. Initializing zip from memory...");
        initSuccess = mz_zip_reader_init_mem(&zip_archive, fileBuffer, size, 0);
        
        if (!initSuccess) {
            Serial.println("mz_zip_reader_init_mem failed!");
            free(fileBuffer);
            fileBuffer = nullptr;
            return false;
        }
    }
    
    if (!initSuccess) {
        return false;
    }
    
    Serial.println("Zip Initialized Successfully.");
    isOpen = true;
    
    Serial.println("Parsing Container XML...");
    if (!parseContainer()) {
        Serial.println("Failed to parse container.xml");
        return false;
    }
    
    Serial.println("Parsing OPF...");
    if (!parseOPF()) {
        Serial.println("Failed to parse OPF");
        return false;
    }
    
    Serial.println("Book Opened Successfully.");
    return true;
}

String EpubReader::extractFileToString(const char* filename) {
    if (!isOpen) return "";
    
    size_t file_size;
    void* p = mz_zip_reader_extract_file_to_heap(&zip_archive, filename, &file_size, 0);
    if (!p) {
        Serial.printf("Failed to extract file: %s\n", filename);
        return "";
    }
    
    // Create string (assuming null termination might be missing, so use size)
    String content = String((char*)p);
    // Warning: String(char*, length) constructor might not behave as expected if not null terminated in older cores,
    // but usually constructor String(char*) relies on null terminator.
    // miniz does not guarantee null terminator.
    // Safer:
    String safeContent;
    safeContent.reserve(file_size + 1);
    for(size_t i=0; i<file_size; i++) safeContent += ((char*)p)[i];
    
    mz_free(p);
    return safeContent;
}

bool EpubReader::parseContainer() {
    // Standard path
    String containerXml = extractFileToString("META-INF/container.xml");
    if (containerXml.length() == 0) return false;
    
    tinyxml2::XMLDocument doc;
    doc.Parse(containerXml.c_str());
    
    tinyxml2::XMLElement* root = doc.RootElement();
    if (!root) return false;
    
    // Find <rootfile full-path="..."/>
    tinyxml2::XMLElement* rootfiles = root->FirstChildElement("rootfiles");
    if (!rootfiles) return false;
    
    tinyxml2::XMLElement* rootfile = rootfiles->FirstChildElement("rootfile");
    if (!rootfile) return false;
    
    const char* path = rootfile->Attribute("full-path");
    if (!path) return false;
    
    opfPath = String(path);
    Serial.printf("OPF Path found: %s\n", opfPath.c_str());
    return true;
}

bool EpubReader::parseOPF() {
    String opfContent = extractFileToString(opfPath.c_str());
    if (opfContent.length() == 0) return false;
    
    tinyxml2::XMLDocument doc;
    doc.Parse(opfContent.c_str());
    
    tinyxml2::XMLElement* package = doc.RootElement();
    if (!package) return false;
    
    // 1. Map Manifest (id -> href)
    std::vector<std::pair<String, String>> manifest;
    tinyxml2::XMLElement* manifestEl = package->FirstChildElement("manifest");
    if (manifestEl) {
        for (tinyxml2::XMLElement* item = manifestEl->FirstChildElement("item"); item; item = item->NextSiblingElement("item")) {
            const char* id = item->Attribute("id");
            const char* href = item->Attribute("href");
            if (id && href) {
                manifest.push_back({String(id), String(href)});
            }
        }
    }
    
    // 2. Read Spine (list of itemrefs)
    tinyxml2::XMLElement* spineEl = package->FirstChildElement("spine");
    if (!spineEl) return false;
    
    // Base path for relative hrefs
    String basePath = "";
    int lastSlash = opfPath.lastIndexOf('/');
    if (lastSlash != -1) {
        basePath = opfPath.substring(0, lastSlash + 1);
    }
    
    for (tinyxml2::XMLElement* itemref = spineEl->FirstChildElement("itemref"); itemref; itemref = itemref->NextSiblingElement("itemref")) {
        const char* idref = itemref->Attribute("idref");
        if (!idref) continue;
        
        // Find href for this idref
        String href = "";
        for (auto& item : manifest) {
            if (item.first == idref) {
                href = item.second;
                break;
            }
        }
        
        if (href.length() > 0) {
            EpubChapter chapter;
            chapter.id = String(idref);
            chapter.filename = basePath + href; 
            // Title extraction from toc.ncx is complex, skipping for MVP. using ID or filename.
            chapter.title = String(idref); 
            chapters.push_back(chapter);
        }
    }
    
    Serial.printf("Parsed %d chapters.\n", chapters.size());
    return chapters.size() > 0;
}

String EpubReader::getChapterContent(int index) {
    if (index < 0 || index >= chapters.size()) return "";
    
    String rawHtml = extractFileToString(chapters[index].filename.c_str());
    if (rawHtml.length() == 0) {
        Serial.printf("Error: Raw content empty for %s\n", chapters[index].filename.c_str());
        return "Error reading chapter.";
    }
    
    Serial.printf("Raw HTML Size: %d bytes\n", rawHtml.length());
    String cleanContent = HTMLParser::stripTags(rawHtml);
    Serial.printf("Clean Text Size: %d bytes\n", cleanContent.length());
    
    return cleanContent;
}
