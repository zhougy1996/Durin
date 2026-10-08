#include "MetalCommandContext.h"
#include "Backend/RHICompletionBackend.h"
#include "Backend/RHIDeferredBufferBackend.h"
#include "MetalAutoreleasePool.h"
#include "MetalBuffer.h"
#include "MetalPipeline.h"
#include "MetalSubmission.h"
#include <QuartzCore/QuartzCore.hpp>
#include "MetalSampler.h"
#include "MetalTexture.h"
#include "MetalViewport.h"
#include "MetalResourceDescriptors.h"
#include "MetalResourceState.h"
#include "RHIShaderParameters.h"
#include "Profiling/Profiling.h"

namespace Durin
{
	namespace
	{
		auto MetalBytesPerTexel(EPixelFormat Format) -> NS::UInteger
		{ return GetPixelFormatInfo(Format).BytesPerBlock; }

		struct FMetalDeferredBacking
		{
			std::shared_ptr<const FRHIDeferredBufferSnapshot> Snapshot;
			NS::SharedPtr<MTL::Buffer> Handle;
			~FMetalDeferredBacking()
			{
				const FMetalAutoreleasePool Pool;
				Handle.reset();
			}
		};

		using FBoundBufferSnapshots = std::map<std::tuple<uint32, uint32, uint32>,
			std::shared_ptr<const FRHIDeferredBufferSnapshot>>;

