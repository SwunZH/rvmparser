#pragma once
#include <cstdio>
#include "Common.h"
#include "StoreVisitor.h"

#include <hc.h>
#include <HBaseModel.h>
#include <stack>
#include "Store.h"
#include <filesystem>

class ExportHsf :
    public StoreVisitor
{
public:
    ExportHsf(const char* path);
    ~ExportHsf();

    void beginFile(Node* group) override;

    void endFile() override;

    void beginModel(Node* group) override;

    void endModel() override;

    void beginGroup(struct Node* group) override;

    void EndGroup() override;

    void attribute(const char* key, const char* val) override;
    void beginAttributes(struct Node* container) override;

    void geometry(struct Geometry* geometry) override;

public:
    bool groupBoundingBoxes = false;
    HC_KEY m_modelKey = 0;

private:
    std::filesystem::path m_savePath;
    Map m_definedColors;
    std::stack<HC_KEY> m_keyList;
    Store* m_store = nullptr;
    unsigned stack_p = 0;
    unsigned off_v = 1;
    unsigned off_n = 1;
    unsigned off_t = 1;
    struct Connectivity* m_conn = nullptr;
    float curr_translation[3] = { 0,0,0 };

    bool anchors = false;
    bool primitiveBoundingBoxes = false;
    bool compositeBoundingBoxes = false;

};

