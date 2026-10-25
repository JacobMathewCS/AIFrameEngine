#pragma once
#include <windows.h>

// Physical screen pixels. Reject mismatched coordinate spaces; never clip
// valid client pixels to compensate for invisible window borders.
inline bool ResolveClientCrop(const RECT& bounds, const RECT& client,
    UINT width, UINT height, RECT& crop) {
    const LONGLONG bw = LONGLONG(bounds.right) - bounds.left;
    const LONGLONG bh = LONGLONG(bounds.bottom) - bounds.top;
    if (bw != width || bh != height || bw <= 0 || bh <= 0) return false;
    if (client.left < bounds.left || client.top < bounds.top ||
        client.right > bounds.right || client.bottom > bounds.bottom ||
        client.right <= client.left || client.bottom <= client.top) return false;
    crop = {client.left - bounds.left, client.top - bounds.top,
        client.right - bounds.left, client.bottom - bounds.top};
    return true;
}