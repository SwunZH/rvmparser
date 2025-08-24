#pragma once
#include "Common.h"

class Store;

// 已有的 parseRVM 等声明...
// bool parseRVM(Store* store, Logger logger, const char* path, const void* ptr, size_t size);

// 新增：从 ExportRev 生成的文本格式解析到 Store
bool parseREV(Store* store, Logger logger, const char* path, const void* ptr, size_t size);