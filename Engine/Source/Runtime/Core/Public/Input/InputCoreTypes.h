#pragma once

#include "Misc/CoreTypes.h"

namespace Doge
{
	enum class EMouseButton : uint8
	{
		Left,
		Middle,
		Right,
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

	enum class EKeyAction : uint8
	{
		Press,
		Release,
		Repeat
	};

	enum class EKeyModFlags : uint8
	{
		None = 0,
		Shift = 1 << 0,
		Alt = 1 << 1,
		Control = 1 << 2,
		Super = 1 << 3, // Windows key or Command key
	};
	ENUM_CLASS_FLAGS(EKeyModFlags)

}