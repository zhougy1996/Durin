#include "HAL/PlatformLibrary.h"
#include "Misc/StringConvert.h"

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace Durin
{
	namespace
	{
		auto Invalid(EPlatformLibraryOperation Operation, const std::filesystem::path& Path,
			std::string_view Symbol, std::string Message) -> FPlatformLibraryError
		{
			return {Operation, Path, std::string(Symbol),
				std::make_error_code(std::errc::invalid_argument), std::move(Message)};
		}

#if defined(_WIN32)
		auto LoaderError(EPlatformLibraryOperation Operation, const std::filesystem::path& Path,
			std::string_view Symbol, DWORD Code) -> FPlatformLibraryError
		{
			const std::error_code Error(static_cast<int>(Code), std::system_category());
			wchar_t* Buffer = nullptr;
			const DWORD Length = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
				| FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, Code, 0, reinterpret_cast<wchar_t*>(&Buffer), 0, nullptr);
			const auto Detail = Length ? StringUtils::WideToUtf8(std::wstring_view(Buffer, Length)) : std::string{};
			if (Buffer) ::LocalFree(Buffer);
			return {Operation, Path, std::string(Symbol), Error,
				std::format("Windows library operation failed ({}): {}", Code, Detail)};
		}
#else
		auto LoaderError(EPlatformLibraryOperation Operation, const std::filesystem::path& Path,
			std::string_view Symbol, const char* Error) -> FPlatformLibraryError
		{
			// dlerror owns the text only until the next loader call on this thread.
			return {Operation, Path, std::string(Symbol), {}, Error ? Error : "Unknown macOS loader error."};
		}
#endif
	}

	auto FPlatformLibrary::Load(const std::filesystem::path& Path)
		-> std::expected<FModuleHandle, FPlatformLibraryError>
	{
		const auto& Native = Path.native();
		if (Path.empty() || !Path.is_absolute() || Native.find(typename std::filesystem::path::value_type{}) != Native.npos)
			return std::unexpected(Invalid(EPlatformLibraryOperation::Load, Path, {},
				"Library path must be absolute and contain no NUL characters."));
#if defined(_WIN32)
		std::wstring Extended = Path.lexically_normal().native();
		std::replace(Extended.begin(), Extended.end(), L'/', L'\\');
		if (!Extended.starts_with(L"\\\\?\\"))
			Extended = Extended.starts_with(L"\\\\")
				? L"\\\\?\\UNC\\" + Extended.substr(2) : L"\\\\?\\" + Extended;
		const auto Handle = ::LoadLibraryExW(Extended.c_str(), nullptr,
			LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		if (!Handle)
		{
			const DWORD Error = ::GetLastError();
			return std::unexpected(LoaderError(EPlatformLibraryOperation::Load, Path, {}, Error));
		}
#else
		dlerror();
		// Preserve filesystem traversal through symlinks, including symlink/.. paths.
		const auto Handle = dlopen(Path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!Handle) return std::unexpected(LoaderError(EPlatformLibraryOperation::Load, Path, {}, dlerror()));
#endif
		return Handle;
	}

	auto FPlatformLibrary::FindSymbol(FModuleHandle Handle, std::string_view Symbol)
		-> std::expected<void*, FPlatformLibraryError>
	{
		if (!Handle || Symbol.empty() || std::ranges::any_of(Symbol,
			[](unsigned char Character) { return Character == 0 || Character > 127; }))
			return std::unexpected(Invalid(EPlatformLibraryOperation::FindSymbol, {}, Symbol,
				"Symbol lookup requires a live handle and a nonempty ASCII export name without NULs."));
		const std::string Name(Symbol);
#if defined(_WIN32)
		const auto Address = reinterpret_cast<void*>(::GetProcAddress(Handle, Name.c_str()));
		if (!Address)
		{
			const DWORD Error = ::GetLastError();
			return std::unexpected(LoaderError(EPlatformLibraryOperation::FindSymbol, {}, Symbol, Error));
		}
#else
		dlerror();
		const auto Address = dlsym(Handle, Name.c_str());
		if (const char* Error = dlerror())
			return std::unexpected(LoaderError(EPlatformLibraryOperation::FindSymbol, {}, Symbol, Error));
#endif
		return Address;
	}

	auto FPlatformLibrary::Unload(FModuleHandle Handle) -> std::expected<void, FPlatformLibraryError>
	{
		if (!Handle) return std::unexpected(Invalid(EPlatformLibraryOperation::Unload, {}, {},
			"Library unload requires an owned live handle."));
#if defined(_WIN32)
		if (!::FreeLibrary(Handle))
		{
			const DWORD Error = ::GetLastError();
			return std::unexpected(LoaderError(EPlatformLibraryOperation::Unload, {}, {}, Error));
		}
#else
		dlerror();
		if (dlclose(Handle) != 0)
			return std::unexpected(LoaderError(EPlatformLibraryOperation::Unload, {}, {}, dlerror()));
#endif
		return {};
	}
}
