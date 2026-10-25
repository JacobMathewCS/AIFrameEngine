#pragma once

// Zero-sized/minimized frames must not replace the last usable extent.
struct CaptureExtent {
    int width = 0, height = 0;
    bool Update(int w, int h) {
        if (w <= 0 || h <= 0 || (w == width && h == height)) return false;
        width = w; height = h;
        return true;
    }
};