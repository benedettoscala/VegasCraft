#include "Bridge.h"
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    int seconds = argc > 1 ? std::atoi(argv[1]) : 15;
    std::wstring name = vegas::kMappingName;
    if (argc > 2) { std::string arg = argv[2]; name.assign(arg.begin(), arg.end()); }
    vegas::Bridge bridge;
    if (!bridge.open(name)) { std::fprintf(stderr, "mapping failed: %lu\n", bridge.error()); return 2; }
    vegas::proto::SkyState state{};
    state.flags = vegas::proto::kSkyInGame;
    state.worldId = 0x3C;
    state.collisionEpoch = 1; state.teleportSeq = 1;
    state.posX = 12.5; state.posY = 64; state.posZ = -7.25;
    state.yaw = 180; state.pitch = 5;
    bool received = false;
    std::puts("READY x86 host"); std::fflush(stdout);
    auto end = GetTickCount64() + seconds * 1000;
    while (GetTickCount64() < end) {
        bridge.heartbeat(); bridge.publish(state);
        vegas::proto::McState mc{};
        if (bridge.readMinecraft(mc) && mc.x == 21.25 && mc.y == 80.5 && mc.z == -9.75 && mc.frameCounter == 42) {
            if (!received) { std::puts("ACK Minecraft state received"); std::fflush(stdout); }
            received = true;
        }
        Sleep(10);
    }
    return received ? 0 : 3;
}
