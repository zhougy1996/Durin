#include "Backend/RHIShaderBufferBinding.h"
#include "Backend/RHITextureUpload.h"
#include "Backend/RHIDynamicBufferWrite.h"
#include "RHICommandList.h"
#include "Backend/RHICreation.h"
#include "Backend/RHIValidation.h"
#include "Backend/RHICompletionBackend.h"
#include "Backend/RHIPipelineStateCacheBackend.h"

#include "DynamicRHI.h"
#include "RHIContext.h"
#include "RHIThread.h"
#include "Threading/RunnableThread.h"
#include "Profiling/Profiling.h"

namespace Durin
{
	static thread_local bool GExecutingRHICommands = false;
	auto IsExecutingRHICommands() -> bool { return GExecutingRHICommands; }
	namespace
	{
		std::atomic<uint64> GInvalidDiagnosticRegionCount = 0;

		auto BufferTransitionsOverlap(const FRHIBufferTransition& A,
			const FRHIBufferTransition& B) -> bool
		{
			return A.Buffer == B.Buffer && A.Offset < B.Offset + B.Size
				&& B.Offset < A.Offset + A.Size;
		}

		auto TextureTransitionsOverlap(const FRHITextureTransition& A,
			const FRHITextureTransition& B) -> bool
		{
			return A.Texture == B.Texture
				&& EnumHasAnyFlags(A.Range.Aspects, B.Range.Aspects)
				&& A.Range.FirstMip < B.Range.FirstMip + B.Range.NumMips
				&& B.Range.FirstMip < A.Range.FirstMip + A.Range.NumMips
				&& A.Range.FirstArrayLayer < B.Range.FirstArrayLayer + B.Range.NumArrayLayers
				&& B.Range.FirstArrayLayer < A.Range.FirstArrayLayer + A.Range.NumArrayLayers;
		}

		[[noreturn]] auto FatalPayloadSizeOverflow() -> void
		{
			DURIN_FATAL("Recorded RHI command payload byte count overflowed.");
			std::terminate();
		}

		auto CheckedAddPayloadBytes(size_t Left, size_t Right) -> size_t
		{
			if (Right > std::numeric_limits<size_t>::max() - Left)
			{
				FatalPayloadSizeOverflow();
			}
			return Left + Right;
		}

		auto CanonicalizeShaderParameters(
			std::span<const FRHIShaderParameterResource> Input,
			std::vector<FRHIShaderParameterResource>& Output,
			bool bFallible = false) -> bool
		{
			auto Fail = [bFallible](const char* Message) -> bool {
				if (!bFallible) checkf(false, "{}", Message);
				return false;
			};
			Output.assign(Input.begin(), Input.end());
			for (FRHIShaderParameterResource& Parameter : Output)
			{
				if (Parameter.ResourceIndex != INDEX_NONE_U32)
				{
					if (!Parameter.Resource || Parameter.Resource->GetResourceType() != ERHIResourceType::UniformBuffer)
						return Fail("Resource table binding requires a logical Uniform Buffer.");
					const auto* Uniform = static_cast<const FRHIUniformBuffer*>(Parameter.Resource);
					const auto Table = Uniform->GetResourceTable();
					const auto Index = Parameter.ResourceIndex;
					if (Index >= Table.size() || Index >= Uniform->GetLayout().Resources.size())
						return Fail("Uniform resource member is outside its layout.");
					const auto& Member = Uniform->GetLayout().Resources[Index];
					const auto IsUniform = [](ERHIBindingType Type) { return Type == ERHIBindingType::UniformBuffer || Type == ERHIBindingType::UniformBufferDynamic; };
					if (Member.Type != Parameter.Type && !(IsUniform(Member.Type) && IsUniform(Parameter.Type)))
						return Fail("Uniform resource member type does not match its shader binding.");
					Parameter.Resource = Table[Index].GetReference();
					Parameter.ResourceIndex = INDEX_NONE_U32;
					if (Parameter.Type == ERHIBindingType::Texture)
					{
						if (Parameter.Resource && Parameter.Resource->GetResourceType() == ERHIResourceType::TextureReference)
							Parameter.Resource = static_cast<FRHITextureReference*>(Parameter.Resource)->GetReferencedTexture_RenderThread();
						if (!Parameter.Resource && Member.FallbackResourceIndex < Table.size()) Parameter.Resource = Table[Member.FallbackResourceIndex].GetReference();
					}
					if (!Parameter.Resource) return Fail("Uniform resource member has no resource or fallback.");
				}
				if (Parameter.Resource == nullptr || Parameter.Type == ERHIBindingType::Sampler) continue;
				if (Parameter.Type == ERHIBindingType::Texture || Parameter.Type == ERHIBindingType::StorageImage)
				{
					if (Parameter.Resource->GetResourceType() == ERHIResourceType::TextureView) continue;
					auto* TextureResource = Parameter.Resource;
					if (TextureResource->GetResourceType() == ERHIResourceType::TextureReference)
					{
						TextureResource = static_cast<FRHITextureReference*>(TextureResource)->GetReferencedTexture_RenderThread();
					}
					if (!TextureResource || TextureResource->GetResourceType() != ERHIResourceType::Texture)
						return Fail("Shader texture binding requires a texture or texture view.");
					auto* Texture = static_cast<FRHITexture*>(TextureResource);
					const auto Usage = Parameter.Type == ERHIBindingType::StorageImage
						? ERHITextureViewUsage::Storage : ERHITextureViewUsage::Sampled;
					if (!IsTextureViewDescValid(Texture, MakeDefaultTextureViewDesc(*Texture, Usage)))
						return Fail("Shader texture does not support the requested binding.");
					Parameter.Resource = Texture;
					continue;
				}

				if (Parameter.Type == ERHIBindingType::UniformBuffer
					|| Parameter.Type == ERHIBindingType::UniformBufferDynamic
					|| Parameter.Type == ERHIBindingType::StorageBuffer)
				{
					if (Parameter.Resource->GetResourceType() == ERHIResourceType::UniformBuffer)
					{
						if (Parameter.Type == ERHIBindingType::StorageBuffer)
							return Fail("Uniform parameters cannot be bound as storage.");
						const auto* Buffer = static_cast<const FRHIUniformBuffer*>(Parameter.Resource);
						if (Parameter.Offset > Buffer->GetSize()) return Fail("Uniform offset exceeds its size.");
						if (!Parameter.Size) Parameter.Size = Buffer->GetSize() - Parameter.Offset;
						const auto* Caps = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
						const auto Alignment = std::max(16u, Caps ? Caps->MinUniformBufferOffsetAlignment : 0u);
						if (!Parameter.Size || Parameter.Size % 16 != 0 || Parameter.Offset % Alignment != 0
							|| Parameter.Size > Buffer->GetSize() - Parameter.Offset
							|| (Caps && Caps->MaxUniformBufferRange && Parameter.Size > Caps->MaxUniformBufferRange))
							return Fail("Uniform parameter range exceeds native binding limits.");
						continue;
					}
					const bool bView = Parameter.Resource->GetResourceType() == ERHIResourceType::BufferView;
					if (!bView && !IsBufferResource(Parameter.Resource))
						return Fail("Shader buffer binding requires a buffer or buffer view.");
					const auto [Buffer, Range] = ResolveShaderBufferBinding(Parameter);
					const bool bDynamic = Parameter.Type == ERHIBindingType::UniformBufferDynamic;
					const uint64 DynamicOffset = bDynamic ? Parameter.Offset : 0;
					const bool bStorage = Range.Type == ERHIBufferViewType::StructuredStorage
						|| Range.Type == ERHIBufferViewType::ByteAddressStorage;
					if (!(Parameter.Type == ERHIBindingType::StorageBuffer ? bStorage : Range.Type == ERHIBufferViewType::Uniform)
						|| !ValidateBufferViewDesc(Buffer, Range))
						return Fail("Shader buffer range is incompatible with its binding.");
					if (DynamicOffset > Buffer->GetSize() - Range.Offset
						|| Range.Size > Buffer->GetSize() - Range.Offset - DynamicOffset)
						return Fail("Dynamic uniform range exceeds its parent buffer.");
					continue;
				}
				return Fail("Unsupported shader parameter binding type.");
			}
			return true;
		}
	}

	auto FRHIShaderParameterBatch::Create(FRHIShader* InShader, std::span<const FRHIShaderParameterResource> InParameters)
		-> std::shared_ptr<const FRHIShaderParameterBatch>
	{
		if (!InShader || std::ranges::any_of(InParameters, [](const auto& Parameter) { return !Parameter.Resource; })) return {};
		auto Batch = std::shared_ptr<FRHIShaderParameterBatch>(new FRHIShaderParameterBatch);
		if (!CanonicalizeShaderParameters(InParameters, Batch->Parameters, true)) return {};
		Batch->Shader = InShader;
		Batch->Resources.reserve(Batch->Parameters.size());
		for (const auto& Parameter : Batch->Parameters) Batch->Resources.emplace_back(Parameter.Resource);
		return Batch;
	}

	auto FRHIShaderParameterBatch::GetRetainedPayloadBytes() const -> size_t
	{
		return CheckedAddPayloadBytes(Parameters.capacity() * sizeof(FRHIShaderParameterResource),
			Resources.capacity() * sizeof(TRefCountPtr<FRHIResource>));
	}

	class FRHICommandReplayContext
	{
	public:
		explicit FRHICommandReplayContext(IRHICommandContext* InGraphicsContext = nullptr)
			: GraphicsContextOverride(InGraphicsContext)
		{
		}

		auto SetStorageOwner(std::shared_ptr<void> Owner) -> void
		{
			for (auto* Context : StorageContexts) Context->RHISetReplayStorageOwner({});
			StorageContexts.clear();
			StorageOwner = std::move(Owner);
			if (StorageOwner) AttachStorage(GetOperationContext("Replay storage"));
		}

		auto BeginGPUSubmission(const FRHIGPUSubmissionDesc& Desc) -> void
		{
			require(!SubmissionContext);
			auto* Context = GetOperationContext("BeginGPUSubmission").RHIGetQueueContext(Desc.Queue);
			require(Context);
			AttachStorage(*Context);
			Context->RHIBeginGPUSubmission(Desc);
			SubmissionContext = Context;
			if (ActivePipeline != ERHIPipeline::None) ActiveContext = Context;
		}

		auto EndGPUSubmission(const FRHIGPUSyncPointRef& Signal) -> void
		{
			require(SubmissionContext);
			SubmissionContext->RHIEndGPUSubmission(Signal);
			SubmissionContext = nullptr;
			SwitchPipeline(ActivePipeline);
		}

		auto SwitchPipeline(ERHIPipeline Pipeline) -> void
		{
			ActivePipeline = Pipeline;
			switch (Pipeline)
			{
			case ERHIPipeline::Graphics:
			case ERHIPipeline::Compute:
				if (SubmissionContext)
				{
					ActiveContext = SubmissionContext;
				}
				else if (GraphicsContextOverride)
				{
					ActiveContext = GraphicsContextOverride;
				}
				else
				{
					checkf(GDynamicRHI,
						"SwitchPipeline replay requires an initialized dynamic RHI.");
					ActiveContext = GDynamicRHI->RHIGetDefaultContext();
				}
				checkf(ActiveContext,
					"SwitchPipeline replay could not resolve a shared command context.");
				break;
			case ERHIPipeline::None:
				ActiveContext = nullptr;
				break;
			default:
				ActiveContext = nullptr;
				checkf(false, "SwitchPipeline replay encountered an unsupported pipeline.");
				break;
			}
		}

		auto GetGraphicsContext(const char* CommandName) const
			-> IRHICommandContext&
		{
			checkf(ActivePipeline == ERHIPipeline::Graphics && ActiveContext,
				"%s replay requires an active graphics pipeline.", CommandName);
			return *ActiveContext;
		}

		auto GetComputeContext(const char* CommandName) const
			-> IRHICommandContext&
		{
			checkf(ActivePipeline == ERHIPipeline::Compute && ActiveContext,
				"%s replay requires an active compute pipeline.", CommandName);
			return *ActiveContext;
		}

		auto GetPipelineContext(const char* CommandName) const
			-> IRHICommandContext&
		{
			checkf((ActivePipeline == ERHIPipeline::Graphics
				|| ActivePipeline == ERHIPipeline::Compute) && ActiveContext,
				"%s replay requires an active graphics or compute pipeline.", CommandName);
			return *ActiveContext;
		}

		auto GetOperationContext(const char* OperationName) const
			-> IRHICommandContext&
		{
			if (SubmissionContext) return *SubmissionContext;
			IRHICommandContext* Context = GraphicsContextOverride;
			if (!Context)
			{
				checkf(GDynamicRHI,
					"%s requires an initialized dynamic RHI.", OperationName);
				Context = GDynamicRHI->RHIGetDefaultContext();
			}
			checkf(Context, "%s could not resolve an RHI context.", OperationName);
			return *Context;
		}

		auto HasGraphicsContextOverride() const -> bool
		{
			return GraphicsContextOverride != nullptr;
		}

	private:
		auto AttachStorage(IRHICommandContext& Context) -> void
		{
			if (StorageOwner && !std::ranges::contains(StorageContexts, &Context))
			{
				StorageContexts.push_back(&Context);
				Context.RHISetReplayStorageOwner(StorageOwner);
			}
		}
		std::shared_ptr<void> StorageOwner;
		std::vector<IRHICommandContext*> StorageContexts;
		IRHICommandContext* SubmissionContext = nullptr;
		IRHICommandContext* GraphicsContextOverride = nullptr;
		IRHICommandContext* ActiveContext = nullptr;
		ERHIPipeline ActivePipeline = ERHIPipeline::None;
	};

	class FRHICommandStorage
	{
	public:
		using FCommandFunction = void (*)(void*, void*);
		using FCommandDestroyFunction = void (*)(void*);

		struct FAllocation
		{
			void* Node = nullptr;
			void* Payload = nullptr;
		};

		FRHICommandStorage() = default;
		auto Append(FRHICommandStorage& Other) -> void
		{
			check(!PendingNode && !Other.PendingNode);
			// Reserve before transferring ownership so a failed allocation leaves both lists intact.
			Blocks.reserve(Blocks.size() + Other.Blocks.size());
			const size_t NewCount = CheckedAddPayloadBytes(CommandCount, Other.CommandCount);
			const size_t NewBytes = CheckedAddPayloadBytes(PayloadBytes, Other.PayloadBytes);
			for (auto& Block : Other.Blocks) Blocks.push_back(std::move(Block));
			if (Other.Head)
			{
				if (Tail) Tail->Next = Other.Head;
				else Head = Other.Head;
				Tail = Other.Tail;
			}
			CommandCount = NewCount;
			PayloadBytes = NewBytes;
			Other.Head = Other.Tail = nullptr;
			Other.CommandCount = Other.PayloadBytes = 0;
		}
		FRHICommandStorage(const FRHICommandStorage&) = delete;
		auto operator=(const FRHICommandStorage&) -> FRHICommandStorage& = delete;

