#ifndef EPUB_READER_H
#define EPUB_READER_H

#include <Arduino.h>
#include <vector>
#include "miniz.h"
#include <tinyxml2.h>

struct EpubChapter {
    String title;
    String filename; // internal path in zip
    String id;
};

class EpubReader {
private:
    mz_zip_archive zip_archive;
    bool isOpen;
    std::vector<EpubChapter> chapters;
    // File Buffer for memory loading fallback
    void* fileBuffer = nullptr;
    String opfPath;

    // Helper to extract a file from zip to String
    String extractFileToString(const char* filename);
    
    // Parse container.xml to find OPF
    bool parseContainer();
    // Parse OPF to get metadata and spine
    bool parseOPF();

public:
    EpubReader();
    ~EpubReader();

    bool open(const char* filepath);
    void close();
    
    // Get list of chapters (spine)
    const std::vector<EpubChapter>& getChapters() { return chapters; }
    
    // Extract text content of a chapter
    String getChapterContent(int index);
};

#endif
