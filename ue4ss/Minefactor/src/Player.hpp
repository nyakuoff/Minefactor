#pragma once

namespace mf
{
    // Once per engine tick (game thread): sync the Abiotic Factor player with Minecraft's.
    void PlayerTick();
    // True while Minecraft drives the player (input goes to Minecraft).
    bool Puppeting();
    // True while an Abiotic Factor menu shows the mouse cursor (input goes to the game).
    bool HostMenuOpen();
    // Single player or hosting (false on a client joined to someone else's game).
    bool IsServer();
    // F8 (from the window hook): hand the controls to Minecraft, or take them back.
    void ToggleMinecraft();
}