		class FMetalCommandContextImpl final : public FMetalCommandContext
		{
		public:
			auto Configure(std::shared_ptr<FMetalSubmissionState> InState) -> void override
			{ State = std::move(InState); }
			auto RHISetReplayStorageOwner(std::shared_ptr<void> Owner) -> void override
			{
				StorageOwner = std::move(Owner);
				if (Active && StorageOwner)
					Active->StorageOwners.push_back(StorageOwner);
			}
			auto RHIBeginGPUSubmission(const FRHIGPUSubmissionDesc& Desc) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(State && !Active, "Metal GPU submissions cannot nest.");
				requiref(Desc.Queue == FRHIQueueId{0},
					"Metal baseline accepts only graphics queue zero.");
				for (const auto& Wait : Desc.Waits)
				{
					const auto Point = FRHIGPUSyncPointBackend::GetPoint(Wait);
					requiref(Point.DeviceGeneration == State->Generation
						&& Point.Queue == Desc.Queue
						&& Wait.GetState() != ERHIGPUSubmissionState::Canceled
						&& Wait.GetState() != ERHIGPUSubmissionState::Failed,
						"Metal GPU dependency must belong to live work on this queue.");
				}
				FMetalPendingSubmission Submission;
				Submission.Command = NS::RetainPtr(State->Queue->commandBuffer());
				requiref(static_cast<bool>(Submission.Command),
					"Metal command queue could not allocate a command buffer.");
				if (StorageOwner) Submission.StorageOwners.push_back(StorageOwner);
				Active.emplace(std::move(Submission));
				ComputePipeline = nullptr;
				ComputeParameters.clear();
				ComputeBufferSnapshots.clear();
				ComputeParameterOwners.clear();
				ComputePushConstants.clear();
				ComputePushConstantWritten.clear();
				GraphicsPipeline = nullptr;
				BoundVertexStreams = 0;
				IndexBuffer = nullptr;
				GraphicsParameters.clear();
				GraphicsBufferSnapshots.clear();
				GraphicsParameterOwners.clear();
				for (auto& Bytes : GraphicsPushConstants) Bytes.clear();
				for (auto& Written : GraphicsPushConstantWritten) Written.clear();
			}
			auto RHIEndGPUSubmission(const FRHIGPUSyncPointRef& Signal) -> void override
			{
				requiref(State && Active && Signal && !RenderEncoder,
					"Metal GPU submission end requires an active recording and signal.");
				{
					std::lock_guard Lock(State->Mutex);
					Active->Producer = State->Timeline->Reserve();
					requiref(Active->Producer
						&& FRHIGPUSyncPointBackend::Attach(Signal, Active->Producer),
						"Metal GPU signal could not attach to its queue reservation.");
				}
				Pending.push_back(std::move(*Active));
				Active.reset();
			}
			auto RHIBeginFrame(const FRHIBeginFrameArgs&) -> void override
			{
				requiref(State && !bFrameOpen && !Active,
					"Metal frame begin requires an idle command context.");
				bFrameOpen = true;
			}
			auto RHISubmitCommands() -> void override
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Metal.SubmitCommands");
				const FMetalAutoreleasePool Pool;
				requiref(State && !Active,
					"Metal submission requires closed logical GPU recordings.");
				if (Pending.empty()) return;
				std::vector<FRHIGPUSyncPointRef> Signals;
				Signals.reserve(Pending.size());
				for (const auto& Submission : Pending)
					Signals.push_back(Submission.Producer);
				{
					std::lock_guard Lock(State->Mutex);
					requiref(State->Timeline->CanSubmitBatch(Signals),
						"Metal queue submissions must form an ordered pending prefix.");
					for (auto& Submission : Pending)
					{
						auto SharedState = State;
						auto Producer = Submission.Producer;
						auto Owners = std::make_shared<FMetalSubmissionOwners>(
							std::move(static_cast<FMetalSubmissionOwners&>(Submission)));
						Submission.Command->addCompletedHandler(MTL::HandlerFunction(
							[SharedState, Producer, Owners](MTL::CommandBuffer* Completed) {
							CompleteMetalSubmission(*SharedState, Producer, *Owners, Completed->status());
						}));
						++State->PendingCallbacks;
						{
							DURIN_PROFILE_CPU_ZONE_NAMED("Metal.CommandBuffer.Commit");
							Submission.Command->commit();
						}
						requiref(State->Timeline->MarkSubmitted(Producer),
							"Metal queue lost an accepted GPU reservation.");
					}
				}
				Pending.clear();
			}
			auto RHIEndFrame() -> void override
			{
				requiref(bFrameOpen && !Active && !RenderEncoder,
					"Metal frame end requires closed GPU submissions.");
				RHISubmitCommands();
				bFrameOpen = false;
			}
			auto RHIBeginGPUTimingQuery(FRHIGPUTimingQuery* Query) -> void override
			{
				auto* Timing = dynamic_cast<FMetalGPUTimingQuery*>(Query);
				requiref(Timing && State && Timing->Pool == State->TimingPool && !RenderEncoder,
					"Metal timing requires a live device query outside a render pass.");
				OpenTimingQueries.emplace_back(Timing);
				RecordTimingSample(*Timing, false);
			}
			auto RHIEndGPUTimingQuery(FRHIGPUTimingQuery* Query) -> void override
			{
				auto* Timing = dynamic_cast<FMetalGPUTimingQuery*>(Query);
				requiref(Timing && !OpenTimingQueries.empty() && OpenTimingQueries.back() == Timing,
					"Metal timing intervals must end in nesting order.");
				require(Timing->CommitRecording());
				RecordTimingSample(*Timing, true);
				OpenTimingQueries.pop_back();
			}
			// Public recording validates region scope; unavailable native labels are harmless.
			auto RHIBeginDiagnosticRegion(std::string_view) -> void override {}
			auto RHIEndDiagnosticRegion() -> void override {}
			auto RHIBeginRenderPass(const FRHIRenderPassInfo& Info, FName) -> void override
			{
				const FMetalAutoreleasePool Pool;
				if (!Active && State)
				{
					FMetalPendingSubmission Submission;
					Submission.Command = NS::RetainPtr(State->Queue->commandBuffer());
					requiref(static_cast<bool>(Submission.Command),
						"Metal implicit render command allocation failed.");
					if (StorageOwner) Submission.StorageOwners.push_back(StorageOwner);
					Active.emplace(std::move(Submission));
					bImplicitRenderSubmission = true;
				}
				requiref(Active && !RenderEncoder && Info.RenderTargetLayout.IsValid()
					&& Info.RenderTargetLayout.NumColorRenderTargets <= 4
					&& (Info.RenderTargetLayout.NumColorRenderTargets > 0
						|| Info.RenderTargetLayout.bHasDepthStencil)
					&& bool(Info.DepthStencilRenderTarget)
						== Info.RenderTargetLayout.bHasDepthStencil,
					"Metal baseline supports up to four color attachments with optional depth.");
				std::vector<FRHITextureTransition> AttachmentStates;
				const auto QueueAttachment = [&](FRHITextureView* View, const FRHIAttachmentLayout& Layout) {
					require(View);
					auto* Texture = dynamic_cast<FMetalTexture*>(View->GetTexture());
					require(Texture);
					const auto Expected = Layout.InitialLayout == ERHITextureLayout::Undefined
						? ERHIAccess::Discard : Layout.InitialAccess;
					ERHIAccess Tracked = ERHIAccess::None;
					requiref(Texture->ValidateAccess(View->GetDesc().Range, Expected, Tracked),
						"Metal render-pass attachment state mismatch: resource={}, expected={}, tracked={}, requested={}.",
						static_cast<const void*>(Texture), static_cast<uint32>(Expected), static_cast<uint32>(Tracked), static_cast<uint32>(Layout.FinalAccess));
					AttachmentStates.push_back({Texture, View->GetDesc().Range, ERHIAccess::Discard, Layout.FinalAccess});
				};
				for (uint32 Index = 0; Index < Info.RenderTargetLayout.NumColorRenderTargets; ++Index)
					QueueAttachment(Info.ColorRenderTargetViews[Index], Info.RenderTargetLayout.ColorAttachments[Index].RenderTarget);
				if (Info.RenderTargetLayout.bHasDepthStencil)
					QueueAttachment(Info.DepthStencilRenderTargetView, Info.RenderTargetLayout.DepthStencilAttachment);
				FRHITexture* Color = Info.ColorRenderTargets[0];
				auto Desc = NS::RetainPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
				for (uint32 Index = 0;
					Index < Info.RenderTargetLayout.NumColorRenderTargets; ++Index)
				{
					FRHITexture* Target = Info.ColorRenderTargets[Index];
					const auto& AttachmentLayout =
						Info.RenderTargetLayout.ColorAttachments[Index];
					const auto& Layout = AttachmentLayout.RenderTarget;
					requiref(Target && Info.ColorRenderTargetViews[Index]
						&& Info.ColorRenderTargetViews[Index]->GetTexture() == Target
						&& !AttachmentLayout.bHasResolveTarget
						&& !Info.ColorResolveTargets[Index]
						&& Target->GetDimension() == ETextureDimension::Texture2D
						&& Target->GetNumSamples() == 1 && Layout.NumSamples == 1
						&& Target->GetFormat() == Layout.Format
						&& IsMetalColorRenderFormat(Layout.Format)
						&& Target->GetSizeX() == Color->GetSizeX()
						&& Target->GetSizeY() == Color->GetSizeY()
						&& EnumHasAnyFlags(Target->GetFlags(),
							ETextureCreateFlags::RenderTargetable)
						&& Info.ColorClearValues[Index].Binding == EClearBinding::Color,
						"Metal color attachment does not match its render pass layout.");
					auto* Texture = static_cast<FMetalTexture*>(Target)->GetHandle();
					auto* Attachment = Desc->colorAttachments()->object(Index);
					Attachment->setTexture(Texture);
					switch (Layout.LoadAction)
					{
					case ERHIRenderTargetLoadAction::Clear:
						Attachment->setLoadAction(MTL::LoadActionClear);
						Attachment->setClearColor(MTL::ClearColor::Make(
							Info.ColorClearValues[Index].ClearValue.Color[0],
							Info.ColorClearValues[Index].ClearValue.Color[1],
							Info.ColorClearValues[Index].ClearValue.Color[2],
							Info.ColorClearValues[Index].ClearValue.Color[3]));
						break;
					case ERHIRenderTargetLoadAction::Load:
						Attachment->setLoadAction(MTL::LoadActionLoad); break;
					case ERHIRenderTargetLoadAction::DontCare:
						Attachment->setLoadAction(MTL::LoadActionDontCare); break;
					}
					Attachment->setStoreAction(Layout.StoreAction
						== ERHIRenderTargetStoreAction::Store
						? MTL::StoreActionStore : MTL::StoreActionDontCare);
					Active->NativeResources.push_back(NS::RetainPtr(Texture));
				}
				MTL::Texture* DepthTexture = nullptr;
				if (Info.RenderTargetLayout.bHasDepthStencil)
				{
					FRHITexture* Depth = Info.DepthStencilRenderTarget;
					const auto& DepthLayout = Info.RenderTargetLayout.DepthStencilAttachment;
					requiref(Info.DepthStencilRenderTargetView
						&& Info.DepthStencilRenderTargetView->GetTexture() == Depth
						&& (Depth->GetDimension() == ETextureDimension::Texture2D
							|| Depth->GetDimension() == ETextureDimension::Texture2DArray)
						&& Depth->GetFormat() == EPixelFormat::D32
						&& DepthLayout.Format == EPixelFormat::D32
						&& Depth->GetNumSamples() == 1
						&& DepthLayout.NumSamples == 1
						&& (!Color || Depth->GetSizeX() == Color->GetSizeX())
						&& (!Color || Depth->GetSizeY() == Color->GetSizeY())
						&& EnumHasAnyFlags(Depth->GetFlags(),
							ETextureCreateFlags::DepthStencilTargetable)
						&& Info.DepthStencilClearValue.Binding
							== EClearBinding::DepthStencil,
						"Metal depth attachment does not match its render pass layout.");
					DepthTexture = static_cast<FMetalTexture*>(Depth)->GetHandle();
					Desc->depthAttachment()->setTexture(DepthTexture);
					Desc->depthAttachment()->setSlice(Info.DepthStencilRenderTargetView->GetDesc().Range.FirstArrayLayer);
					switch (DepthLayout.LoadAction)
					{
					case ERHIRenderTargetLoadAction::Clear:
						Desc->depthAttachment()->setLoadAction(MTL::LoadActionClear);
						Desc->depthAttachment()->setClearDepth(Info.DepthStencilClearValue.ClearValue.DSValue.Depth);
						break;
					case ERHIRenderTargetLoadAction::Load:
						Desc->depthAttachment()->setLoadAction(MTL::LoadActionLoad); break;
					case ERHIRenderTargetLoadAction::DontCare:
						Desc->depthAttachment()->setLoadAction(MTL::LoadActionDontCare); break;
					}
					Desc->depthAttachment()->setStoreAction(DepthLayout.StoreAction == ERHIRenderTargetStoreAction::Store
							? MTL::StoreActionStore : MTL::StoreActionDontCare);
				}
				RenderEncoder = NS::RetainPtr(Active->Command->renderCommandEncoder(Desc.get()));
				requiref(static_cast<bool>(RenderEncoder), "Metal render encoder creation failed.");
				CurrentRenderTargetLayout = Info.RenderTargetLayout;
				PendingAttachmentStates = std::move(AttachmentStates);
				RenderWidth = Color ? Color->GetSizeX()
					: Info.DepthStencilRenderTarget->GetSizeX();
				RenderHeight = Color ? Color->GetSizeY()
					: Info.DepthStencilRenderTarget->GetSizeY();
				if (DepthTexture) Active->NativeResources.push_back(NS::RetainPtr(DepthTexture));
			}
			auto RHIEndRenderPass() -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(static_cast<bool>(RenderEncoder), "Metal render pass is not active.");
				RenderEncoder->endEncoding();
				RenderEncoder.reset();
				for (const auto& Attachment : PendingAttachmentStates)
					static_cast<FMetalTexture*>(Attachment.Texture)->ApplyAccess(Attachment.Range, Attachment.RequiredAfter);
				PendingAttachmentStates.clear();
				GraphicsPipeline = nullptr;
				BoundVertexStreams = 0;
				IndexBuffer = nullptr;
				GraphicsParameters.clear();
				GraphicsBufferSnapshots.clear();
				GraphicsParameterOwners.clear();
				for (auto& Bytes : GraphicsPushConstants) Bytes.clear();
				for (auto& Written : GraphicsPushConstantWritten) Written.clear();
				if (bImplicitRenderSubmission)
				{
					std::lock_guard Lock(State->Mutex);
					Active->Producer = State->Timeline->Reserve();
					requiref(Active->Producer,
						"Metal implicit render submission could not reserve its queue.");
					Pending.push_back(std::move(*Active));
					Active.reset();
					bImplicitRenderSubmission = false;
				}
			}
			auto RHIBeginDrawingViewport(FRHIViewport* Viewport,
				FRHITexture*) -> void override
			{
				auto* MetalViewport = dynamic_cast<FMetalViewport*>(Viewport);
				requiref(State && MetalViewport && !Active && !RenderEncoder,
					"Metal viewport drawing requires an idle context and viewport.");
				MetalViewport->ApplyRequestedPresentationPolicy();
			}
			auto RHIEndDrawingViewport(FRHIViewport* Viewport,
				bool bPresent, bool) -> void override
			{
				const FMetalAutoreleasePool Pool;
				auto* MetalViewport = dynamic_cast<FMetalViewport*>(Viewport);
				requiref(State && MetalViewport && !Active && !RenderEncoder,
					"Metal viewport presentation requires a closed GPU submission.");
				if (!bPresent) return;
				RHISubmitCommands();
				auto BackBuffer = MetalViewport->SnapshotBackBuffer();
				if (!BackBuffer) return;
				auto* Layer = MetalViewport->GetLayer();
				MetalViewport->ApplyRequestedPresentationPolicy();
				auto Drawable = [&]() {
					DURIN_PROFILE_CPU_ZONE_NAMED("Metal.Present.AcquireDrawable");
					return NS::RetainPtr(Layer->nextDrawable());
				}();
				if (!Drawable) return;
				auto* Source = static_cast<FMetalTexture*>(BackBuffer.GetReference())->GetHandle();
				auto* Destination = Drawable->texture();
				if (Source->width() != Destination->width()
					|| Source->height() != Destination->height()
					|| Source->pixelFormat() != Destination->pixelFormat()) return;
				auto Command = NS::RetainPtr(State->Queue->commandBuffer());
				requiref(static_cast<bool>(Command), "Metal presentation command allocation failed.");
				auto Encoder = NS::RetainPtr(Command->blitCommandEncoder());
				requiref(static_cast<bool>(Encoder), "Metal presentation blit encoder failed.");
				Encoder->copyFromTexture(Source, 0, 0, MTL::Origin::Make(0, 0, 0),
					MTL::Size::Make(Source->width(), Source->height(), 1), Destination, 0, 0,
					MTL::Origin::Make(0, 0, 0));
				Encoder->endEncoding();
				Command->presentDrawable(Drawable.get());
				auto SharedState = State;
				auto Owners = std::make_shared<FMetalSubmissionOwners>();
				Owners->NativeResources.push_back(NS::RetainPtr(Source));
				Owners->NativeResources.push_back(NS::RetainPtr(Destination));
				Owners->Drawable = std::move(Drawable);
				Command->addCompletedHandler(MTL::HandlerFunction(
					[SharedState, Owners](MTL::CommandBuffer*) {
					const FMetalAutoreleasePool Pool;
					(void)Owners;
					Owners->Release();
					std::lock_guard Lock(SharedState->Mutex);
					--SharedState->PendingCallbacks;
					SharedState->Completion.notify_all();
				}));
				{
					std::lock_guard Lock(State->Mutex);
					++State->PendingCallbacks;
				}
				{
					DURIN_PROFILE_CPU_ZONE_NAMED("Metal.Present.Commit");
					Command->commit();
				}
				if (Profiling::RecordEditorShellFirstPresent())
					DURIN_PROFILE_STARTUP_FIRST_PRESENT();
			}
			auto RHISetViewport(float MinX, float MinY, float MinZ,
				float MaxX, float MaxY, float MaxZ) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(RenderEncoder && std::isfinite(MinX)
					&& std::isfinite(MinY) && std::isfinite(MinZ)
					&& std::isfinite(MaxX) && std::isfinite(MaxY)
					&& std::isfinite(MaxZ)
					&& MinX >= 0 && MinY >= 0 && MaxX > MinX && MaxY > MinY
					&& MaxX <= RenderWidth && MaxY <= RenderHeight
					&& MinZ >= 0 && MinZ <= 1 && MaxZ >= MinZ && MaxZ <= 1,
					"Invalid Metal viewport bounds.");
				const double MaxDepth = MinZ == MaxZ ? MinZ + 1.0 : MaxZ;
				RenderEncoder->setViewport(MTL::Viewport{
					MinX, MinY, MaxX - MinX, MaxY - MinY, MinZ, MaxDepth});
				RHISetScissor(MinX, MinY, MaxX - MinX, MaxY - MinY);
			}
			auto RHISetScissor(float MinX, float MinY,
				float Width, float Height) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(RenderEncoder && std::isfinite(MinX)
					&& std::isfinite(MinY) && std::isfinite(Width)
					&& std::isfinite(Height) && MinX >= 0 && MinY >= 0
					&& Width > 0 && Height > 0
					&& MinX + Width <= RenderWidth
					&& MinY + Height <= RenderHeight,
					"Invalid Metal scissor bounds.");
				RenderEncoder->setScissorRect(MTL::ScissorRect{
					static_cast<NS::UInteger>(MinX),
					static_cast<NS::UInteger>(MinY),
					static_cast<NS::UInteger>(Width),
					static_cast<NS::UInteger>(Height)});
			}
			auto RHISetDepthBias(float ConstantFactor, float Clamp,
				float SlopeFactor) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(RenderEncoder && GraphicsPipeline
					&& GraphicsPipeline->GetRasterizer().bEnableDepthBias
					&& std::isfinite(ConstantFactor) && std::isfinite(Clamp)
					&& std::isfinite(SlopeFactor),
					"Metal depth bias requires an active depth-bias pipeline and finite values.");
				RenderEncoder->setDepthBias(ConstantFactor, SlopeFactor, Clamp);
			}
			auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& State) -> void override
			{
				const FMetalAutoreleasePool Pool;
				auto* Pipeline = dynamic_cast<FMetalGraphicsPipelineState*>(&State);
				requiref(RenderEncoder && Pipeline
					&& Pipeline->GetRenderTargets() == CurrentRenderTargetLayout,
					"Metal graphics pipeline does not match the active render pass.");
				GraphicsPipeline = Pipeline;
				GraphicsParameters.clear();
				GraphicsBufferSnapshots.clear();
				GraphicsParameterOwners.clear();
				for (auto& Bytes : GraphicsPushConstants) Bytes.clear();
				for (auto& Written : GraphicsPushConstantWritten) Written.clear();
				RenderEncoder->setRenderPipelineState(Pipeline->GetPipeline());
				RenderEncoder->setDepthStencilState(Pipeline->GetDepthStencil());
				RenderEncoder->setBlendColor(0.0f, 0.0f, 0.0f, 0.0f);
				const auto& Raster = Pipeline->GetRasterizer();
				RenderEncoder->setCullMode(Raster.CullMode == ERHICullMode::None
					? MTL::CullModeNone : Raster.CullMode == ERHICullMode::Front
						? MTL::CullModeFront : MTL::CullModeBack);
				RenderEncoder->setFrontFacingWinding(Raster.FrontFace == ERHIFrontFace::Clockwise
						? MTL::WindingClockwise : MTL::WindingCounterClockwise);
				RenderEncoder->setDepthBias(0.0f, 0.0f, 0.0f);
				Active->ResourceOwners.emplace_back(Pipeline);
			}
			auto RHISetComputePipelineState(FRHIComputePipelineState& State) -> void override
			{
				requiref(Active && !RenderEncoder,
					"Metal compute pipeline requires an active submission outside a render pass.");
				ComputePipeline = &State;
				ComputeParameters.clear();
				ComputeBufferSnapshots.clear();
				ComputeParameterOwners.clear();
				ComputePushConstants.clear();
				ComputePushConstantWritten.clear();
			}
			auto RHIBindVertexBuffer(uint32 Stream, FRHIBuffer* Resource,
				uint32 Offset) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(RenderEncoder, "Metal vertex binding requires a render pass.");
				requiref(Stream < 16, "Metal vertex stream index exceeds 15.");
				if (!Resource)
				{
					RenderEncoder->setVertexBuffer(nullptr, 0, Stream);
					BoundVertexStreams &= ~uint16(1u << Stream);
					return;
				}
				requiref(Resource->GetResourceType() == ERHIResourceType::Buffer
					&& EnumHasAnyFlags(Resource->GetUsage(),
						EBufferUsageFlags::VertexBuffer)
					&& Offset <= Resource->GetSize(),
					"Invalid Metal vertex buffer binding.");
				auto* Buffer = dynamic_cast<FMetalBuffer*>(Resource);
				requiref(Buffer && Buffer->GetHandle(),
					"Metal vertex buffer has no native allocation.");
				RenderEncoder->setVertexBuffer(Buffer->GetHandle(), Offset, Stream);
				BoundVertexStreams |= uint16(1u << Stream);
				Active->NativeResources.push_back(NS::RetainPtr(Buffer->GetHandle()));
				Active->ResourceOwners.emplace_back(Buffer);
			}
			auto RHIBindIndexBuffer(FRHIBuffer* Resource, uint32 Offset) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(RenderEncoder && Resource
					&& Resource->GetResourceType() == ERHIResourceType::Buffer
					&& EnumHasAnyFlags(Resource->GetUsage(),
						EBufferUsageFlags::IndexBuffer)
					&& (Resource->GetStride() == 2 || Resource->GetStride() == 4)
					&& Offset % Resource->GetStride() == 0
					&& Offset < Resource->GetSize(),
					"Invalid Metal index buffer binding.");
				IndexBuffer = Resource;
				IndexBufferOffset = Offset;
				Active->NativeResources.push_back(NS::RetainPtr(
					static_cast<FMetalBuffer*>(Resource)->GetHandle()));
				Active->ResourceOwners.emplace_back(Resource);
			}
			auto RHITransitionBuffers(std::span<const FRHIBufferTransition> Transitions)
			-> void override
			{
				const auto Error = ApplyMetalBufferTransitions(Transitions);
				requiref(!Error, "{}", Error.value_or(""));
				// Tracked resources on the single queue supply native hazard synchronization.
			}
			auto RHITransitionTextures(std::span<const FRHITextureTransition> Transitions)
			-> void override
			{
				const auto Error = ApplyMetalTextureTransitions(Transitions);
				requiref(!Error, "{}", Error.value_or(""));
			}
			auto RHICopyBuffer(FRHIBuffer* Source, FRHIBuffer* Destination,
			std::span<const FRHIBufferCopyRegion> Regions) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(Active && ValidateBufferCopies(Source, Destination, Regions),
					"Invalid Metal buffer copy recording.");
				auto* SourceBuffer = static_cast<FMetalBuffer*>(Source);
				auto* DestinationBuffer = static_cast<FMetalBuffer*>(Destination);
				auto Encoder = NS::RetainPtr(Active->Command->blitCommandEncoder());
				requiref(static_cast<bool>(Encoder), "Metal buffer copy encoder creation failed.");
				for (const auto& Region : Regions)
					Encoder->copyFromBuffer(
						SourceBuffer->GetHandle(),
						Region.SourceOffset,
						DestinationBuffer->GetHandle(),
						Region.DestinationOffset,
						Region.Size);
				Encoder->endEncoding();
				Active->NativeResources.push_back(NS::RetainPtr(SourceBuffer->GetHandle()));
				Active->NativeResources.push_back(NS::RetainPtr(DestinationBuffer->GetHandle()));
			}
			auto RHICopyBufferToTexture(FRHIBuffer* Source, FRHITexture* Destination,
			std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(Active && ValidateBufferToTextureCopies(Source, Destination, Regions),
					"Invalid Metal buffer-to-texture copy.");
				auto* Buffer = static_cast<FMetalBuffer*>(Source);
				auto* Texture = static_cast<FMetalTexture*>(Destination);
				auto Encoder = NS::RetainPtr(Active->Command->blitCommandEncoder());
				requiref(static_cast<bool>(Encoder), "Metal texture upload encoder creation failed.");
				for (const auto& Region : Regions)
				{
					const bool bVolume = Destination->GetDimension() == ETextureDimension::Texture3D;
					const NS::UInteger RowLength = Region.BufferRowLength
						? Region.BufferRowLength : Region.TextureExtent.Width;
					const NS::UInteger ImageHeight = Region.BufferImageHeight
						? Region.BufferImageHeight : Region.TextureExtent.Height;
					const NS::UInteger RowPitch = RowLength
						* MetalBytesPerTexel(Destination->GetFormat());
					const NS::UInteger ImagePitch = RowPitch * ImageHeight;
					for (uint32 Layer = 0; Layer < Region.TextureNumArrayLayers; ++Layer)
						Encoder->copyFromBuffer(Buffer->GetHandle(), Region.BufferOffset + Layer * ImagePitch, RowPitch, ImagePitch, MTL::Size::Make(Region.TextureExtent.Width,
								Region.TextureExtent.Height,
								bVolume ? Region.TextureExtent.Depth : 1), Texture->GetHandle(), bVolume ? 0 : Region.TextureFirstArrayLayer + Layer, Region.TextureMip, MTL::Origin::Make(
								Region.TextureOffset.X, Region.TextureOffset.Y,
								bVolume ? Region.TextureOffset.Z : 0));
				}
				Encoder->endEncoding();
				Active->NativeResources.push_back(NS::RetainPtr(Buffer->GetHandle()));
				Active->NativeResources.push_back(NS::RetainPtr(Texture->GetHandle()));
			}
			auto RHICopyTextureToBuffer(FRHITexture* Source, FRHIBuffer* Destination,
			std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(Active && ValidateTextureToBufferCopies(Source, Destination, Regions),
					"Invalid Metal texture-to-buffer copy.");
				auto* Texture = static_cast<FMetalTexture*>(Source);
				auto* Buffer = static_cast<FMetalBuffer*>(Destination);
				auto Encoder = NS::RetainPtr(Active->Command->blitCommandEncoder());
				requiref(static_cast<bool>(Encoder), "Metal texture readback encoder creation failed.");
				for (const auto& Region : Regions)
				{
					const bool bVolume = Source->GetDimension() == ETextureDimension::Texture3D;
					const NS::UInteger RowLength = Region.BufferRowLength
						? Region.BufferRowLength : Region.TextureExtent.Width;
					const NS::UInteger ImageHeight = Region.BufferImageHeight
						? Region.BufferImageHeight : Region.TextureExtent.Height;
					const NS::UInteger RowPitch = RowLength
						* MetalBytesPerTexel(Source->GetFormat());
					const NS::UInteger ImagePitch = RowPitch * ImageHeight;
					for (uint32 Layer = 0; Layer < Region.TextureNumArrayLayers; ++Layer)
						Encoder->copyFromTexture(Texture->GetHandle(), bVolume ? 0 : Region.TextureFirstArrayLayer + Layer, Region.TextureMip, MTL::Origin::Make(Region.TextureOffset.X,
								Region.TextureOffset.Y, bVolume ? Region.TextureOffset.Z : 0), MTL::Size::Make(Region.TextureExtent.Width,
								Region.TextureExtent.Height,
								bVolume ? Region.TextureExtent.Depth : 1), Buffer->GetHandle(), Region.BufferOffset + Layer * ImagePitch, RowPitch, ImagePitch);
				}
				Encoder->endEncoding();
				Active->NativeResources.push_back(NS::RetainPtr(Texture->GetHandle()));
				Active->NativeResources.push_back(NS::RetainPtr(Buffer->GetHandle()));
			}
			auto RHICopyTexture(FRHITexture* Source, FRHITexture* Destination,
			std::span<const FRHITextureCopyRegion> Regions) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(Active && ValidateTextureCopies(Source, Destination, Regions),
					"Invalid Metal texture copy.");
				requiref((Source->GetDimension() == ETextureDimension::Texture3D)
					== (Destination->GetDimension() == ETextureDimension::Texture3D),
					"Metal texture copy cannot mix volume and layered textures.");
				auto* SourceTexture = static_cast<FMetalTexture*>(Source);
				auto* DestinationTexture = static_cast<FMetalTexture*>(Destination);
				auto Encoder = NS::RetainPtr(Active->Command->blitCommandEncoder());
				requiref(static_cast<bool>(Encoder), "Metal texture copy encoder creation failed.");
				for (const auto& Region : Regions)
				{
					const bool bVolume = Source->GetDimension() == ETextureDimension::Texture3D;
					for (uint32 Layer = 0; Layer < Region.NumArrayLayers; ++Layer)
						Encoder->copyFromTexture(SourceTexture->GetHandle(), bVolume ? 0 : Region.SourceFirstArrayLayer + Layer, Region.SourceMip, MTL::Origin::Make(Region.SourceOffset.X, Region.SourceOffset.Y,
								bVolume ? Region.SourceOffset.Z : 0), MTL::Size::Make(Region.Extent.Width, Region.Extent.Height,
								bVolume ? Region.Extent.Depth : 1), DestinationTexture->GetHandle(), bVolume ? 0 : Region.DestinationFirstArrayLayer + Layer, Region.DestinationMip, MTL::Origin::Make(Region.DestinationOffset.X,
								Region.DestinationOffset.Y,
								bVolume ? Region.DestinationOffset.Z : 0));
				}
				Encoder->endEncoding();
				Active->NativeResources.push_back(NS::RetainPtr(SourceTexture->GetHandle()));
				Active->NativeResources.push_back(NS::RetainPtr(DestinationTexture->GetHandle()));
			}
			auto RHIWriteBuffer(FRHIBuffer* Buffer, uint32 Offset,
			FByteView Data) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(State && !RenderEncoder && Buffer
					&& Offset <= Buffer->GetSize()
					&& Data.size() <= Buffer->GetSize() - Offset,
					"Metal buffer upload exceeds its target range.");
				if (Data.empty()) return;
				const bool bImplicitUpload = !Active;
				if (bImplicitUpload)
				{
					FMetalPendingSubmission Submission;
					Submission.Command = NS::RetainPtr(State->Queue->commandBuffer());
					requiref(static_cast<bool>(Submission.Command),
						"Metal implicit buffer upload command allocation failed.");
					if (StorageOwner) Submission.StorageOwners.push_back(StorageOwner);
					Active.emplace(std::move(Submission));
				}
				auto StagingOwner = NS::TransferPtr(State->Queue->device()->newBuffer(
					Data.data(), Data.size(), MTL::ResourceStorageModeShared));
				auto* Staging = StagingOwner.get();
				requiref(Staging != nullptr, "Metal buffer upload staging allocation failed.");
				auto* Target = static_cast<FMetalBuffer*>(Buffer);
				auto Encoder = NS::RetainPtr(Active->Command->blitCommandEncoder());
				requiref(static_cast<bool>(Encoder), "Metal buffer upload encoder creation failed.");
				Encoder->copyFromBuffer(Staging, 0, Target->GetHandle(), Offset, Data.size());
				Encoder->endEncoding();
				const auto CanonicalAccess = GetMetalCanonicalBufferAccess(Buffer->GetUsage());
				Target->GetStateTracker().Apply(Offset, Data.size(), CanonicalAccess == ERHIAccess::None
					? ERHIAccess::TransferWrite : CanonicalAccess);
				Active->NativeResources.push_back(NS::RetainPtr(Staging));
				Active->NativeResources.push_back(NS::RetainPtr(Target->GetHandle()));
				Active->ResourceOwners.emplace_back(Buffer);
				if (bImplicitUpload)
				{
					std::lock_guard Lock(State->Mutex);
					Active->Producer = State->Timeline->Reserve();
					requiref(Active->Producer,
						"Metal implicit buffer upload could not reserve its queue.");
					Pending.push_back(std::move(*Active));
					Active.reset();
				}
			}
			auto RHIUploadBuffer(FRHIBuffer* Buffer, uint32 Offset,
			FByteView Data) -> void override
			{
				RHIWriteBuffer(Buffer, Offset, Data);
				if (!Data.empty()) static_cast<FMetalBuffer*>(Buffer)->GetStateTracker().Apply(Offset, Data.size(), ERHIAccess::TransferWrite);
			}
			auto RHIInitializeTexture(FRHITexture* Texture) -> void override
			{
				requiref(Texture && (Texture->GetDimension() == ETextureDimension::Texture2D
					|| Texture->GetDimension() == ETextureDimension::Texture2DArray
					|| Texture->GetDimension() == ETextureDimension::TextureCube
					|| Texture->GetDimension() == ETextureDimension::TextureCubeArray
					|| Texture->GetDimension() == ETextureDimension::Texture3D)
					&& ToMetalPixelFormat(Texture->GetFormat()) != MTL::PixelFormatInvalid,
					"Metal texture initialization requires a supported texture.");
				// Private Metal textures require no explicit creation-layout transition.
			}
			auto RHIUpdateTexture2D(FRHITexture* Texture, uint32 MipIndex,
			uint32 ArraySlice, const FUpdateTextureRegion2D& Region,
			uint32 SourcePitch, FByteView SourceData) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(State && Texture
					&& (Texture->GetDimension() == ETextureDimension::Texture2D
						|| Texture->GetDimension() == ETextureDimension::Texture2DArray
						|| Texture->GetDimension() == ETextureDimension::TextureCube
						|| Texture->GetDimension() == ETextureDimension::TextureCubeArray)
					&& ToMetalPixelFormat(Texture->GetFormat()) != MTL::PixelFormatInvalid,
					"Metal texture upload requires a supported destination texture.");
				FRHITextureDesc Desc;
				Desc.Dimension = Texture->GetDimension();
				Desc.Extent = FIntPoint(Texture->GetSizeX(), Texture->GetSizeY());
				Desc.Depth = static_cast<uint16>(Texture->GetSizeZ());
				Desc.ArraySize = Texture->GetArraySize();
				Desc.NumMips = Texture->GetNumMips();
				Desc.NumSamples = Texture->GetNumSamples();
				Desc.Format = Texture->GetFormat();
				requiref(ValidateTexture2DUpdate(Desc, MipIndex, ArraySlice,
					Region, SourcePitch).has_value(), "Invalid Metal texture upload region.");
				const auto& Format = GetPixelFormatInfo(Texture->GetFormat());
				const auto Layout = GetPixelFormatLayout(
					Texture->GetFormat(), Region.Width, Region.Height);
				const uint64 SourceStart = static_cast<uint64>(Region.SrcY / Format.BlockSize)
					* SourcePitch + static_cast<uint64>(Region.SrcX / Format.BlockSize)
					* Format.BytesPerBlock;
				const uint64 RequiredBytes = SourceStart
					+ (Layout.BlocksHigh - 1) * SourcePitch + Layout.RowPitch;
				requiref(RequiredBytes <= SourceData.size(),
					"Metal texture upload source data is incomplete.");
				const NS::UInteger RowPitch = Layout.RowPitch;
				const NS::UInteger ByteCount = Layout.DataSize;
				auto StagingOwner = NS::TransferPtr(State->Queue->device()->newBuffer(
					ByteCount, MTL::ResourceStorageModeShared));
				auto* Staging = StagingOwner.get();
				requiref(Staging != nullptr, "Metal texture upload staging allocation failed.");
				auto* TargetBytes = static_cast<std::byte*>(Staging->contents());
				for (uint32 Row = 0; Row < Layout.BlocksHigh; ++Row)
					std::memcpy(TargetBytes + static_cast<size_t>(Row) * RowPitch,
						SourceData.data() + SourceStart + static_cast<uint64>(Row) * SourcePitch,
						RowPitch);
				if (!Active) RHISubmitCommands();
				auto Command = Active ? Active->Command : NS::RetainPtr(State->Queue->commandBuffer());
				requiref(static_cast<bool>(Command), "Metal texture upload command allocation failed.");
				auto Encoder = NS::RetainPtr(Command->blitCommandEncoder());
				requiref(static_cast<bool>(Encoder), "Metal texture upload encoder creation failed.");
				auto* Native = static_cast<FMetalTexture*>(Texture)->GetHandle();
				Encoder->copyFromBuffer(
					Staging,
					0,
					RowPitch,
					ByteCount,
					MTL::Size::Make(Region.Width, Region.Height, 1),
					Native,
					ArraySlice,
					MipIndex,
					MTL::Origin::Make(Region.DestX, Region.DestY, 0));
				Encoder->endEncoding();
				static_cast<FMetalTexture*>(Texture)->ApplyAccess(
					{ERHITextureAspect::Color, MipIndex, 1, ArraySlice, 1},
					EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::Storage)
						? ERHIAccess::GraphicsShaderReadWrite : ERHIAccess::GraphicsShaderRead);
				if (Active)
				{
					Active->NativeResources.push_back(NS::RetainPtr(Staging));
					Active->NativeResources.push_back(NS::RetainPtr(Native));
					return;
				}
				auto SharedState = State;
				auto Owners = std::make_shared<FMetalSubmissionOwners>();
				Owners->NativeResources.push_back(NS::RetainPtr(Native));
				Owners->NativeResources.push_back(StagingOwner);
				Command->addCompletedHandler(MTL::HandlerFunction(
					[SharedState, Owners](MTL::CommandBuffer*) {
					const FMetalAutoreleasePool Pool;
					(void)Owners;
					Owners->Release();
					std::lock_guard Lock(SharedState->Mutex);
					--SharedState->PendingCallbacks;
					SharedState->Completion.notify_all();
				}));
				{
					std::lock_guard Lock(State->Mutex);
					++State->PendingCallbacks;
				}
				Command->commit();
			}
			auto RHIUpdateTexture3D(FRHITexture* Texture, uint32 MipIndex,
			const FUpdateTextureRegion3D& Region, uint32 SourceRowPitch,
			uint32 SourceDepthPitch, FByteView SourceData) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(State && Texture && Texture->GetDimension() == ETextureDimension::Texture3D
					&& ToMetalPixelFormat(Texture->GetFormat()) != MTL::PixelFormatInvalid
					&& EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::DestinationCopy),
					"Metal volume upload requires a supported destination texture.");
				FRHITextureDesc Desc;
				Desc.Dimension = Texture->GetDimension();
				Desc.Extent = FIntPoint(Texture->GetSizeX(), Texture->GetSizeY());
				Desc.Depth = static_cast<uint16>(Texture->GetSizeZ());
				Desc.ArraySize = Texture->GetArraySize();
				Desc.NumMips = Texture->GetNumMips();
				Desc.NumSamples = Texture->GetNumSamples();
				Desc.Format = Texture->GetFormat();
				requiref(ValidateTexture3DUpdate(Desc, MipIndex, Region,
					SourceRowPitch, SourceDepthPitch).has_value(),
					"Invalid Metal volume upload region.");
				const NS::UInteger BytesPerTexel = MetalBytesPerTexel(Texture->GetFormat());
				const uint64 SourceStart = static_cast<uint64>(Region.SrcZ) * SourceDepthPitch
					+ static_cast<uint64>(Region.SrcY) * SourceRowPitch
					+ static_cast<uint64>(Region.SrcX) * BytesPerTexel;
				const uint64 RequiredBytes = SourceStart
					+ static_cast<uint64>(Region.Depth - 1) * SourceDepthPitch
					+ static_cast<uint64>(Region.Height - 1) * SourceRowPitch
					+ static_cast<uint64>(Region.Width) * BytesPerTexel;
				requiref(RequiredBytes <= SourceData.size(),
					"Metal volume upload source data is incomplete.");
				const NS::UInteger RowPitch = static_cast<NS::UInteger>(Region.Width) * BytesPerTexel;
				const NS::UInteger ImagePitch = RowPitch * Region.Height;
				const NS::UInteger ByteCount = ImagePitch * Region.Depth;
				auto StagingOwner = NS::TransferPtr(State->Queue->device()->newBuffer(
					ByteCount, MTL::ResourceStorageModeShared));
				auto* Staging = StagingOwner.get();
				requiref(Staging != nullptr, "Metal volume upload staging allocation failed.");
				auto* TargetBytes = static_cast<std::byte*>(Staging->contents());
				for (uint32 Z = 0; Z < Region.Depth; ++Z)
					for (uint32 Row = 0; Row < Region.Height; ++Row)
						std::memcpy(TargetBytes + static_cast<size_t>(Z) * ImagePitch
							+ static_cast<size_t>(Row) * RowPitch,
							SourceData.data() + SourceStart
								+ static_cast<uint64>(Z) * SourceDepthPitch
								+ static_cast<uint64>(Row) * SourceRowPitch,
							RowPitch);
				if (!Active) RHISubmitCommands();
				auto Command = Active ? Active->Command : NS::RetainPtr(State->Queue->commandBuffer());
				requiref(static_cast<bool>(Command), "Metal volume upload command allocation failed.");
				auto Encoder = NS::RetainPtr(Command->blitCommandEncoder());
				requiref(static_cast<bool>(Encoder), "Metal volume upload encoder creation failed.");
				auto* Native = static_cast<FMetalTexture*>(Texture)->GetHandle();
				Encoder->copyFromBuffer(
					Staging,
					0,
					RowPitch,
					ImagePitch,
					MTL::Size::Make(Region.Width, Region.Height, Region.Depth),
					Native,
					0,
					MipIndex,
					MTL::Origin::Make(Region.DestX, Region.DestY, Region.DestZ));
				Encoder->endEncoding();
				static_cast<FMetalTexture*>(Texture)->ApplyAccess(
					{ERHITextureAspect::Color, MipIndex, 1, 0, 1},
					EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::Storage)
						? ERHIAccess::GraphicsShaderReadWrite : ERHIAccess::GraphicsShaderRead);
				if (Active)
				{
					Active->NativeResources.push_back(NS::RetainPtr(Staging));
					Active->NativeResources.push_back(NS::RetainPtr(Native));
					return;
				}
				auto SharedState = State;
				auto Owners = std::make_shared<FMetalSubmissionOwners>();
				Owners->NativeResources.push_back(NS::RetainPtr(Native));
				Owners->NativeResources.push_back(StagingOwner);
				Command->addCompletedHandler(MTL::HandlerFunction(
					[SharedState, Owners](MTL::CommandBuffer*) {
					const FMetalAutoreleasePool Pool;
					(void)Owners;
					Owners->Release();
					std::lock_guard Lock(SharedState->Mutex);
					--SharedState->PendingCallbacks;
					SharedState->Completion.notify_all();
				}));
				{
					std::lock_guard Lock(State->Mutex);
					++State->PendingCallbacks;
				}
				Command->commit();
			}
			auto RHIReadTexture2D(FRHITexture* Texture, uint32 MipIndex,
			uint32 ArraySlice, FByteBuffer& OutData) -> bool override
			{
				const FMetalAutoreleasePool Pool;
				OutData.clear();
				if (!State || Active || !Texture || MipIndex >= Texture->GetNumMips()
					|| ArraySlice >= Texture->GetArraySize()
					|| (Texture->GetDimension() != ETextureDimension::Texture2D
						&& Texture->GetDimension() != ETextureDimension::Texture2DArray
						&& Texture->GetDimension() != ETextureDimension::TextureCube
						&& Texture->GetDimension() != ETextureDimension::TextureCubeArray)
					|| ToMetalPixelFormat(Texture->GetFormat()) == MTL::PixelFormatInvalid
					|| !EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::CPUReadback))
					return false;
				RHISubmitCommands();
				const NS::UInteger Width = std::max(1u, Texture->GetSizeX() >> MipIndex);
				const NS::UInteger Height = std::max(1u, Texture->GetSizeY() >> MipIndex);
				const auto Layout = GetPixelFormatLayout(Texture->GetFormat(), Width, Height);
				const NS::UInteger RowPitch = Layout.RowPitch;
				const NS::UInteger ByteCount = Layout.DataSize;
				auto ReadbackOwner = NS::TransferPtr(State->Queue->device()->newBuffer(
					ByteCount, MTL::ResourceStorageModeShared));
				auto* Readback = ReadbackOwner.get();
				auto Command = NS::RetainPtr(State->Queue->commandBuffer());
				if (!Readback || !Command) return false;
				auto Encoder = NS::RetainPtr(Command->blitCommandEncoder());
				if (!Encoder) return false;
				Encoder->copyFromTexture(
					static_cast<FMetalTexture*>(Texture)->GetHandle(),
					ArraySlice,
					MipIndex,
					MTL::Origin::Make(0, 0, 0),
					MTL::Size::Make(Width, Height, 1),
					Readback,
					0,
					RowPitch,
					ByteCount);
				Encoder->endEncoding();
				Command->commit();
				Command->waitUntilCompleted();
				if (Command->status() != MTL::CommandBufferStatusCompleted) return false;
				const auto* Bytes = static_cast<const std::byte*>(Readback->contents());
				if (!Bytes) return false;
				OutData.assign(Bytes, Bytes + ByteCount);
				return true;
			}
			auto RHIEnqueueTextureReadback(FRHITexture* Texture, uint32 MipIndex,
				uint32 ArraySlice, std::shared_ptr<FRHITextureReadback> Request) -> void override
			{
				const FMetalAutoreleasePool Pool;
				if (!Request || Request->GetState() != ERHITextureReadbackState::Pending)
					return;
				if (!State || !Texture || MipIndex >= Texture->GetNumMips()
					|| ArraySlice >= Texture->GetArraySize()
					|| (Texture->GetDimension() != ETextureDimension::Texture2D
						&& Texture->GetDimension() != ETextureDimension::Texture2DArray
						&& Texture->GetDimension() != ETextureDimension::TextureCube
						&& Texture->GetDimension() != ETextureDimension::TextureCubeArray)
					|| ToMetalPixelFormat(Texture->GetFormat()) == MTL::PixelFormatInvalid
					|| !EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::CPUReadback))
				{
					Request->Fail();
					return;
				}
				if (!Active) RHISubmitCommands();
				const NS::UInteger Width = std::max(1u, Texture->GetSizeX() >> MipIndex);
				const NS::UInteger Height = std::max(1u, Texture->GetSizeY() >> MipIndex);
				const auto Layout = GetPixelFormatLayout(Texture->GetFormat(), Width, Height);
				const NS::UInteger RowPitch = Layout.RowPitch;
				const NS::UInteger ByteCount = Layout.DataSize;
				auto ReadbackOwner = NS::TransferPtr(State->Queue->device()->newBuffer(
					ByteCount, MTL::ResourceStorageModeShared));
				auto* Readback = ReadbackOwner.get();
				auto Command = Active ? Active->Command : NS::RetainPtr(State->Queue->commandBuffer());
				auto Encoder = NS::RetainPtr(Command ? Command->blitCommandEncoder() : nullptr);
				if (!Readback || !Encoder)
				{
					if (Encoder) Encoder->endEncoding();
					Request->Fail();
					return;
				}
				auto* Native = static_cast<FMetalTexture*>(Texture)->GetHandle();
				Encoder->copyFromTexture(
					Native,
					ArraySlice,
					MipIndex,
					MTL::Origin::Make(0, 0, 0),
					MTL::Size::Make(Width, Height, 1),
					Readback,
					0,
					RowPitch,
					ByteCount);
				Encoder->endEncoding();
				if (Active)
				{
					Active->NativeResources.push_back(NS::RetainPtr(Native));
					Active->NativeResources.push_back(NS::RetainPtr(Readback));
					Active->Readbacks.push_back({ReadbackOwner, ByteCount, std::move(Request)});
					return;
				}
				auto SharedState = State;
				auto Owners = std::make_shared<FMetalSubmissionOwners>();
				Owners->NativeResources.push_back(NS::RetainPtr(Native));
				Owners->Readbacks.push_back({ReadbackOwner, ByteCount, Request});
				Command->addCompletedHandler(MTL::HandlerFunction(
					[SharedState, Owners](MTL::CommandBuffer* Completed) {
					const FMetalAutoreleasePool Pool;
					const auto& Readback = Owners->Readbacks.front();
					if (Completed->status() != MTL::CommandBufferStatusCompleted)
						Readback.Request->Fail();
					else if (const auto* Bytes = static_cast<const std::byte*>(Readback.Buffer->contents()))
						Readback.Request->Complete(FByteBuffer(Bytes, Bytes + Readback.ByteCount));
					else Readback.Request->Fail();
					Owners->Release();
					std::lock_guard Lock(SharedState->Mutex);
					--SharedState->PendingCallbacks;
					SharedState->Completion.notify_all();
				}));
				{
					std::lock_guard Lock(State->Mutex);
					++State->PendingCallbacks;
				}
				Command->commit();
			}
			auto RHIAcquireBackBuffer(FRHITexture*) -> void override { Unsupported(); }
			auto RHIBlockUntilGPUIdle() -> void override
			{
				RHISubmitCommands();
				std::unique_lock Lock(State->Mutex);
				State->Completion.wait(Lock,
					[this] { return State->PendingCallbacks == 0; });
			}
			auto RHIPushConstants(EShaderStageFlags Stages, uint32 Offset,
			uint32 Size, const void* Data) -> void override
			{
				if (RenderEncoder)
				{
					requiref(GraphicsPipeline && Data && Size
						&& Offset % 4 == 0 && Size % 4 == 0
						&& uint64(Offset) + Size <= 65536
						&& Stages != EShaderStageFlags::None
						&& (static_cast<uint32>(Stages)
							& ~static_cast<uint32>(EShaderStageFlags::Vertex
								| EShaderStageFlags::Fragment)) == 0,
						"Invalid Metal graphics push constant update.");
					for (uint32 StageIndex = 0; StageIndex < 2; ++StageIndex)
					{
						const auto Stage = StageIndex == 0
							? EShaderStageFlags::Vertex
							: EShaderStageFlags::Fragment;
						if (!EnumHasAnyFlags(Stages, Stage)) continue;
						for (uint32 Byte = Offset; Byte < Offset + Size; Byte += 4)
							requiref(std::ranges::any_of(
								GraphicsPipeline->GetLayout().PushConstantRanges,
								[&](const auto& Range) {
									return EnumHasAnyFlags(Range.StageFlags, Stage)
										&& Byte >= Range.Offset
										&& Byte + 4 <= uint64(Range.Offset) + Range.Size;
								}), "Metal graphics push constant update exceeds its layout.");
						auto& Bytes = GraphicsPushConstants[StageIndex];
						auto& Written = GraphicsPushConstantWritten[StageIndex];
						if (Bytes.size() < Offset + Size)
						{
							Bytes.resize(Offset + Size);
							Written.resize(Offset + Size);
						}
						std::memcpy(Bytes.data() + Offset, Data, Size);
						std::fill(Written.begin() + Offset,
							Written.begin() + Offset + Size, 1);
					}
					return;
				}
				auto* Pipeline = static_cast<FMetalComputePipelineState*>(
					ComputePipeline.GetReference());
				requiref(Active && Pipeline && Stages == EShaderStageFlags::Compute
					&& Data && Size && Offset % 4 == 0 && Size % 4 == 0
					&& uint64(Offset) + Size <= 65536,
					"Invalid Metal compute push constant update.");
				const auto& Ranges = Pipeline->GetLayout().PushConstantRanges;
				for (uint32 Byte = Offset; Byte < Offset + Size; Byte += 4)
					requiref(std::ranges::any_of(Ranges, [&](const auto& Range) {
						return Byte >= Range.Offset
							&& Byte + 4 <= uint64(Range.Offset) + Range.Size;
					}), "Metal compute push constant update exceeds its layout.");
				if (ComputePushConstants.size() < Offset + Size)
				{
					ComputePushConstants.resize(Offset + Size);
					ComputePushConstantWritten.resize(Offset + Size);
				}
				std::memcpy(ComputePushConstants.data() + Offset, Data, Size);
				std::fill(ComputePushConstantWritten.begin() + Offset,
					ComputePushConstantWritten.begin() + Offset + Size, 1);
			}
			auto RHISetShaderParameters(FRHIShader* Shader,
			const std::span<const FRHIShaderParameterResource>& Parameters) -> void override
			{
				if (RenderEncoder)
				{
					auto* Pipeline = GraphicsPipeline.GetReference();
					const EShaderStageFlags Stage = Pipeline
						&& Shader == Pipeline->GetVertexShader()
						? EShaderStageFlags::Vertex : EShaderStageFlags::Fragment;
					requiref(Pipeline && (Shader == Pipeline->GetVertexShader()
						|| Shader == Pipeline->GetFragmentShader())
						&& ValidateShaderParameterUpdate(Pipeline->GetLayout(),
							Stage, Parameters).has_value(),
						"Invalid Metal graphics shader parameter update.");
					for (const auto& Parameter : Parameters)
					{
						requiref(Parameter.Resource,
							"Metal graphics parameter requires a resource.");
						auto Existing = std::ranges::find_if(GraphicsParameters,
							[&](const auto& Item) {
								return Item.SetIndex == Parameter.SetIndex
									&& Item.BindingIndex == Parameter.BindingIndex
									&& Item.ArrayElement == Parameter.ArrayElement;
							});
						CaptureBufferSnapshot(Parameter, GraphicsBufferSnapshots);
						if (Existing == GraphicsParameters.end())
							GraphicsParameters.push_back(Parameter);
						else *Existing = Parameter;
					}
					std::ranges::sort(GraphicsParameters, {}, [](const auto& Item) {
						return std::tuple(Item.SetIndex, Item.BindingIndex,
							Item.ArrayElement);
					});
					GraphicsParameterOwners.clear();
					for (const auto& Parameter : GraphicsParameters)
						GraphicsParameterOwners.emplace_back(Parameter.Resource);
					return;
				}
				auto* Pipeline = static_cast<FMetalComputePipelineState*>(
					ComputePipeline.GetReference());
				requiref(Active && Pipeline && Shader == Pipeline->GetShader()
					&& ValidateShaderParameterUpdate(Pipeline->GetLayout(),
						EShaderStageFlags::Compute, Parameters).has_value(),
					"Invalid Metal compute shader parameter update.");
				for (const auto& Parameter : Parameters)
				{
					requiref(Parameter.Resource != nullptr,
						"Metal compute parameter requires a resource.");
					auto Existing = std::ranges::find_if(ComputeParameters,
						[&](const auto& Item) {
							return Item.SetIndex == Parameter.SetIndex
								&& Item.BindingIndex == Parameter.BindingIndex
								&& Item.ArrayElement == Parameter.ArrayElement;
						});
					CaptureBufferSnapshot(Parameter, ComputeBufferSnapshots);
					if (Existing == ComputeParameters.end())
						ComputeParameters.push_back(Parameter);
					else *Existing = Parameter;
				}
				std::ranges::sort(ComputeParameters, {}, [](const auto& Item) {
					return std::tuple(Item.SetIndex, Item.BindingIndex,
						Item.ArrayElement);
				});
				ComputeParameterOwners.clear();
				for (const auto& Parameter : ComputeParameters)
					ComputeParameterOwners.emplace_back(Parameter.Resource);
			}
			auto RHIDispatch(uint32 X, uint32 Y, uint32 Z) -> void override
				{ EncodeComputeDispatch(X, Y, Z, nullptr, 0); }
			auto RHIDispatchIndirect(FRHIBuffer* ArgumentBuffer,
				uint64 Offset) -> void override
			{
				requiref(ArgumentBuffer
					&& EnumHasAnyFlags(ArgumentBuffer->GetUsage(),
						EBufferUsageFlags::DrawIndirect)
					&& Offset <= ArgumentBuffer->GetSize()
					&& sizeof(FRHIDispatchIndirectArguments)
						<= ArgumentBuffer->GetSize() - Offset,
					"Metal indirect dispatch requires a valid argument buffer.");
				EncodeComputeDispatch(0, 0, 0, ArgumentBuffer, Offset);
			}
			auto EncodeComputeDispatch(uint32 X, uint32 Y, uint32 Z,
				FRHIBuffer* ArgumentBuffer, uint64 Offset) -> void
			{
				const FMetalAutoreleasePool Pool;
				auto* Pipeline = static_cast<FMetalComputePipelineState*>(
					ComputePipeline.GetReference());
				requiref(Active && !RenderEncoder && Pipeline
					&& (ArgumentBuffer || (X && Y && Z))
					&& ValidateShaderBindingCompleteness(Pipeline->GetLayout(),
						ComputeParameters).has_value(),
					"Invalid Metal compute dispatch state.");
				auto Encoder = NS::RetainPtr(Active->Command->computeCommandEncoder());
				requiref(static_cast<bool>(Encoder), "Metal compute encoder creation failed.");
				Encoder->setComputePipelineState(Pipeline->GetPipeline());
				const auto& PushRanges = Pipeline->GetLayout().PushConstantRanges;
				if (!PushRanges.empty())
				{
					uint32 End = 0;
					for (const auto& Range : PushRanges)
					{
						End = std::max(End, Range.Offset + Range.Size);
						requiref(ComputePushConstantWritten.size()
							>= uint64(Range.Offset) + Range.Size
							&& std::ranges::all_of(
								std::span(ComputePushConstantWritten).subspan(
									Range.Offset, Range.Size),
								[](uint8 Written) { return Written != 0; }),
							"Metal compute push constants are incomplete.");
					}
					auto ConstantsOwner = NS::TransferPtr(State->Queue->device()->newBuffer(
						ComputePushConstants.data(), End, MTL::ResourceStorageModeShared));
					auto* Constants = ConstantsOwner.get();
					requiref(Constants != nullptr,
						"Metal compute push constant allocation failed.");
					Encoder->setBuffer(Constants, 0, Pipeline->GetShader()->GetMetalPushConstantBufferSlot());
					Active->NativeResources.push_back(NS::RetainPtr(Constants));
				}
				const auto& Map = Pipeline->GetShader()->GetMetalBindings();
				for (const auto& Parameter : ComputeParameters)
				{
					auto Binding = std::ranges::find_if(Map, [&](const auto& Item) {
						return Item.SetIndex == Parameter.SetIndex
							&& Item.BindingIndex == Parameter.BindingIndex
							&& Item.Type == (Parameter.Type == ERHIBindingType::UniformBufferDynamic
								? ERHIBindingType::UniformBuffer : Parameter.Type);
					});
					requiref(Binding != Map.end()
						&& Parameter.ArrayElement < Binding->Count,
						"Metal compute binding is missing its native slot.");
					const uint32 Slot = Binding->Slot + Parameter.ArrayElement;
					if (Parameter.Type == ERHIBindingType::StorageBuffer
						|| Parameter.Type == ERHIBindingType::UniformBuffer
						|| Parameter.Type == ERHIBindingType::UniformBufferDynamic)
					{
						requiref(Parameter.Resource->GetResourceType()
							== ERHIResourceType::BufferView,
							"Metal compute buffer binding requires a buffer view.");
						auto* View = static_cast<FRHIBufferView*>(Parameter.Resource);
						const auto Buffer = ResolveBufferBinding(View, Parameter, ComputeBufferSnapshots);
						const bool bStorage = Parameter.Type
							== ERHIBindingType::StorageBuffer;
						requiref(bStorage
								? View->GetDesc().Type == ERHIBufferViewType::StructuredStorage
									|| View->GetDesc().Type == ERHIBufferViewType::ByteAddressStorage
								: View->GetDesc().Type == ERHIBufferViewType::Uniform,
							"Metal compute buffer view does not match its binding type.");
						requiref(Parameter.Type == ERHIBindingType::UniformBufferDynamic
							|| Parameter.Offset == 0,
							"Metal static compute binding cannot use a dynamic offset.");
						const uint64 Offset = View->GetDesc().Offset
							+ (Parameter.Type == ERHIBindingType::UniformBufferDynamic
								? Parameter.Offset : 0);
						requiref(Offset <= Buffer.Size
							&& View->GetDesc().Size <= Buffer.Size - Offset,
							"Metal compute buffer range exceeds its allocation.");
						Encoder->setBuffer(Buffer.Handle, Offset, Slot);
					}
					else if (Parameter.Type == ERHIBindingType::Texture
						|| Parameter.Type == ERHIBindingType::StorageImage)
					{
						requiref(Parameter.Resource->GetResourceType()
							== ERHIResourceType::TextureView,
							"Metal compute texture binding requires a texture view.");
						auto* View = dynamic_cast<FMetalTextureView*>(
							static_cast<FRHITextureView*>(Parameter.Resource));
						requiref(View && View->GetDesc().Usage ==
							(Parameter.Type == ERHIBindingType::Texture
								? ERHITextureViewUsage::Sampled
								: ERHITextureViewUsage::Storage),
							"Metal compute texture view usage is incompatible.");
						Encoder->setTexture(View->GetHandle(), Slot);
						Active->NativeResources.push_back(NS::RetainPtr(View->GetHandle()));
					}
					else if (Parameter.Type == ERHIBindingType::Sampler)
					{
						requiref(Parameter.Resource->GetResourceType()
							== ERHIResourceType::Sampler,
							"Metal compute sampler binding requires a sampler.");
						auto* Sampler = dynamic_cast<FMetalSampler*>(
							static_cast<FRHISampler*>(Parameter.Resource));
						requiref(Sampler, "Metal compute sampler belongs to another backend.");
						Encoder->setSamplerState(Sampler->GetHandle(), Slot);
					}
					else Unsupported();
					Active->ResourceOwners.emplace_back(Parameter.Resource);
				}
				Active->ResourceOwners.emplace_back(Pipeline);
				const auto Group = Pipeline->GetShader()->GetComputeThreadGroupSize();
				if (ArgumentBuffer)
				{
					auto* Buffer = static_cast<FMetalBuffer*>(ArgumentBuffer);
					Encoder->dispatchThreadgroups(
						Buffer->GetHandle(),
						Offset,
						MTL::Size::Make(Group[0], Group[1], Group[2]));
					Active->NativeResources.push_back(NS::RetainPtr(Buffer->GetHandle()));
					Active->ResourceOwners.emplace_back(ArgumentBuffer);
				}
				else Encoder->dispatchThreadgroups(
					MTL::Size::Make(X, Y, Z),
					MTL::Size::Make(Group[0], Group[1], Group[2]));
				Encoder->endEncoding();
			}
			auto RHIDraw(const FRHIDrawArguments& Args) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(RenderEncoder && GraphicsPipeline && Args.VertexCount
					&& Args.InstanceCount
					&& (BoundVertexStreams
						& GraphicsPipeline->GetRequiredVertexStreams())
						== GraphicsPipeline->GetRequiredVertexStreams(),
					"Metal draw requires a render pipeline and nonempty arguments.");
				BindGraphicsParameters();
				RenderEncoder->drawPrimitives(
					GraphicsPipeline->GetPrimitiveType(),
					Args.FirstVertex,
					Args.VertexCount,
					Args.InstanceCount,
					Args.FirstInstance);
			}
			auto RHIDrawIndexed(const FRHIDrawIndexedArguments& Args) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(RenderEncoder && GraphicsPipeline && IndexBuffer
					&& Args.IndexCount && Args.InstanceCount
					&& (BoundVertexStreams
						& GraphicsPipeline->GetRequiredVertexStreams())
						== GraphicsPipeline->GetRequiredVertexStreams(),
					"Metal indexed draw requires complete pipeline and buffer state.");
				const uint64 ByteOffset = uint64(IndexBufferOffset)
					+ uint64(Args.FirstIndex) * IndexBuffer->GetStride();
				requiref(ByteOffset <= IndexBuffer->GetSize()
					&& uint64(Args.IndexCount) * IndexBuffer->GetStride()
						<= IndexBuffer->GetSize() - ByteOffset,
					"Metal indexed draw exceeds its index buffer.");
				BindGraphicsParameters();
				RenderEncoder->drawIndexedPrimitives(GraphicsPipeline->GetPrimitiveType(), Args.IndexCount, IndexBuffer->GetStride() == 2
						? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32, static_cast<FMetalBuffer*>(
						IndexBuffer.GetReference())->GetHandle(), ByteOffset, Args.InstanceCount, Args.VertexOffset, Args.FirstInstance);
			}
			auto RHIDrawIndirect(FRHIBuffer* ArgumentBuffer,
				uint64 Offset) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(RenderEncoder && GraphicsPipeline && ArgumentBuffer
					&& EnumHasAnyFlags(ArgumentBuffer->GetUsage(),
						EBufferUsageFlags::DrawIndirect)
					&& Offset <= ArgumentBuffer->GetSize()
					&& sizeof(FRHIDrawIndirectArguments)
						<= ArgumentBuffer->GetSize() - Offset
					&& (BoundVertexStreams
						& GraphicsPipeline->GetRequiredVertexStreams())
						== GraphicsPipeline->GetRequiredVertexStreams(),
					"Metal indirect draw requires complete pipeline and arguments.");
				auto* Buffer = static_cast<FMetalBuffer*>(ArgumentBuffer);
				BindGraphicsParameters();
				RenderEncoder->drawPrimitives(GraphicsPipeline->GetPrimitiveType(), Buffer->GetHandle(), Offset);
				Active->NativeResources.push_back(NS::RetainPtr(Buffer->GetHandle()));
				Active->ResourceOwners.emplace_back(ArgumentBuffer);
			}
			auto RHIDrawIndexedIndirect(FRHIBuffer* ArgumentBuffer,
				uint64 Offset) -> void override
			{
				const FMetalAutoreleasePool Pool;
				requiref(RenderEncoder && GraphicsPipeline && IndexBuffer
					&& ArgumentBuffer
					&& EnumHasAnyFlags(ArgumentBuffer->GetUsage(),
						EBufferUsageFlags::DrawIndirect)
					&& Offset <= ArgumentBuffer->GetSize()
					&& sizeof(FRHIDrawIndexedIndirectArguments)
						<= ArgumentBuffer->GetSize() - Offset
					&& (BoundVertexStreams
						& GraphicsPipeline->GetRequiredVertexStreams())
						== GraphicsPipeline->GetRequiredVertexStreams(),
					"Metal indexed indirect draw requires complete state.");
				auto* Buffer = static_cast<FMetalBuffer*>(ArgumentBuffer);
				BindGraphicsParameters();
				RenderEncoder->drawIndexedPrimitives(GraphicsPipeline->GetPrimitiveType(), IndexBuffer->GetStride() == 2
						? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32, static_cast<FMetalBuffer*>(
						IndexBuffer.GetReference())->GetHandle(), IndexBufferOffset, Buffer->GetHandle(), Offset);
				Active->NativeResources.push_back(NS::RetainPtr(Buffer->GetHandle()));
				Active->ResourceOwners.emplace_back(ArgumentBuffer);
			}
		private:
			auto RecordTimingSample(FMetalGPUTimingQuery& Query, bool bEnd) -> void
			{
				const FMetalAutoreleasePool Pool;
				requiref(State && !RenderEncoder, "Metal timing samples require a closed encoder.");
				const bool bStandalone = !Active;
				if (bStandalone)
				{
					FMetalPendingSubmission Submission;
					Submission.Command = NS::RetainPtr(State->Queue->commandBuffer());
					require(Submission.Command);
					Active.emplace(std::move(Submission));
				}
				auto Desc = NS::TransferPtr(MTL::BlitPassDescriptor::alloc()->init());
				auto* Attachment = Desc->sampleBufferAttachments()->object(0);
				Attachment->setSampleBuffer(Query.Pool->Buffer.get());
				Attachment->setStartOfEncoderSampleIndex(Query.Slot * 2 + (bEnd ? 1 : 0));
				Attachment->setEndOfEncoderSampleIndex(MTL::CounterDontSample);
				auto Encoder = NS::RetainPtr(Active->Command->blitCommandEncoder(Desc.get()));
				require(Encoder);
				// Metal elides empty encoders, including their timestamp attachments.
				Encoder->fillBuffer(Query.Pool->Marker.get(), NS::Range::Make(0, 4), bEnd ? 1 : 0);
				Encoder->endEncoding();
				Active->TimingSamples.push_back({TRefCountPtr<FMetalGPUTimingQuery>(&Query), bEnd});
				if (bStandalone)
				{
					std::lock_guard Lock(State->Mutex);
					Active->Producer = State->Timeline->Reserve();
					require(Active->Producer);
					Pending.push_back(std::move(*Active));
					Active.reset();
				}
			}
			struct FBufferBinding
			{
				MTL::Buffer* Handle;
				uint64 Size;
			};

			static auto CaptureBufferSnapshot(const FRHIShaderParameterResource& Parameter,
				FBoundBufferSnapshots& Snapshots) -> void
			{
				const auto Key = std::tuple(Parameter.SetIndex, Parameter.BindingIndex, Parameter.ArrayElement);
				if (Parameter.Resource->GetResourceType() == ERHIResourceType::BufferView)
				{
					auto* Buffer = static_cast<FRHIBufferView*>(Parameter.Resource)->GetBuffer();
					if (IsCPUAuthoredBuffer(Buffer))
					{
						Snapshots[Key] = FRHIDeferredBufferBackend::ResolveSnapshot(*Buffer);
						return;
					}
				}
				Snapshots.erase(Key);
			}

			auto ResolveBufferBinding(FRHIBufferView* View,
				const FRHIShaderParameterResource& Parameter,
				const FBoundBufferSnapshots& Snapshots) -> FBufferBinding
			{
				auto* Logical = View->GetBuffer();
				if (IsCPUAuthoredBuffer(Logical))
				{
					// Reuse one immutable native copy for each ordered content version.
					const auto& Snapshot = Snapshots.at(std::tuple(
						Parameter.SetIndex, Parameter.BindingIndex, Parameter.ArrayElement));
					auto Backing = std::static_pointer_cast<FMetalDeferredBacking>(
						FRHIDeferredBufferBackend::GetBacking(*Snapshot, State.get()));
					if (!Backing)
					{
						const auto Data = Snapshot->GetData();
						requiref(!Data.empty(), "Metal deferred buffer has no data.");
						Backing = std::make_shared<FMetalDeferredBacking>();
						Backing->Snapshot = Snapshot;
						Backing->Handle = NS::TransferPtr(State->Queue->device()->newBuffer(
							Data.data(), Data.size(), MTL::ResourceStorageModeShared));
						requiref(static_cast<bool>(Backing->Handle),
							"Metal deferred buffer allocation failed.");
						FRHIDeferredBufferBackend::SetBacking(
							*Snapshot, State.get(), Backing);
					}
					Active->StorageOwners.push_back(Backing);
					Active->NativeResources.push_back(NS::RetainPtr(Backing->Handle.get()));
					return {Backing->Handle.get(), Snapshot->GetData().size()};
				}
				auto* Buffer = dynamic_cast<FMetalBuffer*>(Logical);
				requiref(Buffer && Buffer->GetHandle(),
					"Metal buffer binding has no native buffer.");
				Active->NativeResources.push_back(NS::RetainPtr(Buffer->GetHandle()));
				return {Buffer->GetHandle(), Buffer->GetSize()};
			}

			auto BindGraphicsParameters() -> void
			{
				const auto& Layout = GraphicsPipeline->GetLayout();
				requiref(ValidateShaderBindingCompleteness(Layout,
					GraphicsParameters).has_value(),
					"Metal graphics shader bindings are incomplete.");
				for (uint32 StageIndex = 0; StageIndex < 2; ++StageIndex)
				{
					const auto Stage = StageIndex == 0
						? EShaderStageFlags::Vertex : EShaderStageFlags::Fragment;
					auto* Shader = StageIndex == 0
						? GraphicsPipeline->GetVertexShader()
						: GraphicsPipeline->GetFragmentShader();
					if (Shader->GetMetalPushConstantBufferSlot() == UINT32_MAX)
						continue;
					uint32 End = 0;
					for (const auto& Range : Layout.PushConstantRanges)
					{
						if (!EnumHasAnyFlags(Range.StageFlags, Stage)) continue;
						End = std::max(End, Range.Offset + Range.Size);
						const auto& Written = GraphicsPushConstantWritten[StageIndex];
						requiref(Written.size() >= uint64(Range.Offset) + Range.Size
							&& std::ranges::all_of(std::span(Written).subspan(
								Range.Offset, Range.Size),
								[](uint8 Value) { return Value != 0; }),
							"Metal graphics push constants are incomplete.");
					}
					requiref(End != 0,
						"Metal graphics shader push slot lacks a layout range.");
					auto ConstantsOwner = NS::TransferPtr(State->Queue->device()->newBuffer(
						GraphicsPushConstants[StageIndex].data(), End, MTL::ResourceStorageModeShared));
					auto* Constants = ConstantsOwner.get();
					requiref(Constants != nullptr,
						"Metal graphics push constant allocation failed.");
					if (StageIndex == 0)
						RenderEncoder->setVertexBuffer(Constants, 0, Shader->GetMetalPushConstantBufferSlot());
					else RenderEncoder->setFragmentBuffer(Constants, 0, Shader->GetMetalPushConstantBufferSlot());
					Active->NativeResources.push_back(NS::RetainPtr(Constants));
				}
				for (const auto& Parameter : GraphicsParameters)
				{
					const auto& Set = Layout.BindingLayouts[Parameter.SetIndex];
					const auto Binding = std::ranges::find(Set.BindingLayouts,
						Parameter.BindingIndex, &FBindingLayoutItem::Slot);
					requiref(Binding != Set.BindingLayouts.end(),
						"Metal graphics binding is absent from its layout.");
					for (const auto Stage : {EShaderStageFlags::Vertex,
						EShaderStageFlags::Fragment})
					{
						if (!EnumHasAnyFlags(Binding->StageFlags, Stage)) continue;
						auto* Shader = Stage == EShaderStageFlags::Vertex
							? GraphicsPipeline->GetVertexShader()
							: GraphicsPipeline->GetFragmentShader();
						const auto Map = Shader->GetMetalBindings();
						const auto Native = std::ranges::find_if(Map,
							[&](const auto& Item) {
								return Item.SetIndex == Parameter.SetIndex
									&& Item.BindingIndex == Parameter.BindingIndex
									&& Item.Type == (Parameter.Type == ERHIBindingType::UniformBufferDynamic
										? ERHIBindingType::UniformBuffer : Parameter.Type);
							});
						requiref(Native != Map.end()
							&& Parameter.ArrayElement < Native->Count,
							"Metal graphics binding has no native slot.");
						const uint32 Slot = Native->Slot + Parameter.ArrayElement;
						if (Parameter.Type == ERHIBindingType::Texture
							|| Parameter.Type == ERHIBindingType::StorageImage)
						{
							requiref(Parameter.Resource->GetResourceType()
								== ERHIResourceType::TextureView,
								"Metal graphics texture binding requires a view.");
							auto* View = dynamic_cast<FMetalTextureView*>(
								static_cast<FRHITextureView*>(Parameter.Resource));
							requiref(View && View->GetDesc().Usage ==
								(Parameter.Type == ERHIBindingType::Texture
									? ERHITextureViewUsage::Sampled
									: ERHITextureViewUsage::Storage),
								"Metal graphics texture view usage is incompatible.");
							if (Stage == EShaderStageFlags::Vertex)
								RenderEncoder->setVertexTexture(View->GetHandle(), Slot);
							else RenderEncoder->setFragmentTexture(View->GetHandle(), Slot);
							Active->NativeResources.push_back(NS::RetainPtr(View->GetHandle()));
						}
						else if (Parameter.Type == ERHIBindingType::Sampler)
						{
							requiref(Parameter.Resource->GetResourceType()
								== ERHIResourceType::Sampler,
								"Metal graphics sampler binding requires a sampler.");
							auto* Sampler = dynamic_cast<FMetalSampler*>(
								static_cast<FRHISampler*>(Parameter.Resource));
							requiref(Sampler,
								"Metal graphics sampler belongs to another backend.");
							if (Stage == EShaderStageFlags::Vertex)
								RenderEncoder->setVertexSamplerState(Sampler->GetHandle(), Slot);
							else RenderEncoder->setFragmentSamplerState(Sampler->GetHandle(), Slot);
						}
						else if (Parameter.Type == ERHIBindingType::UniformBuffer
							|| Parameter.Type == ERHIBindingType::UniformBufferDynamic
							|| Parameter.Type == ERHIBindingType::StorageBuffer)
						{
							requiref(Parameter.Resource->GetResourceType()
								== ERHIResourceType::BufferView,
								"Metal graphics buffer binding requires a view.");
							auto* View = static_cast<FRHIBufferView*>(Parameter.Resource);
							const auto Buffer = ResolveBufferBinding(View, Parameter, GraphicsBufferSnapshots);
							const bool bStorage = Parameter.Type
								== ERHIBindingType::StorageBuffer;
							requiref(bStorage
									? View->GetDesc().Type == ERHIBufferViewType::StructuredStorage
										|| View->GetDesc().Type == ERHIBufferViewType::ByteAddressStorage
									: View->GetDesc().Type == ERHIBufferViewType::Uniform,
								"Metal graphics buffer view type is incompatible.");
							requiref(Parameter.Type == ERHIBindingType::UniformBufferDynamic
								|| Parameter.Offset == 0,
								"Metal static graphics binding cannot use a dynamic offset.");
							const uint64 Offset = View->GetDesc().Offset
								+ (Parameter.Type == ERHIBindingType::UniformBufferDynamic
									? Parameter.Offset : 0);
							requiref(Offset <= Buffer.Size
								&& View->GetDesc().Size <= Buffer.Size - Offset,
								"Metal graphics buffer range exceeds its allocation.");
							if (Stage == EShaderStageFlags::Vertex)
								RenderEncoder->setVertexBuffer(Buffer.Handle, Offset, Slot);
							else RenderEncoder->setFragmentBuffer(Buffer.Handle, Offset, Slot);
						}
						else Unsupported();
					}
					Active->ResourceOwners.emplace_back(Parameter.Resource);
				}
			}
		public:
			auto CancelPending() -> void override
			{
				PendingAttachmentStates.clear();
				const FMetalAutoreleasePool Pool;
				if (!State) return;
				std::lock_guard Lock(State->Mutex);
				for (const auto& Submission : Pending)
				{
					for (const auto& Readback : Submission.Readbacks)
						Readback.Request->Cancel();
					for (const auto& Sample : Submission.TimingSamples) Sample.Query->Invalidate();
					State->Timeline->Cancel(Submission.Producer);
				}
				Pending.clear();
				if (Active)
					for (const auto& Readback : Active->Readbacks)
						Readback.Request->Cancel();
				if (Active)
					for (const auto& Sample : Active->TimingSamples) Sample.Query->Invalidate();
				for (const auto& Query : OpenTimingQueries) Query->Invalidate();
				OpenTimingQueries.clear();
				Active.reset();
				bImplicitRenderSubmission = false;
				RenderEncoder.reset();
				ComputePipeline = nullptr;
				ComputeParameters.clear();
				ComputeBufferSnapshots.clear();
				ComputeParameterOwners.clear();
				ComputePushConstants.clear();
				ComputePushConstantWritten.clear();
				GraphicsPipeline = nullptr;
				BoundVertexStreams = 0;
				IndexBuffer = nullptr;
				GraphicsParameters.clear();
				GraphicsBufferSnapshots.clear();
				GraphicsParameterOwners.clear();
				for (auto& Bytes : GraphicsPushConstants) Bytes.clear();
				for (auto& Written : GraphicsPushConstantWritten) Written.clear();
				StorageOwner.reset();
				bFrameOpen = false;
			}
		private:
			static auto Unsupported() -> void
			{
				requiref(false, "Metal command execution has not been implemented.");
			}
			std::shared_ptr<FMetalSubmissionState> State;
			std::shared_ptr<void> StorageOwner;
			std::optional<FMetalPendingSubmission> Active;
			bool bImplicitRenderSubmission = false;
			bool bFrameOpen = false;
			NS::SharedPtr<MTL::RenderCommandEncoder> RenderEncoder;
			FRHIRenderTargetLayout CurrentRenderTargetLayout;
			std::vector<FRHITextureTransition> PendingAttachmentStates;
			uint32 RenderWidth = 0;
			uint32 RenderHeight = 0;
			TRefCountPtr<FMetalGraphicsPipelineState> GraphicsPipeline;
			uint16 BoundVertexStreams = 0;
			FBufferRHIRef IndexBuffer;
			uint32 IndexBufferOffset = 0;
			std::vector<FRHIShaderParameterResource> GraphicsParameters;
			FBoundBufferSnapshots GraphicsBufferSnapshots;
			std::vector<TRefCountPtr<FRHIResource>> GraphicsParameterOwners;
			std::array<std::vector<std::byte>, 2> GraphicsPushConstants;
			std::array<std::vector<uint8>, 2> GraphicsPushConstantWritten;
			TRefCountPtr<FRHIComputePipelineState> ComputePipeline;
			std::vector<FRHIShaderParameterResource> ComputeParameters;
			FBoundBufferSnapshots ComputeBufferSnapshots;
			std::vector<TRefCountPtr<FRHIResource>> ComputeParameterOwners;
			std::vector<std::byte> ComputePushConstants;
			std::vector<uint8> ComputePushConstantWritten;
			std::vector<FMetalPendingSubmission> Pending;
			std::vector<TRefCountPtr<FMetalGPUTimingQuery>> OpenTimingQueries;
		};

	}

	auto CreateMetalCommandContext() -> std::unique_ptr<FMetalCommandContext>
	{ return std::make_unique<FMetalCommandContextImpl>(); }
}
