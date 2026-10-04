#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "DynamicRHI.h"
#include "Backend/RHICompletionBackend.h"
#include "Backend/RHIDeferredBufferBackend.h"
#include "MetalBuffer.h"
#include "MetalSampler.h"
#include "MetalTexture.h"
#include "RHIContext.h"
#include "RHICommandList.h"
#include "RHIShaderParameters.h"
#include "PipelineStateCache.h"

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
			case EPixelFormat::D32: return MTLPixelFormatDepth32Float;
			case EPixelFormat::BC1_UNORM: return MTLPixelFormatBC1_RGBA;
			case EPixelFormat::BC1_UNORM_SRGB: return MTLPixelFormatBC1_RGBA_sRGB;
			default: return MTLPixelFormatInvalid;
			}
		}

		auto IsMetalColorRenderFormat(EPixelFormat Format) -> bool
		{
			return Format == EPixelFormat::R8_UNORM
				|| Format == EPixelFormat::RGBA8_UNORM
				|| Format == EPixelFormat::BGRA8_UNORM
				|| Format == EPixelFormat::SRGBA8_UNORM
				|| Format == EPixelFormat::SBGRA8_UNORM
				|| Format == EPixelFormat::R11G11B10_FLOAT
				|| Format == EPixelFormat::RGBA16_FLOAT
				|| Format == EPixelFormat::RG32_UINT;
		}

		auto MetalBytesPerTexel(EPixelFormat Format) -> NSUInteger
		{ return GetPixelFormatInfo(Format).BytesPerBlock; }

		auto ToMetalVertexFormat(EVertexElementType Type) -> MTLVertexFormat
		{
			switch (Type)
			{
			case EVertexElementType::Float1: return MTLVertexFormatFloat;
			case EVertexElementType::Float2: return MTLVertexFormatFloat2;
			case EVertexElementType::Float3: return MTLVertexFormatFloat3;
			case EVertexElementType::Float4: return MTLVertexFormatFloat4;
			case EVertexElementType::Color:
			case EVertexElementType::UByte4N: return MTLVertexFormatUChar4Normalized;
			case EVertexElementType::Half2: return MTLVertexFormatHalf2;
			case EVertexElementType::Half4: return MTLVertexFormatHalf4;
			case EVertexElementType::Short4N: return MTLVertexFormatShort4Normalized;
			default: return MTLVertexFormatInvalid;
			}
		}

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

		auto ToMetalDepthCompare(ERHIDepthCompareOp Op)
			-> std::optional<MTLCompareFunction>
		{
			switch (Op)
			{
			case ERHIDepthCompareOp::Never: return MTLCompareFunctionNever;
			case ERHIDepthCompareOp::Less: return MTLCompareFunctionLess;
			case ERHIDepthCompareOp::Equal: return MTLCompareFunctionEqual;
			case ERHIDepthCompareOp::LessOrEqual: return MTLCompareFunctionLessEqual;
			case ERHIDepthCompareOp::Greater: return MTLCompareFunctionGreater;
			case ERHIDepthCompareOp::NotEqual: return MTLCompareFunctionNotEqual;
			case ERHIDepthCompareOp::GreaterOrEqual: return MTLCompareFunctionGreaterEqual;
			case ERHIDepthCompareOp::Always: return MTLCompareFunctionAlways;
			default: return std::nullopt;
			}
		}

		auto ToMetalBlendFactor(ERHIBlendFactor Factor)
			-> std::optional<MTLBlendFactor>
		{
			switch (Factor)
			{
			case ERHIBlendFactor::Zero: return MTLBlendFactorZero;
			case ERHIBlendFactor::One: return MTLBlendFactorOne;
			case ERHIBlendFactor::SrcColor: return MTLBlendFactorSourceColor;
			case ERHIBlendFactor::OneMinusSrcColor:
				return MTLBlendFactorOneMinusSourceColor;
			case ERHIBlendFactor::DstColor: return MTLBlendFactorDestinationColor;
			case ERHIBlendFactor::OneMinusDstColor:
				return MTLBlendFactorOneMinusDestinationColor;
			case ERHIBlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
			case ERHIBlendFactor::OneMinusSrcAlpha:
				return MTLBlendFactorOneMinusSourceAlpha;
			case ERHIBlendFactor::DstAlpha: return MTLBlendFactorDestinationAlpha;
			case ERHIBlendFactor::OneMinusDstAlpha:
				return MTLBlendFactorOneMinusDestinationAlpha;
			default: return std::nullopt;
			}
		}

		auto ToMetalBlendOperation(ERHIBlendOp Op)
			-> std::optional<MTLBlendOperation>
		{
			switch (Op)
			{
			case ERHIBlendOp::Add: return MTLBlendOperationAdd;
			case ERHIBlendOp::Subtract: return MTLBlendOperationSubtract;
			case ERHIBlendOp::ReverseSubtract:
				return MTLBlendOperationReverseSubtract;
			case ERHIBlendOp::Min: return MTLBlendOperationMin;
			case ERHIBlendOp::Max: return MTLBlendOperationMax;
			default: return std::nullopt;
			}
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
			std::vector<TRefCountPtr<FRHIResource>> ResourceOwners;
			NSMutableArray<id<MTLResource>>* NativeResources = [NSMutableArray new];
			std::vector<FReadback> Readbacks;
		};

		struct FMetalDeferredBacking
		{
			std::shared_ptr<const FRHIDeferredBufferSnapshot> Snapshot;
			id<MTLBuffer> Handle = nil;
		};

		class FMetalShader final : public FRHIShader
		{
		public:
			FMetalShader(const FRHIShaderCreateDesc& Desc,
				id<MTLLibrary> InLibrary, id<MTLFunction> InFunction)
				: FRHIShader(Desc), Library(InLibrary), Function(InFunction) {}

			auto GetFunction() const -> id<MTLFunction> { return Function; }

		private:
			id<MTLLibrary> Library;
			id<MTLFunction> Function;
		};

		class FMetalComputePipelineState final : public FRHIComputePipelineState
		{
		public:
			FMetalComputePipelineState(FRHIShader* InShader,
				FPipelineLayoutDesc InLayout, id<MTLComputePipelineState> InPipeline)
				: Shader(InShader), Layout(std::move(InLayout)), Pipeline(InPipeline) {}

			auto GetPipeline() const -> id<MTLComputePipelineState> { return Pipeline; }
			auto GetLayout() const -> const FPipelineLayoutDesc& { return Layout; }
			auto GetShader() const -> FRHIShader* { return Shader.GetReference(); }

		private:
			FShaderRHIRef Shader;
			FPipelineLayoutDesc Layout;
			id<MTLComputePipelineState> Pipeline;
		};

		class FMetalVertexDeclaration final : public FRHIVertexDeclaration
		{
		public:
			explicit FMetalVertexDeclaration(FVertexDeclarationElementList InElements)
				: Elements(std::move(InElements)) {}
			auto GetElements() const -> const FVertexDeclarationElementList& override
			{ return Elements; }
		private:
			FVertexDeclarationElementList Elements;
		};

		class FMetalGraphicsPipelineState final : public FRHIGraphicsPipelineState
		{
		public:
			FMetalGraphicsPipelineState(FRHIShader* InVertex,
				FRHIShader* InFragment, FRHIVertexDeclaration* InDeclaration,
				id<MTLRenderPipelineState> InPipeline,
				FRHIRenderTargetLayout InRenderTargets,
				FRHIRasterizerState InRasterizer,
				FPipelineLayoutDesc InLayout,
				id<MTLDepthStencilState> InDepthStencil)
				: Vertex(InVertex), Fragment(InFragment), Declaration(InDeclaration),
					Pipeline(InPipeline), RenderTargets(std::move(InRenderTargets)),
					Rasterizer(InRasterizer), Layout(std::move(InLayout)),
					DepthStencil(InDepthStencil) {}
			auto GetPipeline() const -> id<MTLRenderPipelineState> { return Pipeline; }
			auto GetRenderTargets() const -> const FRHIRenderTargetLayout&
				{ return RenderTargets; }
			auto GetRasterizer() const -> const FRHIRasterizerState&
				{ return Rasterizer; }
			auto GetLayout() const -> const FPipelineLayoutDesc& { return Layout; }
			auto GetVertexShader() const -> FRHIShader* { return Vertex.GetReference(); }
			auto GetFragmentShader() const -> FRHIShader* { return Fragment.GetReference(); }
			auto GetDepthStencil() const -> id<MTLDepthStencilState>
				{ return DepthStencil; }
			auto GetRequiredVertexStreams() const -> uint16
			{
				uint16 Streams = 0;
				for (const auto& Element : Declaration->GetElements())
				{
					if (Element.Type == EVertexElementType::None) break;
					Streams |= uint16(1u << Element.StreamIndex);
				}
				return Streams;
			}
		private:
			FShaderRHIRef Vertex;
			FShaderRHIRef Fragment;
			FVertexDeclarationRHIRef Declaration;
			id<MTLRenderPipelineState> Pipeline;
			FRHIRenderTargetLayout RenderTargets;
			FRHIRasterizerState Rasterizer;
			FPipelineLayoutDesc Layout;
			id<MTLDepthStencilState> DepthStencil;
		};

		class FMetalViewport final : public FRHIViewport
		{
		public:
			FMetalViewport(id<MTLDevice> InDevice, CAMetalLayer* InLayer,
				uint32 Width, uint32 Height, EPixelFormat InFormat)
				: Device(InDevice), Layer(InLayer), Format(InFormat)
			{
				Layer.device = Device;
				Layer.pixelFormat = ToMetalPixelFormat(Format);
				Layer.framebufferOnly = NO;
				Resize(Width, Height);
			}
			auto Resize(uint32 Width, uint32 Height) -> bool
			{
				if (!Width || !Height) return false;
				MTLTextureDescriptor* Native = [MTLTextureDescriptor
					texture2DDescriptorWithPixelFormat:ToMetalPixelFormat(Format)
					width:Width height:Height mipmapped:NO];
				Native.storageMode = MTLStorageModePrivate;
				Native.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
				id<MTLTexture> Texture = [Device newTextureWithDescriptor:Native];
				if (!Texture) return false;
				auto Desc = FRHITextureCreateDesc::Create2D(
				"Metal viewport back buffer", Width, Height, Format);
			Desc.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::SourceCopy
				| ETextureCreateFlags::CPUReadback);
			{
				std::lock_guard Lock(Mutex);
				BackBuffer = new FMetalTexture(Desc, Texture);
				Layer.drawableSize = CGSizeMake(Width, Height);
			}
			return true;
			}
			auto GetBackBuffer(FRHICommandListImmediate&) -> TRefCountPtr<FRHITexture> override
			{ return SnapshotBackBuffer(); }
			auto SnapshotBackBuffer() const -> FTextureRHIRef
			{
				std::lock_guard Lock(Mutex);
				return BackBuffer;
			}
			auto GetLayer() const -> CAMetalLayer* { return Layer; }
			auto GetFormat() const -> EPixelFormat override { return Format; }
		private:
			id<MTLDevice> Device;
			CAMetalLayer* Layer;
			EPixelFormat Format;
			mutable std::mutex Mutex;
			FTextureRHIRef BackBuffer;
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
				ComputePipeline = nullptr;
				ComputeParameters.clear();
				ComputeParameterOwners.clear();
				ComputePushConstants.clear();
				ComputePushConstantWritten.clear();
				GraphicsPipeline = nullptr;
				BoundVertexStreams = 0;
				IndexBuffer = nullptr;
				GraphicsParameters.clear();
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
						auto ResourceOwners = std::make_shared<
							std::vector<TRefCountPtr<FRHIResource>>>(
							std::move(Submission.ResourceOwners));
						auto Readbacks = std::make_shared<std::vector<FMetalPendingSubmission::FReadback>>(
							std::move(Submission.Readbacks));
						NSArray<id<MTLResource>>* NativeResources =
							[Submission.NativeResources copy];
						[Submission.Command addCompletedHandler:^(id<MTLCommandBuffer> Completed) {
							(void)Owners;
							(void)ResourceOwners;
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
			auto RHIEndFrame() -> void override
			{
				requiref(bFrameOpen && !Active && !RenderEncoder,
					"Metal frame end requires closed GPU submissions.");
				RHISubmitCommands();
				bFrameOpen = false;
			}
			auto RHIBeginDiagnosticRegion(std::string_view) -> void override { Unsupported(); }
			auto RHIEndDiagnosticRegion() -> void override { Unsupported(); }
			auto RHIBeginRenderPass(const FRHIRenderPassInfo& Info, FName) -> void override
			{
				if (!Active && State)
				{
					FMetalPendingSubmission Submission;
					Submission.Command = [State->Queue commandBuffer];
					requiref(Submission.Command != nil,
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
				FRHITexture* Color = Info.ColorRenderTargets[0];
				MTLRenderPassDescriptor* Desc = [MTLRenderPassDescriptor renderPassDescriptor];
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
					id<MTLTexture> Texture = static_cast<FMetalTexture*>(Target)->GetHandle();
					auto* Attachment = Desc.colorAttachments[Index];
					Attachment.texture = Texture;
					switch (Layout.LoadAction)
					{
					case ERHIRenderTargetLoadAction::Clear:
						Attachment.loadAction = MTLLoadActionClear;
						Attachment.clearColor = MTLClearColorMake(
							Info.ColorClearValues[Index].ClearValue.Color[0],
							Info.ColorClearValues[Index].ClearValue.Color[1],
							Info.ColorClearValues[Index].ClearValue.Color[2],
							Info.ColorClearValues[Index].ClearValue.Color[3]);
						break;
					case ERHIRenderTargetLoadAction::Load:
						Attachment.loadAction = MTLLoadActionLoad; break;
					case ERHIRenderTargetLoadAction::DontCare:
						Attachment.loadAction = MTLLoadActionDontCare; break;
					}
					Attachment.storeAction = Layout.StoreAction
						== ERHIRenderTargetStoreAction::Store
						? MTLStoreActionStore : MTLStoreActionDontCare;
					[Active->NativeResources addObject:Texture];
				}
				id<MTLTexture> DepthTexture = nil;
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
					Desc.depthAttachment.texture = DepthTexture;
					Desc.depthAttachment.slice =
						Info.DepthStencilRenderTargetView->GetDesc().Range.FirstArrayLayer;
					switch (DepthLayout.LoadAction)
					{
					case ERHIRenderTargetLoadAction::Clear:
						Desc.depthAttachment.loadAction = MTLLoadActionClear;
						Desc.depthAttachment.clearDepth =
							Info.DepthStencilClearValue.ClearValue.DSValue.Depth;
						break;
					case ERHIRenderTargetLoadAction::Load:
						Desc.depthAttachment.loadAction = MTLLoadActionLoad; break;
					case ERHIRenderTargetLoadAction::DontCare:
						Desc.depthAttachment.loadAction = MTLLoadActionDontCare; break;
					}
					Desc.depthAttachment.storeAction =
						DepthLayout.StoreAction == ERHIRenderTargetStoreAction::Store
							? MTLStoreActionStore : MTLStoreActionDontCare;
				}
				RenderEncoder = [Active->Command renderCommandEncoderWithDescriptor:Desc];
				requiref(RenderEncoder != nil, "Metal render encoder creation failed.");
				CurrentRenderTargetLayout = Info.RenderTargetLayout;
				RenderWidth = Color ? Color->GetSizeX()
					: Info.DepthStencilRenderTarget->GetSizeX();
				RenderHeight = Color ? Color->GetSizeY()
					: Info.DepthStencilRenderTarget->GetSizeY();
				if (DepthTexture) [Active->NativeResources addObject:DepthTexture];
			}
			auto RHIEndRenderPass() -> void override
			{
				requiref(RenderEncoder != nil, "Metal render pass is not active.");
				[RenderEncoder endEncoding];
				RenderEncoder = nil;
				GraphicsPipeline = nullptr;
				BoundVertexStreams = 0;
				IndexBuffer = nullptr;
				GraphicsParameters.clear();
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
				requiref(State && dynamic_cast<FMetalViewport*>(Viewport)
					&& !Active && !RenderEncoder,
					"Metal viewport drawing requires an idle context and viewport.");
			}
			auto RHIEndDrawingViewport(FRHIViewport* Viewport,
				bool bPresent, bool bLockToVsync) -> void override
			{
				auto* MetalViewport = dynamic_cast<FMetalViewport*>(Viewport);
				requiref(State && MetalViewport && !Active && !RenderEncoder,
					"Metal viewport presentation requires a closed GPU submission.");
				if (!bPresent) return;
				RHISubmitCommands();
				auto BackBuffer = MetalViewport->SnapshotBackBuffer();
				if (!BackBuffer) return;
				CAMetalLayer* Layer = MetalViewport->GetLayer();
				Layer.displaySyncEnabled = bLockToVsync;
				id<CAMetalDrawable> Drawable = [Layer nextDrawable];
				if (!Drawable) return;
				id<MTLTexture> Source =
					static_cast<FMetalTexture*>(BackBuffer.GetReference())->GetHandle();
				id<MTLTexture> Destination = Drawable.texture;
				if (Source.width != Destination.width
					|| Source.height != Destination.height
					|| Source.pixelFormat != Destination.pixelFormat) return;
				id<MTLCommandBuffer> Command = [State->Queue commandBuffer];
				requiref(Command != nil, "Metal presentation command allocation failed.");
				id<MTLBlitCommandEncoder> Encoder = [Command blitCommandEncoder];
				requiref(Encoder != nil, "Metal presentation blit encoder failed.");
				[Encoder copyFromTexture:Source sourceSlice:0 sourceLevel:0
					sourceOrigin:MTLOriginMake(0, 0, 0)
					sourceSize:MTLSizeMake(Source.width, Source.height, 1)
					toTexture:Destination destinationSlice:0 destinationLevel:0
					destinationOrigin:MTLOriginMake(0, 0, 0)];
				[Encoder endEncoding];
				[Command presentDrawable:Drawable];
				auto SharedState = State;
				[Command addCompletedHandler:^(id<MTLCommandBuffer>) {
					(void)Source;
					(void)Drawable;
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
			auto RHISetViewport(float MinX, float MinY, float MinZ,
				float MaxX, float MaxY, float MaxZ) -> void override
			{
				requiref(RenderEncoder && std::isfinite(MinX)
					&& std::isfinite(MinY) && std::isfinite(MinZ)
					&& std::isfinite(MaxX) && std::isfinite(MaxY)
					&& std::isfinite(MaxZ)
					&& MinX >= 0 && MinY >= 0 && MaxX > MinX && MaxY > MinY
					&& MaxX <= RenderWidth && MaxY <= RenderHeight
					&& MinZ >= 0 && MinZ <= 1 && MaxZ >= MinZ && MaxZ <= 1,
					"Invalid Metal viewport bounds.");
				const double MaxDepth = MinZ == MaxZ ? MinZ + 1.0 : MaxZ;
				[RenderEncoder setViewport:MTLViewport{
					MinX, MinY, MaxX - MinX, MaxY - MinY, MinZ, MaxDepth}];
				RHISetScissor(MinX, MinY, MaxX - MinX, MaxY - MinY);
			}
			auto RHISetScissor(float MinX, float MinY,
				float Width, float Height) -> void override
			{
				requiref(RenderEncoder && std::isfinite(MinX)
					&& std::isfinite(MinY) && std::isfinite(Width)
					&& std::isfinite(Height) && MinX >= 0 && MinY >= 0
					&& Width > 0 && Height > 0
					&& MinX + Width <= RenderWidth
					&& MinY + Height <= RenderHeight,
					"Invalid Metal scissor bounds.");
				[RenderEncoder setScissorRect:MTLScissorRect{
					static_cast<NSUInteger>(MinX),
					static_cast<NSUInteger>(MinY),
					static_cast<NSUInteger>(Width),
					static_cast<NSUInteger>(Height)}];
			}
			auto RHISetDepthBias(float ConstantFactor, float Clamp,
				float SlopeFactor) -> void override
			{
				requiref(RenderEncoder && GraphicsPipeline
					&& GraphicsPipeline->GetRasterizer().bEnableDepthBias
					&& std::isfinite(ConstantFactor) && std::isfinite(Clamp)
					&& std::isfinite(SlopeFactor),
					"Metal depth bias requires an active depth-bias pipeline and finite values.");
				[RenderEncoder setDepthBias:ConstantFactor
					slopeScale:SlopeFactor clamp:Clamp];
			}
			auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& State) -> void override
			{
				auto* Pipeline = dynamic_cast<FMetalGraphicsPipelineState*>(&State);
				requiref(RenderEncoder && Pipeline
					&& Pipeline->GetRenderTargets() == CurrentRenderTargetLayout,
					"Metal graphics pipeline does not match the active render pass.");
				GraphicsPipeline = Pipeline;
				GraphicsParameters.clear();
				GraphicsParameterOwners.clear();
				for (auto& Bytes : GraphicsPushConstants) Bytes.clear();
				for (auto& Written : GraphicsPushConstantWritten) Written.clear();
				[RenderEncoder setRenderPipelineState:Pipeline->GetPipeline()];
				[RenderEncoder setDepthStencilState:Pipeline->GetDepthStencil()];
				const auto& Raster = Pipeline->GetRasterizer();
				[RenderEncoder setCullMode:Raster.CullMode == ERHICullMode::None
					? MTLCullModeNone : Raster.CullMode == ERHICullMode::Front
						? MTLCullModeFront : MTLCullModeBack];
				[RenderEncoder setFrontFacingWinding:
					Raster.FrontFace == ERHIFrontFace::Clockwise
						? MTLWindingClockwise : MTLWindingCounterClockwise];
				[RenderEncoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
				Active->ResourceOwners.emplace_back(Pipeline);
			}
			auto RHISetComputePipelineState(FRHIComputePipelineState& State) -> void override
			{
				requiref(Active && !RenderEncoder,
					"Metal compute pipeline requires an active submission outside a render pass.");
				ComputePipeline = &State;
				ComputeParameters.clear();
				ComputeParameterOwners.clear();
				ComputePushConstants.clear();
				ComputePushConstantWritten.clear();
			}
			auto RHIBindVertexBuffer(uint32 Stream, FRHIBuffer* Resource,
				uint32 Offset) -> void override
			{
				requiref(RenderEncoder, "Metal vertex binding requires a render pass.");
				requiref(Stream < 16, "Metal vertex stream index exceeds 15.");
				if (!Resource)
				{
					[RenderEncoder setVertexBuffer:nil offset:0 atIndex:Stream];
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
				[RenderEncoder setVertexBuffer:Buffer->GetHandle()
					offset:Offset atIndex:Stream];
				BoundVertexStreams |= uint16(1u << Stream);
				[Active->NativeResources addObject:Buffer->GetHandle()];
				Active->ResourceOwners.emplace_back(Buffer);
			}
			auto RHIBindIndexBuffer(FRHIBuffer* Resource, uint32 Offset) -> void override
			{
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
				[Active->NativeResources addObject:
					static_cast<FMetalBuffer*>(Resource)->GetHandle()];
				Active->ResourceOwners.emplace_back(Resource);
			}
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
				requiref(State && !RenderEncoder && Buffer
					&& Offset <= Buffer->GetSize()
					&& Data.size() <= Buffer->GetSize() - Offset,
					"Metal buffer upload exceeds its target range.");
				if (Data.empty()) return;
				const bool bImplicitUpload = !Active;
				if (bImplicitUpload)
				{
					FMetalPendingSubmission Submission;
					Submission.Command = [State->Queue commandBuffer];
					requiref(Submission.Command != nil,
						"Metal implicit buffer upload command allocation failed.");
					if (StorageOwner) Submission.StorageOwners.push_back(StorageOwner);
					Active.emplace(std::move(Submission));
				}
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
					&& ToMetalPixelFormat(Texture->GetFormat()) != MTLPixelFormatInvalid,
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
				const NSUInteger RowPitch = Layout.RowPitch;
				const NSUInteger ByteCount = Layout.DataSize;
				id<MTLBuffer> Staging = [State->Queue.device newBufferWithLength:ByteCount
					options:MTLResourceStorageModeShared];
				requiref(Staging != nil, "Metal texture upload staging allocation failed.");
				auto* TargetBytes = static_cast<std::byte*>(Staging.contents);
				for (uint32 Row = 0; Row < Layout.BlocksHigh; ++Row)
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
				const auto Layout = GetPixelFormatLayout(Texture->GetFormat(), Width, Height);
				const NSUInteger RowPitch = Layout.RowPitch;
				const NSUInteger ByteCount = Layout.DataSize;
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
				const auto Layout = GetPixelFormatLayout(Texture->GetFormat(), Width, Height);
				const NSUInteger RowPitch = Layout.RowPitch;
				const NSUInteger ByteCount = Layout.DataSize;
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
				auto* Pipeline = static_cast<FMetalComputePipelineState*>(
					ComputePipeline.GetReference());
				requiref(Active && !RenderEncoder && Pipeline
					&& (ArgumentBuffer || (X && Y && Z))
					&& ValidateShaderBindingCompleteness(Pipeline->GetLayout(),
						ComputeParameters).has_value(),
					"Invalid Metal compute dispatch state.");
				id<MTLComputeCommandEncoder> Encoder =
					[Active->Command computeCommandEncoder];
				requiref(Encoder != nil, "Metal compute encoder creation failed.");
				[Encoder setComputePipelineState:Pipeline->GetPipeline()];
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
					id<MTLBuffer> Constants = [State->Queue.device
						newBufferWithBytes:ComputePushConstants.data() length:End
						options:MTLResourceStorageModeShared];
					requiref(Constants != nil,
						"Metal compute push constant allocation failed.");
					[Encoder setBuffer:Constants offset:0 atIndex:
						Pipeline->GetShader()->GetMetalPushConstantBufferSlot()];
					[Active->NativeResources addObject:Constants];
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
						const auto Buffer = ResolveBufferBinding(View);
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
						[Encoder setBuffer:Buffer.Handle offset:Offset atIndex:Slot];
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
						[Encoder setTexture:View->GetHandle() atIndex:Slot];
						[Active->NativeResources addObject:View->GetHandle()];
					}
					else if (Parameter.Type == ERHIBindingType::Sampler)
					{
						requiref(Parameter.Resource->GetResourceType()
							== ERHIResourceType::Sampler,
							"Metal compute sampler binding requires a sampler.");
						auto* Sampler = dynamic_cast<FMetalSampler*>(
							static_cast<FRHISampler*>(Parameter.Resource));
						requiref(Sampler, "Metal compute sampler belongs to another backend.");
						[Encoder setSamplerState:Sampler->GetHandle() atIndex:Slot];
					}
					else Unsupported();
					Active->ResourceOwners.emplace_back(Parameter.Resource);
				}
				Active->ResourceOwners.emplace_back(Pipeline);
				const auto Group = Pipeline->GetShader()->GetComputeThreadGroupSize();
				if (ArgumentBuffer)
				{
					auto* Buffer = static_cast<FMetalBuffer*>(ArgumentBuffer);
					[Encoder dispatchThreadgroupsWithIndirectBuffer:Buffer->GetHandle()
						indirectBufferOffset:Offset
						threadsPerThreadgroup:MTLSizeMake(Group[0], Group[1], Group[2])];
					[Active->NativeResources addObject:Buffer->GetHandle()];
					Active->ResourceOwners.emplace_back(ArgumentBuffer);
				}
				else [Encoder dispatchThreadgroups:MTLSizeMake(X, Y, Z)
					threadsPerThreadgroup:MTLSizeMake(Group[0], Group[1], Group[2])];
				[Encoder endEncoding];
			}
			auto RHIDraw(const FRHIDrawArguments& Args) -> void override
			{
				requiref(RenderEncoder && GraphicsPipeline && Args.VertexCount
					&& Args.InstanceCount
					&& (BoundVertexStreams
						& GraphicsPipeline->GetRequiredVertexStreams())
						== GraphicsPipeline->GetRequiredVertexStreams(),
					"Metal draw requires a render pipeline and nonempty arguments.");
				BindGraphicsParameters();
				[RenderEncoder drawPrimitives:MTLPrimitiveTypeTriangle
					vertexStart:Args.FirstVertex vertexCount:Args.VertexCount
					instanceCount:Args.InstanceCount baseInstance:Args.FirstInstance];
			}
			auto RHIDrawIndexed(const FRHIDrawIndexedArguments& Args) -> void override
			{
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
				[RenderEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
					indexCount:Args.IndexCount
					indexType:IndexBuffer->GetStride() == 2
						? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
					indexBuffer:static_cast<FMetalBuffer*>(
						IndexBuffer.GetReference())->GetHandle()
					indexBufferOffset:ByteOffset
					instanceCount:Args.InstanceCount
					baseVertex:Args.VertexOffset
					baseInstance:Args.FirstInstance];
			}
			auto RHIDrawIndirect(FRHIBuffer* ArgumentBuffer,
				uint64 Offset) -> void override
			{
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
				[RenderEncoder drawPrimitives:MTLPrimitiveTypeTriangle
					indirectBuffer:Buffer->GetHandle()
					indirectBufferOffset:Offset];
				[Active->NativeResources addObject:Buffer->GetHandle()];
				Active->ResourceOwners.emplace_back(ArgumentBuffer);
			}
			auto RHIDrawIndexedIndirect(FRHIBuffer* ArgumentBuffer,
				uint64 Offset) -> void override
			{
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
				[RenderEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
					indexType:IndexBuffer->GetStride() == 2
						? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
					indexBuffer:static_cast<FMetalBuffer*>(
						IndexBuffer.GetReference())->GetHandle()
					indexBufferOffset:IndexBufferOffset
					indirectBuffer:Buffer->GetHandle()
					indirectBufferOffset:Offset];
				[Active->NativeResources addObject:Buffer->GetHandle()];
				Active->ResourceOwners.emplace_back(ArgumentBuffer);
			}
		private:
			struct FBufferBinding
			{
				id<MTLBuffer> Handle;
				uint64 Size;
			};

			auto ResolveBufferBinding(FRHIBufferView* View) -> FBufferBinding
			{
				auto* Logical = View->GetBuffer();
				if (IsCPUAuthoredBuffer(Logical))
				{
					// Reuse one immutable native copy for each ordered content version.
					const auto Snapshot = FRHIDeferredBufferBackend::ResolveSnapshot(*Logical);
					auto Backing = std::static_pointer_cast<FMetalDeferredBacking>(
						FRHIDeferredBufferBackend::GetBacking(*Snapshot, State.get()));
					if (!Backing)
					{
						const auto Data = Snapshot->GetData();
						requiref(!Data.empty(), "Metal deferred buffer has no data.");
						Backing = std::make_shared<FMetalDeferredBacking>();
						Backing->Snapshot = Snapshot;
						Backing->Handle = [State->Queue.device
							newBufferWithBytes:Data.data() length:Data.size()
							options:MTLResourceStorageModeShared];
						requiref(Backing->Handle != nil,
							"Metal deferred buffer allocation failed.");
						FRHIDeferredBufferBackend::SetBacking(
							*Snapshot, State.get(), Backing);
					}
					Active->StorageOwners.push_back(Backing);
					[Active->NativeResources addObject:Backing->Handle];
					return {Backing->Handle, Snapshot->GetData().size()};
				}
				auto* Buffer = dynamic_cast<FMetalBuffer*>(Logical);
				requiref(Buffer && Buffer->GetHandle(),
					"Metal buffer binding has no native buffer.");
				[Active->NativeResources addObject:Buffer->GetHandle()];
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
					id<MTLBuffer> Constants = [State->Queue.device
						newBufferWithBytes:GraphicsPushConstants[StageIndex].data()
						length:End options:MTLResourceStorageModeShared];
					requiref(Constants != nil,
						"Metal graphics push constant allocation failed.");
					if (StageIndex == 0)
						[RenderEncoder setVertexBuffer:Constants offset:0
							atIndex:Shader->GetMetalPushConstantBufferSlot()];
					else [RenderEncoder setFragmentBuffer:Constants offset:0
							atIndex:Shader->GetMetalPushConstantBufferSlot()];
					[Active->NativeResources addObject:Constants];
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
								[RenderEncoder setVertexTexture:View->GetHandle()
									atIndex:Slot];
							else [RenderEncoder setFragmentTexture:View->GetHandle()
									atIndex:Slot];
							[Active->NativeResources addObject:View->GetHandle()];
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
								[RenderEncoder setVertexSamplerState:Sampler->GetHandle()
									atIndex:Slot];
							else [RenderEncoder setFragmentSamplerState:Sampler->GetHandle()
									atIndex:Slot];
						}
						else if (Parameter.Type == ERHIBindingType::UniformBuffer
							|| Parameter.Type == ERHIBindingType::UniformBufferDynamic
							|| Parameter.Type == ERHIBindingType::StorageBuffer)
						{
							requiref(Parameter.Resource->GetResourceType()
								== ERHIResourceType::BufferView,
								"Metal graphics buffer binding requires a view.");
							auto* View = static_cast<FRHIBufferView*>(Parameter.Resource);
							const auto Buffer = ResolveBufferBinding(View);
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
								[RenderEncoder setVertexBuffer:Buffer.Handle
									offset:Offset atIndex:Slot];
							else [RenderEncoder setFragmentBuffer:Buffer.Handle
									offset:Offset atIndex:Slot];
						}
						else Unsupported();
					}
					Active->ResourceOwners.emplace_back(Parameter.Resource);
				}
			}
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
				bImplicitRenderSubmission = false;
				RenderEncoder = nil;
				ComputePipeline = nullptr;
				ComputeParameters.clear();
				ComputeParameterOwners.clear();
				ComputePushConstants.clear();
				ComputePushConstantWritten.clear();
				GraphicsPipeline = nullptr;
				BoundVertexStreams = 0;
				IndexBuffer = nullptr;
				GraphicsParameters.clear();
				GraphicsParameterOwners.clear();
				for (auto& Bytes : GraphicsPushConstants) Bytes.clear();
				for (auto& Written : GraphicsPushConstantWritten) Written.clear();
				StorageOwner.reset();
				bFrameOpen = false;
			}
			friend class FMetalDynamicRHI;
			static auto Unsupported() -> void
			{
				requiref(false, "Metal command execution has not been implemented.");
			}
			std::shared_ptr<FMetalSubmissionState> State;
			std::shared_ptr<void> StorageOwner;
			std::optional<FMetalPendingSubmission> Active;
			bool bImplicitRenderSubmission = false;
			bool bFrameOpen = false;
			id<MTLRenderCommandEncoder> RenderEncoder = nil;
			FRHIRenderTargetLayout CurrentRenderTargetLayout;
			uint32 RenderWidth = 0;
			uint32 RenderHeight = 0;
			TRefCountPtr<FMetalGraphicsPipelineState> GraphicsPipeline;
			uint16 BoundVertexStreams = 0;
			FBufferRHIRef IndexBuffer;
			uint32 IndexBufferOffset = 0;
			std::vector<FRHIShaderParameterResource> GraphicsParameters;
			std::vector<TRefCountPtr<FRHIResource>> GraphicsParameterOwners;
			std::array<std::vector<std::byte>, 2> GraphicsPushConstants;
			std::array<std::vector<uint8>, 2> GraphicsPushConstantWritten;
			TRefCountPtr<FRHIComputePipelineState> ComputePipeline;
			std::vector<FRHIShaderParameterResource> ComputeParameters;
			std::vector<TRefCountPtr<FRHIResource>> ComputeParameterOwners;
			std::vector<std::byte> ComputePushConstants;
			std::vector<uint8> ComputePushConstantWritten;
			std::vector<FMetalPendingSubmission> Pending;
		};

		class FMetalDynamicRHI final : public FDynamicRHI
		{
		public:
			auto Init(const FRHIInitializationContext& Context) -> void override
			{
				if (const auto& Target = Context.GetPresentationTarget())
				{
					StartupLayer = (__bridge CAMetalLayer*)Target->NativeMetalLayer;
					if (!StartupLayer
						|| ![StartupLayer isKindOfClass:[CAMetalLayer class]])
						throw std::runtime_error("Metal presentation requires a CAMetalLayer.");
					StartupWindow = Target->NativeWindowHandle;
				}
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
				FRHICapabilities Supported;
				Supported.SupportedTextureDimensions =
					ERHITextureDimensionFlags::Texture2D;
				Supported.MaxTextureDimension2D = 8192;
				Supported.MaxTextureDimensionCube = 8;
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
				PublishCapabilities(std::move(Supported));
				PipelineCreationClosed = false;
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
						@autoreleasepool {
							return RHICreateGraphicsPipelineState(FName(Inputs.DebugName), Inputs.Initializer);
						}
					};
					Backend.CreateCompute = [this](const FRHIComputePipelineCreationInputs& Inputs,
						const FComputePipelineStateKey&) -> FComputePipelineStateRHIRef {
						@autoreleasepool {
							return RHICreateComputePipelineState(FName(Inputs.DebugName), Inputs.Initializer);
						}
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
			RHIStopPipelineCreation();
			RHIRetirePipelineCreationResults();
			PipelineCache.reset();
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
				StartupLayer = nil;
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
			{ CommandContext.RHIBeginFrame(Args); }
			auto RHIEndFrame() -> void override
			{ CommandContext.RHIEndFrame(); }
			auto RHICreateViewport(const FRHIViewportCreateInfo& Info)
			-> TRefCountPtr<FRHIViewport> override
			{
				if (!Device || !Info.NativeWindowHandle || !Info.NativeMetalLayer
					|| !Info.SizeX || !Info.SizeY) return nullptr;
				CAMetalLayer* Layer = (__bridge CAMetalLayer*)Info.NativeMetalLayer;
				if (![Layer isKindOfClass:[CAMetalLayer class]]) return nullptr;
				if (Info.bAdoptInitializationPresentationCandidate)
				{
					if (!StartupLayer || Layer != StartupLayer
						|| Info.NativeWindowHandle != StartupWindow) return nullptr;
				}
				const auto Format = Info.PreferredPixelFormat
					== EPixelFormat::RGBA16_FLOAT
					? EPixelFormat::RGBA16_FLOAT
					: Info.PreferredPixelFormat == EPixelFormat::RGBA8_UNORM
						|| Info.PreferredPixelFormat == EPixelFormat::BGRA8_UNORM
						? EPixelFormat::BGRA8_UNORM
						: EPixelFormat::SBGRA8_UNORM;
				auto Viewport = MakeRefCount<FMetalViewport>(
					Device, Layer, Info.SizeX, Info.SizeY, Format);
				if (!Viewport->SnapshotBackBuffer()) return nullptr;
				if (Info.bAdoptInitializationPresentationCandidate)
				{
					StartupLayer = nil;
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
			{
				if (!Device || !Initializer.BoundShaders.VertexShader
					|| !Initializer.BoundShaders.FragmentShader
					|| !Initializer.VertexDeclaration
					|| Initializer.PrimitiveTopology !=
						FGraphicsPipelineStateInitializer::EPrimitiveTopology::TriangleList
					|| Initializer.RenderTargetLayout.NumColorRenderTargets > 4
					|| (Initializer.RenderTargetLayout.NumColorRenderTargets == 0
						&& !Initializer.RenderTargetLayout.bHasDepthStencil)
					|| (Initializer.RenderTargetLayout.bHasDepthStencil
						&& Initializer.RenderTargetLayout.DepthStencilAttachment.Format
							!= EPixelFormat::D32)
					|| Initializer.MultisampleState != FRHIMultisampleState{}
					|| Initializer.DepthStencilState.bEnableStencil
					|| (!Initializer.RenderTargetLayout.bHasDepthStencil
						&& Initializer.DepthStencilState != FRHIDepthStencilState{})
					|| Initializer.RasterizerState.PolygonMode != ERHIPolygonMode::Fill
					|| Initializer.RasterizerState.bEnableDepthClamp
					|| Initializer.RasterizerState.LineWidth != 1.0f)
					return nullptr;
				auto Key = BuildGraphicsPipelineStateKey(Initializer, nullptr);
				if (!Key || Key->Target != MetalShaderTarget) return nullptr;
				auto* Vertex = dynamic_cast<FMetalShader*>(
					Initializer.BoundShaders.VertexShader);
				auto* Fragment = dynamic_cast<FMetalShader*>(
					Initializer.BoundShaders.FragmentShader);
				if (!Vertex || !Fragment)
					return nullptr;
				const auto MatchesStage = [&](FMetalShader* Shader,
				EShaderStageFlags Stage) {
					const auto Map = Shader->GetMetalBindings();
					size_t Count = 0;
					for (uint32 SetIndex = 0;
						SetIndex < Key->PipelineLayout.BindingLayouts.size(); ++SetIndex)
						for (const auto& Binding : Key->PipelineLayout
							.BindingLayouts[SetIndex].BindingLayouts)
						{
							if (!EnumHasAnyFlags(Binding.StageFlags, Stage)) continue;
							if (Count >= Map.size()
								|| Map[Count].SetIndex != SetIndex
								|| Map[Count].BindingIndex != Binding.Slot
								|| Map[Count].Type != (Binding.Type == ERHIBindingType::UniformBufferDynamic
									? ERHIBindingType::UniformBuffer : Binding.Type)
								|| Map[Count].Count != Binding.ArraySize)
								return false;
							++Count;
						}
					return Count == Map.size();
				};
				if (!MatchesStage(Vertex, EShaderStageFlags::Vertex)
					|| !MatchesStage(Fragment, EShaderStageFlags::Fragment))
					return nullptr;
				for (const auto Stage : {EShaderStageFlags::Vertex,
					EShaderStageFlags::Fragment})
				{
					const bool bPush = std::ranges::any_of(
						Key->PipelineLayout.PushConstantRanges,
						[&](const auto& Range) {
							return EnumHasAnyFlags(Range.StageFlags, Stage);
						});
					auto* Shader = Stage == EShaderStageFlags::Vertex
						? Vertex : Fragment;
					if (bPush != (Shader->GetMetalPushConstantBufferSlot()
						!= UINT32_MAX)) return nullptr;
				}
				for (const auto& Range : Key->PipelineLayout.PushConstantRanges)
					if (uint64(Range.Offset) + Range.Size > 65536)
						return nullptr;
				MTLRenderPipelineDescriptor* Desc = [MTLRenderPipelineDescriptor new];
				Desc.vertexFunction = Vertex->GetFunction();
				Desc.fragmentFunction = Fragment->GetFunction();
				Desc.sampleCount = 1;
				Desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
				for (uint32 Index = 0;
					Index < Initializer.RenderTargetLayout.NumColorRenderTargets; ++Index)
				{
				const auto Format = Initializer.RenderTargetLayout.ColorAttachments[Index]
					.RenderTarget.Format;
				if (!IsMetalColorRenderFormat(Format))
					return nullptr;
				Desc.colorAttachments[Index].pixelFormat = ToMetalPixelFormat(Format);
				const auto& Blend = Initializer.ColorBlendStates[Index];
				auto* NativeBlend = Desc.colorAttachments[Index];
					NativeBlend.blendingEnabled = Blend.bEnable;
					MTLColorWriteMask WriteMask = MTLColorWriteMaskNone;
					if (EnumHasAnyFlags(Blend.ColorWriteMask, ERHIColorWriteMask::Red))
						WriteMask |= MTLColorWriteMaskRed;
					if (EnumHasAnyFlags(Blend.ColorWriteMask, ERHIColorWriteMask::Green))
						WriteMask |= MTLColorWriteMaskGreen;
					if (EnumHasAnyFlags(Blend.ColorWriteMask, ERHIColorWriteMask::Blue))
						WriteMask |= MTLColorWriteMaskBlue;
					if (EnumHasAnyFlags(Blend.ColorWriteMask, ERHIColorWriteMask::Alpha))
						WriteMask |= MTLColorWriteMaskAlpha;
					NativeBlend.writeMask = WriteMask;
					if (Blend.bEnable)
					{
						const auto SrcColor = ToMetalBlendFactor(Blend.SrcColorFactor);
						const auto DstColor = ToMetalBlendFactor(Blend.DstColorFactor);
						const auto SrcAlpha = ToMetalBlendFactor(Blend.SrcAlphaFactor);
						const auto DstAlpha = ToMetalBlendFactor(Blend.DstAlphaFactor);
						const auto ColorOp = ToMetalBlendOperation(Blend.ColorOp);
						const auto AlphaOp = ToMetalBlendOperation(Blend.AlphaOp);
						if (!SrcColor || !DstColor || !SrcAlpha || !DstAlpha
							|| !ColorOp || !AlphaOp) return nullptr;
						NativeBlend.sourceRGBBlendFactor = *SrcColor;
						NativeBlend.destinationRGBBlendFactor = *DstColor;
						NativeBlend.rgbBlendOperation = *ColorOp;
						NativeBlend.sourceAlphaBlendFactor = *SrcAlpha;
						NativeBlend.destinationAlphaBlendFactor = *DstAlpha;
						NativeBlend.alphaBlendOperation = *AlphaOp;
					}
				}
				if (Initializer.RenderTargetLayout.bHasDepthStencil)
					Desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
				MTLVertexDescriptor* VertexDesc = [MTLVertexDescriptor vertexDescriptor];
				for (const auto& Element : Initializer.VertexDeclaration->GetElements())
				{
					if (Element.Type == EVertexElementType::None) break;
					const MTLVertexFormat Format = ToMetalVertexFormat(Element.Type);
					if (Format == MTLVertexFormatInvalid
						|| Element.StreamIndex >= 16 || Element.AttributeIndex >= 31)
						return nullptr;
					auto* Attribute = VertexDesc.attributes[Element.AttributeIndex];
					Attribute.format = Format;
					Attribute.offset = Element.Offset;
					Attribute.bufferIndex = Element.StreamIndex;
					auto* Layout = VertexDesc.layouts[Element.StreamIndex];
					Layout.stride = Element.Stride;
					Layout.stepFunction = Element.InputRate
						== FRHIVertexElementIdentity::EInputRate::Instance
						? MTLVertexStepFunctionPerInstance
						: MTLVertexStepFunctionPerVertex;
				}
				Desc.vertexDescriptor = VertexDesc;
				NSError* Error = nil;
				id<MTLRenderPipelineState> Pipeline = [Device
					newRenderPipelineStateWithDescriptor:Desc error:&Error];
				if (!Pipeline)
				{
					DURIN_ERROR("Metal graphics pipeline creation failed: {}",
						Error ? Error.localizedDescription.UTF8String : "unknown error");
					return nullptr;
				}
				id<MTLDepthStencilState> DepthStencil = nil;
				if (Initializer.RenderTargetLayout.bHasDepthStencil)
				{
					const auto Compare = ToMetalDepthCompare(
						Initializer.DepthStencilState.CompareOp);
					if (!Compare) return nullptr;
					MTLDepthStencilDescriptor* DepthDesc = [MTLDepthStencilDescriptor new];
					DepthDesc.depthCompareFunction =
						Initializer.DepthStencilState.bEnableTest
							? *Compare : MTLCompareFunctionAlways;
					DepthDesc.depthWriteEnabled =
						Initializer.DepthStencilState.bEnableWrite;
					DepthStencil = [Device newDepthStencilStateWithDescriptor:DepthDesc];
					if (!DepthStencil) return nullptr;
				}
				return new FMetalGraphicsPipelineState(Vertex, Fragment,
					Initializer.VertexDeclaration, Pipeline,
					Initializer.RenderTargetLayout, Initializer.RasterizerState,
					std::move(Key->PipelineLayout), DepthStencil);
			}
			auto RHICreateComputePipelineState(FName,
			const FComputePipelineStateInitializer& Initializer)
			-> TRefCountPtr<FRHIComputePipelineState> override
			{
				if (!Device || !Initializer.ComputeShader
					|| Initializer.ComputeShader->GetTarget() != MetalShaderTarget)
					return nullptr;
				auto Key = BuildComputePipelineStateKey(Initializer, nullptr);
				if (!Key) return nullptr;
				auto* Shader = dynamic_cast<FMetalShader*>(Initializer.ComputeShader);
				if (!Shader) return nullptr;
				const auto Bindings = Shader->GetMetalBindings();
				size_t Count = 0;
				for (uint32 SetIndex = 0;
					SetIndex < Key->PipelineLayout.BindingLayouts.size(); ++SetIndex)
					for (const auto& Binding : Key->PipelineLayout.BindingLayouts[SetIndex].BindingLayouts)
					{
						if (Count >= Bindings.size()
							|| Bindings[Count].SetIndex != SetIndex
							|| Bindings[Count].BindingIndex != Binding.Slot
							|| Bindings[Count].Type != (Binding.Type == ERHIBindingType::UniformBufferDynamic
								? ERHIBindingType::UniformBuffer : Binding.Type)
							|| Bindings[Count].Count != Binding.ArraySize)
							return nullptr;
						++Count;
					}
				if (Count != Bindings.size()
					|| Key->PipelineLayout.PushConstantRanges.empty()
						!= (Shader->GetMetalPushConstantBufferSlot() == UINT32_MAX))
					return nullptr;
				for (const auto& Range : Key->PipelineLayout.PushConstantRanges)
					if (uint64(Range.Offset) + Range.Size > 65536)
						return nullptr;
				NSError* Error = nil;
				id<MTLComputePipelineState> Pipeline = [Device
					newComputePipelineStateWithFunction:Shader->GetFunction()
					error:&Error];
				if (!Pipeline)
				{
					DURIN_ERROR("Metal compute pipeline creation failed: {}",
						Error ? Error.localizedDescription.UTF8String : "unknown error");
					return nullptr;
				}
				const auto Group = Shader->GetComputeThreadGroupSize();
				if (uint64(Group[0]) * Group[1] * Group[2]
					> Pipeline.maxTotalThreadsPerThreadgroup) return nullptr;
				return new FMetalComputePipelineState(Shader,
					std::move(Key->PipelineLayout), Pipeline);
			}
			auto RHIGetDefaultContext() -> IRHICommandContext* override
			{ return &CommandContext; }
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
							== MTLVertexFormatInvalid)
						return nullptr;
				}
				return new FMetalVertexDeclaration(Elements);
			}
			auto RHIIsTextureSupported(const FRHITextureCreateDesc& Desc) const -> bool override
			{
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
				const bool bBC1 = Desc.Format == EPixelFormat::BC1_UNORM
					|| Desc.Format == EPixelFormat::BC1_UNORM_SRGB;
				return Device && ValidateTextureCreateDesc(Desc)
					&& (b2D || b2DArray || bCube || bCubeArray || b3D)
					&& ToMetalPixelFormat(Desc.Format) != MTLPixelFormatInvalid
					&& (!bBC1 || (Device.supportsBCTextureCompression
						&& (static_cast<uint64>(Desc.Flags)
							& ~static_cast<uint64>(ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::CPUReadback)) == 0))
					&& (b2D || Desc.Format == EPixelFormat::RGBA8_UNORM
						|| (b3D && Desc.Format == EPixelFormat::R8_UNORM)
						|| (b2DArray && Desc.Format == EPixelFormat::D32)
						|| (bCube && (Desc.Format == EPixelFormat::RGBA16_FLOAT
						|| Desc.Format == EPixelFormat::RGBA32_FLOAT
						|| Desc.Format == EPixelFormat::BC1_UNORM
						|| Desc.Format == EPixelFormat::BC1_UNORM_SRGB)))
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
				if (EnumHasAnyFlags(Desc.Flags,
					ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::DepthStencilTargetable))
					Native.usage |= MTLTextureUsageRenderTarget;
				if (EnumHasAnyFlags(Desc.Flags, ETextureCreateFlags::ShaderResource))
					Native.usage |= MTLTextureUsageShaderRead;
				if (EnumHasAnyFlags(Desc.Flags, ETextureCreateFlags::Storage))
					Native.usage |= MTLTextureUsageShaderWrite;
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
			auto RHICreateShader(const FRHIShaderCreateDesc& Desc)
			-> TRefCountPtr<FRHIShader> override
			{
				if (!Device || Desc.Target != MetalShaderTarget
					|| Desc.CodeFormat != EShaderCodeFormat::Msl20Source
					|| !Desc.EntryPoint || !*Desc.EntryPoint
					|| Desc.Code.size() < 32
					|| std::ranges::find(Desc.Code, std::byte{0}) != Desc.Code.end()
					|| FXxHash128::HashBuffer(Desc.Code) != Desc.Hash
					|| (Desc.Frequency == EShaderFrequency::Compute
						&& !IsValidComputeThreadGroupSize(Desc.ComputeThreadGroupSize))
					|| (Desc.Frequency != EShaderFrequency::Compute
						&& Desc.ComputeThreadGroupSize != std::array<uint32, 3>{})
					|| !ValidateMetalBindingRemap(Desc.Frequency,
						Desc.MetalBindings, Desc.MetalPushConstantBufferSlot,
						Desc.BindingRemapIdentity)) return nullptr;
				const std::string_view Source(
					reinterpret_cast<const char*>(Desc.Code.data()), Desc.Code.size());
				if (!Source.substr(0, 256).contains("#include <metal_stdlib>"))
					return nullptr;
				NSString* Text = [[NSString alloc] initWithBytes:Desc.Code.data()
					length:Desc.Code.size() encoding:NSUTF8StringEncoding];
				if (!Text) return nullptr;
				MTLCompileOptions* Options = [MTLCompileOptions new];
				Options.languageVersion = MTLLanguageVersion2_0;
				NSError* Error = nil;
				id<MTLLibrary> Library = [Device newLibraryWithSource:Text
					options:Options error:&Error];
				if (!Library)
				{
					DURIN_ERROR("Metal shader library compilation failed: {}",
						Error ? Error.localizedDescription.UTF8String : "unknown error");
					return nullptr;
				}
				NSString* Entry = [NSString stringWithUTF8String:Desc.EntryPoint];
				if (!Entry) return nullptr;
				id<MTLFunction> Function = [Library newFunctionWithName:Entry];
				if (!Function) return nullptr;
				const MTLFunctionType Expected = Desc.Frequency == EShaderFrequency::Vertex
					? MTLFunctionTypeVertex
					: Desc.Frequency == EShaderFrequency::Fragment
						? MTLFunctionTypeFragment : MTLFunctionTypeKernel;
				if (Function.functionType != Expected) return nullptr;
				return new FMetalShader(Desc, Library, Function);
			}
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
					|| (Desc.Usage != ERHITextureViewUsage::Sampled
						&& Desc.Usage != ERHITextureViewUsage::Storage
						&& Desc.Usage != ERHITextureViewUsage::TransferSource
						&& Desc.Usage != ERHITextureViewUsage::TransferDestination
						&& Desc.Usage != ERHITextureViewUsage::ColorAttachment
						&& Desc.Usage != ERHITextureViewUsage::DepthStencilAttachment))
					return nullptr;
			if (Desc.Usage == ERHITextureViewUsage::DepthStencilAttachment
				&& (Texture->GetDimension() != ETextureDimension::Texture2D
					&& Texture->GetDimension() != ETextureDimension::Texture2DArray
						|| Texture->GetFormat() != EPixelFormat::D32
						|| Desc.Dimension != ERHITextureViewDimension::Texture2D
						|| Desc.Range.FirstMip != 0 || Desc.Range.NumMips != 1
						|| Desc.Range.NumArrayLayers != 1))
					return nullptr;
			if (Desc.Usage == ERHITextureViewUsage::Sampled
				|| Desc.Usage == ERHITextureViewUsage::Storage)
			{
				MTLTextureType Type = MTLTextureType2D;
				if (Desc.Usage == ERHITextureViewUsage::Storage)
				{
					if (Desc.Dimension != ERHITextureViewDimension::Texture2D)
						return nullptr;
				}
				else switch (Desc.Dimension)
				{
				case ERHITextureViewDimension::Texture2D: break;
				case ERHITextureViewDimension::Texture2DArray:
					Type = MTLTextureType2DArray; break;
				case ERHITextureViewDimension::TextureCube:
					Type = MTLTextureTypeCube; break;
				case ERHITextureViewDimension::TextureCubeArray:
					Type = MTLTextureTypeCubeArray; break;
				case ERHITextureViewDimension::Texture3D:
					Type = MTLTextureType3D; break;
				default: return nullptr;
				}
				id<MTLTexture> Native = static_cast<FMetalTexture*>(Texture)
					->GetHandle();
				id<MTLTexture> View = [Native newTextureViewWithPixelFormat:
					ToMetalPixelFormat(Desc.Format)
					textureType:Type
					levels:NSMakeRange(Desc.Range.FirstMip, Desc.Range.NumMips)
					slices:NSMakeRange(Desc.Range.FirstArrayLayer,
						Desc.Range.NumArrayLayers)];
					return View ? TRefCountPtr<FRHITextureView>(
						new FMetalTextureView(Texture, Desc, View)) : nullptr;
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
			id<MTLDevice> Device = nil;
			id<MTLCommandQueue> Queue = nil;
			CAMetalLayer* StartupLayer = nil;
			void* StartupWindow = nullptr;
			std::shared_ptr<FMetalSubmissionState> State;
			FRHIQueueCapabilities QueueCapabilities;
			FMetalCommandContext CommandContext;
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

	IMPLEMENT_MODULE(FMetalDynamicRHIModule, MetalRHI)
}
