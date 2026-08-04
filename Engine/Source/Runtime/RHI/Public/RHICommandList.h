#pragma once

#include "RHIAPI.h"
#include "DynamicRHI.h"
#include "RHIDefinitions.h"
#include "RHIResources.h"
#include "RHIShaderParameters.h"

namespace Durin
{
	struct FRHITextureCreateDesc;
	struct FRHIRenderPassInfo;

	class FRHITexture;
	class IRHICommandContext;
	class FRHICommandListExecutor;
	class FRHIViewport;
	class FRHIGraphicsPipelineState;
	class FRHICommandStorage;

	// Provides backend-neutral command recording shared by regular and immediate lists.
	class FRHICommandListBase
	{
	public:
		RHI_API virtual ~FRHICommandListBase();

		FRHICommandListBase(const FRHICommandListBase&) = delete;
		auto operator=(const FRHICommandListBase&) -> FRHICommandListBase& = delete;

		// Stores an owned callable in the command arena. Captures must own every
		// value they need when the callable eventually replays.
		template<typename CallableType>
		auto EnqueueLambda(CallableType&& Callable) -> void
		{
			using FStoredCallable = std::remove_cvref_t<CallableType>;
			static_assert(std::is_invocable_r_v<void, FStoredCallable&>);
			static_assert(alignof(FStoredCallable) <= alignof(std::max_align_t));

			const FCommandAllocation Allocation = AllocateCommand(
				sizeof(FStoredCallable),
				alignof(FStoredCallable),
				[](void* Payload, void*) {
					std::invoke(*static_cast<FStoredCallable*>(Payload));
				},
				[](void* Payload) {
					std::destroy_at(static_cast<FStoredCallable*>(Payload));
				});
			std::construct_at(
				static_cast<FStoredCallable*>(Allocation.Payload),
				std::forward<CallableType>(Callable));
			CommitCommand(Allocation.Node);
		}

		RHI_API auto IsRecording() const -> bool;
		RHI_API auto GetNumRecordedCommands() const -> size_t;

		// Call this function to switch between graphics and compute pipelines.
		RHI_API auto SwitchPipeline(ERHIPipeline Pipeline) -> void;

