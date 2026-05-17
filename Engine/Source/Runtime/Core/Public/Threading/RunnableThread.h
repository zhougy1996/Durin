#pragma once

#include "CoreAPI.h"

#include "HAL/Platform.h"

namespace Durin
{
	class FRunnable;

	enum class EThreadPriority : uint8
	{
		Normal,
		AboveNormal,
		BelowNormal,
		Highest,
		Lowest,
	};

	class FRunnableThread
	{
	public:
		CORE_API static auto Create(FRunnable* InRunnable, const char* ThreadName, uint32 StackSize = 0, EThreadPriority ThreadPri = EThreadPriority::Normal) -> FRunnableThread*;

		virtual ~FRunnableThread() = default;

		CORE_API virtual auto Kill(bool bShouldWait = false) -> void = 0;
		CORE_API virtual auto Suspend(bool bShouldPause = true) -> void = 0;
		CORE_API virtual auto Resume() -> void = 0;
		CORE_API virtual auto WaitForCompletion() -> void = 0;

		auto GetThreadId() const -> uint32 { return ThreadId; }
		auto GetThreadName() const -> const char* { return ThreadName.c_str(); }

	protected:
		virtual auto CreateInternal(FRunnable* InRunnable, const char* InThreadName, uint32 InStackSize, EThreadPriority InThreadPriority) -> bool = 0;

		CORE_API auto AsCurrentThread() -> void;

		uint32 ThreadId = 0;

		std::string ThreadName{};

		FRunnable* Runnable = nullptr;

		EThreadPriority ThreadPriority = EThreadPriority::Normal;
	};

	CORE_API auto GetCurrentThread() -> FRunnableThread*;
	CORE_API auto GetCurrentThreadName() -> const char*;

	CORE_API auto IsInGameThread() -> bool;
	CORE_API auto IsInRenderingThread() -> bool;



} // namespace Doge