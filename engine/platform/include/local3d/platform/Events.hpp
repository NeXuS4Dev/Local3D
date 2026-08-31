#pragma once
/// @file Events.hpp
/// @brief Platform independent input and window events.
///
/// Events are plain data produced by a platform backend and consumed by the
/// Input module.  Game code never sees a platform event type: backends translate
/// their native events into these, which keeps gameplay portable and testable
/// (the headless backend can inject any event sequence).

#include "local3d/core/Common.hpp"
#include "local3d/math/Math.hpp"

#include <string>
#include <variant>

namespace l3d::platform {

/// Platform independent key codes.  Named after the US QWERTY *position*
/// (KeyW is the same physical key on AZERTY), which is what gameplay wants;
/// text input arrives separately as TextInputEvent.
enum class Key : u16 {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Escape, Enter, Space, Backspace, Tab, Delete, Insert,
    Up, Down, Left, Right,
    Home, End, PageUp, PageDown,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt,
    LeftSuper, RightSuper,
    Count,
};

enum class MouseButton : u8 { Left = 0, Right, Middle, Button4, Button5, Count };

enum class GamepadButton : u8 {
    A = 0, B, X, Y, LeftBumper, RightBumper, Back, Start, Guide,
    LeftStick, RightStick, DpadUp, DpadDown, DpadLeft, DpadRight, Count,
};

enum class GamepadAxis : u8 { LeftX = 0, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

struct KeyEvent {
    Key key = Key::Unknown;
    bool pressed = false;  ///< true = down, false = up
    bool repeat = false;   ///< OS auto-repeat
    u16 scancode = 0;
};

struct TextInputEvent {
    std::string text; // UTF-8
};

struct MouseMoveEvent {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 deltaX = 0.0f;
    f32 deltaY = 0.0f;
};

struct MouseButtonEvent {
    MouseButton button = MouseButton::Left;
    bool pressed = false;
    f32 x = 0.0f;
    f32 y = 0.0f;
};

struct MouseWheelEvent {
    f32 deltaX = 0.0f;
    f32 deltaY = 0.0f;
};

struct GamepadButtonEvent {
    u32 gamepadIndex = 0;
    GamepadButton button = GamepadButton::A;
    bool pressed = false;
};

struct GamepadAxisEvent {
    u32 gamepadIndex = 0;
    GamepadAxis axis = GamepadAxis::LeftX;
    f32 value = 0.0f;
};

struct GamepadConnectedEvent {
    u32 gamepadIndex = 0;
    bool connected = false;
    std::string name;
};

struct WindowResizedEvent {
    u32 width = 0;
    u32 height = 0;
    bool fullscreen = false;
};

struct WindowCloseEvent {};

struct QuitEvent {};

/// Everything a backend can emit.  Extend by adding a variant member; consumers
/// use std::visit or the typed accessors in Input.
using Event = std::variant<KeyEvent, TextInputEvent, MouseMoveEvent, MouseButtonEvent,
                           MouseWheelEvent, GamepadButtonEvent, GamepadAxisEvent,
                           GamepadConnectedEvent, WindowResizedEvent, WindowCloseEvent, QuitEvent>;

[[nodiscard]] std::string_view KeyName(Key key) noexcept;

} // namespace l3d::platform
