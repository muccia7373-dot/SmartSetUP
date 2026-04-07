#pragma once

#include "config.h"
#include <FS.h>

class Rf73StorageClass {
public:
  bool begin();
  fs::File open(const String& path, const char* mode = FILE_READ);
  bool exists(const String& path);
  bool remove(const String& path);
  bool isReady() const { return _ready; }
  bool usingMmc() const { return _usingMmc; }
  const char* backendName() const;

private:
  bool _ready = false;
  bool _usingMmc = false;
};

extern Rf73StorageClass rf73Storage;