#include "../../RDGTestAccess.h"
#include "RDG.h"

#include "RHICommandList.h"
#include "RHIContext.h"
#include "Shader/Shader.h"

#include <gtest/gtest.h>

#include <chrono>
#include <bit>
#include <cstdio>
#include <thread>

namespace Durin
{

	namespace
	{
		template<typename Builder>
		concept CPublicRawPass = requires(Builder& Graph) {
			Graph.AddPass("Raw", ERDGPassType::Graphics);
		};
		template<typename Builder>
		concept CPublicManualUse = requires(Builder& Graph, FRDGPassHandle Pass,
			FRDGTokenHandle Token) { Graph.UseToken(Pass, Token, ERDGUse::Write); };
		static_assert(!CPublicRawPass<FRDGBuilder>);
		static_assert(!CPublicManualUse<FRDGBuilder>);

		auto ExpectCapturedBarriersMatchPlan(const FRDGBuilder& Builder) -> void
		{
			const auto Capture = Builder.Capture();
			EXPECT_EQ(Capture.ExecutionPlan, Builder.GetExecutionPlan());
			size_t HandoffCount = 0;
			for (const auto& Handoff : Capture.ExecutionPlan.Handoffs)
			{
				ASSERT_LT(Handoff.Consumer.Index, Capture.ExecutionPlan.Batches.size());
				for (const auto Producer : Handoff.Producers)
					EXPECT_LT(Producer.Index, Handoff.Consumer.Index);
				const auto& Batch = Capture.ExecutionPlan.Batches[Handoff.Consumer.Index];
				const auto& Barriers = Batch.bEpilogue ? Builder.GetFinalBarriers()
					: Builder.GetPasses()[Batch.FirstPass].Barriers;
				if (Handoff.bTexture)
				{
					ASSERT_LT(Handoff.TransitionIndex, Barriers.GetTextureTransitions().size());
					EXPECT_EQ(Handoff.ResourceId, Barriers.GetTextureTransitions()[Handoff.TransitionIndex].ResourceId);
				}
				else
				{
					ASSERT_LT(Handoff.TransitionIndex, Barriers.GetBufferTransitions().size());
					EXPECT_EQ(Handoff.ResourceId, Barriers.GetBufferTransitions()[Handoff.TransitionIndex].ResourceId);
				}
				++HandoffCount;
			}
			EXPECT_EQ(HandoffCount, Capture.Statistics.BufferTransitions + Capture.Statistics.TextureTransitions);
			for (uint32 PassIndex = 0; PassIndex <= Builder.GetPasses().size(); ++PassIndex)
			{
				const bool bFinal = PassIndex == Builder.GetPasses().size();
				const auto Buffers = bFinal ? Builder.GetFinalBarriers().GetBufferTransitions()
					: std::span<const FRDGBufferTransition>(Builder.GetPasses()[PassIndex].Barriers.GetBufferTransitions());
				const auto Textures = bFinal ? Builder.GetFinalBarriers().GetTextureTransitions()
					: std::span<const FRDGTextureTransition>(Builder.GetPasses()[PassIndex].Barriers.GetTextureTransitions());
				std::vector<FRDGTextureTransition> ExpandedTextures;
				for (const auto& Barrier : Textures)
					for (uint32 Mip = 0; Mip < Barrier.Range.NumMips; ++Mip)
						for (uint32 Layer = 0; Layer < Barrier.Range.NumArrayLayers; ++Layer)
						{
							auto Cell = Barrier;
							Cell.Range = {Barrier.Range.Aspects, Barrier.Range.FirstMip + Mip, 1,
								Barrier.Range.FirstArrayLayer + Layer, 1};
							ExpandedTextures.push_back(Cell);
						}
				size_t BufferIndex = 0;
				size_t TextureIndex = 0;
				for (const auto& Event : Capture.Transitions)
				{
					if (Event.Kind != ERDGTransitionKind::RHIBarrier || Event.bFinal != bFinal
						|| (!bFinal && Event.PassIndex != PassIndex)) continue;
					ASSERT_LT(Event.ResourceId, Capture.Resources.size());
					if (Capture.Resources[Event.ResourceId].Kind == ERDGResourceKind::Texture)
					{
						ASSERT_LT(TextureIndex, ExpandedTextures.size());
						const auto& Barrier = ExpandedTextures[TextureIndex++];
						EXPECT_EQ(Barrier, (FRDGTextureTransition{Event.ResourceId, Event.TextureRange,
							Event.Before, Event.After, Event.bDiscardContents}));
					}
					else
					{
						ASSERT_LT(BufferIndex, Buffers.size());
						const auto& Barrier = Buffers[BufferIndex++];
						EXPECT_EQ(Barrier, (FRDGBufferTransition{Event.ResourceId, Event.BufferOffset,
							Event.BufferSize, Event.Before, Event.After, Event.bDiscardContents}));
					}
				}
				EXPECT_EQ(BufferIndex, Buffers.size());
				EXPECT_EQ(TextureIndex, ExpandedTextures.size());
			}
		}

		// Captures backend payloads after RDG scratch storage and graph storage expire.
		class FBarrierRecordingContext final : public IRHICommandContext
		{
		public:
			auto RHIBeginRenderPass(const FRHIRenderPassInfo&, FName) -> void override
			{ ADD_FAILURE() << "Unexpected render pass"; }
			auto RHIBindVertexBuffer(uint32, FRHIBuffer*, uint32) -> void override
			{ ADD_FAILURE() << "Unexpected vertex buffer binding"; }
			std::vector<std::vector<FRHIBufferTransition>> BufferBatches;
			std::vector<std::vector<FRHITextureTransition>> TextureBatches;
			auto RHIBeginFrame(const FRHIBeginFrameArgs& Args) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIBeginFrame"; }
			auto RHISubmitCommands() -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHISubmitCommands"; }
			auto RHIEndFrame() -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIEndFrame"; }
			auto RHIBeginDiagnosticRegion(std::string_view Name) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIBeginDiagnosticRegion"; }
			auto RHIEndDiagnosticRegion() -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIEndDiagnosticRegion"; }
			auto RHIEndRenderPass() -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIEndRenderPass"; }
			auto RHIBeginDrawingViewport(FRHIViewport* InViewport, FRHITexture* InRenderTargetRHI) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIBeginDrawingViewport"; }
			auto RHIEndDrawingViewport(FRHIViewport* InViewport, bool bInPresent, bool bInLockToVsync) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIEndDrawingViewport"; }
			auto RHISetViewport(float InMinX, float InMinY, float InMinZ, float InMaxX, float InMaxY, float InMaxZ) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHISetViewport"; }
			auto RHISetScissor(float InMinX, float InMinY, float InWidth, float InHeight) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHISetScissor"; }
			auto RHISetDepthBias(float InConstantFactor, float InClamp, float InSlopeFactor) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHISetDepthBias"; }
			auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& InGraphicsPipelineState) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHISetGraphicsPipelineState"; }
			auto RHIBindIndexBuffer(FRHIBuffer* IndexBuffer, uint32 Offset) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIBindIndexBuffer"; }
			auto RHITransitionBuffers(std::span<const FRHIBufferTransition> Transitions) -> void override
			{ BufferBatches.emplace_back(Transitions.begin(), Transitions.end()); }
			auto RHITransitionTextures(std::span<const FRHITextureTransition> Transitions) -> void override
			{ TextureBatches.emplace_back(Transitions.begin(), Transitions.end()); }
			auto RHICopyBuffer(FRHIBuffer* Source, FRHIBuffer* Destination, std::span<const FRHIBufferCopyRegion> Regions) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHICopyBuffer"; }
			auto RHICopyBufferToTexture(FRHIBuffer* Source, FRHITexture* Destination, std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHICopyBufferToTexture"; }
			auto RHICopyTextureToBuffer(FRHITexture* Source, FRHIBuffer* Destination, std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHICopyTextureToBuffer"; }
			auto RHICopyTexture(FRHITexture* Source, FRHITexture* Destination, std::span<const FRHITextureCopyRegion> Regions) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHICopyTexture"; }
			auto RHIWriteBuffer(FRHIBuffer* Buffer, uint32 Offset, FByteView Data) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIWriteBuffer"; }
			auto RHIInitializeTexture(FRHITexture* Texture) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIInitializeTexture"; }
			auto RHIUpdateTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, FByteView SourceData) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIUpdateTexture2D"; }
			auto RHIUpdateTexture3D(FRHITexture* Texture, uint32 MipIndex, const FUpdateTextureRegion3D& UpdateRegion, uint32 SourceRowPitch, uint32 SourceDepthPitch, FByteView SourceData) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIUpdateTexture3D"; }
			auto RHIReadTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, FByteBuffer& OutData) -> bool override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIReadTexture2D"; return {}; }
			auto RHIAllocateDynamicUniformBuffer(const void* Data, uint32 Size) -> FRHIUniformBufferRange override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIAllocateDynamicUniformBuffer"; return {}; }
			auto RHIAllocateDynamicStorageBuffer(const void* Data, uint32 Size) -> FRHIStorageBufferRange override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIAllocateDynamicStorageBuffer"; return {}; }
			auto RHIAcquireBackBuffer(FRHITexture* BackBuffer) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIAcquireBackBuffer"; }
			auto RHIBlockUntilGPUIdle() -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIBlockUntilGPUIdle"; }
			auto RHIPushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIPushConstants"; }
			auto RHISetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHISetShaderParameters"; }
			auto RHIDraw(const FRHIDrawArguments& Arguments) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIDraw"; }
			auto RHIDrawIndexed(const FRHIDrawIndexedArguments& Arguments) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIDrawIndexed"; }
		};

		class FRDGTests : public testing::Test
		{
		protected:
			auto GetCommandList() -> FRHICommandListImmediate&
			{
				return Executor.GetImmediateCommandList();
			}

		private:
			FRHICommandListExecutor Executor;
		};

		struct alignas(64) FTypedValuePayload final
		{
			explicit FTypedValuePayload(int* InDestructions = nullptr)
				: Destructions(InDestructions) {}
			int Value = 0;
			int* Destructions = nullptr;
			~FTypedValuePayload()
			{
				if (Destructions != nullptr) ++*Destructions;
			}
		};

