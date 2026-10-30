#pragma once
#include <cstdint>
#include <memory>
#include <string>

struct HWND__;

// Player 2's picture drawn with Direct3D 11. Colour conversion and scaling run on the GPU; with GDI
// they took a CPU frame time on handhelds, where Proton maps Direct3D 11 to Vulkan. Windows only.
namespace openshao::stream {

class ViewRenderer {
public:
    ViewRenderer();
    ~ViewRenderer();
    ViewRenderer(const ViewRenderer&) = delete;
    ViewRenderer& operator=(const ViewRenderer&) = delete;

    // Creates the device and swap chain for the window. Call before any Draw.
    bool Open(HWND__* window, std::string& error);
    // Draws one packed NV12 frame (see PackNv12) letterboxed at `aspect` (width / height), with the
    // round trip in the top-right corner when ping_ms >= 0. A null nv12 redraws the previous frame,
    // which also follows window size changes. Call from one thread only.
    bool Draw(const uint8_t* nv12, uint32_t width, uint32_t height, float aspect, int ping_ms);

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace openshao::stream
