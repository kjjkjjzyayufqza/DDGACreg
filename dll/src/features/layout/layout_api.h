#pragma once

// Implemented by dll/src/features/layout/layout.cpp.
// The display compositor calls Layout_Query each frame. 32:9 leaves the
// DDGUltrawide arcade layout in place. 16:9 and single-screen replace it.

enum LayoutPreset
{
    LayoutPreset_Cabinet32x9 = 0,
    LayoutPreset_Widescreen16x9 = 1,
    LayoutPreset_Single = 2,
};

// Matches DDGUltrawide source order: left, center, right, touch.
enum LayoutFocus
{
    LayoutFocus_Left = 0,
    LayoutFocus_Center = 1,
    LayoutFocus_Right = 2,
    LayoutFocus_Touch = 3,
};

struct LayoutRect
{
    float sizeX;
    float sizeY;
    float originX;
    float originY;
};

struct LayoutView
{
    int preset;
    int focus;
    // When false, the compositor keeps the arcade cabinet layout.
    bool overridePlacements;
    LayoutRect dest[4];
    LayoutRect source[4];
    int activeCount;
};

LayoutView Layout_Query(int outputWidth, int outputHeight);