		struct FTypedValueWriteParameters final
		{
			TRDGValueWrite<FTypedValuePayload> Output;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					MakeRDGValueParameterMemberMetadata<
						FTypedValueWriteParameters, decltype(Output),
						FTypedValuePayload>("Output", offsetof(
							FTypedValueWriteParameters, Output)),
				};
				static const auto Metadata =
					MakeInlineRDGParametersMetadata<
						FTypedValueWriteParameters>(
							"FTypedValueWriteParameters", Members);
				return &Metadata;
			}
		};

		struct FTypedValueReadParameters final
		{
			TRDGValueRead<FTypedValuePayload> Input;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					MakeRDGValueParameterMemberMetadata<
						FTypedValueReadParameters, decltype(Input),
						FTypedValuePayload>("Input", offsetof(
							FTypedValueReadParameters, Input)),
				};
				static const auto Metadata =
					MakeInlineRDGParametersMetadata<
						FTypedValueReadParameters>(
							"FTypedValueReadParameters", Members);
				return &Metadata;
			}
		};

		auto WholeColor(uint32 Mips = 1) -> FRHITextureSubresourceRange
		{
			return {ERHITextureAspect::Color, 0, Mips, 0, 1};
		}

		auto MakeGraphTexture(const char* Name, uint8 Mips = 1) -> FTextureRHIRef
		{
			return MakeRefCount<FRHITexture>(FRHITextureCreateDesc::Create2D(
				Name, 64, 64, EPixelFormat::RGBA8_UNORM)
				.SetNumMips(Mips)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::Storage
					| ETextureCreateFlags::SourceCopy
					| ETextureCreateFlags::DestinationCopy));
		}

		auto DescribeGraphTexture(const FRHITexture& Texture)
			-> FRDGTextureDesc
		{
			FRHITextureDesc Desc(Texture.GetDimension());
			Desc.Extent = {static_cast<int32>(Texture.GetSizeX()),
				static_cast<int32>(Texture.GetSizeY())};
			Desc.Depth = static_cast<uint16>(Texture.GetSizeZ());
			Desc.Format = Texture.GetFormat();
			Desc.ArraySize = Texture.GetArraySize();
			Desc.NumMips = Texture.GetNumMips();
			Desc.NumSamples = Texture.GetNumSamples();
			Desc.Flags = Texture.GetFlags();
			return {.Texture = Desc};
		}

		auto CreateTestTexture(FRDGBuilder& Builder, std::string_view Name,
			const FTextureRHIRef& Texture,
			ERHIAccess FinalAccess = ERHIAccess::None) -> FRDGTextureHandle
		{
			return Builder.CreateTexture(DescribeGraphTexture(*Texture), Name, FinalAccess);
		}

		auto CreateTestBuffer(FRDGBuilder& Builder, std::string_view Name,
			const FBufferRHIRef& Buffer,
			ERHIAccess FinalAccess = ERHIAccess::None) -> FRDGBufferHandle
		{
			return Builder.CreateBuffer({.Buffer = Buffer->GetDesc()}, Name, FinalAccess);
		}

		class FTestRDGAllocator final : public FRDGAllocator
		{
		public:
			bool bFail = false;
			std::string FailureMessage = "injected allocation failure";
			bool bOmitResources = false;
			uint32 AllocationCount = 0;
			std::function<void()> OnAllocate;
			std::vector<FRDGAllocationRequest> LastRequests;
			std::vector<FTextureRHIRef> CreatedTextures;
			FTextureRHIRef TextureOverride;
			FBufferRHIRef BufferOverride;
			std::unordered_map<uint32, FTextureRHIRef> TextureOverrides;
			std::unordered_map<uint32, FBufferRHIRef> BufferOverrides;

			auto Allocate(std::span<const FRDGAllocationRequest> Requests,
				FRDGAllocatedResources& OutResources, std::string& OutError)
				-> bool override
			{
				if (OnAllocate) OnAllocate();
				LastRequests.assign(Requests.begin(), Requests.end());
				if (bFail)
				{
					OutError = FailureMessage;
					return false;
				}
				if (bOmitResources)
				{
					OutError.clear();
					return true;
				}
				for (const FRDGAllocationRequest& Request : Requests)
				{
					++AllocationCount;
					if (Request.Kind == ERDGResourceKind::Texture)
					{
						FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create(
							"TestRDG", Request.TextureDesc.Dimension);
						static_cast<FRHITextureDesc&>(Desc) = Request.TextureDesc;
						const auto Override = TextureOverrides.find(Request.ResourceId);
						auto Texture = Override != TextureOverrides.end()
							? Override->second : (TextureOverride
								? TextureOverride : MakeRefCount<FRHITexture>(Desc));
						CreatedTextures.push_back(Texture);
						if (!OutResources.SetTexture(Request.ResourceId,
							std::move(Texture), AllocationCount)) return false;
					}
					else
					{
						const auto Override = BufferOverrides.find(Request.ResourceId);
						auto Buffer = Override != BufferOverrides.end()
							? Override->second : (BufferOverride
								? BufferOverride : MakeRefCount<FRHIBuffer>(
									FRHIBufferCreateDesc::Create("TestRDG",
										Request.BufferDesc)));
						if (!OutResources.SetBuffer(Request.ResourceId,
							std::move(Buffer), AllocationCount)) return false;
					}
				}
				OutError.clear();
				return true;
			}
		};

		struct FNestedGraphParameters final
		{
			FRDGTokenParameter Completion;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*;
		};

		auto FNestedGraphParameters::GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*
		{
			static const std::array Members{
				MakeRDGResourceParameterMemberMetadata<
					FNestedGraphParameters, decltype(Completion),
					FRDGTokenParameter>("Completion",
						offsetof(FNestedGraphParameters, Completion),
						ERDGParameterMemberKind::Token,
						ERDGResourceKind::Token,
						ERDGParameterRangeKind::None,
						ERDGUse::Write, ERHIAccess::None, true),
			};
			static const auto Metadata =
				MakeInlineRDGParametersMetadata<FNestedGraphParameters>(
					"FNestedGraphParameters", Members);
			return &Metadata;
		}

		struct alignas(64) FGraphParameterLayoutFixture final
		{
			FRDGTextureParameter Input;
			std::array<std::optional<FRDGBufferParameter>, 2> Buffers;
			std::optional<FRDGColorAttachmentParameter> Color;
			FRDGDepthStencilAttachmentParameter Depth;
			FRDGManagedTextureParameter Managed;
			FNestedGraphParameters Nested;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*;
		};

		auto FGraphParameterLayoutFixture::GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*
		{
			static const std::array Members{
				MakeRDGResourceParameterMemberMetadata<
					FGraphParameterLayoutFixture, decltype(Input),
					FRDGTextureParameter>("Input",
						offsetof(FGraphParameterLayoutFixture, Input),
						ERDGParameterMemberKind::Texture,
						ERDGResourceKind::Texture,
						ERDGParameterRangeKind::TextureSubresource,
						ERDGUse::Read,
						ERHIAccess::GraphicsShaderRead),
				MakeRDGResourceParameterMemberMetadata<
					FGraphParameterLayoutFixture, decltype(Buffers),
					FRDGBufferParameter>("Buffers",
						offsetof(FGraphParameterLayoutFixture, Buffers),
						ERDGParameterMemberKind::Buffer,
						ERDGResourceKind::Buffer,
						ERDGParameterRangeKind::BufferBytes,
						ERDGUse::ReadWrite,
						ERHIAccess::ComputeShaderReadWrite),
				MakeRDGResourceParameterMemberMetadata<
					FGraphParameterLayoutFixture, decltype(Color),
					FRDGColorAttachmentParameter>("Color",
						offsetof(FGraphParameterLayoutFixture, Color),
						ERDGParameterMemberKind::ManagedColorAttachment,
						ERDGResourceKind::Texture,
						ERDGParameterRangeKind::TextureSubresource,
						ERDGUse::ReadWrite,
						ERHIAccess::ColorAttachmentReadWrite, true,
						ERHIRenderTargetLoadAction::Clear,
						ERHIRenderTargetStoreAction::Store, true,
						ERHIAccess::GraphicsShaderRead),
				MakeRDGResourceParameterMemberMetadata<
					FGraphParameterLayoutFixture, decltype(Depth),
					FRDGDepthStencilAttachmentParameter>("Depth",
						offsetof(FGraphParameterLayoutFixture, Depth),
						ERDGParameterMemberKind::DepthStencilAttachment,
						ERDGResourceKind::Texture,
						ERDGParameterRangeKind::TextureSubresource,
						ERDGUse::ReadWrite,
						ERHIAccess::DepthStencilReadWrite, false,
						ERHIRenderTargetLoadAction::Load,
						ERHIRenderTargetStoreAction::Store),
				MakeRDGResourceParameterMemberMetadata<
					FGraphParameterLayoutFixture, decltype(Managed),
					FRDGManagedTextureParameter>("Managed",
						offsetof(FGraphParameterLayoutFixture, Managed),
						ERDGParameterMemberKind::ManagedTexture,
						ERDGResourceKind::Texture,
						ERDGParameterRangeKind::TextureSubresource,
						ERDGUse::Write,
						ERHIAccess::GraphicsShaderReadWrite, true,
						ERHIRenderTargetLoadAction::Load,
						ERHIRenderTargetStoreAction::Store, true,
						ERHIAccess::GraphicsShaderRead),
				MakeRDGNestedParameterMemberMetadata<
					FGraphParameterLayoutFixture, decltype(Nested)>("Nested",
						offsetof(FGraphParameterLayoutFixture, Nested),
						FNestedGraphParameters::GetRDGParametersMetadata()),
			};
			static const auto Metadata =
				MakeInlineRDGParametersMetadata<FGraphParameterLayoutFixture>(
					"FGraphParameterLayoutFixture", Members);
			return &Metadata;
		}

		std::vector<int>* GParameterDestructionOrder = nullptr;

		struct FFirstLifetimeGraphParameters final
		{
			~FFirstLifetimeGraphParameters()
			{
				if (GParameterDestructionOrder) GParameterDestructionOrder->push_back(1);
			}

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array<FRDGParameterMemberMetadata, 0> Members{};
				static const auto Metadata =
					MakeInlineRDGParametersMetadata<FFirstLifetimeGraphParameters>(
						"FFirstLifetimeGraphParameters", Members);
				return &Metadata;
			}
		};

		struct FSecondLifetimeGraphParameters final
		{
			~FSecondLifetimeGraphParameters()
			{
				if (GParameterDestructionOrder) GParameterDestructionOrder->push_back(2);
			}

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array<FRDGParameterMemberMetadata, 0> Members{};
				static const auto Metadata =
					MakeInlineRDGParametersMetadata<FSecondLifetimeGraphParameters>(
						"FSecondLifetimeGraphParameters", Members);
				return &Metadata;
			}
		};

		struct FMalformedGraphParameters final
		{
			FRDGTextureParameter Texture;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					FRDGParameterMemberMetadata{
						.Name = "Texture",
						.Offset = static_cast<uint32>(sizeof(FMalformedGraphParameters)),
						.ElementSize = static_cast<uint32>(sizeof(Texture)),
						.Kind = ERDGParameterMemberKind::Texture,
						.ResourceKind = ERDGResourceKind::Texture,
						.RangeKind = ERDGParameterRangeKind::TextureSubresource,
						.Access = ERHIAccess::GraphicsShaderRead,
					},
				};
				static const auto Metadata =
					MakeInlineRDGParametersMetadata<FMalformedGraphParameters>(
						"FMalformedGraphParameters", Members);
				return &Metadata;
			}
		};

		struct FAllGraphUseParameters final
		{
			std::array<std::optional<FRDGTextureParameter>, 2> Inputs;
			FRDGBufferParameter Buffer;
			FRDGColorAttachmentParameter Color;
			FRDGDepthStencilAttachmentParameter Depth;
			FRDGColorAttachmentParameter ManagedColor;
			std::optional<FRDGDepthStencilAttachmentParameter> ManagedDepth;
			FRDGManagedTextureParameter ManagedTexture;
			FNestedGraphParameters Nested;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*;
		};

		auto FAllGraphUseParameters::GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*
		{
			static const std::array Members{
				MakeRDGResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(Inputs),
					FRDGTextureParameter>("Inputs",
						offsetof(FAllGraphUseParameters, Inputs),
						ERDGParameterMemberKind::Texture,
						ERDGResourceKind::Texture,
						ERDGParameterRangeKind::TextureSubresource,
						ERDGUse::Read, ERHIAccess::GraphicsShaderRead),
				MakeRDGResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(Buffer),
					FRDGBufferParameter>("Buffer",
						offsetof(FAllGraphUseParameters, Buffer),
						ERDGParameterMemberKind::Buffer,
						ERDGResourceKind::Buffer,
						ERDGParameterRangeKind::BufferBytes,
						ERDGUse::ReadWrite,
						ERHIAccess::GraphicsShaderReadWrite),
				MakeRDGResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(Color),
					FRDGColorAttachmentParameter>("Color",
						offsetof(FAllGraphUseParameters, Color),
						ERDGParameterMemberKind::ColorAttachment,
						ERDGResourceKind::Texture,
						ERDGParameterRangeKind::TextureSubresource,
						ERDGUse::ReadWrite,
						ERHIAccess::ColorAttachmentReadWrite, false,
						ERHIRenderTargetLoadAction::Clear,
						ERHIRenderTargetStoreAction::Store),
				MakeRDGResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(Depth),
					FRDGDepthStencilAttachmentParameter>("Depth",
						offsetof(FAllGraphUseParameters, Depth),
						ERDGParameterMemberKind::DepthStencilAttachment,
						ERDGResourceKind::Texture,
						ERDGParameterRangeKind::TextureSubresource,
						ERDGUse::ReadWrite,
						ERHIAccess::DepthStencilReadWrite, false,
						ERHIRenderTargetLoadAction::Clear,
						ERHIRenderTargetStoreAction::Store),
				MakeRDGResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(ManagedColor),
					FRDGColorAttachmentParameter>("ManagedColor",
						offsetof(FAllGraphUseParameters, ManagedColor),
						ERDGParameterMemberKind::ManagedColorAttachment,
						ERDGResourceKind::Texture,
						ERDGParameterRangeKind::TextureSubresource,
						ERDGUse::ReadWrite,
						ERHIAccess::ColorAttachmentReadWrite, false,
						ERHIRenderTargetLoadAction::Clear,
						ERHIRenderTargetStoreAction::Store, true,
						ERHIAccess::GraphicsShaderRead),
				MakeRDGResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(ManagedDepth),
					FRDGDepthStencilAttachmentParameter>("ManagedDepth",
						offsetof(FAllGraphUseParameters, ManagedDepth),
						ERDGParameterMemberKind::ManagedDepthStencilAttachment,
						ERDGResourceKind::Texture,
						ERDGParameterRangeKind::TextureSubresource,
						ERDGUse::ReadWrite,
						ERHIAccess::DepthStencilReadWrite, false,
						ERHIRenderTargetLoadAction::Clear,
						ERHIRenderTargetStoreAction::Store, true,
						ERHIAccess::GraphicsShaderRead),
				MakeRDGResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(ManagedTexture),
					FRDGManagedTextureParameter>("ManagedTexture",
						offsetof(FAllGraphUseParameters, ManagedTexture),
						ERDGParameterMemberKind::ManagedTexture,
						ERDGResourceKind::Texture,
						ERDGParameterRangeKind::TextureSubresource,
						ERDGUse::Write,
						ERHIAccess::GraphicsShaderReadWrite, true,
						ERHIRenderTargetLoadAction::Load,
						ERHIRenderTargetStoreAction::Store, true,
						ERHIAccess::GraphicsShaderRead),
				MakeRDGNestedParameterMemberMetadata<
					FAllGraphUseParameters, decltype(Nested)>("Nested",
						offsetof(FAllGraphUseParameters, Nested),
						FNestedGraphParameters::GetRDGParametersMetadata()),
			};
			static const auto Metadata =
				MakeInlineRDGParametersMetadata<FAllGraphUseParameters>(
					"FAllGraphUseParameters", Members);
			return &Metadata;
		}

		struct FTwoTextureGraphParameters final
		{
			std::array<FRDGTextureParameter, 2> Textures;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					MakeRDGResourceParameterMemberMetadata<
						FTwoTextureGraphParameters, decltype(Textures),
						FRDGTextureParameter>("Textures",
							offsetof(FTwoTextureGraphParameters, Textures),
							ERDGParameterMemberKind::Texture,
							ERDGResourceKind::Texture,
							ERDGParameterRangeKind::TextureSubresource,
							ERDGUse::Read,
							ERHIAccess::GraphicsShaderRead),
				};
				static const auto Metadata =
					MakeInlineRDGParametersMetadata<FTwoTextureGraphParameters>(
						"FTwoTextureGraphParameters", Members);
				return &Metadata;
			}
		};

		struct FComposedTextureArrayParameters final
		{
			std::array<std::optional<FRDGTextureParameter>, 2> Textures;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					WithRDGShaderBinding(MakeRDGTextureReadMetadata<FComposedTextureArrayParameters, decltype(Textures)>(
						"Textures", offsetof(FComposedTextureArrayParameters, Textures)),
						ERHIBindingType::Texture),
				};
				static const auto Metadata =
					MakeInlineRDGParametersMetadata<
						FComposedTextureArrayParameters>(
							"FComposedTextureArrayParameters", Members);
				return &Metadata;
			}
		};

		struct FMalformedComposedAccessParameters final
		{
			FRDGTextureParameter Texture;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					WithRDGShaderBinding(MakeRDGTextureReadMetadata<FMalformedComposedAccessParameters, decltype(Texture)>(
						"Texture", offsetof(FMalformedComposedAccessParameters, Texture)),
						ERHIBindingType::StorageImage),
				};
				static const auto Metadata =
					MakeInlineRDGParametersMetadata<
						FMalformedComposedAccessParameters>(
							"FMalformedComposedAccessParameters", Members);
				return &Metadata;
			}
		};

		struct FComposedComputeBufferParameters final
		{
			FRDGBufferParameter InputBuffer;
			FRDGBufferParameter OutputBuffer;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					WithRDGShaderBinding(MakeRDGResourceParameterMemberMetadata<
						FComposedComputeBufferParameters, decltype(InputBuffer), FRDGBufferParameter>(
						"InputBuffer", offsetof(FComposedComputeBufferParameters, InputBuffer),
						ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer,
						ERDGParameterRangeKind::BufferBytes, ERDGUse::Read,
						ERHIAccess::ComputeShaderRead),
						ERHIBindingType::StorageBuffer),
					WithRDGShaderBinding(MakeRDGResourceParameterMemberMetadata<
						FComposedComputeBufferParameters, decltype(OutputBuffer), FRDGBufferParameter>(
						"OutputBuffer", offsetof(FComposedComputeBufferParameters, OutputBuffer),
						ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer,
						ERDGParameterRangeKind::BufferBytes, ERDGUse::Write,
						ERHIAccess::ComputeShaderReadWrite, true),
						ERHIBindingType::StorageBuffer),
				};
				static const auto Metadata =
					MakeInlineRDGParametersMetadata<
						FComposedComputeBufferParameters>(
							"FComposedComputeBufferParameters", Members);
				return &Metadata;
			}
		};

		struct FLargeTokenGraphParameters final
		{
			std::array<FRDGTokenParameter, 128> Tokens;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					MakeRDGResourceParameterMemberMetadata<
						FLargeTokenGraphParameters, decltype(Tokens),
						FRDGTokenParameter>("Tokens",
							offsetof(FLargeTokenGraphParameters, Tokens),
							ERDGParameterMemberKind::Token,
							ERDGResourceKind::Token,
							ERDGParameterRangeKind::None,
							ERDGUse::Write, ERHIAccess::None, true),
				};
				static const auto Metadata =
					MakeInlineRDGParametersMetadata<FLargeTokenGraphParameters>(
						"FLargeTokenGraphParameters", Members);
				return &Metadata;
			}
		};

		struct FComputeResolutionParameters final
		{
			FRDGTextureParameter Texture;
			FRDGBufferParameter Buffer;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					MakeRDGResourceParameterMemberMetadata<
						FComputeResolutionParameters, decltype(Texture),
						FRDGTextureParameter>("Texture",
							offsetof(FComputeResolutionParameters, Texture),
							ERDGParameterMemberKind::Texture,
							ERDGResourceKind::Texture,
							ERDGParameterRangeKind::TextureSubresource,
							ERDGUse::Read, ERHIAccess::ComputeShaderRead),
					MakeRDGResourceParameterMemberMetadata<
						FComputeResolutionParameters, decltype(Buffer),
						FRDGBufferParameter>("Buffer",
							offsetof(FComputeResolutionParameters, Buffer),
							ERDGParameterMemberKind::Buffer,
							ERDGResourceKind::Buffer,
							ERDGParameterRangeKind::BufferBytes,
							ERDGUse::Read, ERHIAccess::ComputeShaderRead),
				};
				static const auto Metadata = MakeInlineRDGParametersMetadata<
					FComputeResolutionParameters>("FComputeResolutionParameters", Members);
				return &Metadata;
			}
		};

		struct FUnavailableBufferParameters final
		{
			FRDGBufferParameter Buffer;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					MakeRDGResourceParameterMemberMetadata<
						FUnavailableBufferParameters, decltype(Buffer),
						FRDGBufferParameter>("Buffer",
							offsetof(FUnavailableBufferParameters, Buffer),
							ERDGParameterMemberKind::Buffer,
							ERDGResourceKind::Buffer,
							ERDGParameterRangeKind::BufferBytes,
							ERDGUse::Write,
							ERHIAccess::ComputeShaderReadWrite, true),
				};
				static const auto Metadata = MakeInlineRDGParametersMetadata<
					FUnavailableBufferParameters>("FUnavailableBufferParameters", Members);
				return &Metadata;
			}
		};

		struct FCopyResolutionParameters final
		{
			FRDGTextureParameter Texture;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					MakeRDGResourceParameterMemberMetadata<
						FCopyResolutionParameters, decltype(Texture),
						FRDGTextureParameter>("Texture",
							offsetof(FCopyResolutionParameters, Texture),
							ERDGParameterMemberKind::Texture,
							ERDGResourceKind::Texture,
							ERDGParameterRangeKind::TextureSubresource,
							ERDGUse::Read, ERHIAccess::TransferRead),
				};
				static const auto Metadata = MakeInlineRDGParametersMetadata<
					FCopyResolutionParameters>("FCopyResolutionParameters", Members);
				return &Metadata;
			}
		};

		template<typename Argument>
		concept CTextureResolverArgument = requires(
			const FRDGParameterResolver& Resolver, const Argument& Value)
		{
			Resolver.GetTexture(Value);
		};

		auto StripParameterFields(std::string Dump) -> std::string
		{
			size_t ParameterLine = Dump.find("parameter ");
			while (ParameterLine != std::string::npos)
			{
				const size_t End = Dump.find('\n', ParameterLine);
				Dump.erase(ParameterLine, End == std::string::npos
					? std::string::npos : End - ParameterLine + 1);
				ParameterLine = Dump.find("parameter ", ParameterLine);
			}
			size_t ParameterStruct = Dump.find(" parameters=");
			while (ParameterStruct != std::string::npos)
			{
				const size_t End = Dump.find('\n', ParameterStruct);
				Dump.erase(ParameterStruct, End == std::string::npos
					? std::string::npos : End - ParameterStruct);
				ParameterStruct = Dump.find(" parameters=", ParameterStruct);
			}
			size_t Field = Dump.find(" field=");
			while (Field != std::string::npos)
			{
				const size_t End = Dump.find('\n', Field);
				Dump.erase(Field, End == std::string::npos
					? std::string::npos : End - Field);
				Field = Dump.find(" field=", Field);
			}
			return Dump;
		}
	} // namespace

	TEST_F(FRDGTests, ExecuteConsumesEmptyManualParameterizedAndTypedGraphs)
	{
		for (int Shape = 0; Shape < 4; ++Shape)
		{
			int Calls = 0;
			FRDGBuilder Builder;
			EXPECT_EQ(Builder.GetState(), ERDGBuilderState::Building);
			EXPECT_FALSE(Builder.Capture().bCompiled);
			EXPECT_TRUE(Builder.GetPasses().empty());
			if (Shape == 1)
				FRDGBuilderTestAccessor::AddPass(Builder, "Manual", ERDGPassType::Copy,
					[&](FRHICommandListImmediate&, const FRDGPassResources&) { ++Calls; });
			if (Shape == 2)
			{
				auto Parameters = Builder.AllocParameters<FNestedGraphParameters>();
				Parameters->Completion = {Builder.CreateToken("Done")};
				const auto* Submitted = &Parameters.Get();
				Builder.AddPass("Parameterized", ERDGPassType::Copy, std::move(Parameters),
					[&, Submitted](FRHICommandListImmediate&, const FNestedGraphParameters& Values,
						const FRDGParameterResolver&) { EXPECT_EQ(&Values, Submitted); ++Calls; });
			}
			if (Shape == 3)
			{
				const auto Value = Builder.CreateValue<int>("Value", "int", 0);
				const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Typed", ERDGPassType::Copy,
					[&, Value](FRHICommandListImmediate&, const FRDGPassResources& Resources) {
						Resources.WriteValue(Value) = 42;
						++Calls;
					});
				FRDGBuilderTestAccessor::UseValue(Builder, Pass, Value, ERDGUse::Write);
			}
			const auto Result = Builder.Execute(GetCommandList());
			ASSERT_EQ(Result.Status, ERDGExecutionStatus::Recorded) << Result.Result.Message;
			EXPECT_EQ(Builder.GetState(), ERDGBuilderState::Recorded);
			const auto Capture = Builder.Capture();
			EXPECT_EQ(Builder.Execute(GetCommandList()).Status, ERDGExecutionStatus::InvalidState);
			EXPECT_EQ(Calls, Shape == 0 ? 0 : 1);
			EXPECT_EQ(Builder.GetExecutionResult().Status, Result.Status);
			EXPECT_EQ(Builder.GetExecutionResult().Result.Message, Result.Result.Message);
			EXPECT_EQ(Builder.Capture().Dump, Capture.Dump);
			EXPECT_EQ(Builder.GetStatistics().ExecuteMicroseconds, Capture.Statistics.ExecuteMicroseconds);
		}
	}

	TEST_F(FRDGTests, PhaseTimingsSeparatePreparationRecordingAndFailedCompilation)
	{
		for (bool FailAllocation : {false, true})
		{
			FRDGBuilder Builder;
			const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferDesc(
				64, 4, EBufferUsageFlags::UnorderedAccess)}, "TimedBuffer");
			int Calls = 0;
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "TimedPass", ERDGPassType::Compute,
				[&](FRHICommandListImmediate&, const FRDGPassResources&) {
					++Calls;
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				});
			FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, 64,
				ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			FTestRDGAllocator Allocator;
			Allocator.bFail = FailAllocation;
			Allocator.OnAllocate = [] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); };
			FRDGExecutionContext Context{Allocator};
			EXPECT_EQ(Builder.Execute(GetCommandList(), &Context).IsSuccess(), !FailAllocation);
			const auto Stats = Builder.GetStatistics();
			EXPECT_GE(Stats.Phases.PreparationMicroseconds, 1000u);
			if (FailAllocation) EXPECT_EQ(Stats.Phases.RecordingMicroseconds, 0u);
			else EXPECT_GE(Stats.Phases.RecordingMicroseconds, 1000u);
			EXPECT_EQ(Calls, FailAllocation ? 0 : 1);
			EXPECT_GE(Stats.ExecuteMicroseconds,
				Stats.Phases.PreparationMicroseconds + Stats.Phases.RecordingMicroseconds);
			EXPECT_GE(Stats.CompileMicroseconds, Stats.Phases.ValidationMicroseconds
				+ Stats.Phases.RangeMicroseconds + Stats.Phases.DependencyMicroseconds
				+ Stats.Phases.CullingMicroseconds + Stats.Phases.PlanMicroseconds);
			EXPECT_EQ(Builder.Capture().Statistics.Phases.PreparationMicroseconds,
				Stats.Phases.PreparationMicroseconds);
			EXPECT_EQ(Builder.Execute(GetCommandList(), &Context).Status, ERDGExecutionStatus::InvalidState);
			EXPECT_EQ(Builder.GetStatistics().Phases.RecordingMicroseconds, Stats.Phases.RecordingMicroseconds);
		}
		FRDGBuilder Invalid;
		Invalid.CreateToken("");
		EXPECT_EQ(Invalid.Execute(GetCommandList()).Status, ERDGExecutionStatus::CompileFailed);
		const auto Stats = Invalid.GetStatistics();
		EXPECT_GE(Stats.CompileMicroseconds, Stats.Phases.ValidationMicroseconds);
		EXPECT_EQ(Stats.Phases.RangeMicroseconds, 0u);
		EXPECT_EQ(Stats.Phases.DependencyMicroseconds, 0u);
		EXPECT_EQ(Stats.Phases.CullingMicroseconds, 0u);
		EXPECT_EQ(Stats.Phases.PlanMicroseconds, 0u);
		EXPECT_EQ(Stats.ExecuteMicroseconds, 0u);
		EXPECT_FALSE(Invalid.Capture().bCompiled);
	}

	TEST_F(FRDGTests, StorageConstructorReentryConsumesWithoutRecordingIncompleteValues)
	{
		struct FReentrantValue
		{
			FReentrantValue(FRDGBuilder& Builder, FRHICommandListImmediate& Commands,
				int& InDestructions) : Destructions(InDestructions)
			{
				const auto Result = Builder.Execute(Commands);
				EXPECT_EQ(Result.Status, ERDGExecutionStatus::CompileFailed);
				EXPECT_EQ(Result.Result.Message, "render graph storage construction is incomplete");
				EXPECT_EQ(Result.Result.Error, ERDGError::InvalidState);
			}
			~FReentrantValue() { ++Destructions; }
			int& Destructions;
		};
		int Destructions = 0;
		int Calls = 0;
		{
			FRDGBuilder Builder;
			FRDGBuilderTestAccessor::AddPass(Builder, "MustNotRun", ERDGPassType::Copy,
				[&](FRHICommandListImmediate&, const FRDGPassResources&) { ++Calls; });
			Builder.CreateValue<FReentrantValue>("Reentrant", "constructor", Builder,
				GetCommandList(), Destructions);
			EXPECT_EQ(Builder.GetState(), ERDGBuilderState::Failed);
			EXPECT_FALSE(Builder.Capture().bCompiled);
			EXPECT_EQ(Calls, 0);
			EXPECT_EQ(Destructions, 0);
			EXPECT_EQ(Builder.Execute(GetCommandList()).Status, ERDGExecutionStatus::InvalidState);
		}
		EXPECT_EQ(Destructions, 1);
	}

	TEST_F(FRDGTests, FailedCompilationConsumesBuilderAndPublishesNoPartialPlan)
	{
		FRDGBuilder Builder;
		Builder.CreateValue<int>("MissingWriter", "int", 7);
		FTestRDGAllocator Allocator;
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_EQ(Result.Status, ERDGExecutionStatus::CompileFailed);
		EXPECT_EQ(Builder.GetState(), ERDGBuilderState::Failed);
		EXPECT_FALSE(Builder.Capture().bCompiled);
		EXPECT_TRUE(Builder.Capture().Passes.empty());
		EXPECT_TRUE(Builder.GetDependencies().empty());
		EXPECT_TRUE(Allocator.LastRequests.empty());
		EXPECT_EQ(Builder.Execute(GetCommandList(), &Context).Status, ERDGExecutionStatus::InvalidState);
		EXPECT_EQ(Builder.GetExecutionResult().Result.Message, Result.Result.Message);
		EXPECT_DEATH(Builder.CreateToken("Late"), "require Building state");
	}

	TEST_F(FRDGTests, CompileOnlyEvidenceSealsWithoutExecutableOwnership)
	{
		FRDGBuilder Builder;
		FRDGBuilderTestAccessor::AddPass(Builder, "Diagnostic", ERDGPassType::Copy);
		const auto Evidence = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Evidence.IsSuccess()) << Evidence.Result.Message;
		EXPECT_TRUE(Builder.Capture().bCompiled);
		EXPECT_EQ(Builder.Execute(GetCommandList()).Status, ERDGExecutionStatus::InvalidState);
		EXPECT_FALSE(FRDGBuilderTestAccessor::Compile(Builder).IsSuccess());
	}

	TEST_F(FRDGTests, FinalizedDeclarationsRejectLateMutation)
	{
		FRDGBuilder Builder;
		const auto Token = Builder.CreateToken("Token");
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Copy);
		FRDGBuilderTestAccessor::UseToken(Builder, Pass, Token, ERDGUse::Write);
		ASSERT_TRUE(Builder.Execute(GetCommandList()).IsSuccess());
		EXPECT_DEATH(FRDGBuilderTestAccessor::AddPass(Builder, "Late", ERDGPassType::Copy), "require Building state");
		EXPECT_DEATH(Builder.CreateValue<int>("Late", "int"), "require Building state");
		EXPECT_DEATH(Builder.AllocParameters<FNestedGraphParameters>(), "require Building state");
		EXPECT_DEATH(Builder.SetBudget({}), "require Building state");
		EXPECT_DEATH(Builder.EnablePassCulling(), "require Building state");
		EXPECT_DEATH(Builder.AddPassDependency(Pass, Pass), "require Building state");
		EXPECT_DEATH(Builder.MarkPassRoot(Pass), "require Building state");
		EXPECT_DEATH(FRDGBuilderTestAccessor::UseToken(Builder, Pass, Token, ERDGUse::Read), "require Building state");
		EXPECT_DEATH(Builder.QueueBufferExtraction({}, nullptr, ERHIAccess::None), "require Building state");
	}

	TEST_F(FRDGTests, AllocatorAndCallbackReentryCannotExecuteOrAlterEvidence)
	{
		FRDGBuilder Builder;
		FTestRDGAllocator Allocator;
		FRDGExecutionContext Context{Allocator};
		FBufferRHIRef Extraction;
		int Calls = 0;
		int AllocatorCalls = 0;
		const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferDesc(
			64, 4, EBufferUsageFlags::UnorderedAccess)}, "Output");
		Builder.QueueBufferExtraction(Buffer, &Extraction, ERHIAccess::ComputeShaderReadWrite);
		Allocator.OnAllocate = [&] {
			++AllocatorCalls;
			EXPECT_EQ(Builder.GetState(), ERDGBuilderState::Preparing);
			const auto Before = Builder.Capture();
			const auto CommandCount = GetCommandList().GetNumRecordedCommands();
			EXPECT_EQ(Builder.Execute(GetCommandList(), &Context).Status, ERDGExecutionStatus::InvalidState);
			EXPECT_EQ(Builder.Capture().Dump, Before.Dump);
			EXPECT_EQ(GetCommandList().GetNumRecordedCommands(), CommandCount);
			EXPECT_FALSE(Extraction);
			EXPECT_EQ(Calls, 0);
		};
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute,
			[&](FRHICommandListImmediate&, const FRDGPassResources&) {
				++Calls;
				EXPECT_EQ(Builder.GetState(), ERDGBuilderState::Recording);
				EXPECT_EQ(Builder.Execute(GetCommandList(), &Context).Status, ERDGExecutionStatus::InvalidState);
				EXPECT_FALSE(Extraction);
			});
		FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, 64, ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		ASSERT_TRUE(Builder.Execute(GetCommandList(), &Context).IsSuccess());
		EXPECT_EQ(AllocatorCalls, 1);
		EXPECT_EQ(Calls, 1);
		EXPECT_TRUE(Extraction);
		EXPECT_EQ(Builder.Execute(GetCommandList(), &Context).Status, ERDGExecutionStatus::InvalidState);
		EXPECT_EQ(AllocatorCalls, 1);
		EXPECT_EQ(Calls, 1);
	}

	TEST_F(FRDGTests, ReentrantDeclarationFailsBeforeChangingGraph)
	{
		for (bool InAllocator : {false, true})
		{
			FRDGBuilder Builder;
			FTestRDGAllocator Allocator;
			FRDGExecutionContext Context{Allocator};
			const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferDesc(
				64, 4, EBufferUsageFlags::UnorderedAccess)}, "Buffer");
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute,
				[&](FRHICommandListImmediate&, const FRDGPassResources&) { Builder.EnablePassCulling(); });
			FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, 64, ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			if (InAllocator) Allocator.OnAllocate = [&] { Builder.CreateToken("Late"); };
			EXPECT_DEATH(Builder.Execute(GetCommandList(), &Context), "require Building state");
		}
	}

	TEST_F(FRDGTests, FailedPreparationRetainsCaptureAndNeverPublishesExtraction)
	{
		for (int Failure = 0; Failure < 4; ++Failure)
		{
			FRDGCapture Capture;
			int Destructions = 0;
			{
				FRDGBuilder Builder;
				FTestRDGAllocator Allocator;
				Allocator.bFail = Failure == 0 || Failure == 3;
				if (Failure == 3) Allocator.FailureMessage.clear();
				Allocator.bOmitResources = Failure == 1;
				if (Failure == 2)
					Allocator.BufferOverride = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
						"Incompatible", 32, 4, EBufferUsageFlags::UnorderedAccess));
				FRDGExecutionContext Context{Allocator};
				const auto Value = Builder.CreateValue<FTypedValuePayload>("Tracked", "tracked", &Destructions);
				FBufferRHIRef Destination = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
					"Original", 64, 4, EBufferUsageFlags::UnorderedAccess));
				const auto* Original = Destination.GetReference();
				const auto Buffer = Builder.CreateBuffer({.Buffer = Destination->GetDesc()}, "Output");
				Builder.QueueBufferExtraction(Buffer, &Destination, ERHIAccess::ComputeShaderReadWrite);
				int Calls = 0;
				const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute,
					[&](FRHICommandListImmediate&, const FRDGPassResources&) { ++Calls; });
				FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, 64, ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
				FRDGBuilderTestAccessor::UseValue(Builder, Pass, Value, ERDGUse::Write);
				const auto CommandsBefore = GetCommandList().GetNumRecordedCommands();
				const auto Result = Builder.Execute(GetCommandList(), &Context);
				ASSERT_EQ(Result.Status, ERDGExecutionStatus::PreparationFailed);
				const auto ExpectedError = Failure == 1 ? ERDGError::MissingAllocation
					: Failure == 2 ? ERDGError::IncompatibleAllocation : ERDGError::AllocationFailed;
				EXPECT_EQ(Result.Result.Error, ExpectedError);
				EXPECT_FALSE(Result.Result.IsSuccess());
				if (Failure == 3) EXPECT_TRUE(Result.Result.Message.empty());
				EXPECT_EQ(GetCommandList().GetNumRecordedCommands(), CommandsBefore);
				EXPECT_EQ(Builder.GetState(), ERDGBuilderState::Failed);
				EXPECT_EQ(Calls, 0);
				EXPECT_EQ(Destination.GetReference(), Original);
				EXPECT_EQ(Destructions, 0);
				Capture = Builder.Capture();
				ASSERT_TRUE(Capture.bCompiled);
				EXPECT_FALSE(Capture.Transitions.empty());
				EXPECT_EQ(Builder.Execute(GetCommandList(), &Context).Status, ERDGExecutionStatus::InvalidState);
				EXPECT_EQ(Builder.Capture().Dump, Capture.Dump);
				EXPECT_EQ(Builder.GetExecutionResult().Result.Message, Result.Result.Message);
				EXPECT_EQ(Builder.GetExecutionResult().Result.Error, ExpectedError);
			}
			EXPECT_EQ(Destructions, 1);
			EXPECT_FALSE(Capture.Passes.empty());
		}
	}

	TEST_F(FRDGTests, SupportedUnwindingLeavesTerminalStateAndRetainsStorage)
	{
		for (bool InAllocator : {false, true})
		{
			int Destructions = 0;
			{
				FRDGBuilder Builder;
				const auto Value = Builder.CreateValue<FTypedValuePayload>("Tracked", "tracked", &Destructions);
				const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferDesc(
					64, 4, EBufferUsageFlags::UnorderedAccess)}, "Buffer");
				const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Throw", ERDGPassType::Compute,
					[](FRHICommandListImmediate&, const FRDGPassResources&) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); throw std::runtime_error("callback"); });
				FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, 64, ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
				FRDGBuilderTestAccessor::UseValue(Builder, Pass, Value, ERDGUse::Write);
				FTestRDGAllocator Allocator;
				if (InAllocator) Allocator.OnAllocate = [] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); throw std::runtime_error("allocator"); };
				FRDGExecutionContext Context{Allocator};
				EXPECT_THROW(Builder.Execute(GetCommandList(), &Context), std::runtime_error);
				const auto Stats = Builder.GetStatistics();
				EXPECT_GE(InAllocator ? Stats.Phases.PreparationMicroseconds
					: Stats.Phases.RecordingMicroseconds, 1000u);
				EXPECT_GE(Stats.ExecuteMicroseconds, Stats.Phases.PreparationMicroseconds
					+ Stats.Phases.RecordingMicroseconds);
				if (InAllocator) EXPECT_EQ(Stats.Phases.RecordingMicroseconds, 0u);
				EXPECT_EQ(Builder.GetState(), ERDGBuilderState::Failed);
				EXPECT_EQ(Destructions, 0);
				EXPECT_TRUE(Builder.Capture().bCompiled);
				EXPECT_EQ(Builder.Execute(GetCommandList(), &Context).Status, ERDGExecutionStatus::InvalidState);
			}
			EXPECT_EQ(Destructions, 1);
		}
	}

	TEST_F(FRDGTests, FreshExternalGraphUsesExplicitPreviousFinalAccess)
	{
		auto Texture = MakeGraphTexture("ExternalHandoff");
		FRDGBuilder First;
		const auto FirstTexture = First.RegisterExternalTexture(Texture, "External",
			ERHIAccess::GraphicsShaderRead, ERHIAccess::TransferRead);
		const auto Write = FRDGBuilderTestAccessor::AddPass(First, "Write", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseColorAttachment(First, Write, FirstTexture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store);
		ASSERT_TRUE(First.Execute(GetCommandList()).IsSuccess());
		ASSERT_EQ(First.GetPasses()[0].Barriers.GetTextureTransitions().size(), 1u);
		EXPECT_EQ(First.GetPasses()[0].Barriers.GetTextureTransitions()[0].ExpectedBefore, ERHIAccess::GraphicsShaderRead);
		const auto Before = First.Capture();
		const auto CommandCount = GetCommandList().GetNumRecordedCommands();
		EXPECT_EQ(First.Execute(GetCommandList()).Status, ERDGExecutionStatus::InvalidState);
		EXPECT_EQ(First.Capture().Dump, Before.Dump);
		EXPECT_EQ(GetCommandList().GetNumRecordedCommands(), CommandCount);
		FRDGBuilder Second;
		const auto SecondTexture = Second.RegisterExternalTexture(Texture, "External",
			ERHIAccess::TransferRead, ERHIAccess::GraphicsShaderRead);
		const auto Read = FRDGBuilderTestAccessor::AddPass(Second, "Read", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseTexture(Second, Read, SecondTexture, WholeColor(), ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
		ASSERT_TRUE(Second.Execute(GetCommandList()).IsSuccess());
		ASSERT_EQ(Second.GetPasses()[0].Barriers.GetTextureTransitions().size(), 1u);
		EXPECT_EQ(Second.GetPasses()[0].Barriers.GetTextureTransitions()[0].ExpectedBefore, ERHIAccess::TransferRead);
		EXPECT_EQ(Second.GetPasses()[0].Barriers.GetTextureTransitions()[0].RequiredAfter, ERHIAccess::GraphicsShaderRead);
	}

	TEST_F(FRDGTests, GraphParameterMetadataPreservesStableCompleteLayout)
	{
		const auto* Metadata =
			FGraphParameterLayoutFixture::GetRDGParametersMetadata();
		ASSERT_NE(Metadata, nullptr);
		EXPECT_STREQ(Metadata->StructName, "FGraphParameterLayoutFixture");
		EXPECT_EQ(Metadata->StructSize, sizeof(FGraphParameterLayoutFixture));
		EXPECT_EQ(Metadata->StructAlignment, 64u);
		ASSERT_EQ(Metadata->Members.size(), 6u);
		EXPECT_STREQ(Metadata->Members[0].Name, "Input");
		EXPECT_STREQ(Metadata->Members[1].Name, "Buffers");
		EXPECT_STREQ(Metadata->Members[2].Name, "Color");
		EXPECT_STREQ(Metadata->Members[3].Name, "Depth");
		EXPECT_STREQ(Metadata->Members[4].Name, "Managed");
		EXPECT_STREQ(Metadata->Members[5].Name, "Nested");
		EXPECT_EQ(Metadata->Members[1].ArraySize, 2u);
		EXPECT_EQ(Metadata->Members[1].ElementSize,
			sizeof(std::optional<FRDGBufferParameter>));
		EXPECT_TRUE(Metadata->Members[1].bOptional);
		EXPECT_TRUE(Metadata->Members[2].bOptional);
		EXPECT_EQ(Metadata->Members[2].LoadAction,
			ERHIRenderTargetLoadAction::Clear);
		EXPECT_TRUE(Metadata->Members[2].bPassManagedTransition);
		EXPECT_EQ(Metadata->Members[2].ResultAccess,
			ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Metadata->Members[5].Kind,
			ERDGParameterMemberKind::Nested);
		ASSERT_NE(Metadata->Members[5].NestedParameters, nullptr);
		EXPECT_STREQ(Metadata->Members[5].NestedParameters->Members[0].Name,
			"Completion");
	}

	TEST_F(FRDGTests,
		ComposedGraphMetadataCapturesStableBindingAndExactArrayElements)
	{
		auto TextureA = MakeGraphTexture("ComposedA", 2);
		auto TextureB = MakeGraphTexture("ComposedB", 2);
		FRDGBuilder Builder;
		const auto HandleA = Builder.RegisterExternalTexture(TextureA, "A", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto HandleB = Builder.RegisterExternalTexture(TextureB, "B", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		auto Parameters = Builder.AllocParameters<
			FComposedTextureArrayParameters>();
		Parameters->Textures[0] = FRDGTextureParameter{
			HandleA, {ERHITextureAspect::Color, 0, 1, 0, 1}};
		Parameters->Textures[1] = FRDGTextureParameter{
			HandleB, {ERHITextureAspect::Color, 1, 1, 0, 1}};
		bool bExecuted = false;
		Builder.AddPass("Composed", ERDGPassType::Graphics,
			std::move(Parameters),
			[&](FRHICommandListImmediate&,
				const FComposedTextureArrayParameters& Values,
				const FRDGParameterResolver& Resolver) {
				const auto ShaderParameters = Resolver.GetShaderParameters(Values);
				EXPECT_EQ(ShaderParameters.GetData(), &Values);
				EXPECT_EQ(Resolver.GetTexture(*Values.Textures[0]), TextureA.GetReference());
				EXPECT_EQ(Resolver.GetTexture(*Values.Textures[1]), TextureB.GetReference());
				bExecuted = true;
			});

		const auto Result = Builder.Execute(GetCommandList());
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Uses.size(), 2u);
		EXPECT_EQ(Capture.Uses[0].ParameterPath,
			"FComposedTextureArrayParameters.Textures[0]");
		EXPECT_EQ(Capture.Uses[1].ParameterPath,
			"FComposedTextureArrayParameters.Textures[1]");
		for (const auto& Use : Capture.Uses)
		{
			EXPECT_EQ(Use.ShaderBindingName, "Textures");
			EXPECT_EQ(Use.ShaderBindingType, ERHIBindingType::Texture);
		}
		EXPECT_NE(Capture.Dump.find("shader-binding=Textures"),
			std::string::npos);
		EXPECT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_TRUE(bExecuted);
	}

	TEST_F(FRDGTests,
		ComposedGraphMetadataRejectsAccessWeakeningBeforePassPublication)
	{
		FRDGBuilder Builder;
		auto Parameters = Builder.AllocParameters<
			FMalformedComposedAccessParameters>();
		EXPECT_FALSE(Parameters);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Result.Message.find("incompatible graph/shader declaration"),
			std::string::npos);
	}

	TEST_F(FRDGTests,
		ComposedShaderSubmissionRejectsReflectionArrayExtentBeforeRecording)
	{
		auto TextureA = MakeGraphTexture("BindingExtentA");
		auto TextureB = MakeGraphTexture("BindingExtentB");
		FRDGBuilder Builder;
		const auto HandleA = Builder.RegisterExternalTexture(TextureA, "TextureA", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto HandleB = Builder.RegisterExternalTexture(TextureB, "TextureB", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		auto Parameters = Builder.AllocParameters<
			FComposedTextureArrayParameters>();
		Parameters->Textures[0] = FRDGTextureParameter{
			HandleA, WholeColor()};
		Parameters->Textures[1] = FRDGTextureParameter{
			HandleB, WholeColor()};
		Builder.AddPass("ComposedExtent", ERDGPassType::Graphics,
			std::move(Parameters),
			[](FRHICommandListImmediate&,
				const FComposedTextureArrayParameters& Values,
				const FRDGParameterResolver& Resolver) {
				FRHICommandList Commands;
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
				auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(
					EShaderFrequency::Fragment, FXxHash128{}));
				const std::array Bindings{FShaderParameterBinding{
					.Name = "Textures", .Type = ERHIBindingType::Texture,
					.ArraySize = 1}};
				const auto GraphShaderParameters =
					Resolver.GetShaderParameters(Values);
				SetRDGShaderParametersImpl(Commands, Shader.GetReference(),
					"FExtentFixture", EShaderFrequency::Fragment, Bindings,
					GraphShaderParameters, nullptr, nullptr);
			});

		EXPECT_DEATH(Builder.Execute(GetCommandList()),
			"array extent does not match");
	}

	TEST_F(FRDGTests,
		ComposedShaderSubmissionRejectsUnavailableRequiredOptional)
	{
		FRDGBuilder Builder;
		auto Parameters = Builder.AllocParameters<
			FComposedTextureArrayParameters>();
		Builder.AddPass("ComposedOptional", ERDGPassType::Graphics,
			std::move(Parameters),
			[](FRHICommandListImmediate&,
				const FComposedTextureArrayParameters& Values,
				const FRDGParameterResolver& Resolver) {
				FRHICommandList Commands;
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
				auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(
					EShaderFrequency::Fragment, FXxHash128{}));
				const std::array Bindings{FShaderParameterBinding{
					.Name = "Textures", .Type = ERHIBindingType::Texture,
					.ArraySize = 2, .bGraphResource = true}};
				const auto GraphShaderParameters =
					Resolver.GetShaderParameters(Values);
				SetRDGShaderParametersImpl(Commands, Shader.GetReference(),
					"FOptionalFixture", EShaderFrequency::Fragment, Bindings,
					GraphShaderParameters, nullptr, nullptr);
			});

		EXPECT_DEATH(Builder.Execute(GetCommandList()),
			"is unavailable for required shader");
	}

	TEST_F(FRDGTests,
		ComposedShaderSubmissionRejectsMissingGraphAuthorityAndWrongDomain)
	{
		auto MakeResult = [&](bool bWrongDomain) {
			FRDGBuilder Builder;
			auto Parameters = Builder.AllocParameters<
				FComposedTextureArrayParameters>();
			Builder.AddPass("ComposedAuthority", ERDGPassType::Graphics,
				std::move(Parameters),
				[bWrongDomain](FRHICommandListImmediate&,
					const FComposedTextureArrayParameters& Values,
					const FRDGParameterResolver& Resolver) {
					FRHICommandList Commands;
					Commands.SwitchPipeline(ERHIPipeline::Graphics);
					auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(
						bWrongDomain ? EShaderFrequency::Compute
							: EShaderFrequency::Fragment, FXxHash128{}));
					const std::array Bindings{FShaderParameterBinding{
						.Name = "MissingGraph", .Type = ERHIBindingType::Texture,
						.bGraphResource = true}};
					const auto GraphShaderParameters =
						Resolver.GetShaderParameters(Values);
					SetRDGShaderParametersImpl(Commands,
						Shader.GetReference(), "FAuthorityFixture",
						bWrongDomain ? EShaderFrequency::Compute
							: EShaderFrequency::Fragment,
						Bindings, GraphShaderParameters, nullptr, nullptr);
				});
			return Builder.Execute(GetCommandList());
		};
		EXPECT_DEATH(MakeResult(false),
			"has no composed graph member");
		EXPECT_DEATH(MakeResult(true),
			"domain is incompatible");
	}

	TEST_F(FRDGTests,
		ComposedComputeCapturePreservesExactBufferRangesAndWriteAuthority)
	{
		static const auto InputBuffer = MakeRefCount<FRHIBuffer>(
			FRHIBufferCreateDesc::Create("ComposedInput", 128, 4,
				EBufferUsageFlags::StructuredBuffer
					| EBufferUsageFlags::UnorderedAccess));
		static const auto OutputBuffer = MakeRefCount<FRHIBuffer>(
			FRHIBufferCreateDesc::Create("ComposedOutput", 128, 4,
				EBufferUsageFlags::StructuredBuffer
					| EBufferUsageFlags::UnorderedAccess));
		FRDGBuilder Builder;
		const auto Input = Builder.RegisterExternalBuffer(InputBuffer, "Input", ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		const auto Output = Builder.RegisterExternalBuffer(OutputBuffer, "Output", ERHIAccess::ComputeShaderReadWrite, ERHIAccess::ComputeShaderReadWrite);
		auto Parameters = Builder.AllocParameters<
			FComposedComputeBufferParameters>();
		Parameters->InputBuffer = {Input, 16, 32};
		Parameters->OutputBuffer = {Output, 32, 64};
		Builder.AddPass("ComposedBuffers", ERDGPassType::Compute,
			std::move(Parameters),
			[](FRHICommandListImmediate&,
				const FComposedComputeBufferParameters&,
				const FRDGParameterResolver&) {});
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Uses.size(), 2u);
		EXPECT_EQ(Capture.Uses[0].BufferOffset, 16u);
		EXPECT_EQ(Capture.Uses[0].BufferSize, 32u);
		EXPECT_EQ(Capture.Uses[1].BufferOffset, 32u);
		EXPECT_EQ(Capture.Uses[1].BufferSize, 64u);
	}

	TEST_F(FRDGTests, AllocatesAlignedParametersWithExactRuntimeValues)
	{
		FRDGBuilder Builder;
		auto Parameters = Builder.AllocParameters<FGraphParameterLayoutFixture>();
		ASSERT_TRUE(Parameters.IsValid());
		EXPECT_EQ(reinterpret_cast<uintptr_t>(&Parameters.Get()) % 64u, 0u);

		auto Texture = MakeGraphTexture("ParameterTexture", 2);
		const auto TextureHandle = CreateTestTexture(Builder, "ParameterTexture", Texture);
		const auto TokenHandle = Builder.CreateToken("ParameterToken");
		Parameters->Input = {TextureHandle,
			{ERHITextureAspect::Color, 1, 1, 0, 1}};
		Parameters->Buffers[0] = std::nullopt;
		Parameters->Color = FRDGColorAttachmentParameter{
			TextureHandle, WholeColor(2)};
		Parameters->Nested.Completion = {TokenHandle};

		EXPECT_EQ(Parameters->Input.Texture, TextureHandle);
		EXPECT_EQ(Parameters->Input.Range.FirstMip, 1u);
		EXPECT_FALSE(Parameters->Buffers[0].has_value());
		ASSERT_TRUE(Parameters->Color.has_value());
		EXPECT_EQ(Parameters->Color->Range.NumMips, 2u);
		EXPECT_EQ(Parameters->Nested.Completion.Token, TokenHandle);
	}

	TEST_F(FRDGTests, ParameterReferencesSurviveGrowthMovesAndReverseSubmission)
	{
		FRDGBuilder Builder;
		std::vector<TRDGParametersRef<FFirstLifetimeGraphParameters>> Parameters;
		for (size_t Index = 0; Index < 256; ++Index)
			Parameters.push_back(Builder.AllocParameters<FFirstLifetimeGraphParameters>());
		size_t Calls = 0;
		for (size_t Index = Parameters.size(); Index-- > 0;)
		{
			const auto* Address = &Parameters[Index].Get();
			auto Moved = std::move(Parameters[Index]);
			EXPECT_FALSE(Parameters[Index].IsValid());
			TRDGParametersRef<FFirstLifetimeGraphParameters> Assigned;
			Assigned = std::move(Moved);
			EXPECT_FALSE(Moved.IsValid());
			if (Index % 2 == 0)
			{
				EXPECT_TRUE(Builder.AddPass("Typed" + std::to_string(Index), ERDGPassType::Copy,
					std::move(Assigned), [&, Address](FRHICommandListImmediate&,
						const FFirstLifetimeGraphParameters& Values,
						const FRDGParameterResolver&) {
						EXPECT_EQ(&Values, Address);
						++Calls;
					}).IsValid());
			}
			else
			{
				EXPECT_TRUE(FRDGBuilderTestAccessor::AddPass(Builder, "Untyped" + std::to_string(Index), ERDGPassType::Copy,
					std::move(Assigned), [&](FRHICommandListImmediate&,
						const FRDGPassResources&) { ++Calls; }).IsValid());
			}
			EXPECT_FALSE(Assigned.IsValid());
		}
		const auto Result = Builder.Execute(GetCommandList());
		EXPECT_EQ(Result.Status, ERDGExecutionStatus::Recorded) << Result.Result.Message;
		EXPECT_EQ(Calls, Parameters.size());
	}

	TEST_F(FRDGTests, RejectsMalformedGraphParameterMetadataAtomically)
	{
		FRDGBuilder Builder;
		auto Parameters = Builder.AllocParameters<FMalformedGraphParameters>();
		EXPECT_FALSE(Parameters.IsValid());
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_EQ(Result.Result.Message,
			"render graph parameter metadata for 'FMalformedGraphParameters' member "
			"'Texture' has an invalid or unstable offset");
	}

	TEST_F(FRDGTests, ParameterLayoutsAreSharedFlattenedAndTypeIsolated)
	{
		const FRDGParameterLayout* First =
			GetRDGParameterLayout<FGraphParameterLayoutFixture>();
		const FRDGParameterLayout* Second =
			GetRDGParameterLayout<FGraphParameterLayoutFixture>();
		const FRDGParameterLayout* Other =
			GetRDGParameterLayout<FNestedGraphParameters>();
		ASSERT_NE(First, nullptr);
		EXPECT_EQ(First, Second);
		EXPECT_NE(First, Other);
		EXPECT_EQ(First->Metadata,
			FGraphParameterLayoutFixture::GetRDGParametersMetadata());
		ASSERT_EQ(First->Leaves.size(), 6u);
		ASSERT_EQ(First->Elements.size(), 7u);
		EXPECT_EQ(First->Elements[0].FieldPath,
			"FGraphParameterLayoutFixture.Input");
		EXPECT_EQ(First->Elements[2].FieldPath,
			"FGraphParameterLayoutFixture.Buffers[1]");
		EXPECT_EQ(First->Elements.back().FieldPath,
			"FGraphParameterLayoutFixture.Nested.Completion");
		EXPECT_EQ(First->TextureElements.size(), 2u);
		EXPECT_EQ(First->BufferElements.size(), 2u);
		EXPECT_EQ(First->AttachmentElements.size(), 2u);
		EXPECT_EQ(First->TokenElements.size(), 1u);
		EXPECT_TRUE(First->ValueElements.empty());
		EXPECT_EQ(First->OffsetIndex.size(), First->Elements.size());
		const FRDGParameterLayout* Composed =
			GetRDGParameterLayout<FComposedTextureArrayParameters>();
		ASSERT_NE(Composed, nullptr);
		ASSERT_EQ(Composed->ShaderBindings.size(), 1u);
		EXPECT_EQ(Composed->ShaderBindings[0].Name, "Textures");
		EXPECT_EQ(Composed->Leaves[
			Composed->ShaderBindings[0].LeafIndex].Metadata->ArraySize, 2u);

		const auto& InvalidFirst =
			GetRDGParameterLayoutBuildResult<FMalformedGraphParameters>();
		const auto& InvalidSecond =
			GetRDGParameterLayoutBuildResult<FMalformedGraphParameters>();
		EXPECT_EQ(&InvalidFirst, &InvalidSecond);
		EXPECT_EQ(InvalidFirst.Layout, nullptr);
		EXPECT_EQ(InvalidFirst.Result.Error, ERDGError::InvalidParameterMetadata);
		EXPECT_EQ(InvalidFirst.Result.Message,
			"render graph parameter metadata for 'FMalformedGraphParameters' member "
			"'Texture' has an invalid or unstable offset");
	}

	TEST_F(FRDGTests, DestroysUncompiledParametersExactlyOnceInReverseOrder)
	{
		std::vector<int> DestructionOrder;
		GParameterDestructionOrder = &DestructionOrder;
		{
			FRDGBuilder Builder;
			auto First = Builder.AllocParameters<FFirstLifetimeGraphParameters>();
			auto Second = Builder.AllocParameters<FSecondLifetimeGraphParameters>();
			ASSERT_TRUE(First.IsValid());
			ASSERT_TRUE(Second.IsValid());
		}
		GParameterDestructionOrder = nullptr;
		EXPECT_EQ(DestructionOrder, (std::vector<int>{2, 1}));
	}

	TEST_F(FRDGTests, KeepsParametersWithBuilderAcrossCompileFailure)
	{
		std::vector<int> DestructionOrder;
		GParameterDestructionOrder = &DestructionOrder;
		{
			FRDGBuilder Builder;
			auto Parameters =
				Builder.AllocParameters<FFirstLifetimeGraphParameters>();
			Builder.SetBudget({.MaxPasses = 0});
			FRDGBuilderTestAccessor::AddPass(Builder, "Rejected", ERDGPassType::Graphics);
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_TRUE(Parameters.IsValid());
			EXPECT_TRUE(DestructionOrder.empty());
		}
		GParameterDestructionOrder = nullptr;
		EXPECT_EQ(DestructionOrder, (std::vector<int>{1}));
	}

	TEST_F(FRDGTests, KeepsParametersUntilBuilderDestruction)
	{
		std::vector<int> DestructionOrder;
		GParameterDestructionOrder = &DestructionOrder;
		TRDGParametersRef<FFirstLifetimeGraphParameters> Parameters;
		{
			FRDGBuilder Builder;
			Parameters = Builder.AllocParameters<FFirstLifetimeGraphParameters>();
			EXPECT_TRUE(Builder.Execute(GetCommandList()).IsSuccess());
			EXPECT_TRUE(Parameters.IsValid());
			EXPECT_TRUE(DestructionOrder.empty());
		}
		EXPECT_FALSE(Parameters.IsValid());
		GParameterDestructionOrder = nullptr;
		EXPECT_EQ(DestructionOrder, (std::vector<int>{1}));
	}

	TEST_F(FRDGTests, KeepsParametersAcrossPreparationFailure)
	{
		std::vector<int> DestructionOrder;
		GParameterDestructionOrder = &DestructionOrder;
		TRDGParametersRef<FFirstLifetimeGraphParameters> Parameters;
		{
			FRDGBuilder Builder;
			Parameters = Builder.AllocParameters<FFirstLifetimeGraphParameters>();
			const auto Texture = Builder.CreateTexture(FRDGTextureDesc{
				.Texture = FRHITextureCreateDesc::Create2D("MissingBacking", 16, 16,
					EPixelFormat::RGBA8_UNORM)}, "MissingBacking");
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "UseMissingBacking", ERDGPassType::Graphics);
			FRDGBuilderTestAccessor::UseColorAttachment(Builder, Pass, Texture, WholeColor(),
				ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store);
			EXPECT_EQ(Builder.Execute(GetCommandList()).Status, ERDGExecutionStatus::PreparationFailed);
			EXPECT_TRUE(Parameters.IsValid());
			EXPECT_TRUE(DestructionOrder.empty());
		}
		EXPECT_FALSE(Parameters.IsValid());
		GParameterDestructionOrder = nullptr;
		EXPECT_EQ(DestructionOrder, (std::vector<int>{1}));
	}

	TEST_F(FRDGTests, ParameterizedPassMatchesEveryManualUseKind)
	{
		auto BuildCapture = [](bool bParameterized) {
			auto InputTexture = MakeGraphTexture("Input");
			auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
				"Buffer", 256, 4, EBufferUsageFlags::UnorderedAccess
			));
			auto ColorTexture = MakeGraphTexture("Color");
			auto DepthTexture = MakeRefCount<FRHITexture>(FRHITextureCreateDesc::Create2D(
								 "Depth", 64, 64, EPixelFormat::D32
			)
								 .SetFlags(ETextureCreateFlags::DepthStencilTargetable));
			auto ManagedColorTexture = MakeGraphTexture("ManagedColor");
			auto ManagedDepthTexture = MakeRefCount<FRHITexture>(FRHITextureCreateDesc::Create2D(
								 "ManagedDepth", 64, 64, EPixelFormat::D32
			)
								 .SetFlags(ETextureCreateFlags::DepthStencilTargetable));
			auto ManagedTexture = MakeGraphTexture("ManagedTexture");

			FRDGBuilder Builder;
			const auto Input = Builder.RegisterExternalTexture(InputTexture, "Input", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			const auto BufferHandle = Builder.RegisterExternalBuffer(Buffer, "Buffer", ERHIAccess::GraphicsShaderReadWrite, ERHIAccess::GraphicsShaderReadWrite);
			const auto Color = CreateTestTexture(Builder, "Color", ColorTexture);
			const auto Depth = CreateTestTexture(Builder, "Depth", DepthTexture);
			const auto ManagedColor = CreateTestTexture(Builder, "ManagedColor", ManagedColorTexture, ERHIAccess::GraphicsShaderRead);
			const auto ManagedDepth = CreateTestTexture(Builder, "ManagedDepth", ManagedDepthTexture, ERHIAccess::GraphicsShaderRead);
			const auto Managed = CreateTestTexture(Builder, "ManagedTexture", ManagedTexture, ERHIAccess::GraphicsShaderRead);
			const auto Completion = Builder.CreateToken("Completion");

			if (bParameterized)
			{
				auto Parameters = Builder.AllocParameters<FAllGraphUseParameters>();
				Parameters->Inputs[0] = FRDGTextureParameter{
					Input, WholeColor()};
				Parameters->Inputs[1] = std::nullopt;
				Parameters->Buffer = {BufferHandle, 32, 128};
				Parameters->Color = {Color, WholeColor()};
				Parameters->Depth = {Depth,
					{ERHITextureAspect::Depth, 0, 1, 0, 1}};
				Parameters->ManagedColor = {ManagedColor, WholeColor()};
				Parameters->ManagedDepth =
					FRDGDepthStencilAttachmentParameter{ManagedDepth,
						{ERHITextureAspect::Depth, 0, 1, 0, 1}};
				Parameters->ManagedTexture = {Managed, WholeColor()};
				Parameters->Nested.Completion = {Completion};
				const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "AllUses",
					ERDGPassType::Graphics, std::move(Parameters));
				EXPECT_TRUE(Pass.IsValid());
			}
			else
			{
				const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder,
					"AllUses", ERDGPassType::Graphics);
				FRDGBuilderTestAccessor::UseTexture(Builder, Pass, Input, WholeColor(),
					ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
				FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, BufferHandle, 32, 128,
					ERDGUse::ReadWrite,
					ERHIAccess::GraphicsShaderReadWrite);
				FRDGBuilderTestAccessor::UseColorAttachment(Builder, Pass, Color, WholeColor(),
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store);
				FRDGBuilderTestAccessor::UseDepthStencilAttachment(Builder, Pass, Depth,
					{ERHITextureAspect::Depth, 0, 1, 0, 1},
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store);
				FRDGBuilderTestAccessor::UseManagedColorAttachment(Builder, Pass, ManagedColor, WholeColor(),
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store,
					ERHIAccess::GraphicsShaderRead);
				FRDGBuilderTestAccessor::UseManagedDepthStencilAttachment(Builder, Pass, ManagedDepth,
					{ERHITextureAspect::Depth, 0, 1, 0, 1},
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store,
					ERHIAccess::GraphicsShaderRead);
				FRDGBuilderTestAccessor::UseManagedTexture(Builder, Pass, Managed, WholeColor(),
					ERDGUse::Write,
					ERHIAccess::GraphicsShaderReadWrite,
					ERHIAccess::GraphicsShaderRead, true);
				FRDGBuilderTestAccessor::UseToken(Builder, Pass, Completion, ERDGUse::Write);
			}

			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			return Builder.Capture();
		};

		const auto Manual = BuildCapture(false);
		const auto Parameterized = BuildCapture(true);
		const auto ParameterizedAgain = BuildCapture(true);
		EXPECT_EQ(StripParameterFields(Parameterized.Dump), Manual.Dump);
		EXPECT_EQ(Parameterized.Dump, ParameterizedAgain.Dump);
		ASSERT_EQ(Parameterized.Passes.size(), 1u);
		EXPECT_EQ(Parameterized.Passes[0].ParameterStructName,
			"FAllGraphUseParameters");
		EXPECT_TRUE(Manual.Passes[0].ParameterStructName.empty());
		ASSERT_EQ(Parameterized.Parameters.size(), 9u);
		EXPECT_EQ(Parameterized.Parameters[0].FieldPath,
			"FAllGraphUseParameters.Inputs[0]");
		EXPECT_TRUE(Parameterized.Parameters[0].bPresent);
		EXPECT_EQ(Parameterized.Parameters[0].ResourceId,
			Parameterized.Uses[0].ResourceId);
		EXPECT_EQ(Parameterized.Parameters[1].FieldPath,
			"FAllGraphUseParameters.Inputs[1]");
		EXPECT_FALSE(Parameterized.Parameters[1].bPresent);
		EXPECT_EQ(Parameterized.Parameters[1].ResourceId,
			std::numeric_limits<uint32>::max());
		EXPECT_EQ(Parameterized.Parameters.back().Kind,
			ERDGParameterMemberKind::Token);
		ASSERT_EQ(Parameterized.Uses.size(), 8u);
		const std::array ExpectedPaths{
			"FAllGraphUseParameters.Inputs[0]",
			"FAllGraphUseParameters.Buffer",
			"FAllGraphUseParameters.Color",
			"FAllGraphUseParameters.Depth",
			"FAllGraphUseParameters.ManagedColor",
			"FAllGraphUseParameters.ManagedDepth",
			"FAllGraphUseParameters.ManagedTexture",
			"FAllGraphUseParameters.Nested.Completion",
		};
		for (uint32 Index = 0; Index < ExpectedPaths.size(); ++Index)
			EXPECT_EQ(Parameterized.Uses[Index].ParameterPath,
				ExpectedPaths[Index]);
		EXPECT_TRUE(Manual.Uses[0].ParameterPath.empty());
		EXPECT_NE(Parameterized.Dump.find(
			"field=FAllGraphUseParameters.Nested.Completion"), std::string::npos);
		EXPECT_NE(Parameterized.Dump.find(
			"parameter pass=0 field=FAllGraphUseParameters.Inputs[1] kind=texture "
			"present=0 resource=none"), std::string::npos);
	}

	TEST_F(FRDGTests, ParameterizedPassRejectsExactInvalidFieldPaths)
	{
		auto Texture = MakeGraphTexture("Texture");
		auto ForeignTexture = MakeGraphTexture("ForeignTexture");
		FRDGBuilder ForeignBuilder;
		const auto Foreign = CreateTestTexture(ForeignBuilder, "ForeignTexture", ForeignTexture);

		{
			FRDGBuilder Builder;
			const auto Local = Builder.RegisterExternalTexture(Texture, "Texture", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
			Parameters->Textures = {{{Local, WholeColor()},
				{Foreign, WholeColor()}}};
			EXPECT_FALSE(FRDGBuilderTestAccessor::AddPass(Builder, "ForeignHandle",
				ERDGPassType::Graphics, std::move(Parameters)).IsValid());
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_EQ(Result.Result.Message,
				"pass 'ForeignHandle' parameter 'FTwoTextureGraphParameters.Textures[1]' "
				"has an invalid resource handle");
		}

		{
			FRDGBuilder Builder;
			const auto Local = Builder.RegisterExternalTexture(Texture, "Texture", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
			Parameters->Textures = {{{Local,
				{ERHITextureAspect::Color, 1, 1, 0, 1}},
				{Local, WholeColor()}}};
			EXPECT_FALSE(FRDGBuilderTestAccessor::AddPass(Builder, "InvalidRange",
				ERDGPassType::Graphics, std::move(Parameters)).IsValid());
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_EQ(Result.Result.Message,
				"pass 'InvalidRange' parameter 'FTwoTextureGraphParameters.Textures[0]' "
				"resource 'Texture' has invalid texture range");
		}

		{
			auto BuildOverlapError = [&] {
				FRDGBuilder Builder;
				const auto Local = Builder.RegisterExternalTexture(Texture, "Texture", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
				auto Parameters =
					Builder.AllocParameters<FTwoTextureGraphParameters>();
				Parameters->Textures = {{{Local, WholeColor()},
					{Local, {ERHITextureAspect::Color, 0, 1, 0, 1}}}};
				EXPECT_FALSE(FRDGBuilderTestAccessor::AddPass(Builder, "Overlap",
					ERDGPassType::Graphics,
					std::move(Parameters)).IsValid());
				return FRDGBuilderTestAccessor::Compile(Builder).Result.Message;
			};
			const std::string Error = BuildOverlapError();
			EXPECT_EQ(Error,
				"pass 'Overlap' parameter 'FTwoTextureGraphParameters.Textures[1]' "
				"declares overlapping uses of resource 'Texture' with parameter "
				"'FTwoTextureGraphParameters.Textures[0]'");
			EXPECT_EQ(BuildOverlapError(), Error);
		}

		{
			FRDGBuilder Builder;
			const auto Local = Builder.RegisterExternalTexture(Texture, "Texture", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
			Parameters->Textures = {{{Local, WholeColor()},
				{Local, WholeColor()}}};
			EXPECT_FALSE(FRDGBuilderTestAccessor::AddPass(Builder, "WrongDomain",
				ERDGPassType::Compute, std::move(Parameters)).IsValid());
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_EQ(Result.Result.Message,
				"pass 'WrongDomain' parameter 'FTwoTextureGraphParameters.Textures[0]' "
				"resource 'Texture' access is incompatible with pass domain");
		}
	}

	TEST_F(FRDGTests, ValidatedParameterDeclarationsStillRequireGraphValidation)
	{
		{
			FRDGBuilder Builder;
			for (uint32 Index = 0; Index < 2; ++Index)
			{
				auto Parameters = Builder.AllocParameters<FComposedTextureArrayParameters>();
				ASSERT_TRUE(FRDGBuilderTestAccessor::AddPass(Builder, "Duplicate", ERDGPassType::Graphics,
					std::move(Parameters)).IsValid());
			}
			EXPECT_EQ(FRDGBuilderTestAccessor::Compile(Builder).Result.Message,
				"duplicate pass name 'Duplicate'");
		}
		{
			FRDGBuilder Builder;
			auto Parameters = Builder.AllocParameters<FComposedTextureArrayParameters>();
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Cyclic", ERDGPassType::Graphics,
				std::move(Parameters));
			ASSERT_TRUE(Pass.IsValid());
			Builder.AddPassDependency(Pass, Pass);
			EXPECT_EQ(FRDGBuilderTestAccessor::Compile(Builder).Result.Message,
				"dependency must point forward: producer[0] consumer[0]");
		}
		{
			FRDGBuilder Builder;
			const auto Texture = CreateTestTexture(Builder, "Missing", MakeGraphTexture("Missing"));
			auto Parameters = Builder.AllocParameters<FComposedTextureArrayParameters>();
			Parameters->Textures[0] = FRDGTextureParameter{Texture, WholeColor()};
			ASSERT_TRUE(FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Graphics,
				std::move(Parameters)).IsValid());
			const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_NE(Result.Result.Message.find("before its producer"), std::string::npos);
		}
	}

	TEST_F(FRDGTests, ManualTextureDeclarationsRejectInvalidAndParameterizedPasses)
	{
		using FDeclare = void (*)(FRDGBuilder&, FRDGPassHandle, FRDGTextureHandle);
		const std::array<FDeclare, 6> Declarations{
			[](FRDGBuilder& Builder, FRDGPassHandle Pass, FRDGTextureHandle Texture) {
				FRDGBuilderTestAccessor::UseTexture(Builder, Pass, Texture, WholeColor(),
					ERDGUse::Write, ERHIAccess::GraphicsShaderReadWrite, true);
			},
			[](FRDGBuilder& Builder, FRDGPassHandle Pass, FRDGTextureHandle Texture) {
				FRDGBuilderTestAccessor::UseColorAttachment(Builder, Pass, Texture, WholeColor(),
					ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::DontCare);
			},
			[](FRDGBuilder& Builder, FRDGPassHandle Pass, FRDGTextureHandle Texture) {
				FRDGBuilderTestAccessor::UseDepthStencilAttachment(Builder, Pass, Texture, WholeColor(),
					ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::DontCare);
			},
			[](FRDGBuilder& Builder, FRDGPassHandle Pass, FRDGTextureHandle Texture) {
				FRDGBuilderTestAccessor::UseManagedColorAttachment(Builder, Pass, Texture, WholeColor(),
					ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::DontCare, ERHIAccess::GraphicsShaderRead);
			},
			[](FRDGBuilder& Builder, FRDGPassHandle Pass, FRDGTextureHandle Texture) {
				FRDGBuilderTestAccessor::UseManagedDepthStencilAttachment(Builder, Pass, Texture, WholeColor(),
					ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::DontCare, ERHIAccess::GraphicsShaderRead);
			},
			[](FRDGBuilder& Builder, FRDGPassHandle Pass, FRDGTextureHandle Texture) {
				FRDGBuilderTestAccessor::UseManagedTexture(Builder, Pass, Texture, WholeColor(),
					ERDGUse::Write, ERHIAccess::GraphicsShaderReadWrite, ERHIAccess::GraphicsShaderRead, true);
			}};
		for (size_t Index = 0; Index < Declarations.size(); ++Index)
			for (const bool bParameterized : {false, true})
			{
				SCOPED_TRACE(Index);
				SCOPED_TRACE(bParameterized);
				FRDGBuilder Builder;
				FRDGPassHandle Pass;
				if (bParameterized)
				{
					auto Parameters = Builder.AllocParameters<FNestedGraphParameters>();
					Parameters->Completion = {Builder.CreateToken("Completion")};
					Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Parameterized",
						ERDGPassType::Graphics, std::move(Parameters));
					ASSERT_TRUE(Pass.IsValid());
				}
				Declarations[Index](Builder, Pass, {});
				const auto Evidence = FRDGBuilderTestAccessor::Compile(Builder);
				EXPECT_FALSE(Evidence.IsSuccess());
				EXPECT_EQ(Evidence.Result.Message, bParameterized
					? "pass 'Parameterized' uses parameter declarations and cannot accept manual uses"
					: "texture use has an invalid pass handle");
			}
	}

	TEST_F(FRDGTests, ParameterizedPassRejectsMixedAndConsumedAuthority)
	{
		{
			FRDGBuilder Builder;
			const auto Token = Builder.CreateToken("Token");
			auto Parameters = Builder.AllocParameters<FNestedGraphParameters>();
			Parameters->Completion = {Token};
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Parameterized",
				ERDGPassType::Graphics, std::move(Parameters));
			ASSERT_TRUE(Pass.IsValid());
			FRDGBuilderTestAccessor::UseToken(Builder, Pass, Token, ERDGUse::Write);
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_EQ(Result.Result.Message,
				"pass 'Parameterized' uses parameter declarations and cannot accept manual uses");
		}

		{
			FRDGBuilder Builder;
			const auto Token = Builder.CreateToken("Token");
			auto Parameters = Builder.AllocParameters<FNestedGraphParameters>();
			Parameters->Completion = {Token};
			EXPECT_TRUE(FRDGBuilderTestAccessor::AddPass(Builder, "First", ERDGPassType::Graphics,
				std::move(Parameters)).IsValid());
			EXPECT_FALSE(Parameters.IsValid());
			EXPECT_FALSE(FRDGBuilderTestAccessor::AddPass(Builder, "Second", ERDGPassType::Graphics,
				std::move(Parameters)).IsValid());
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_EQ(Result.Result.Message,
				"pass 'Second' parameter 'FNestedGraphParameters' has an invalid or "
				"foreign parameter allocation");
		}

		{
			FRDGBuilder Owner;
			auto Parameters = Owner.AllocParameters<FNestedGraphParameters>();
			FRDGBuilder Other;
			// The foreign index is in range and has the same layout locally.
			auto Local = Other.AllocParameters<FNestedGraphParameters>();
			EXPECT_FALSE(FRDGBuilderTestAccessor::AddPass(Other, "ForeignAllocation",
				ERDGPassType::Graphics, std::move(Parameters)).IsValid());
			auto Result = FRDGBuilderTestAccessor::Compile(Other);
			EXPECT_EQ(Result.Result.Message,
				"pass 'ForeignAllocation' parameter 'FNestedGraphParameters' has an "
				"invalid or foreign parameter allocation");
		}
	}

	TEST_F(FRDGTests, ParameterizedDeclarationFailureIsCallbackAtomic)
	{
		auto Texture = MakeGraphTexture("Texture");
		FRDGBuilder Builder;
		const auto Local = Builder.RegisterExternalTexture(Texture, "Texture", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
		Parameters->Textures = {{{Local,
			{ERHITextureAspect::Color, 4, 1, 0, 1}}, {Local, WholeColor()}}};
		bool bExecuted = false;
		Builder.AddPass("Invalid", ERDGPassType::Graphics,
			std::move(Parameters),
			[&](FRHICommandListImmediate&, const FTwoTextureGraphParameters&,
				const FRDGParameterResolver&) {
				bExecuted = true;
			});
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_FALSE(bExecuted);
	}

	TEST_F(FRDGTests, ParameterTraversalStaysWithinFoundationBudget)
	{
		FRDGBuilder Builder;
		Builder.SetBudget({.MaxCompileMicroseconds = 1'000'000});
		const auto Started = std::chrono::steady_clock::now();
		auto Parameters = Builder.AllocParameters<FLargeTokenGraphParameters>();
		for (uint32 Index = 0; Index < Parameters->Tokens.size(); ++Index)
			Parameters->Tokens[Index] = {
				Builder.CreateToken("Token." + std::to_string(Index))};
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "LargeParameters",
			ERDGPassType::Graphics, std::move(Parameters));
		ASSERT_TRUE(Pass.IsValid());
		const auto DeclarationMicroseconds = std::chrono::duration_cast<
			std::chrono::microseconds>(std::chrono::steady_clock::now() - Started)
			.count();
		EXPECT_LT(DeclarationMicroseconds, 1'000'000);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_EQ(Builder.Capture().Uses.size(), 128u);
		EXPECT_FALSE(Builder.GetStatistics().bCompileBudgetExceeded);
		EXPECT_EQ(Builder.Capture().Uses.back().ParameterPath,
			"FLargeTokenGraphParameters.Tokens[127]");
	}

	TEST_F(FRDGTests, ParameterResolverResolvesDeclaredGraphicsResources)
	{
		auto InputTexture = MakeGraphTexture("Input");
		auto ColorTexture = MakeGraphTexture("Color");
		auto DepthTexture = MakeRefCount<FRHITexture>(FRHITextureCreateDesc::Create2D(
							 "Depth", 64, 64, EPixelFormat::D32
		)
							 .SetFlags(ETextureCreateFlags::DepthStencilTargetable));
		auto ManagedTexture = MakeGraphTexture("ManagedTexture");
		FRDGBuilder Builder;
		const auto Input = Builder.RegisterExternalTexture(InputTexture, "Input", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Color = CreateTestTexture(Builder, "Color", ColorTexture, ERHIAccess::GraphicsShaderRead);
		const auto Depth = Builder.RegisterExternalTexture(DepthTexture, "Depth", ERHIAccess::DepthStencilReadWrite, ERHIAccess::DepthStencilReadWrite);
		const auto Managed = CreateTestTexture(Builder, "Managed", ManagedTexture, ERHIAccess::GraphicsShaderRead);
		const auto Completion = Builder.CreateToken("Completion");
		auto Parameters = Builder.AllocParameters<FGraphParameterLayoutFixture>();
		Parameters->Input = {Input, WholeColor()};
		Parameters->Buffers[0] = std::nullopt;
		Parameters->Buffers[1] = std::nullopt;
		Parameters->Color = {Color, WholeColor()};
		Parameters->Depth = {Depth, {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		Parameters->Managed = {Managed, WholeColor()};
		Parameters->Nested.Completion = {Completion};
		uint32 CallbackCount = 0;
		Builder.AddPass("ResolveGraphics", ERDGPassType::Graphics,
			std::move(Parameters),
			[&](FRHICommandListImmediate&,
				const FGraphParameterLayoutFixture& Values,
				const FRDGParameterResolver& Resolver) {
				static_assert(std::is_const_v<std::remove_reference_t<decltype(Values)>>);
				EXPECT_EQ(Resolver.GetTexture(Values.Input), InputTexture.GetReference());
				EXPECT_EQ(Resolver.GetBuffer(Values.Buffers[0]), nullptr);
				const auto ColorView = Resolver.GetColorAttachment(Values.Color);
				EXPECT_EQ(ColorView.Texture, ColorTexture.GetReference());
				EXPECT_EQ(ColorView.LoadAction, ERHIRenderTargetLoadAction::Clear);
				EXPECT_TRUE(ColorView.bPassManagedTransition);
				const auto DepthView = Resolver.GetDepthStencilAttachment(Values.Depth);
				EXPECT_EQ(DepthView.Texture, DepthTexture.GetReference());
				EXPECT_EQ(Resolver.GetTexture(Values.Managed), ManagedTexture.GetReference());
				++CallbackCount;
			});

		FTestRDGAllocator Allocator;
		Allocator.TextureOverrides.emplace(1, ColorTexture);
		Allocator.TextureOverrides.emplace(3, ManagedTexture);
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
		EXPECT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_EQ(CallbackCount, 1u);
	}

	TEST_F(FRDGTests, ParameterResolverAuthorizesOptionalObjectAndContainedValue)
	{
		auto Texture = MakeGraphTexture("Optional", 2);
		FRDGBuilder Builder;
		const auto Handle = Builder.RegisterExternalTexture(Texture, "Optional",
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		auto Parameters =
			Builder.AllocParameters<FComposedTextureArrayParameters>();
		Parameters->Textures[0] = FRDGTextureParameter{Handle,
			{ERHITextureAspect::Color, 0, 1, 0, 1}};
		Parameters->Textures[1] = FRDGTextureParameter{Handle,
			{ERHITextureAspect::Color, 1, 1, 0, 1}};
		Builder.AddPass("OptionalAliases", ERDGPassType::Graphics,
			std::move(Parameters),
			[&](FRHICommandListImmediate&,
				const FComposedTextureArrayParameters& Values,
				const FRDGParameterResolver& Resolver) {
				EXPECT_EQ(Resolver.GetTexture(Values.Textures[0]),
					Texture.GetReference());
				EXPECT_EQ(Resolver.GetTexture(*Values.Textures[1]),
					Texture.GetReference());
			});

		const auto Result = Builder.Execute(GetCommandList());
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
		EXPECT_TRUE(Result.IsSuccess()) << Result.Result.Message;
	}

	TEST_F(FRDGTests, ParameterResolverSupportsComputeAndCopyDomains)
	{
		auto ComputeTexture = MakeGraphTexture("Compute");
		auto CopyTexture = MakeGraphTexture("Copy");
		auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"Buffer", 64, 4, EBufferUsageFlags::UnorderedAccess
		));
		FRDGBuilder Builder;
		const auto ComputeTextureHandle = Builder.RegisterExternalTexture(ComputeTexture, "Compute", ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		const auto CopyTextureHandle = Builder.RegisterExternalTexture(CopyTexture, "Copy", ERHIAccess::TransferRead, ERHIAccess::TransferRead);
		const auto BufferHandle = Builder.RegisterExternalBuffer(Buffer, "Buffer", ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		auto ComputeParameters = Builder.AllocParameters<FComputeResolutionParameters>();
		ComputeParameters->Texture = {ComputeTextureHandle, WholeColor()};
		ComputeParameters->Buffer = {BufferHandle, 0, 64};
		uint32 CallbackCount = 0;
		Builder.AddPass("ResolveCompute", ERDGPassType::Compute, std::move(ComputeParameters), [&](FRHICommandListImmediate&, const FComputeResolutionParameters& Values, const FRDGParameterResolver& Resolver) {
			EXPECT_EQ(Resolver.GetTexture(Values.Texture), ComputeTexture.GetReference());
			EXPECT_EQ(Resolver.GetBuffer(Values.Buffer), Buffer.GetReference());
			++CallbackCount;
		});
		auto CopyParameters = Builder.AllocParameters<FCopyResolutionParameters>();
		CopyParameters->Texture = {CopyTextureHandle, WholeColor()};
		Builder.AddPass("ResolveCopy", ERDGPassType::Copy, std::move(CopyParameters), [&](FRHICommandListImmediate&, const FCopyResolutionParameters& Values, const FRDGParameterResolver& Resolver) {
			EXPECT_EQ(Resolver.GetTexture(Values.Texture), CopyTexture.GetReference());
			++CallbackCount;
		});

		const auto Result = Builder.Execute(GetCommandList());
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
		EXPECT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_EQ(CallbackCount, 2u);
	}

	TEST_F(FRDGTests, ParameterResolverRejectsRawWrongKindAndWrongPassAccess)
	{
		static_assert(!CTextureResolverArgument<FRDGTextureHandle>);
		static_assert(!CTextureResolverArgument<FRDGBufferParameter>);
		auto FirstTexture = MakeGraphTexture("First", 2);
		auto SecondTexture = MakeGraphTexture("Second", 2);
		FRDGBuilder Builder;
		const auto First = Builder.RegisterExternalTexture(FirstTexture, "First", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Second = Builder.RegisterExternalTexture(SecondTexture, "Second", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		auto FirstParameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
		FirstParameters->Textures = {{{First,
			{ERHITextureAspect::Color, 0, 1, 0, 1}}, {First,
			{ERHITextureAspect::Color, 1, 1, 0, 1}}}};
		auto SecondParameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
		SecondParameters->Textures = {{{Second,
			{ERHITextureAspect::Color, 0, 1, 0, 1}}, {Second,
			{ERHITextureAspect::Color, 1, 1, 0, 1}}}};
		const auto* WrongPassMember = &SecondParameters->Textures[0];
		Builder.AddPass("FirstPass", ERDGPassType::Graphics,
			std::move(FirstParameters),
			[WrongPassMember](FRHICommandListImmediate&,
				const FTwoTextureGraphParameters&,
				const FRDGParameterResolver& Resolver) {
				Resolver.GetTexture(*WrongPassMember);
			});
		FRDGBuilderTestAccessor::AddPass(Builder, "SecondPass", ERDGPassType::Graphics,
			std::move(SecondParameters));

		EXPECT_DEATH(Builder.Execute(GetCommandList()),
			"pass 'FirstPass'.*requested capability 'texture'");
	}

	TEST_F(FRDGTests, ParameterResolverRejectsCopiedAndForeignOptionalMembers)
	{
		auto Texture = MakeGraphTexture("Declared", 2);
		FRDGBuilder Builder;
		const auto Handle = Builder.RegisterExternalTexture(Texture, "Declared", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
		Parameters->Textures = {{{Handle,
			{ERHITextureAspect::Color, 0, 1, 0, 1}}, {Handle,
			{ERHITextureAspect::Color, 1, 1, 0, 1}}}};
		const FRDGTextureParameter Copied = Parameters->Textures[0];
		const std::optional<FRDGTextureParameter> ForeignOptional;
		Builder.AddPass("Copied", ERDGPassType::Graphics,
			std::move(Parameters),
			[&](FRHICommandListImmediate&, const FTwoTextureGraphParameters&,
				const FRDGParameterResolver& Resolver) {
				if (Copied.Texture.IsValid()) Resolver.GetTexture(Copied);
				else Resolver.GetTexture(ForeignOptional);
			});

		EXPECT_DEATH(Builder.Execute(GetCommandList()),
			"not declared by the executing pass parameters");

		// An empty optional is nullable only when that exact optional object is a
		// declared field; a foreign empty optional remains an invalid capability.
		FRDGBuilder OptionalBuilder;
		const auto OptionalHandle = OptionalBuilder.RegisterExternalTexture(Texture, "Declared", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		auto OptionalParameters =
			OptionalBuilder.AllocParameters<FTwoTextureGraphParameters>();
		OptionalParameters->Textures = {{{OptionalHandle,
			{ERHITextureAspect::Color, 0, 1, 0, 1}}, {OptionalHandle,
			{ERHITextureAspect::Color, 1, 1, 0, 1}}}};
		OptionalBuilder.AddPass("ForeignOptional", ERDGPassType::Graphics,
			std::move(OptionalParameters),
			[&](FRHICommandListImmediate&, const FTwoTextureGraphParameters&,
				const FRDGParameterResolver& Resolver) {
				Resolver.GetTexture(ForeignOptional);
			});

		EXPECT_DEATH(OptionalBuilder.Execute(
			GetCommandList()),
			"not declared by the executing pass parameters");
	}

	TEST_F(FRDGTests, ParameterizedCallbacksStayAtomicWhenCulledOrUnavailable)
	{
		{
			FRDGBuilder Builder;
			Builder.EnablePassCulling();
			const auto Token = Builder.CreateToken("CulledToken");
			auto Parameters = Builder.AllocParameters<FNestedGraphParameters>();
			Parameters->Completion = {Token};
			bool bExecuted = false;
			Builder.AddPass("Culled", ERDGPassType::Graphics,
				std::move(Parameters),
				[&](FRHICommandListImmediate&, const FNestedGraphParameters&,
					const FRDGParameterResolver&) { bExecuted = true; });

			const auto Result = Builder.Execute(GetCommandList());
			ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
			const auto Capture = Builder.Capture();
			EXPECT_TRUE(Capture.Passes.empty());
			ASSERT_EQ(Capture.Parameters.size(), 1u);
			EXPECT_EQ(Capture.Parameters[0].PassDeclarationIndex, 0u);
			EXPECT_EQ(Capture.Parameters[0].FieldPath,
				"FNestedGraphParameters.Completion");
			EXPECT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			EXPECT_FALSE(bExecuted);
		}
		{
			FRDGBuilder Builder;
			const auto Buffer = Builder.CreateBuffer(
				FRDGBufferDesc{.Buffer = FRHIBufferDesc(
					64, 4, EBufferUsageFlags::UnorderedAccess)}, "Unavailable");
			auto Parameters = Builder.AllocParameters<FUnavailableBufferParameters>();
			Parameters->Buffer = {Buffer, 0, 64};
			bool bExecuted = false;
			Builder.AddPass("Unavailable", ERDGPassType::Compute,
				std::move(Parameters),
				[&](FRHICommandListImmediate&, const FUnavailableBufferParameters&,
					const FRDGParameterResolver&) { bExecuted = true; });

			FTestRDGAllocator Allocator;
			Allocator.bOmitResources = true;
			FRDGExecutionContext Context{Allocator};
			const auto Result = Builder.Execute(GetCommandList(), &Context);
			ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
			std::string Error;
			Error = Result.Result.Message;
			EXPECT_FALSE(Result.IsSuccess()) << Result.Result.Message;
			EXPECT_NE(Error.find("omitted retained resource id="),
				std::string::npos);
			EXPECT_FALSE(bExecuted);
		}
	}

	TEST_F(FRDGTests, LogicalBarrierBatchesResolveBackingsWithoutMutatingPlan)
	{
		FBarrierRecordingContext Backend;
		FRHICommandListExecutor Executor(Backend);
		auto& Commands = Executor.GetImmediateCommandList();
		const auto BufferA = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"A", 64, 4, EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy));
		const auto BufferB = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"B", 128, 4, EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy));
		const auto TextureA = MakeGraphTexture("A", 2);
		const auto TextureB = MakeGraphTexture("B");
		std::vector<FRDGBarrierBatch> Plan;
		{
			FRDGBuilder Builder;
			Builder.EnablePassCulling();
			const auto A = CreateTestBuffer(Builder, "A", BufferA, ERHIAccess::TransferRead);
			const auto TA = CreateTestTexture(Builder, "TA", TextureA, ERHIAccess::TransferRead);
			const auto Unused = CreateTestBuffer(Builder, "Unused", BufferA);
			const auto B = CreateTestBuffer(Builder, "B", BufferB, ERHIAccess::TransferRead);
			const auto TB = CreateTestTexture(Builder, "TB", TextureB, ERHIAccess::TransferRead);
			const auto Culled = FRDGBuilderTestAccessor::AddPass(Builder, "Culled", ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Culled, Unused, 0, 64,
				ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Write, B, 0, 128,
				ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			FRDGBuilderTestAccessor::UseTexture(Builder, Write, TB, WholeColor(),
				ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Write, A, 0, 64,
				ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			FRDGBuilderTestAccessor::UseTexture(Builder, Write, TA, WholeColor(2),
				ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Copy);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Read, B, 32, 32,
				ERDGUse::Read, ERHIAccess::TransferRead);
			FRDGBuilderTestAccessor::UseTexture(Builder, Read, TA,
				{ERHITextureAspect::Color, 1, 1, 0, 1}, ERDGUse::Read, ERHIAccess::TransferRead);
			Builder.MarkPassRoot(Write, "barrier fixture");
			Builder.MarkPassRoot(Read, "barrier fixture");
			const auto Empty = FRDGBuilderTestAccessor::AddPass(Builder, "Empty", ERDGPassType::Copy);
			Builder.MarkPassRoot(Empty, "empty batch fixture");
			FTestRDGAllocator Allocator;
			Allocator.BufferOverrides = {{0, BufferA}, {3, BufferB}};
			Allocator.TextureOverrides = {{1, TextureA}, {4, TextureB}};
			Allocator.OnAllocate = [&] {
				for (const auto& Pass : Builder.GetPasses()) Plan.push_back(Pass.Barriers);
				Plan.push_back(Builder.GetFinalBarriers());
			};
			FRDGExecutionContext Context{Allocator};
			const auto Result = Builder.Execute(Commands, &Context);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			EXPECT_EQ(Allocator.AllocationCount, 4u);
			ASSERT_EQ(Builder.GetPasses().size(), 3u);
			ASSERT_EQ(Plan.size(), 4u);
			for (size_t Index = 0; Index < Plan.size(); ++Index)
			{
				const auto& After = Index < Builder.GetPasses().size()
					? Builder.GetPasses()[Index].Barriers : Builder.GetFinalBarriers();
				EXPECT_TRUE(std::ranges::equal(Plan[Index].GetBufferTransitions(), After.GetBufferTransitions()));
				EXPECT_TRUE(std::ranges::equal(Plan[Index].GetTextureTransitions(), After.GetTextureTransitions()));
			}
			ExpectCapturedBarriersMatchPlan(Builder);
		}
		// Replay only after the builder and its reusable RHI batch scratch have died.
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		ASSERT_EQ(Backend.BufferBatches.size(), 3u);
		ASSERT_EQ(Backend.TextureBatches.size(), 3u);
		size_t BufferBatch = 0;
		size_t TextureBatch = 0;
		for (const auto& Batch : Plan)
		{
			if (!Batch.GetBufferTransitions().empty())
			{
				const auto& Recorded = Backend.BufferBatches[BufferBatch++];
				ASSERT_EQ(Recorded.size(), Batch.GetBufferTransitions().size());
				for (size_t Index = 0; Index < Recorded.size(); ++Index)
				{
					const auto& Logical = Batch.GetBufferTransitions()[Index];
					ASSERT_TRUE(Logical.ResourceId == 0 || Logical.ResourceId == 3);
					EXPECT_EQ(Recorded[Index], (FRHIBufferTransition{
						Logical.ResourceId == 0 ? BufferA.GetReference() : BufferB.GetReference(),
						Logical.Offset, Logical.Size, Logical.ExpectedBefore,
						Logical.RequiredAfter, Logical.bDiscardContents}));
				}
			}
			if (!Batch.GetTextureTransitions().empty())
			{
				const auto& Recorded = Backend.TextureBatches[TextureBatch++];
				ASSERT_EQ(Recorded.size(), Batch.GetTextureTransitions().size());
				for (size_t Index = 0; Index < Recorded.size(); ++Index)
				{
					const auto& Logical = Batch.GetTextureTransitions()[Index];
					ASSERT_TRUE(Logical.ResourceId == 1 || Logical.ResourceId == 4);
					EXPECT_EQ(Recorded[Index], (FRHITextureTransition{
						Logical.ResourceId == 1 ? TextureA.GetReference() : TextureB.GetReference(),
						Logical.Range, Logical.ExpectedBefore,
						Logical.RequiredAfter, Logical.bDiscardContents}));
				}
			}
		}
	}

	TEST_F(FRDGTests, CompilesStableHazardOrderAndExactTextureTransitions)
	{
		auto Texture = MakeGraphTexture("SceneColor");
		FRDGBuilder Builder;
		const auto SceneColor = CreateTestTexture(Builder, "SceneColor", Texture, ERHIAccess::GraphicsShaderRead);
		const auto Independent = FRDGBuilderTestAccessor::AddPass(Builder,
			"Independent", ERDGPassType::Copy);
		const auto Produce = FRDGBuilderTestAccessor::AddPass(Builder,
			"Produce", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseColorAttachment(Builder, Produce, SceneColor, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		const auto Consume = FRDGBuilderTestAccessor::AddPass(Builder,
			"Consume", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Consume, SceneColor, WholeColor(),
			ERDGUse::Read, ERHIAccess::ComputeShaderRead);

		FTestRDGAllocator Allocator;
		Allocator.TextureOverrides.emplace(0, Texture);
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ExpectCapturedBarriersMatchPlan(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetPasses().size(), 3u);
		EXPECT_EQ(Builder.GetPasses()[0].Name, "Independent");
		EXPECT_EQ(Builder.GetPasses()[1].Name, "Produce");
		EXPECT_EQ(Builder.GetPasses()[2].Name, "Consume");
		ASSERT_EQ(Builder.GetDependencies().size(), 1u);
		EXPECT_EQ(Builder.GetDependencies()[0],
			(FRDGDependency{1, 2, "SceneColor",
				ERDGDependencyKind::Value}));
		ASSERT_EQ(Builder.GetPasses()[1].Barriers.GetTextureTransitions().size(), 1u);
		EXPECT_EQ(Builder.GetPasses()[1].Barriers.GetTextureTransitions()[0], (FRDGTextureTransition{0, WholeColor(), ERHIAccess::Discard, ERHIAccess::ColorAttachmentReadWrite, true}));
		ASSERT_EQ(Builder.GetPasses()[2].Barriers.GetTextureTransitions().size(), 1u);
		EXPECT_EQ(Builder.GetPasses()[2].Barriers.GetTextureTransitions()[0], (FRDGTextureTransition{0, WholeColor(), ERHIAccess::ColorAttachmentReadWrite, ERHIAccess::ComputeShaderRead}));
		ASSERT_EQ(Builder.GetFinalBarriers().GetTextureTransitions().size(), 1u);
		EXPECT_EQ(Builder.GetFinalBarriers().GetTextureTransitions()[0], (FRDGTextureTransition{0, WholeColor(), ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead}));
	}

	TEST_F(FRDGTests, CompilesBufferRawWarAndWawDependencies)
	{
		auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"Work", 64, 4, EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy
		));
		FRDGBuilder Builder;
		const auto Work = CreateTestBuffer(Builder, "Work", Buffer);
		const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Write, Work, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Copy);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Read, Work, 0, 64, ERDGUse::Read,
			ERHIAccess::TransferRead);
		const auto Rewrite = FRDGBuilderTestAccessor::AddPass(Builder, "Rewrite", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Rewrite, Work, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);

		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetDependencies().size(), 3u);
		EXPECT_EQ(Builder.GetDependencies()[0].Kind,
			ERDGDependencyKind::Value);
		EXPECT_EQ(Builder.GetDependencies()[1].Kind,
			ERDGDependencyKind::Value);
		EXPECT_EQ(Builder.GetDependencies()[2].Kind,
			ERDGDependencyKind::Execution);
		EXPECT_EQ(Builder.GetPasses()[0].Barriers.GetBufferTransitions()[0].ExpectedBefore,
			ERHIAccess::Discard);
		EXPECT_EQ(Builder.GetPasses()[1].Barriers.GetBufferTransitions()[0].ExpectedBefore,
			ERHIAccess::ComputeShaderReadWrite);
		EXPECT_EQ(Builder.GetPasses()[2].Barriers.GetBufferTransitions()[0].ExpectedBefore,
			ERHIAccess::TransferRead);
		EXPECT_TRUE(Builder.GetPasses()[2].Barriers.GetBufferTransitions()[0].bDiscardContents);
	}

	TEST_F(FRDGTests, SameStateWritesSynchronizeWholeBuffersAndExactTextureSubresources)
	{
		for (const ERDGUse NextUse : {ERDGUse::Write, ERDGUse::ReadWrite})
		{
			SCOPED_TRACE(static_cast<uint32>(NextUse));
			FRDGBuilder Builder;
			const auto Buffer = Builder.CreateBuffer({.Buffer =
				FRHIBufferCreateDesc::Create("Work", 64, 4,
					EBufferUsageFlags::UnorderedAccess)}, "Work");
			const auto Texture = CreateTestTexture(Builder, "Image",
				MakeGraphTexture("Image", 2));
			const FRHITextureSubresourceRange Mip{ERHITextureAspect::Color, 1, 1, 0, 1};
			const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Write, Buffer, 0, 64, ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
			FRDGBuilderTestAccessor::UseTexture(Builder, Write, Texture, WholeColor(2), ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
			const auto Consume = FRDGBuilderTestAccessor::AddPass(Builder, "Consume", ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Consume, Buffer, 16, 16, NextUse,
				ERHIAccess::ComputeShaderReadWrite);
			FRDGBuilderTestAccessor::UseTexture(Builder, Consume, Texture, Mip, NextUse,
				ERHIAccess::ComputeShaderReadWrite);
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			ExpectCapturedBarriersMatchPlan(Builder);
			const auto& Pass = Builder.GetPasses()[1];
			ASSERT_EQ(Pass.Barriers.GetBufferTransitions().size(), 1u);
			EXPECT_EQ(Pass.Barriers.GetBufferTransitions()[0], (FRDGBufferTransition{
				0, 0, 64, ERHIAccess::ComputeShaderReadWrite,
				ERHIAccess::ComputeShaderReadWrite}));
			ASSERT_EQ(Pass.Barriers.GetTextureTransitions().size(), 1u);
			EXPECT_EQ(Pass.Barriers.GetTextureTransitions()[0], (FRDGTextureTransition{
				1, Mip, ERHIAccess::ComputeShaderReadWrite,
				ERHIAccess::ComputeShaderReadWrite}));
			const auto Capture = Builder.Capture();
			EXPECT_EQ(Capture.Statistics.BufferTransitions, 2u);
			EXPECT_EQ(Capture.Statistics.TextureTransitions, 2u);
			EXPECT_EQ(Capture.Statistics.TextureTransitionSubresources, 3u);
		}
	}

	TEST_F(FRDGTests, SameStateReadsDoNotAddBufferOrTextureBarriers)
	{
		FRDGBuilder Builder;
		auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"Read", 64, 4, EBufferUsageFlags::UnorderedAccess));
		const auto Input = Builder.RegisterExternalBuffer(Buffer, "Read",
			ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		const auto Texture = Builder.RegisterExternalTexture(MakeGraphTexture("Read"),
			"ReadTexture", ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		for (const char* Name : {"ReadA", "ReadB"})
		{
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, Name, ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Input, 0, 64, ERDGUse::Read,
				ERHIAccess::ComputeShaderRead);
			FRDGBuilderTestAccessor::UseTexture(Builder, Pass, Texture, WholeColor(), ERDGUse::Read,
				ERHIAccess::ComputeShaderRead);
		}
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		for (const auto& Pass : Builder.GetPasses())
		{
			EXPECT_TRUE(Pass.Barriers.GetBufferTransitions().empty());
			EXPECT_TRUE(Pass.Barriers.GetTextureTransitions().empty());
		}
		EXPECT_TRUE(Builder.Capture().Transitions.empty());
	}

	TEST_F(FRDGTests, ManagedReadCaptureDoesNotInventEntryBarrier)
	{
		FRDGBuilder Builder;
		const auto Texture = Builder.RegisterExternalTexture(MakeGraphTexture("ManagedRead"),
			"ManagedRead", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "ManagedRead", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseManagedTexture(Builder, Pass, Texture, WholeColor(),
			ERDGUse::Read, ERHIAccess::GraphicsShaderRead, ERHIAccess::TransferRead);
		// The internal transition must not consume the one-barrier budget needed at exit.
		Builder.SetBudget({.MaxTextureTransitions = 1});
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_FALSE(FRDGBuilderTestAccessor::HasDiagnostics(Builder));
		EXPECT_TRUE(Builder.GetPasses()[0].Barriers.GetTextureTransitions().empty());
		ASSERT_EQ(Builder.GetFinalBarriers().GetTextureTransitions().size(), 1u);
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Transitions.size(), 2u);
		EXPECT_EQ(Capture.Transitions[0].Kind, ERDGTransitionKind::PassManaged);
		EXPECT_EQ(Capture.Transitions[0].Before, ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Capture.Transitions[0].After, ERHIAccess::TransferRead);
		EXPECT_EQ(Capture.Transitions[1].Kind, ERDGTransitionKind::RHIBarrier);
		EXPECT_TRUE(Capture.Transitions[1].bFinal);
		EXPECT_EQ(Capture.Transitions[1].Before, ERHIAccess::TransferRead);
		EXPECT_EQ(Capture.Statistics.TextureTransitions, 1u);
		ExpectCapturedBarriersMatchPlan(Builder);
		EXPECT_EQ(Builder.Capture().Transitions.size(), Capture.Transitions.size());
	}

	TEST_F(FRDGTests, ManagedAttachmentLoadSynchronizesSameStateWrites)
	{
		FRDGBuilder Builder;
		const auto Texture = CreateTestTexture(Builder, "Color", MakeGraphTexture("Color"));
		const auto Clear = FRDGBuilderTestAccessor::AddPass(Builder, "Clear", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseManagedColorAttachment(Builder, Clear, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store,
			ERHIAccess::ColorAttachmentReadWrite);
		const auto Load = FRDGBuilderTestAccessor::AddPass(Builder, "Load", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseManagedColorAttachment(Builder, Load, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Load, ERHIRenderTargetStoreAction::Store,
			ERHIAccess::ColorAttachmentReadWrite);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetPasses()[0].Barriers.GetTextureTransitions().size(), 1u);
		EXPECT_TRUE(Builder.GetPasses()[0].Barriers.GetTextureTransitions()[0].bDiscardContents);
		const auto& Transitions = Builder.GetPasses()[1].Barriers.GetTextureTransitions();
		ASSERT_EQ(Transitions.size(), 1u);
		EXPECT_EQ(Transitions[0], (FRDGTextureTransition{0, WholeColor(),
			ERHIAccess::ColorAttachmentReadWrite, ERHIAccess::ColorAttachmentReadWrite}));
	}

	TEST_F(FRDGTests, RejectsMissingProducerForeignHandleAndInvalidOrder)
	{
		auto Texture = MakeGraphTexture("Missing");
		FRDGBuilder MissingProducer;
		const auto Logical = CreateTestTexture(MissingProducer, "Missing", Texture);
		const auto Read = FRDGBuilderTestAccessor::AddPass(MissingProducer, "Read", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseTexture(MissingProducer, Read, Logical, WholeColor(),
			ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
		auto Missing = FRDGBuilderTestAccessor::Compile(MissingProducer);
		EXPECT_FALSE(Missing.IsSuccess());
		EXPECT_NE(Missing.Result.Message.find("before its producer"), std::string::npos);
		EXPECT_EQ(Missing.Result.Error, ERDGError::MissingProducer);

		FRDGBuilder ForeignOwner;
		const auto Foreign = CreateTestTexture(ForeignOwner, "Foreign", Texture);
		FRDGBuilder ForeignUse;
		const auto Pass = FRDGBuilderTestAccessor::AddPass(ForeignUse, "Use", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseTexture(ForeignUse, Pass, Foreign, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Invalid = FRDGBuilderTestAccessor::Compile(ForeignUse);
		EXPECT_FALSE(Invalid.IsSuccess());
		EXPECT_NE(Invalid.Result.Message.find("invalid resource handle"), std::string::npos);
		EXPECT_EQ(Invalid.Result.Error, ERDGError::InvalidDeclaration);

		FRDGBuilder Cyclic;
		const auto A = FRDGBuilderTestAccessor::AddPass(Cyclic, "A", ERDGPassType::Compute);
		const auto B = FRDGBuilderTestAccessor::AddPass(Cyclic, "B", ERDGPassType::Compute);
		Cyclic.AddPassDependency(A, B);
		Cyclic.AddPassDependency(B, A);
		auto Cycle = FRDGBuilderTestAccessor::Compile(Cyclic);
		EXPECT_FALSE(Cycle.IsSuccess());
		EXPECT_EQ(Cycle.Result.Message, "dependency must point forward: producer[1] consumer[0]");
		EXPECT_EQ(Cycle.Result.Error, ERDGError::InvalidDependency);

		FRDGBuilder SelfDependent;
		const auto Self = FRDGBuilderTestAccessor::AddPass(SelfDependent,
			"Self", ERDGPassType::Compute);
		SelfDependent.AddPassDependency(Self, Self);
		auto SelfCycle = FRDGBuilderTestAccessor::Compile(SelfDependent);
		EXPECT_FALSE(SelfCycle.IsSuccess());
		EXPECT_EQ(SelfCycle.Result.Message, "dependency must point forward: producer[0] consumer[0]");
	}

	TEST_F(FRDGTests, ResourceUseSlicesPreserveInterleavedUsesAndEmptyResources)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		Builder.CreateToken("UnusedFirst");
		const auto A = Builder.CreateToken("A");
		Builder.CreateToken("UnusedMiddle");
		const auto B = Builder.CreateToken("B");
		Builder.CreateToken("UnusedLast");
		const auto First = FRDGBuilderTestAccessor::AddPass(Builder, "First", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseToken(Builder, First, B, ERDGUse::Write);
		FRDGBuilderTestAccessor::UseToken(Builder, First, A, ERDGUse::Write);
		FRDGBuilderTestAccessor::AddPass(Builder, "Empty", ERDGPassType::Compute);
		const auto Last = FRDGBuilderTestAccessor::AddPass(Builder, "Last", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseToken(Builder, Last, A, ERDGUse::Read);
		FRDGBuilderTestAccessor::UseToken(Builder, Last, B, ERDGUse::Read);
		Builder.MarkPassRoot(Last);
		ASSERT_TRUE(FRDGBuilderTestAccessor::Compile(Builder).IsSuccess());
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Passes.size(), 2u);
		EXPECT_EQ(Capture.Passes[0].DeclarationIndex, 0u);
		EXPECT_EQ(Capture.Passes[1].DeclarationIndex, 2u);
		ASSERT_EQ(Capture.Dependencies.size(), 1u);
		EXPECT_EQ(Capture.Dependencies[0].Cause, "A");
		ASSERT_EQ(Capture.ResourceLifetimes.size(), 5u);
		for (uint32 Index = 0; Index < 5; ++Index)
			EXPECT_EQ(Capture.ResourceLifetimes[Index].bCulled, Index % 2 == 0);
		ASSERT_EQ(Capture.Uses.size(), 4u);
		const uint32 ExpectedResources[] = {3, 1, 1, 3};
		for (size_t Index = 0; Index < Capture.Uses.size(); ++Index)
		{
			EXPECT_EQ(Capture.Uses[Index].ResourceId, ExpectedResources[Index]);
			EXPECT_EQ(Capture.Uses[Index].Version, 1u);
		}
		EXPECT_EQ(Builder.Capture().Dump, Capture.Dump);
	}

	TEST_F(FRDGTests, DeclarationOrderRetainsSharedAncestorsAndFinalizedValueEdges)
	{
		for (bool bCull : {false, true})
		{
			FRDGBuilder Builder;
			if (bCull) Builder.EnablePassCulling();
			const auto Texture = CreateTestTexture(Builder, "Texture", MakeGraphTexture("Texture"));
			const auto Token = Builder.CreateToken("Value");
			const auto First = FRDGBuilderTestAccessor::AddPass(Builder, "First", ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseTexture(Builder, First, Texture, WholeColor(), ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
			FRDGBuilderTestAccessor::UseToken(Builder, First, Token, ERDGUse::Write);
			FRDGBuilderTestAccessor::AddPass(Builder, "Unused", ERDGPassType::Compute);
			const auto Second = FRDGBuilderTestAccessor::AddPass(Builder, "Second", ERDGPassType::Compute);
			// The texture inserts an Execution edge; the token then upgrades it to Value.
			FRDGBuilderTestAccessor::UseTexture(Builder, Second, Texture, WholeColor(), ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
			FRDGBuilderTestAccessor::UseToken(Builder, Second, Token, ERDGUse::Read);
			const auto Left = FRDGBuilderTestAccessor::AddPass(Builder, "Left", ERDGPassType::Compute);
			const auto Right = FRDGBuilderTestAccessor::AddPass(Builder, "Right", ERDGPassType::Compute);
			Builder.AddPassDependency(Left, Right);
			Builder.AddPassDependency(Second, Left);
			Builder.AddPassDependency(Second, Right);
			Builder.MarkPassRoot(Left);
			Builder.MarkPassRoot(Right);
			ASSERT_TRUE(FRDGBuilderTestAccessor::Compile(Builder).IsSuccess());
			ASSERT_EQ(Builder.GetPasses().size(), bCull ? 4u : 5u);
			uint32 Position = 0;
			for (uint32 Index = 0; Index < 5; ++Index)
				if (!bCull || Index != 1)
					EXPECT_EQ(Builder.GetPasses()[Position++].DeclarationIndex, Index);
			const auto Edges = Builder.GetDependencies();
			ASSERT_EQ(Edges.size(), 4u);
			const auto Upgraded = std::ranges::find_if(Edges, [](const FRDGDependency& Edge) {
				return Edge.BeforePass == 0 && Edge.AfterPass == 2;
			});
			ASSERT_NE(Upgraded, Edges.end());
			EXPECT_EQ(Upgraded->Kind, ERDGDependencyKind::Value);
			EXPECT_EQ(Upgraded->Cause, "Value");
		}
	}

	TEST_F(FRDGTests, ReadWriteRetentionPreservesItsInputValue)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferDesc(
			64, 4, EBufferUsageFlags::UnorderedAccess)}, "Buffer");
		const auto Producer = FRDGBuilderTestAccessor::AddPass(Builder, "Producer", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Producer, Buffer, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Update = FRDGBuilderTestAccessor::AddPass(Builder, "Update", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Update, Buffer, 0, 64, ERDGUse::ReadWrite,
			ERHIAccess::ComputeShaderReadWrite);
		Builder.MarkPassRoot(Update);
		ASSERT_TRUE(FRDGBuilderTestAccessor::Compile(Builder).IsSuccess());
		ASSERT_EQ(Builder.GetPasses().size(), 2u);
		ASSERT_EQ(Builder.GetDependencies().size(), 1u);
		EXPECT_EQ(Builder.GetDependencies()[0].Kind, ERDGDependencyKind::Value);
	}

	TEST_F(FRDGTests, ExplicitDependencyCannotRepairReadBeforeProducer)
	{
		FRDGBuilder Builder;
		const auto Token = Builder.CreateToken("Token");
		const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Compute);
		const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseToken(Builder, Read, Token, ERDGUse::Read);
		FRDGBuilderTestAccessor::UseToken(Builder, Write, Token, ERDGUse::Write);
		Builder.AddPassDependency(Read, Write);
		EXPECT_NE(FRDGBuilderTestAccessor::Compile(Builder).Result.Message.find(
			"before its producer"), std::string::npos);
		EXPECT_TRUE(Builder.GetPasses().empty());
	}

	TEST_F(FRDGTests, DeclarationOrderScalesAcrossIndependentPassesAndSparseChains)
	{
		for (bool bCull : {false, true})
			for (bool bChain : {false, true})
				for (uint32 Count : {0u, 128u, 1024u, 8192u})
					for (bool bRoot : {false, true})
					{
						FRDGBuilder Builder;
						if (bCull) Builder.EnablePassCulling();
						FRDGPassHandle Previous;
						for (uint32 Index = 0; Index < Count; ++Index)
						{
							const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Pass" + std::to_string(Index), ERDGPassType::Compute);
							if (bChain && Index != 0) Builder.AddPassDependency(Previous, Pass);
							Previous = Pass;
						}
						if (bRoot && Count != 0) Builder.MarkPassRoot(Previous);
						const auto Start = std::chrono::steady_clock::now();
						const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
						const double Milliseconds = std::chrono::duration<double, std::milli>(
							std::chrono::steady_clock::now() - Start).count();
						ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
						const uint32 Expected = !bCull ? Count : !bRoot || Count == 0 ? 0 : bChain ? Count : 1;
						ASSERT_EQ(Builder.GetPasses().size(), Expected);
						for (uint32 Index = 0; Index < Expected; ++Index)
							EXPECT_EQ(Builder.GetPasses()[Index].DeclarationIndex,
								bCull && !bChain ? Count - 1 : Index);
						std::printf("RDG scale passes=%u chain=%d cull=%d root=%d compile-ms=%.3f\n",
							Count, bChain, bCull, bRoot, Milliseconds);
					}
	}

	TEST_F(FRDGTests, ValidatesDependencyHandlesBeforeCulling)
	{
		for (bool bInvalidConsumer : {false, true})
			for (bool bForeign : {false, true})
			{
				FRDGBuilder Owner;
				const auto Foreign = FRDGBuilderTestAccessor::AddPass(Owner, "Foreign", ERDGPassType::Compute);
				FRDGBuilder Builder;
				Builder.EnablePassCulling();
				const auto Local = FRDGBuilderTestAccessor::AddPass(Builder, "Local", ERDGPassType::Compute);
				const auto Invalid = bForeign ? Foreign : FRDGPassHandle{};
				Builder.AddPassDependency(bInvalidConsumer ? Local : Invalid,
					bInvalidConsumer ? Invalid : Local);
				EXPECT_EQ(FRDGBuilderTestAccessor::Compile(Builder).Result.Message,
					bInvalidConsumer ? "dependency has an invalid consumer pass handle"
					: "pass 'Local' has an invalid producer pass handle");
				EXPECT_TRUE(Builder.GetPasses().empty());
			}
		for (bool bSelf : {false, true})
		{
			FRDGBuilder Builder;
			Builder.EnablePassCulling();
			const auto First = FRDGBuilderTestAccessor::AddPass(Builder, "First", ERDGPassType::Compute);
			const auto Second = FRDGBuilderTestAccessor::AddPass(Builder, "Second", ERDGPassType::Compute);
			Builder.AddPassDependency(bSelf ? First : Second, First);
			EXPECT_EQ(FRDGBuilderTestAccessor::Compile(Builder).Result.Message,
				bSelf ? "dependency must point forward: producer[0] consumer[0]"
				: "dependency must point forward: producer[1] consumer[0]");
			EXPECT_TRUE(Builder.GetPasses().empty());
		}
	}

	TEST_F(FRDGTests, RejectsTextureAspectsOutsideResourceFormat)
	{
		auto Texture = MakeGraphTexture("ColorOnly");
		FRDGBuilder Builder;
		const auto Resource = CreateTestTexture(Builder, "ColorOnly", Texture);
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "InvalidAspects",
			ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Pass, Resource,
			{ERHITextureAspect::Color | ERHITextureAspect::Depth, 0, 1, 0, 1},
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);

		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Result.Message.find("invalid texture range"), std::string::npos);
	}

	TEST_F(FRDGTests, NormalizesDisjointAndPartiallyOverlappingSubresources)
	{
		auto Texture = MakeGraphTexture("MipChain", 4);
		FRDGBuilder Builder;
		const auto Chain = CreateTestTexture(Builder, "MipChain", Texture);
		const auto Mip0 = FRDGBuilderTestAccessor::AddPass(Builder, "Mip0", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Mip0, Chain, {ERHITextureAspect::Color, 0, 1, 0, 1},
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto Mip1 = FRDGBuilderTestAccessor::AddPass(Builder, "Mip1", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Mip1, Chain, {ERHITextureAspect::Color, 1, 1, 0, 1},
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		auto Disjoint = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Disjoint.IsSuccess()) << Disjoint.Result.Message;
		EXPECT_TRUE(Builder.GetDependencies().empty());

		FRDGBuilder Partial;
		const auto PartialChain = CreateTestTexture(Partial, "MipChain", Texture);
		const auto Whole = FRDGBuilderTestAccessor::AddPass(Partial, "Whole", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Partial, Whole, PartialChain, WholeColor(4),
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto OneMip = FRDGBuilderTestAccessor::AddPass(Partial, "OneMip", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Partial, OneMip, PartialChain,
			{ERHITextureAspect::Color, 1, 1, 0, 1}, ERDGUse::Read,
			ERHIAccess::ComputeShaderRead);
		auto Overlap = FRDGBuilderTestAccessor::Compile(Partial);
		ASSERT_TRUE(Overlap.IsSuccess()) << Overlap.Result.Message;
		ASSERT_EQ(Partial.GetDependencies().size(), 1u);
		EXPECT_EQ(Partial.GetDependencies()[0].Kind,
			ERDGDependencyKind::Value);
		EXPECT_EQ(Partial.GetPasses()[0].Barriers.GetTextureTransitions().size(), 1u);
		EXPECT_EQ(Partial.GetPasses()[1].Barriers.GetTextureTransitions().size(), 1u);
	}

	TEST_F(FRDGTests, CompactsTextureLayersAndMipsWithoutChangingCapturedSubresources)
	{
		for (const bool bAsync : {false, true})
		{
			FRDGBuilder Builder;
			Builder.SetAsyncComputeEnabled(bAsync);
			Builder.SetBudget({.RegressionMaxTextureTransitions = 2});
			auto Desc = DescribeGraphTexture(*MakeGraphTexture("Array", 5));
			Desc.Texture.ArraySize = 6;
			const auto Texture = Builder.CreateTexture(Desc, "Array");
			const FRHITextureSubresourceRange Whole{ERHITextureAspect::Color, 0, 5, 0, 6};
			const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
			Builder.SetPassAsyncComputeEligible(Write);
			FRDGBuilderTestAccessor::UseTexture(Builder, Write, Texture, Whole,
				ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Graphics);
			FRDGBuilderTestAccessor::UseTexture(Builder, Read, Texture, Whole,
				ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
			const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			EXPECT_EQ(Builder.GetStatistics().TextureTransitions, 2u);
			EXPECT_EQ(Builder.GetStatistics().TextureTransitionSubresources, 60u);
			EXPECT_FALSE(Builder.GetStatistics().bTextureTransitionRegressionBudgetExceeded);
			for (const auto& Pass : Builder.GetPasses())
			{
				ASSERT_EQ(Pass.Barriers.GetTextureTransitions().size(), 1u);
				EXPECT_EQ(Pass.Barriers.GetTextureTransitions()[0].Range, Whole);
			}
			ExpectCapturedBarriersMatchPlan(Builder);
		}
	}

	TEST_F(FRDGTests, TextureCompactionPreservesProducerAndQueueBoundaries)
	{
		for (const bool bAsync : {false, true})
		{
			FRDGBuilder Builder;
			Builder.SetAsyncComputeEnabled(bAsync);
			auto Desc = DescribeGraphTexture(*MakeGraphTexture("Array", 2));
			Desc.Texture.ArraySize = 2;
			const auto Texture = Builder.CreateTexture(Desc, "Array");
			for (uint32 Layer = 0; Layer < 2; ++Layer)
			{
				const auto Write = FRDGBuilderTestAccessor::AddPass(Builder,
					"Write" + std::to_string(Layer), ERDGPassType::Compute);
				if (Layer == 1) Builder.SetPassAsyncComputeEligible(Write);
				FRDGBuilderTestAccessor::UseTexture(Builder, Write, Texture,
					{ERHITextureAspect::Color, 0, 2, Layer, 1},
					ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			}
			const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Graphics);
			FRDGBuilderTestAccessor::UseTexture(Builder, Read, Texture,
				{ERHITextureAspect::Color, 0, 2, 0, 2}, ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
			const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			const auto& Plan = Builder.GetExecutionPlan();
			for (const auto& Handoff : Plan.Handoffs)
			{
				if (Handoff.Consumer.Index != 2) continue;
				const auto& Range = Builder.GetPasses()[2].Barriers.GetTextureTransitions()[Handoff.TransitionIndex].Range;
				EXPECT_EQ(Range.NumArrayLayers, 1u);
				EXPECT_EQ(Handoff.Producers, (std::vector<FRDGSubmissionId>{{Range.FirstArrayLayer}}));
				EXPECT_EQ(Handoff.SourceQueue, bAsync && Range.FirstArrayLayer == 1
					? ERDGQueueAssignment::AsyncCompute : ERDGQueueAssignment::Graphics);
			}
			ExpectCapturedBarriersMatchPlan(Builder);
		}
	}

	TEST_F(FRDGTests, DiscardedAttachmentStoreCannotBecomeAProducer)
	{
		auto Texture = MakeGraphTexture("Discarded");
		FRDGBuilder Builder;
		const auto Target = CreateTestTexture(Builder, "Discarded", Texture);
		const auto Clear = FRDGBuilderTestAccessor::AddPass(Builder, "Clear", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseColorAttachment(Builder, Clear, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::DontCare);
		const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseTexture(Builder, Read, Target, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Result.Message.find("before its producer"), std::string::npos);
	}

	TEST_F(FRDGTests, PreservesExternalInitialAndFinalStates)
	{
		auto Texture = MakeGraphTexture("Imported");
		FRDGBuilder Builder;
		const auto External = Builder.RegisterExternalTexture(Texture, "External", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Compute = FRDGBuilderTestAccessor::AddPass(Builder, "Compute", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Compute, External, WholeColor(), ERDGUse::Read, ERHIAccess::ComputeShaderRead);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetPasses()[0].Barriers.GetTextureTransitions().size(), 1u);
		EXPECT_EQ(Builder.GetPasses()[0].Barriers.GetTextureTransitions()[0].ExpectedBefore,
			ERHIAccess::GraphicsShaderRead);
		ASSERT_EQ(Builder.GetFinalBarriers().GetTextureTransitions().size(), 1u);
		EXPECT_EQ(Builder.GetFinalBarriers().GetTextureTransitions()[0].RequiredAfter,
			ERHIAccess::GraphicsShaderRead);
	}

	TEST_F(FRDGTests, RejectsAttachmentLoadWithoutPriorContents)
	{
		auto Texture = MakeGraphTexture("Load");
		FRDGBuilder Builder;
		const auto Target = CreateTestTexture(Builder, "Load", Texture);
		const auto Load = FRDGBuilderTestAccessor::AddPass(Builder, "Load", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseColorAttachment(Builder, Load, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Load,
			ERHIRenderTargetStoreAction::Store);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Result.Message.find("before its producer"), std::string::npos);
	}

	TEST_F(FRDGTests, DumpIsDeterministicAndSyntheticCompileCostIsBounded)
	{
		auto CompileFixture = [] {
			static const auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
				"Fixture", 512, 4, EBufferUsageFlags::UnorderedAccess
			));
			FRDGBuilder Builder;
			const auto Work = CreateTestBuffer(Builder, "Fixture", Buffer);
			for (uint32 Index = 0; Index < 128; ++Index)
			{
				const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Pass" + std::to_string(Index),
					ERDGPassType::Compute);
				FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Work, 0, 512, ERDGUse::Write,
					ERHIAccess::ComputeShaderReadWrite, Index == 0);
			}
			const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			return Builder.Capture();
		};
		auto First = CompileFixture();
		auto Second = CompileFixture();
		EXPECT_EQ(First.Dump, Second.Dump);
		EXPECT_LT(First.Statistics.CompileMicroseconds, 250000u);
		EXPECT_LT(Second.Statistics.CompileMicroseconds, 250000u);
		EXPECT_EQ(First.Dependencies.size(), 127u);
	}

	TEST_F(FRDGTests, SubmissionPlanCompactsCulledPassesAndSurvivesRecording)
	{
		FRDGBuilder Builder;
		FRDGExecutionPlan DuringRecording;
		Builder.EnablePassCulling();
		const auto Value = Builder.CreateToken("Output");
		FRDGBuilderTestAccessor::AddPass(Builder, "Culled", ERDGPassType::Graphics);
		const auto Producer = FRDGBuilderTestAccessor::AddPass(Builder, "Producer", ERDGPassType::Compute,
			[&](FRHICommandListImmediate&, const FRDGPassResources&) {
				DuringRecording = Builder.GetExecutionPlan();
			});
		FRDGBuilderTestAccessor::UseToken(Builder, Producer, Value, ERDGUse::Write);
		const auto Consumer = FRDGBuilderTestAccessor::AddPass(Builder, "Consumer", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseToken(Builder, Consumer, Value, ERDGUse::Read);
		Builder.MarkPassRoot(Consumer, "external-effect");
		ASSERT_TRUE(Builder.Execute(GetCommandList()).IsSuccess());
		const auto Before = Builder.Capture();
		ASSERT_EQ(Before.ExecutionPlan.Batches.size(), 3u);
		EXPECT_EQ(Before.Passes[0].DeclarationIndex, 1u);
		EXPECT_EQ(Before.ExecutionPlan.Batches[0].FirstPass, 0u);
		EXPECT_EQ(Before.ExecutionPlan.Batches[1].FirstPass, 1u);
		EXPECT_TRUE(Before.ExecutionPlan.Batches[2].bEpilogue);
		EXPECT_EQ(Before.ExecutionPlan.Batches[2].NumPasses, 0u);
		for (const auto& Batch : Before.ExecutionPlan.Batches)
			EXPECT_EQ(Batch.Queue, ERDGQueueAssignment::Graphics);
		for (const auto& Dependency : Before.ExecutionPlan.Dependencies)
		{
			EXPECT_LT(Dependency.Before.Index, Dependency.After.Index);
			EXPECT_FALSE(Dependency.Cause.empty());
		}
		EXPECT_EQ(DuringRecording, Before.ExecutionPlan);
		EXPECT_EQ(Before.ExecutionPlan, Builder.GetExecutionPlan());
		EXPECT_EQ(Before.ExecutionPlan, Builder.Capture().ExecutionPlan);
	}

	TEST_F(FRDGTests, AsyncPolicySeparatesEligibilityFromQueueOrderAndJoinsTerminalPrefixes)
	{
		for (bool bEnabled : {false, true})
		{
			FRDGExecutionPlan Previous;
			for (int Run = 0; Run < 2; ++Run)
			{
				FRDGBuilder Builder;
				Builder.EnablePassCulling();
				Builder.SetAsyncComputeEnabled(bEnabled);
				const auto Dead = FRDGBuilderTestAccessor::AddPass(Builder, "Dead", ERDGPassType::Compute);
				Builder.SetPassAsyncComputeEligible(Dead);
				const auto G0 = FRDGBuilderTestAccessor::AddPass(Builder, "G0", ERDGPassType::Graphics);
				const auto C0 = FRDGBuilderTestAccessor::AddPass(Builder, "C0", ERDGPassType::Compute);
				const auto G1 = FRDGBuilderTestAccessor::AddPass(Builder, "IneligibleCompute", ERDGPassType::Compute);
				const auto C1 = FRDGBuilderTestAccessor::AddPass(Builder, "C1", ERDGPassType::Compute);
				for (auto Pass : {G0, C0, G1, C1}) Builder.MarkPassRoot(Pass);
				Builder.SetPassAsyncComputeEligible(C0);
				Builder.SetPassAsyncComputeEligible(C1);
				Builder.AddPassDependency(G0, C0);
				ASSERT_TRUE(FRDGBuilderTestAccessor::Compile(Builder).IsSuccess());
				const auto& Plan = Builder.GetExecutionPlan();
				ASSERT_EQ(Plan.Batches.size(), 5u);
				EXPECT_EQ(Plan.Batches[0].Queue, ERDGQueueAssignment::Graphics);
				EXPECT_EQ(Plan.Batches[2].Queue, ERDGQueueAssignment::Graphics);
				for (size_t Index : {1u, 3u})
					EXPECT_EQ(Plan.Batches[Index].Queue, bEnabled ? ERDGQueueAssignment::AsyncCompute : ERDGQueueAssignment::Graphics);
				auto HasEdge = [&](uint32 Before, uint32 After) {
					return std::ranges::any_of(Plan.Dependencies, [&](const auto& Edge) {
						return Edge.Before.Index == Before && Edge.After.Index == After;
					});
				};
				EXPECT_TRUE(HasEdge(0, 1));
				EXPECT_TRUE(HasEdge(3, 4));
				if (bEnabled)
				{
					EXPECT_TRUE(HasEdge(0, 2));
					EXPECT_TRUE(HasEdge(1, 3));
					EXPECT_TRUE(HasEdge(2, 4));
					EXPECT_FALSE(HasEdge(1, 2));
					EXPECT_FALSE(HasEdge(2, 3));
				}
				if (Run != 0) EXPECT_EQ(Plan, Previous);
				Previous = Plan;
			}
		}
	}

	TEST_F(FRDGTests, ResourceHandoffRetainsLatestReaderOnEveryLogicalQueue)
	{
		FRDGBuilder Builder;
		Builder.SetAsyncComputeEnabled(true);
		const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferDesc(64, 4,
			EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource)}, "Shared");
		for (uint32 Index = 0; Index < 5; ++Index)
		{
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, std::to_string(Index), ERDGPassType::Compute);
			if (Index == 2) Builder.SetPassAsyncComputeEligible(Pass);
			const bool bWrite = Index == 0 || Index == 4;
			FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, 64,
				bWrite ? ERDGUse::Write : ERDGUse::Read,
				bWrite ? ERHIAccess::ComputeShaderReadWrite : ERHIAccess::ComputeShaderRead, bWrite);
		}
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		const auto& Plan = Builder.GetExecutionPlan();
		const auto Handoff = std::ranges::find_if(Plan.Handoffs, [](const auto& Item) { return Item.Consumer.Index == 4; });
		ASSERT_NE(Handoff, Plan.Handoffs.end());
		EXPECT_EQ(Handoff->Producers, (std::vector<FRDGSubmissionId>{{3}, {2}}));
		EXPECT_TRUE(std::ranges::any_of(Plan.Dependencies, [](const auto& Edge) {
			return Edge.Before.Index == 2 && Edge.After.Index == 4 && Edge.Cause == "resource-handoff";
		}));
		ExpectCapturedBarriersMatchPlan(Builder);
	}

	TEST_F(FRDGTests, EqualAccessQueueHandoffsIncludeInitialAndFinalGraphicsOwnership)
	{
		for (bool bEnabled : {false, true})
		{
			FRDGBuilder Builder;
			Builder.SetAsyncComputeEnabled(bEnabled);
			const auto Physical = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
				"Input", 64, 4, EBufferUsageFlags::ShaderResource));
			const auto Buffer = Builder.RegisterExternalBuffer(Physical, "Input",
				ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Compute);
			Builder.SetPassAsyncComputeEligible(Pass);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, 64,
				ERDGUse::Read, ERHIAccess::ComputeShaderRead);
			const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			const auto& Plan = Builder.GetExecutionPlan();
			ASSERT_EQ(Plan.Handoffs.size(), bEnabled ? 2u : 0u);
			if (bEnabled)
			{
				EXPECT_EQ(Plan.Handoffs[0].SourceQueue, ERDGQueueAssignment::Graphics);
				EXPECT_TRUE(Plan.Handoffs[0].Producers.empty());
				EXPECT_EQ(Plan.Handoffs[1].SourceQueue, ERDGQueueAssignment::AsyncCompute);
				EXPECT_EQ(Plan.Handoffs[1].Producers, (std::vector<FRDGSubmissionId>{{0}}));
				EXPECT_TRUE(Plan.Batches[Plan.Handoffs[1].Consumer.Index].bEpilogue);
				const auto Capture = Builder.Capture();
				ASSERT_EQ(Capture.Transitions.size(), 2u);
				for (const auto& Transition : Capture.Transitions)
				{
					EXPECT_EQ(Transition.Before, Transition.After);
					EXPECT_NE(Transition.SourceQueue, Transition.DestinationQueue);
				}
			}
			ExpectCapturedBarriersMatchPlan(Builder);
		}
	}

	TEST_F(FRDGTests, AsyncEligibilityRejectsForeignAndNonComputePasses)
	{
		for (bool bForeign : {false, true})
		{
			FRDGBuilder Builder, Other;
			const auto Pass = FRDGBuilderTestAccessor::AddPass(bForeign ? Other : Builder,
				"Invalid", bForeign ? ERDGPassType::Compute : ERDGPassType::Graphics);
			Builder.SetPassAsyncComputeEligible(Pass);
			EXPECT_FALSE(FRDGBuilderTestAccessor::Compile(Builder).IsSuccess());
		}
	}

	TEST_F(FRDGTests, EmptyGraphHasNoSyntheticSubmission)
	{
		FRDGBuilder Builder;
		ASSERT_TRUE(Builder.Execute(GetCommandList()).IsSuccess());
		EXPECT_TRUE(Builder.GetExecutionPlan().Batches.empty());
		EXPECT_TRUE(Builder.GetExecutionPlan().Dependencies.empty());
		EXPECT_TRUE(Builder.GetExecutionPlan().Handoffs.empty());
	}

	TEST_F(FRDGTests, CullsUnreachableBranchesAndReportsExactLifetimes)
	{
		auto RetainedBuffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"Retained", 64, 4, EBufferUsageFlags::UnorderedAccess
		));
		auto CulledBuffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"Culled", 64, 4, EBufferUsageFlags::UnorderedAccess
		));
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Retained = CreateTestBuffer(Builder, "Retained", RetainedBuffer);
		const auto Culled = CreateTestBuffer(Builder, "Culled", CulledBuffer);
		const auto Produce = FRDGBuilderTestAccessor::AddPass(Builder, "Produce", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Produce, Retained, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Consume = FRDGBuilderTestAccessor::AddPass(Builder, "Present", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Consume, Retained, 0, 64, ERDGUse::Read,
			ERHIAccess::ComputeShaderRead);
		Builder.MarkPassRoot(Consume, "present");
		const auto Unused = FRDGBuilderTestAccessor::AddPass(Builder, "Unused", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Unused, Culled, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);

		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetPasses().size(), 2u);
		EXPECT_EQ(Builder.GetPasses()[0].Name, "Produce");
		EXPECT_EQ(Builder.GetPasses()[1].Name, "Present");
		ASSERT_EQ(Builder.GetResourceLifetimes().size(), 2u);
		EXPECT_EQ(Builder.GetResourceLifetimes()[0].FirstPass, 0u);
		EXPECT_EQ(Builder.GetResourceLifetimes()[0].LastPass, 1u);
		EXPECT_FALSE(Builder.GetResourceLifetimes()[0].bCulled);
		EXPECT_TRUE(Builder.GetResourceLifetimes()[1].bCulled);
		ASSERT_EQ(Builder.GetCullingDecisions().size(), 3u);
		EXPECT_FALSE(Builder.GetCullingDecisions()[0].bCulled);
		EXPECT_EQ(Builder.GetCullingDecisions()[0].Reason, "value dependency");
		EXPECT_EQ(Builder.GetCullingDecisions()[1].Reason, "present");
		EXPECT_TRUE(Builder.GetCullingDecisions()[2].bCulled);
	}

	TEST_F(FRDGTests, CanonicalizesEquivalentExternalIdentity)
	{
		auto Texture = MakeGraphTexture("Shared");
		auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"SharedBuffer", 64, 4, EBufferUsageFlags::UnorderedAccess
		));
		FRDGBuilder Builder;
		const auto FirstTexture = Builder.RegisterExternalTexture(Texture, "First", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto SecondTexture = Builder.RegisterExternalTexture(Texture, "Second", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto FirstBuffer = Builder.RegisterExternalBuffer(Buffer, "FirstBuffer", ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		const auto SecondBuffer = Builder.RegisterExternalBuffer(Buffer, "SecondBuffer", ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		EXPECT_EQ(FirstTexture, SecondTexture);
		EXPECT_EQ(FirstBuffer, SecondBuffer);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Resources.size(), 2u);
		EXPECT_EQ(Capture.Resources[0].Name, "First");
		EXPECT_EQ(Capture.Resources[1].Name, "FirstBuffer");

		FRDGBuilder OtherBuilder;
		const auto Other = OtherBuilder.RegisterExternalTexture(Texture, "Other", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		EXPECT_NE(FirstTexture, Other);
	}

	TEST_F(FRDGTests, RejectsConflictingExternalIdentityAndDomainMismatch)
	{
		auto Texture = MakeGraphTexture("Shared");
		auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"SharedBuffer", 64, 4, EBufferUsageFlags::UnorderedAccess
		));
		FRDGBuilder Duplicate;
		Duplicate.RegisterExternalTexture(Texture, "First", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		Duplicate.RegisterExternalTexture(Texture, "Second", ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead);
		auto DuplicateResult = FRDGBuilderTestAccessor::Compile(Duplicate);
		EXPECT_FALSE(DuplicateResult.IsSuccess());
		EXPECT_NE(DuplicateResult.Result.Message.find("conflicting external physical resource: canonical 'First'"), std::string::npos);
		EXPECT_NE(DuplicateResult.Result.Message.find("conflicts with 'Second'"),
			std::string::npos);

		FRDGBuilder BufferConflict;
		BufferConflict.RegisterExternalBuffer(Buffer, "CanonicalBuffer", ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		BufferConflict.RegisterExternalBuffer(Buffer, "ConflictingBuffer", ERHIAccess::ComputeShaderRead, ERHIAccess::TransferRead);
		auto BufferConflictResult = FRDGBuilderTestAccessor::Compile(BufferConflict);
		EXPECT_FALSE(BufferConflictResult.IsSuccess());
		EXPECT_NE(BufferConflictResult.Result.Message.find(
			"canonical 'CanonicalBuffer' (kind=buffer"), std::string::npos);
		EXPECT_NE(BufferConflictResult.Result.Message.find(
			"conflicts with 'ConflictingBuffer'"), std::string::npos);

		FRDGBuilder NullExternal;
		const auto NullHandle = NullExternal.RegisterExternalTexture({}, "Null", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		EXPECT_TRUE(NullHandle.IsValid());
		auto NullResult = FRDGBuilderTestAccessor::Compile(NullExternal);
		EXPECT_FALSE(NullResult.IsSuccess());
		EXPECT_EQ(NullResult.Result.Message, "resource 'Null' has no physical resource");

		FRDGBuilder Domain;
		const auto External = Domain.RegisterExternalTexture(Texture, "Shared", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Copy = FRDGBuilderTestAccessor::AddPass(Domain, "Copy", ERDGPassType::Copy);
		FRDGBuilderTestAccessor::UseTexture(Domain, Copy, External, WholeColor(), ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
		auto DomainResult = FRDGBuilderTestAccessor::Compile(Domain);
		EXPECT_FALSE(DomainResult.IsSuccess());
		EXPECT_NE(DomainResult.Result.Message.find("incompatible with pass domain"),
			std::string::npos);
	}

	TEST_F(FRDGTests, DiscardAndDontCareStorePreserveRetainedTextureAccess)
	{
		auto Texture = MakeGraphTexture("DiscardSync");
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Target = Builder.RegisterExternalTexture(Texture, "DiscardSync",
			ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Read, Target, WholeColor(), ERDGUse::Read,
			ERHIAccess::ComputeShaderRead);
		Builder.MarkPassRoot(Read, "read effect");
		const auto Clear = FRDGBuilderTestAccessor::AddPass(Builder, "Clear", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseColorAttachment(Builder, Clear, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::DontCare);
		Builder.MarkPassRoot(Clear, "write effect");
		const auto Rewrite = FRDGBuilderTestAccessor::AddPass(Builder, "Rewrite", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Rewrite, Target, WholeColor(), ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		Builder.MarkPassRoot(Rewrite, "replacement");
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetPasses().size(), 3u);
		const auto& ClearTransitions = Builder.GetPasses()[1].Barriers.GetTextureTransitions();
		ASSERT_EQ(ClearTransitions.size(), 1u);
		EXPECT_EQ(ClearTransitions[0].ExpectedBefore, ERHIAccess::ComputeShaderRead);
		EXPECT_TRUE(ClearTransitions[0].bDiscardContents);
		const auto& RewriteTransitions = Builder.GetPasses()[2].Barriers.GetTextureTransitions();
		ASSERT_EQ(RewriteTransitions.size(), 1u);
		EXPECT_EQ(RewriteTransitions[0].ExpectedBefore, ERHIAccess::ColorAttachmentReadWrite);
		EXPECT_TRUE(RewriteTransitions[0].bDiscardContents);
	}

	TEST_F(FRDGTests, DiscardValueCullingDoesNotRetainOverwrittenProducer)
	{
		auto Texture = MakeGraphTexture("Versioned");
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Resource = CreateTestTexture(Builder, "Versioned", Texture);
		const auto Old = FRDGBuilderTestAccessor::AddPass(Builder, "Old", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Old, Resource, WholeColor(), ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Replacement = FRDGBuilderTestAccessor::AddPass(Builder, "Replacement",
			ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Replacement, Resource, WholeColor(),
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto Consume = FRDGBuilderTestAccessor::AddPass(Builder, "Consume", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Consume, Resource, WholeColor(), ERDGUse::Read,
			ERHIAccess::ComputeShaderRead);
		Builder.MarkPassRoot(Consume, "output");
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetPasses().size(), 2u);
		EXPECT_EQ(Builder.GetPasses()[0].Name, "Replacement");
		EXPECT_TRUE(Builder.GetCullingDecisions()[0].bCulled);
	}

	TEST_F(FRDGTests, RetainedLogicalResourcesPublishExactPreparationCapture)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		FRDGBufferDesc Desc{
			.Buffer = FRHIBufferDesc(64, 4, EBufferUsageFlags::UnorderedAccess)};
		const auto Retained = Builder.CreateBuffer(Desc, "Retained");
		const auto Culled = Builder.CreateBuffer(Desc, "Culled");
		const auto Produce = FRDGBuilderTestAccessor::AddPass(Builder, "Produce", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Produce, Retained, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		Builder.MarkPassRoot(Produce, "effect");
		const auto Unused = FRDGBuilderTestAccessor::AddPass(Builder, "Unused", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Unused, Culled, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Resources.size(), 2u);
		EXPECT_EQ(Capture.Resources[0].Preparation, "requested");
		EXPECT_EQ(Capture.Resources[1].Preparation, "culled");
		ASSERT_EQ(Capture.Uses.size(), 1u);
		EXPECT_EQ(Capture.Uses[0].Version, 1u);
	}

	TEST_F(FRDGTests, PassResourceViewRejectsUndeclaredLookup)
	{
		auto DeclaredTexture = MakeGraphTexture("Declared");
		auto HiddenTexture = MakeGraphTexture("Hidden");
		FRDGBuilder Builder;
		const auto Declared = Builder.RegisterExternalTexture(DeclaredTexture, "Declared", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Hidden = Builder.RegisterExternalTexture(HiddenTexture, "Hidden", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Pass", ERDGPassType::Graphics,
			[=](FRHICommandListImmediate&, const FRDGPassResources& Resources) {
				Resources.GetTexture(Hidden);
			});
		FRDGBuilderTestAccessor::UseTexture(Builder, Pass, Declared, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);

		EXPECT_DEATH(Builder.Execute(GetCommandList()),
			"undeclared texture");
	}

	TEST_F(FRDGTests, ManagedAttachmentExitStateDrivesFollowingTransition)
	{
		auto Texture = MakeGraphTexture("Managed");
		FRDGBuilder Builder;
		const auto Target = CreateTestTexture(Builder, "Managed", Texture);
		const auto Render = FRDGBuilderTestAccessor::AddPass(Builder, "Render", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseManagedColorAttachment(Builder, Render, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			ERHIAccess::GraphicsShaderRead);
		const auto Consume = FRDGBuilderTestAccessor::AddPass(Builder, "Consume", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Consume, Target, WholeColor(), ERDGUse::Read,
			ERHIAccess::ComputeShaderRead);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ExpectCapturedBarriersMatchPlan(Builder);
		ASSERT_EQ(Builder.GetPasses()[0].Barriers.GetTextureTransitions().size(), 1u);
		EXPECT_TRUE(Builder.GetPasses()[0].Barriers.GetTextureTransitions()[0].bDiscardContents);
		ASSERT_EQ(Builder.GetPasses()[1].Barriers.GetTextureTransitions().size(), 1u);
		EXPECT_EQ(Builder.GetPasses()[1].Barriers.GetTextureTransitions()[0].ExpectedBefore,
			ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Builder.Capture().Transitions.size(), 3u);
	}

	TEST_F(FRDGTests, IncompleteBackingPublicationRecordsNoCallback)
	{
		bool bExecuted = false;
		FRDGBuilder Builder;
		const auto Buffer = Builder.CreateBuffer(
			FRDGBufferDesc{.Buffer = FRHIBufferDesc(
				64, 4, EBufferUsageFlags::UnorderedAccess)}, "Logical");
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute,
			[&](FRHICommandListImmediate&, const FRDGPassResources&) {
				bExecuted = true;
			});
		FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);

		FTestRDGAllocator Allocator;
		Allocator.bOmitResources = true;
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
		std::string Error;
		Error = Result.Result.Message;
		EXPECT_FALSE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_FALSE(bExecuted);
		EXPECT_NE(Error.find("omitted retained resource id="), std::string::npos);
	}

	TEST_F(FRDGTests, RDGAllocationIsDescriptorDrivenAndExtractionIsTransactional)
	{
		FTextureRHIRef FirstExtraction;
		FTextureRHIRef SecondExtraction;
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const FRDGTextureDesc Desc{
			.Texture = FRHITextureCreateDesc::Create2D(
				"DiagnosticOnly", 16, 16, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource)};
		const auto First = Builder.CreateTexture(Desc, "Renamed.First");
		const auto Second = Builder.CreateTexture(Desc, "Renamed.Second");
		const auto FirstPass = FRDGBuilderTestAccessor::AddPass(Builder,
			"First", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseColorAttachment(Builder, FirstPass, First, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		const auto SecondPass = FRDGBuilderTestAccessor::AddPass(Builder,
			"Second", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseColorAttachment(Builder, SecondPass, Second, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		Builder.QueueTextureExtraction(First, &FirstExtraction,
			ERHIAccess::GraphicsShaderRead);
		Builder.QueueTextureExtraction(Second, &SecondExtraction,
			ERHIAccess::GraphicsShaderRead);

		FTestRDGAllocator Allocator;
		FRDGExecutionContext Context{Allocator};
		EXPECT_FALSE(FirstExtraction);
		EXPECT_FALSE(SecondExtraction);
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetPasses().size(), 3u);

		std::string Error;
		Error = Result.Result.Message;
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_EQ(Allocator.AllocationCount, 2u);
		ASSERT_TRUE(FirstExtraction);
		ASSERT_TRUE(SecondExtraction);
		EXPECT_NE(FirstExtraction.GetReference(), SecondExtraction.GetReference());
		const auto Capture = Builder.Capture();
		EXPECT_EQ(Capture.Resources[0].AllocationDisposition, "allocated");
		EXPECT_NE(Capture.Resources[0].PhysicalAllocationId, 0u);
		EXPECT_NE(Capture.Resources[0].PhysicalAllocationId,
			Capture.Resources[1].PhysicalAllocationId);
	}


	TEST_F(FRDGTests, AllocationRequestsDistinguishExportsFromTransientResources)
	{
		FTextureRHIRef ExportedTexture;
		FBufferRHIRef ExportedBuffer;
		FRDGBuilder Builder;
		const auto Texture = Builder.CreateTexture(
			FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
				"Export", 8, 8, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable)}, "Export");
		const FRDGBufferDesc BufferDesc{.Buffer = FRHIBufferDesc(
			64, 4, EBufferUsageFlags::UnorderedAccess)};
		const auto Buffer = Builder.CreateBuffer(BufferDesc, "ExportBuffer");
		const auto Transient = Builder.CreateBuffer(BufferDesc, "Transient");
		const auto Graphics = FRDGBuilderTestAccessor::AddPass(Builder, "Graphics", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseColorAttachment(Builder, Graphics, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store);
		const auto Compute = FRDGBuilderTestAccessor::AddPass(Builder, "Compute", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Compute, Buffer, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Compute, Transient, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		Builder.QueueTextureExtraction(Texture, &ExportedTexture,
			ERHIAccess::ColorAttachmentReadWrite);
		Builder.QueueBufferExtraction(Buffer, &ExportedBuffer,
			ERHIAccess::ComputeShaderReadWrite);

		FTestRDGAllocator Allocator;
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Allocator.LastRequests.size(), 3u);
		EXPECT_TRUE(Allocator.LastRequests[0].bExtracted);
		EXPECT_TRUE(Allocator.LastRequests[1].bExtracted);
		EXPECT_FALSE(Allocator.LastRequests[2].bExtracted);
		EXPECT_TRUE(ExportedTexture);
		EXPECT_TRUE(ExportedBuffer);
	}

	TEST_F(FRDGTests, RDGAllocationFailurePublishesNoExtractionOrPass)
	{
		auto Original = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D(
				"Original", 4, 4, EPixelFormat::RGBA8_UNORM));
		FTextureRHIRef Destination = Original;
		bool bExecuted = false;
		FRDGBuilder Builder;
		const auto Texture = Builder.CreateTexture(
			FRDGTextureDesc{.Texture =
				FRHITextureCreateDesc::Create2D(
					"Logical", 16, 16, EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable)},
			"Logical");
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Graphics,
			[&](FRHICommandListImmediate&, const FRDGPassResources&) {
				bExecuted = true;
			});
		FRDGBuilderTestAccessor::UseColorAttachment(Builder, Pass, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		Builder.QueueTextureExtraction(Texture, &Destination,
			ERHIAccess::GraphicsShaderRead);

		FTestRDGAllocator Allocator;
		Allocator.bFail = true;
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
		std::string Error;
		Error = Result.Result.Message;
		EXPECT_FALSE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_FALSE(bExecuted);
		EXPECT_EQ(Destination.GetReference(), Original.GetReference());
		EXPECT_NE(Error.find("injected allocation failure"), std::string::npos);
	}

	TEST_F(FRDGTests, ExternalRegistrationRetainsPhysicalResource)
	{
		auto Texture = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D(
				"External", 8, 8, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource));
		const int32 InitialReferences = Texture.GetRefCount();
		{
			FRDGBuilder Builder;
			const auto External = Builder.RegisterExternalTexture(Texture,
				"External", ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Graphics);
			FRDGBuilderTestAccessor::UseTexture(Builder, Pass, External, WholeColor(), ERDGUse::Read,
				ERHIAccess::GraphicsShaderRead);

			const auto Result = Builder.Execute(GetCommandList());
			ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
			EXPECT_GT(Texture.GetRefCount(), InitialReferences);
			EXPECT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		}
		EXPECT_EQ(Texture.GetRefCount(), InitialReferences);
	}

	TEST_F(FRDGTests, ExternalAndAllocatedCapturesPublishHonestIdentity)
	{
		auto ExternalTexture = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D(
				"External", 8, 8, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource));
		auto AllocatedTexture = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D(
				"Allocated", 8, 8, EPixelFormat::RGBA8_UNORM
			)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
		);
		FRDGBuilder Builder;
		const auto External = Builder.RegisterExternalTexture(ExternalTexture,
			"External", ERHIAccess::GraphicsShaderRead,
			ERHIAccess::GraphicsShaderRead);
		const auto Allocated = CreateTestTexture(Builder, "Allocated", AllocatedTexture, ERHIAccess::GraphicsShaderRead);
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseTexture(Builder, Pass, External, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);
		FRDGBuilderTestAccessor::UseColorAttachment(Builder, Pass, Allocated, WholeColor(), ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store);

		FTestRDGAllocator Allocator;
		Allocator.TextureOverrides.emplace(1, AllocatedTexture);
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Resources.size(), 2u);
		EXPECT_EQ(Capture.Resources[0].AllocationDisposition, "external");
		EXPECT_EQ(Capture.Resources[0].PhysicalAllocationId, 0u);
		EXPECT_EQ(Capture.Resources[1].AllocationDisposition, "allocated");
		EXPECT_NE(Capture.Resources[1].PhysicalAllocationId, 0u);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		const auto ExecutedCapture = Builder.Capture();
		EXPECT_EQ(ExecutedCapture.Resources[1].AllocationDisposition, "allocated");
		EXPECT_NE(ExecutedCapture.Resources[1].PhysicalAllocationId, 0u);
	}

	TEST_F(FRDGTests, ExtractionRequiresCompleteStoredTextureContents)
	{
		for (bool Cull : {false, true})
			for (uint32 Mode = 0; Mode < 3; ++Mode)
			{
				FRDGBuilder Builder;
				if (Cull) Builder.EnablePassCulling();
				FTextureRHIRef Destination = MakeRefCount<FRHITexture>(
					FRHITextureCreateDesc::Create2D("Previous", 8, 8, EPixelFormat::RGBA8_UNORM));
				const auto* Previous = Destination.GetReference();
				const auto Texture = Builder.CreateTexture(FRDGTextureDesc{
					.Texture = FRHITextureCreateDesc::Create2D(
						"Output", 8, 8, EPixelFormat::RGBA8_UNORM)
						.SetNumMips(2).SetFlags(ETextureCreateFlags::RenderTargetable)},
					"Output");
				Builder.QueueTextureExtraction(Texture, &Destination,
					ERHIAccess::GraphicsShaderRead);
				if (Mode != 0)
				{
					const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Graphics);
					FRDGBuilderTestAccessor::UseColorAttachment(Builder, Pass, Texture, WholeColor(Mode == 1 ? 1 : 2),
						ERHIRenderTargetLoadAction::Clear,
						Mode == 1 ? ERHIRenderTargetStoreAction::Store
							: ERHIRenderTargetStoreAction::DontCare);
				}
				const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
				EXPECT_FALSE(Result.IsSuccess());
				EXPECT_NE(Result.Result.Message.find("RDG.Export"), std::string::npos);
				EXPECT_NE(Result.Result.Message.find("before its producer"), std::string::npos);
				EXPECT_EQ(Destination.GetReference(), Previous);
			}
	}

	TEST_F(FRDGTests, ExportDeclarationSurvivesCompilationAndLateFailurePublishesNothing)
	{
		for (bool bFail : {false, true})
		{
			FRDGCapture Capture;
			{
				FRDGBuilder Builder;
				if (bFail) Builder.SetBudget({.MaxBufferTransitions = 1});
				const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferDesc(
					64, 4, EBufferUsageFlags::UnorderedAccess)}, "Output");
				const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "RDG.Export", ERDGPassType::Compute);
				FRDGBuilderTestAccessor::UseBuffer(Builder, Write, Buffer, 0, 64, ERDGUse::Write,
					ERHIAccess::ComputeShaderReadWrite, true);
				FRDGBuilderTestAccessor::AddPass(Builder, "RDG.Export.Output", ERDGPassType::Compute);
				FBufferRHIRef Destination;
				Builder.QueueBufferExtraction(Buffer, &Destination, ERHIAccess::ComputeShaderRead);
				EXPECT_FALSE(Builder.Capture().bCompiled);
				const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
				EXPECT_EQ(Result.IsSuccess(), !bFail) << Result.Result.Message;
				EXPECT_EQ(Builder.HasCompiledPlan(), !bFail);
				EXPECT_FALSE(Destination);
				if (bFail)
					EXPECT_EQ(Result.Result.Message,
						"render graph safety limit exceeded: buffer-transitions actual=2 limit=1");
				Capture = Builder.Capture();
			}
			EXPECT_EQ(Capture.bCompiled, !bFail);
			if (bFail)
			{
				EXPECT_TRUE(Capture.Passes.empty());
				EXPECT_TRUE(Capture.Dependencies.empty());
				EXPECT_TRUE(Capture.Transitions.empty());
				EXPECT_TRUE(Capture.CullingDecisions.empty());
			}
			else
			{
				ASSERT_EQ(Capture.Passes.size(), 3u);
				EXPECT_EQ(Capture.Passes.back().Name, "RDG.Export.Output.Output");
				EXPECT_EQ(Capture.Uses.size(), 2u);
				EXPECT_EQ(Capture.Transitions.size(), 2u);
				EXPECT_NE(Capture.Dump.find("RDG.Export.Output.Output"), std::string::npos);
			}
		}
	}

	TEST_F(FRDGTests, BufferExtractionRequiresAProducerWithoutProvingByteCoverage)
	{
		for (bool Cull : {false, true})
			for (uint64 WrittenSize : {0u, 32u, 64u})
			{
				FRDGBuilder Builder;
				if (Cull) Builder.EnablePassCulling();
				FBufferRHIRef Destination;
				const auto Buffer = Builder.CreateBuffer(FRDGBufferDesc{
					.Buffer = FRHIBufferDesc(64, 4, EBufferUsageFlags::UnorderedAccess)},
					"Output");
				Builder.QueueBufferExtraction(Buffer, &Destination,
					ERHIAccess::ComputeShaderReadWrite);
				if (WrittenSize != 0)
				{
					const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
					FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, WrittenSize, ERDGUse::Write,
						ERHIAccess::ComputeShaderReadWrite, true);
				}

				FTestRDGAllocator Allocator;
				FRDGExecutionContext Context{Allocator};
				const auto Result = Builder.Execute(GetCommandList(), &Context);
				EXPECT_EQ(Result.IsSuccess(), WrittenSize != 0) << Result.Result.Message;
				if (Result.IsSuccess())
				{
					EXPECT_EQ(Builder.GetPasses().back().Name, "RDG.Export");
					EXPECT_EQ(Builder.GetResourceLifetimes()[0].LastPass, 1u);
					EXPECT_EQ(Builder.GetDependencies().size(), 1u);
					ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
					EXPECT_TRUE(Destination);
				}
				else
					EXPECT_NE(Result.Result.Message.find("before its producer"), std::string::npos);
			}
	}

	TEST_F(FRDGTests, BufferExtractionConservativelyRetainsEarlierPartialProducers)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		Builder.CreateToken("UnusedBeforeExport");
		FBufferRHIRef Destination;
		const auto Buffer = Builder.CreateBuffer(FRDGBufferDesc{
			.Buffer = FRHIBufferDesc(64, 4, EBufferUsageFlags::UnorderedAccess)}, "Output");
		Builder.CreateToken("UnusedAfterExport");
		for (uint32 Index = 0; Index < 3; ++Index)
		{
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write" + std::to_string(Index), ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, Index == 2 ? 32 : 0, Index == 0 ? 64 : 32,
				ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		}
		Builder.QueueBufferExtraction(Buffer, &Destination, ERHIAccess::ComputeShaderRead);
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetPasses().size(), 4u);
		EXPECT_EQ(Builder.GetPasses()[0].Name, "Write0");
		EXPECT_EQ(Builder.GetPasses()[1].Name, "Write1");
		EXPECT_EQ(Builder.GetResourceLifetimes()[1].LastPass, 3u);
		EXPECT_EQ(Builder.GetDependencies().size(), 3u);
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Uses.size(), 4u);
		for (const auto& Use : Capture.Uses) EXPECT_EQ(Use.ResourceId, 1u);
		EXPECT_EQ(Capture.Uses.back().PassDeclarationIndex, 3u);
	}

	TEST_F(FRDGTests, ExtractionRejectsInvalidFinalAccess)
	{
		for (const auto Access : {ERHIAccess::None, ERHIAccess::Discard,
			ERHIAccess::Present, static_cast<ERHIAccess>(1u << 30)})
		{
			FRDGBuilder Builder;
			FBufferRHIRef Destination;
			const auto Buffer = Builder.CreateBuffer(FRDGBufferDesc{
				.Buffer = FRHIBufferDesc(64, 4, EBufferUsageFlags::UnorderedAccess)}, "Output");
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, 64, ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
			Builder.QueueBufferExtraction(Buffer, &Destination, Access);
			const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_NE(Result.Result.Message.find("final access"), std::string::npos);
			EXPECT_FALSE(Destination);
		}
	}

	TEST_F(FRDGTests, ExtractionRejectsDiscardedExternalContents)
	{
		auto Texture = MakeRefCount<FRHITexture>(FRHITextureCreateDesc::Create2D(
			"External", 8, 8, EPixelFormat::RGBA8_UNORM));
		FTextureRHIRef Destination;
		FRDGBuilder Builder;
		const auto Handle = Builder.RegisterExternalTexture(Texture, "External",
			ERHIAccess::Discard, ERHIAccess::GraphicsShaderRead);
		Builder.QueueTextureExtraction(Handle, &Destination, ERHIAccess::GraphicsShaderRead);
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Result.Message.find("before its producer"), std::string::npos);
	}

	TEST_F(FRDGTests, ExternalExtractionRoundTripPublishesAfterExecution)
	{
		auto Texture = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D(
				"External", 8, 8, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource));
		FTextureRHIRef Extracted;
		FRDGBuilder Builder;
		const auto External = Builder.RegisterExternalTexture(Texture,
			"External", ERHIAccess::GraphicsShaderRead,
			ERHIAccess::GraphicsShaderRead);
		Builder.QueueTextureExtraction(External, &Extracted,
			ERHIAccess::ComputeShaderRead);

		EXPECT_FALSE(Extracted);
		const auto Result = Builder.Execute(GetCommandList());
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetPasses().size(), 1u);
		const auto& Export = Builder.GetPasses()[0];
		ASSERT_EQ(Export.Barriers.GetTextureTransitions().size(), 1u);
		EXPECT_EQ(Export.Barriers.GetTextureTransitions()[0].RequiredAfter, ERHIAccess::ComputeShaderRead);
		EXPECT_FALSE(Builder.GetResourceLifetimes()[0].bCulled);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_EQ(Extracted.GetReference(), Texture.GetReference());
	}

	TEST_F(FRDGTests, DuplicateExtractionFailsWithoutPublishing)
	{
		FTextureRHIRef First;
		FTextureRHIRef Second;
		FRDGBuilder Builder;
		const auto Texture = Builder.CreateTexture(
			FRDGTextureDesc{.Texture =
				FRHITextureCreateDesc::Create2D(
					"Logical", 8, 8, EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable)},
			"Logical");
		Builder.QueueTextureExtraction(Texture, &First,
			ERHIAccess::GraphicsShaderRead);
		Builder.QueueTextureExtraction(Texture, &Second,
			ERHIAccess::GraphicsShaderRead);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Result.Message.find("duplicate or conflicting texture extraction"),
			std::string::npos);
		EXPECT_FALSE(First);
		EXPECT_FALSE(Second);
	}

	TEST_F(FRDGTests, BufferExtractionPublishesCountedAllocation)
	{
		FBufferRHIRef Extracted;
		FRDGBuilder Builder;
		const auto Buffer = Builder.CreateBuffer(
			FRDGBufferDesc{.Buffer = FRHIBufferDesc(
				64, 4, EBufferUsageFlags::UnorderedAccess)}, "LogicalBuffer");
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		Builder.QueueBufferExtraction(Buffer, &Extracted,
			ERHIAccess::ComputeShaderRead);

		FTestRDGAllocator Allocator;
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
		std::string Error;
		Error = Result.Result.Message;
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_TRUE(Extracted);
		EXPECT_EQ(Extracted->GetDesc().Size, 64u);
	}

	TEST_F(FRDGTests, RejectsBackingWithIncompatibleUsageFlags)
	{
		auto TextureBacking = MakeRefCount<FRHITexture>(FRHITextureCreateDesc::Create2D(
			"TextureBacking", 16, 16, EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::ShaderResource));
		FRDGBuilder TextureBuilder;
		const auto Texture = TextureBuilder.CreateTexture(FRDGTextureDesc{
				.Texture = FRHITextureCreateDesc::Create2D(
					"LogicalTexture", 16, 16, EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable)}, "LogicalTexture");
		const auto TexturePass = FRDGBuilderTestAccessor::AddPass(TextureBuilder,
			"TextureWrite", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseColorAttachment(TextureBuilder, TexturePass, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);

		FTestRDGAllocator TextureAllocator;
		TextureAllocator.TextureOverride = TextureBacking;
		FRDGExecutionContext TextureContext{TextureAllocator};
		const auto TextureResult = TextureBuilder.Execute(GetCommandList(), &TextureContext);
		ASSERT_TRUE(TextureBuilder.HasCompiledPlan()) << TextureResult.Result.Message;
		std::string Error;
		Error = TextureResult.Result.Message;
		EXPECT_FALSE(TextureResult.IsSuccess()) << TextureResult.Result.Message;
		EXPECT_NE(Error.find("incompatible texture"), std::string::npos);

		auto BufferBacking = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"BufferBacking", 64, 4, EBufferUsageFlags::StructuredBuffer));
		FRDGBuilder BufferBuilder;
		const auto Buffer = BufferBuilder.CreateBuffer(FRDGBufferDesc{
				.Buffer = FRHIBufferDesc(
					64, 4, EBufferUsageFlags::UnorderedAccess)}, "LogicalBuffer");
		const auto BufferPass = FRDGBuilderTestAccessor::AddPass(BufferBuilder,
			"BufferWrite", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(BufferBuilder, BufferPass, Buffer, 0, 64,
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);

		FTestRDGAllocator BufferAllocator;
		BufferAllocator.BufferOverride = BufferBacking;
		FRDGExecutionContext BufferContext{BufferAllocator};
		const auto BufferResult = BufferBuilder.Execute(GetCommandList(), &BufferContext);
		ASSERT_TRUE(BufferBuilder.HasCompiledPlan()) << BufferResult.Result.Message;
		Error = BufferResult.Result.Message;
		EXPECT_FALSE(BufferResult.IsSuccess()) << BufferResult.Result.Message;
		EXPECT_NE(Error.find("incompatible buffer"), std::string::npos);
	}

	TEST_F(FRDGTests, AcceptsBackingWithSupersetUsageFlags)
	{
		static const auto TextureBacking = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D(
			"TextureBacking", 16, 16, EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource));
		FRDGBuilder TextureBuilder;
		const auto Texture = TextureBuilder.CreateTexture(FRDGTextureDesc{
				.Texture = FRHITextureCreateDesc::Create2D(
					"LogicalTexture", 16, 16, EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable)}, "LogicalTexture");
		const auto TexturePass = FRDGBuilderTestAccessor::AddPass(TextureBuilder,
			"TextureWrite", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseColorAttachment(TextureBuilder, TexturePass, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);

		FTestRDGAllocator TextureAllocator;
		TextureAllocator.TextureOverride = TextureBacking;
		FRDGExecutionContext TextureContext{TextureAllocator};
		const auto TextureResult = TextureBuilder.Execute(GetCommandList(), &TextureContext);
		ASSERT_TRUE(TextureBuilder.HasCompiledPlan()) << TextureResult.Result.Message;
		std::string Error;
		Error = TextureResult.Result.Message;
		EXPECT_TRUE(TextureResult.IsSuccess()) << TextureResult.Result.Message;

		static const auto BufferBacking = MakeRefCount<FRHIBuffer>(
			FRHIBufferCreateDesc::Create(
			"BufferBacking", 64, 4, EBufferUsageFlags::UnorderedAccess
				| EBufferUsageFlags::StructuredBuffer));
		FRDGBuilder BufferBuilder;
		const auto Buffer = BufferBuilder.CreateBuffer(FRDGBufferDesc{
				.Buffer = FRHIBufferDesc(
					64, 4, EBufferUsageFlags::UnorderedAccess)}, "LogicalBuffer");
		const auto BufferPass = FRDGBuilderTestAccessor::AddPass(BufferBuilder,
			"BufferWrite", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(BufferBuilder, BufferPass, Buffer, 0, 64,
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);

		FTestRDGAllocator BufferAllocator;
		BufferAllocator.BufferOverride = BufferBacking;
		FRDGExecutionContext BufferContext{BufferAllocator};
		const auto BufferResult = BufferBuilder.Execute(GetCommandList(), &BufferContext);
		ASSERT_TRUE(BufferBuilder.HasCompiledPlan()) << BufferResult.Result.Message;
		Error = BufferResult.Result.Message;
		EXPECT_TRUE(BufferResult.IsSuccess()) << BufferResult.Result.Message;
	}

	TEST_F(FRDGTests, ExplicitEffectRootSurvivesWithoutResourceOutputs)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Timestamp = FRDGBuilderTestAccessor::AddPass(Builder,
			"Timestamp", ERDGPassType::Graphics);
		Builder.MarkPassRoot(Timestamp, "timestamp");
		FRDGBuilderTestAccessor::AddPass(Builder, "Unused", ERDGPassType::Graphics);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetPasses().size(), 1u);
		EXPECT_EQ(Builder.GetPasses()[0].Name, "Timestamp");
	}

	TEST_F(FRDGTests, LogicalTokensDriveDependenciesAndLifetimesWithoutRHIState)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Prepared = Builder.CreateToken("Prepared");
		const auto Output = Builder.CreateToken("Output");
		const auto Prepare = FRDGBuilderTestAccessor::AddPass(Builder, "Prepare", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseToken(Builder, Prepare, Prepared, ERDGUse::Write);
		const auto Render = FRDGBuilderTestAccessor::AddPass(Builder, "Render", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseToken(Builder, Render, Prepared, ERDGUse::Read);
		FRDGBuilderTestAccessor::UseToken(Builder, Render, Output, ERDGUse::Write);
		Builder.MarkPassRoot(Render, "present");
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetPasses().size(), 2u);
		ASSERT_EQ(Builder.GetDependencies().size(), 1u);
		EXPECT_EQ(Builder.GetDependencies()[0].Cause, "Prepared");
		EXPECT_TRUE(Builder.GetPasses()[0].Barriers.GetBufferTransitions().empty());
		EXPECT_TRUE(Builder.GetPasses()[0].Barriers.GetTextureTransitions().empty());
		EXPECT_EQ(Builder.GetResourceLifetimes()[0].FirstPass, 0u);
		EXPECT_EQ(Builder.GetResourceLifetimes()[0].LastPass, 1u);
	}

	TEST_F(FRDGTests, GBufferManualDeclarationOracleFreezesCompletePassShape)
	{
		std::array<FTextureRHIRef, 4> ColorTextures{
			MakeGraphTexture("Scene.GBuffer.Material"),
			MakeGraphTexture("Scene.GBuffer.Normals"),
			MakeGraphTexture("Scene.GBuffer.Surface"),
			MakeGraphTexture("Scene.GBuffer.Emissive"),
		};
		auto DepthTexture = MakeRefCount<FRHITexture>(FRHITextureCreateDesc::Create2D(
							 "Scene.Depth", 64, 64, EPixelFormat::D32
		)
							 .SetFlags(ETextureCreateFlags::DepthStencilTargetable | ETextureCreateFlags::ShaderResource));
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		std::array<FRDGTextureHandle, 4> Colors{};
		const std::array Names{"Scene.GBuffer.Material", "Scene.GBuffer.Normals",
			"Scene.GBuffer.Surface", "Scene.GBuffer.Emissive"};
		for (uint32 Index = 0; Index < Colors.size(); ++Index)
			Colors[Index] = CreateTestTexture(Builder, Names[Index], ColorTextures[Index], ERHIAccess::GraphicsShaderRead);
		const auto Depth = CreateTestTexture(Builder, "Scene.Depth", DepthTexture, ERHIAccess::GraphicsShaderRead);
		const auto Completion = Builder.CreateToken("Scene.GBuffer.Result");
		uint32 CallbackCount = 0;
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Scene.GBuffer",
			ERDGPassType::Graphics,
			[&](FRHICommandListImmediate&,
				const FRDGPassResources& Resources) {
				for (const auto Color : Colors)
					EXPECT_NE(Resources.GetTexture(Color), nullptr);
				EXPECT_EQ(Resources.GetTexture(Depth), DepthTexture.GetReference());
				++CallbackCount;
			});
		FRDGBuilderTestAccessor::UseToken(Builder, Pass, Completion, ERDGUse::Write);
		for (const auto Color : Colors)
			FRDGBuilderTestAccessor::UseManagedColorAttachment(Builder, Pass, Color, WholeColor(),
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead);
		FRDGBuilderTestAccessor::UseManagedDepthStencilAttachment(Builder, Pass, Depth,
			{ERHITextureAspect::Depth, 0, 1, 0, 1},
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			ERHIAccess::GraphicsShaderRead);
		Builder.MarkPassRoot(Pass, "gbuffer-pilot");

		FTestRDGAllocator Allocator;
		for (uint32 Index = 0; Index < ColorTextures.size(); ++Index)
			Allocator.TextureOverrides.emplace(Index, ColorTextures[Index]);
		Allocator.TextureOverrides.emplace(4, DepthTexture);
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
		const FRDGCapture Capture = Builder.Capture();
		ASSERT_EQ(Capture.Passes.size(), 1u);
		EXPECT_EQ(Capture.Passes[0].Name, "Scene.GBuffer");
		EXPECT_EQ(Capture.Passes[0].Type, ERDGPassType::Graphics);
		EXPECT_EQ(Capture.Statistics.DeclaredPasses, 1u);
		EXPECT_EQ(Capture.Statistics.ScheduledPasses, 1u);
		EXPECT_EQ(Capture.Statistics.Dependencies, 0u);
		EXPECT_EQ(Capture.Statistics.TextureTransitions, 5u);
		ASSERT_EQ(Capture.Uses.size(), 6u);
		EXPECT_EQ(Capture.Uses[0].ResourceId, 5u);
		EXPECT_EQ(Capture.Uses[0].Use, ERDGUse::Write);
		EXPECT_EQ(Capture.Uses[0].Access, ERHIAccess::None);
		EXPECT_TRUE(Capture.Uses[0].bDiscard);
		for (uint32 Index = 0; Index < Colors.size(); ++Index)
		{
			const auto& Use = Capture.Uses[Index + 1];
			EXPECT_EQ(Use.ResourceId, Index);
			EXPECT_EQ(Use.Use, ERDGUse::ReadWrite);
			EXPECT_EQ(Use.Access, ERHIAccess::ColorAttachmentReadWrite);
			EXPECT_TRUE(Use.bDiscard);
			EXPECT_TRUE(Use.bStore);
		}
		EXPECT_EQ(Capture.Uses[5].ResourceId, 4u);
		EXPECT_EQ(Capture.Uses[5].Access, ERHIAccess::DepthStencilReadWrite);
		EXPECT_EQ(Capture.Uses[5].TextureRange.Aspects,
			ERHITextureAspect::Depth);
		ASSERT_EQ(Capture.Transitions.size(), 10u);
		for (uint32 Index = 0; Index < 5; ++Index)
		{
			const auto& Entry = Capture.Transitions[Index * 2];
			const auto& Exit = Capture.Transitions[Index * 2 + 1];
			const ERHIAccess AttachmentAccess = Index < 4
				? ERHIAccess::ColorAttachmentReadWrite
				: ERHIAccess::DepthStencilReadWrite;
			EXPECT_EQ(Entry.Kind, ERDGTransitionKind::RHIBarrier);
			EXPECT_EQ(Exit.Kind, ERDGTransitionKind::PassManaged);
			EXPECT_EQ(Entry.ResourceId, Index);
			EXPECT_EQ(Entry.PassIndex, 0u);
			EXPECT_EQ(Entry.Before, ERHIAccess::Discard);
			EXPECT_EQ(Entry.After, AttachmentAccess);
			EXPECT_FALSE(Entry.bFinal);
			EXPECT_EQ(Exit.ResourceId, Index);
			EXPECT_EQ(Exit.Before, AttachmentAccess);
			EXPECT_EQ(Exit.After, ERHIAccess::GraphicsShaderRead);
			EXPECT_FALSE(Exit.bFinal);
		}
		ASSERT_EQ(Capture.ResourceLifetimes.size(), 6u);
		for (const auto& Lifetime : Capture.ResourceLifetimes)
		{
			EXPECT_EQ(Lifetime.FirstPass, 0u);
			EXPECT_EQ(Lifetime.LastPass, 0u);
			EXPECT_FALSE(Lifetime.bCulled);
		}
		ASSERT_EQ(Capture.CullingDecisions.size(), 1u);
		EXPECT_EQ(Capture.CullingDecisions[0].Reason, "gbuffer-pilot");
		EXPECT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_EQ(CallbackCount, 1u);
	}

	TEST_F(FRDGTests, GBufferManualDeclarationOracleKeepsBackingFailureAtomic)
	{
		FRDGBuilder Builder;
		const auto Material = Builder.CreateTexture(FRDGTextureDesc{
				.Texture = FRHITextureCreateDesc::Create2D("Scene.GBuffer.Material",
					64, 64, EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource)},
			"Scene.GBuffer.Material",
			ERHIAccess::GraphicsShaderRead);
		bool bExecuted = false;
		const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Scene.GBuffer",
			ERDGPassType::Graphics,
			[&](FRHICommandListImmediate&,
				const FRDGPassResources&) { bExecuted = true; });
		FRDGBuilderTestAccessor::UseManagedColorAttachment(Builder, Pass, Material, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			ERHIAccess::GraphicsShaderRead);

		FTestRDGAllocator Allocator;
		Allocator.bOmitResources = true;
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
		std::string Error;
		Error = Result.Result.Message;
		EXPECT_FALSE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_FALSE(bExecuted);
		EXPECT_NE(Error.find("omitted retained resource id="),
			std::string::npos);
	}

	TEST_F(FRDGTests, EnforcesDeterministicStructuralBudgets)
	{
		FRDGBuilder Builder;
		Builder.SetBudget({.MaxPasses = 1});
		FRDGBuilderTestAccessor::AddPass(Builder, "First", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::AddPass(Builder, "Second", ERDGPassType::Graphics);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_EQ(Result.Result.Error, ERDGError::SafetyLimitExceeded);
		EXPECT_EQ(Result.Result.Message,
			"render graph safety limit exceeded: passes actual=2 limit=1");
	}

	TEST_F(FRDGTests, BoundsRangeExpansionAndVisitsBeforeDependencyCompilation)
	{
		auto Compile = [](FRDGBudget Budget) {
			FRDGBuilder Builder;
			Builder.SetBudget(Budget);
			auto Desc = DescribeGraphTexture(*MakeGraphTexture("Sparse", 4));
			Desc.Texture.ArraySize = 4;
			const auto Texture = Builder.CreateTexture(Desc, "Sparse");
			// Fixed layout includes all sixteen subresources, even with sparse uses.
			for (uint32 Index = 0; Index < 4; ++Index)
			{
				const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write" + std::to_string(Index), ERDGPassType::Compute);
				FRDGBuilderTestAccessor::UseTexture(Builder, Pass, Texture, {ERHITextureAspect::Color, Index, 1, Index, 1},
					ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			}
			return FRDGBuilderTestAccessor::Compile(Builder).Result.Message;
		};
		EXPECT_EQ(Compile({.MaxResources = 0}),
			"render graph safety limit exceeded: resources actual=1 limit=0");
		EXPECT_EQ(Compile({.MaxUses = 3}),
			"render graph safety limit exceeded: uses actual=4 limit=3");
		EXPECT_EQ(Compile({.MaxRangeCells = 3}),
			"render graph safety limit exceeded: range-cells actual=4 limit=3");
		EXPECT_EQ(Compile({.MaxRangeCellCandidates = 3}),
			"render graph safety limit exceeded: range-cell-candidates actual=4 limit=3");
		EXPECT_EQ(Compile({.MaxCellVisits = 0}),
			"render graph safety limit exceeded: cell-visits actual=1 limit=0");
		EXPECT_TRUE(Compile({.MaxRangeCells = 16, .MaxRangeCellCandidates = 16}).empty());
	}

	TEST_F(FRDGTests, BufferTrackingUsesOneCellRegardlessOfByteRanges)
	{
		for (uint32 Count : {32u, 256u})
		{
			FRDGBuilder Builder;
			// One layout cell and one visit per resource access in each traversal.
			Builder.SetBudget({.MaxRangeCells = 1, .MaxCellVisits = 6 * Count});
			const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferCreateDesc::Create(
				"Sparse", Count * 16, 4, EBufferUsageFlags::UnorderedAccess)}, "Sparse");
			for (uint32 Index = Count; Index-- > 0;)
			{
				const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write" + std::to_string(Index), ERDGPassType::Compute);
				FRDGBuilderTestAccessor::UseBuffer(Builder, Write, Buffer, Index * 16, 8, ERDGUse::Write,
					ERHIAccess::ComputeShaderReadWrite, true);
				const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read" + std::to_string(Index), ERDGPassType::Compute);
				FRDGBuilderTestAccessor::UseBuffer(Builder, Read, Buffer, Index * 16, 8, ERDGUse::Read, ERHIAccess::ComputeShaderRead);
			}
			const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			const auto Capture = Builder.Capture();
			EXPECT_EQ(Capture.Dependencies.size(), 3 * Count - 2);
			ASSERT_EQ(Capture.Uses.size(), 2u * Count);
			EXPECT_EQ(Capture.Transitions.size(), 2u * Count);
			for (uint32 Index = 0; Index < Capture.Uses.size(); ++Index)
			{
				EXPECT_EQ(Capture.Uses[Index].BufferOffset, (Count - 1 - Index / 2) * 16u);
				EXPECT_EQ(Capture.Uses[Index].BufferSize, 8u);
				EXPECT_EQ(Capture.Uses[Index].Version, Index / 2 + 1);
			}
		}
	}

	TEST_F(FRDGTests, TextureUseIndexKeepsDepthAndStencilStatesSeparate)
	{
		FRDGBuilder Builder;
		const auto Texture = CreateTestTexture(Builder, "DepthStencil",
			MakeRefCount<FRHITexture>(FRHITextureCreateDesc::Create2D("DepthStencil", 64, 64,
				EPixelFormat::D24S8).SetFlags(ETextureCreateFlags::DepthStencilTargetable
					| ETextureCreateFlags::ShaderResource)));
		const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "WriteBoth", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseTexture(Builder, Write, Texture,
			{ERHITextureAspect::Depth | ERHITextureAspect::Stencil, 0, 1, 0, 1},
			ERDGUse::Write, ERHIAccess::DepthStencilReadWrite, true);
		const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "ReadStencil", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseTexture(Builder, Read, Texture, {ERHITextureAspect::Stencil, 0, 1, 0, 1},
			ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
		const auto Overwrite = FRDGBuilderTestAccessor::AddPass(Builder, "OverwriteDepth", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::UseTexture(Builder, Overwrite, Texture, {ERHITextureAspect::Depth, 0, 1, 0, 1},
			ERDGUse::Write, ERHIAccess::DepthStencilReadWrite, true);
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		const auto Capture = Builder.Capture();
		EXPECT_EQ(Capture.Dependencies.size(), 2u);
		ASSERT_EQ(Capture.Uses.size(), 4u);
		const std::array Aspects{ERHITextureAspect::Depth, ERHITextureAspect::Stencil,
			ERHITextureAspect::Stencil, ERHITextureAspect::Depth};
		for (size_t Index = 0; Index < Aspects.size(); ++Index)
		{
			EXPECT_EQ(Capture.Uses[Index].TextureRange.Aspects, Aspects[Index]);
			EXPECT_EQ(Capture.Uses[Index].Version, Index == 3 ? 2u : 1u);
		}
		ASSERT_EQ(Builder.GetPasses()[1].Barriers.GetTextureTransitions().size(), 1u);
		EXPECT_EQ(Builder.GetPasses()[1].Barriers.GetTextureTransitions()[0].Range.Aspects, ERHITextureAspect::Stencil);
	}

	TEST_F(FRDGTests, TextureUseIndexBoundsSparseLayersAcrossMips)
	{
		FRDGBuilder Builder;
		constexpr uint32 Layers = 64;
		constexpr uint32 Mips = 4;
		// Fixed layout includes unused layers; visits address subresources directly.
		Builder.SetBudget({.MaxRangeCells = 2 * Layers * Mips, .MaxCellVisits = 6 * Layers * Mips});
		auto Desc = DescribeGraphTexture(*MakeGraphTexture("SparseLayers", Mips));
		Desc.Texture.ArraySize = Layers * 2;
		const auto Texture = Builder.CreateTexture(Desc, "SparseLayers");
		for (uint32 Layer = Layers; Layer-- > 0;)
		{
			for (uint32 Mip = 0; Mip < Mips; ++Mip)
			{
				const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write" + std::to_string(Layer) + "."
					+ std::to_string(Mip), ERDGPassType::Compute);
				FRDGBuilderTestAccessor::UseTexture(Builder, Write, Texture, {ERHITextureAspect::Color, Mip, 1, Layer * 2, 1},
					ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			}
			const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read" + std::to_string(Layer), ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseTexture(Builder, Read, Texture, {ERHITextureAspect::Color, 0, Mips, Layer * 2, 1},
				ERDGUse::Read, ERHIAccess::ComputeShaderRead);
		}
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		const auto Capture = Builder.Capture();
		EXPECT_EQ(Capture.Dependencies.size(), Layers * Mips);
		ASSERT_EQ(Capture.Uses.size(), 2u * Layers * Mips);
		EXPECT_EQ(Capture.Transitions.size(), 2u * Layers * Mips);
		for (uint32 Index = 0; Index < Capture.Uses.size(); ++Index)
		{
			const auto& Use = Capture.Uses[Index];
			EXPECT_EQ(Use.TextureRange.FirstMip, Index % Mips);
			EXPECT_EQ(Use.TextureRange.NumMips, 1u);
			EXPECT_EQ(Use.TextureRange.FirstArrayLayer, (Layers - 1 - Index / (2 * Mips)) * 2);
			EXPECT_EQ(Use.TextureRange.NumArrayLayers, 1u);
			EXPECT_EQ(Use.Version, 1u);
		}
	}

	TEST_F(FRDGTests, ResourceCellIndexBoundsMultiResourceInterleavedWork)
	{
		FRDGBuilder Builder;
		// Range comparisons scale with each resource's cells, not all 64 resources.
		Builder.SetBudget({.MaxRangeCells = 64 * 16, .MaxCellVisits = 64 * 296});
		auto Desc = DescribeGraphTexture(*MakeGraphTexture("Grid", 4));
		Desc.Texture.ArraySize = 4;
		for (uint32 Resource = 0; Resource < 64; ++Resource)
		{
			const auto Name = std::to_string(Resource);
			const auto Texture = Builder.CreateTexture(Desc, "Grid" + Name);
			for (uint32 Mip = 0; Mip < 4; ++Mip)
			{
				const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Mip" + Name + "." + std::to_string(Mip), ERDGPassType::Compute);
				FRDGBuilderTestAccessor::UseTexture(Builder, Pass, Texture, {ERHITextureAspect::Color, Mip, 1, 0, 4},
					ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			}
			const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read" + Name, ERDGPassType::Compute);
			for (uint32 Layer = 0; Layer < 4; ++Layer)
				FRDGBuilderTestAccessor::UseTexture(Builder, Read, Texture, {ERHITextureAspect::Color, 0, 4, Layer, 1},
					ERDGUse::Read, ERHIAccess::ComputeShaderRead);
		}
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		const auto Capture = Builder.Capture();
		EXPECT_EQ(Capture.Dependencies.size(), 64u * 4);
		EXPECT_EQ(Capture.Uses.size(), 64u * 32);
		EXPECT_EQ(Capture.Transitions.size(), 64u * 32);
		for (const auto& Use : Capture.Uses)
		{
			EXPECT_EQ(Use.TextureRange.NumMips, 1u);
			EXPECT_EQ(Use.TextureRange.NumArrayLayers, 1u);
			EXPECT_EQ(Use.Version, 1u);
		}
	}

	TEST_F(FRDGTests, RejectsDependenciesBeforeCullingAndTransitionsBeforeLaterUses)
	{
		{
			FRDGBuilder Builder;
			Builder.SetBudget({.MaxDependencies = 0});
			Builder.EnablePassCulling();
			const auto Token = Builder.CreateToken("Token");
			const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
			const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseToken(Builder, Write, Token, ERDGUse::Write);
			FRDGBuilderTestAccessor::UseToken(Builder, Read, Token, ERDGUse::Read);
			EXPECT_EQ(FRDGBuilderTestAccessor::Compile(Builder).Result.Message,
				"render graph safety limit exceeded: dependencies actual=1 limit=0");
		}
		for (bool bTexture : {false, true})
		{
			FRDGBuilder Builder;
			Builder.SetBudget({.MaxBufferTransitions = 0, .MaxTextureTransitions = 0,
				.MaxCellVisits = 4});
			const auto First = FRDGBuilderTestAccessor::AddPass(Builder, "First", ERDGPassType::Compute);
			const auto Second = FRDGBuilderTestAccessor::AddPass(Builder, "Second", ERDGPassType::Compute);
			if (bTexture)
			{
				const auto Texture = CreateTestTexture(Builder, "Texture", MakeGraphTexture("Texture"));
				for (const auto Pass : {First, Second})
					FRDGBuilderTestAccessor::UseTexture(Builder, Pass, Texture, WholeColor(), ERDGUse::Write,
						ERHIAccess::ComputeShaderReadWrite, true);
			}
			else
			{
				const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferCreateDesc::Create(
					"Buffer", 64, 4, EBufferUsageFlags::UnorderedAccess)}, "Buffer");
				for (const auto Pass : {First, Second})
					FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, 64, ERDGUse::Write,
						ERHIAccess::ComputeShaderReadWrite, true);
			}
			// One coverage visit and two hazard visits precede the first transition.
			// Permit that transition visit, but no later use visit.
			EXPECT_EQ(FRDGBuilderTestAccessor::Compile(Builder).Result.Message,
				std::string("render graph safety limit exceeded: ")
					+ (bTexture ? "texture" : "buffer") + "-transitions actual=1 limit=0");
		}
	}

	TEST_F(FRDGTests, CountsExportUsesAndDeduplicatesDependenciesAtTheLimit)
	{
		for (const auto Budget : {FRDGBudget{.MaxPasses = 1}, FRDGBudget{.MaxUses = 1}})
		{
			FRDGBuilder Builder;
			Builder.SetBudget(Budget);
			const auto Texture = CreateTestTexture(Builder, "Export", MakeGraphTexture("Export"));
			const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseTexture(Builder, Write, Texture, WholeColor(), ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
			FTextureRHIRef Destination;
			Builder.QueueTextureExtraction(Texture, &Destination, ERHIAccess::ComputeShaderRead);
			EXPECT_EQ(FRDGBuilderTestAccessor::Compile(Builder).Result.Message,
				std::string("render graph safety limit exceeded: ")
					+ (Budget.MaxPasses == 1 ? "passes" : "uses") + " actual=2 limit=1");
		}
		FRDGBuilder Builder;
		Builder.SetBudget({.MaxDependencies = 1});
		const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
		const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Compute);
		Builder.AddPassDependency(Write, Read);
		Builder.AddPassDependency(Write, Read);
		for (uint32 Index = 0; Index < 8; ++Index)
		{
			const auto Token = Builder.CreateToken("Token" + std::to_string(Index));
			FRDGBuilderTestAccessor::UseToken(Builder, Write, Token, ERDGUse::Write);
			FRDGBuilderTestAccessor::UseToken(Builder, Read, Token, ERDGUse::Read);
		}
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_EQ(Builder.GetDependencies().size(), 1u);
	}

	TEST_F(FRDGTests, ReportsStructuralRegressionBudgetsWithoutRejectingGraph)
	{
		FRDGBuilder Builder;
		Builder.SetBudget({
			.MaxPasses = 8,
			.RegressionMaxPasses = 1,
		});
		FRDGBuilderTestAccessor::AddPass(Builder, "First", ERDGPassType::Graphics);
		FRDGBuilderTestAccessor::AddPass(Builder, "Second", ERDGPassType::Graphics);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		const FRDGStatistics Statistics = Builder.GetStatistics();
		EXPECT_TRUE(Statistics.bPassRegressionBudgetExceeded);
		EXPECT_TRUE(Statistics.IsStructuralRegressionBudgetExceeded());
		EXPECT_EQ(Builder.Capture().Budget.RegressionMaxPasses, 1u);
	}

	TEST_F(FRDGTests, DetailedDiagnosticsAreLazyAndAllocationRefreshesExistingCapture)
	{
		for (bool bInspectDuringAllocation : {false, true})
		{
			FRDGBuilder Builder;
			const auto Texture = CreateTestTexture(Builder, "Lazy", MakeGraphTexture("Lazy"));
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseTexture(Builder, Pass, Texture, WholeColor(), ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
			FTestRDGAllocator Allocator;
			FRDGCapture Before;
			Allocator.OnAllocate = [&] {
				EXPECT_FALSE(FRDGBuilderTestAccessor::HasDiagnostics(Builder));
				EXPECT_EQ(Builder.GetStatistics().DeclaredPasses, 1u);
				EXPECT_EQ(Builder.GetResourceLifetimes().size(), 1u);
				EXPECT_FALSE(FRDGBuilderTestAccessor::HasDiagnostics(Builder));
				if (bInspectDuringAllocation) Before = Builder.Capture();
			};
			FRDGExecutionContext Context{Allocator};
			const auto Execution = Builder.Execute(GetCommandList(), &Context);
			ASSERT_TRUE(Execution.IsSuccess()) << Execution.Result.Message;
			EXPECT_EQ(FRDGBuilderTestAccessor::HasDiagnostics(Builder), bInspectDuringAllocation);
			const auto Capture = Builder.Capture();
			ASSERT_EQ(Capture.Resources.size(), 1u);
			EXPECT_EQ(Capture.Resources[0].AllocationDisposition, "allocated");
			EXPECT_NE(Capture.Resources[0].PhysicalAllocationId, 0u);
			EXPECT_EQ(Capture.Dump, Builder.Dump());
			EXPECT_EQ(Capture.Dump, Builder.Capture().Dump);
			if (bInspectDuringAllocation)
			{
				ASSERT_EQ(Before.Resources.size(), 1u);
				EXPECT_EQ(Before.Resources[0].AllocationDisposition, "pending");
				EXPECT_EQ(Before.Resources[0].PhysicalAllocationId, 0u);
			}
		}
	}

	TEST_F(FRDGTests, LazyParameterCaptureUsesFrozenDeclarationsAndOwnsItsData)
	{
		FRDGCapture Capture;
		{
			FRDGBuilder Builder;
			// Inspecting an uncompiled graph must not leave an empty cache after compilation.
			EXPECT_FALSE(Builder.Capture().bCompiled);
			const auto Texture = Builder.RegisterExternalTexture(MakeGraphTexture("Frozen", 2),
				"Frozen", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<FComposedTextureArrayParameters>();
			Parameters->Textures[1] = FRDGTextureParameter{Texture, {ERHITextureAspect::Color, 1, 1, 0, 1}};
			auto* Payload = &Parameters.Get();
			FRDGBuilderTestAccessor::AddPass(Builder, "Read", ERDGPassType::Graphics, std::move(Parameters));
			const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			EXPECT_FALSE(FRDGBuilderTestAccessor::HasDiagnostics(Builder));
			// Mutating the payload after submission cannot change the declared capabilities.
			Payload->Textures[0] = Payload->Textures[1];
			Payload->Textures[1].reset();
			Capture = Builder.Capture();
			EXPECT_TRUE(FRDGBuilderTestAccessor::HasDiagnostics(Builder));
			EXPECT_EQ(Capture.Dump, Builder.Dump());
		}
		ASSERT_EQ(Capture.Parameters.size(), 2u);
		EXPECT_FALSE(Capture.Parameters[0].bPresent);
		EXPECT_EQ(Capture.Parameters[0].ResourceId, std::numeric_limits<uint32>::max());
		EXPECT_TRUE(Capture.Parameters[1].bPresent);
		EXPECT_EQ(Capture.Parameters[1].TextureRange.FirstMip, 1u);
		EXPECT_EQ(Capture.Parameters[1].FieldPath, "FComposedTextureArrayParameters.Textures[1]");
		ASSERT_EQ(Capture.Uses.size(), 1u);
		EXPECT_EQ(Capture.Uses[0].TextureRange.FirstMip, 1u);
	}

	TEST_F(FRDGTests, FixedTextureSubresourcesMatchIndependentCoverageOracle)
	{
		const std::array<FRHITextureSubresourceRange, 6> Ranges{{
			{ERHITextureAspect::Color, 0, 2, 0, 2},
			{ERHITextureAspect::Color, 3, 3, 3, 3},
			{ERHITextureAspect::Color, 1, 3, 1, 3},
			{ERHITextureAspect::Color, 0, 1, 5, 1},
			{ERHITextureAspect::Color, 3, 1, 0, 6},
			{ERHITextureAspect::Color, 0, 6, 2, 1}}};
		FRDGBuilder Builder;
		auto Desc = DescribeGraphTexture(*MakeGraphTexture("Sweep", 6));
		Desc.Texture.ArraySize = 6;
		const auto Texture = Builder.CreateTexture(Desc, "Sweep");
		for (uint32 Index = 0; Index < Ranges.size(); ++Index)
		{
			const auto& Range = Ranges[Index];
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write" + std::to_string(Index), ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseTexture(Builder, Pass, Texture, Range, ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
		}
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		const auto Capture = Builder.Capture();
		std::array<std::array<uint32, 6>, 6> Versions{};
		size_t ExpectedIndex = 0;
		// Check every subresource against declaration coverage, without using layout helpers.
		for (uint32 PassIndex = 0; PassIndex < Ranges.size(); ++PassIndex)
			for (uint32 Mip = 0; Mip < 6; ++Mip)
				for (uint32 Layer = 0; Layer < 6; ++Layer)
				{
					const auto& Range = Ranges[PassIndex];
					if (Mip < Range.FirstMip || (Mip + 1) > Range.FirstMip + Range.NumMips
						|| Layer < Range.FirstArrayLayer
						|| (Layer + 1) > Range.FirstArrayLayer + Range.NumArrayLayers) continue;
					ASSERT_LT(ExpectedIndex, Capture.Uses.size());
					ASSERT_LT(ExpectedIndex, Capture.Transitions.size());
					const auto& Use = Capture.Uses[ExpectedIndex];
					const auto& Transition = Capture.Transitions[ExpectedIndex++];
					auto& Version = Versions[Mip][Layer];
					EXPECT_EQ(Transition.Before, Version == 0 ? ERHIAccess::Discard : ERHIAccess::ComputeShaderReadWrite);
					EXPECT_EQ(Transition.After, ERHIAccess::ComputeShaderReadWrite);
					EXPECT_EQ(Transition.PassIndex, PassIndex);
					EXPECT_TRUE(Transition.bDiscardContents);
					EXPECT_EQ(Use.PassDeclarationIndex, PassIndex);
					EXPECT_EQ(Use.Version, ++Version);
					for (const auto& Actual : {Use.TextureRange, Transition.TextureRange})
					{
						EXPECT_EQ(Actual.FirstMip, Mip);
						EXPECT_EQ(Actual.NumMips, 1u);
						EXPECT_EQ(Actual.FirstArrayLayer, Layer);
						EXPECT_EQ(Actual.NumArrayLayers, 1u);
					}
				}
		EXPECT_EQ(Capture.Uses.size(), ExpectedIndex);
		EXPECT_EQ(Capture.Transitions.size(), ExpectedIndex);
	}

	TEST_F(FRDGTests, BufferTrackingPreservesDeclaredRangesWithResourceVersions)
	{
		FRDGBuilder Builder;
		Builder.CreateToken("UnusedBefore");
		const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferDesc(
			64, 4, EBufferUsageFlags::UnorderedAccess)}, "Buffer");
		Builder.CreateToken("UnusedAfter");
		const std::array<std::pair<uint64, uint64>, 3> Ranges{{{0, 16}, {32, 16}, {8, 32}}};
		for (uint32 Index = 0; Index < Ranges.size(); ++Index)
		{
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write" + std::to_string(Index), ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, Ranges[Index].first, Ranges[Index].second,
				ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		}
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Uses.size(), Ranges.size());
		ASSERT_EQ(Capture.Transitions.size(), Ranges.size());
		for (size_t Index = 0; Index < Ranges.size(); ++Index)
		{
			EXPECT_EQ(Capture.Uses[Index].ResourceId, 1u);
			EXPECT_EQ(Capture.Uses[Index].BufferOffset, Ranges[Index].first);
			EXPECT_EQ(Capture.Uses[Index].BufferSize, Ranges[Index].second);
			EXPECT_EQ(Capture.Uses[Index].Version, Index + 1);
			EXPECT_EQ(Capture.Transitions[Index].BufferOffset, 0u);
			EXPECT_EQ(Capture.Transitions[Index].BufferSize, 64u);
			EXPECT_FALSE(Capture.Transitions[Index].bDiscardContents);
		}
	}

	TEST_F(FRDGTests, CombinesSamePassBufferAccessWithoutLosingBindingRanges)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferDesc(
			64, 4, EBufferUsageFlags::UnorderedAccess)}, "Buffer");
		const auto Initialize = FRDGBuilderTestAccessor::AddPass(Builder, "Initialize", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Initialize, Buffer, 0, 64,
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto Mixed = FRDGBuilderTestAccessor::AddPass(Builder, "Mixed", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Mixed, Buffer, 0, 16,
			ERDGUse::Read, ERHIAccess::ComputeShaderRead);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Mixed, Buffer, 16, 16,
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		FRDGBuilderTestAccessor::UseBuffer(Builder, Mixed, Buffer, 32, 16,
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		Builder.MarkPassRoot(Mixed);
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetPasses().size(), 2u);
		ASSERT_EQ(Builder.GetDependencies().size(), 1u);
		EXPECT_EQ(Builder.GetDependencies()[0].Kind, ERDGDependencyKind::Value);
		const auto& Barriers = Builder.GetPasses()[1].Barriers.GetBufferTransitions();
		ASSERT_EQ(Barriers.size(), 1u);
		EXPECT_EQ(Barriers[0].RequiredAfter,
			ERHIAccess::ComputeShaderRead | ERHIAccess::ComputeShaderReadWrite);
		EXPECT_EQ(Barriers[0].Offset, 0u);
		EXPECT_EQ(Barriers[0].Size, 64u);
		EXPECT_FALSE(Barriers[0].bDiscardContents);
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Uses.size(), 4u);
		for (uint32 Index = 1; Index < 4; ++Index)
		{
			EXPECT_EQ(Capture.Uses[Index].BufferOffset, (Index - 1) * 16u);
			EXPECT_EQ(Capture.Uses[Index].BufferSize, 16u);
			EXPECT_EQ(Capture.Uses[Index].Version, 2u);
		}
		ExpectCapturedBarriersMatchPlan(Builder);
	}

	TEST_F(FRDGTests, FixedTextureLayoutFinalizesOnlyRetainedUsedSubresources)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Texture = Builder.RegisterExternalTexture(MakeGraphTexture("Imported", 4),
			"Imported", ERHIAccess::GraphicsShaderRead, ERHIAccess::TransferRead);
		const auto Used = FRDGBuilderTestAccessor::AddPass(Builder, "Used", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Used, Texture,
			{ERHITextureAspect::Color, 1, 1, 0, 1}, ERDGUse::Read, ERHIAccess::ComputeShaderRead);
		Builder.MarkPassRoot(Used);
		const auto Culled = FRDGBuilderTestAccessor::AddPass(Builder, "Culled", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseTexture(Builder, Culled, Texture,
			{ERHITextureAspect::Color, 3, 1, 0, 1}, ERDGUse::Read, ERHIAccess::ComputeShaderRead);
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetFinalBarriers().GetTextureTransitions().size(), 1u);
		EXPECT_EQ(Builder.GetFinalBarriers().GetTextureTransitions()[0].Range.FirstMip, 1u);
		ASSERT_EQ(Builder.GetExecutionPlan().Handoffs.size(), 2u);
		EXPECT_TRUE(Builder.GetExecutionPlan().Handoffs[0].Producers.empty());
		EXPECT_EQ(Builder.GetExecutionPlan().Handoffs[1].Producers,
			(std::vector<FRDGSubmissionId>{{0}}));
		ExpectCapturedBarriersMatchPlan(Builder);
	}

	TEST_F(FRDGTests, IndexedNameValidationPreservesDuplicateErrors)
	{
		FRDGBuilder Resources;
		Resources.CreateToken("Duplicate");
		Resources.CreateToken("Between");
		Resources.CreateToken("Duplicate");
		EXPECT_EQ(FRDGBuilderTestAccessor::Compile(Resources).Result.Message,
			"duplicate resource name 'Duplicate'");
		FRDGBuilder Passes;
		FRDGBuilderTestAccessor::AddPass(Passes, "Duplicate", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::AddPass(Passes, "Between", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::AddPass(Passes, "Duplicate", ERDGPassType::Compute);
		EXPECT_EQ(FRDGBuilderTestAccessor::Compile(Passes).Result.Message,
			"duplicate pass name 'Duplicate'");
	}

	TEST_F(FRDGTests, ReaderFanoutRetainsEveryExecutionDependency)
	{
		FRDGBuilder Builder;
		const auto Token = Builder.CreateToken("Fanout");
		const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseToken(Builder, Write, Token, ERDGUse::Write);
		for (uint32 Index = 0; Index < 128; ++Index)
		{
			const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read" + std::to_string(Index), ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseToken(Builder, Read, Token, ERDGUse::Read);
		}
		const auto Overwrite = FRDGBuilderTestAccessor::AddPass(Builder, "Overwrite", ERDGPassType::Compute);
		FRDGBuilderTestAccessor::UseToken(Builder, Overwrite, Token, ERDGUse::Write);
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_FALSE(FRDGBuilderTestAccessor::HasDiagnostics(Builder));
		const auto Dependencies = Builder.GetDependencies();
		EXPECT_EQ(Dependencies.size(), 256u);
		for (uint32 Index = 1; Index <= 128; ++Index)
		{
			EXPECT_TRUE(std::ranges::any_of(Dependencies, [&](const auto& Edge) {
				return Edge.BeforePass == 0 && Edge.AfterPass == Index && Edge.Kind == ERDGDependencyKind::Value;
			}));
			EXPECT_TRUE(std::ranges::any_of(Dependencies, [&](const auto& Edge) {
				return Edge.BeforePass == Index && Edge.AfterPass == 129 && Edge.Kind == ERDGDependencyKind::Execution;
			}));
		}
	}

	TEST_F(FRDGTests, CaptureOwnsPointerFreeDiagnosticsBeyondGraphLifetime)
	{
		FRDGCapture Capture;
		{
			FRDGBuilder Builder;
			Builder.EnablePassCulling();
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"Value", "scene-result");
			auto Write = Builder.AllocParameters<FTypedValueWriteParameters>();
			Write->Output = {Value};
			FRDGBuilderTestAccessor::AddPass(Builder, "Produce", ERDGPassType::Compute,
				std::move(Write));
			auto Read = Builder.AllocParameters<FTypedValueReadParameters>();
			Read->Input = {Value};
			const auto Consume = FRDGBuilderTestAccessor::AddPass(Builder, "Consume",
				ERDGPassType::Graphics, std::move(Read));
			Builder.MarkPassRoot(Consume, "present");
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			Capture = Builder.Capture();
			EXPECT_EQ(Capture.Dump, Builder.Dump());
		}
		ASSERT_EQ(Capture.Passes.size(), 2u);
		EXPECT_EQ(Capture.Passes[0].Name, "Produce");
		EXPECT_EQ(Capture.Statistics.DeclaredPasses, 2u);
		EXPECT_EQ(Capture.Statistics.ScheduledPasses, 2u);
		EXPECT_EQ(Capture.Statistics.Dependencies, 1u);
		EXPECT_EQ(Capture.Dependencies[0].Cause, "Value");
		ASSERT_EQ(Capture.Parameters.size(), 2u);
		EXPECT_EQ(Capture.Parameters[0].FieldPath,
			"FTypedValueWriteParameters.Output");
		EXPECT_EQ(Capture.Parameters[0].Kind,
			ERDGParameterMemberKind::ValueWrite);
		EXPECT_EQ(Capture.Parameters[1].FieldPath,
			"FTypedValueReadParameters.Input");
		EXPECT_NE(Capture.Dump.find("name=Consume"), std::string::npos);
	}

	TEST_F(FRDGTests, TypedValuesReuseTokenDependencyAndCullingSemantics)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Value = Builder.CreateValue<FTypedValuePayload>(
			"Scene.Result", "scene-result");
		bool bProduced = false;
		bool bConsumed = false;
		const auto Produce = FRDGBuilderTestAccessor::AddPass(Builder, "Produce",
			ERDGPassType::Compute,
			[Value, &bProduced](FRHICommandListImmediate&,
				const FRDGPassResources& Resources) {
				auto& Payload = Resources.WriteValue(Value);
				EXPECT_EQ(reinterpret_cast<uintptr_t>(&Payload) % alignof(
					FTypedValuePayload), 0u);
				Payload.Value = 41;
				bProduced = true;
			});
		FRDGBuilderTestAccessor::UseValue(Builder, Produce, Value, ERDGUse::Write);
		const auto Consume = FRDGBuilderTestAccessor::AddPass(Builder, "Consume",
			ERDGPassType::Graphics,
			[Value, &bConsumed](FRHICommandListImmediate&,
				const FRDGPassResources& Resources) {
				EXPECT_EQ(Resources.ReadValue(Value).Value, 41);
				bConsumed = true;
			});
		FRDGBuilderTestAccessor::UseValue(Builder, Consume, Value, ERDGUse::Read);
		Builder.MarkPassRoot(Consume, "publish");

		const auto Result = Builder.Execute(GetCommandList());
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
		ASSERT_EQ(Builder.GetDependencies().size(), 1u);
		EXPECT_EQ(Builder.GetDependencies()[0].Kind,
			ERDGDependencyKind::Value);
		EXPECT_EQ(Builder.GetDependencies()[0].Cause, "Scene.Result");
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Resources.size(), 1u);
		EXPECT_EQ(Capture.Resources[0].ValueType, "scene-result");
		EXPECT_EQ(Capture.Uses.size(), 2u);
		EXPECT_TRUE(Result.IsSuccess()) << Result.Result.Message;
		EXPECT_TRUE(bProduced);
		EXPECT_TRUE(bConsumed);
	}

	TEST_F(FRDGTests, ParameterizedTypedValuesExposeExactCapabilities)
	{
		FRDGBuilder Builder;
		const auto Value = Builder.CreateValue<FTypedValuePayload>(
			"Scene.ParameterResult", "scene-result");
		auto Write = Builder.AllocParameters<FTypedValueWriteParameters>();
		Write->Output = {Value};
		Builder.AddPass("Write", ERDGPassType::Compute,
			std::move(Write), [](FRHICommandListImmediate&,
				const FTypedValueWriteParameters& Parameters,
				const FRDGParameterResolver& Resolver) {
				Resolver.WriteValue(Parameters.Output).Value = 73;
			});
		auto Read = Builder.AllocParameters<FTypedValueReadParameters>();
		Read->Input = {Value};
		const auto ReadPass = Builder.AddPass("Read",
			ERDGPassType::Graphics, std::move(Read),
			[](FRHICommandListImmediate&,
				const FTypedValueReadParameters& Parameters,
				const FRDGParameterResolver& Resolver) {
				EXPECT_EQ(Resolver.ReadValue(Parameters.Input).Value, 73);
			});
		Builder.MarkPassRoot(ReadPass, "publish");

		const auto Result = Builder.Execute(GetCommandList());
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Uses.size(), 2u);
		ASSERT_EQ(Capture.Parameters.size(), 2u);
		EXPECT_EQ(Capture.Parameters[0].Kind,
			ERDGParameterMemberKind::ValueWrite);
		EXPECT_EQ(Capture.Parameters[0].ResourceId, Capture.Uses[0].ResourceId);
		EXPECT_EQ(Capture.Parameters[1].Kind,
			ERDGParameterMemberKind::ValueRead);
		EXPECT_EQ(Capture.Uses[0].ParameterPath,
			"FTypedValueWriteParameters.Output");
		EXPECT_EQ(Capture.Uses[1].ParameterPath,
			"FTypedValueReadParameters.Input");
		EXPECT_TRUE(Result.IsSuccess()) << Result.Result.Message;
	}

	TEST_F(FRDGTests, TypedValuesRejectInvalidWriterAndTypeContracts)
	{
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<int>(
				"MissingWriter", "signed-int", 0);
			const auto Read = FRDGBuilderTestAccessor::AddPass(Builder, "Read",
				ERDGPassType::Graphics);
			FRDGBuilderTestAccessor::UseValue(Builder, Read, Value, ERDGUse::Read);
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_EQ(Result.Result.Message, "typed value 'MissingWriter' type 'signed-int' "
				"requires exactly one writer; actual=0");
		}
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<int>(
				"DuplicateWriter", "signed-int", 0);
			for (const char* Name : {"First", "Second"})
			{
				const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, Name,
					ERDGPassType::Compute);
				FRDGBuilderTestAccessor::UseValue(Builder, Pass, Value, ERDGUse::Write);
			}
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_EQ(Result.Result.Message, "typed value 'DuplicateWriter' type 'signed-int' "
				"requires exactly one writer; actual=2");
		}
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<int>(
				"WrongType", "signed-int", 0);
			const auto Wrong = std::bit_cast<TRDGValueHandle<float>>(Value);
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write",
				ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseValue(Builder, Pass, Wrong, ERDGUse::Write);
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_EQ(Result.Result.Message, "pass 'Write' declares an invalid, foreign, or "
				"wrongly typed graph value");
		}
	}

	TEST_F(FRDGTests, TypedValueStorageRemainsOwnedAndDestroysExactlyOnce)
	{
		int BuilderDestructions = 0;
		{
			FRDGBuilder Builder;
			Builder.CreateValue<FTypedValuePayload>("BuilderOwned", "tracked",
				&BuilderDestructions);
		}
		EXPECT_EQ(BuilderDestructions, 1);

		int CompileFailureDestructions = 0;
		{
			FRDGBuilder Builder;
			Builder.CreateValue<FTypedValuePayload>("CompileFailure", "tracked",
				&CompileFailureDestructions);
			EXPECT_FALSE(FRDGBuilderTestAccessor::Compile(Builder).IsSuccess());
			EXPECT_EQ(CompileFailureDestructions, 0);
		}
		EXPECT_EQ(CompileFailureDestructions, 1);

		int GraphDestructions = 0;
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"GraphOwned", "tracked",
				&GraphDestructions);
			const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write",
				ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseValue(Builder, Write, Value, ERDGUse::Write);
			auto Result = Builder.Execute(GetCommandList());
			ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			EXPECT_EQ(GraphDestructions, 0);
			EXPECT_EQ(GraphDestructions, 0);
		}
		EXPECT_EQ(GraphDestructions, 1);

		int CulledDestructions = 0;
		{
			FRDGBuilder Builder;
			Builder.EnablePassCulling();
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"Culled", "tracked", &CulledDestructions);
			const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "CulledWrite",
				ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseValue(Builder, Write, Value, ERDGUse::Write);
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			EXPECT_TRUE(Builder.GetPasses().empty());
			EXPECT_EQ(CulledDestructions, 0);
		}
		EXPECT_EQ(CulledDestructions, 1);

		int AllocationFailureDestructions = 0;
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"AllocationFailure", "tracked",
				&AllocationFailureDestructions);
			const auto Buffer = Builder.CreateBuffer(FRDGBufferDesc{
				.Buffer = FRHIBufferDesc(
					64, 4, EBufferUsageFlags::UnorderedAccess)},
				"AllocationFailure.Buffer");
			const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write",
				ERDGPassType::Compute);
			FRDGBuilderTestAccessor::UseValue(Builder, Write, Value, ERDGUse::Write);
			FRDGBuilderTestAccessor::UseBuffer(Builder, Write, Buffer, 0, 64, ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
			Builder.MarkPassRoot(Write, "publish");

			FTestRDGAllocator Allocator;
			Allocator.bFail = true;
			FRDGExecutionContext Context{Allocator};
			const auto Result = Builder.Execute(GetCommandList(), &Context);
			ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Result.Message;
			std::string Error;
			Error = Result.Result.Message;
			EXPECT_FALSE(Result.IsSuccess()) << Result.Result.Message;
			EXPECT_EQ(Error, "injected allocation failure");
			EXPECT_EQ(AllocationFailureDestructions, 0);
		}
		EXPECT_EQ(AllocationFailureDestructions, 1);
	}

	TEST_F(FRDGTests, TypedValueResolutionRejectsWrongDirectionAndCopies)
	{
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<int>(
				"WrongDirection", "signed-int", 0);
			const auto Write = FRDGBuilderTestAccessor::AddPass(Builder, "Write",
				ERDGPassType::Compute,
				[Value](FRHICommandListImmediate&,
					const FRDGPassResources& Resources) {
					(void)Resources.ReadValue(Value);
				});
			FRDGBuilderTestAccessor::UseValue(Builder, Write, Value, ERDGUse::Write);
			Builder.MarkPassRoot(Write, "publish");

			EXPECT_DEATH(Builder.Execute(GetCommandList()),
				"wrong-direction capability");
		}
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"CopiedParameter", "scene-result");
			auto Parameters =
				Builder.AllocParameters<FTypedValueWriteParameters>();
			Parameters->Output = {Value};
			const auto Write = Builder.AddPass("Write",
				ERDGPassType::Compute, std::move(Parameters),
				[](FRHICommandListImmediate&,
					const FTypedValueWriteParameters& Submitted,
					const FRDGParameterResolver& Resolver) {
					auto Copy = Submitted.Output;
					(void)Resolver.WriteValue(Copy);
				});
			Builder.MarkPassRoot(Write, "publish");

			EXPECT_DEATH(Builder.Execute(GetCommandList()),
				"not declared by the executing pass parameters");
		}
	}

	TEST_F(FRDGTests, PrecompileFallbackSelectionCapturesOnlyChosenImport)
	{
		for (const bool bCandidateReady : {false, true})
		{
			auto Candidate = MakeGraphTexture("Candidate");
			auto Fallback = MakeGraphTexture("Fallback");
			FTextureRHIRef Selected = bCandidateReady ? Candidate : Fallback;
			FRDGBuilder Builder;
			const auto Input = Builder.RegisterExternalTexture(Selected, "Selected.Environment", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<
				FComposedTextureArrayParameters>();
			Parameters->Textures[0] = FRDGTextureParameter{
				Input, WholeColor()};
			Parameters->Textures[1] = std::nullopt;
			const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Consume",
				ERDGPassType::Graphics, std::move(Parameters));
			Builder.MarkPassRoot(Pass, "publish");
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Result.Message;
			const auto Capture = Builder.Capture();
			ASSERT_EQ(Capture.Resources.size(), 1u);
			EXPECT_EQ(Capture.Resources[0].Name, "Selected.Environment");
			EXPECT_EQ(Capture.Uses.size(), 1u);
			ASSERT_EQ(Capture.Parameters.size(), 2u);
			EXPECT_TRUE(Capture.Parameters[0].bPresent);
			EXPECT_EQ(Capture.Parameters[0].ResourceId, 0u);
			EXPECT_EQ(Capture.Parameters[0].ShaderBindingName, "Textures");
			EXPECT_FALSE(Capture.Parameters[1].bPresent);
		}
	}
} // namespace Durin
