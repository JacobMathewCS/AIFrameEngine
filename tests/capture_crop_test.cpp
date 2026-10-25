#include "../src/capture_crop.h"
#include "../src/inference_slot.h"
#include "../src/capture_extent.h"
#include "../src/frame_timeline.h"
#include <iostream>
#include <stdexcept>

static void Check(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}
int main() {
    try {
        RECT crop = {};
        CaptureExtent extent;
        Check(extent.Update(1280,720), "initial extent");
        Check(!extent.Update(1280,720), "unchanged extent");
        Check(!extent.Update(0,0) && extent.width == 1280, "minimize retains valid extent");
        Check(extent.Update(1920,1080), "grow extent");
        Check(extent.Update(853,479), "odd smaller extent");
        Check(extent.Update(3440,1440), "ultrawide extent");
        FrameTimeline<int> timeline;
        timeline.Add(1, 100, 200, 1, 2);
        timeline.Clear();
        timeline.Add(3, 300, 400, 3, 4);
        Check(!timeline.Attach(1, 99, 400), "old-size result cannot attach after reset");
        Check(timeline.Attach(3, 5, 400), "new-size result attaches");
        const RECT window = {100,100,1398,867};
        const RECT visible = {108,100,1390,859};
        const RECT client = {109,138,1389,858};
        Check(!ResolveClientCrop(window,client,1282,759,crop), "reject invisible-border mismatch");
        Check(ResolveClientCrop(visible,client,1282,759,crop), "visible bounds match capture");
        Check(crop.left==1 && crop.top==38 && crop.right-crop.left==1280 &&
            crop.bottom-crop.top==720, "retain 1280 client columns, not 1273");
        Check(ResolveClientCrop(window,client,1298,767,crop) && crop.left==9, "full-window origin");
        Check(ResolveClientCrop(client,client,1280,720,crop) && crop.left==0 && crop.top==0, "client-only");
        const RECT negative = {-1920,-200,-640,520};
        Check(ResolveClientCrop(negative,negative,1280,720,crop), "negative monitor coordinates");
        // Synthetic decorated 1080p client extending past a 1920x1080 desktop.
        // Crop math uses capture bounds, not the physical monitor's rectangle.
        const RECT nativeWindow = {-8,-8,1928,1104};
        const RECT nativeClient = {0,24,1920,1104};
        Check(ResolveClientCrop(nativeWindow,nativeClient,1936,1112,crop),
            "off-screen decorated native-size window is valid capture geometry");
        Check(crop.left==8 && crop.top==32 && crop.right-crop.left==1920 &&
            crop.bottom-crop.top==1080, "preserve full native client beyond desktop");
        Check(!ResolveClientCrop(nativeWindow,nativeClient,1920,1080,crop),
            "do not pretend desktop-sized texture contains decorated window");
        Check(!ResolveClientCrop(visible,client,1300,800,crop), "resize mismatch rejected");
        const RECT outside = {107,138,1389,858};
        Check(!ResolveClientCrop(visible,outside,1282,759,crop), "outside client rejected");
        Check(CanSubmitInference(false,false), "idle empty slot accepts");
        Check(!CanSubmitInference(true,false), "busy blocks");
        Check(!CanSubmitInference(false,true), "pending result blocks overwrite");
        Check(!CanSubmitInference(true,true), "publication before idle blocks");
        std::cout << "PASS: crop coordinate spaces and result-slot admission\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}