		RHI_API auto BeginRenderPass(const FRHIRenderPassInfo& Info, FName Name) -> void;
		RHI_API auto EndRenderPass() -> void;
		RHI_API auto BeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetTexture) -> void;
		RHI_API auto EndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void;
		RHI_API auto SetGraphicsPipelineState(FRHIGraphicsPipelineState& State) -> void;
		RHI_API auto BindVertexBuffer(uint32 StreamIndex, FRHIBuffer* VertexBuffer, uint32 Offset) -> void;
		RHI_API auto BindIndexBuffer(FRHIBuffer* Buffer, uint32 Offset) -> void;
		RHI_API auto DrawIndexed(uint32 IndexCount, uint32 StartIndexLocation, int32 VertexOffset) -> void;
		RHI_API auto SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void;
		RHI_API auto SetScissor(float MinX, float MinY, float Width, float Height) -> void;
		RHI_API auto WriteBuffer(FRHIBuffer* Buffer, const void* Data, uint32 Size, uint32 OffsetBytes) -> void;
		RHI_API auto UpdateUniformBuffer(FRHIBuffer* UniformBuffer, const void* Data, uint32 Size, uint32 Offset) -> void;
		RHI_API auto InitializeTexture(FRHITexture* Texture) -> void;
		RHI_API auto UpdateTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, const uint8* SourceData) -> void;
		RHI_API auto PushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void;
		RHI_API auto SetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void;

	protected:
		RHI_API FRHICommandListBase();
		RHI_API FRHICommandListBase(FRHICommandListBase&& Other) noexcept;
		RHI_API auto operator=(FRHICommandListBase&& Other) noexcept -> FRHICommandListBase&;

	private:
		enum class ERecordingState : uint8
		{
			Recording,
			Finished,
			Admitted,
			MovedFrom
		};

		struct FCommandAllocation
		{
			void* Node = nullptr;
			void* Payload = nullptr;
		};

		using FCommandFunction = void (*)(void*, void*);
		using FCommandDestroyFunction = void (*)(void*);

		RHI_API auto AllocateCommand(
			size_t PayloadSize,
			size_t PayloadAlignment,
			FCommandFunction Execute,
			FCommandDestroyFunction Destroy) -> FCommandAllocation;
		template<typename CommandType, typename... ArgumentTypes>
		auto RecordCommand(ArgumentTypes&&... Arguments) -> void
		{
			static_assert(alignof(CommandType) <= alignof(std::max_align_t));
			const FCommandAllocation Allocation = AllocateCommand(
				sizeof(CommandType),
				alignof(CommandType),
				[](void* Payload, void* ReplayContext) {
					static_cast<CommandType*>(Payload)->Execute(ReplayContext);
				},
				[](void* Payload) {
					std::destroy_at(static_cast<CommandType*>(Payload));
				});
			std::construct_at(
				static_cast<CommandType*>(Allocation.Payload),
				std::forward<ArgumentTypes>(Arguments)...);
			CommitCommand(Allocation.Node);
		}
		RHI_API auto CommitCommand(void* Node) -> void;
		auto DetachStorage() -> std::unique_ptr<FRHICommandStorage>;
		auto IsFinished() const -> bool;
		auto MarkAdmitted() -> void;

		std::unique_ptr<FRHICommandStorage> Storage;
		ERecordingState RecordingState = ERecordingState::Recording;
		ERHIPipeline ActivePipeline = ERHIPipeline::None;
		bool bInsideRenderPass = false;

		friend class FRHICommandList;
		friend class FRHICommandListExecutor;
	};

	// A finite movable recorder. FinishRecording seals it without submitting it.
	class FRHICommandList : public FRHICommandListBase
	{
	public:
		RHI_API FRHICommandList();
		RHI_API ~FRHICommandList() override;
		RHI_API FRHICommandList(FRHICommandList&& Other) noexcept;
		RHI_API auto operator=(FRHICommandList&& Other) noexcept -> FRHICommandList&;

		FRHICommandList(const FRHICommandList&) = delete;
		auto operator=(const FRHICommandList&) -> FRHICommandList& = delete;

		RHI_API auto FinishRecording() -> void;
		RHI_API auto IsFinished() const -> bool;

	protected:
		explicit FRHICommandList(bool bImmediate);
	};

	// Selects how far an immediate command-list flush advances queued RHI work.
	enum class EImmediateFlushType
	{
		WaitForOutstandingTasksOnly,
		DispatchToRHIThread,
		FlushRHIThread,
		FlushRHIThreadFlushResources
	};

	// Combines submission, resource cleanup, and frame-boundary work for an immediate flush.
	enum class ERHISubmitFlags
	{
		None = 0,
		SubmitToGPU = 1 << 0,
		DeleteResources = 1 << 1,
		FlushRHIThread = 1 << 2,
		EndFrame = 1 << 3,
	};

	ENUM_CLASS_FLAGS(ERHISubmitFlags)

	struct FRHICommandListExecutorStats
	{
		uint64 RecordedCommandCount = 0;
		uint64 RecordedPayloadBytes = 0;
		uint64 SubmittedBatchCount = 0;
		uint64 SubmissionGroupCount = 0;
		uint64 ReplayDurationNanoseconds = 0;
		uint64 WaitCount = 0;
		uint64 RejectedSubmissionCount = 0;
		uint64 PendingBatchCount = 0;
	};

	// Owns the primary timeline and immediate-only coordination operations.
	class FRHICommandListImmediate final : public FRHICommandList
	{
	public:
		FRHICommandListImmediate(const FRHICommandListImmediate&) = delete;
		auto operator=(const FRHICommandListImmediate&)
			-> FRHICommandListImmediate& = delete;
		FRHICommandListImmediate(FRHICommandListImmediate&&) = delete;
		auto operator=(FRHICommandListImmediate&&)
			-> FRHICommandListImmediate& = delete;

		RHI_API static auto Get() -> FRHICommandListImmediate&;
		RHI_API ~FRHICommandListImmediate() override;
		RHI_API auto QueueCommandList(FRHICommandList&& CommandList) -> void;
		RHI_API auto TryQueueCommandList(FRHICommandList&& CommandList) -> bool;
		RHI_API auto ImmediateFlush(EImmediateFlushType FlushType, ERHISubmitFlags SubmitFlags = ERHISubmitFlags::None) -> void;
		RHI_API auto LockBuffer(FRHIBuffer* Buffer, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void*;
		RHI_API auto UnlockBuffer(FRHIBuffer* Buffer) -> void;
		RHI_API auto AllocateDynamicUniformBuffer(const void* Data, uint32 Size) -> FRHIUniformBufferRange;
		RHI_API auto ReadTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, std::vector<uint8>& OutData) -> bool;
		RHI_API auto AcquireBackBuffer(FRHITexture* BackBuffer) -> void;
		RHI_API auto BlockUntilGPUIdle() -> void;

		auto FinishRecording() -> void = delete;

	private:
		class FLockState;

		explicit FRHICommandListImmediate(FRHICommandListExecutor& InExecutor);
		auto HasOpenBufferLocks() const -> bool;

		FRHICommandListExecutor* Executor = nullptr;
		std::unique_ptr<FLockState> LockState;

		friend class FRHICommandListExecutor;
	};

	class FRHICommandListFence
	{
	public:
		FRHICommandListFence() = default;

		RHI_API auto IsComplete() const -> bool;
		RHI_API auto Wait() const -> void;
		FORCEINLINE auto GetTargetSerial() const -> uint64 { return TargetSerial; }

	private:
		FRHICommandListFence(FRHICommandListExecutor& InExecutor, uint64 InTargetSerial);

		FRHICommandListExecutor* Executor = nullptr;
		uint64 TargetSerial = 0;

		friend class FRHICommandListExecutor;
	};

	// Owns the immediate command list and replays immutable batches inline.
	class FRHICommandListExecutor
	{
	public:
		RHI_API FRHICommandListExecutor();
		RHI_API explicit FRHICommandListExecutor(IRHICommandContext& InGraphicsContext);
		RHI_API ~FRHICommandListExecutor();

		FRHICommandListExecutor(const FRHICommandListExecutor&) = delete;
		auto operator=(const FRHICommandListExecutor&) -> FRHICommandListExecutor& = delete;

		RHI_API auto GetImmediateCommandList() -> FRHICommandListImmediate&;
		RHI_API auto Submit(const std::vector<FRHICommandList*>& AdditionalCmdLists, ERHISubmitFlags SubmitFlags) -> uint64;
		RHI_API auto CreateFence() -> FRHICommandListFence;
		RHI_API auto GetLastSubmittedSerial() const -> uint64;
		RHI_API auto GetCompletedSerial() const -> uint64;
		RHI_API auto GetStats() const -> FRHICommandListExecutorStats;

	private:
		class FState;

		auto TryQueueCommandList(FRHICommandList& CommandList) -> bool;
		auto SealImmediateSegment() -> void;
		auto IsSerialComplete(uint64 Serial) const -> bool;
		auto WaitForSerial(uint64 Serial) const -> void;
		auto GetSynchronousOperationContext(bool bFlushRecordedCommands) -> IRHICommandContext&;

		std::unique_ptr<FState> State;
		FRHICommandListImmediate CommandListImmediate;

		friend class FRHICommandListFence;
		friend class FRHICommandListImmediate;
	};

	extern RHI_API FRHICommandListExecutor GCommandListExecutor;

	FORCEINLINE TRefCountPtr<FRHITexture> RHICreateTexture(const FRHITextureCreateDesc& CreateDesc)
	{
		return GDynamicRHI->RHICreateTexture(FRHICommandListImmediate::Get(), CreateDesc);
	}

	FORCEINLINE TRefCountPtr<FRHISampler> RHICreateSampler(const FRHISamplerDesc& CreateDesc)
	{
		return GDynamicRHI->RHICreateSampler(CreateDesc);
	}

	FORCEINLINE TRefCountPtr<FRHIBuffer> RHICreateBuffer(const FRHIBufferCreateDesc& CreateDesc)
	{
		return GDynamicRHI->RHICreateBuffer(FRHICommandListImmediate::Get(), CreateDesc);
	}
} // namespace Durin
