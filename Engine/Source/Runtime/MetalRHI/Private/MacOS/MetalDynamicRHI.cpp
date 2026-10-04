#include "DynamicRHI.h"
#include "MacOS/MacOSPresentationTarget.h"
#include "Backend/RHICompletionBackend.h"
#include "MetalAutoreleasePool.h"
#include "MetalBuffer.h"
#include "MetalCommandContext.h"
#include "MetalCppDevice.h"
#include "MetalPipeline.h"
#include "MetalSubmission.h"
#include "MetalSampler.h"
#include "MetalTexture.h"
#include "MetalViewport.h"
#include "MetalResourceDescriptors.h"
#include "MetalResourceState.h"
#include <mach/mach_time.h>
#include "RHICommandList.h"
#include "PipelineStateCache.h"
#include "Threading/Task.h"

namespace Durin
{
	namespace
	{
		class FMetalDynamicRHI final : public FDynamicRHI
		{
		public:
			~FMetalDynamicRHI() override
			{
				// Also cover partially initialized instances after factory exceptions.
				const FMetalAutoreleasePool Pool;
				PipelineCache.reset();
				CommandContext.reset();
				State.reset();
				StartupLayer.reset();
				Queue.reset();
				Device.reset();
			}
			auto GetSubmissionQueue() const -> MTL::CommandQueue* { return Queue.get(); }
			auto Init(const FRHIInitializationContext& Context) -> void override
			{
				const FMetalAutoreleasePool Pool;
				if (const auto& Target = Context.GetPresentationTarget())
				{
					const auto* NativeTarget = dynamic_cast<const FMacOSPresentationTarget*>(Target->PlatformTarget.get());
					StartupLayer = NativeTarget ? NS::RetainPtr(NativeTarget->GetMetalLayer()) : nullptr;
					if (!StartupLayer)
						throw std::runtime_error("Metal presentation requires a CAMetalLayer.");
					StartupWindow = Target->GetNativeWindowHandle();
				}
				auto Native = CreateMetalCppDeviceAndQueue();
				Device = std::move(Native.Device);
				Queue = std::move(Native.Queue);
				State = std::make_shared<FMetalSubmissionState>();
				State->Queue = Queue;
				State->Generation = AllocateRHIDeviceGeneration();
				State->Timeline = std::make_unique<FRHIGPUQueueTimeline>(
					State->Generation, FRHIQueueId{0});
				QueueCapabilities.DeviceGeneration = State->Generation;
				QueueCapabilities.Queues = {{.Id = {0}, .OwnershipDomain = 0,
					.bGraphics = true, .bCompute = true, .bCopy = true}};
				QueueCapabilities.Graphics = {0};
				QueueCapabilities.Compute = {0};
				CommandContext->Configure(State);
				FRHICapabilities Supported;
				Supported.SupportedTextureDimensions =
					ERHITextureDimensionFlags::Texture2D | ERHITextureDimensionFlags::TextureCube;
				Supported.MaxTextureDimension2D = 8192;
				Supported.MaxTextureDimensionCube = 16384;
				Supported.MaxTextureArrayLayers = TextureCubeFaceCount;
				Supported.ColorSampleCounts = ERHISampleCountFlags::Samples1;
				Supported.DepthSampleCounts = ERHISampleCountFlags::Samples1;
				Supported.MaxColorAttachments = 4;
				Supported.MinStorageBufferOffsetAlignment = 16;
				Supported.MaxStorageBufferRange = 1u << 27;
				Supported.MaxFragmentSampledImages = 16;
				Supported.MaxFragmentSamplers = 16;
				Supported.MaxFragmentUniformBuffers = 16;
				Supported.MaxFragmentResources = 31;
				Supported.MaxUniformBufferRange = 65536;
				Supported.MinUniformBufferOffsetAlignment = 16;
				Supported.MaxComputeWorkGroupCount = {65535, 65535, 65535};
				Supported.bSupportsIndirectDraw = true;
				Supported.bSupportsIndirectDispatch = true;
				Supported.bSupportsSkyLighting = Device->supports32BitFloatFiltering();
				if (Device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary))
				{
					auto* Sets = Device->counterSets();
					for (NS::UInteger Index = 0; Sets && Index < Sets->count(); ++Index)
					{
						auto* Set = Sets->object<MTL::CounterSet>(Index);
						if (!Set->name()->isEqualToString(MTL::CommonCounterSetTimestamp)) continue;
						auto Desc = NS::TransferPtr(MTL::CounterSampleBufferDescriptor::alloc()->init());
						Desc->setCounterSet(Set);
						Desc->setSampleCount(FMetalGPUTimingPool::Capacity * 2);
						Desc->setStorageMode(MTL::StorageModeShared);
						NS::Error* Error = nullptr;
						auto Buffer = NS::TransferPtr(Device->newCounterSampleBuffer(Desc.get(), &Error));
						auto Marker = NS::TransferPtr(Device->newBuffer(4, MTL::ResourceStorageModeShared));
						mach_timebase_info_data_t Timebase{};
						if (!Buffer || !Marker || mach_timebase_info(&Timebase) != KERN_SUCCESS || !Timebase.denom) break;
						State->TimingPool = std::make_shared<FMetalGPUTimingPool>();
						State->TimingPool->Buffer = std::move(Buffer);
						State->TimingPool->Marker = std::move(Marker);
						State->TimingPool->Generation = State->Generation;
						// The admitted Apple GPUs use the mach_absolute_time clock for counters.
						State->TimingPool->NanosecondsPerTick = double(Timebase.numer) / Timebase.denom;
						Supported.bSupportsGPUTimestamps = true;
						Supported.GPUTimestampNanosecondsPerTick = State->TimingPool->NanosecondsPerTick;
						break;
					}
				}
				PublishCapabilities(std::move(Supported));
				PipelineCreationClosed = false;
			}
			auto RHICreateGPUTimingQuery() -> FGPUTimingQueryRHIRef override
			{
				if (!State || !State->TimingPool) return nullptr;
				auto Pool = State->TimingPool;
				std::lock_guard Lock(Pool->Mutex);
				for (uint32 Slot = 0; Slot < FMetalGPUTimingPool::Capacity; ++Slot)
					if (!Pool->Used[Slot])
					{
						Pool->Used[Slot] = true;
						return new FMetalGPUTimingQuery(Pool, Slot);
					}
				return nullptr;
			}
			auto RHIGetPipelineStateCache() -> FRHIPipelineStateCache* override
			{
				std::lock_guard Lock(PipelineCreationMutex);
				if (PipelineCreationClosed || !Device || !IsTaskSchedulerRunning()) return nullptr;
				if (!PipelineCache)
				{
					FRHIPipelineCompileBackend Backend;
					Backend.FindGraphics = [](const FGraphicsPipelineStateKey&) -> FGraphicsPipelineStateRHIRef { return nullptr; };
					Backend.FindCompute = [](const FComputePipelineStateKey&) -> FComputePipelineStateRHIRef { return nullptr; };
					Backend.CreateGraphics = [this](const FRHIGraphicsPipelineCreationInputs& Inputs,
						const FGraphicsPipelineStateKey&) -> FGraphicsPipelineStateRHIRef {
						const FMetalAutoreleasePool Pool;
						return RHICreateGraphicsPipelineState(FName(Inputs.DebugName), Inputs.Initializer);
					};
					Backend.CreateCompute = [this](const FRHIComputePipelineCreationInputs& Inputs,
						const FComputePipelineStateKey&) -> FComputePipelineStateRHIRef {
						const FMetalAutoreleasePool Pool;
						return RHICreateComputePipelineState(FName(Inputs.DebugName), Inputs.Initializer);
					};
					Backend.PublishTerminalFailure = [](std::exception_ptr Failure) {
						GCommandListExecutor.ReportExternalFailure(Failure);
					};
					PipelineCache = std::make_unique<FRHIPipelineStateCache>(
						*RHIGetCapabilities(), std::move(Backend));
				}
				return PipelineCache.get();
			}
			auto RHIStopPipelineCreation() -> void override
			{
				FRHIPipelineStateCache* Cache;
				{
					std::lock_guard Lock(PipelineCreationMutex);
					PipelineCreationClosed = true;
					Cache = PipelineCache.get();
				}
				if (Cache) Cache->StopAndWait();
			}
			auto RHIRetirePipelineCreationResults() -> void override
			{
				FRHIPipelineStateCache* Cache;
				{
					std::lock_guard Lock(PipelineCreationMutex);
					Cache = PipelineCache.get();
				}
				if (Cache) Cache->ReleaseResources();
			}
			auto RHIIsPipelineCreationClosed() const -> bool override
			{
				std::lock_guard Lock(PipelineCreationMutex);
				return PipelineCreationClosed || (PipelineCache && PipelineCache->IsClosed());
			}
			auto RHIGetPipelineCreationStatistics() const -> FRHIPipelineCreationStatistics override
			{
				std::lock_guard Lock(PipelineCreationMutex);
				return PipelineCache ? PipelineCache->GetStatistics() : FRHIPipelineCreationStatistics{};
			}

