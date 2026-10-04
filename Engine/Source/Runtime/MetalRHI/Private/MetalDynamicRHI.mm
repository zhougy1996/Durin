#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "DynamicRHI.h"
#include "Backend/RHICompletionBackend.h"
#include "MetalBuffer.h"
#include "MetalSampler.h"
#include "MetalTexture.h"
#include "RHIContext.h"

namespace Durin
{
	namespace
	{
		auto ToMetalPixelFormat(EPixelFormat Format) -> MTLPixelFormat
		{
			switch (Format)
			{
			case EPixelFormat::R8_UNORM: return MTLPixelFormatR8Unorm;
			case EPixelFormat::RG8_UNORM: return MTLPixelFormatRG8Unorm;
			case EPixelFormat::R16_FLOAT: return MTLPixelFormatR16Float;
			case EPixelFormat::RGBA8_UNORM: return MTLPixelFormatRGBA8Unorm;
			case EPixelFormat::BGRA8_UNORM: return MTLPixelFormatBGRA8Unorm;
			case EPixelFormat::SRGBA8_UNORM: return MTLPixelFormatRGBA8Unorm_sRGB;
			case EPixelFormat::SBGRA8_UNORM: return MTLPixelFormatBGRA8Unorm_sRGB;
			case EPixelFormat::R11G11B10_FLOAT: return MTLPixelFormatRG11B10Float;
			case EPixelFormat::RGBA16_FLOAT: return MTLPixelFormatRGBA16Float;
			case EPixelFormat::RGBA32_FLOAT: return MTLPixelFormatRGBA32Float;
			case EPixelFormat::RG32_UINT: return MTLPixelFormatRG32Uint;
			default: return MTLPixelFormatInvalid;
			}
		}

		auto MetalBytesPerTexel(EPixelFormat Format) -> NSUInteger
		{ return GetPixelFormatInfo(Format).BytesPerBlock; }

		auto ToMetalAddressMode(ESamplerAddressMode Mode)
			-> std::optional<MTLSamplerAddressMode>
		{
			switch (Mode)
			{
			case ESamplerAddressMode::Repeat: return MTLSamplerAddressModeRepeat;
			case ESamplerAddressMode::MirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
			case ESamplerAddressMode::ClampToEdge: return MTLSamplerAddressModeClampToEdge;
			case ESamplerAddressMode::ClampToBorder: return MTLSamplerAddressModeClampToBorderColor;
			}
			return std::nullopt;
		}

		auto ToMetalCompareFunction(ESamplerCompareOp Op)
			-> std::optional<MTLCompareFunction>
		{
			switch (Op)
			{
			case ESamplerCompareOp::Never: return MTLCompareFunctionNever;
			case ESamplerCompareOp::Less: return MTLCompareFunctionLess;
			case ESamplerCompareOp::Equal: return MTLCompareFunctionEqual;
			case ESamplerCompareOp::LessOrEqual: return MTLCompareFunctionLessEqual;
			case ESamplerCompareOp::Greater: return MTLCompareFunctionGreater;
			case ESamplerCompareOp::NotEqual: return MTLCompareFunctionNotEqual;
			case ESamplerCompareOp::GreaterOrEqual: return MTLCompareFunctionGreaterEqual;
			case ESamplerCompareOp::Always: return MTLCompareFunctionAlways;
			}
			return std::nullopt;
		}

		struct FMetalSubmissionState
		{
			id<MTLCommandQueue> Queue = nil;
			uint64 Generation = 0;
			std::mutex Mutex;
			std::condition_variable Completion;
			std::unique_ptr<FRHIGPUQueueTimeline> Timeline;
			size_t PendingCallbacks = 0;
		};

		struct FMetalPendingSubmission
		{
			struct FReadback
			{
				id<MTLBuffer> Buffer = nil;
				NSUInteger ByteCount = 0;
				std::shared_ptr<FRHITextureReadback> Request;
			};
			id<MTLCommandBuffer> Command = nil;
			FRHIGPUSyncPointRef Producer;
			std::vector<std::shared_ptr<void>> StorageOwners;
			NSMutableArray<id<MTLResource>>* NativeResources = [NSMutableArray new];
			std::vector<FReadback> Readbacks;
		};

