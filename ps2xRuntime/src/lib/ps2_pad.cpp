#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <cstring>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

void PSPadBackend::setStartupInputCapture(bool active)
{
    m_startupInputCapture.store(active, std::memory_order_release);
}

bool PSPadBackend::consumeStartupStartPress()
{
    return m_startupStartPress.exchange(false, std::memory_order_acq_rel);
}

bool PSPadBackend::readState(int port, int /*slot*/, uint8_t *data, size_t size,
                             uint64_t presentationGeneration)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    uint16_t pressedEdges = 0u;
    const bool captureStartupInput =
        m_startupInputCapture.load(std::memory_order_acquire);
    constexpr int kGamepad = 0;
    const bool useGamepad = IsGamepadAvailable(kGamepad);
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (useGamepad)
    {
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonPressed(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            pressedEdges |= PAD_CROSS;
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonPressed(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            pressedEdges |= PAD_START;
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }
    else
    {
        // A press and release can both arrive during one host event poll.
        // IsKeyDown/IsKeyPressed then see an up key; the press queue still
        // retains the event. Feed it through the same bounded pad latch.
        for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed())
        {
            switch (key)
            {
            case KEY_UP: case KEY_W: pressedEdges |= PAD_UP; break;
            case KEY_DOWN: case KEY_S: pressedEdges |= PAD_DOWN; break;
            case KEY_LEFT: case KEY_A: pressedEdges |= PAD_LEFT; break;
            case KEY_RIGHT: case KEY_D: pressedEdges |= PAD_RIGHT; break;
            case KEY_X: case KEY_SPACE: pressedEdges |= PAD_CROSS; break;
            case KEY_C: case KEY_ESCAPE: pressedEdges |= PAD_CIRCLE; break;
            case KEY_Z: case KEY_KP_0: pressedEdges |= PAD_SQUARE; break;
            case KEY_V: case KEY_KP_1: pressedEdges |= PAD_TRIANGLE; break;
            case KEY_Q: pressedEdges |= PAD_L1; break;
            case KEY_E: pressedEdges |= PAD_R1; break;
            case KEY_LEFT_SHIFT: case KEY_ONE: pressedEdges |= PAD_L2; break;
            case KEY_RIGHT_SHIFT: case KEY_THREE: pressedEdges |= PAD_R2; break;
            case KEY_ENTER: pressedEdges |= PAD_START; break;
            case KEY_TAB: pressedEdges |= PAD_SELECT; break;
            default: break;
            }
        }
        auto keyDownOrPressed = [&pressedEdges](int key, uint16_t mask)
        {
            if (IsKeyPressed(key))
                pressedEdges |= mask;
            return IsKeyDown(key);
        };
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))
            clearBit(PAD_UP);
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
            clearBit(PAD_DOWN);
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A))
            clearBit(PAD_LEFT);
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D))
            clearBit(PAD_RIGHT);
        if (keyDownOrPressed(KEY_X, PAD_CROSS) || keyDownOrPressed(KEY_SPACE, PAD_CROSS))
            clearBit(PAD_CROSS);
        if (IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
            clearBit(PAD_CIRCLE);
        if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0))
            clearBit(PAD_SQUARE);
        if (IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
            clearBit(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q))
            clearBit(PAD_L1);
        if (IsKeyDown(KEY_E))
            clearBit(PAD_R1);
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_ONE))
            clearBit(PAD_L2);
        if (IsKeyDown(KEY_RIGHT_SHIFT) || IsKeyDown(KEY_THREE))
            clearBit(PAD_R2);
        if (keyDownOrPressed(KEY_ENTER, PAD_START))
            clearBit(PAD_START);
        if (IsKeyDown(KEY_TAB))
            clearBit(PAD_SELECT);
    }

    if (captureStartupInput)
    {
        if (IsKeyDown(KEY_ENTER) || IsKeyPressed(KEY_ENTER))
            pressedEdges |= PAD_START;
        for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed())
        {
            if (key == KEY_ENTER)
                pressedEdges |= PAD_START;
        }
    }

    const bool validPort = port >= 0 &&
        port < static_cast<int>(m_latchedButtons.size());
    bool captureStartPress = false;
    if (captureStartupInput)
    {
        captureStartPress = (pressedEdges & PAD_START) != 0u ||
                            (btns & PAD_START) == 0u;
        if (validPort)
        {
            m_latchedButtons[port] = 0u;
            m_latchSamples[port] = 0u;
            m_latchReleaseGeneration[port] = 0u;
            m_startupReleaseGate[port] = true;
        }
        btns = 0xFFFFu;
        data[2] = 0xFFu;
        data[3] = 0xFFu;
        data[4] = data[5] = data[6] = data[7] = kPadStickCenter;
        if (captureStartPress)
            m_startupStartPress.store(true, std::memory_order_release);
        return true;
    }

    if (validPort)
    {
        if (m_startupReleaseGate[port])
        {
            if (btns == 0xFFFFu)
                m_startupReleaseGate[port] = false;
            m_latchedButtons[port] = 0u;
            m_latchSamples[port] = 0u;
            m_latchReleaseGeneration[port] = 0u;
            data[2] = 0xFFu;
            data[3] = 0xFFu;
            data[4] = data[5] = data[6] = data[7] = kPadStickCenter;
            return true;
        }

        if (pressedEdges != 0u)
        {
            m_latchedButtons[port] |= pressedEdges;
            // Six published guest frames give a slow guest a chance to read
            // the input. Bound the host polls as well if it stops publishing.
            m_latchSamples[port] = presentationGeneration == UINT64_MAX ? 2u : 120u;
            m_latchReleaseGeneration[port] = presentationGeneration == UINT64_MAX
                ? UINT64_MAX : presentationGeneration + 6u;
        }
        if (m_latchSamples[port] != 0u)
        {
            btns &= static_cast<uint16_t>(~m_latchedButtons[port]);
            if (--m_latchSamples[port] == 0u ||
                (presentationGeneration != UINT64_MAX &&
                 presentationGeneration >= m_latchReleaseGeneration[port]))
            {
                m_latchSamples[port] = 0u;
                m_latchedButtons[port] = 0u;
            }
        }
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