		~FRHICommandStorage()
		{
			DestroyCommands();
		}

		auto Allocate(
			size_t PayloadSize,
			size_t PayloadAlignment,
			FCommandFunction Execute,
			FCommandDestroyFunction Destroy) -> FAllocation
		{
			check(PendingNode == nullptr);
			check(PayloadSize > 0);
			check(PayloadAlignment > 0);
			check(PayloadAlignment <= alignof(std::max_align_t));
			check((PayloadAlignment & (PayloadAlignment - 1)) == 0);

			const size_t AllocationSize = sizeof(FCommandNode)
				+ PayloadAlignment - 1 + PayloadSize;
			void* Allocation = AllocateBytes(
				AllocationSize, alignof(FCommandNode));
			auto* Node = std::construct_at(
				static_cast<FCommandNode*>(Allocation));
			std::byte* PayloadBegin = reinterpret_cast<std::byte*>(Node)
				+ sizeof(FCommandNode);
			const uintptr_t AlignedPayload =
				(reinterpret_cast<uintptr_t>(PayloadBegin)
					+ PayloadAlignment - 1)
				& ~(static_cast<uintptr_t>(PayloadAlignment) - 1);
			Node->Payload = reinterpret_cast<void*>(AlignedPayload);
			Node->PayloadSize = PayloadSize;
			Node->Execute = Execute;
			Node->Destroy = Destroy;
			PendingNode = Node;
			return {Node, Node->Payload};
		}

		auto Commit(void* OpaqueNode, size_t OwnedPayloadBytes) -> void
		{
			auto* Node = static_cast<FCommandNode*>(OpaqueNode);
			check(Node != nullptr && Node == PendingNode);
			Node->OwnedPayloadBytes = OwnedPayloadBytes;
			if (Tail)
			{
				Tail->Next = Node;
			}
			else
			{
				Head = Node;
			}
			Tail = Node;
			PendingNode = nullptr;
			++CommandCount;
			PayloadBytes = CheckedAddPayloadBytes(
				PayloadBytes,
				CheckedAddPayloadBytes(
					Node->PayloadSize, Node->OwnedPayloadBytes));
		}

		auto Replay(FRHICommandReplayContext& ReplayContext) -> void
		{
			check(PendingNode == nullptr);
			for (FCommandNode* Node = Head; Node; Node = Node->Next)
			{
				Node->Execute(Node->Payload, &ReplayContext);
			}
		}

		auto GetCommandCount() const -> size_t
		{
			return CommandCount;
		}

		auto GetPayloadBytes() const -> size_t
		{
			return PayloadBytes;
		}

	private:
		struct alignas(std::max_align_t) FCommandNode
		{
			FCommandNode* Next = nullptr;
			void* Payload = nullptr;
			size_t PayloadSize = 0;
			size_t OwnedPayloadBytes = 0;
			FCommandFunction Execute = nullptr;
			FCommandDestroyFunction Destroy = nullptr;
		};

		struct FBlock
		{
			FBlock() = default;

			explicit FBlock(size_t InSize)
				: Data(static_cast<std::byte*>(::operator new(
					InSize,
					std::align_val_t{alignof(std::max_align_t)})))
				, Size(InSize)
			{
			}

			FBlock(const FBlock&) = delete;
			auto operator=(const FBlock&) -> FBlock& = delete;

			FBlock(FBlock&& Other) noexcept
				: Data(std::exchange(Other.Data, nullptr))
				, Size(std::exchange(Other.Size, 0))
				, Used(std::exchange(Other.Used, 0))
			{
			}

			auto operator=(FBlock&& Other) noexcept -> FBlock&
			{
				if (this != &Other)
				{
					Release();
					Data = std::exchange(Other.Data, nullptr);
					Size = std::exchange(Other.Size, 0);
					Used = std::exchange(Other.Used, 0);
				}
				return *this;
			}

			~FBlock()
			{
				Release();
			}

			auto Release() -> void
			{
				if (Data)
				{
					::operator delete(
						Data,
						std::align_val_t{alignof(std::max_align_t)});
					Data = nullptr;
				}
			}

			std::byte* Data = nullptr;
			size_t Size = 0;
			size_t Used = 0;
		};

		static constexpr size_t DefaultBlockSize = 16 * 1024;

		auto AllocateBytes(size_t Size, size_t Alignment) -> void*
		{
			check(Alignment <= alignof(std::max_align_t));
			auto TryAllocate = [Size, Alignment](FBlock& Block) -> void* {
				const uintptr_t Begin = reinterpret_cast<uintptr_t>(Block.Data)
					+ Block.Used;
				const uintptr_t Aligned = (Begin + Alignment - 1)
					& ~(static_cast<uintptr_t>(Alignment) - 1);
				const size_t NewUsed = static_cast<size_t>(
					Aligned - reinterpret_cast<uintptr_t>(Block.Data)) + Size;
				if (NewUsed > Block.Size)
				{
					return nullptr;
				}
				Block.Used = NewUsed;
				return reinterpret_cast<void*>(Aligned);
			};

			if (!Blocks.empty())
			{
				if (void* Allocation = TryAllocate(Blocks.back()))
				{
					return Allocation;
				}
			}

			Blocks.emplace_back(std::max(DefaultBlockSize, Size + Alignment));
			void* Allocation = TryAllocate(Blocks.back());
			check(Allocation);
			return Allocation;
		}

		auto DestroyCommands() -> void
		{
			check(PendingNode == nullptr);
			for (FCommandNode* Node = Head; Node;)
			{
				FCommandNode* Next = Node->Next;
				Node->Destroy(Node->Payload);
				std::destroy_at(Node);
				Node = Next;
			}
			Head = nullptr;
			Tail = nullptr;
			CommandCount = 0;
			PayloadBytes = 0;
		}

		std::vector<FBlock> Blocks;
		FCommandNode* Head = nullptr;
		FCommandNode* Tail = nullptr;
		FCommandNode* PendingNode = nullptr;
		size_t CommandCount = 0;
		size_t PayloadBytes = 0;
	};

	namespace
	{
		auto GetReplayContext(void* OpaqueContext)
			-> FRHICommandReplayContext&
		{
			check(OpaqueContext);
			return *static_cast<FRHICommandReplayContext*>(OpaqueContext);
		}

		// Begin/end command nodes and the recorder share this lease. An early
		// payload retirement cannot cancel an end marker still waiting for replay.
		struct FGPUSubmissionRecordingLease final
		{
			FRHIQueueId Queue;
			FRHIGPUSyncPointRef Signal = FRHIGPUSyncPoint::Create();
			~FGPUSubmissionRecordingLease() { FRHIGPUSyncPointBackend::CancelUnassociated(Signal); }
		};

		struct FBeginGPUSubmissionCommand final
		{
			FRHIGPUSubmissionDesc Desc;
			std::shared_ptr<FGPUSubmissionRecordingLease> Lease;
			auto Execute(void* Context) -> void
			{ GetReplayContext(Context).BeginGPUSubmission(Desc); }
			auto GetOwnedPayloadBytes() const -> size_t
			{ return Desc.Waits.capacity() * sizeof(FRHIGPUSyncPointRef); }
		};
		struct FEndGPUSubmissionCommand final
		{
			std::shared_ptr<FGPUSubmissionRecordingLease> Lease;
			auto Execute(void* Context) -> void
			{ GetReplayContext(Context).EndGPUSubmission(Lease->Signal); }
		};

		struct FQueueOwnershipCommand
		{
			std::shared_ptr<FRHIQueueTransfer> Transfer;
			bool bRelease;
			auto Execute(void* Context) -> void
			{
				auto& RHIContext = GetReplayContext(Context).GetOperationContext("QueueOwnership");
				if (bRelease) RHIContext.RHIReleaseQueueOwnership(Transfer);
				else RHIContext.RHIAcquireQueueOwnership(Transfer);
			}
		};

		struct FSwitchPipelineCommand
		{
			explicit FSwitchPipelineCommand(ERHIPipeline InPipeline)
				: Pipeline(InPipeline)
			{
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).SwitchPipeline(Pipeline);
			}

			ERHIPipeline Pipeline;
		};

		struct FBeginRenderPassCommand
		{
			FBeginRenderPassCommand(const FRHIRenderPassInfo& InInfo, FName InName)
				: Info(InInfo)
				, Name(InName)
			{
				for (uint32 Index = 0; Index < MaxSimultaneousRenderTargets; ++Index)
				{
					ColorRenderTargets[Index] = Info.ColorRenderTargets[Index];
					ColorResolveTargets[Index] = Info.ColorResolveTargets[Index];
					ColorRenderTargetViews[Index] = Info.ColorRenderTargetViews[Index];
					ColorResolveTargetViews[Index] = Info.ColorResolveTargetViews[Index];
				}
				DepthStencilRenderTarget = Info.DepthStencilRenderTarget;
				DepthStencilRenderTargetView = Info.DepthStencilRenderTargetView;
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetGraphicsContext("BeginRenderPass")
					.RHIBeginRenderPass(Info, Name);
			}

			FRHIRenderPassInfo Info;
			FName Name;
			std::array<TRefCountPtr<FRHITexture>, MaxSimultaneousRenderTargets>
				ColorRenderTargets;
			std::array<TRefCountPtr<FRHITexture>, MaxSimultaneousRenderTargets>
				ColorResolveTargets;
			TRefCountPtr<FRHITexture> DepthStencilRenderTarget;
			std::array<FTextureViewRHIRef, MaxSimultaneousRenderTargets> ColorRenderTargetViews;
			std::array<FTextureViewRHIRef, MaxSimultaneousRenderTargets> ColorResolveTargetViews;
			FTextureViewRHIRef DepthStencilRenderTargetView;
		};

		struct FBeginDiagnosticRegionCommand
		{
			explicit FBeginDiagnosticRegionCommand(std::string_view InName)
				: Name(InName)
			{
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetOperationContext("BeginDiagnosticRegion")
					.RHIBeginDiagnosticRegion(Name);
			}

			auto GetOwnedPayloadBytes() const -> size_t { return Name.size(); }

			std::string Name;
		};

