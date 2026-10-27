#include "runtime/ps2_pad.h"
#include "raylib.h"
#include <cstdio>
#include <deque>
#include <set>

// Model a complete press/release delivered in one host event poll: the
// current key state is up, but raylib's press queue retains the event.
static std::deque<int> queuedKeys;
static std::set<int> heldKeys;
static bool gamepadAvailable = false;
extern "C" bool IsKeyDown(int key) { return heldKeys.count(key) != 0; }
extern "C" bool IsKeyPressed(int) { return false; }
extern "C" int GetKeyPressed(void) {
    if (queuedKeys.empty()) return 0;
    int key = queuedKeys.front(); queuedKeys.pop_front(); return key;
}
extern "C" bool IsGamepadAvailable(int) { return gamepadAvailable; }
extern "C" bool IsGamepadButtonDown(int, int) { return false; }
extern "C" bool IsGamepadButtonPressed(int, int) { return false; }
extern "C" float GetGamepadAxisMovement(int, int) { return 0.0f; }

int main() {
    const struct { int key; unsigned mask; } cases[] = {
        {KEY_X, 0x4000}, {KEY_Z, 0x8000}, {KEY_C, 0x2000},
        {KEY_V, 0x1000}, {KEY_ENTER, 0x0008}, {KEY_UP, 0x0010},
        {KEY_DOWN, 0x0040}, {KEY_LEFT, 0x0080}, {KEY_RIGHT, 0x0020},
        {KEY_Q, 0x0400}, {KEY_E, 0x0800}, {KEY_TAB, 0x0001},
        {KEY_ONE, 0x0100}, {KEY_THREE, 0x0200}
    };
    for (auto c : cases) {
        PSPadBackend pad;
        queuedKeys = {c.key};
        unsigned char data[32]{};
        pad.readState(0, 0, data, sizeof(data));
        if ((static_cast<unsigned>(data[2] | (data[3] << 8)) & c.mask) != 0) {
            std::printf("FAIL: queued key %d lost\n", c.key); return 1;
        }
        // The existing latch is bounded and must eventually release.
        for (int i = 0; i < 3; ++i) pad.readState(0, 0, data, sizeof(data));
        if ((data[2] | (data[3] << 8)) != 0xffff) {
            std::puts("FAIL: queued key stuck"); return 1;
        }
    }
    PSPadBackend slowPad;
    unsigned char data[32]{};
    queuedKeys = {KEY_Z};
    for (int i = 0; i < 30; ++i) {
        slowPad.readState(0, 0, data, sizeof(data), 100);
        if ((data[3] & 0x80) != 0) {
            std::puts("FAIL: key expired before guest frames advanced"); return 1;
        }
    }
    slowPad.readState(0, 0, data, sizeof(data), 102);
    slowPad.readState(0, 0, data, sizeof(data), 103);
    if ((data[3] & 0x80) != 0) {
        std::puts("FAIL: key expired during a burst of guest publications"); return 1;
    }
    slowPad.readState(0, 0, data, sizeof(data), 106);
    slowPad.readState(0, 0, data, sizeof(data), 107);
    if ((data[2] | (data[3] << 8)) != 0xffff) {
        std::puts("FAIL: key not released after guest progress"); return 1;
    }
    queuedKeys = {KEY_X};
    for (int i = 0; i < 121; ++i)
        slowPad.readState(0, 0, data, sizeof(data), 103);
    if ((data[2] | (data[3] << 8)) != 0xffff) {
        std::puts("FAIL: key stuck when guest stops publishing"); return 1;
    }
    // Held controls must outlive the edge latch and release on key-up.
    heldKeys.insert(KEY_THREE);
    for (int i = 0; i < 130; ++i) {
        slowPad.readState(0, 0, data, sizeof(data), 1000 + i);
        if ((data[3] & 0x02) != 0) {
            std::puts("FAIL: held R2 alias released early"); return 1;
        }
    }
    heldKeys.clear();
    slowPad.readState(0, 0, data, sizeof(data), 1200);
    if ((data[2] | (data[3] << 8)) != 0xffff) {
        std::puts("FAIL: held R2 alias stuck after key-up"); return 1;
    }

    auto neutralPadState = [](const unsigned char *state) {
        return state[2] == 0xff && state[3] == 0xff &&
               state[4] == 0x80 && state[5] == 0x80 &&
               state[6] == 0x80 && state[7] == 0x80;
    };

    PSPadBackend startupPad;
    queuedKeys = {KEY_ENTER, KEY_X, KEY_Z};
    heldKeys.clear();
    startupPad.setStartupInputCapture(true);
    startupPad.readState(0, 0, data, sizeof(data), 2000);
    if (!neutralPadState(data)) {
        std::puts("FAIL: startup capture exposed queued buttons to the guest"); return 1;
    }
    if (!startupPad.consumeStartupStartPress() ||
        startupPad.consumeStartupStartPress()) {
        std::puts("FAIL: startup Start edge was not published exactly once"); return 1;
    }
    startupPad.setStartupInputCapture(false);
    startupPad.readState(0, 0, data, sizeof(data), 2001);
    if (!neutralPadState(data)) {
        std::puts("FAIL: queued movie buttons leaked after capture ended"); return 1;
    }

    PSPadBackend gamepadStartupPad;
    queuedKeys = {KEY_ENTER, KEY_X};
    gamepadAvailable = true;
    gamepadStartupPad.setStartupInputCapture(true);
    gamepadStartupPad.readState(0, 0, data, sizeof(data), 2500);
    if (!queuedKeys.empty() || !neutralPadState(data) ||
        !gamepadStartupPad.consumeStartupStartPress()) {
        std::puts("FAIL: capture did not drain keyboard Start with gamepad available"); return 1;
    }
    gamepadStartupPad.setStartupInputCapture(false);
    gamepadStartupPad.readState(0, 0, data, sizeof(data), 2501);
    gamepadAvailable = false;

    PSPadBackend heldStartupPad;
    queuedKeys.clear();
    heldKeys = {KEY_ENTER, KEY_X};
    heldStartupPad.setStartupInputCapture(true);
    heldStartupPad.readState(0, 0, data, sizeof(data), 3000);
    if (!neutralPadState(data) || !heldStartupPad.consumeStartupStartPress()) {
        std::puts("FAIL: held startup Start was not captured with neutral guest input"); return 1;
    }
    heldStartupPad.setStartupInputCapture(false);
    heldStartupPad.readState(0, 0, data, sizeof(data), 3001);
    if (!neutralPadState(data)) {
        std::puts("FAIL: held movie controls leaked when capture ended"); return 1;
    }
    heldKeys.clear();
    heldStartupPad.readState(0, 0, data, sizeof(data), 3002);
    if (!neutralPadState(data)) {
        std::puts("FAIL: guest input was not held neutral through release"); return 1;
    }
    queuedKeys = {KEY_X};
    heldStartupPad.readState(0, 0, data, sizeof(data), 3003);
    if ((static_cast<unsigned>(data[2] | (data[3] << 8)) & 0x4000u) != 0u) {
        std::puts("FAIL: fresh Cross press did not work after startup release"); return 1;
    }
    std::puts("PASS: queued keyboard events survive host polls and release after guest progress");
}
