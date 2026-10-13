#include "../src/capture_crop.h"
#include "../src/inference_slot.h"
#include <iostream>
#include <stdexcept>

static void Check(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}
int main() {
    try {
        RECT crop = {};
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