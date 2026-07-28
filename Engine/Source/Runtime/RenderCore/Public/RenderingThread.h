#pragma once

#include "RenderCoreAPI.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	class FRHICommandListImmediate;

	// Lets the game thread wait until all render commands queued before the fence have completed.
	class FRenderCommandFence
	{
	public:
		FRenderCommandFence() = default;
		~FRenderCommandFence() = default;

		RENDERCORE_API auto BeginFence() -> void;

		RENDERCORE_API auto Wait() -> void;
		auto IsFenceComplete() const -> bool { return bIsComplete.load(std::memory_order_acquire); }
	private:
		std::atomic<bool> bIsComplete = true;
		std::condition_variable CV;
		std::mutex Mutex;
	};

	namespace FFrameSync
	{
		// Selects end-of-frame synchronization or a thread-only command drain.
		enum class EFlushMode
		{
			EndFrame,

			Threads
		};
		RENDERCORE_API auto Sync(EFlushMode FlushMode) -> void;
	}

	RENDERCORE_API auto InitRenderingThread() -> void;

	enum class ERenderCommandAdmissionState : uint8
	{
		Stopped,
		Running,
		Draining,
	};

	// Stops producer admission without discarding commands accepted before the transition.
	RENDERCORE_API auto CloseRenderCommandAdmission() -> void;

	// Atomically closes admission behind a final render-thread resource/RHI audit,
	// then waits until every previously accepted command has completed.
	RENDERCORE_API auto FinalizeRenderingThreadBeforeRHIExit() -> void;

	RENDERCORE_API auto ShutdownRenderingThread() -> void;

	// Waits for the rendering thread to finish processing all enqueued commands. Should only be used from the game thread.
	RENDERCORE_API auto FlushRenderingCommands() -> void;

	RENDERCORE_API auto GetRenderCommandAdmissionState()
		-> ERenderCommandAdmissionState;
	RENDERCORE_API auto GetNumPendingRenderCommands() -> size_t;

	// Owns the double-buffered command queue transferred from producers to the render thread.
	class FRenderThreadCommandPipe
	{
	public:
		template<typename RenderCommandTag, typename LambdaType>
		static auto Enqueue(LambdaType&& Lambda) -> void
		{
			Instance.EnqueueImpl(
				RenderCommandTag::GetName(), std::move(Lambda));
		}

		template<typename RenderCommandTag, typename LambdaType>
		static auto TryEnqueue(LambdaType&& Lambda) -> bool
		{
			return Instance.TryEnqueueImpl(
				RenderCommandTag::GetName(), std::move(Lambda));
		}

		static auto Launch() -> void
		{
			Instance.LaunchImpl();
		}

		static auto Wake() -> void
		{
			Instance.WakeImpl();
		}

		static auto StartAdmission() -> void
		{
			Instance.StartAdmissionImpl();
		}

		static auto CloseAdmission() -> void
		{
			Instance.CloseAdmissionImpl();
		}

		static auto CloseWithFinalCommand(
			const char* Name,
			std::function<void(FRHICommandListImmediate&)>&& Function) -> void
		{
			Instance.CloseWithFinalCommandImpl(Name, std::move(Function));
		}

		static auto DrainAcceptedCommands() -> void
		{
			Instance.DrainAcceptedCommandsImpl();
		}

		static auto MarkStopped() -> void
		{
			Instance.MarkStoppedImpl();
		}

		static auto GetAdmissionState() -> ERenderCommandAdmissionState
		{
			return Instance.GetAdmissionStateImpl();
		}

		static auto GetNumPendingCommands() -> size_t
		{
			return Instance.GetNumPendingCommandsImpl();
		}

	private:
		RENDERCORE_API auto EnqueueImpl(
			const char* Name,
			std::function<void(FRHICommandListImmediate&)>&& Function) -> void;
		RENDERCORE_API auto TryEnqueueImpl(
			const char* Name,
			std::function<void(FRHICommandListImmediate&)>&& Function) -> bool;

		RENDERCORE_API auto LaunchImpl() -> void;

		RENDERCORE_API auto WakeImpl() -> void;
		RENDERCORE_API auto StartAdmissionImpl() -> void;
		RENDERCORE_API auto CloseAdmissionImpl() -> void;
		RENDERCORE_API auto CloseWithFinalCommandImpl(
			const char* Name,
			std::function<void(FRHICommandListImmediate&)>&& Function) -> void;
		RENDERCORE_API auto DrainAcceptedCommandsImpl() -> void;
		RENDERCORE_API auto MarkStoppedImpl() -> void;
		RENDERCORE_API auto GetAdmissionStateImpl() const
			-> ERenderCommandAdmissionState;
		RENDERCORE_API auto GetNumPendingCommandsImpl() const -> size_t;

		static RENDERCORE_API FRenderThreadCommandPipe Instance;

		struct FCommand
		{
			FCommand(const char* InName, std::function<void(FRHICommandListImmediate&)>&& InFunction)
				: Name(InName)
				, Function(std::move(InFunction))
			{
			}
			const char* Name;
			std::function<void(FRHICommandListImmediate&)> Function;
		};

		std::array<std::vector<FCommand>, 2> CommandQueue;
		mutable std::mutex Mutex;
		std::condition_variable CommandAvailableCV;
		std::condition_variable DrainCompleteCV;
		uint32 ProduceIndex = 0;
		size_t ActiveCommandCount = 0;
		bool bWakeRequested = false;
		ERenderCommandAdmissionState AdmissionState =
			ERenderCommandAdmissionState::Stopped;
	};

	RENDERCORE_API auto GetImmediateCommandList_ForRenderCommand() -> FRHICommandListImmediate&;

	template<typename RenderCommandTag, typename LambdaType>
	auto EnqueueRenderCommand(LambdaType&& Lambda) -> void
	{
		if (IsInRenderingThread())
		{
			Lambda(GetImmediateCommandList_ForRenderCommand());
			return;
		}
		FRenderThreadCommandPipe::Enqueue<RenderCommandTag, LambdaType>(
			std::move(Lambda));
	}

	template<typename RenderCommandTag, typename LambdaType>
	void EnqueueRenderCommand(LambdaType& Lambda)
	{
		static_assert(sizeof(LambdaType) == 0, "EnqueueRenderCommand only accepts rvalue references.");
	}

#define DECLARE_RENDER_COMMAND_TAG(Type, Name) \
	struct Type \
	{ \
		static constexpr const char* GetName() { return STR(#Name); } \
	};

#define ENQUEUE_RENDER_COMMAND(Name) \
	DECLARE_RENDER_COMMAND_TAG(DURIN_JOIN(FRenderCommandTag_, DURIN_JOIN(Name, __LINE__)), Name) \
	EnqueueRenderCommand<DURIN_JOIN(FRenderCommandTag_, DURIN_JOIN(Name, __LINE__))>


} // namespace Durin
