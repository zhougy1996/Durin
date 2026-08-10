#include "RHICommandList.h"

#include "DynamicRHI.h"
#include "RHIContext.h"
#include "RHIThread.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GInvalidDiagnosticRegionCount = 0;

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

		auto CreateBufferViewForRecording(
			FRHIBuffer* Buffer,
			const FRHIBufferViewDesc& Desc) -> FBufferViewRHIRef
		{
			if (GDynamicRHI) return GDynamicRHI->RHIGetOrCreateBufferView(Buffer, Desc);
			std::string Error;
			return ValidateBufferViewDesc(Buffer, Desc, Error)
				? FBufferViewRHIRef(new FRHIBufferView(Buffer, Desc)) : nullptr;
		}

		auto CreateTextureViewForRecording(
			FRHITexture* Texture,
			const FRHITextureViewDesc& Desc) -> FTextureViewRHIRef
		{
			if (GDynamicRHI) return GDynamicRHI->RHIGetOrCreateTextureView(Texture, Desc);
			std::string Error;
			return ValidateTextureViewDesc(Texture, Desc, Error)
				? FTextureViewRHIRef(new FRHITextureView(Texture, Desc)) : nullptr;
		}

		auto CanonicalizeShaderParameters(
			std::span<const FRHIShaderParameterResource> Input,
			std::vector<FRHIShaderParameterResource>& Output,
			std::vector<TRefCountPtr<FRHIResource>>& CreatedViews) -> void
		{
			Output.assign(Input.begin(), Input.end());
			CreatedViews.reserve(Output.size());
			for (FRHIShaderParameterResource& Parameter : Output)
			{
				if (Parameter.Resource == nullptr || Parameter.Type == ERHIBindingType::Sampler) continue;
				if (Parameter.Type == ERHIBindingType::Texture || Parameter.Type == ERHIBindingType::StorageImage)
				{
					if (Parameter.Resource->GetResourceType() == ERHIResourceType::TextureView) continue;
					auto* Texture = static_cast<FRHITexture*>(Parameter.Resource);
					if (Texture->GetResourceType() == ERHIResourceType::TextureReference)
					{
						Texture = static_cast<FRHITextureReference*>(Texture)->GetReferencedTexture_RenderThread();
					}
					checkf(Texture && Texture->GetResourceType() == ERHIResourceType::Texture,
						"Shader texture binding requires a texture or texture view.");
					FRHITextureViewDesc Desc = MakeDefaultTextureViewDesc(*Texture,
						Parameter.Type == ERHIBindingType::StorageImage
							? ERHITextureViewUsage::Storage : ERHITextureViewUsage::Sampled);
					FTextureViewRHIRef View = CreateTextureViewForRecording(Texture, Desc);
					checkf(View, "Shader texture binding could not create its canonical view.");
					Parameter.Resource = View.GetReference();
					CreatedViews.emplace_back(View.GetReference());
					continue;
				}

				if (Parameter.Type == ERHIBindingType::UniformBuffer
					|| Parameter.Type == ERHIBindingType::UniformBufferDynamic
					|| Parameter.Type == ERHIBindingType::StorageBuffer)
				{
					if (Parameter.Resource->GetResourceType() == ERHIResourceType::BufferView) continue;
					checkf(Parameter.Resource->GetResourceType() == ERHIResourceType::Buffer,
						"Shader buffer binding requires a buffer or buffer view.");
					auto* Buffer = static_cast<FRHIBuffer*>(Parameter.Resource);
					const bool bDynamic = Parameter.Type == ERHIBindingType::UniformBufferDynamic;
					const uint64 ViewOffset = bDynamic ? 0 : Parameter.Offset;
					const uint64 ViewSize = Parameter.Size != 0
						? Parameter.Size : Buffer->GetSize() - Parameter.Offset;
					if (bDynamic)
					{
						checkf(Parameter.Offset <= Buffer->GetSize()
							&& ViewSize <= Buffer->GetSize() - Parameter.Offset,
							"Dynamic uniform range exceeds its parent buffer.");
					}
					ERHIBufferViewType ViewType = ERHIBufferViewType::Uniform;
					if (Parameter.Type == ERHIBindingType::StorageBuffer)
					{
						ViewType = EnumHasAnyFlags(Buffer->GetUsage(), EBufferUsageFlags::ByteAddressBuffer)
							? ERHIBufferViewType::ByteAddressStorage
							: ERHIBufferViewType::StructuredStorage;
					}
					const FRHIBufferViewDesc Desc{ViewOffset, ViewSize, ViewType, EPixelFormat::Unknown};
					FBufferViewRHIRef View = CreateBufferViewForRecording(Buffer, Desc);
					checkf(View, "Shader buffer binding could not create its canonical view.");
					Parameter.Resource = View.GetReference();
					Parameter.Size = 0;
					if (!bDynamic) Parameter.Offset = 0;
					CreatedViews.emplace_back(View.GetReference());
					continue;
				}
				checkf(false, "Unsupported shader parameter binding type.");
			}
		}
	}

	class FRHICommandReplayContext
	{
	public:
		explicit FRHICommandReplayContext(IRHICommandContext* InGraphicsContext = nullptr)
			: GraphicsContextOverride(InGraphicsContext)
		{
		}

		auto SwitchPipeline(ERHIPipeline Pipeline) -> void
		{
			ActivePipeline = Pipeline;
			switch (Pipeline)
			{
			case ERHIPipeline::Graphics:
				if (GraphicsContextOverride)
				{
					ActiveContext = GraphicsContextOverride;
				}
				else
				{
					checkf(GDynamicRHI,
						"SwitchPipeline(Graphics) replay requires an initialized dynamic RHI.");
					ActiveContext = GDynamicRHI->RHIGetDefaultContext();
				}
				checkf(ActiveContext,
					"SwitchPipeline(Graphics) replay could not resolve a graphics context.");
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

		auto GetOperationContext(const char* OperationName) const
			-> IRHICommandContext&
		{
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
					Info.ColorRenderTargets[Index] = ColorRenderTargets[Index].GetReference();
					Info.ColorResolveTargets[Index] = ColorResolveTargets[Index].GetReference();
					if (Info.ColorRenderTargets[Index])
					{
						ColorRenderTargetViews[Index] = Info.ColorRenderTargetViews[Index];
						if (!ColorRenderTargetViews[Index])
						{
							ColorRenderTargetViews[Index] = CreateTextureViewForRecording(
								Info.ColorRenderTargets[Index], MakeDefaultTextureViewDesc(
									*Info.ColorRenderTargets[Index], ERHITextureViewUsage::ColorAttachment));
						}
						checkf(ColorRenderTargetViews[Index], "Render target could not create its attachment view.");
						Info.ColorRenderTargetViews[Index] = ColorRenderTargetViews[Index].GetReference();
					}
					if (Info.ColorResolveTargets[Index])
					{
						ColorResolveTargetViews[Index] = Info.ColorResolveTargetViews[Index];
						if (!ColorResolveTargetViews[Index])
						{
							ColorResolveTargetViews[Index] = CreateTextureViewForRecording(
								Info.ColorResolveTargets[Index], MakeDefaultTextureViewDesc(
									*Info.ColorResolveTargets[Index], ERHITextureViewUsage::ColorAttachment));
						}
						checkf(ColorResolveTargetViews[Index], "Resolve target could not create its attachment view.");
						Info.ColorResolveTargetViews[Index] = ColorResolveTargetViews[Index].GetReference();
					}
				}
				DepthStencilRenderTarget = Info.DepthStencilRenderTarget;
				Info.DepthStencilRenderTarget = DepthStencilRenderTarget.GetReference();
				if (Info.DepthStencilRenderTarget)
				{
					DepthStencilRenderTargetView = Info.DepthStencilRenderTargetView;
					if (!DepthStencilRenderTargetView)
					{
						DepthStencilRenderTargetView = CreateTextureViewForRecording(
							Info.DepthStencilRenderTarget, MakeDefaultTextureViewDesc(
								*Info.DepthStencilRenderTarget, ERHITextureViewUsage::DepthStencilAttachment));
					}
					checkf(DepthStencilRenderTargetView, "Depth/stencil target could not create its attachment view.");
					Info.DepthStencilRenderTargetView = DepthStencilRenderTargetView.GetReference();
				}
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
					.GetGraphicsContext("PushConstants")
					.RHIPushConstants(
						StageFlags, Offset, static_cast<uint32>(Data.size()), Data.data());
			}

			auto GetOwnedPayloadBytes() const -> size_t
			{
				return Data.capacity() * sizeof(Data.front());
			}

			EShaderStageFlags StageFlags;
			uint32 Offset;
			std::vector<uint8> Data;
		};

		struct FWriteBufferCommand
		{
			FWriteBufferCommand(
				FRHIBuffer* InBuffer,
				uint32 InOffset,
				const void* InData,
				uint32 InSize)
				: Buffer(InBuffer), Offset(InOffset), Data(InSize)
			{
				check(Buffer && InData && InSize != 0);
				check(Offset <= Buffer->GetSize() && InSize <= Buffer->GetSize() - Offset);
				if (InSize != 0)
				{
					std::memcpy(Data.data(), InData, InSize);
				}
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetOperationContext("WriteBuffer")
					.RHIWriteBuffer(Buffer.GetReference(), Offset, Data);
			}

			auto GetOwnedPayloadBytes() const -> size_t
			{
				return Data.capacity() * sizeof(Data.front());
			}

			TRefCountPtr<FRHIBuffer> Buffer;
			uint32 Offset;
			std::vector<uint8> Data;
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

		struct FAcquireBackBufferCommand
		{
			explicit FAcquireBackBufferCommand(FRHITexture* InBackBuffer)
				: BackBuffer(InBackBuffer)
			{
				check(BackBuffer);
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetOperationContext("AcquireBackBuffer")
					.RHIAcquireBackBuffer(BackBuffer.GetReference());
			}

			TRefCountPtr<FRHITexture> BackBuffer;
		};

		struct FUpdateTexture2DCommand
		{
			FUpdateTexture2DCommand(
				FRHITexture* InTexture,
				uint32 InMipIndex,
				uint32 InArraySlice,
				const FUpdateTextureRegion2D& InRegion,
				uint32 InSourcePitch,
				const uint8* InSourceData)
				: Texture(InTexture)
				, MipIndex(InMipIndex)
				, ArraySlice(InArraySlice)
				, Region(InRegion)
			{
				check(Texture && InSourceData);
				FRHITextureDesc Desc;
				Desc.Dimension = Texture->GetDimension();
				Desc.Extent = FIntPoint(Texture->GetSizeX(), Texture->GetSizeY());
				Desc.Format = Texture->GetFormat();
				Desc.ArraySize = Texture->GetArraySize();
				Desc.NumMips = Texture->GetNumMips();
				Desc.NumSamples = Texture->GetNumSamples();
				std::string Error;
				checkf(ValidateTexture2DUpdate(
					Desc, MipIndex, ArraySlice, Region, InSourcePitch, Error),
					"Invalid RHI texture upload: {}", Error);

				const FPixelFormatInfo& FormatInfo = GetPixelFormatInfo(Texture->GetFormat());
				const FPixelFormatLayout Layout = GetPixelFormatLayout(
					Texture->GetFormat(), Region.Width, Region.Height);
				check(Layout.RowPitch <= std::numeric_limits<uint32>::max());
				check(Layout.DataSize <= std::numeric_limits<uint32>::max());
				SourcePitch = static_cast<uint32>(Layout.RowPitch);
				Data.resize(static_cast<size_t>(Layout.DataSize));
				const uint64 SourceBlockX = static_cast<uint32>(Region.SrcX)
					/ FormatInfo.BlockSize;
				const uint64 SourceBlockY = static_cast<uint32>(Region.SrcY)
					/ FormatInfo.BlockSize;
				const uint8* SourceRegion = InSourceData
					+ SourceBlockY * InSourcePitch
					+ SourceBlockX * FormatInfo.BytesPerBlock;
				for (uint64 Row = 0; Row < Layout.BlocksHigh; ++Row)
				{
					std::memcpy(
						Data.data() + Row * SourcePitch,
						SourceRegion + Row * InSourcePitch,
						SourcePitch);
				}
				Region.SrcX = 0;
				Region.SrcY = 0;
			}

			auto Execute(void* ReplayContext) -> void
			{
				GetReplayContext(ReplayContext)
					.GetOperationContext("UpdateTexture2D")
					.RHIUpdateTexture2D(
						Texture.GetReference(), MipIndex, ArraySlice,
						Region, SourcePitch, Data);
			}

			auto GetOwnedPayloadBytes() const -> size_t
			{
				return Data.capacity() * sizeof(Data.front());
			}

			TRefCountPtr<FRHITexture> Texture;
			uint32 MipIndex;
			uint32 ArraySlice;
			FUpdateTextureRegion2D Region;
			uint32 SourcePitch = 0;
			std::vector<uint8> Data;
		};

		struct FSetShaderParametersCommand
		{
			FSetShaderParametersCommand(
				FRHIShader* InShader,
				std::span<FRHIShaderParameterResource> InParameters)
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
					.GetGraphicsContext("SetShaderParameters")
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
				, Batches(std::move(InBatches))
			{
				BatchCount = Batches.size();
				for (const FBatch& Batch : Batches)
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
				for (FBatch& Batch : Batches)
				{
					Batch.Replay(ReplayContext);
				}
			}

			auto ReleaseBatches() -> void
			{
				Batches.clear();
			}

			auto TakeBatches() -> std::vector<FBatch>
			{
				return std::move(Batches);
			}

			auto GetFlags() const -> ERHISubmitFlags { return Flags; }
			auto GetBatchCount() const -> size_t { return BatchCount; }
			auto GetCommandCount() const -> size_t { return CommandCount; }
			auto GetPayloadBytes() const -> size_t { return PayloadBytes; }

		private:
			const ERHISubmitFlags Flags;
			std::vector<FBatch> Batches;
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
		std::atomic<uint64> RejectedSubmissionCount = 0;
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
			std::vector<uint8> Data;
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
		, bInsideRenderPass(Other.bInsideRenderPass)
		, DiagnosticRegionDepth(Other.DiagnosticRegionDepth)
		, RenderPassDiagnosticRegionDepth(Other.RenderPassDiagnosticRegionDepth)
		, ActiveGPUTimingQuery(Other.ActiveGPUTimingQuery)
		, ActiveGPUTimingReservation(std::move(Other.ActiveGPUTimingReservation))
	{
		Other.RecordingState = ERecordingState::MovedFrom;
		Other.ActivePipeline = ERHIPipeline::None;
		Other.bInsideRenderPass = false;
		Other.DiagnosticRegionDepth = 0;
		Other.RenderPassDiagnosticRegionDepth = 0;
		Other.ActiveGPUTimingQuery = nullptr;
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
			bInsideRenderPass = Other.bInsideRenderPass;
			DiagnosticRegionDepth = Other.DiagnosticRegionDepth;
			RenderPassDiagnosticRegionDepth = Other.RenderPassDiagnosticRegionDepth;
			ActiveGPUTimingQuery = Other.ActiveGPUTimingQuery;
			ActiveGPUTimingReservation = std::move(Other.ActiveGPUTimingReservation);
			Other.RecordingState = ERecordingState::MovedFrom;
			Other.ActivePipeline = ERHIPipeline::None;
			Other.bInsideRenderPass = false;
			Other.DiagnosticRegionDepth = 0;
			Other.RenderPassDiagnosticRegionDepth = 0;
			Other.ActiveGPUTimingQuery = nullptr;
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

	auto FRHICommandListBase::RecordAcquireBackBuffer(
		FRHITexture* BackBuffer) -> void
	{
		RecordCommand<FAcquireBackBufferCommand>(BackBuffer);
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
	FRHICommandList::FRHICommandList(bool bImmediate)
	{
		(void)bImmediate;
	}
	FRHICommandList::~FRHICommandList() = default;
	FRHICommandList::FRHICommandList(FRHICommandList&& Other) noexcept = default;
	auto FRHICommandList::operator=(FRHICommandList&& Other) noexcept
		-> FRHICommandList& = default;

	auto FRHICommandList::FinishRecording() -> void
	{
		checkf(RecordingState == ERecordingState::Recording,
			"FinishRecording requires a recording regular command list.");
		checkf(!bInsideRenderPass,
			"FinishRecording cannot seal a command list inside a render pass.");
		if (DiagnosticRegionDepth != 0) RecordInvalidDiagnosticRegion();
		checkf(DiagnosticRegionDepth == 0,
			"FinishRecording cannot seal a command list with open diagnostic regions.");
		checkf(ActiveGPUTimingQuery == nullptr,
			"FinishRecording cannot seal a command list with an open GPU timing query.");
		RecordingState = ERecordingState::Finished;
	}

	auto FRHICommandList::IsFinished() const -> bool
	{
		return FRHICommandListBase::IsFinished();
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
	}

	auto FRHICommandListBase::BeginDiagnosticRegion(std::string_view Name) -> void
	{
		if (Name.empty() || Name.size() > 255 || DiagnosticRegionDepth >= 64)
			RecordInvalidDiagnosticRegion();
		checkf(!Name.empty(), "Diagnostic region names cannot be empty.");
		checkf(Name.size() <= 255,
			"Diagnostic region names cannot exceed 255 bytes.");
		checkf(DiagnosticRegionDepth < 64,
			"Diagnostic region nesting cannot exceed 64 levels.");
		RecordCommand<FBeginDiagnosticRegionCommand>(Name);
		++DiagnosticRegionDepth;
	}

	auto FRHICommandListBase::EndDiagnosticRegion() -> void
	{
		if (DiagnosticRegionDepth == 0) RecordInvalidDiagnosticRegion();
		checkf(DiagnosticRegionDepth != 0,
			"EndDiagnosticRegion requires a matching begin.");
		RecordCommand<FEndDiagnosticRegionCommand>();
		--DiagnosticRegionDepth;
	}

	auto FRHICommandListBase::BeginGPUTimingQuery(
		FRHIGPUTimingQuery* Query) -> void
	{
		checkf(Query, "BeginGPUTimingQuery requires a query.");
		if (ActiveGPUTimingQuery != nullptr)
			FRHIGPUTimingQuery::RecordInvalidRecording();
		checkf(ActiveGPUTimingQuery == nullptr,
			"GPU timing queries cannot overlap on one command list.");
		checkf(Query->TryReserveRecording(),
			"GPU timing query is already recording or pending.");
		auto Reservation = std::make_shared<FGPUTimingRecordingReservation>(Query);
		RecordCommand<FBeginGPUTimingQueryCommand>(Reservation);
		ActiveGPUTimingQuery = Query;
		ActiveGPUTimingReservation = std::move(Reservation);
	}

	auto FRHICommandListBase::EndGPUTimingQuery(
		FRHIGPUTimingQuery* Query) -> void
	{
		if (!Query || ActiveGPUTimingQuery != Query)
			FRHIGPUTimingQuery::RecordInvalidRecording();
		checkf(Query && ActiveGPUTimingQuery == Query,
			"EndGPUTimingQuery requires the active query on this command list.");
		auto Reservation = std::static_pointer_cast<FGPUTimingRecordingReservation>(
			ActiveGPUTimingReservation);
		RecordCommand<FEndGPUTimingQueryCommand>(std::move(Reservation));
		ActiveGPUTimingQuery = nullptr;
		ActiveGPUTimingReservation.reset();
	}

	auto FRHICommandListBase::BeginRenderPass(const FRHIRenderPassInfo& Info, FName Name) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"BeginRenderPass requires an active graphics pipeline while recording.");
		checkf(!bInsideRenderPass,
			"BeginRenderPass cannot be nested while recording.");
		RecordCommand<FBeginRenderPassCommand>(Info, Name);
		bInsideRenderPass = true;
		RenderPassDiagnosticRegionDepth = DiagnosticRegionDepth;
	}

	auto FRHICommandListBase::EndRenderPass() -> void
	{
		checkf(bInsideRenderPass,
			"EndRenderPass requires a matching BeginRenderPass while recording.");
		if (DiagnosticRegionDepth != RenderPassDiagnosticRegionDepth)
			RecordInvalidDiagnosticRegion();
		checkf(DiagnosticRegionDepth == RenderPassDiagnosticRegionDepth,
			"Diagnostic regions cannot cross a render-pass boundary.");
		RecordCommand<FEndRenderPassCommand>();
		bInsideRenderPass = false;
		RenderPassDiagnosticRegionDepth = 0;
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
		if (Transitions.empty()) return;
		checkf(!bInsideRenderPass,
			"Buffer transitions cannot be recorded inside a render pass.");
		std::string Error;
		checkf(ValidateBufferTransitions(Transitions, Error),
			"Invalid RHI buffer transition batch: {}", Error);
		RecordCommand<FBufferTransitionCommand>(Transitions);
	}

	auto FRHICommandListBase::TransitionTextures(
		std::span<const FRHITextureTransition> Transitions) -> void
	{
		if (Transitions.empty()) return;
		checkf(!bInsideRenderPass,
			"Texture transitions cannot be recorded inside a render pass.");
		std::string Error;
		checkf(ValidateTextureTransitions(Transitions, Error),
			"Invalid RHI texture transition batch: {}", Error);
		RecordCommand<FTextureTransitionCommand>(Transitions);
	}

	auto FRHICommandListBase::CopyBuffer(FRHIBuffer* Source, FRHIBuffer* Destination,
		std::span<const FRHIBufferCopyRegion> Regions) -> void
	{
		if (Regions.empty()) return;
		checkf(!bInsideRenderPass, "Buffer copies cannot be recorded inside a render pass.");
		std::string Error;
		checkf(ValidateBufferCopies(Source, Destination, Regions, Error),
			"Invalid RHI buffer copy batch: {}", Error);
		RecordCommand<FCopyBufferCommand>(Source, Destination, Regions);
	}

	auto FRHICommandListBase::CopyBufferToTexture(FRHIBuffer* Source, FRHITexture* Destination,
		std::span<const FRHIBufferTextureCopyRegion> Regions) -> void
	{
		if (Regions.empty()) return;
		checkf(!bInsideRenderPass, "Buffer-to-texture copies cannot be recorded inside a render pass.");
		std::string Error;
		checkf(ValidateBufferToTextureCopies(Source, Destination, Regions, Error),
			"Invalid RHI buffer-to-texture copy batch: {}", Error);
		RecordCommand<FCopyBufferToTextureCommand>(Source, Destination, Regions);
	}

	auto FRHICommandListBase::CopyTextureToBuffer(FRHITexture* Source, FRHIBuffer* Destination,
		std::span<const FRHIBufferTextureCopyRegion> Regions) -> void
	{
		if (Regions.empty()) return;
		checkf(!bInsideRenderPass, "Texture-to-buffer copies cannot be recorded inside a render pass.");
		std::string Error;
		checkf(ValidateTextureToBufferCopies(Source, Destination, Regions, Error),
			"Invalid RHI texture-to-buffer copy batch: {}", Error);
		RecordCommand<FCopyTextureToBufferCommand>(Source, Destination, Regions);
	}

	auto FRHICommandListBase::CopyTexture(FRHITexture* Source, FRHITexture* Destination,
		std::span<const FRHITextureCopyRegion> Regions) -> void
	{
		if (Regions.empty()) return;
		checkf(!bInsideRenderPass, "Texture copies cannot be recorded inside a render pass.");
		std::string Error;
		checkf(ValidateTextureCopies(Source, Destination, Regions, Error),
			"Invalid RHI texture copy batch: {}", Error);
		RecordCommand<FCopyTextureCommand>(Source, Destination, Regions);
	}

	auto FRHICommandListBase::Draw(const FRHIDrawArguments& Arguments) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"Draw requires an active graphics pipeline while recording.");
		if (Arguments.VertexCount == 0 || Arguments.InstanceCount == 0) return;
		RecordCommand<FDrawCommand>(Arguments);
	}

	auto FRHICommandListBase::DrawIndexed(
		const FRHIDrawIndexedArguments& Arguments) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"DrawIndexed requires an active graphics pipeline while recording.");
		if (Arguments.IndexCount == 0 || Arguments.InstanceCount == 0) return;
		RecordCommand<FDrawIndexedCommand>(Arguments);
	}

	auto FRHICommandListBase::DrawIndexed(uint32 IndexCount,
		uint32 StartIndexLocation, int32 VertexOffset) -> void
	{
		DrawIndexed({.IndexCount = IndexCount, .FirstIndex = StartIndexLocation,
			.VertexOffset = VertexOffset});
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

	auto FRHICommandListBase::WriteBuffer(
		FRHIBuffer* Buffer,
		const void* Data,
		uint32 Size,
		uint32 OffsetBytes) -> void
	{
		RecordCommand<FWriteBufferCommand>(Buffer, OffsetBytes, Data, Size);
	}

	auto FRHICommandListBase::UpdateUniformBuffer(
		FRHIBuffer* UniformBuffer,
		const void* Data,
		uint32 Size,
		uint32 Offset) -> void
	{
		check(Size % 16 == 0 && Offset % 16 == 0);
		WriteBuffer(UniformBuffer, Data, Size, Offset);
	}

	auto FRHICommandListBase::InitializeTexture(FRHITexture* Texture) -> void
	{
		RecordCommand<FInitializeTextureCommand>(Texture);
	}

	auto FRHICommandListBase::UpdateTexture2D(
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		const FUpdateTextureRegion2D& UpdateRegion,
		uint32 SourcePitch,
		const uint8* SourceData) -> void
	{
		RecordCommand<FUpdateTexture2DCommand>(
			Texture, MipIndex, ArraySlice, UpdateRegion, SourcePitch, SourceData);
	}

	auto FRHICommandListBase::PushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"PushConstants requires an active graphics pipeline while recording.");
		RecordCommand<FPushConstantsCommand>(StageFlags, Offset, Size, Data);
	}

	auto FRHICommandListBase::SetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void
	{
		checkf(ActivePipeline == ERHIPipeline::Graphics,
			"SetShaderParameters requires an active graphics pipeline while recording.");
		std::vector<FRHIShaderParameterResource> CanonicalParameters;
		std::vector<TRefCountPtr<FRHIResource>> CreatedViews;
		CanonicalizeShaderParameters(InResourceParameters, CanonicalParameters, CreatedViews);
		RecordCommand<FSetShaderParametersCommand>(InShader, CanonicalParameters);
	}

	FRHICommandListImmediate::FRHICommandListImmediate(
		FRHICommandListExecutor& InExecutor)
		: FRHICommandList(true)
		, Executor(&InExecutor)
		, LockState(std::make_unique<FLockState>())
	{
	}

	FRHICommandListImmediate::~FRHICommandListImmediate() = default;

	auto FRHICommandListImmediate::Get() -> FRHICommandListImmediate&
	{
		return GCommandListExecutor.GetImmediateCommandList();
	}

	auto FRHICommandListImmediate::QueueCommandList(
		FRHICommandList&& CommandList) -> void
	{
		if (!TryQueueCommandList(std::move(CommandList)))
		{
			checkf(false,
				"QueueCommandList requires a finished command list that has not been admitted.");
		}
	}

	auto FRHICommandListImmediate::TryQueueCommandList(
		FRHICommandList&& CommandList) -> bool
	{
		return Executor->TryQueueCommandList(CommandList);
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
		checkf(LockMode == EResourceLockMode::WriteOnly,
			"Recorded buffer locks currently support WriteOnly mode.");
		check(Size != 0 && Offset <= Buffer->GetSize()
			&& Size <= Buffer->GetSize() - Offset);
		checkf(!LockState->PendingLocks.contains(Buffer),
			"A buffer may only have one open recorded lock.");
		auto Pending = std::make_unique<FLockState::FPendingLock>();
		Pending->Buffer = Buffer;
		Pending->Offset = Offset;
		Pending->Data.resize(Size);
		void* Result = Pending->Data.data();
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
		WriteBuffer(
			Pending->Buffer.GetReference(), Pending->Data.data(),
			static_cast<uint32>(Pending->Data.size()), Pending->Offset);
	}

	auto FRHICommandListImmediate::AllocateDynamicUniformBuffer(const void* Data, uint32 Size) -> FRHIUniformBufferRange
	{
		check(Data && Size != 0);
		if (GDynamicRHI)
		{
			return GDynamicRHI->RHIAllocateDynamicUniformBuffer(
				*this, Data, Size);
		}
		return AllocateDynamicUniformBufferSynchronous(Data, Size);
	}

	auto FRHICommandListImmediate::AllocateDynamicUniformBufferSynchronous(
		const void* Data,
		uint32 Size) -> FRHIUniformBufferRange
	{
		check(Data && Size != 0);
		FRHIUniformBufferRange Result;
		Executor->ExecuteSynchronousContextOperation(false,
			[Data, Size, &Result](IRHICommandContext& Context) {
				Result = Context.RHIAllocateDynamicUniformBuffer(Data, Size);
			});
		return Result;
	}

	auto FRHICommandListImmediate::AllocateDynamicStorageBuffer(
		const void* Data, uint32 Size) -> FRHIStorageBufferRange
	{
		check(Data && Size != 0);
		const FRHICapabilities* Capabilities = GDynamicRHI
			? GDynamicRHI->RHIGetCapabilities() : nullptr;
		if (Capabilities && (Size > Capabilities->MaxStorageBufferRange
			|| Capabilities->MinStorageBufferOffsetAlignment == 0)) return {};
		if (GDynamicRHI)
			return GDynamicRHI->RHIAllocateDynamicStorageBuffer(*this, Data, Size);
		return AllocateDynamicStorageBufferSynchronous(Data, Size);
	}

	auto FRHICommandListImmediate::AllocateDynamicStorageBufferSynchronous(
		const void* Data, uint32 Size) -> FRHIStorageBufferRange
	{
		check(Data && Size != 0);
		FRHIStorageBufferRange Result;
		Executor->ExecuteSynchronousContextOperation(false,
			[Data, Size, &Result](IRHICommandContext& Context) {
				Result = Context.RHIAllocateDynamicStorageBuffer(Data, Size);
			});
		return Result;
	}

	auto FRHICommandListImmediate::ReadTexture2D(
		FRHITexture* Texture,
		uint32 MipIndex,
		uint32 ArraySlice,
		std::vector<uint8>& OutData) -> bool
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

	auto FRHICommandListImmediate::AcquireBackBuffer(
		FRHITexture* BackBuffer) -> void
	{
		RecordAcquireBackBuffer(BackBuffer);
	}

	auto FRHICommandListImmediate::AcquireBackBufferSynchronously(
		FRHITexture* BackBuffer) -> void
	{
		check(BackBuffer);
		Executor->ExecuteSynchronousContextOperation(true,
			[BackBuffer](IRHICommandContext& Context) {
				Context.RHIAcquireBackBuffer(BackBuffer);
			});
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
		if (Executor)
		{
			Executor->WaitForSerial(TargetSerial);
		}
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

	auto FRHICommandListExecutor::TryQueueCommandList(
		FRHICommandList& CommandList) -> bool
	{
		checkf(!CommandListImmediate.bInsideRenderPass,
			"QueueCommandList cannot split an active immediate render pass.");
		checkf(CommandListImmediate.DiagnosticRegionDepth == 0,
			"QueueCommandList cannot split active immediate diagnostic regions.");
		checkf(CommandListImmediate.ActiveGPUTimingQuery == nullptr,
			"QueueCommandList cannot split an active immediate GPU timing query.");
		if (!CommandList.IsFinished())
		{
			State->RejectedSubmissionCount.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		if (CommandList.GetNumRecordedCommands() == 0)
		{
			CommandList.MarkAdmitted();
			return true;
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
		return true;
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

	auto FRHICommandListExecutor::TrySubmit(
		const std::vector<FRHICommandList*>& AdditionalCmdLists,
		ERHISubmitFlags SubmitFlags)
		-> FRHICommandListSubmission
	{
		if (CommandListImmediate.DiagnosticRegionDepth != 0)
			FRHICommandListBase::RecordInvalidDiagnosticRegion();
		if (CommandListImmediate.HasOpenBufferLocks()
			|| CommandListImmediate.bInsideRenderPass
			|| CommandListImmediate.DiagnosticRegionDepth != 0
			|| CommandListImmediate.ActiveGPUTimingQuery != nullptr)
		{
			State->RejectedSubmissionCount.fetch_add(1, std::memory_order_relaxed);
			return {.Result = ERHICommandListSubmitResult::InvalidCommandList};
		}
		std::unordered_set<FRHICommandList*> UniqueLists;
		for (FRHICommandList* CommandList : AdditionalCmdLists)
		{
			if (!CommandList || !CommandList->IsFinished())
			{
				State->RejectedSubmissionCount.fetch_add(1, std::memory_order_relaxed);
				return {.Result = ERHICommandListSubmitResult::InvalidCommandList};
			}
			if (!UniqueLists.emplace(CommandList).second)
			{
				State->RejectedSubmissionCount.fetch_add(1, std::memory_order_relaxed);
				return {.Result = ERHICommandListSubmitResult::InvalidCommandList};
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
			return {
				.Result = ERHICommandListSubmitResult::Accepted,
				.Serial = Serial
			};
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
			if (State->RHIThread)
			{
				CheckRHIThread();
			}
			const auto ReplayStart = std::chrono::steady_clock::now();
			if (EnumHasAnyFlags(Group.GetFlags(), ERHISubmitFlags::BeginFrame))
			{
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
			Group.Replay(State->ReplayContext);
			const auto ReplayEnd = std::chrono::steady_clock::now();
			State->ReplayDurationNanoseconds.fetch_add(
				static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					ReplayEnd - ReplayStart).count()),
				std::memory_order_relaxed);

			if (EnumHasAnyFlags(Group.GetFlags(), ERHISubmitFlags::SubmitToGPU))
			{
				State->ReplayContext.GetOperationContext("SubmitToGPU")
					.RHISubmitCommands();
			}
			if (EnumHasAnyFlags(Group.GetFlags(), ERHISubmitFlags::EndFrame))
			{
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
			Group.ReleaseBatches();
			if (EnumHasAnyFlags(Group.GetFlags(), ERHISubmitFlags::DeleteResources))
			{
				DeleteDeferredResources();
			}
		};

		if (State->RHIThread)
		{
			static_assert(sizeof(size_t) <= sizeof(uint64));
			if (GroupBatchCount > std::numeric_limits<uint32>::max())
			{
				State->PendingBatches = SubmissionGroup->TakeBatches();
				State->RejectedSubmissionCount.fetch_add(
					1, std::memory_order_relaxed);
				return {.Result = ERHICommandListSubmitResult::Oversized};
			}
			FRHIThreadWork Work;
			Work.BatchCount = static_cast<uint32>(GroupBatchCount);
			Work.PayloadBytes = static_cast<uint64>(GroupPayloadBytes);
			Work.Execute = [Replay, SubmissionGroup]() mutable {
				Replay(*SubmissionGroup);
				SubmissionGroup.reset();
				return FRHIThreadWorkResult::Success();
			};
			const FRHIThreadSubmission Submission = State->RHIThread->Enqueue(Work);
			if (!Submission.IsAccepted())
			{
				// Enqueue rejection preserves Work, so release its shared capture before
				// returning every batch to the executor for a later retry.
				Work.Execute = {};
				State->PendingBatches = SubmissionGroup->TakeBatches();
				State->RejectedSubmissionCount.fetch_add(1, std::memory_order_relaxed);
				ERHICommandListSubmitResult Result =
					ERHICommandListSubmitResult::InvalidCommandList;
				switch (Submission.Result)
				{
				case ERHIThreadEnqueueResult::Stopped:
					Result = ERHICommandListSubmitResult::ThreadStopped;
					break;
				case ERHIThreadEnqueueResult::Draining:
					Result = ERHICommandListSubmitResult::ThreadDraining;
					break;
				case ERHIThreadEnqueueResult::Failed:
					Result = ERHICommandListSubmitResult::ThreadFailed;
					break;
				case ERHIThreadEnqueueResult::Oversized:
					Result = ERHICommandListSubmitResult::Oversized;
					break;
				case ERHIThreadEnqueueResult::SerialExhausted:
					Result = ERHICommandListSubmitResult::SerialExhausted;
					break;
				case ERHIThreadEnqueueResult::SelfEnqueue:
					Result = ERHICommandListSubmitResult::SelfEnqueue;
					break;
				case ERHIThreadEnqueueResult::InvalidWork:
				case ERHIThreadEnqueueResult::Accepted:
					break;
				}
				return {.Result = Result};
			}
			RecordAcceptedSubmission();
			if (EnumHasAnyFlags(SubmitFlags, ERHISubmitFlags::FlushRHIThread))
			{
				WaitForSerial(Submission.Serial);
			}
			return {
				.Result = ERHICommandListSubmitResult::Accepted,
				.Serial = Submission.Serial
			};
		}

		const uint64 PreviousSerial =
			State->LastSubmittedSerial.load(std::memory_order_relaxed);
		if (PreviousSerial == std::numeric_limits<uint64>::max())
		{
			State->PendingBatches = SubmissionGroup->TakeBatches();
			State->RejectedSubmissionCount.fetch_add(1, std::memory_order_relaxed);
			return {.Result = ERHICommandListSubmitResult::SerialExhausted};
		}
		const uint64 Serial = PreviousSerial + 1;
		RecordAcceptedSubmission();
		State->LastSubmittedSerial.store(Serial, std::memory_order_release);
		Replay(*SubmissionGroup);
		SubmissionGroup.reset();
		State->CompletedSerial.store(Serial, std::memory_order_release);
		State->CompletionCV.notify_all();
		if (EnumHasAnyFlags(SubmitFlags, ERHISubmitFlags::FlushRHIThread))
		{
			WaitForSerial(Serial);
		}
		return {
			.Result = ERHICommandListSubmitResult::Accepted,
			.Serial = Serial
		};
	}

	auto FRHICommandListExecutor::Submit(
		const std::vector<FRHICommandList*>& AdditionalCmdLists,
		ERHISubmitFlags SubmitFlags) -> uint64
	{
		const FRHICommandListSubmission Submission =
			TrySubmit(AdditionalCmdLists, SubmitFlags);
		if (!Submission.IsAccepted())
		{
			const FRHIThreadStats ThreadStats = State->RHIThread
				? State->RHIThread->GetStats()
				: FRHIThreadStats{};
			DURIN_FATAL(
				"RHI command-list submission rejected (result {}, state {}, failure '{}').",
				static_cast<uint32>(Submission.Result),
				static_cast<uint32>(ThreadStats.AdmissionState),
				ThreadStats.FailureDiagnostic);
			std::terminate();
		}
		return Submission.Serial;
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

	auto FRHICommandListExecutor::ExecuteFallibleSynchronousOperation(
		bool bFlushRecordedCommands,
		std::function<void()> Operation,
		size_t OwnedPayloadBytes) -> FRHIFallibleOperationResult
	{
		checkf(!CommandListImmediate.HasOpenBufferLocks(),
			"A synchronous RHI operation requires every buffer lock to be unlocked.");
		check(Operation);
		State->SynchronousOperationCount.fetch_add(1, std::memory_order_relaxed);
		if (bFlushRecordedCommands)
		{
			Submit({}, ERHISubmitFlags::None);
		}

		auto Result = std::make_shared<FRHIFallibleOperationResult>();
		auto ExecuteOperation =
			[Operation = std::move(Operation), Result]() mutable {
				try
				{
					Operation();
				}
				catch (const std::exception& Exception)
				{
					Result->bSucceeded = false;
					Result->Diagnostic = Exception.what();
				}
				catch (...)
				{
					Result->bSucceeded = false;
					Result->Diagnostic =
						"Fallible RHI operation failed with an unknown exception.";
				}
			};

		if (!State->RHIThread)
		{
			State->ReplayContext.GetOperationContext(
				"Fallible synchronous RHI operation");
			ExecuteOperation();
			return std::move(*Result);
		}

		FRHIThreadWork Work;
		Work.PayloadBytes = static_cast<uint64>(OwnedPayloadBytes);
		Work.Execute = [this, ExecuteOperation = std::move(ExecuteOperation)]() mutable {
			CheckRHIThread();
			State->ReplayContext.GetOperationContext(
				"Fallible synchronous RHI operation");
			ExecuteOperation();
			return FRHIThreadWorkResult::Success();
		};
		const FRHIThreadSubmission Submission = State->RHIThread->Enqueue(Work);
		if (!Submission.IsAccepted())
		{
			DURIN_FATAL(
				"RHI thread rejected fallible synchronous operation ({}).",
				static_cast<uint32>(Submission.Result));
			std::terminate();
		}
		WaitForSerial(Submission.Serial);
		return std::move(*Result);
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
			return FRHIThreadWorkResult::Success();
		};
		const FRHIThreadSubmission Submission = State->RHIThread->Enqueue(Work);
		if (!Submission.IsAccepted())
		{
			DURIN_FATAL(
				"RHI thread rejected synchronous operation ({}).",
				static_cast<uint32>(Submission.Result));
			std::terminate();
		}
		WaitForSerial(Submission.Serial);
	}

	auto FRHICommandListExecutor::CreateFence() -> FRHICommandListFence
	{
		return CreateFence(GetLastSubmittedSerial());
	}

	auto FRHICommandListExecutor::CreateFence(uint64 TargetSerial)
		-> FRHICommandListFence
	{
		check(TargetSerial <= GetLastSubmittedSerial());
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
			.RejectedSubmissionCount = State->RejectedSubmissionCount.load(std::memory_order_relaxed),
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
			if (ThreadStats.FailedSerial == 0)
			{
				WaitForSerial(LastSubmittedSerial);
			}
			State->LastSubmittedSerial.store(
				LastSubmittedSerial, std::memory_order_release);
			State->CompletedSerial.store(
				LastSubmittedSerial, std::memory_order_release);
			State->RHIThread = nullptr;
		}
	}

	auto FRHICommandListExecutor::IsSerialComplete(uint64 Serial) const -> bool
	{
		return GetCompletedSerial() >= Serial;
	}

	auto FRHICommandListExecutor::IsSerialFailed(uint64 Serial) const -> bool
	{
		if (!State->RHIThread || IsSerialComplete(Serial))
		{
			return false;
		}
		const FRHIThreadStats ThreadStats = State->RHIThread->GetStats();
		return ThreadStats.FailedSerial != 0
			|| ThreadStats.AdmissionState == ERHIThreadAdmissionState::Stopped;
	}

	auto FRHICommandListExecutor::TryWaitForSerial(uint64 Serial) const -> bool
	{
		State->WaitCount.fetch_add(1, std::memory_order_relaxed);
		const auto WaitStart = std::chrono::steady_clock::now();
		if (IsSerialComplete(Serial))
		{
			return true;
		}

		bool bCompleted = true;
		if (State->RHIThread)
		{
			bCompleted = State->RHIThread->WaitForSerial(Serial)
				== ERHIThreadWaitResult::Completed;
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
		return bCompleted;
	}

	auto FRHICommandListExecutor::WaitForSerial(uint64 Serial) const -> void
	{
		if (!TryWaitForSerial(Serial))
		{
			DURIN_FATAL(
				"RHI thread failed while waiting for serial {}.", Serial);
			std::terminate();
		}
	}
} // namespace Durin
