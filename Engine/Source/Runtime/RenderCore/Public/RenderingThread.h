#pragma once
#include "HAL/RunnableThread.h"

namespace Doge
{
	class FRHICommandListImmediate;

	class FRenderCommandFence
	{
	public:
		FRenderCommandFence() = default;
		~FRenderCommandFence() = default;

		RENDERCORE_API auto BeginFence() -> void;

		RENDERCORE_API auto Wait() -> void;
	private:
		std::atomic<bool> bIsComplete = true;
		std::condition_variable CV;
		std::mutex Mutex;
	};

	namespace FFrameEndSync
	{
		// Wait for the render thread to finish processing all commands of N-1 Frame.
		RENDERCORE_API auto Sync() -> void;
	}

	RENDERCORE_API auto InitRenderingThread() -> void;

	RENDERCORE_API auto ShutdownRenderingThread() -> void;

	RENDERCORE_API auto FlushRenderingCommands() -> void;



	class FRenderThreadCommandPipe
	{
	public:
		template<typename RenderCommandTag, typename LambdaType>
		static auto Enqueue(LambdaType&& Lambda) -> void
		{
			Instance.EnqueueImpl(RenderCommandTag::GetName(), std::move(Lambda));
		}

		static auto Launch() -> void
		{
			Instance.LaunchImpl();
		}

	private:
		RENDERCORE_API auto EnqueueImpl(const CharT* Name, std::function<void(FRHICommandListImmediate&)>&& Function) -> void;

		RENDERCORE_API auto LaunchImpl() -> void;

		static RENDERCORE_API FRenderThreadCommandPipe Instance;

		struct FCommand
		{
			FCommand(const CharT* InName, std::function<void(FRHICommandListImmediate&)>&& InFunction)
				: Name(InName)
				, Function(std::move(InFunction))
			{
			}
			const CharT* Name;
			std::function<void(FRHICommandListImmediate&)> Function;
		};

		std::array<std::vector<FCommand>, 2> CommandQueue;
		std::mutex Mutex;
		uint32 ProduceIndex = 0;
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
		FRenderThreadCommandPipe::Enqueue<RenderCommandTag, LambdaType>(std::move(Lambda));
	}

	template<typename RenderCommandTag, typename LambdaType>
	void EnqueueRenderCommand(LambdaType& Lambda)
	{
		static_assert(sizeof(LambdaType) == 0, "EnqueueRenderCommand only accepts rvalue references.");
	}

#define DECLARE_RENDER_COMMAND_TAG(Type, Name) \
	struct Type \
	{ \
		static constexpr const CharT* GetName() { return STR(#Name); } \
	};

#define ENQUEUE_RENDER_COMMAND(Name) \
	DECLARE_RENDER_COMMAND_TAG(DOGE_JOIN(FRenderCommandTag_, DOGE_JOIN(Name, __LINE__)), Name) \
	EnqueueRenderCommand<DOGE_JOIN(FRenderCommandTag_, DOGE_JOIN(Name, __LINE__))>


} // namespace Doge