#include "MonaImGuiEventHandler.h"

#include "MonaBackendGlobals.h"
#include "Misc/StringConvert.h"

namespace Doge::Mona
{
	namespace
	{
		// clang-format off
		auto ConvertKeyToImGuiType(EKey Key) -> ImGuiKey
		{
			switch (Key)
			{
			// Control keys
			case EKey::Escape:		return ImGuiKey_Escape;
			case EKey::CapsLock:	return ImGuiKey_CapsLock;
			case EKey::LShift:		return ImGuiKey_LeftShift;
			case EKey::RShift:		return ImGuiKey_RightShift;
			case EKey::LAlt:		return ImGuiKey_LeftAlt;
			case EKey::RAlt:		return ImGuiKey_RightAlt;
			case EKey::LControl:	return ImGuiKey_LeftCtrl;
			case EKey::RControl:	return ImGuiKey_RightCtrl;

			// Whitespace keys
			case EKey::Tab:			return ImGuiKey_Tab;
			case EKey::Space:		return ImGuiKey_Space;
			case EKey::Enter:		return ImGuiKey_Enter;
			case EKey::Backspace:	return ImGuiKey_Backspace;

			// Arrow keys
			case EKey::Left:		return ImGuiKey_LeftArrow;
			case EKey::Right:		return ImGuiKey_RightArrow;
			case EKey::Up:			return ImGuiKey_UpArrow;
			case EKey::Down:		return ImGuiKey_DownArrow;

			// Navigation keys
			case EKey::PageUp:		return ImGuiKey_PageUp;
			case EKey::PageDown:	return ImGuiKey_PageDown;
			case EKey::Home:		return ImGuiKey_Home;
			case EKey::End:			return ImGuiKey_End;
			case EKey::Insert:		return ImGuiKey_Insert;
			case EKey::Delete:		return ImGuiKey_Delete;

			// Function keys
			case EKey::F1:			return ImGuiKey_F1;
			case EKey::F2:			return ImGuiKey_F2;
			case EKey::F3:			return ImGuiKey_F3;
			case EKey::F4:			return ImGuiKey_F4;
			case EKey::F5:			return ImGuiKey_F5;
			case EKey::F6:			return ImGuiKey_F6;
			case EKey::F7:			return ImGuiKey_F7;
			case EKey::F8:			return ImGuiKey_F8;
			case EKey::F9:			return ImGuiKey_F9;
			case EKey::F10:			return ImGuiKey_F10;
			case EKey::F11:			return ImGuiKey_F11;
			case EKey::F12:			return ImGuiKey_F12;

			// Letters A-Z (both EKey and ImGuiKey are sequential)
			case EKey::A: case EKey::B: case EKey::C: case EKey::D: case EKey::E:
			case EKey::F: case EKey::G: case EKey::H: case EKey::I: case EKey::J:
			case EKey::K: case EKey::L: case EKey::M: case EKey::N: case EKey::O:
			case EKey::P: case EKey::Q: case EKey::R: case EKey::S: case EKey::T:
			case EKey::U: case EKey::V: case EKey::W: case EKey::X: case EKey::Y:
			case EKey::Z:
				return static_cast<ImGuiKey>(ImGuiKey_A + (static_cast<int>(Key) - static_cast<int>(EKey::A)));

			// Punctuation
			case EKey::Comma:			return ImGuiKey_Comma;
			case EKey::Period:			return ImGuiKey_Period;
			case EKey::Apostrophe:		return ImGuiKey_Apostrophe;
			case EKey::Semicolon:		return ImGuiKey_Semicolon;
			case EKey::Slash:			return ImGuiKey_Slash;
			case EKey::Backslash:		return ImGuiKey_Backslash;
			case EKey::LeftBracket:		return ImGuiKey_LeftBracket;
			case EKey::RightBracket:	return ImGuiKey_RightBracket;
			case EKey::GraveAccent:		return ImGuiKey_GraveAccent;

			// Main keyboard numbers and symbols
			case EKey::Num0: case EKey::Num1: case EKey::Num2: case EKey::Num3: case EKey::Num4:
			case EKey::Num5: case EKey::Num6: case EKey::Num7: case EKey::Num8: case EKey::Num9:
				return static_cast<ImGuiKey>(ImGuiKey_0 + (static_cast<int>(Key) - static_cast<int>(EKey::Num0)));
			case EKey::Minus:			return ImGuiKey_Minus;
			case EKey::Equal:			return ImGuiKey_Equal;

			// Keypad
			case EKey::Keypad0: case EKey::Keypad1: case EKey::Keypad2: case EKey::Keypad3: case EKey::Keypad4:
			case EKey::Keypad5: case EKey::Keypad6: case EKey::Keypad7: case EKey::Keypad8: case EKey::Keypad9:
				return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (static_cast<int>(Key) - static_cast<int>(EKey::Keypad0)));
			case EKey::KeypadDecimal:	return ImGuiKey_KeypadDecimal;
			case EKey::KeypadDivide:	return ImGuiKey_KeypadDivide;
			case EKey::KeypadMultiply:	return ImGuiKey_KeypadMultiply;
			case EKey::KeypadPlus:		return ImGuiKey_KeypadAdd;
			case EKey::KeypadMinus:		return ImGuiKey_KeypadSubtract;
			case EKey::KeypadEquals:	return ImGuiKey_KeypadEqual;

			default: return ImGuiKey_None;
			}
		}
		// clang-format on