		struct FEndDiagnosticRegionCommand
		{
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetOperationContext("EndDiagnosticRegion")
					.RHIEndDiagnosticRegion();
			}
		};

		struct FGPUTimingRecordingReservation
		{
			explicit FGPUTimingRecordingReservation(FRHIGPUTimingQuery* InQuery)
				: Query(InQuery) {}
			~FGPUTimingRecordingReservation()
			{
				if (Query) Query->CancelRecording();
			}
			TRefCountPtr<FRHIGPUTimingQuery> Query;
		};

		struct FBeginGPUTimingQueryCommand
		{
			explicit FBeginGPUTimingQueryCommand(
				std::shared_ptr<FGPUTimingRecordingReservation> InReservation)
				: Reservation(std::move(InReservation)) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetOperationContext("BeginGPUTimingQuery")
					.RHIBeginGPUTimingQuery(Reservation->Query.GetReference());
			}
			std::shared_ptr<FGPUTimingRecordingReservation> Reservation;
		};

		struct FEndGPUTimingQueryCommand
		{
			explicit FEndGPUTimingQueryCommand(
				std::shared_ptr<FGPUTimingRecordingReservation> InReservation)
				: Reservation(std::move(InReservation)) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetOperationContext("EndGPUTimingQuery")
					.RHIEndGPUTimingQuery(Reservation->Query.GetReference());
			}
			std::shared_ptr<FGPUTimingRecordingReservation> Reservation;
		};

		struct FEndRenderPassCommand
		{
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetGraphicsContext("EndRenderPass")
					.RHIEndRenderPass();
			}
		};

		struct FBeginDrawingViewportCommand
		{
			FBeginDrawingViewportCommand(
				FRHIViewport* InViewport,
				FRHITexture* InRenderTargetTexture)
				: Viewport(InViewport)
				, RenderTargetTexture(InRenderTargetTexture)
			{
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetGraphicsContext("BeginDrawingViewport")
					.RHIBeginDrawingViewport(
						Viewport.GetReference(), RenderTargetTexture.GetReference());
			}

			TRefCountPtr<FRHIViewport> Viewport;
			TRefCountPtr<FRHITexture> RenderTargetTexture;
		};

		struct FEndDrawingViewportCommand
		{
			FEndDrawingViewportCommand(
				FRHIViewport* InViewport,
				bool bInPresent,
				bool bInLockToVsync)
				: Viewport(InViewport)
				, bPresent(bInPresent)
				, bLockToVsync(bInLockToVsync)
			{
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetGraphicsContext("EndDrawingViewport")
					.RHIEndDrawingViewport(
						Viewport.GetReference(), bPresent, bLockToVsync);
			}

			TRefCountPtr<FRHIViewport> Viewport;
			bool bPresent;
			bool bLockToVsync;
		};

		struct FSetGraphicsPipelineStateCommand
		{
			explicit FSetGraphicsPipelineStateCommand(
				FRHIGraphicsPipelineState& InState)
				: State(&InState)
			{
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetGraphicsContext("SetGraphicsPipelineState")
					.RHISetGraphicsPipelineState(*State);
			}

			TRefCountPtr<FRHIGraphicsPipelineState> State;
		};

		struct FSetComputePipelineStateCommand
		{
			explicit FSetComputePipelineStateCommand(
				FRHIComputePipelineState& InState)
				: State(&InState)
			{
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetComputeContext("SetComputePipelineState")
					.RHISetComputePipelineState(*State);
			}

			TRefCountPtr<FRHIComputePipelineState> State;
		};
		struct FWaitForPipelineCommand
		{
			FPipelineCompilation Request;
			auto Execute(void*) -> void { FRHIPipelineStateCacheBackend::WaitForRequiredPipeline(Request); }
		};

		struct FSetCachedPipelineStateCommand
		{
			explicit FSetCachedPipelineStateCommand(FPipelineCompilation InRequest, std::shared_ptr<FPipelineState> InPipeline)
				: Request(std::move(InRequest)), Pipeline(std::move(InPipeline)) {}
			auto Execute(void* Context) -> void
			{
				FRHIPipelineStateCacheBackend::WaitForRequiredPipeline(Request);
				if (Request.IsCompute())
				{
					auto Native = FRHIPipelineStateCacheBackend::GetComputePipeline(Request);
					if (!Native) RHITerminateOnFailure("Compute PSO", "Required pipeline is not ready for replay");
					GetReplayContext(Context).GetComputeContext("SetComputePipelineState").RHISetComputePipelineState(*Native);
				}
				else
				{
					auto Native = FRHIPipelineStateCacheBackend::GetGraphicsPipeline(Request);
					if (!Native) RHITerminateOnFailure("Graphics PSO", "Required pipeline is not ready for replay");
					GetReplayContext(Context).GetGraphicsContext("SetGraphicsPipelineState").RHISetGraphicsPipelineState(*Native);
				}
			}
			FPipelineCompilation Request;
			std::shared_ptr<FPipelineState> Pipeline;
		};

		struct FBindVertexBufferCommand
		{
			FBindVertexBufferCommand(
				uint32 InStreamIndex,
				FRHIBuffer* InBuffer,
				uint32 InOffset)
				: StreamIndex(InStreamIndex), Buffer(InBuffer), Offset(InOffset)
			{
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetGraphicsContext("BindVertexBuffer")
					.RHIBindVertexBuffer(StreamIndex, Buffer.GetReference(), Offset);
			}

			uint32 StreamIndex;
			TRefCountPtr<FRHIBuffer> Buffer;
			uint32 Offset;
		};

		struct FBindIndexBufferCommand
		{
			FBindIndexBufferCommand(FRHIBuffer* InBuffer, uint32 InOffset)
				: Buffer(InBuffer), Offset(InOffset)
			{
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetGraphicsContext("BindIndexBuffer")
					.RHIBindIndexBuffer(Buffer.GetReference(), Offset);
			}

			TRefCountPtr<FRHIBuffer> Buffer;
			uint32 Offset;
		};

		struct FBufferTransitionCommand
		{
			explicit FBufferTransitionCommand(
				std::span<const FRHIBufferTransition> InTransitions)
				: Transitions(InTransitions.begin(), InTransitions.end())
			{
				Resources.reserve(Transitions.size());
				for (const FRHIBufferTransition& Transition : Transitions)
				{
					Resources.emplace_back(Transition.Buffer);
				}
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetOperationContext("TransitionBuffers")
					.RHITransitionBuffers(Transitions);
			}

			auto GetOwnedPayloadBytes() const -> size_t
			{
				return CheckedAddPayloadBytes(
					Transitions.capacity() * sizeof(Transitions.front()),
					Resources.capacity() * sizeof(Resources.front()));
			}

			std::vector<FRHIBufferTransition> Transitions;
			std::vector<TRefCountPtr<FRHIBuffer>> Resources;
		};

		struct FTextureTransitionCommand
		{
			explicit FTextureTransitionCommand(
				std::span<const FRHITextureTransition> InTransitions)
				: Transitions(InTransitions.begin(), InTransitions.end())
			{
				Resources.reserve(Transitions.size());
				for (const FRHITextureTransition& Transition : Transitions)
				{
					Resources.emplace_back(Transition.Texture);
				}
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetOperationContext("TransitionTextures")
					.RHITransitionTextures(Transitions);
			}

			auto GetOwnedPayloadBytes() const -> size_t
			{
				return CheckedAddPayloadBytes(
					Transitions.capacity() * sizeof(Transitions.front()),
					Resources.capacity() * sizeof(Resources.front()));
			}

			std::vector<FRHITextureTransition> Transitions;
			std::vector<TRefCountPtr<FRHITexture>> Resources;
		};

		struct FBeginTransitionCommand
		{
			explicit FBeginTransitionCommand(std::shared_ptr<FRHITransition> InTransition)
				: Transition(std::move(InTransition)) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetOperationContext("BeginTransition")
					.RHIBeginTransition(Transition);
			}
			std::shared_ptr<FRHITransition> Transition;
		};

		struct FAliasingBarrierCommand
		{
			FAliasingBarrierCommand(FRHIResource* InBefore, FRHIResource* InAfter)
				: Before(InBefore), After(InAfter) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetOperationContext("AliasingBarrier")
					.RHIAliasingBarrier({Before.GetReference(), After.GetReference()});
			}
			TRefCountPtr<FRHIResource> Before, After;
		};

		struct FEndTransitionCommand
		{
			explicit FEndTransitionCommand(std::shared_ptr<FRHITransition> InTransition)
				: Transition(std::move(InTransition)) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetOperationContext("EndTransition")
					.RHIEndTransition(Transition);
			}
			std::shared_ptr<FRHITransition> Transition;
		};

		template<typename RegionType>
		auto GetRegionPayloadBytes(const std::vector<RegionType>& Regions) -> size_t
		{
			return Regions.capacity() * sizeof(Regions.front());
		}

		struct FCopyBufferCommand
		{
			FCopyBufferCommand(FRHIBuffer* InSource, FRHIBuffer* InDestination,
				std::span<const FRHIBufferCopyRegion> InRegions)
				: Source(InSource), Destination(InDestination), Regions(InRegions.begin(), InRegions.end()) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetOperationContext("CopyBuffer")
					.RHICopyBuffer(Source.GetReference(), Destination.GetReference(), Regions);
			}
			auto GetOwnedPayloadBytes() const -> size_t { return GetRegionPayloadBytes(Regions); }
			TRefCountPtr<FRHIBuffer> Source;
			TRefCountPtr<FRHIBuffer> Destination;
			std::vector<FRHIBufferCopyRegion> Regions;
		};

		struct FCopyBufferToTextureCommand
		{
			FCopyBufferToTextureCommand(FRHIBuffer* InSource, FRHITexture* InDestination,
				std::span<const FRHIBufferTextureCopyRegion> InRegions)
				: Source(InSource), Destination(InDestination), Regions(InRegions.begin(), InRegions.end()) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetOperationContext("CopyBufferToTexture")
					.RHICopyBufferToTexture(Source.GetReference(), Destination.GetReference(), Regions);
			}
			auto GetOwnedPayloadBytes() const -> size_t { return GetRegionPayloadBytes(Regions); }
			TRefCountPtr<FRHIBuffer> Source;
			TRefCountPtr<FRHITexture> Destination;
			std::vector<FRHIBufferTextureCopyRegion> Regions;
		};

		struct FCopyTextureToBufferCommand
		{
			FCopyTextureToBufferCommand(FRHITexture* InSource, FRHIBuffer* InDestination,
				std::span<const FRHIBufferTextureCopyRegion> InRegions)
				: Source(InSource), Destination(InDestination), Regions(InRegions.begin(), InRegions.end()) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetOperationContext("CopyTextureToBuffer")
					.RHICopyTextureToBuffer(Source.GetReference(), Destination.GetReference(), Regions);
			}
			auto GetOwnedPayloadBytes() const -> size_t { return GetRegionPayloadBytes(Regions); }
			TRefCountPtr<FRHITexture> Source;
			TRefCountPtr<FRHIBuffer> Destination;
			std::vector<FRHIBufferTextureCopyRegion> Regions;
		};

		struct FCopyTextureCommand
		{
			FCopyTextureCommand(FRHITexture* InSource, FRHITexture* InDestination,
				std::span<const FRHITextureCopyRegion> InRegions)
				: Source(InSource), Destination(InDestination), Regions(InRegions.begin(), InRegions.end()) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetOperationContext("CopyTexture")
					.RHICopyTexture(Source.GetReference(), Destination.GetReference(), Regions);
			}
			auto GetOwnedPayloadBytes() const -> size_t { return GetRegionPayloadBytes(Regions); }
			TRefCountPtr<FRHITexture> Source;
			TRefCountPtr<FRHITexture> Destination;
			std::vector<FRHITextureCopyRegion> Regions;
		};

		struct FDrawCommand
		{
			explicit FDrawCommand(FRHIDrawArguments InArguments)
				: Arguments(InArguments) {}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetGraphicsContext("Draw")
					.RHIDraw(Arguments);
			}

			FRHIDrawArguments Arguments;
		};

		struct FDrawIndexedCommand
		{
			explicit FDrawIndexedCommand(FRHIDrawIndexedArguments InArguments)
				: Arguments(InArguments) {}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetGraphicsContext("DrawIndexed")
					.RHIDrawIndexed(Arguments);
			}

			FRHIDrawIndexedArguments Arguments;
		};

		struct FDrawIndirectCommand
		{
			FDrawIndirectCommand(FRHIBuffer* InBuffer, uint64 InOffset)
				: Buffer(InBuffer), Offset(InOffset) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetGraphicsContext("DrawIndirect")
					.RHIDrawIndirect(Buffer.GetReference(), Offset);
			}
			TRefCountPtr<FRHIBuffer> Buffer;
			uint64 Offset = 0;
		};

		struct FDrawIndexedIndirectCommand
		{
			FDrawIndexedIndirectCommand(FRHIBuffer* InBuffer, uint64 InOffset)
				: Buffer(InBuffer), Offset(InOffset) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetGraphicsContext("DrawIndexedIndirect")
					.RHIDrawIndexedIndirect(Buffer.GetReference(), Offset);
			}
			TRefCountPtr<FRHIBuffer> Buffer;
			uint64 Offset = 0;
		};

		struct FDispatchCommand
		{
			FDispatchCommand(uint32 InX, uint32 InY, uint32 InZ)
				: X(InX), Y(InY), Z(InZ) {}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetComputeContext("Dispatch")
					.RHIDispatch(X, Y, Z);
			}

			uint32 X;
			uint32 Y;
			uint32 Z;
		};

		struct FDispatchIndirectCommand
		{
			FDispatchIndirectCommand(FRHIBuffer* InBuffer, uint64 InOffset)
				: Buffer(InBuffer), Offset(InOffset) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetComputeContext("DispatchIndirect")
					.RHIDispatchIndirect(Buffer.GetReference(), Offset);
			}
			TRefCountPtr<FRHIBuffer> Buffer;
			uint64 Offset = 0;
		};

		struct FSetViewportCommand
		{
			FSetViewportCommand(float A, float B, float C, float D, float E, float F)
				: MinX(A), MinY(B), MinZ(C), MaxX(D), MaxY(E), MaxZ(F)
			{
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetGraphicsContext("SetViewport")
					.RHISetViewport(MinX, MinY, MinZ, MaxX, MaxY, MaxZ);
			}

			float MinX, MinY, MinZ, MaxX, MaxY, MaxZ;
		};

		struct FSetScissorCommand
		{
			FSetScissorCommand(float A, float B, float C, float D)
				: MinX(A), MinY(B), Width(C), Height(D)
			{
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetGraphicsContext("SetScissor")
					.RHISetScissor(MinX, MinY, Width, Height);
			}

			float MinX, MinY, Width, Height;
		};

		struct FSetDepthBiasCommand
		{
			FSetDepthBiasCommand(float InConstantFactor, float InClamp,
				float InSlopeFactor)
				: ConstantFactor(InConstantFactor), Clamp(InClamp),
				  SlopeFactor(InSlopeFactor)
			{
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetGraphicsContext("SetDepthBias")
					.RHISetDepthBias(ConstantFactor, Clamp, SlopeFactor);
			}

			float ConstantFactor, Clamp, SlopeFactor;
		};

		struct FPushConstantsCommand
		{
			FPushConstantsCommand(
				EShaderStageFlags InStageFlags,
				uint32 InOffset,
				uint32 InSize,
				const void* InData)
				: StageFlags(InStageFlags), Offset(InOffset), Data(InSize)
			{
				check(InSize == 0 || InData);
				if (InSize != 0)
				{
					std::memcpy(Data.data(), InData, InSize);
				}
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetPipelineContext("PushConstants")
					.RHIPushConstants(
						StageFlags, Offset, static_cast<uint32>(Data.size()), Data.data());
			}

			auto GetOwnedPayloadBytes() const -> size_t
			{
				return Data.capacity() * sizeof(Data.front());
			}

			EShaderStageFlags StageFlags;
			uint32 Offset;
			FByteBuffer Data;
		};

		struct FBufferWriteCommand
		{
			TRefCountPtr<FRHIBuffer> Buffer;
			uint32 Offset;
			std::shared_ptr<const FRHIBufferUploadData> Data;
			bool bDiscard = false;
			auto Execute(void* ReplayContext) -> void
			{
				auto& Context = GetReplayContext(ReplayContext).GetOperationContext("WriteBuffer");
				if (bDiscard) Buffer->PrepareDiscardWrite(Context);
				Context.RHIWriteBuffer(Buffer.GetReference(), Offset, Data->GetData());
			}
			auto GetOwnedPayloadBytes() const -> size_t { return Data->GetOwnedPayloadBytes(); }
		};

		struct FBufferMappedWriteCommand
		{
			TRefCountPtr<FRHIBuffer> Buffer;
			uint32 Offset;
			uint32 Size;
			std::shared_ptr<FRHIBufferMappedWrite> Write;
			auto Execute(void* ReplayContext) -> void
			{
				Write->Commit(GetReplayContext(ReplayContext).GetOperationContext("MappedWriteBuffer"),
					Buffer.GetReference(), Offset, Size);
			}
		};

		struct FTextureReadbackCommand
		{
			TRefCountPtr<FRHITexture> Texture;
			uint32 MipIndex;
			uint32 ArraySlice;
			std::shared_ptr<RHIPrivate::FReadbackState> Request;
			bool bReplayed = false;
			~FTextureReadbackCommand() { if (!bReplayed) Request->Abandon(); }
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetOperationContext("TextureReadback")
					.RHIEnqueueTextureReadback(Texture.GetReference(), MipIndex, ArraySlice, Request);
				bReplayed = true;
			}
		};
		struct FBufferReadbackCommand
		{
			TRefCountPtr<FRHIBuffer> Source;
			uint32 Offset, NumBytes;
			std::shared_ptr<RHIPrivate::FReadbackState> Request;
			bool bReplayed = false;
			~FBufferReadbackCommand() { if (!bReplayed) Request->Abandon(); }
			auto Execute(void* ReplayContext) -> void
			{ GetReplayContext(ReplayContext).GetOperationContext("BufferReadback").RHIEnqueueBufferReadback(Source.GetReference(), Offset, NumBytes, Request); bReplayed = true; }
		};
		struct FPollReadbacksCommand
		{
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetOperationContext("PollReadbacks")
					.RHIPollReadbacks();
			}
		};

		struct FInitializeTextureCommand
		{
			explicit FInitializeTextureCommand(FRHITexture* InTexture)
				: Texture(InTexture)
			{
				check(Texture);
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetOperationContext("InitializeTexture")
					.RHIInitializeTexture(Texture.GetReference());
			}

			TRefCountPtr<FRHITexture> Texture;
		};


		struct FUpdateTextureCommand
		{
			TRefCountPtr<FRHITexture> Texture;
			std::shared_ptr<const FRHITextureUploadData> Data;
			auto Execute(void* ReplayContext) -> void
			{
				auto& Context = GetReplayContext(ReplayContext).GetOperationContext(
					Data->IsVolume() ? "UpdateTexture3D" : "UpdateTexture2D");
				if (Data->IsVolume())
					Context.RHIUpdateTexture3D(Texture, Data->GetMip(), Data->GetRegion3D(),
						Data->GetLayout().RowPitch, Data->GetLayout().DepthPitch, Data->GetData());
				else
					Context.RHIUpdateTexture2D(Texture, Data->GetMip(), Data->GetLayer(),
						Data->GetRegion2D(), Data->GetLayout().RowPitch, Data->GetData());
			}
			auto GetOwnedPayloadBytes() const -> size_t { return Data->GetData().size(); }
		};

		struct FSetPreparedShaderParametersCommand
		{
			explicit FSetPreparedShaderParametersCommand(std::shared_ptr<const FRHIShaderParameterBatch> InBatch)
				: Batch(std::move(InBatch)) {}
			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext).GetPipelineContext("SetPreparedShaderParameters")
					.RHISetShaderParameters(Batch->GetShader(), Batch->GetParameters());
			}
			auto GetOwnedPayloadBytes() const -> size_t
			{
				// Conservative per-reference charge preserves queue admission bounds.
				return Batch->GetRetainedPayloadBytes();
			}
			std::shared_ptr<const FRHIShaderParameterBatch> Batch;
		};

		struct FSetShaderParametersCommand
		{
			FSetShaderParametersCommand(
				FRHIShader* InShader,
				std::span<const FRHIShaderParameterResource> InParameters)
				: Shader(InShader)
				, Parameters(InParameters.begin(), InParameters.end())
			{
				Resources.reserve(Parameters.size());
				for (FRHIShaderParameterResource& Parameter : Parameters)
				{
					Resources.emplace_back(Parameter.Resource);
					Parameter.Resource = Resources.back().GetReference();
				}
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetPipelineContext("SetShaderParameters")
					.RHISetShaderParameters(Shader.GetReference(), Parameters);
			}

			auto GetOwnedPayloadBytes() const -> size_t
			{
				const size_t ParameterBytes = Parameters.capacity()
					* sizeof(Parameters.front());
				const size_t ResourceBytes = Resources.capacity()
					* sizeof(Resources.front());
				return CheckedAddPayloadBytes(ParameterBytes, ResourceBytes);
			}

			TRefCountPtr<FRHIShader> Shader;
			std::vector<FRHIShaderParameterResource> Parameters;
			std::vector<TRefCountPtr<FRHIResource>> Resources;
		};

		auto DeleteDeferredResources() -> void
		{
			std::vector<FRHIResource*> ResourcesToDelete;
			while (true)
			{
				FRHIResource::GatherResourcesToDelete(ResourcesToDelete);
				if (ResourcesToDelete.empty())
				{
					break;
				}
				FRHIResource::DeleteResources(ResourcesToDelete);
				ResourcesToDelete.clear();
			}
		}
	}

	auto RHIFlushDeferredResources() -> void
	{
		DeleteDeferredResources();
	}

	class FRHICommandListExecutor::FState
	{
	public:
		class FBatch
		{
		public:
			explicit FBatch(std::unique_ptr<FRHICommandStorage>&& InStorage)
				: Storage(std::move(InStorage))
			{
				check(Storage);
			}

			FBatch(FBatch&&) noexcept = default;
			auto operator=(FBatch&&) noexcept -> FBatch& = default;
			FBatch(const FBatch&) = delete;
			auto operator=(const FBatch&) -> FBatch& = delete;

			auto Replay(FRHICommandReplayContext& ReplayContext) -> void
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("RHI.Batch.Replay");
				DURIN_PROFILE_CPU_ZONE_TEXT(std::format("commands={} payloadBytes={}", GetCommandCount(), GetPayloadBytes()));
				check(State == EState::Submitted);
				Storage->Replay(ReplayContext);
				State = EState::Consumed;
			}

			auto GetCommandCount() const -> size_t
			{
				return Storage->GetCommandCount();
			}

			auto GetPayloadBytes() const -> size_t
			{
				return Storage->GetPayloadBytes();
			}

		private:
			enum class EState : uint8
			{
				Submitted,
				Consumed
			};

			std::unique_ptr<FRHICommandStorage> Storage;
			EState State = EState::Submitted;
		};

		class FSubmissionGroup
		{
		public:
			FSubmissionGroup(
				ERHISubmitFlags InFlags,
				std::vector<FBatch>&& InBatches)
				: Flags(InFlags)
				, Batches(std::make_shared<std::vector<FBatch>>(std::move(InBatches)))
			{
				BatchCount = Batches->size();
				for (const FBatch& Batch : *Batches)
				{
					CommandCount = CheckedAddPayloadBytes(
						CommandCount, Batch.GetCommandCount());
					PayloadBytes = CheckedAddPayloadBytes(
						PayloadBytes, Batch.GetPayloadBytes());
				}
			}
			FSubmissionGroup(FSubmissionGroup&&) noexcept = default;
			FSubmissionGroup(const FSubmissionGroup&) = delete;
			auto operator=(const FSubmissionGroup&)
				-> FSubmissionGroup& = delete;

			auto Replay(FRHICommandReplayContext& ReplayContext) -> void
			{
				for (FBatch& Batch : *Batches) Batch.Replay(ReplayContext);
			}

			auto ReleaseBatches() -> void
			{
				Batches.reset();
			}

			auto GetStorageOwner() const -> std::shared_ptr<void> { return Batches; }
			auto GetFlags() const -> ERHISubmitFlags { return Flags; }
			auto GetBatchCount() const -> size_t { return BatchCount; }
			auto GetCommandCount() const -> size_t { return CommandCount; }
			auto GetPayloadBytes() const -> size_t { return PayloadBytes; }

		private:
			const ERHISubmitFlags Flags;
			std::shared_ptr<std::vector<FBatch>> Batches;
			size_t BatchCount = 0;
			size_t CommandCount = 0;
			size_t PayloadBytes = 0;
		};

		explicit FState(
			IRHICommandContext* GraphicsContext = nullptr,
			FRHIThread* InRHIThread = nullptr)
			: ReplayContext(GraphicsContext)
			, RHIThread(InRHIThread)
		{
		}

		std::vector<FBatch> PendingBatches;
		FRHICommandReplayContext ReplayContext;
		FRHIThread* RHIThread = nullptr;
		std::atomic<uint64> LastSubmittedSerial = 0;
		std::atomic<uint64> CompletedSerial = 0;
		std::atomic<uint64> FrameNumber = 0;
		std::atomic<uint64> RecordedCommandCount = 0;
		std::atomic<uint64> RecordedPayloadBytes = 0;
		std::atomic<uint64> SubmittedBatchCount = 0;
		std::atomic<uint64> SubmissionGroupCount = 0;
		std::atomic<uint64> ReplayDurationNanoseconds = 0;
		mutable std::atomic<uint64> WaitCount = 0;
		std::atomic<uint64> SynchronousOperationCount = 0;
		mutable std::atomic<uint64> WaitDurationNanoseconds = 0;
		mutable std::mutex CompletionMutex;
		mutable std::condition_variable CompletionCV;
	};

	class FRHICommandListImmediate::FLockState
	{
	public:
		struct FPendingLock
		{
			TRefCountPtr<FRHIBuffer> Buffer;
			uint32 Offset = 0;
			uint32 Size = 0;
			std::shared_ptr<FRHIBufferMappedWrite> MappedWrite;
			bool bDiscard = false;
			FByteBuffer Data;
		};

		std::unordered_map<FRHIBuffer*, std::unique_ptr<FPendingLock>> PendingLocks;
	};

	FRHICommandListExecutor GCommandListExecutor;

	FRHICommandListBase::FRHICommandListBase()
		: Storage(std::make_unique<FRHICommandStorage>())
	{
	}

	FRHICommandListBase::~FRHICommandListBase() = default;

	FRHICommandListBase::FRHICommandListBase(FRHICommandListBase&& Other) noexcept
		: Storage(std::move(Other.Storage))
		, RecordingState(Other.RecordingState)
		, ActivePipeline(Other.ActivePipeline)
		, bHasActiveGraphicsPipelineState(Other.bHasActiveGraphicsPipelineState)
		, ActiveComputePipelineState(Other.ActiveComputePipelineState)
		, ActiveGraphicsPipeline(std::move(Other.ActiveGraphicsPipeline))
		, ActiveComputePipeline(std::move(Other.ActiveComputePipeline))
		, bInsideRenderPass(Other.bInsideRenderPass)
		, ActiveRenderTargetLayout(Other.ActiveRenderTargetLayout)
		, RenderPassParent(Other.RenderPassParent)
		, RenderPassSerial(Other.RenderPassSerial)
		, ActiveGPUSubmissionLease(std::move(Other.ActiveGPUSubmissionLease))
		, DiagnosticRegionDepth(Other.DiagnosticRegionDepth)
		, ActiveGPUTimingQueries(std::move(Other.ActiveGPUTimingQueries))
		, ActiveGPUTimingReservations(
			std::move(Other.ActiveGPUTimingReservations))
		, ActiveTransitions(std::move(Other.ActiveTransitions))
		, NumRecordedDrawCommands(Other.NumRecordedDrawCommands)
	{
		Other.RecordingState = ERecordingState::MovedFrom;
		Other.ActivePipeline = ERHIPipeline::None;
		Other.bHasActiveGraphicsPipelineState = false;
		Other.ActiveComputePipelineState = nullptr;
		Other.bInsideRenderPass = false;
		Other.RenderPassParent = nullptr;
		Other.DiagnosticRegionDepth = 0;
		Other.ActiveGPUTimingQueries.clear();
		Other.ActiveGPUTimingReservations.clear();
		Other.ActiveTransitions.clear();
		Other.NumRecordedDrawCommands = 0;
	}

	auto FRHICommandListBase::operator=(FRHICommandListBase&& Other) noexcept
		-> FRHICommandListBase&
	{
		if (this != &Other)
		{
			check(RecordingState == ERecordingState::MovedFrom
				|| (RecordingState == ERecordingState::Recording
					&& Storage && Storage->GetCommandCount() == 0));
			Storage = std::move(Other.Storage);
			RecordingState = Other.RecordingState;
			ActivePipeline = Other.ActivePipeline;
			bHasActiveGraphicsPipelineState = Other.bHasActiveGraphicsPipelineState;
			ActiveComputePipelineState = Other.ActiveComputePipelineState;
			ActiveGraphicsPipeline = std::move(Other.ActiveGraphicsPipeline);
			ActiveComputePipeline = std::move(Other.ActiveComputePipeline);
			bInsideRenderPass = Other.bInsideRenderPass;
			ActiveRenderTargetLayout = Other.ActiveRenderTargetLayout;
			RenderPassParent = Other.RenderPassParent;
			RenderPassSerial = Other.RenderPassSerial;
			ActiveGPUSubmissionLease = std::move(Other.ActiveGPUSubmissionLease);
			DiagnosticRegionDepth = Other.DiagnosticRegionDepth;
			ActiveGPUTimingQueries = std::move(Other.ActiveGPUTimingQueries);
			ActiveGPUTimingReservations =
				std::move(Other.ActiveGPUTimingReservations);
			ActiveTransitions = std::move(Other.ActiveTransitions);
			NumRecordedDrawCommands = Other.NumRecordedDrawCommands;
			Other.RecordingState = ERecordingState::MovedFrom;
			Other.ActivePipeline = ERHIPipeline::None;
			Other.bHasActiveGraphicsPipelineState = false;
			Other.ActiveComputePipelineState = nullptr;
			Other.bInsideRenderPass = false;
			Other.RenderPassParent = nullptr;
			Other.DiagnosticRegionDepth = 0;
			Other.ActiveGPUTimingQueries.clear();
			Other.ActiveGPUTimingReservations.clear();
			Other.ActiveTransitions.clear();
			Other.NumRecordedDrawCommands = 0;
		}
		return *this;
	}

	auto FRHICommandListBase::AllocateCommand(
		size_t PayloadSize,
		size_t PayloadAlignment,
		FCommandFunction Execute,
		FCommandDestroyFunction Destroy) -> FCommandAllocation
	{
		checkf(RecordingState == ERecordingState::Recording,
			"RHI commands can only be recorded into a recording command list.");
		check(Storage);
		const FRHICommandStorage::FAllocation Allocation = Storage->Allocate(
			PayloadSize, PayloadAlignment, Execute, Destroy);
		return {Allocation.Node, Allocation.Payload};
	}

	auto FRHICommandListBase::CommitCommand(
		void* Node,
		size_t OwnedPayloadBytes) -> void
	{
		check(Storage);
		Storage->Commit(Node, OwnedPayloadBytes);
	}


	auto FRHICommandListBase::DetachStorage()
		-> std::unique_ptr<FRHICommandStorage>
	{
		check(Storage);
		std::unique_ptr<FRHICommandStorage> Detached = std::move(Storage);
		Storage = std::make_unique<FRHICommandStorage>();
		return Detached;
	}

	auto FRHICommandListBase::IsFinished() const -> bool
	{
		return RecordingState == ERecordingState::Finished;
	}

	auto FRHICommandListBase::MarkAdmitted() -> void
	{
		check(RecordingState == ERecordingState::Finished);
		RecordingState = ERecordingState::Admitted;
	}

	auto FRHICommandListBase::IsRecording() const -> bool
	{
		return RecordingState == ERecordingState::Recording;
	}

	auto FRHICommandListBase::GetNumRecordedCommands() const -> size_t
	{
		return Storage ? Storage->GetCommandCount() : 0;
	}

	auto FRHICommandListBase::GetNumRecordedDrawCommands() const -> uint64
	{
		return NumRecordedDrawCommands;
	}

	auto FRHICommandListBase::RecordInvalidDiagnosticRegion() -> void
	{
		uint64 Current = GInvalidDiagnosticRegionCount.load(
			std::memory_order_relaxed);
		while (Current != std::numeric_limits<uint64>::max()
			&& !GInvalidDiagnosticRegionCount.compare_exchange_weak(
				Current, Current + 1, std::memory_order_relaxed,
				std::memory_order_relaxed)) {}
	}

	auto FRHICommandListBase::GetInvalidDiagnosticRegionCount() -> uint64
	{
		return GInvalidDiagnosticRegionCount.load(std::memory_order_relaxed);
	}

	auto FRHICommandListBase::ResetInvalidDiagnosticRegionCount() -> void
	{
		GInvalidDiagnosticRegionCount.store(0, std::memory_order_relaxed);
	}

	FRHICommandList::FRHICommandList() = default;
	FRHICommandList::~FRHICommandList() = default;
	FRHICommandList::FRHICommandList(FRHICommandList&& Other) noexcept = default;
	auto FRHICommandList::operator=(FRHICommandList&& Other) noexcept
		-> FRHICommandList& = default;

	auto FRHICommandList::FinishRecording() -> void
	{
		checkf(RecordingState == ERecordingState::Recording,
			"FinishRecording requires a recording regular command list.");
		checkf(!bInsideRenderPass || RenderPassParent,
			"FinishRecording cannot seal a command list inside a render pass.");
		requiref(!ActiveGPUSubmissionLease, "FinishRecording requires a closed GPU submission.");
		if (DiagnosticRegionDepth != 0) RecordInvalidDiagnosticRegion();
		while (DiagnosticRegionDepth != 0) EndDiagnosticRegion();
		checkf(ActiveGPUTimingQueries.empty(),
			"FinishRecording cannot seal a command list with an open GPU timing query.");
		checkf(ActiveTransitions.empty(),
			"FinishRecording cannot seal a command list with an open split transition.");
		RecordingState = ERecordingState::Finished;
	}

	auto FRHICommandList::IsFinished() const -> bool
	{
		return FRHICommandListBase::IsFinished();
	}

	FRHISubCommandList::FRHISubCommandList(const FRHICommandList& Parent)
	{
		requiref(Parent.IsRecording() && Parent.bInsideRenderPass && !Parent.RenderPassParent,
			"Subcommand lists require a recording parent inside a render pass.");
		ActivePipeline = ERHIPipeline::Graphics;
		bInsideRenderPass = true;
		ActiveRenderTargetLayout = Parent.ActiveRenderTargetLayout;
		RenderPassParent = &Parent;
		RenderPassSerial = Parent.RenderPassSerial;
	}

	auto FRHICommandList::InsertParallelRenderPass(std::span<FRHISubCommandList* const> CommandLists) -> void
	{
		requiref(IsRecording() && bInsideRenderPass && !RenderPassParent,
			"Parallel render-pass insertion requires an active parent render pass.");
		// Validate the entire group before admitting any child.
		for (size_t Index = 0; Index < CommandLists.size(); ++Index)
		{
			const auto* Child = CommandLists[Index];
			requiref(Child && Child->IsFinished() && Child->RenderPassParent == this
				&& Child->RenderPassSerial == RenderPassSerial,
				"Subcommand list is unfinished, already admitted, or belongs to another render pass.");
			requiref(std::find(CommandLists.begin(), CommandLists.begin() + Index, Child) == CommandLists.begin() + Index,
				"A subcommand list cannot be inserted twice.");
		}
		for (auto* Child : CommandLists)
		{
			Storage->Append(*Child->Storage);
			NumRecordedDrawCommands = CheckedAddPayloadBytes(NumRecordedDrawCommands, Child->NumRecordedDrawCommands);
			Child->MarkAdmitted();
		}
		// Children establish their own state; subsequent parent draws must bind
		// their PSO again rather than inheriting the parent's pre-insertion state.
		bHasActiveGraphicsPipelineState = false;
		ActiveGraphicsPipeline = {};
	}

	auto FRHICommandListBase::BeginGPUSubmission(const FRHIGPUSubmissionDesc& Desc)
		-> FRHIGPUSyncPointRef
	{
		requiref(!bInsideRenderPass && !ActiveGPUSubmissionLease, "GPU submissions cannot nest or begin inside a render pass.");
		FRHIGPUSubmissionDesc Recorded{.Queue = Desc.Queue};
		Recorded.Waits.reserve(Desc.Waits.size());
		for (const auto& Wait : Desc.Waits)
		{
			requiref(Wait.GetState() != ERHIGPUSubmissionState::Invalid, "GPU submission has an invalid dependency.");
			if (!std::ranges::contains(Recorded.Waits, Wait))
				Recorded.Waits.push_back(Wait);
		}
		auto Lease = std::make_shared<FGPUSubmissionRecordingLease>();
		Lease->Queue = Desc.Queue;
		RecordCommand<FBeginGPUSubmissionCommand>(std::move(Recorded), Lease);
		ActiveGPUSubmissionLease = Lease;
		return Lease->Signal;
	}

	auto FRHICommandListBase::EndGPUSubmission() -> void
	{
		requiref(!bInsideRenderPass && ActiveGPUSubmissionLease, "GPU submission end requires a matching begin outside a render pass.");
		RecordCommand<FEndGPUSubmissionCommand>(
			std::static_pointer_cast<FGPUSubmissionRecordingLease>(ActiveGPUSubmissionLease));
		ActiveGPUSubmissionLease.reset();
	}

	auto FRHICommandListBase::ReleaseQueueOwnership(std::shared_ptr<FRHIQueueTransfer> Transfer) -> void
	{
		requiref(Transfer && !bInsideRenderPass, "Queue release requires a transfer outside a render pass.");
		if (ActiveGPUSubmissionLease)
			requiref(std::static_pointer_cast<FGPUSubmissionRecordingLease>(ActiveGPUSubmissionLease)->Queue == Transfer->GetSourceQueue(),
				"Queue release must belong to the active GPU submission queue.");
		RecordCommand<FQueueOwnershipCommand>(std::move(Transfer), true);
	}

	auto FRHICommandListBase::AcquireQueueOwnership(std::shared_ptr<FRHIQueueTransfer> Transfer) -> void
	{
		requiref(Transfer && !bInsideRenderPass, "Queue acquire requires a transfer outside a render pass.");
		if (ActiveGPUSubmissionLease)
			requiref(std::static_pointer_cast<FGPUSubmissionRecordingLease>(ActiveGPUSubmissionLease)->Queue == Transfer->GetDestinationQueue(),
				"Queue acquire must belong to the active GPU submission queue.");
		RecordCommand<FQueueOwnershipCommand>(std::move(Transfer), false);
	}

	auto FRHICommandListBase::SwitchPipeline(ERHIPipeline Pipeline) -> void
	{
		checkf(!bInsideRenderPass,
			"SwitchPipeline cannot be recorded inside a render pass.");
		if (ActivePipeline == Pipeline)
		{
			return;
		}
		RecordCommand<FSwitchPipelineCommand>(Pipeline);
		ActivePipeline = Pipeline;
		bHasActiveGraphicsPipelineState = false;
		ActiveComputePipelineState = nullptr;
		ActiveGraphicsPipeline = {};
		ActiveComputePipeline = {};
	}

	auto FRHICommandListBase::BeginDiagnosticRegion(std::string_view Name) -> void
	{
		RecordCommand<FBeginDiagnosticRegionCommand>(Name.empty() ? std::string_view("Unnamed") : Name);
		++DiagnosticRegionDepth;
	}

	auto FRHICommandListBase::EndDiagnosticRegion() -> void
	{
		if (DiagnosticRegionDepth == 0)
		{
			RecordInvalidDiagnosticRegion();
			return;
		}
		RecordCommand<FEndDiagnosticRegionCommand>();
		--DiagnosticRegionDepth;
	}

	auto FRHICommandListBase::BeginGPUTimingQuery(
		FRHIGPUTimingQuery* Query) -> void
	{
		requiref(Query, "BeginGPUTimingQuery requires a query.");
		requiref(Query->TryReserveRecording(),
			"GPU timing query is already recording or pending.");
		auto Reservation = std::make_shared<FGPUTimingRecordingReservation>(Query);
		RecordCommand<FBeginGPUTimingQueryCommand>(Reservation);
		ActiveGPUTimingQueries.push_back(Query);
		ActiveGPUTimingReservations.push_back(std::move(Reservation));
	}

	auto FRHICommandListBase::EndGPUTimingQuery(
		FRHIGPUTimingQuery* Query) -> void
	{
		if (!Query || ActiveGPUTimingQueries.empty()
			|| ActiveGPUTimingQueries.back() != Query)
			FRHIGPUTimingQuery::RecordInvalidRecording();
		checkf(Query && !ActiveGPUTimingQueries.empty()
			&& ActiveGPUTimingQueries.back() == Query,
			"EndGPUTimingQuery requires the innermost active query on this command list.");
		auto Reservation = std::static_pointer_cast<FGPUTimingRecordingReservation>(
			ActiveGPUTimingReservations.back());
		RecordCommand<FEndGPUTimingQueryCommand>(std::move(Reservation));
		ActiveGPUTimingQueries.pop_back();
		ActiveGPUTimingReservations.pop_back();
	}

	auto FRHICommandListBase::BeginRenderPass(const FRHIRenderPassInfo& Info, FName Name) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"BeginRenderPass requires an active graphics pipeline while recording.");
		checkf(!bInsideRenderPass,
			"BeginRenderPass cannot be nested while recording.");
		RecordCommand<FBeginRenderPassCommand>(Info, Name);
		ActiveRenderTargetLayout = Info.RenderTargetLayout;
		bInsideRenderPass = true;
		requiref(RenderPassSerial != std::numeric_limits<uint64>::max(), "Render-pass serial exhausted.");
		++RenderPassSerial;
	}

	auto FRHICommandListBase::EndRenderPass() -> void
	{
		requiref(!RenderPassParent, "A subcommand list cannot end its parent's render pass.");
		checkf(bInsideRenderPass,
			"EndRenderPass requires a matching BeginRenderPass while recording.");
		RecordCommand<FEndRenderPassCommand>();
		bInsideRenderPass = false;
	}

	auto FRHICommandListBase::BeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetTexture) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"BeginDrawingViewport requires an active graphics pipeline while recording.");
		RecordCommand<FBeginDrawingViewportCommand>(Viewport, RenderTargetTexture);
	}

	auto FRHICommandListBase::EndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"EndDrawingViewport requires an active graphics pipeline while recording.");
		RecordCommand<FEndDrawingViewportCommand>(Viewport, bPresent, bLockToVsync);
	}

	auto FRHICommandListBase::SetGraphicsPipelineState(FRHIGraphicsPipelineState& State) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"SetGraphicsPipelineState requires an active graphics pipeline while recording.");
		RecordCommand<FSetGraphicsPipelineStateCommand>(State);
		bHasActiveGraphicsPipelineState = true;
		ActiveGraphicsPipeline = {};
	}

	auto FRHICommandListBase::SetComputePipelineState(
		FRHIComputePipelineState& State) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Compute,
			"SetComputePipelineState requires an active compute pipeline while recording.");
		checkf(!bInsideRenderPass,
			"SetComputePipelineState cannot be recorded inside a render pass.");
		RecordCommand<FSetComputePipelineStateCommand>(State);
		ActiveComputePipelineState = &State;
		ActiveComputePipeline = {};
	}
	auto FRHICommandListBase::RecordPipelineWait(const std::shared_ptr<FPipelineState>& Pipeline) -> void
	{
		require(IsRecording() && Pipeline);
		const auto& Request = *Pipeline->Compilation;
		if (Request.GetState() != ERHIPipelineState::Ready)
			RecordCommand<FWaitForPipelineCommand>(Request);
	}
	auto FRHICommandListBase::SetGraphicsPipelineState(const FGraphicsPipelineStateRef& Pipeline) -> void
	{
		check(Pipeline);
		const auto& Request = *Pipeline->Compilation;
		check(ActivePipeline == ERHIPipeline::Graphics && Request.IsValid() && !Request.IsCompute());
		RecordCommand<FSetCachedPipelineStateCommand>(Request, Pipeline);
		bHasActiveGraphicsPipelineState = true;
		ActiveGraphicsPipeline = Pipeline;
	}
	auto FRHICommandListBase::SetComputePipelineState(const FComputePipelineStateRef& Pipeline) -> void
	{
		check(Pipeline);
		const auto& Request = *Pipeline->Compilation;
		check(ActivePipeline == ERHIPipeline::Compute && !bInsideRenderPass && Request.IsCompute());
		check(Request.GetPipelineLayout());
		RecordCommand<FSetCachedPipelineStateCommand>(Request, Pipeline);
		ActiveComputePipelineState = nullptr;
		ActiveComputePipeline = Pipeline;
	}

	auto FRHICommandListBase::BindVertexBuffer(uint32 StreamIndex, FRHIBuffer* VertexBuffer, uint32 Offset) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"BindVertexBuffer requires an active graphics pipeline while recording.");
		RecordCommand<FBindVertexBufferCommand>(StreamIndex, VertexBuffer, Offset);
	}

	auto FRHICommandListBase::BindIndexBuffer(FRHIBuffer* Buffer, uint32 Offset) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"BindIndexBuffer requires an active graphics pipeline while recording.");
		RecordCommand<FBindIndexBufferCommand>(Buffer, Offset);
	}

	auto FRHICommandListBase::TransitionBuffers(
		std::span<const FRHIBufferTransition> Transitions) -> void
	{
		for (const auto& Transition : Transitions)
		{
			for (const auto& Active : ActiveTransitions)
				for (const auto& Pending : Active->GetBufferTransitions())
					requiref(!BufferTransitionsOverlap(Transition, Pending),
						"A resource range cannot be transitioned while its split transition is open.");
		}
		if (Transitions.empty()) return;
		checkf(!bInsideRenderPass,
			"Buffer transitions cannot be recorded inside a render pass.");
#if DO_CHECK
		const auto ValidationResult = ValidateBufferTransitions(Transitions);
		checkf(ValidationResult,
			"Invalid RHI buffer transition batch: {}", ToString(ValidationResult.error()));
#endif
		RecordCommand<FBufferTransitionCommand>(Transitions);
	}

	auto FRHICommandListBase::TransitionTextures(
		std::span<const FRHITextureTransition> Transitions) -> void
	{
		if (Transitions.empty()) return;
		for (const auto& Transition : Transitions)
			for (const auto& Active : ActiveTransitions)
				for (const auto& Pending : Active->GetTextureTransitions())
					requiref(!TextureTransitionsOverlap(Transition, Pending),
						"A subresource range cannot be transitioned while its split transition is open.");
		checkf(!bInsideRenderPass,
			"Texture transitions cannot be recorded inside a render pass.");
#if DO_CHECK
		const auto ValidationResult = ValidateTextureTransitions(Transitions);
		checkf(ValidationResult,
			"Invalid RHI texture transition batch: {}", ToString(ValidationResult.error()));
#endif
		RecordCommand<FTextureTransitionCommand>(Transitions);
	}

	auto FRHICommandListBase::AliasingBarrier(const FRHITransientAliasingBarrier& Barrier) -> void
	{
		const auto [Before, After] = Barrier;
		requiref(After && Before != After && !bInsideRenderPass,
			"Aliasing requires distinct resources outside render passes.");
		RecordCommand<FAliasingBarrierCommand>(Before, After);
	}

	auto FRHICommandListBase::BeginTransition(
		std::shared_ptr<FRHITransition> Transition) -> void
	{
		requiref(Transition && !bInsideRenderPass,
			"Split transition begin requires a valid object outside a render pass.");
		requiref(!std::ranges::contains(ActiveTransitions, Transition),
			"Split transition begin cannot record the same object twice.");
		for (const auto& Active : ActiveTransitions)
		{
			for (const auto& A : Active->GetBufferTransitions())
				for (const auto& B : Transition->GetBufferTransitions())
					requiref(!BufferTransitionsOverlap(A, B),
						"Open split transitions cannot overlap one buffer range.");
			for (const auto& A : Active->GetTextureTransitions())
				for (const auto& B : Transition->GetTextureTransitions())
					requiref(!TextureTransitionsOverlap(A, B),
						"Open split transitions cannot overlap one texture range.");
		}
		ActiveTransitions.push_back(Transition);
		RecordCommand<FBeginTransitionCommand>(std::move(Transition));
	}

	auto FRHICommandListBase::EndTransition(
		std::shared_ptr<FRHITransition> Transition) -> void
	{
		requiref(Transition && !bInsideRenderPass,
			"Split transition end requires a valid object outside a render pass.");
		const auto It = std::ranges::find(ActiveTransitions, Transition);
		requiref(It != ActiveTransitions.end(),
			"Split transition end requires a matching begin on this command list.");
		ActiveTransitions.erase(It);
		RecordCommand<FEndTransitionCommand>(std::move(Transition));
	}

	auto FRHICommandListBase::CopyBuffer(FRHIBuffer* Source, FRHIBuffer* Destination,
		std::span<const FRHIBufferCopyRegion> Regions) -> void
	{

		if (Regions.empty()) return;
		checkf(!bInsideRenderPass, "Buffer copies cannot be recorded inside a render pass.");
#if DO_CHECK
		const auto ValidationResult = ValidateBufferCopies(Source, Destination, Regions);
		checkf(ValidationResult,
			"Invalid RHI buffer copy batch: {}", ToString(ValidationResult.error()));
#endif
		RecordCommand<FCopyBufferCommand>(Source, Destination, Regions);
	}

	auto FRHICommandListBase::CopyBufferToTexture(FRHIBuffer* Source, FRHITexture* Destination,
		std::span<const FRHIBufferTextureCopyRegion> Regions) -> void
	{
		if (Regions.empty()) return;
		checkf(!bInsideRenderPass, "Buffer-to-texture copies cannot be recorded inside a render pass.");
#if DO_CHECK
		const auto ValidationResult = ValidateBufferToTextureCopies(Source, Destination, Regions);
		checkf(ValidationResult,
			"Invalid RHI buffer-to-texture copy batch: {}", ToString(ValidationResult.error()));
#endif
		RecordCommand<FCopyBufferToTextureCommand>(Source, Destination, Regions);
	}

	auto FRHICommandListBase::CopyTextureToBuffer(FRHITexture* Source, FRHIBuffer* Destination,
		std::span<const FRHIBufferTextureCopyRegion> Regions) -> void
	{
		if (Regions.empty()) return;
		checkf(!bInsideRenderPass, "Texture-to-buffer copies cannot be recorded inside a render pass.");
#if DO_CHECK
		const auto ValidationResult = ValidateTextureToBufferCopies(Source, Destination, Regions);
		checkf(ValidationResult,
			"Invalid RHI texture-to-buffer copy batch: {}", ToString(ValidationResult.error()));
#endif
		RecordCommand<FCopyTextureToBufferCommand>(Source, Destination, Regions);
	}

	auto FRHICommandListBase::CopyTexture(FRHITexture* Source, FRHITexture* Destination,
		std::span<const FRHITextureCopyRegion> Regions) -> void
	{
		if (Regions.empty()) return;
		checkf(!bInsideRenderPass, "Texture copies cannot be recorded inside a render pass.");
#if DO_CHECK
		const auto ValidationResult = ValidateTextureCopies(Source, Destination, Regions);
		checkf(ValidationResult,
			"Invalid RHI texture copy batch: {}", ToString(ValidationResult.error()));
#endif
		RecordCommand<FCopyTextureCommand>(Source, Destination, Regions);
	}

	auto FRHICommandListBase::Draw(const FRHIDrawArguments& Arguments) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"Draw requires an active graphics pipeline while recording.");
		if (Arguments.VertexCount == 0 || Arguments.InstanceCount == 0) return;
		RecordCommand<FDrawCommand>(Arguments);
		CountRecordedDrawCommand();
	}

	auto FRHICommandListBase::DrawIndexed(
		const FRHIDrawIndexedArguments& Arguments) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"DrawIndexed requires an active graphics pipeline while recording.");
		if (Arguments.IndexCount == 0 || Arguments.InstanceCount == 0) return;
		RecordCommand<FDrawIndexedCommand>(Arguments);
		CountRecordedDrawCommand();
	}

	auto FRHICommandListBase::ValidateIndirectDrawRecording(const char* OperationName,
		FRHIBuffer* ArgumentBuffer, uint64 Offset, size_t ArgumentSize) -> void
	{
		require(IsRecording());
		requiref(ActivePipeline == ERHIPipeline::Graphics, "{} requires the graphics pipeline domain.", OperationName);
		requiref(bInsideRenderPass, "{} has invalid render-pass placement.", OperationName);
		requiref(bHasActiveGraphicsPipelineState, "{} requires an active pipeline state.", OperationName);
		const FRHICapabilities* Capabilities = GDynamicRHI
			? GDynamicRHI->RHIGetCapabilities() : nullptr;
		const auto Validation = ValidateIndirectArgumentBuffer(ArgumentBuffer, Offset,
			ArgumentSize, Capabilities && Capabilities->bSupportsIndirectDraw);
		requiref(Validation.has_value(), "Invalid indirect arguments: {}", ToString(Validation.error()));
	}

	auto FRHICommandListBase::CountRecordedDrawCommand() -> void
	{
		if (NumRecordedDrawCommands != std::numeric_limits<uint64>::max())
			++NumRecordedDrawCommands;
	}

	auto FRHICommandListBase::DrawIndirect(FRHIBuffer* ArgumentBuffer,
		uint64 Offset) -> void
	{
		ValidateIndirectDrawRecording("DrawIndirect", ArgumentBuffer, Offset, sizeof(FRHIDrawIndirectArguments));
		RecordCommand<FDrawIndirectCommand>(ArgumentBuffer, Offset);
		CountRecordedDrawCommand();
	}

	auto FRHICommandListBase::DrawIndexedIndirect(FRHIBuffer* ArgumentBuffer,
		uint64 Offset) -> void
	{
		ValidateIndirectDrawRecording("DrawIndexedIndirect", ArgumentBuffer, Offset, sizeof(FRHIDrawIndexedIndirectArguments));
		RecordCommand<FDrawIndexedIndirectCommand>(ArgumentBuffer, Offset);
		CountRecordedDrawCommand();
	}

	auto FRHICommandListBase::DrawIndexed(uint32 IndexCount,
		uint32 StartIndexLocation, int32 VertexOffset) -> void
	{
		DrawIndexed({.IndexCount = IndexCount, .FirstIndex = StartIndexLocation,
			.VertexOffset = VertexOffset});
	}

	auto FRHICommandListBase::Dispatch(uint32 GroupCountX,
		uint32 GroupCountY, uint32 GroupCountZ) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Compute,
			"Dispatch requires an active compute pipeline while recording.");
		checkf(!bInsideRenderPass,
			"Dispatch cannot be recorded inside a render pass.");
		checkf(ActiveComputePipelineState || bool(ActiveComputePipeline),
			"Dispatch requires an active compute pipeline state while recording.");
		const FRHICapabilities* Capabilities = GDynamicRHI
			? GDynamicRHI->RHIGetCapabilities() : nullptr;
		const std::array Counts{GroupCountX, GroupCountY, GroupCountZ};
		for (size_t Axis = 0; Axis < Counts.size(); ++Axis)
		{
			checkf(Counts[Axis] != 0,
				"Dispatch group counts must be nonzero.");
			checkf(!Capabilities
				|| Counts[Axis] <= Capabilities->MaxComputeWorkGroupCount[Axis],
				"Dispatch group count exceeds the published device limit.");
		}
		RecordCommand<FDispatchCommand>(GroupCountX, GroupCountY, GroupCountZ);
	}

	auto FRHICommandListBase::DispatchIndirect(FRHIBuffer* ArgumentBuffer,
		uint64 Offset) -> void
	{
		require(IsRecording());
		requiref(ActivePipeline == ERHIPipeline::Compute, "DispatchIndirect requires the compute pipeline domain.");
		requiref(!bInsideRenderPass, "DispatchIndirect has invalid render-pass placement.");
		requiref(ActiveComputePipelineState || bool(ActiveComputePipeline), "DispatchIndirect requires an active pipeline state.");
		const FRHICapabilities* Capabilities = GDynamicRHI
			? GDynamicRHI->RHIGetCapabilities() : nullptr;
		const auto Validation = ValidateIndirectArgumentBuffer(ArgumentBuffer, Offset,
			sizeof(FRHIDispatchIndirectArguments), Capabilities
				&& Capabilities->bSupportsIndirectDispatch);
		requiref(Validation.has_value(), "Invalid indirect arguments: {}", ToString(Validation.error()));
		RecordCommand<FDispatchIndirectCommand>(ArgumentBuffer, Offset);
	}

	auto FRHICommandListBase::SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"SetViewport requires an active graphics pipeline while recording.");
		RecordCommand<FSetViewportCommand>(MinX, MinY, MinZ, MaxX, MaxY, MaxZ);
	}

	auto FRHICommandListBase::SetScissor(float MinX, float MinY, float Width, float Height) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"SetScissor requires an active graphics pipeline while recording.");
		RecordCommand<FSetScissorCommand>(MinX, MinY, Width, Height);
	}

	auto FRHICommandListBase::SetDepthBias(float ConstantFactor, float Clamp,
		float SlopeFactor) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"SetDepthBias requires an active graphics pipeline while recording.");
		checkf(std::isfinite(ConstantFactor) && std::isfinite(Clamp)
			&& std::isfinite(SlopeFactor),
			"SetDepthBias requires finite values.");
		RecordCommand<FSetDepthBiasCommand>(ConstantFactor, Clamp, SlopeFactor);
	}

	auto FRHICommandListBase::WriteBuffer(FRHIBuffer* Buffer, const void* Data,
		uint32 Size, uint32 OffsetBytes) -> void
	{
		require(Data);
		WriteBuffer(Buffer, OffsetBytes, {static_cast<const std::byte*>(Data), Size});
	}

	auto FRHICommandListBase::WriteBuffer(FRHIBuffer* Buffer, uint32 Offset, FByteView Data) -> void
	{
		WriteBuffer(Buffer, Offset, FRHIBufferUploadData::Copy(Data));
	}

	auto FRHICommandListBase::WriteBuffer(FRHIBuffer* Buffer, uint32 Offset,
		std::shared_ptr<const FRHIBufferUploadData> Data) -> void
	{
		require(IsRecording());
		require(Buffer && Data);
		const auto Bytes = Data->GetData();
		require(!Bytes.empty() && Offset <= Buffer->GetSize() && Bytes.size() <= Buffer->GetSize() - Offset);
		RecordCommand<FBufferWriteCommand>(TRefCountPtr<FRHIBuffer>(Buffer), Offset, std::move(Data));
	}

	auto FRHICommandListBase::InitializeTexture(FRHITexture* Texture) -> void
	{
		RecordCommand<FInitializeTextureCommand>(Texture);
	}

	auto FRHICommandListBase::UpdateTexture2D(FRHITexture* Texture, uint32 MipIndex,
		uint32 ArraySlice, const FUpdateTextureRegion2D& Region, uint32 SourcePitch, FByteView Source) -> void
	{
		require(Texture);
		auto Data = FRHITextureUploadData::Pack2D(GetTextureUploadDesc(*Texture),
			MipIndex, ArraySlice, Region, SourcePitch, Source);
		requiref(Data, "Invalid RHI texture upload: {}", ToString(Data.error()));
		UpdateTexture2D(Texture, std::move(*Data));
	}

	auto FRHICommandListBase::UpdateTexture3D(FRHITexture* Texture, uint32 MipIndex,
		const FUpdateTextureRegion3D& Region, uint32 RowPitch, uint32 DepthPitch, FByteView Source) -> void
	{
		require(Texture);
		auto Data = FRHITextureUploadData::Pack3D(GetTextureUploadDesc(*Texture),
			MipIndex, Region, RowPitch, DepthPitch, Source);
		requiref(Data, "Invalid RHI volume texture upload: {}", ToString(Data.error()));
		UpdateTexture3D(Texture, std::move(*Data));
	}

	auto FRHICommandListBase::UpdateTexture2D(FRHITexture* Texture,
		std::shared_ptr<const FRHITextureUploadData> Data) -> void
	{
		require(Texture && Data && !Data->IsVolume() && Texture->GetFormat() == Data->GetFormat());
		require(ValidateTexture2DUpdate(GetTextureUploadDesc(*Texture), Data->GetMip(),
			Data->GetLayer(), Data->GetRegion2D(), Data->GetLayout().RowPitch).has_value());
		RecordCommand<FUpdateTextureCommand>(TRefCountPtr<FRHITexture>(Texture), std::move(Data));
	}

	auto FRHICommandListBase::UpdateTexture3D(FRHITexture* Texture,
		std::shared_ptr<const FRHITextureUploadData> Data) -> void
	{
		require(Texture && Data && Data->IsVolume() && Texture->GetFormat() == Data->GetFormat());
		require(ValidateTexture3DUpdate(GetTextureUploadDesc(*Texture), Data->GetMip(),
			Data->GetRegion3D(), Data->GetLayout().RowPitch, Data->GetLayout().DepthPitch).has_value());
		RecordCommand<FUpdateTextureCommand>(TRefCountPtr<FRHITexture>(Texture), std::move(Data));
	}

	auto FRHICommandListBase::PushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics
			|| ActivePipeline == ERHIPipeline::Compute,
			"PushConstants requires an active graphics or compute pipeline while recording.");
		checkf(ActivePipeline != ERHIPipeline::Compute
			|| StageFlags == EShaderStageFlags::Compute,
			"Compute push constants require compute-only stage visibility.");
		RecordCommand<FPushConstantsCommand>(StageFlags, Offset, Size, Data);
	}

	auto FRHICommandListBase::SetShaderParameters(FRHIShader* InShader, std::span<const FRHIShaderParameterResource> InResourceParameters) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics
			|| ActivePipeline == ERHIPipeline::Compute,
			"SetShaderParameters requires an active graphics or compute pipeline while recording.");
		checkf(ActivePipeline != ERHIPipeline::Compute || (InShader
			&& InShader->GetFrequency() == EShaderFrequency::Compute),
			"Compute shader parameters require a compute shader.");
		std::vector<FRHIShaderParameterResource> CanonicalParameters;
		if (!CanonicalizeShaderParameters(InResourceParameters, CanonicalParameters)) return;
		RecordCommand<FSetShaderParametersCommand>(InShader, CanonicalParameters);
	}

	auto FRHICommandListBase::SetPreparedShaderParameters(std::shared_ptr<const FRHIShaderParameterBatch> Batch) -> void
	{
		checkf(Batch && (ActivePipeline == ERHIPipeline::Graphics || ActivePipeline == ERHIPipeline::Compute),
			"Prepared shader parameters require a batch and an active pipeline.");
		checkf(ActivePipeline != ERHIPipeline::Compute || Batch->GetShader()->GetFrequency() == EShaderFrequency::Compute,
			"Compute shader parameters require a compute shader.");
		RecordCommand<FSetPreparedShaderParametersCommand>(std::move(Batch));
	}

	FRHICommandListImmediate::FRHICommandListImmediate(
		FRHICommandListExecutor& InExecutor)
		: Executor(&InExecutor)
		, LockState(std::make_unique<FLockState>())
	{
	}

	FRHICommandListImmediate::~FRHICommandListImmediate() = default;

	auto FRHICommandListImmediate::Get() -> FRHICommandListImmediate&
	{
		return GCommandListExecutor.GetImmediateCommandList();
	}

	auto FRHICommandListImmediate::QueueCommandList(FRHICommandList&& CommandList) -> void
	{ Executor->QueueCommandList(CommandList); }

	auto FRHICommandListImmediate::DispatchToRHIThread() -> uint64
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("RHI.DispatchToRHIThread");
		return Executor->Submit({}, ERHISubmitFlags::None);
	}

	auto FRHICommandListImmediate::IsRHIThreadEnabled() const -> bool
	{
		return Executor->State->RHIThread != nullptr;
	}

	auto FRHICommandListImmediate::CanDispatchToRHIThread() const -> bool
	{
		return !bInsideRenderPass
			&& ActiveGPUTimingQueries.empty() && !HasOpenBufferLocks()
			&& !ActiveGPUSubmissionLease;
	}

	auto FRHICommandListImmediate::ImmediateFlush(
		EImmediateFlushType FlushType,
		ERHISubmitFlags SubmitFlags) -> void
	{
		if (FlushType == EImmediateFlushType::WaitForOutstandingTasksOnly)
		{
			return;
		}
		if (FlushType == EImmediateFlushType::FlushRHIThreadFlushResources)
		{
			EnumAddFlags(SubmitFlags, ERHISubmitFlags::DeleteResources);
		}
		if (FlushType >= EImmediateFlushType::FlushRHIThread)
		{
			EnumAddFlags(SubmitFlags, ERHISubmitFlags::FlushRHIThread);
		}
		Executor->Submit({}, SubmitFlags);
	}

	auto FRHICommandListImmediate::LockBuffer(FRHIBuffer* Buffer, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void*
	{
		check(Buffer);
		checkf(LockMode == EResourceLockMode::WriteOnly || LockMode == EResourceLockMode::WriteOnlyDiscard,
			"Recorded buffer locks support WriteOnly and WriteOnlyDiscard modes.");
		check(Size != 0 && Offset <= Buffer->GetSize()
			&& Size <= Buffer->GetSize() - Offset);
		checkf(!LockState->PendingLocks.contains(Buffer),
			"A buffer may only have one open recorded lock.");
		auto Pending = std::make_unique<FLockState::FPendingLock>();
		Pending->Buffer = Buffer;
		Pending->Offset = Offset;
		Pending->Size = Size;
		Pending->bDiscard = LockMode == EResourceLockMode::WriteOnlyDiscard;
		if (LockMode == EResourceLockMode::WriteOnlyDiscard)
			Pending->MappedWrite = Buffer->TryAcquireMappedWrite(Offset, Size);
		void* Result;
		if (Pending->MappedWrite) Result = Pending->MappedWrite->GetData();
		else
		{
			Pending->Data.resize(Size);
			Result = Pending->Data.data();
		}
		LockState->PendingLocks.emplace(Buffer, std::move(Pending));
		return Result;
	}

	auto FRHICommandListImmediate::UnlockBuffer(FRHIBuffer* Buffer) -> void
	{
		check(Buffer);
		const auto It = LockState->PendingLocks.find(Buffer);
		checkf(It != LockState->PendingLocks.end(),
			"UnlockBuffer requires a matching LockBuffer.");
		std::unique_ptr<FLockState::FPendingLock> Pending = std::move(It->second);
		LockState->PendingLocks.erase(It);
		if (Pending->MappedWrite)
		{
			RecordCommand<FBufferMappedWriteCommand>(std::move(Pending->Buffer),
				Pending->Offset, Pending->Size, std::move(Pending->MappedWrite));
			return;
		}
		RecordCommand<FBufferWriteCommand>(std::move(Pending->Buffer), Pending->Offset,
			FRHIBufferUploadData::Take(std::move(Pending->Data)), Pending->bDiscard);
	}

	auto FRHICommandListImmediate::ReadTexture2D(
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		FByteBuffer& OutData) -> bool
	{
		bool bSucceeded = false;
		Executor->ExecuteSynchronousContextOperation(true,
			[Texture, MipIndex, ArraySlice, &OutData, &bSucceeded](
				IRHICommandContext& Context) {
				bSucceeded = Context.RHIReadTexture2D(
					Texture, MipIndex, ArraySlice, OutData);
			});
		return bSucceeded;
	}

	auto FRHICommandListBase::EnqueueTextureReadback(FRHITexture* Texture,
		uint32 MipIndex, uint32 ArraySlice, std::shared_ptr<FRHITextureReadback> Request) -> std::shared_ptr<FRHITextureReadback>
	{
		if (!Request) Request = std::make_shared<FRHITextureReadback>();
		RecordCommand<FTextureReadbackCommand>(TRefCountPtr<FRHITexture>(Texture), MipIndex, ArraySlice, Request->BeginCopy());
		return Request;
	}

	auto FRHICommandListBase::PollTextureReadbacks() -> void { PollReadbacks(); }
	auto FRHICommandListBase::PollReadbacks() -> void { RecordCommand<FPollReadbacksCommand>(); }
	auto FRHICommandListBase::EnqueueBufferReadback(FRHIBuffer* Source, uint32 Offset, uint32 NumBytes, std::shared_ptr<RHIPrivate::FReadbackState> Request) -> void
	{
		require(Request);
		if (!Source || Offset >= Source->GetSize() || !EnumHasAnyFlags(Source->GetUsage(), EBufferUsageFlags::SourceCopy)) { Request->Fail(); return; }
		if (!NumBytes) NumBytes = Source->GetSize() - Offset;
		if (!NumBytes || NumBytes > Source->GetSize() - Offset || Offset % 4 || NumBytes % 4) { Request->Fail(); return; }
		RecordCommand<FBufferReadbackCommand>(TRefCountPtr<FRHIBuffer>(Source), Offset, NumBytes, std::move(Request));
	}
	auto FRHIGPUBufferReadback::EnqueueCopy(FRHICommandListBase& Commands, FRHIBuffer* Source, uint32 NumBytes, uint32 Offset) -> void
	{ Commands.EnqueueBufferReadback(Source, Offset, NumBytes, BeginCopy()); }

	auto FRHIGPUBufferReadback::Wait(FRHICommandListImmediate& Commands) const -> bool
	{
		if (GetState() != ERHIGPUReadbackState::Pending || !IsInFlight()) return IsReady();
		auto* Executor = Commands.Executor;
		auto Resolve = [Request = State](IRHICommandContext& Context) {
			DURIN_PROFILE_CPU_ZONE_NAMED("RHI.Readback.ResolveOnExecutor");
			if (Request->GetState() == ERHIGPUReadbackState::Pending && Request->IsInFlight())
				Context.RHIWaitForBufferReadback(*Request);
		};
		auto* RHIThread = Executor->State->RHIThread;
		if (!RHIThread)
		{
			Executor->ExecuteSynchronousContextOperation(true, std::move(Resolve));
			return IsReady();
		}
		checkf(!Commands.HasOpenBufferLocks(), "A buffer readback wait requires all buffer locks to be closed.");
		Executor->State->SynchronousOperationCount.fetch_add(1, std::memory_order_relaxed);
		Executor->Submit({}, ERHISubmitFlags::None);
		if (GetState() != ERHIGPUReadbackState::Pending || !IsInFlight()) return IsReady();

		struct FFallback
		{
			std::mutex Mutex;
			bool bWaitReturned = false;
			bool bFinished = false; // Accessed under the readback state's mutex.
		};
		const auto Fallback = std::make_shared<FFallback>();
		FRHIThreadWork Work;
		Work.PayloadBytes = sizeof(FFallback);
		Work.Execute = [Executor, Request = State, Fallback, Resolve = std::move(Resolve)] {
			std::lock_guard Lock(Fallback->Mutex);
			if (Fallback->bWaitReturned) return;
			Resolve(Executor->State->ReplayContext.GetOperationContext("Buffer readback wait"));
			Request->FinishFallback(Fallback->bFinished);
		};
		RHIThread->Enqueue(std::move(Work));
		State->WaitForResult(Fallback->bFinished);
		// A queued fallback must not inspect a later copy using this readback.
		std::lock_guard Lock(Fallback->Mutex);
		Fallback->bWaitReturned = true;
		return IsReady();
	}

	auto FRHICommandListImmediate::BlockUntilGPUIdle() -> void
	{
		Executor->ExecuteSynchronousContextOperation(true,
			[](IRHICommandContext& Context) {
				Context.RHIBlockUntilGPUIdle();
			});
	}

	auto FRHICommandListImmediate::HasOpenBufferLocks() const -> bool
	{
		return !LockState->PendingLocks.empty();
	}

	FRHICommandListFence::FRHICommandListFence(
		FRHICommandListExecutor& InExecutor,
		uint64 InTargetSerial)
		: Executor(&InExecutor)
		, TargetSerial(InTargetSerial)
	{
	}

	auto FRHICommandListFence::IsComplete() const -> bool
	{
		return !Executor || Executor->IsSerialComplete(TargetSerial);
	}

	auto FRHICommandListFence::Wait() const -> void
	{
		if (Executor) Executor->WaitForSerial(TargetSerial);
	}

	FRHICommandListExecutor::FRHICommandListExecutor()
		: State(std::make_unique<FState>())
		, CommandListImmediate(*this)
	{
	}

	FRHICommandListExecutor::FRHICommandListExecutor(
		IRHICommandContext& InGraphicsContext)
		: State(std::make_unique<FState>(&InGraphicsContext))
		, CommandListImmediate(*this)
	{
	}

	FRHICommandListExecutor::FRHICommandListExecutor(
		IRHICommandContext& InGraphicsContext,
		FRHIThread& InRHIThread)
		: State(std::make_unique<FState>(&InGraphicsContext, &InRHIThread))
		, CommandListImmediate(*this)
	{
	}

	FRHICommandListExecutor::~FRHICommandListExecutor()
	{
		if (State->RHIThread)
		{
			WaitForSerial(GetLastSubmittedSerial());
		}
	}

	auto FRHICommandListExecutor::GetImmediateCommandList()
		-> FRHICommandListImmediate&
	{
		return CommandListImmediate;
	}

	auto FRHICommandListExecutor::QueueCommandList(
		FRHICommandList& CommandList) -> void
	{
		checkf(!CommandListImmediate.bInsideRenderPass,
			"QueueCommandList cannot split an active immediate render pass.");
		checkf(CommandListImmediate.ActiveGPUTimingQueries.empty(),
			"QueueCommandList cannot split an active immediate GPU timing query.");
		if (!CommandList.IsFinished() || CommandList.RenderPassParent)
		{
			RHITerminateOnFailure("QueueCommandList", "Command list is not finished or is a render-pass child");
		}
		if (CommandList.GetNumRecordedCommands() == 0)
		{
			CommandList.MarkAdmitted();
			return;
		}
		SealImmediateSegment();
		const ERHIPipeline InsertedFinalPipeline = CommandList.ActivePipeline;
		State->PendingBatches.emplace_back(CommandList.DetachStorage());
		CommandList.MarkAdmitted();
		if (InsertedFinalPipeline != CommandListImmediate.ActivePipeline)
		{
			CommandListImmediate.RecordCommand<FSwitchPipelineCommand>(
				CommandListImmediate.ActivePipeline);
		}
		return;
	}

	auto FRHICommandListExecutor::SealImmediateSegment() -> void
	{
		if (CommandListImmediate.GetNumRecordedCommands() == 0)
		{
			return;
		}
		State->PendingBatches.emplace_back(
			CommandListImmediate.DetachStorage());
	}

	auto FRHICommandListExecutor::Submit(
		const std::vector<FRHICommandList*>& AdditionalCmdLists,
		ERHISubmitFlags SubmitFlags)
		-> uint64
	{
		if (CommandListImmediate.HasOpenBufferLocks()
			|| CommandListImmediate.bInsideRenderPass
			|| !CommandListImmediate.ActiveGPUTimingQueries.empty())
		{
			RHITerminateOnFailure("RHI submission", "InvalidCommandList");
		}
		std::unordered_set<FRHICommandList*> UniqueLists;
		for (FRHICommandList* CommandList : AdditionalCmdLists)
		{
			if (!CommandList || !CommandList->IsFinished() || CommandList->RenderPassParent)
			{
				RHITerminateOnFailure("RHI submission", "InvalidCommandList");
			}
			if (!UniqueLists.emplace(CommandList).second)
			{
				RHITerminateOnFailure("RHI submission", "InvalidCommandList");
			}
		}

		SealImmediateSegment();
		ERHIPipeline AppendedFinalPipeline = CommandListImmediate.ActivePipeline;
		bool bAppendedCommands = false;
		for (FRHICommandList* CommandList : AdditionalCmdLists)
		{
			if (CommandList->GetNumRecordedCommands() != 0)
			{
				State->PendingBatches.emplace_back(
					CommandList->DetachStorage());
				AppendedFinalPipeline = CommandList->ActivePipeline;
				bAppendedCommands = true;
			}
			CommandList->MarkAdmitted();
		}
		if (bAppendedCommands
			&& AppendedFinalPipeline != CommandListImmediate.ActivePipeline)
		{
			CommandListImmediate.RecordCommand<FSwitchPipelineCommand>(
				CommandListImmediate.ActivePipeline);
			SealImmediateSegment();
		}

		const bool bHasOrderedEvent = EnumHasAnyFlags(
			SubmitFlags,
			ERHISubmitFlags::SubmitToGPU
				| ERHISubmitFlags::DeleteResources
				| ERHISubmitFlags::EndFrame
				| ERHISubmitFlags::BeginFrame);
		if (State->PendingBatches.empty() && !bHasOrderedEvent)
		{
			const uint64 Serial = GetLastSubmittedSerial();
			if (EnumHasAnyFlags(SubmitFlags, ERHISubmitFlags::FlushRHIThread))
			{
				WaitForSerial(Serial);
			}
			return Serial;
		}

		auto SubmissionGroup = std::make_shared<FState::FSubmissionGroup>(
			SubmitFlags, std::move(State->PendingBatches));
		State->PendingBatches.clear();
		const size_t GroupCommandCount = SubmissionGroup->GetCommandCount();
		const size_t GroupPayloadBytes = SubmissionGroup->GetPayloadBytes();
		const size_t GroupBatchCount = SubmissionGroup->GetBatchCount();
		auto RecordAcceptedSubmission = [this, GroupCommandCount,
			GroupPayloadBytes, GroupBatchCount]() {
			State->RecordedCommandCount.fetch_add(
				GroupCommandCount, std::memory_order_relaxed);
			State->RecordedPayloadBytes.fetch_add(
				GroupPayloadBytes, std::memory_order_relaxed);
			State->SubmittedBatchCount.fetch_add(
				GroupBatchCount, std::memory_order_relaxed);
			State->SubmissionGroupCount.fetch_add(1, std::memory_order_relaxed);
		};
		auto Replay = [this](FState::FSubmissionGroup& Group) {
			struct FReplayScope
			{
				bool Previous = GExecutingRHICommands;
				FReplayScope() { GExecutingRHICommands = true; }
				~FReplayScope() { GExecutingRHICommands = Previous; }
			} ReplayScope;
			if (State->RHIThread)
			{
				CheckRHIThread();
			}
			const auto ReplayStart = std::chrono::steady_clock::now();
			if (EnumHasAnyFlags(Group.GetFlags(), ERHISubmitFlags::BeginFrame))
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("RHI.BeginFrame.Replay");
				const FRHIBeginFrameArgs BeginFrameArgs{
					.FrameNumber = State->FrameNumber.load(std::memory_order_acquire)
				};
				if (State->ReplayContext.HasGraphicsContextOverride())
				{
					IRHICommandContext& Context =
						State->ReplayContext.GetOperationContext("BeginFrame");
					Context.RHIBeginFrame(BeginFrameArgs);
				}
				else
				{
					check(GDynamicRHI);
					GDynamicRHI->RHIBeginFrame(BeginFrameArgs);
				}
			}
			// Uploads/readbacks may submit midway through replay. Unwinding detaches
			// this active owner while preserving every backend payload lease.
			struct FStorageOwnerScope
			{
				FRHICommandReplayContext* Context = nullptr;
				~FStorageOwnerScope() { if (Context) Context->SetStorageOwner({}); }
			} StorageOwnerScope;
			if (Group.GetCommandCount() != 0
				&& (GDynamicRHI || State->ReplayContext.HasGraphicsContextOverride()))
			{
				StorageOwnerScope.Context = &State->ReplayContext;
				StorageOwnerScope.Context->SetStorageOwner(Group.GetStorageOwner());
			}
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("RHI.Commands.Replay");
				DURIN_PROFILE_CPU_ZONE_TEXT(std::format("commands={}", Group.GetCommandCount()));
				Group.Replay(State->ReplayContext);
			}
			const auto ReplayEnd = std::chrono::steady_clock::now();
			State->ReplayDurationNanoseconds.fetch_add(
				static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					ReplayEnd - ReplayStart).count()),
				std::memory_order_relaxed);

			if (EnumHasAnyFlags(Group.GetFlags(), ERHISubmitFlags::SubmitToGPU))
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("RHI.SubmitCommands");
				State->ReplayContext.GetOperationContext("SubmitToGPU")
					.RHISubmitCommands();
			}
			if (EnumHasAnyFlags(Group.GetFlags(), ERHISubmitFlags::EndFrame))
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("RHI.EndFrame.Replay");
				if (State->ReplayContext.HasGraphicsContextOverride())
				{
					State->ReplayContext.GetOperationContext("EndFrame").RHIEndFrame();
				}
				else
				{
					check(GDynamicRHI);
					GDynamicRHI->RHIEndFrame();
				}
				const uint64 FrameNumber =
					State->FrameNumber.load(std::memory_order_relaxed);
				checkf(FrameNumber != std::numeric_limits<uint64>::max(),
					"RHI executor frame number exhausted.");
				State->FrameNumber.store(
					FrameNumber + 1, std::memory_order_release);
			}
			if (StorageOwnerScope.Context)
			{
				StorageOwnerScope.Context->SetStorageOwner({});
				StorageOwnerScope.Context = nullptr;
			}
			Group.ReleaseBatches();
			if (EnumHasAnyFlags(Group.GetFlags(), ERHISubmitFlags::DeleteResources))
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("RHI.DeleteDeferredResources");
				DeleteDeferredResources();
			}
		};

		if (State->RHIThread)
		{
			static_assert(sizeof(size_t) <= sizeof(uint64));
			if (GroupBatchCount > std::numeric_limits<uint32>::max())
			{
				RHITerminateOnFailure("RHI submission", "Oversized");
			}
			FRHIThreadWork Work;
			Work.FrameCount = EnumHasAnyFlags(SubmitFlags, ERHISubmitFlags::EndFrame) ? 1u : 0u;
			Work.BatchCount = static_cast<uint32>(GroupBatchCount);
			Work.PayloadBytes = static_cast<uint64>(GroupPayloadBytes);
			Work.Execute = [Replay, SubmissionGroup]() mutable {
				Replay(*SubmissionGroup);
			};
			const uint64 Serial = State->RHIThread->Enqueue(std::move(Work));

			RecordAcceptedSubmission();
			if (EnumHasAnyFlags(SubmitFlags, ERHISubmitFlags::FlushRHIThread))
			{
				WaitForSerial(Serial);
			}
			return Serial;
		}

		const uint64 PreviousSerial =
			State->LastSubmittedSerial.load(std::memory_order_relaxed);
		if (PreviousSerial == std::numeric_limits<uint64>::max())
		{
			RHITerminateOnFailure("RHI submission", "SerialExhausted");
		}
		const uint64 Serial = PreviousSerial + 1;
		RecordAcceptedSubmission();
		State->LastSubmittedSerial.store(Serial, std::memory_order_release);
		Replay(*SubmissionGroup);
		SubmissionGroup.reset();
		{
			std::lock_guard Lock(State->CompletionMutex);
			State->CompletedSerial.store(Serial, std::memory_order_release);
		}
		State->CompletionCV.notify_all();
		if (EnumHasAnyFlags(SubmitFlags, ERHISubmitFlags::FlushRHIThread))
		{
			WaitForSerial(Serial);
		}
		return Serial;
	}

	auto FRHICommandListExecutor::ExecuteSynchronousOperation(
		bool bFlushRecordedCommands,
		std::function<void()> Operation,
		size_t OwnedPayloadBytes) -> void
	{
		check(Operation);
		ExecuteSynchronousContextOperation(
			bFlushRecordedCommands,
			[Operation = std::move(Operation)](IRHICommandContext&) mutable {
				Operation();
			},
			OwnedPayloadBytes);
	}

	auto FRHICommandListExecutor::ExecuteSynchronousContextOperation(
		bool bFlushRecordedCommands,
		std::function<void(IRHICommandContext&)> Operation,
		size_t OwnedPayloadBytes) -> void
	{
		checkf(!CommandListImmediate.HasOpenBufferLocks(),
			"A synchronous RHI operation requires every buffer lock to be unlocked.");
		check(Operation);
		State->SynchronousOperationCount.fetch_add(1, std::memory_order_relaxed);
		if (bFlushRecordedCommands)
		{
			Submit({}, ERHISubmitFlags::None);
		}
		if (!State->RHIThread)
		{
			Operation(State->ReplayContext.GetOperationContext(
				"Synchronous RHI operation"));
			return;
		}

		FRHIThreadWork Work;
		Work.PayloadBytes = static_cast<uint64>(OwnedPayloadBytes);
		Work.Execute = [this, Operation = std::move(Operation)]() mutable {
			CheckRHIThread();
			Operation(State->ReplayContext.GetOperationContext(
				"Synchronous RHI operation"));
		};
		const uint64 Serial = State->RHIThread->Enqueue(std::move(Work));

		WaitForSerial(Serial);
	}

	auto FRHICommandListExecutor::CreateFence() -> FRHICommandListFence
	{
		return CreateFence(GetLastSubmittedSerial());
	}

	auto FRHICommandListExecutor::CreateFence(uint64 TargetSerial)
		-> FRHICommandListFence
	{
		requiref(TargetSerial <= GetLastSubmittedSerial(), "RHI fence requires an already submitted serial.");
		return FRHICommandListFence(*this, TargetSerial);
	}

	auto FRHICommandListExecutor::GetLastSubmittedSerial() const -> uint64
	{
		if (State->RHIThread)
		{
			return State->RHIThread->CaptureLastSubmittedSerial();
		}
		return State->LastSubmittedSerial.load(std::memory_order_acquire);
	}

	auto FRHICommandListExecutor::GetCompletedSerial() const -> uint64
	{
		if (State->RHIThread)
		{
			return State->RHIThread->GetStats().CompletedSerial;
		}
		return State->CompletedSerial.load(std::memory_order_acquire);
	}

	auto FRHICommandListExecutor::GetFrameNumber() const -> uint64
	{
		return State->FrameNumber.load(std::memory_order_acquire);
	}

	auto FRHICommandListExecutor::GetStats() const
		-> FRHICommandListExecutorStats
	{
		size_t PendingPayloadBytes = 0;
		for (const FState::FBatch& Batch : State->PendingBatches)
		{
			PendingPayloadBytes = CheckedAddPayloadBytes(
				PendingPayloadBytes, Batch.GetPayloadBytes());
		}
		const FRHIThreadStats ThreadStats = State->RHIThread
			? State->RHIThread->GetStats()
			: FRHIThreadStats{};
		return {
			.Mode = State->RHIThread
				? ERHICommandListExecutorMode::Threaded
				: ERHICommandListExecutorMode::Inline,
			.RecordedCommandCount = State->RecordedCommandCount.load(std::memory_order_relaxed),
			.RecordedPayloadBytes = State->RecordedPayloadBytes.load(std::memory_order_relaxed),
			.SubmittedBatchCount = State->SubmittedBatchCount.load(std::memory_order_relaxed),
			.SubmissionGroupCount = State->SubmissionGroupCount.load(std::memory_order_relaxed),
			.ReplayDurationNanoseconds = State->ReplayDurationNanoseconds.load(std::memory_order_relaxed),
			.WaitCount = State->WaitCount.load(std::memory_order_relaxed),
			.SynchronousOperationCount = State->SynchronousOperationCount.load(
				std::memory_order_relaxed),
			.PendingBatchCount = static_cast<uint64>(State->PendingBatches.size())
				+ ThreadStats.OutstandingBatchCount,
			.PendingPayloadBytes = CheckedAddPayloadBytes(
				PendingPayloadBytes,
				static_cast<size_t>(ThreadStats.OutstandingPayloadBytes)),
			.LastSubmittedSerial = GetLastSubmittedSerial(),
			.CompletedSerial = GetCompletedSerial(),
			.WaitDurationNanoseconds = State->WaitDurationNanoseconds.load(
				std::memory_order_relaxed),
			.BackpressureWaitCount = ThreadStats.BackpressureWaitCount,
			.PeakQueueEntryCount = ThreadStats.PeakOutstandingEntryCount,
			.PeakQueueBatchCount = ThreadStats.PeakOutstandingBatchCount,
			.PeakQueuePayloadBytes = ThreadStats.PeakOutstandingPayloadBytes,
			.PendingFrameCount = ThreadStats.OutstandingFrameCount,
			.PeakQueueFrameCount = ThreadStats.PeakOutstandingFrameCount,
			.FramePressureWaitCount = ThreadStats.FramePressureWaitCount,
			.FramePressureWaitNanoseconds = ThreadStats.FramePressureWaitNanoseconds,
		};
	}

	auto FRHICommandListExecutor::SetThreadedMode(FRHIThread& InRHIThread) -> void
	{
		checkf(State->RHIThread == nullptr,
			"RHI command-list executor is already threaded.");
		checkf(GetLastSubmittedSerial() == GetCompletedSerial(),
			"Threaded mode requires an idle inline executor.");
		const FRHIThreadStats ThreadStats = InRHIThread.GetStats();
		checkf(ThreadStats.AdmissionState == ERHIThreadAdmissionState::Running,
			"Threaded mode requires a running RHI thread.");
		State->LastSubmittedSerial.store(
			InRHIThread.CaptureLastSubmittedSerial(), std::memory_order_release);
		State->RHIThread = &InRHIThread;
	}

	auto FRHICommandListExecutor::SetInlineMode() -> void
	{
		if (State->RHIThread)
		{
			const FRHIThreadStats ThreadStats = State->RHIThread->GetStats();
			const uint64 LastSubmittedSerial = ThreadStats.LastSubmittedSerial;
			WaitForSerial(LastSubmittedSerial);
			State->LastSubmittedSerial.store(
				LastSubmittedSerial, std::memory_order_release);
			State->CompletedSerial.store(
				LastSubmittedSerial,
				std::memory_order_release);
			State->RHIThread = nullptr;
		}
	}

	auto FRHICommandListExecutor::IsSerialComplete(uint64 Serial) const -> bool
	{
		return GetCompletedSerial() >= Serial;
	}

	auto FRHICommandListExecutor::WaitForSerial(uint64 Serial) const -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("RHI.WaitForSerial");
		State->WaitCount.fetch_add(1, std::memory_order_relaxed);
		const auto WaitStart = std::chrono::steady_clock::now();
		if (IsSerialComplete(Serial))
		{
			return;
		}

		if (IsExecutingRHICommands()) RHITerminateOnFailure("RHI wait", "SelfWait");
		if (State->RHIThread)
		{
			State->RHIThread->WaitForSerial(Serial);
		}
		else
		{
			std::unique_lock Lock(State->CompletionMutex);
			State->CompletionCV.wait(Lock, [this, Serial]() {
				return IsSerialComplete(Serial);
			});
		}
		const auto WaitEnd = std::chrono::steady_clock::now();
		State->WaitDurationNanoseconds.fetch_add(
			static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				WaitEnd - WaitStart).count()), std::memory_order_relaxed);
	}

} // namespace Durin
