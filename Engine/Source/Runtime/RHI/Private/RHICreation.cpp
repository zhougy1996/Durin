#include "Backend/RHICreation.h"

#if defined(_WIN32)
	#include <Windows.h>
#else
	#include <unistd.h>
#endif

namespace Durin
{
	static auto ReportFailure(const char* Operation, const char* Reason, int64 NativeCode) noexcept -> void
	{
		// Report without the allocating logger; failure to report must remain terminal.
		char Message[1024];
		const int Count = std::snprintf(Message, sizeof(Message), "RHI terminal failure: %.240s: %.640s (native=%lld)\n", Operation ? Operation : "unknown operation", Reason ? Reason : "unknown failure", static_cast<long long>(NativeCode));
		if (Count > 0)
		{
			const auto Size = static_cast<size_t>(std::min(Count, static_cast<int>(sizeof(Message) - 1)));
#if defined(_WIN32)
			DWORD Written;
			WriteFile(GetStdHandle(STD_ERROR_HANDLE), Message, static_cast<DWORD>(Size), &Written, nullptr);
#else
			(void)::write(STDERR_FILENO, Message, Size);
#endif
		}
	}

	[[noreturn]] auto RHITerminateOnFailure(const char* Operation, const char* Reason, int64 NativeCode) noexcept -> void
	{
		ReportFailure(Operation, Reason, NativeCode);
		// Launch owns the process terminate handler and crash artifacts.
		std::terminate();
	}

	[[noreturn]] auto RHITerminateOnOutOfMemory(const char* Operation, int64 NativeCode) noexcept -> void
	{
		ReportFailure(Operation, "out of memory", NativeCode);
#if defined(_WIN32)
		// ExitProcess (also used by the CRT's _Exit) runs DLL detach callbacks on
		// this thread. RHI failure must not enter module/render teardown, especially
		// when called from a native creator or replay thread.
		TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
#endif
		std::_Exit(EXIT_FAILURE);
	}
} // namespace Durin
