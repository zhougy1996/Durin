#pragma once

#include "ApplicationCoreAPI.h"
#include "Window/GenericWindowDefinition.h"

namespace Doge
{
	class FGenericApplication;

	enum class EWindowMode : uint8
	{
		Fullscreen,			// Fullscreen with a window border
		WindowedFullScreen, // Fullscreen without a window border
		Windowed,			// Stretch the window to the size of the monitor
	};

	// Enumeration specifying the type of mouse cursor to display.
	enum class EMouseCursor : uint8
	{
		None = 0,
		Arrow,
		TextInput,
		ResizeAll,
		ResizeNS,	// top to bottom.
		ResizeEW,	// left to right.
		ResizeNESW, // top-right to the bottom-left.
		ResizeNWSE, // top-left to the bottom-right.
		Hand,
		NotAllowed
	};

	enum class EMouseButton : uint8
	{
		Left,
		Middle,
		Right,
	};

	enum class EKey : uint16
	{
		None = 0,

		// Control keys
		Escape,
		CapsLock,
		LShift,
		RShift,
		LAlt,
		RAlt,
		LControl,
		RControl,

		// Whitespace keys
		Tab,
		Space,
		Enter,
		Backspace,

		// Arrow keys
		Left,
		Right,
		Up,
		Down,

		// Navigation keys
		PageUp,
		PageDown,
		Home,
		End,
		Insert,
		Delete,

		// Function keys
		F1 = 51,
		F2,
		F3,
		F4,
		F5,
		F6,
		F7,
		F8,
		F9,
		F10,
		F11,
		F12 = 62,

		// Letters, using ASCII values for easy conversion
		A = 65,
		B,
		C,
		D,
		E,
		F,
		G,
		H,
		I,
		J,
		K,
		L,
		M,
		N,
		O,
		P,
		Q,
		R,
		S,
		T,
		U,
		V,
		W,
		X,
		Y,
		Z = 90,

		Comma = 91,		  // Comma ',' or Less '<'
		Period,			  // Period '.' or Greater '>'
		Apostrophe,		  // Apostrophe ''' or Quote '"'
		Semicolon,		  // Semicolon ';' or Colon ':'
		Slash,			  // Slash '/' or Question Mark '?'
		Backslash,		  // Backslash '\' or Vertical Bar '|'
		LeftBracket,	  // Left Bracket '[' or Left Brace '{'
		RightBracket,	  // Right Bracket ']' or Right Brace '}'
		GraveAccent = 99, // Grave Accent '`' or Tilde '~'

		// Numbers
		Num0 = 100,
		Num1,
		Num2,
		Num3,
		Num4,
		Num5,
		Num6,
		Num7,
		Num8,
		Num9,
		Minus,
		Equal,

		Keypad0 = 200,
		Keypad1,
		Keypad2,
		Keypad3,
		Keypad4,
		Keypad5,
		Keypad6,
		Keypad7,
		Keypad8,
		Keypad9,
		KeypadDecimal,
		KeypadDivide,
		KeypadMultiply,
		KeypadPlus,
		KeypadMinus,
		KeypadEquals,
	};

	class FGenericWindow
	{
	public:
		APPLICATIONCORE_API FGenericWindow();

		APPLICATIONCORE_API virtual ~FGenericWindow();

		APPLICATIONCORE_API virtual auto Initialize(const std::shared_ptr<FGenericWindowDefinition>& InDefinition) -> void;

		APPLICATIONCORE_API virtual auto PollEvents() const -> void;

		APPLICATIONCORE_API virtual auto ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void;

		APPLICATIONCORE_API virtual auto MoveWindowTo(int32 X, int32 Y) -> void;

		APPLICATIONCORE_API virtual auto GetWindowMode() const -> EWindowMode;

		APPLICATIONCORE_API virtual auto SetWindowMode(EWindowMode WindowMode) -> void;

		APPLICATIONCORE_API virtual auto GetOSNativeWindowHandle() const -> void*;

		APPLICATIONCORE_API virtual auto ShouldClose() const -> bool;

		APPLICATIONCORE_API virtual auto Close() -> void;

		APPLICATIONCORE_API virtual auto GetViewportSize() const -> FIntPoint;

		APPLICATIONCORE_API virtual auto CreateVulkanSurface(void* InVulkanInstance) const -> void*;

		APPLICATIONCORE_API virtual auto IsMinimized() const -> bool;

		APPLICATIONCORE_API virtual auto SetCursor(EMouseCursor Cursor) -> void;

	protected:
		std::shared_ptr<FGenericWindowDefinition> Definition;

		void* OSNativeWindowHandle = nullptr;
	};
}