		auto ConvertMouseButtonToImGuiType(EMouseButton Button) -> int32
		{
			switch (Button)
			{
			case EMouseButton::Left: return ImGuiMouseButton_Left;
			case EMouseButton::Right: return ImGuiMouseButton_Right;
			case EMouseButton::Middle: return ImGuiMouseButton_Middle;
			default:
				return -1;
			}
		}

		auto UpdateKeyModifiers(ImGuiIO& IO, EKeyModFlags Mods) -> void
		{
			IO.AddKeyEvent(ImGuiMod_Shift, EnumHasAnyFlags(Mods, EKeyModFlags::Shift));
			IO.AddKeyEvent(ImGuiMod_Alt, EnumHasAnyFlags(Mods, EKeyModFlags::Alt));
			IO.AddKeyEvent(ImGuiMod_Ctrl, EnumHasAnyFlags(Mods, EKeyModFlags::Control));
			IO.AddKeyEvent(ImGuiMod_Super, EnumHasAnyFlags(Mods, EKeyModFlags::Super));
		}

		auto GetImGuiIO(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> ImGuiIO&
		{
			return ImGui::GetIO(GMonaImGuiContext);
		}
	} // namespace

	bool FMonaImGuiEventHandler::OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat)
	{
		// IsRepeat parameter is not used here because ImGui handles key repeats internally based on the state of the keys. We just need to update the key state and modifiers.
		auto& IO = GetImGuiIO(InPlatformWindow);
		IO.AddKeyEvent(ConvertKeyToImGuiType(Key), true);
		UpdateKeyModifiers(IO, Mods);
		return IO.WantCaptureKeyboard;
	}

	bool FMonaImGuiEventHandler::OnKeyUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods)
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		IO.AddKeyEvent(ConvertKeyToImGuiType(Key), false);
		UpdateKeyModifiers(IO, Mods);
		return IO.WantCaptureKeyboard;
	}

	bool FMonaImGuiEventHandler::OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint)
	{
		// std::string Input = StringConvert::CodepointToUtf8(Codepoint);
		// DOGE_DEBUG(STR("Received character input: {} (codepoint: {})"), Input, Codepoint);
		auto& IO = GetImGuiIO(InPlatformWindow);
		IO.AddInputCharacter(Codepoint);
		return IO.WantTextInput;
	}

	bool FMonaImGuiEventHandler::OnMouseMove(const std::shared_ptr<FGenericWindow>& InPlatformWindow, FVector2d CursorPos)
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		IO.AddMousePosEvent(static_cast<float>(CursorPos.x), static_cast<float>(CursorPos.y));
		// DOGE_DEBUG(STR("Mouse moved to position ({}, {}), CaptureMouse: {}"), CursorPos.x, CursorPos.y, IO.WantCaptureMouse);
		return IO.WantCaptureMouse;
	}

	bool FMonaImGuiEventHandler::OnMouseDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos)
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		IO.AddMouseButtonEvent(ConvertMouseButtonToImGuiType(Button), true);
		// DOGE_DEBUG(STR("Mouse button {} pressed at position ({}, {}), CaptureMouse: {}"), static_cast<int>(Button), CursorPos.x, CursorPos.y, IO.WantCaptureMouse);
		return IO.WantCaptureMouse;
	}

	bool FMonaImGuiEventHandler::OnMouseUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos)
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		IO.AddMouseButtonEvent(ConvertMouseButtonToImGuiType(Button), false);
		// (STR("Mouse button {} released at position ({}, {}), CaptureMouse: {}"), static_cast<int>(Button), CursorPos.x, CursorPos.y, IO.WantCaptureMouse);
		return IO.WantCaptureMouse;
	}
} // namespace Doge::Mona