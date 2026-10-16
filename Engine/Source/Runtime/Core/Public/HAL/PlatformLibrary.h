#pragma once

#include "Misc/CoreStd.h"
#include "HAL/Platform.h"
#include "CoreAPI.h"

namespace Durin
{
	// Identifies the loader operation without requiring callers to parse native text.
	enum class EPlatformLibraryOperation { Load, FindSymbol, Unload };

	// Owns diagnostics captured at failure, independent of subsequent loader calls.
	struct FPlatformLibraryError
	{
		EPlatformLibraryOperation Operation;
		std::filesystem::path Path;
		std::string Symbol;
		std::error_code NativeError;
		std::string Message;
		auto ToString() const -> const std::string& { return Message; }
	};

	// Native library references on Windows and macOS; does not initialize engine modules.
	struct FPlatformLibrary
	{
		// Requires an absolute native path, never searches for the requested library by name.
		// Success transfers one loader reference to the caller. Unload exactly once, or
		// intentionally retain it for process lifetime. No ownership is implied by a copy.
		// Windows searches dependencies beside the library and in default/user DLL dirs;
		// macOS uses Mach-O install names/rpaths, with eager, local symbol binding.
		[[nodiscard]] static CORE_API auto Load(const std::filesystem::path& Path)
			-> std::expected<FModuleHandle, FPlatformLibraryError>;

		// The handle must be live. Symbol is a nonempty ASCII export name without NULs.
		// Returned addresses are borrowed and must not outlive the library reference.
		[[nodiscard]] static CORE_API auto FindSymbol(FModuleHandle Handle, std::string_view Symbol)
			-> std::expected<void*, FPlatformLibraryError>;

		// Requires an owned live reference and no outstanding calls/borrowed addresses.
		// Success consumes that reference; failure leaves ownership with the caller.
		[[nodiscard]] static CORE_API auto Unload(FModuleHandle Handle)
			-> std::expected<void, FPlatformLibraryError>;
	};
}