			auto Shutdown() -> void override
			{
				const FMetalAutoreleasePool Pool;
				RHIStopPipelineCreation();
				RHIRetirePipelineCreationResults();
				PipelineCache.reset();
				CommandContext->CancelPending();
				if (State)
				{
					std::unique_lock Lock(State->Mutex);
					State->Completion.wait(Lock,
						[this] { return State->PendingCallbacks == 0; });
					State->Timeline->Fail();
				}
				// Callback/context releases may enqueue RHI deletes after the outer shutdown marker.
				RHIFlushDeferredResources();
				CommandContext->Configure({});
				State.reset();
				ClearCapabilities();
				QueueCapabilities = {};
				Queue.reset();
				Device.reset();
				StartupLayer.reset();
				StartupWindow = nullptr;
			}
			auto RHIGetQueueCapabilities() const -> const FRHIQueueCapabilities& override
			{ return QueueCapabilities; }
			auto RHIGetCompletionStatus(const FRHIGPUSyncPointRef& Signal) const
			-> ERHIGPUSubmissionState override
			{
				if (!State) return ERHIGPUSubmissionState::Invalid;
				const auto Point = FRHIGPUSyncPointBackend::GetPoint(Signal);
				if (Point.DeviceGeneration != State->Generation || Point.Queue != FRHIQueueId{0}
					|| !State->Timeline->Owns(Signal)) return ERHIGPUSubmissionState::Invalid;
				return Signal.GetState();
			}
			auto RHIWaitForCompletion(const FRHIGPUSyncPointRef& Signal,
			uint64 TimeoutNanoseconds) -> ERHIGPUWaitResult override
			{
				if (Signal.GetState() == ERHIGPUSubmissionState::Pending)
					return ERHIGPUWaitResult::Pending;
				if (RHIGetCompletionStatus(Signal) == ERHIGPUSubmissionState::Invalid)
					return ERHIGPUWaitResult::Invalid;
				std::unique_lock Lock(State->Mutex);
				if (Signal.GetState() == ERHIGPUSubmissionState::Submitted)
				{
					const auto Done = [&] {
						return Signal.GetState() != ERHIGPUSubmissionState::Submitted;
					};
					if (TimeoutNanoseconds == UINT64_MAX)
						State->Completion.wait(Lock, Done);
					else
						State->Completion.wait_for(Lock, std::chrono::nanoseconds(
							std::min<uint64>(TimeoutNanoseconds, INT64_MAX)), Done);
				}
				switch (Signal.GetState())
				{
				case ERHIGPUSubmissionState::Complete: return ERHIGPUWaitResult::Complete;
				case ERHIGPUSubmissionState::Canceled: return ERHIGPUWaitResult::Canceled;
				case ERHIGPUSubmissionState::Failed: return ERHIGPUWaitResult::Failed;
				case ERHIGPUSubmissionState::DeviceLost: return ERHIGPUWaitResult::DeviceLost;
				case ERHIGPUSubmissionState::Submitted: return ERHIGPUWaitResult::Timeout;
				case ERHIGPUSubmissionState::Pending: return ERHIGPUWaitResult::Pending;
				case ERHIGPUSubmissionState::Invalid: return ERHIGPUWaitResult::Invalid;
				}
				return ERHIGPUWaitResult::Invalid;
			}
			auto RHIBeginFrame(const FRHIBeginFrameArgs& Args) -> void override
			{ CommandContext->RHIBeginFrame(Args); }
			auto RHIEndFrame() -> void override
			{ CommandContext->RHIEndFrame(); }
			auto RHICreateViewport(const FRHIViewportCreateInfo& Info)
			-> TRefCountPtr<FRHIViewport> override
			{
				const FMetalAutoreleasePool Pool;
				if (!Device || !Info.PresentationTarget.IsValid() || !Info.SizeX || !Info.SizeY) return nullptr;
				const auto* NativeTarget = dynamic_cast<const FMacOSPresentationTarget*>(Info.PresentationTarget.PlatformTarget.get());
				if (!NativeTarget) return nullptr;
				auto Layer = NS::RetainPtr(NativeTarget->GetMetalLayer());
				if (!Layer) return nullptr;
				if (Info.bAdoptInitializationPresentationCandidate)
				{
					if (!StartupLayer || Layer.get() != StartupLayer.get()
						|| Info.PresentationTarget.GetNativeWindowHandle() != StartupWindow) return nullptr;
				}
				const auto Format = Info.PreferredPixelFormat
					== EPixelFormat::RGBA16_FLOAT
					? EPixelFormat::RGBA16_FLOAT
					: Info.PreferredPixelFormat == EPixelFormat::RGBA8_UNORM
						|| Info.PreferredPixelFormat == EPixelFormat::BGRA8_UNORM
						? EPixelFormat::BGRA8_UNORM
						: EPixelFormat::SBGRA8_UNORM;
				auto Viewport = MakeRefCount<FMetalViewport>(
					Device, std::move(Layer), Info.SizeX, Info.SizeY, Format, Info.PresentationPolicy);
				if (!Viewport->SnapshotBackBuffer()) return nullptr;
				if (Info.bAdoptInitializationPresentationCandidate)
				{
					StartupLayer.reset();
					StartupWindow = nullptr;
				}
				return Viewport;
			}
			auto RHIResizeViewport(FRHIViewport* Viewport,
				uint32 Width, uint32 Height, bool) -> void override
			{
				auto* MetalViewport = dynamic_cast<FMetalViewport*>(Viewport);
				requiref(MetalViewport, "Metal viewport resize requires a Metal viewport.");
				if (Width && Height)
					requiref(MetalViewport->Resize(Width, Height),
						"Metal viewport back buffer recreation failed.");
			}
			auto RHICreateGraphicsPipelineState(FName,
			const FGraphicsPipelineStateInitializer& Initializer)
			-> TRefCountPtr<FRHIGraphicsPipelineState> override
			{ return CreateMetalGraphicsPipeline(Device.get(), Initializer); }
			auto RHICreateComputePipelineState(FName,
			const FComputePipelineStateInitializer& Initializer)
			-> TRefCountPtr<FRHIComputePipelineState> override
			{ return CreateMetalComputePipeline(Device.get(), Initializer); }
			auto RHIGetDefaultContext() -> IRHICommandContext* override
			{ return CommandContext.get(); }
			auto RHIGetViewportBackBuffer(FRHIViewport* Viewport)
			-> TRefCountPtr<FRHITexture> override
			{
				auto* MetalViewport = dynamic_cast<FMetalViewport*>(Viewport);
				return MetalViewport ? MetalViewport->SnapshotBackBuffer() : nullptr;
			}
			auto RHICreateVertexDeclaration(const FVertexDeclarationElementList& Elements)
			-> TRefCountPtr<FRHIVertexDeclaration> override
			{
				bool bTail = false;
				for (const auto& Element : Elements)
				{
					if (Element.Type == EVertexElementType::None)
					{
						bTail = true;
						continue;
					}
					if (bTail || Element.StreamIndex >= 16
						|| Element.AttributeIndex >= 31
						|| ToMetalVertexFormat(Element.Type)
							== MTL::VertexFormatInvalid)
						return nullptr;
				}
				return new FMetalVertexDeclaration(Elements);
			}
			auto RHIIsTextureSupported(const FRHITextureCreateDesc& Desc) const -> bool override
			{
				const FMetalAutoreleasePool Pool;
				constexpr ETextureCreateFlags SupportedFlags = ETextureCreateFlags::SourceCopy
					| ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback
					| ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::DepthStencilTargetable
					| ETextureCreateFlags::ShaderResource | ETextureCreateFlags::Storage;
				const bool b2D = Desc.Dimension == ETextureDimension::Texture2D;
				const bool b2DArray = Desc.Dimension == ETextureDimension::Texture2DArray;
				const bool bCube = Desc.Dimension == ETextureDimension::TextureCube;
				const bool bCubeArray = Desc.Dimension == ETextureDimension::TextureCubeArray;
				const bool b3D = Desc.Dimension == ETextureDimension::Texture3D;
				const bool bBC = Desc.Format == EPixelFormat::BC1_UNORM
					|| Desc.Format == EPixelFormat::BC1_UNORM_SRGB
					|| Desc.Format == EPixelFormat::BC5_UNORM
					|| Desc.Format == EPixelFormat::BC5_SNORM
					|| Desc.Format == EPixelFormat::BC7_UNORM
					|| Desc.Format == EPixelFormat::BC7_UNORM_SRGB;
				return Device && ValidateTextureCreateDesc(Desc)
					&& (b2D || b2DArray || bCube || bCubeArray || b3D)
					&& ToMetalPixelFormat(Desc.Format) != MTL::PixelFormatInvalid
					&& (!bBC || (Device->supportsBCTextureCompression()
						&& (static_cast<uint64>(Desc.Flags)
							& ~static_cast<uint64>(ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::CPUReadback)) == 0))
					&& (b2D || Desc.Format == EPixelFormat::RGBA8_UNORM
						|| (b3D && Desc.Format == EPixelFormat::R8_UNORM)
						|| (b2DArray && Desc.Format == EPixelFormat::D32)
						|| (bCube && (Desc.Format == EPixelFormat::RGBA16_FLOAT
						|| Desc.Format == EPixelFormat::RGBA32_FLOAT
						|| bBC)))
					&& Desc.NumSamples == 1
					&& Desc.Extent.x <= (b3D ? 2048 : 16384)
					&& Desc.Extent.y <= (b3D ? 2048 : 16384)
					&& (!b3D || Desc.Depth <= 2048)
					&& (!b2DArray || Desc.ArraySize <= 2048)
					&& (!bCubeArray || Desc.ArraySize <= 2048 * TextureCubeFaceCount)
					&& EnumHasAnyFlags(Desc.Flags, SupportedFlags)
					&& (!EnumHasAnyFlags(Desc.Flags,
						ETextureCreateFlags::ShaderResource)
						|| b2D || bCube || b2DArray || b3D)
					&& (!EnumHasAnyFlags(Desc.Flags, ETextureCreateFlags::Storage)
						|| (bCube && Desc.Format == EPixelFormat::RGBA16_FLOAT)
						|| (b2D && (Desc.Format == EPixelFormat::R8_UNORM
							|| Desc.Format == EPixelFormat::RGBA8_UNORM
							|| Desc.Format == EPixelFormat::RGBA16_FLOAT)))
					&& (!EnumHasAnyFlags(Desc.Flags, ETextureCreateFlags::RenderTargetable)
						|| (b2D && Desc.NumMips == 1
						&& IsMetalColorRenderFormat(Desc.Format)))
					&& (!EnumHasAnyFlags(Desc.Flags,
						ETextureCreateFlags::DepthStencilTargetable)
						|| ((b2D || b2DArray) && Desc.NumMips == 1
							&& Desc.Format == EPixelFormat::D32))
					&& (Desc.Format != EPixelFormat::D32
						|| !EnumHasAnyFlags(Desc.Flags,
							ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::Storage))
					&& (static_cast<uint64>(Desc.Flags)
						& ~static_cast<uint64>(SupportedFlags)) == 0;
			}
			auto RHITryCreateTexture(FRHICommandListBase&,
			const FRHITextureCreateDesc& Desc)
			-> std::expected<FTextureRHIRef, FRHICreationError> override
			{
				const FMetalAutoreleasePool Pool;
				if (!RHIIsTextureSupported(Desc))
					return std::unexpected(UnsupportedCreation());
				auto Native = MakeMetalTextureDescriptor(Desc);
				auto Texture = Native ? NS::TransferPtr(Device->newTexture(Native.get()))
					: NS::SharedPtr<MTL::Texture>{};
				if (!Texture)
					return std::unexpected(FRHICreationError{
						.Failure = ERHIResourceCreationFailure::OutOfMemory,
						.Source = ERHICreationFailureSource::NativeBackend});
				return FTextureRHIRef(new FMetalTexture(Desc, std::move(Texture)));
			}
			auto RHICreateSampler(const FRHISamplerDesc& Desc)
			-> TRefCountPtr<FRHISampler> override
			{
				const FMetalAutoreleasePool Pool;
				if (!Device) return nullptr;
				auto Native = MakeMetalSamplerDescriptor(Desc);
				if (!Native) return nullptr;
				auto Sampler = NS::TransferPtr(Device->newSamplerState(Native.get()));
				return Sampler ? TRefCountPtr<FRHISampler>(
					new FMetalSampler(std::move(Sampler))) : nullptr;
			}
			auto RHICreateShader(const FRHIShaderCreateDesc& Desc)
			-> TRefCountPtr<FRHIShader> override
			{ return CreateMetalShader(Device.get(), Desc); }
			auto RHITryCreateBuffer(FRHICommandListImmediate&,
			const FRHIBufferCreateDesc& Desc)
			-> std::expected<FBufferRHIRef, FRHICreationError> override
			{
				const FMetalAutoreleasePool Pool;
				if (!Device || Desc.Size == 0 || Desc.Usage == EBufferUsageFlags::None
					|| EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::NullResource)
					|| (Desc.InitialData.Size != 0 && !Desc.InitialData.Data)
					|| Desc.InitialData.Size > Desc.Size)
					return std::unexpected(UnsupportedCreation());
				if (Desc.Size > Device->maxBufferLength())
					return std::unexpected(FRHICreationError{
						.Failure = ERHIResourceCreationFailure::ResourceExhausted,
						.Source = ERHICreationFailureSource::NativeBackend});
				auto Buffer = NS::TransferPtr(Device->newBuffer(
					Desc.Size, MTL::ResourceStorageModeShared));
				if (!Buffer)
					return std::unexpected(FRHICreationError{
						.Failure = ERHIResourceCreationFailure::OutOfMemory,
						.Source = ERHICreationFailureSource::NativeBackend});
				if (Desc.InitialData.Data && Desc.InitialData.Size)
					std::memcpy(Buffer->contents(), Desc.InitialData.Data, Desc.InitialData.Size);
				auto Result = FBufferRHIRef(new FMetalBuffer(Desc, std::move(Buffer)));
				if (Desc.InitialData.Data && Desc.InitialData.Size)
				{
					const auto CanonicalAccess = GetMetalCanonicalBufferAccess(Desc.Usage);
					static_cast<FMetalBuffer*>(Result.GetReference())->GetStateTracker().Apply(
						0, Desc.InitialData.Size, CanonicalAccess == ERHIAccess::None
							? ERHIAccess::HostWrite : CanonicalAccess);
				}
				return Result;
			}
			auto RHICreateBufferView(FRHIBuffer* Buffer,
			const FRHIBufferViewDesc& Desc) -> TRefCountPtr<FRHIBufferView> override
			{
				if (!Buffer || IsCPUAuthoredBuffer(Buffer)
					|| !ValidateBufferViewDesc(Buffer, Desc)
					|| Desc.Type == ERHIBufferViewType::Formatted)
					return nullptr;
				return new FRHIBufferView(Buffer, Desc);
			}
			auto RHICreateTextureView(FRHITexture* Texture,
			const FRHITextureViewDesc& Desc) -> TRefCountPtr<FRHITextureView> override
			{
				const FMetalAutoreleasePool Pool;
				if (!Texture || !ValidateTextureViewDesc(Texture, Desc)
					|| (Desc.Usage != ERHITextureViewUsage::Sampled
						&& Desc.Usage != ERHITextureViewUsage::Storage
						&& Desc.Usage != ERHITextureViewUsage::TransferSource
						&& Desc.Usage != ERHITextureViewUsage::TransferDestination
						&& Desc.Usage != ERHITextureViewUsage::ColorAttachment
						&& Desc.Usage != ERHITextureViewUsage::DepthStencilAttachment))
					return nullptr;
				if (Desc.Usage == ERHITextureViewUsage::DepthStencilAttachment
					&& ((Texture->GetDimension() != ETextureDimension::Texture2D
						&& Texture->GetDimension() != ETextureDimension::Texture2DArray)
						|| Texture->GetFormat() != EPixelFormat::D32
						|| Desc.Dimension != ERHITextureViewDimension::Texture2D
						|| Desc.Range.FirstMip != 0 || Desc.Range.NumMips != 1
						|| Desc.Range.NumArrayLayers != 1))
					return nullptr;
				if (Desc.Usage == ERHITextureViewUsage::Sampled
					|| Desc.Usage == ERHITextureViewUsage::Storage)
				{
					auto View = MakeMetalTextureView(
						static_cast<FMetalTexture*>(Texture)->GetHandle(), Desc);
					return View ? TRefCountPtr<FRHITextureView>(
						new FMetalTextureView(Texture, Desc, std::move(View))) : nullptr;
				}
				if (Desc.Usage == ERHITextureViewUsage::ColorAttachment
					&& (!EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::RenderTargetable)
						|| Texture->GetDimension() != ETextureDimension::Texture2D
						|| Desc.Range.FirstMip != 0 || Desc.Range.NumMips != 1
						|| Desc.Range.FirstArrayLayer != 0 || Desc.Range.NumArrayLayers != 1))
					return nullptr;
				return new FRHITextureView(Texture, Desc);
			}
		private:
			static auto UnsupportedCreation() -> FRHICreationError
			{
				return {.Failure = ERHIResourceCreationFailure::UnsupportedDescriptor,
					.Source = ERHICreationFailureSource::NativeBackend};
			}
			NS::SharedPtr<MTL::Device> Device;
			NS::SharedPtr<MTL::CommandQueue> Queue;
			NS::SharedPtr<CA::MetalLayer> StartupLayer;
			void* StartupWindow = nullptr;
			std::shared_ptr<FMetalSubmissionState> State;
			FRHIQueueCapabilities QueueCapabilities;
			std::unique_ptr<FMetalCommandContext> CommandContext = CreateMetalCommandContext();
			mutable std::mutex PipelineCreationMutex;
			std::unique_ptr<FRHIPipelineStateCache> PipelineCache;
			bool PipelineCreationClosed = false;
		};

		class FMetalDynamicRHIModule final : public IDynamicRHIModule
		{
		public:
			auto SupportsDynamicReloading() const -> bool override { return true; }
			auto CreateRHI() -> FDynamicRHI* override
			{ return new FMetalDynamicRHI(); }
		};
	}

	auto GetMetalSubmissionQueue(FDynamicRHI& RHI) -> MTL::CommandQueue*
	{
		auto* Metal = dynamic_cast<FMetalDynamicRHI*>(&RHI);
		return Metal ? Metal->GetSubmissionQueue() : nullptr;
	}

	IMPLEMENT_MODULE(FMetalDynamicRHIModule, MetalRHI)
}
