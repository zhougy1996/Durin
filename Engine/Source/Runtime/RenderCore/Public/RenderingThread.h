#pragma once

#include "CoreMinimal.h"

#include "RenderCoreAPI.h"
#include "Threading/RunnableThread.h"
#include "Threading/TaskRuntime.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FRHICommandListFence;

	enum class ERenderCommandFenceMode : uint8
	{
		RenderThread,
		RHIThread,
	};

	// GameThread-owned fence for ordered render or RHI command completion.
	class FRenderCommandFence
	{
	public:
		RENDERCORE_API FRenderCommandFence();
		RENDERCORE_API ~FRenderCommandFence();

		FRenderCommandFence(const FRenderCommandFence&) = delete;
		auto operator=(const FRenderCommandFence&)
			-> FRenderCommandFence& = delete;
		FRenderCommandFence(FRenderCommandFence&&) = delete;
		auto operator=(FRenderCommandFence&&)
			-> FRenderCommandFence& = delete;

		RENDERCORE_API auto BeginFence(
			ERenderCommandFenceMode Mode =
				ERenderCommandFenceMode::RenderThread) -> void;

		RENDERCORE_API auto Wait(std::source_location Caller = std::source_location::current()) -> void;
		RENDERCORE_API auto IsFenceComplete() const -> bool;
	private:
		Tasks::FTask CompletionTask;
		// Published before CompletionTask completes; retained independently by the render command.
		std::shared_ptr<FRHICommandListFence> RHIFence;
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

	RENDERCORE_API auto ShutdownRenderingThread() -> void;
	RENDERCORE_API auto IsRenderingThreadRunning() -> bool;

	// GameThread barrier for accepted render/RHI work; a stopped queue is already drained.
	RENDERCORE_API auto FlushRenderingCommands() -> void;

	// Executes inline on the rendering thread. Command names must have static storage.
	RENDERCORE_API auto EnqueueRenderCommand(
		const char* Name, TMoveOnlyFunction<void(FRHICommandListImmediate&)> Function) -> void;

	// Always queues behind prior submissions, including from the rendering thread.
	RENDERCORE_API auto EnqueueRenderCommandDeferred(
		const char* Name, TMoveOnlyFunction<void(FRHICommandListImmediate&)> Function) -> void;

	template<typename RenderCommandTag, typename LambdaType>
	auto EnqueueRenderCommand(LambdaType&& Lambda) -> void
	{
		static_assert(!std::is_lvalue_reference_v<LambdaType>,
			"EnqueueRenderCommand only accepts rvalue references.");
		EnqueueRenderCommand(RenderCommandTag::GetName(), std::move(Lambda));
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
