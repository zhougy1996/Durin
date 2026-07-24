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

	enum class EThreadRole : uint8
	{
		Unknown,
		GameThread,
		RenderingThread,
		WorkerThread,
		IOThread,
	};

	// Owns a platform thread running an FRunnable and records its engine thread role.
	class FRunnableThread
	{
	public:
		CORE_API static auto Create(FRunnable* InRunnable, const char* ThreadName, uint32 StackSize = 0, EThreadPriority ThreadPri = EThreadPriority::Normal, EThreadRole ThreadRole = EThreadRole::Unknown) -> FRunnableThread*;

		virtual ~FRunnableThread() = default;

		CORE_API virtual auto Kill(bool bShouldWait = false) -> void = 0;
		CORE_API virtual auto Suspend(bool bShouldPause = true) -> void = 0;
		CORE_API virtual auto Resume() -> void = 0;
		CORE_API virtual auto WaitForCompletion() -> void = 0;

		auto GetThreadId() const -> uint32 { return ThreadId.load(std::memory_order::relaxed); }
		auto GetThreadName() const -> const char* { return ThreadName.c_str(); }
		auto GetThreadRole() const -> EThreadRole { return ThreadRole; }

	protected:
		virtual auto CreateInternal(FRunnable* InRunnable, const char* InThreadName, uint32 InStackSize, EThreadPriority InThreadPriority, EThreadRole InThreadRole) -> bool = 0;

		CORE_API auto AsCurrentThread() -> void;

		std::atomic<uint32> ThreadId = 0;

		std::string ThreadName{};

		FRunnable* Runnable = nullptr;

		EThreadPriority ThreadPriority = EThreadPriority::Normal;

		EThreadRole ThreadRole = EThreadRole::Unknown;
	};

	CORE_API auto GetThreadRoleName(EThreadRole ThreadRole) -> const char*;

	CORE_API auto GetCurrentThread() -> FRunnableThread*;
	CORE_API auto GetCurrentThreadName() -> const char*;
	CORE_API auto GetCurrentThreadRole() -> EThreadRole;

	CORE_API auto IsInGameThread() -> bool;
	CORE_API auto IsInRenderingThread() -> bool;
	CORE_API auto IsInWorkerThread() -> bool;
	CORE_API auto IsInTaskThread() -> bool;

	CORE_API auto CheckGameThread() -> void;
	CORE_API auto CheckRenderingThread() -> void;
	CORE_API auto CheckThreadRole(EThreadRole ThreadRole) -> void;


} // namespace Durin