		class FMetalCommandContext final : public IRHICommandContext
		{
		public:
			auto Configure(std::shared_ptr<FMetalSubmissionState> InState) -> void
			{ State = std::move(InState); }
			auto RHISetReplayStorageOwner(std::shared_ptr<void> Owner) -> void override
			{
				StorageOwner = std::move(Owner);
				if (Active && StorageOwner)
					Active->StorageOwners.push_back(StorageOwner);
			}
			auto RHIBeginGPUSubmission(const FRHIGPUSubmissionDesc& Desc) -> void override
			{
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
				Submission.Command = [State->Queue commandBuffer];
				requiref(Submission.Command != nil,
					"Metal command queue could not allocate a command buffer.");
				if (StorageOwner) Submission.StorageOwners.push_back(StorageOwner);
				Active.emplace(std::move(Submission));
			}
			auto RHIEndGPUSubmission(const FRHIGPUSyncPointRef& Signal) -> void override
			{
				requiref(State && Active && Signal,
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
			auto RHIBeginFrame(const FRHIBeginFrameArgs&) -> void override { Unsupported(); }
			auto RHISubmitCommands() -> void override
			{
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
						auto Owners = std::move(Submission.StorageOwners);
						auto Readbacks = std::make_shared<std::vector<FMetalPendingSubmission::FReadback>>(
							std::move(Submission.Readbacks));
						NSArray<id<MTLResource>>* NativeResources =
							[Submission.NativeResources copy];
						[Submission.Command addCompletedHandler:^(id<MTLCommandBuffer> Completed) {
							(void)Owners;
							(void)NativeResources;
							for (const auto& Readback : *Readbacks)
							{
								if (Completed.status != MTLCommandBufferStatusCompleted)
								{
									Readback.Request->Fail();
									continue;
								}
								const auto* Bytes = static_cast<const std::byte*>(Readback.Buffer.contents);
								if (!Bytes) Readback.Request->Fail();
								else Readback.Request->Complete(FByteBuffer(Bytes, Bytes + Readback.ByteCount));
							}
							std::lock_guard CompletionLock(SharedState->Mutex);
							if (Completed.status == MTLCommandBufferStatusCompleted)
								SharedState->Timeline->ObserveCompleted(Producer);
							else
								SharedState->Timeline->Fail();
							--SharedState->PendingCallbacks;
							SharedState->Completion.notify_all();
						}];
						++State->PendingCallbacks;
						[Submission.Command commit];
						requiref(State->Timeline->MarkSubmitted(Producer),
							"Metal queue lost an accepted GPU reservation.");
					}
				}
				Pending.clear();
			}
			auto RHIEndFrame() -> void override { Unsupported(); }
			auto RHIBeginDiagnosticRegion(std::string_view) -> void override { Unsupported(); }
			auto RHIEndDiagnosticRegion() -> void override { Unsupported(); }
			auto RHIBeginRenderPass(const FRHIRenderPassInfo&, FName) -> void override { Unsupported(); }
			auto RHIEndRenderPass() -> void override { Unsupported(); }
			auto RHIBeginDrawingViewport(FRHIViewport*, FRHITexture*) -> void override { Unsupported(); }
			auto RHIEndDrawingViewport(FRHIViewport*, bool, bool) -> void override { Unsupported(); }
			auto RHISetViewport(float, float, float, float, float, float) -> void override { Unsupported(); }
			auto RHISetScissor(float, float, float, float) -> void override { Unsupported(); }
			auto RHISetDepthBias(float, float, float) -> void override { Unsupported(); }
			auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState&) -> void override { Unsupported(); }
			auto RHIBindVertexBuffer(uint32, FRHIBuffer*, uint32) -> void override { Unsupported(); }
			auto RHIBindIndexBuffer(FRHIBuffer*, uint32) -> void override { Unsupported(); }
			auto RHITransitionBuffers(std::span<const FRHIBufferTransition> Transitions)
			-> void override
			{
				requiref(ValidateBufferTransitions(Transitions).has_value(),
					"Invalid Metal buffer transition.");
				// The baseline uses one tracked Metal queue. Ending each blit encoder
				// supplies the native boundary for its hazard-tracked accesses.
			}
			auto RHITransitionTextures(std::span<const FRHITextureTransition> Transitions)
			-> void override
			{
				requiref(ValidateTextureTransitions(Transitions).has_value(),
					"Invalid Metal texture transition.");
				// The current transfer path uses one hazard-tracked command queue
				// and ends each blit encoder before the next access.
			}
			auto RHICopyBuffer(FRHIBuffer* Source, FRHIBuffer* Destination,
			std::span<const FRHIBufferCopyRegion> Regions) -> void override
			{
				requiref(Active && ValidateBufferCopies(Source, Destination, Regions),
					"Invalid Metal buffer copy recording.");
				auto* SourceBuffer = static_cast<FMetalBuffer*>(Source);
				auto* DestinationBuffer = static_cast<FMetalBuffer*>(Destination);
				id<MTLBlitCommandEncoder> Encoder = [Active->Command blitCommandEncoder];
				requiref(Encoder != nil, "Metal buffer copy encoder creation failed.");
				for (const auto& Region : Regions)
					[Encoder copyFromBuffer:SourceBuffer->GetHandle()
						sourceOffset:Region.SourceOffset
						toBuffer:DestinationBuffer->GetHandle()
						destinationOffset:Region.DestinationOffset size:Region.Size];
				[Encoder endEncoding];
				[Active->NativeResources addObject:SourceBuffer->GetHandle()];
				[Active->NativeResources addObject:DestinationBuffer->GetHandle()];
			}
			auto RHICopyBufferToTexture(FRHIBuffer* Source, FRHITexture* Destination,
			std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override
			{
				requiref(Active && ValidateBufferToTextureCopies(Source, Destination, Regions),
					"Invalid Metal buffer-to-texture copy.");
				auto* Buffer = static_cast<FMetalBuffer*>(Source);
				auto* Texture = static_cast<FMetalTexture*>(Destination);
				id<MTLBlitCommandEncoder> Encoder = [Active->Command blitCommandEncoder];
				requiref(Encoder != nil, "Metal texture upload encoder creation failed.");
				for (const auto& Region : Regions)
				{
					const bool bVolume = Destination->GetDimension() == ETextureDimension::Texture3D;
					const NSUInteger RowLength = Region.BufferRowLength
						? Region.BufferRowLength : Region.TextureExtent.Width;
					const NSUInteger ImageHeight = Region.BufferImageHeight
						? Region.BufferImageHeight : Region.TextureExtent.Height;
					const NSUInteger RowPitch = RowLength
						* MetalBytesPerTexel(Destination->GetFormat());
					const NSUInteger ImagePitch = RowPitch * ImageHeight;
					for (uint32 Layer = 0; Layer < Region.TextureNumArrayLayers; ++Layer)
						[Encoder copyFromBuffer:Buffer->GetHandle()
							sourceOffset:Region.BufferOffset + Layer * ImagePitch
							sourceBytesPerRow:RowPitch sourceBytesPerImage:ImagePitch
							sourceSize:MTLSizeMake(Region.TextureExtent.Width,
								Region.TextureExtent.Height,
								bVolume ? Region.TextureExtent.Depth : 1)
							toTexture:Texture->GetHandle()
							destinationSlice:bVolume ? 0 : Region.TextureFirstArrayLayer + Layer
							destinationLevel:Region.TextureMip destinationOrigin:MTLOriginMake(
								Region.TextureOffset.X, Region.TextureOffset.Y,
								bVolume ? Region.TextureOffset.Z : 0)];
				}
				[Encoder endEncoding];
				[Active->NativeResources addObject:Buffer->GetHandle()];
				[Active->NativeResources addObject:Texture->GetHandle()];
			}
			auto RHICopyTextureToBuffer(FRHITexture* Source, FRHIBuffer* Destination,
			std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override
			{
				requiref(Active && ValidateTextureToBufferCopies(Source, Destination, Regions),
					"Invalid Metal texture-to-buffer copy.");
				auto* Texture = static_cast<FMetalTexture*>(Source);
				auto* Buffer = static_cast<FMetalBuffer*>(Destination);
				id<MTLBlitCommandEncoder> Encoder = [Active->Command blitCommandEncoder];
				requiref(Encoder != nil, "Metal texture readback encoder creation failed.");
				for (const auto& Region : Regions)
				{
					const bool bVolume = Source->GetDimension() == ETextureDimension::Texture3D;
					const NSUInteger RowLength = Region.BufferRowLength
						? Region.BufferRowLength : Region.TextureExtent.Width;
					const NSUInteger ImageHeight = Region.BufferImageHeight
						? Region.BufferImageHeight : Region.TextureExtent.Height;
					const NSUInteger RowPitch = RowLength
						* MetalBytesPerTexel(Source->GetFormat());
					const NSUInteger ImagePitch = RowPitch * ImageHeight;
					for (uint32 Layer = 0; Layer < Region.TextureNumArrayLayers; ++Layer)
						[Encoder copyFromTexture:Texture->GetHandle()
							sourceSlice:bVolume ? 0 : Region.TextureFirstArrayLayer + Layer
							sourceLevel:Region.TextureMip
							sourceOrigin:MTLOriginMake(Region.TextureOffset.X,
								Region.TextureOffset.Y, bVolume ? Region.TextureOffset.Z : 0)
							sourceSize:MTLSizeMake(Region.TextureExtent.Width,
								Region.TextureExtent.Height,
								bVolume ? Region.TextureExtent.Depth : 1)
							toBuffer:Buffer->GetHandle()
							destinationOffset:Region.BufferOffset + Layer * ImagePitch
							destinationBytesPerRow:RowPitch
							destinationBytesPerImage:ImagePitch];
				}
				[Encoder endEncoding];
				[Active->NativeResources addObject:Texture->GetHandle()];
				[Active->NativeResources addObject:Buffer->GetHandle()];
			}
			auto RHICopyTexture(FRHITexture* Source, FRHITexture* Destination,
			std::span<const FRHITextureCopyRegion> Regions) -> void override
			{
				requiref(Active && ValidateTextureCopies(Source, Destination, Regions),
					"Invalid Metal texture copy.");
				requiref((Source->GetDimension() == ETextureDimension::Texture3D)
					== (Destination->GetDimension() == ETextureDimension::Texture3D),
					"Metal texture copy cannot mix volume and layered textures.");
				auto* SourceTexture = static_cast<FMetalTexture*>(Source);
				auto* DestinationTexture = static_cast<FMetalTexture*>(Destination);
				id<MTLBlitCommandEncoder> Encoder = [Active->Command blitCommandEncoder];
				requiref(Encoder != nil, "Metal texture copy encoder creation failed.");
				for (const auto& Region : Regions)
				{
					const bool bVolume = Source->GetDimension() == ETextureDimension::Texture3D;
					for (uint32 Layer = 0; Layer < Region.NumArrayLayers; ++Layer)
						[Encoder copyFromTexture:SourceTexture->GetHandle()
							sourceSlice:bVolume ? 0 : Region.SourceFirstArrayLayer + Layer
							sourceLevel:Region.SourceMip
							sourceOrigin:MTLOriginMake(Region.SourceOffset.X, Region.SourceOffset.Y,
								bVolume ? Region.SourceOffset.Z : 0)
							sourceSize:MTLSizeMake(Region.Extent.Width, Region.Extent.Height,
								bVolume ? Region.Extent.Depth : 1)
							toTexture:DestinationTexture->GetHandle()
							destinationSlice:bVolume ? 0 : Region.DestinationFirstArrayLayer + Layer
							destinationLevel:Region.DestinationMip
							destinationOrigin:MTLOriginMake(Region.DestinationOffset.X,
								Region.DestinationOffset.Y,
								bVolume ? Region.DestinationOffset.Z : 0)];
				}
				[Encoder endEncoding];
				[Active->NativeResources addObject:SourceTexture->GetHandle()];
				[Active->NativeResources addObject:DestinationTexture->GetHandle()];
			}
			auto RHIWriteBuffer(FRHIBuffer* Buffer, uint32 Offset,
			FByteView Data) -> void override
			{
				requiref(Active && Buffer && Offset <= Buffer->GetSize()
					&& Data.size() <= Buffer->GetSize() - Offset,
					"Metal buffer upload exceeds its target range.");
				if (Data.empty()) return;
				id<MTLBuffer> Staging = [State->Queue.device
					newBufferWithBytes:Data.data() length:Data.size()
					options:MTLResourceStorageModeShared];
				requiref(Staging != nil, "Metal buffer upload staging allocation failed.");
				auto* Target = static_cast<FMetalBuffer*>(Buffer);
				id<MTLBlitCommandEncoder> Encoder = [Active->Command blitCommandEncoder];
				requiref(Encoder != nil, "Metal buffer upload encoder creation failed.");
				[Encoder copyFromBuffer:Staging sourceOffset:0
					toBuffer:Target->GetHandle() destinationOffset:Offset
					size:Data.size()];
				[Encoder endEncoding];
				[Active->NativeResources addObject:Staging];
				[Active->NativeResources addObject:Target->GetHandle()];
			}
			auto RHIUploadBuffer(FRHIBuffer* Buffer, uint32 Offset,
			FByteView Data) -> void override
			{ RHIWriteBuffer(Buffer, Offset, Data); }
			auto RHIInitializeTexture(FRHITexture* Texture) -> void override
			{
				requiref(Texture && (Texture->GetDimension() == ETextureDimension::Texture2D
					|| Texture->GetDimension() == ETextureDimension::Texture2DArray
					|| Texture->GetDimension() == ETextureDimension::TextureCube
					|| Texture->GetDimension() == ETextureDimension::TextureCubeArray
					|| Texture->GetDimension() == ETextureDimension::Texture3D)
					&& ToMetalPixelFormat(Texture->GetFormat()) != MTLPixelFormatInvalid,
					"Metal texture initialization requires a supported texture.");
				// Private Metal textures require no explicit creation-layout transition.
			}
			auto RHIUpdateTexture2D(FRHITexture* Texture, uint32 MipIndex,
			uint32 ArraySlice, const FUpdateTextureRegion2D& Region,
			uint32 SourcePitch, FByteView SourceData) -> void override
			{
				requiref(State && Texture
					&& (Texture->GetDimension() == ETextureDimension::Texture2D
						|| Texture->GetDimension() == ETextureDimension::Texture2DArray
						|| Texture->GetDimension() == ETextureDimension::TextureCube
						|| Texture->GetDimension() == ETextureDimension::TextureCubeArray)
					&& ToMetalPixelFormat(Texture->GetFormat()) != MTLPixelFormatInvalid
					&& EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::DestinationCopy),
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
				const NSUInteger BytesPerTexel = MetalBytesPerTexel(Texture->GetFormat());
				const uint64 SourceStart = static_cast<uint64>(Region.SrcY) * SourcePitch
					+ static_cast<uint64>(Region.SrcX) * BytesPerTexel;
				const uint64 RequiredBytes = SourceStart
					+ static_cast<uint64>(Region.Height - 1) * SourcePitch
					+ static_cast<uint64>(Region.Width) * BytesPerTexel;
				requiref(RequiredBytes <= SourceData.size(),
					"Metal texture upload source data is incomplete.");
				const NSUInteger RowPitch = static_cast<NSUInteger>(Region.Width) * BytesPerTexel;
				const NSUInteger ByteCount = RowPitch * Region.Height;
				id<MTLBuffer> Staging = [State->Queue.device newBufferWithLength:ByteCount
					options:MTLResourceStorageModeShared];
				requiref(Staging != nil, "Metal texture upload staging allocation failed.");
				auto* TargetBytes = static_cast<std::byte*>(Staging.contents);
				for (uint32 Row = 0; Row < Region.Height; ++Row)
					std::memcpy(TargetBytes + static_cast<size_t>(Row) * RowPitch,
						SourceData.data() + SourceStart + static_cast<uint64>(Row) * SourcePitch,
						RowPitch);
				if (!Active) RHISubmitCommands();
				id<MTLCommandBuffer> Command = Active
					? Active->Command : [State->Queue commandBuffer];
				requiref(Command != nil, "Metal texture upload command allocation failed.");
				id<MTLBlitCommandEncoder> Encoder = [Command blitCommandEncoder];
				requiref(Encoder != nil, "Metal texture upload encoder creation failed.");
				id<MTLTexture> Native = static_cast<FMetalTexture*>(Texture)->GetHandle();
				[Encoder copyFromBuffer:Staging sourceOffset:0
					sourceBytesPerRow:RowPitch sourceBytesPerImage:ByteCount
					sourceSize:MTLSizeMake(Region.Width, Region.Height, 1)
					toTexture:Native destinationSlice:ArraySlice destinationLevel:MipIndex
					destinationOrigin:MTLOriginMake(Region.DestX, Region.DestY, 0)];
				[Encoder endEncoding];
				if (Active)
				{
					[Active->NativeResources addObject:Staging];
					[Active->NativeResources addObject:Native];
					return;
				}
				auto SharedState = State;
				[Command addCompletedHandler:^(id<MTLCommandBuffer>) {
					(void)Staging;
					(void)Native;
					std::lock_guard Lock(SharedState->Mutex);
					--SharedState->PendingCallbacks;
					SharedState->Completion.notify_all();
				}];
				{
					std::lock_guard Lock(State->Mutex);
					++State->PendingCallbacks;
				}
				[Command commit];
			}
			auto RHIUpdateTexture3D(FRHITexture* Texture, uint32 MipIndex,
			const FUpdateTextureRegion3D& Region, uint32 SourceRowPitch,
			uint32 SourceDepthPitch, FByteView SourceData) -> void override
			{
				requiref(State && Texture && Texture->GetDimension() == ETextureDimension::Texture3D
					&& ToMetalPixelFormat(Texture->GetFormat()) != MTLPixelFormatInvalid
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
				const NSUInteger BytesPerTexel = MetalBytesPerTexel(Texture->GetFormat());
				const uint64 SourceStart = static_cast<uint64>(Region.SrcZ) * SourceDepthPitch
					+ static_cast<uint64>(Region.SrcY) * SourceRowPitch
					+ static_cast<uint64>(Region.SrcX) * BytesPerTexel;
				const uint64 RequiredBytes = SourceStart
					+ static_cast<uint64>(Region.Depth - 1) * SourceDepthPitch
					+ static_cast<uint64>(Region.Height - 1) * SourceRowPitch
					+ static_cast<uint64>(Region.Width) * BytesPerTexel;
				requiref(RequiredBytes <= SourceData.size(),
					"Metal volume upload source data is incomplete.");
				const NSUInteger RowPitch = static_cast<NSUInteger>(Region.Width) * BytesPerTexel;
				const NSUInteger ImagePitch = RowPitch * Region.Height;
				const NSUInteger ByteCount = ImagePitch * Region.Depth;
				id<MTLBuffer> Staging = [State->Queue.device newBufferWithLength:ByteCount
					options:MTLResourceStorageModeShared];
				requiref(Staging != nil, "Metal volume upload staging allocation failed.");
				auto* TargetBytes = static_cast<std::byte*>(Staging.contents);
				for (uint32 Z = 0; Z < Region.Depth; ++Z)
					for (uint32 Row = 0; Row < Region.Height; ++Row)
						std::memcpy(TargetBytes + static_cast<size_t>(Z) * ImagePitch
							+ static_cast<size_t>(Row) * RowPitch,
							SourceData.data() + SourceStart
								+ static_cast<uint64>(Z) * SourceDepthPitch
								+ static_cast<uint64>(Row) * SourceRowPitch,
							RowPitch);
				if (!Active) RHISubmitCommands();
				id<MTLCommandBuffer> Command = Active
					? Active->Command : [State->Queue commandBuffer];
				requiref(Command != nil, "Metal volume upload command allocation failed.");
				id<MTLBlitCommandEncoder> Encoder = [Command blitCommandEncoder];
				requiref(Encoder != nil, "Metal volume upload encoder creation failed.");
				id<MTLTexture> Native = static_cast<FMetalTexture*>(Texture)->GetHandle();
				[Encoder copyFromBuffer:Staging sourceOffset:0
					sourceBytesPerRow:RowPitch sourceBytesPerImage:ImagePitch
					sourceSize:MTLSizeMake(Region.Width, Region.Height, Region.Depth)
					toTexture:Native destinationSlice:0 destinationLevel:MipIndex
					destinationOrigin:MTLOriginMake(Region.DestX, Region.DestY, Region.DestZ)];
				[Encoder endEncoding];
				if (Active)
				{
					[Active->NativeResources addObject:Staging];
					[Active->NativeResources addObject:Native];
					return;
				}
				auto SharedState = State;
				[Command addCompletedHandler:^(id<MTLCommandBuffer>) {
					(void)Staging;
					(void)Native;
					std::lock_guard Lock(SharedState->Mutex);
					--SharedState->PendingCallbacks;
					SharedState->Completion.notify_all();
				}];
				{
					std::lock_guard Lock(State->Mutex);
					++State->PendingCallbacks;
				}
				[Command commit];
			}
			auto RHIReadTexture2D(FRHITexture* Texture, uint32 MipIndex,
			uint32 ArraySlice, FByteBuffer& OutData) -> bool override
			{
				OutData.clear();
				if (!State || Active || !Texture || MipIndex >= Texture->GetNumMips()
					|| ArraySlice >= Texture->GetArraySize()
					|| (Texture->GetDimension() != ETextureDimension::Texture2D
						&& Texture->GetDimension() != ETextureDimension::Texture2DArray
						&& Texture->GetDimension() != ETextureDimension::TextureCube
						&& Texture->GetDimension() != ETextureDimension::TextureCubeArray)
					|| ToMetalPixelFormat(Texture->GetFormat()) == MTLPixelFormatInvalid
					|| !EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::CPUReadback))
					return false;
				RHISubmitCommands();
				const NSUInteger Width = std::max(1u, Texture->GetSizeX() >> MipIndex);
				const NSUInteger Height = std::max(1u, Texture->GetSizeY() >> MipIndex);
				const NSUInteger RowPitch = Width
					* MetalBytesPerTexel(Texture->GetFormat());
				const NSUInteger ByteCount = RowPitch * Height;
				id<MTLBuffer> Readback = [State->Queue.device newBufferWithLength:ByteCount
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> Command = [State->Queue commandBuffer];
				if (!Readback || !Command) return false;
				id<MTLBlitCommandEncoder> Encoder = [Command blitCommandEncoder];
				if (!Encoder) return false;
				[Encoder copyFromTexture:static_cast<FMetalTexture*>(Texture)->GetHandle()
					sourceSlice:ArraySlice sourceLevel:MipIndex sourceOrigin:MTLOriginMake(0, 0, 0)
					sourceSize:MTLSizeMake(Width, Height, 1)
					toBuffer:Readback destinationOffset:0 destinationBytesPerRow:RowPitch
					destinationBytesPerImage:ByteCount];
				[Encoder endEncoding];
				[Command commit];
				[Command waitUntilCompleted];
				if (Command.status != MTLCommandBufferStatusCompleted) return false;
				const auto* Bytes = static_cast<const std::byte*>(Readback.contents);
				if (!Bytes) return false;
				OutData.assign(Bytes, Bytes + ByteCount);
				return true;
			}
			auto RHIEnqueueTextureReadback(FRHITexture* Texture, uint32 MipIndex,
				uint32 ArraySlice, std::shared_ptr<FRHITextureReadback> Request) -> void override
			{
				if (!Request || Request->GetState() != ERHITextureReadbackState::Pending)
					return;
				if (!State || !Texture || MipIndex >= Texture->GetNumMips()
					|| ArraySlice >= Texture->GetArraySize()
					|| (Texture->GetDimension() != ETextureDimension::Texture2D
						&& Texture->GetDimension() != ETextureDimension::Texture2DArray
						&& Texture->GetDimension() != ETextureDimension::TextureCube
						&& Texture->GetDimension() != ETextureDimension::TextureCubeArray)
					|| ToMetalPixelFormat(Texture->GetFormat()) == MTLPixelFormatInvalid
					|| !EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::CPUReadback))
				{
					Request->Fail();
					return;
				}
				if (!Active) RHISubmitCommands();
				const NSUInteger Width = std::max(1u, Texture->GetSizeX() >> MipIndex);
				const NSUInteger Height = std::max(1u, Texture->GetSizeY() >> MipIndex);
				const NSUInteger RowPitch = Width
					* MetalBytesPerTexel(Texture->GetFormat());
				const NSUInteger ByteCount = RowPitch * Height;
				id<MTLBuffer> Readback = [State->Queue.device newBufferWithLength:ByteCount
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> Command = Active
					? Active->Command : [State->Queue commandBuffer];
				id<MTLBlitCommandEncoder> Encoder = Command
					? [Command blitCommandEncoder] : nil;
				if (!Readback || !Encoder)
				{
					if (Encoder) [Encoder endEncoding];
					Request->Fail();
					return;
				}
				id<MTLTexture> Native = static_cast<FMetalTexture*>(Texture)->GetHandle();
				[Encoder copyFromTexture:Native sourceSlice:ArraySlice sourceLevel:MipIndex
					sourceOrigin:MTLOriginMake(0, 0, 0)
					sourceSize:MTLSizeMake(Width, Height, 1)
					toBuffer:Readback destinationOffset:0 destinationBytesPerRow:RowPitch
						destinationBytesPerImage:ByteCount];
				[Encoder endEncoding];
				if (Active)
				{
					[Active->NativeResources addObject:Native];
					[Active->NativeResources addObject:Readback];
					Active->Readbacks.push_back({Readback, ByteCount, std::move(Request)});
					return;
				}
				auto SharedState = State;
				[Command addCompletedHandler:^(id<MTLCommandBuffer> Completed) {
					(void)Native;
					if (Completed.status != MTLCommandBufferStatusCompleted)
						Request->Fail();
					else if (const auto* Bytes = static_cast<const std::byte*>(Readback.contents))
						Request->Complete(FByteBuffer(Bytes, Bytes + ByteCount));
					else Request->Fail();
					std::lock_guard Lock(SharedState->Mutex);
					--SharedState->PendingCallbacks;
					SharedState->Completion.notify_all();
				}];
				{
					std::lock_guard Lock(State->Mutex);
					++State->PendingCallbacks;
				}
				[Command commit];
			}
			auto RHIAcquireBackBuffer(FRHITexture*) -> void override { Unsupported(); }
			auto RHIBlockUntilGPUIdle() -> void override
			{
				RHISubmitCommands();
				std::unique_lock Lock(State->Mutex);
				State->Completion.wait(Lock,
					[this] { return State->PendingCallbacks == 0; });
			}
			auto RHIPushConstants(EShaderStageFlags, uint32, uint32,
			const void*) -> void override { Unsupported(); }
			auto RHISetShaderParameters(FRHIShader*,
			const std::span<const FRHIShaderParameterResource>&) -> void override
		{ Unsupported(); }
			auto RHIDraw(const FRHIDrawArguments&) -> void override { Unsupported(); }
			auto RHIDrawIndexed(const FRHIDrawIndexedArguments&) -> void override { Unsupported(); }
		private:
			auto CancelPending() -> void
			{
				if (!State) return;
				std::lock_guard Lock(State->Mutex);
				for (const auto& Submission : Pending)
				{
					for (const auto& Readback : Submission.Readbacks)
						Readback.Request->Cancel();
					State->Timeline->Cancel(Submission.Producer);
				}
				Pending.clear();
				if (Active)
					for (const auto& Readback : Active->Readbacks)
						Readback.Request->Cancel();
				Active.reset();
				StorageOwner.reset();
			}
			friend class FMetalDynamicRHI;
			static auto Unsupported() -> void
			{
				requiref(false, "Metal command execution has not been implemented.");
			}
			std::shared_ptr<FMetalSubmissionState> State;
			std::shared_ptr<void> StorageOwner;
			std::optional<FMetalPendingSubmission> Active;
			std::vector<FMetalPendingSubmission> Pending;
		};

		class FMetalDynamicRHI final : public FDynamicRHI
		{
		public:
			auto Init(const FRHIInitializationContext& Context) -> void override
			{
				if (Context.GetPresentationTarget())
					throw std::runtime_error("Metal presentation is not implemented yet.");
				const NSOperatingSystemVersion OS =
					[NSProcessInfo processInfo].operatingSystemVersion;
				if (OS.majorVersion < 27)
					throw std::runtime_error("MetalRHI requires macOS 27 or newer.");
				id<MTLDevice> CandidateDevice = MTLCreateSystemDefaultDevice();
				if (!CandidateDevice)
					throw std::runtime_error("MetalRHI could not create a Metal device.");
				if (![CandidateDevice supportsFamily:MTLGPUFamilyApple9])
					throw std::runtime_error("MetalRHI requires Apple GPU Family 9 or newer.");
				id<MTLCommandQueue> CandidateQueue = [CandidateDevice newCommandQueue];
				if (!CandidateQueue)
					throw std::runtime_error("MetalRHI could not create its command queue.");
				Device = CandidateDevice;
				Queue = CandidateQueue;
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
				CommandContext.Configure(State);
				// Resource capabilities stay unpublished until native resource
				// operations implement the limits they report.
			}

			auto Shutdown() -> void override
			{
			CommandContext.CancelPending();
			if (State)
			{
				std::unique_lock Lock(State->Mutex);
				State->Completion.wait(Lock,
					[this] { return State->PendingCallbacks == 0; });
				State->Timeline->Fail();
			}
			CommandContext.Configure({});
			State.reset();
				ClearCapabilities();
				QueueCapabilities = {};
				Queue = nil;
				Device = nil;
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
			auto RHIBeginFrame(const FRHIBeginFrameArgs&) -> void override
			{ requiref(false, "Metal frame execution has not been implemented."); }
			auto RHIEndFrame() -> void override
			{ requiref(false, "Metal frame execution has not been implemented."); }
			auto RHICreateViewport(const FRHIViewportCreateInfo&)
			-> TRefCountPtr<FRHIViewport> override { return nullptr; }
			auto RHIResizeViewport(FRHIViewport*, uint32, uint32, bool) -> void override
			{ requiref(false, "Metal viewport resize has not been implemented."); }
			auto RHICreateGraphicsPipelineState(FName,
			const FGraphicsPipelineStateInitializer&)
			-> TRefCountPtr<FRHIGraphicsPipelineState> override { return nullptr; }
			auto RHIGetDefaultContext() -> IRHICommandContext* override
			{ return &CommandContext; }
			auto RHIGetViewportBackBuffer(FRHIViewport*)
			-> TRefCountPtr<FRHITexture> override { return nullptr; }
			auto RHICreateVertexDeclaration(const FVertexDeclarationElementList&)
			-> TRefCountPtr<FRHIVertexDeclaration> override { return nullptr; }
			auto RHIIsTextureSupported(const FRHITextureCreateDesc& Desc) const -> bool override
			{
				constexpr ETextureCreateFlags TransferFlags = ETextureCreateFlags::SourceCopy
					| ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback;
				const bool b2D = Desc.Dimension == ETextureDimension::Texture2D;
				const bool b2DArray = Desc.Dimension == ETextureDimension::Texture2DArray;
				const bool bCube = Desc.Dimension == ETextureDimension::TextureCube;
				const bool bCubeArray = Desc.Dimension == ETextureDimension::TextureCubeArray;
				const bool b3D = Desc.Dimension == ETextureDimension::Texture3D;
				return Device && ValidateTextureCreateDesc(Desc)
					&& (b2D || b2DArray || bCube || bCubeArray || b3D)
					&& ToMetalPixelFormat(Desc.Format) != MTLPixelFormatInvalid
					&& (b2D || Desc.Format == EPixelFormat::RGBA8_UNORM
						|| (bCube && Desc.Format == EPixelFormat::RGBA16_FLOAT))
					&& Desc.NumSamples == 1
					&& Desc.Extent.x <= (b3D ? 2048 : 16384)
					&& Desc.Extent.y <= (b3D ? 2048 : 16384)
					&& (!b3D || Desc.Depth <= 2048)
					&& (!b2DArray || Desc.ArraySize <= 2048)
					&& (!bCubeArray || Desc.ArraySize <= 2048 * TextureCubeFaceCount)
					&& EnumHasAnyFlags(Desc.Flags, TransferFlags)
					&& (static_cast<uint64>(Desc.Flags)
						& ~static_cast<uint64>(TransferFlags)) == 0;
			}
			auto RHITryCreateTexture(FRHICommandListBase&,
			const FRHITextureCreateDesc& Desc)
			-> std::expected<FTextureRHIRef, FRHICreationError> override
			{
				if (!RHIIsTextureSupported(Desc))
					return std::unexpected(UnsupportedCreation());
				MTLTextureDescriptor* Native = [MTLTextureDescriptor
					texture2DDescriptorWithPixelFormat:ToMetalPixelFormat(Desc.Format)
					width:Desc.Extent.x height:Desc.Extent.y mipmapped:NO];
				Native.mipmapLevelCount = Desc.NumMips;
				if (Desc.Dimension == ETextureDimension::TextureCube)
					Native.textureType = MTLTextureTypeCube;
				else if (Desc.Dimension == ETextureDimension::TextureCubeArray)
				{
					Native.textureType = MTLTextureTypeCubeArray;
					Native.arrayLength = Desc.ArraySize / TextureCubeFaceCount;
				}
				else if (Desc.Dimension == ETextureDimension::Texture2DArray)
				{
					Native.textureType = MTLTextureType2DArray;
					Native.arrayLength = Desc.ArraySize;
				}
				else if (Desc.Dimension == ETextureDimension::Texture3D)
				{
					Native.textureType = MTLTextureType3D;
					Native.depth = Desc.Depth;
				}
				Native.storageMode = MTLStorageModePrivate;
				Native.usage = MTLTextureUsageUnknown;
				id<MTLTexture> Texture = [Device newTextureWithDescriptor:Native];
				if (!Texture)
					return std::unexpected(FRHICreationError{
						.Failure = ERHIResourceCreationFailure::OutOfMemory,
						.Source = ERHICreationFailureSource::NativeBackend});
				return FTextureRHIRef(new FMetalTexture(Desc, Texture));
			}
			auto RHICreateSampler(const FRHISamplerDesc& Desc)
			-> TRefCountPtr<FRHISampler> override
			{
				if (!Device || !std::isfinite(Desc.MinLod) || !std::isfinite(Desc.MaxLod)
					|| Desc.MinLod < 0.0f || Desc.MaxLod < Desc.MinLod
					|| !std::isfinite(Desc.MipLodBias) || Desc.MipLodBias < -16.0f
					|| Desc.MipLodBias > 15.999f || Desc.bUnnormalizedCoordinates
					|| (Desc.MinFilter != ESamplerFilter::Nearest
						&& Desc.MinFilter != ESamplerFilter::Linear)
					|| (Desc.MagFilter != ESamplerFilter::Nearest
						&& Desc.MagFilter != ESamplerFilter::Linear)
					|| (Desc.MipmapMode != ESamplerMipmapMode::Nearest
						&& Desc.MipmapMode != ESamplerMipmapMode::Linear)
					|| !std::isfinite(Desc.MaxAnisotropy)
					|| (Desc.bEnableAnisotropy && (Desc.MaxAnisotropy < 1.0f
						|| Desc.MaxAnisotropy > 16.0f
						|| std::floor(Desc.MaxAnisotropy) != Desc.MaxAnisotropy)))
					return nullptr;
				const auto U = ToMetalAddressMode(Desc.AddressU);
				const auto V = ToMetalAddressMode(Desc.AddressV);
				const auto W = ToMetalAddressMode(Desc.AddressW);
				const auto Compare = ToMetalCompareFunction(Desc.CompareOp);
				if (!U || !V || !W || !Compare) return nullptr;
				MTLSamplerDescriptor* Native = [MTLSamplerDescriptor new];
				Native.minFilter = Desc.MinFilter == ESamplerFilter::Linear
					? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
				Native.magFilter = Desc.MagFilter == ESamplerFilter::Linear
					? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
				Native.mipFilter = Desc.MipmapMode == ESamplerMipmapMode::Linear
					? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
				Native.sAddressMode = *U;
				Native.tAddressMode = *V;
				Native.rAddressMode = *W;
				Native.maxAnisotropy = Desc.bEnableAnisotropy
					? static_cast<NSUInteger>(Desc.MaxAnisotropy) : 1;
				Native.lodMinClamp = Desc.MinLod;
				Native.lodMaxClamp = Desc.MaxLod;
				Native.lodBias = Desc.MipLodBias;
				Native.compareFunction = Desc.bEnableCompare ? *Compare : MTLCompareFunctionNever;
				switch (Desc.BorderColor)
				{
				case ESamplerBorderColor::FloatTransparentBlack:
				case ESamplerBorderColor::IntTransparentBlack:
					Native.borderColor = MTLSamplerBorderColorTransparentBlack; break;
				case ESamplerBorderColor::FloatOpaqueBlack:
				case ESamplerBorderColor::IntOpaqueBlack:
					Native.borderColor = MTLSamplerBorderColorOpaqueBlack; break;
				case ESamplerBorderColor::FloatOpaqueWhite:
				case ESamplerBorderColor::IntOpaqueWhite:
					Native.borderColor = MTLSamplerBorderColorOpaqueWhite; break;
				default: return nullptr;
				}
				id<MTLSamplerState> State = [Device newSamplerStateWithDescriptor:Native];
				return State ? TRefCountPtr<FRHISampler>(new FMetalSampler(State)) : nullptr;
			}
			auto RHICreateShader(const FRHIShaderCreateDesc&)
			-> TRefCountPtr<FRHIShader> override { return nullptr; }
			auto RHITryCreateBuffer(FRHICommandListImmediate&,
			const FRHIBufferCreateDesc& Desc)
			-> std::expected<FBufferRHIRef, FRHICreationError> override
			{
				if (!Device || Desc.Size == 0 || Desc.Usage == EBufferUsageFlags::None
					|| EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::NullResource)
					|| (Desc.InitialData.Size != 0 && !Desc.InitialData.Data)
					|| Desc.InitialData.Size > Desc.Size)
					return std::unexpected(UnsupportedCreation());
				if (Desc.Size > Device.maxBufferLength)
					return std::unexpected(FRHICreationError{
						.Failure = ERHIResourceCreationFailure::ResourceExhausted,
						.Source = ERHICreationFailureSource::NativeBackend});
				id<MTLBuffer> Buffer = [Device newBufferWithLength:Desc.Size
					options:MTLResourceStorageModeShared];
				if (!Buffer)
					return std::unexpected(FRHICreationError{
						.Failure = ERHIResourceCreationFailure::OutOfMemory,
						.Source = ERHICreationFailureSource::NativeBackend});
				if (Desc.InitialData.Data && Desc.InitialData.Size)
					std::memcpy(Buffer.contents, Desc.InitialData.Data, Desc.InitialData.Size);
				return FBufferRHIRef(new FMetalBuffer(Desc, Buffer));
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
				if (!Texture || !ValidateTextureViewDesc(Texture, Desc)
					|| (Desc.Usage != ERHITextureViewUsage::TransferSource
						&& Desc.Usage != ERHITextureViewUsage::TransferDestination))
					return nullptr;
				return new FRHITextureView(Texture, Desc);
			}
		private:
			static auto UnsupportedCreation() -> FRHICreationError
			{
				return {.Failure = ERHIResourceCreationFailure::UnsupportedDescriptor,
					.Source = ERHICreationFailureSource::NativeBackend};
			}
			id<MTLDevice> Device = nil;
			id<MTLCommandQueue> Queue = nil;
			std::shared_ptr<FMetalSubmissionState> State;
			FRHIQueueCapabilities QueueCapabilities;
			FMetalCommandContext CommandContext;
		};

		class FMetalDynamicRHIModule final : public IDynamicRHIModule
		{
		public:
			auto SupportsDynamicReloading() const -> bool override { return true; }
			auto CreateRHI() -> FDynamicRHI* override
			{ return new FMetalDynamicRHI(); }
		};
	}

	IMPLEMENT_MODULE(FMetalDynamicRHIModule, MetalRHI)
}
