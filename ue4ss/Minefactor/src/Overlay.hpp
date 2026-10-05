#pragma once
#include <atomic>

namespace mf::overlay
{
    // Written by the game thread (PlayerTick) and the window hook; read on the render thread.
    inline std::atomic<bool> show{false};         // Minecraft drives the player and no game menu is up
    inline std::atomic<bool> screenOpen{false};   // a Minecraft screen (inventory, chat, ...) is open
    inline std::atomic<bool> crosshair{false};    // first person, no screen: draw MC's crosshair inverted
    inline std::atomic<int> guiScale{0};
    inline std::atomic<int> cursorX{0}, cursorY{0};  // Minecraft cursor, overlay pixels
    inline std::atomic<int> viewportW{0}, viewportH{0};
    inline std::atomic<bool> reset{false};        // Minecraft reconnected: restart the triple buffer

    // Hooks the D3D12 swap chain's Present to draw Minecraft's frame (hand, HUD, screens) on top.
    void Install();
}
