#include "RenderGraph.h"

#include "RHICommandList.h"
#include "Shader/Shader.h"

#include <gtest/gtest.h>

#include <chrono>
#include <bit>

namespace Durin
{
	namespace
	{
		class FRenderGraphTests : public testing::Test
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
			TRenderGraphValueWrite<FTypedValuePayload> Output;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array Members{
					MakeRenderGraphValueParameterMemberMetadata<
						FTypedValueWriteParameters, decltype(Output),
						FTypedValuePayload>("Output", offsetof(
							FTypedValueWriteParameters, Output)),
				};
				static const auto Metadata =
					MakeInlineRenderGraphParametersMetadata<
						FTypedValueWriteParameters>(
							"FTypedValueWriteParameters", Members);
				return &Metadata;
			}
		};

		struct FTypedValueReadParameters final
		{
			TRenderGraphValueRead<FTypedValuePayload> Input;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array Members{
					MakeRenderGraphValueParameterMemberMetadata<
						FTypedValueReadParameters, decltype(Input),
						FTypedValuePayload>("Input", offsetof(
							FTypedValueReadParameters, Input)),
				};
				static const auto Metadata =
					MakeInlineRenderGraphParametersMetadata<
						FTypedValueReadParameters>(
							"FTypedValueReadParameters", Members);
				return &Metadata;
			}
		};

		auto WholeColor(uint32 Mips = 1) -> FRHITextureSubresourceRange
		{
			return {ERHITextureAspect::Color, 0, Mips, 0, 1};
		}

		auto MakeGraphTexture(const char* Name, uint8 Mips = 1) -> FRHITexture
		{
			return FRHITexture(FRHITextureCreateDesc::Create2D(
				Name, 64, 64, EPixelFormat::RGBA8_UNORM)
				.SetNumMips(Mips)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::Storage
					| ETextureCreateFlags::SourceCopy
					| ETextureCreateFlags::DestinationCopy));
		}

		class FTestRDGAllocator final : public FRDGAllocator
		{
		public:
			bool bFail = false;
			uint32 AllocationCount = 0;
			std::vector<FTextureRHIRef> CreatedTextures;

			auto Allocate(std::span<const FRDGAllocationRequest> Requests,
				FRDGAllocatedResources& OutResources, std::string& OutError)
				-> bool override
			{
				if (bFail)
				{
					OutError = "injected allocation failure";
					return false;
				}
				for (const FRDGAllocationRequest& Request : Requests)
				{
					++AllocationCount;
					if (Request.Kind == ERenderGraphResourceKind::Texture)
					{
						FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create(
							"TestRDG", Request.TextureDesc.Dimension);
						static_cast<FRHITextureDesc&>(Desc) = Request.TextureDesc;
						auto Texture = MakeRefCount<FRHITexture>(Desc);
						CreatedTextures.push_back(Texture);
						if (!OutResources.SetTexture(Request.ResourceId,
							std::move(Texture), AllocationCount)) return false;
					}
					else
					{
						auto Buffer = MakeRefCount<FRHIBuffer>(
							FRHIBufferCreateDesc::Create("TestRDG",
								Request.BufferDesc));
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
			FRenderGraphTokenParameter Completion;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*;
		};

		auto FNestedGraphParameters::GetRenderGraphParametersMetadata()
			-> const FRenderGraphParametersMetadata*
		{
			static const std::array Members{
				MakeRenderGraphResourceParameterMemberMetadata<
					FNestedGraphParameters, decltype(Completion),
					FRenderGraphTokenParameter>("Completion",
						offsetof(FNestedGraphParameters, Completion),
						ERenderGraphParameterMemberKind::Token,
						ERenderGraphResourceKind::Token,
						ERenderGraphParameterRangeKind::None,
						ERenderGraphUse::Write, ERHIAccess::None, true),
			};
			static const auto Metadata =
				MakeInlineRenderGraphParametersMetadata<FNestedGraphParameters>(
					"FNestedGraphParameters", Members);
			return &Metadata;
		}

		struct alignas(64) FGraphParameterLayoutFixture final
		{
			FRenderGraphTextureParameter Input;
			std::array<std::optional<FRenderGraphBufferParameter>, 2> Buffers;
			std::optional<FRenderGraphColorAttachmentParameter> Color;
			FRenderGraphDepthStencilAttachmentParameter Depth;
			FRenderGraphManagedTextureParameter Managed;
			FNestedGraphParameters Nested;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*;
		};

		auto FGraphParameterLayoutFixture::GetRenderGraphParametersMetadata()
			-> const FRenderGraphParametersMetadata*
		{
			static const std::array Members{
				MakeRenderGraphResourceParameterMemberMetadata<
					FGraphParameterLayoutFixture, decltype(Input),
					FRenderGraphTextureParameter>("Input",
						offsetof(FGraphParameterLayoutFixture, Input),
						ERenderGraphParameterMemberKind::Texture,
						ERenderGraphResourceKind::Texture,
						ERenderGraphParameterRangeKind::TextureSubresource,
						ERenderGraphUse::Read,
						ERHIAccess::GraphicsShaderRead),
				MakeRenderGraphResourceParameterMemberMetadata<
					FGraphParameterLayoutFixture, decltype(Buffers),
					FRenderGraphBufferParameter>("Buffers",
						offsetof(FGraphParameterLayoutFixture, Buffers),
						ERenderGraphParameterMemberKind::Buffer,
						ERenderGraphResourceKind::Buffer,
						ERenderGraphParameterRangeKind::BufferBytes,
						ERenderGraphUse::ReadWrite,
						ERHIAccess::ComputeShaderReadWrite),
				MakeRenderGraphResourceParameterMemberMetadata<
					FGraphParameterLayoutFixture, decltype(Color),
					FRenderGraphColorAttachmentParameter>("Color",
						offsetof(FGraphParameterLayoutFixture, Color),
						ERenderGraphParameterMemberKind::ManagedColorAttachment,
						ERenderGraphResourceKind::Texture,
						ERenderGraphParameterRangeKind::TextureSubresource,
						ERenderGraphUse::ReadWrite,
						ERHIAccess::ColorAttachmentReadWrite, true,
						ERHIRenderTargetLoadAction::Clear,
						ERHIRenderTargetStoreAction::Store, true,
						ERHIAccess::GraphicsShaderRead),
				MakeRenderGraphResourceParameterMemberMetadata<
					FGraphParameterLayoutFixture, decltype(Depth),
					FRenderGraphDepthStencilAttachmentParameter>("Depth",
						offsetof(FGraphParameterLayoutFixture, Depth),
						ERenderGraphParameterMemberKind::DepthStencilAttachment,
						ERenderGraphResourceKind::Texture,
						ERenderGraphParameterRangeKind::TextureSubresource,
						ERenderGraphUse::ReadWrite,
						ERHIAccess::DepthStencilReadWrite, false,
						ERHIRenderTargetLoadAction::Load,
						ERHIRenderTargetStoreAction::Store),
				MakeRenderGraphResourceParameterMemberMetadata<
					FGraphParameterLayoutFixture, decltype(Managed),
					FRenderGraphManagedTextureParameter>("Managed",
						offsetof(FGraphParameterLayoutFixture, Managed),
						ERenderGraphParameterMemberKind::ManagedTexture,
						ERenderGraphResourceKind::Texture,
						ERenderGraphParameterRangeKind::TextureSubresource,
						ERenderGraphUse::Write,
						ERHIAccess::GraphicsShaderReadWrite, true,
						ERHIRenderTargetLoadAction::Load,
						ERHIRenderTargetStoreAction::Store, true,
						ERHIAccess::GraphicsShaderRead),
				MakeRenderGraphNestedParameterMemberMetadata<
					FGraphParameterLayoutFixture, decltype(Nested)>("Nested",
						offsetof(FGraphParameterLayoutFixture, Nested),
						FNestedGraphParameters::GetRenderGraphParametersMetadata()),
			};
			static const auto Metadata =
				MakeInlineRenderGraphParametersMetadata<FGraphParameterLayoutFixture>(
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

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array<FRenderGraphParameterMemberMetadata, 0> Members{};
				static const auto Metadata =
					MakeInlineRenderGraphParametersMetadata<FFirstLifetimeGraphParameters>(
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

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array<FRenderGraphParameterMemberMetadata, 0> Members{};
				static const auto Metadata =
					MakeInlineRenderGraphParametersMetadata<FSecondLifetimeGraphParameters>(
						"FSecondLifetimeGraphParameters", Members);
				return &Metadata;
			}
		};

		struct FMalformedGraphParameters final
		{
			FRenderGraphTextureParameter Texture;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array Members{
					FRenderGraphParameterMemberMetadata{
						.Name = "Texture",
						.Offset = static_cast<uint32>(sizeof(FMalformedGraphParameters)),
						.ElementSize = static_cast<uint32>(sizeof(Texture)),
						.Kind = ERenderGraphParameterMemberKind::Texture,
						.ResourceKind = ERenderGraphResourceKind::Texture,
						.RangeKind = ERenderGraphParameterRangeKind::TextureSubresource,
						.Access = ERHIAccess::GraphicsShaderRead,
					},
				};
				static const auto Metadata =
					MakeInlineRenderGraphParametersMetadata<FMalformedGraphParameters>(
						"FMalformedGraphParameters", Members);
				return &Metadata;
			}
		};

		struct FAllGraphUseParameters final
		{
			std::array<std::optional<FRenderGraphTextureParameter>, 2> Inputs;
			FRenderGraphBufferParameter Buffer;
			FRenderGraphColorAttachmentParameter Color;
			FRenderGraphDepthStencilAttachmentParameter Depth;
			FRenderGraphColorAttachmentParameter ManagedColor;
			std::optional<FRenderGraphDepthStencilAttachmentParameter> ManagedDepth;
			FRenderGraphManagedTextureParameter ManagedTexture;
			FNestedGraphParameters Nested;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*;
		};

		auto FAllGraphUseParameters::GetRenderGraphParametersMetadata()
			-> const FRenderGraphParametersMetadata*
		{
			static const std::array Members{
				MakeRenderGraphResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(Inputs),
					FRenderGraphTextureParameter>("Inputs",
						offsetof(FAllGraphUseParameters, Inputs),
						ERenderGraphParameterMemberKind::Texture,
						ERenderGraphResourceKind::Texture,
						ERenderGraphParameterRangeKind::TextureSubresource,
						ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
				MakeRenderGraphResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(Buffer),
					FRenderGraphBufferParameter>("Buffer",
						offsetof(FAllGraphUseParameters, Buffer),
						ERenderGraphParameterMemberKind::Buffer,
						ERenderGraphResourceKind::Buffer,
						ERenderGraphParameterRangeKind::BufferBytes,
						ERenderGraphUse::ReadWrite,
						ERHIAccess::GraphicsShaderReadWrite),
				MakeRenderGraphResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(Color),
					FRenderGraphColorAttachmentParameter>("Color",
						offsetof(FAllGraphUseParameters, Color),
						ERenderGraphParameterMemberKind::ColorAttachment,
						ERenderGraphResourceKind::Texture,
						ERenderGraphParameterRangeKind::TextureSubresource,
						ERenderGraphUse::ReadWrite,
						ERHIAccess::ColorAttachmentReadWrite, false,
						ERHIRenderTargetLoadAction::Clear,
						ERHIRenderTargetStoreAction::Store),
				MakeRenderGraphResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(Depth),
					FRenderGraphDepthStencilAttachmentParameter>("Depth",
						offsetof(FAllGraphUseParameters, Depth),
						ERenderGraphParameterMemberKind::DepthStencilAttachment,
						ERenderGraphResourceKind::Texture,
						ERenderGraphParameterRangeKind::TextureSubresource,
						ERenderGraphUse::ReadWrite,
						ERHIAccess::DepthStencilReadWrite, false,
						ERHIRenderTargetLoadAction::Clear,
						ERHIRenderTargetStoreAction::Store),
				MakeRenderGraphResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(ManagedColor),
					FRenderGraphColorAttachmentParameter>("ManagedColor",
						offsetof(FAllGraphUseParameters, ManagedColor),
						ERenderGraphParameterMemberKind::ManagedColorAttachment,
						ERenderGraphResourceKind::Texture,
						ERenderGraphParameterRangeKind::TextureSubresource,
						ERenderGraphUse::ReadWrite,
						ERHIAccess::ColorAttachmentReadWrite, false,
						ERHIRenderTargetLoadAction::Clear,
						ERHIRenderTargetStoreAction::Store, true,
						ERHIAccess::GraphicsShaderRead),
				MakeRenderGraphResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(ManagedDepth),
					FRenderGraphDepthStencilAttachmentParameter>("ManagedDepth",
						offsetof(FAllGraphUseParameters, ManagedDepth),
						ERenderGraphParameterMemberKind::ManagedDepthStencilAttachment,
						ERenderGraphResourceKind::Texture,
						ERenderGraphParameterRangeKind::TextureSubresource,
						ERenderGraphUse::ReadWrite,
						ERHIAccess::DepthStencilReadWrite, false,
						ERHIRenderTargetLoadAction::Clear,
						ERHIRenderTargetStoreAction::Store, true,
						ERHIAccess::GraphicsShaderRead),
				MakeRenderGraphResourceParameterMemberMetadata<
					FAllGraphUseParameters, decltype(ManagedTexture),
					FRenderGraphManagedTextureParameter>("ManagedTexture",
						offsetof(FAllGraphUseParameters, ManagedTexture),
						ERenderGraphParameterMemberKind::ManagedTexture,
						ERenderGraphResourceKind::Texture,
						ERenderGraphParameterRangeKind::TextureSubresource,
						ERenderGraphUse::Write,
						ERHIAccess::GraphicsShaderReadWrite, true,
						ERHIRenderTargetLoadAction::Load,
						ERHIRenderTargetStoreAction::Store, true,
						ERHIAccess::GraphicsShaderRead),
				MakeRenderGraphNestedParameterMemberMetadata<
					FAllGraphUseParameters, decltype(Nested)>("Nested",
						offsetof(FAllGraphUseParameters, Nested),
						FNestedGraphParameters::GetRenderGraphParametersMetadata()),
			};
			static const auto Metadata =
				MakeInlineRenderGraphParametersMetadata<FAllGraphUseParameters>(
					"FAllGraphUseParameters", Members);
			return &Metadata;
		}

		struct FTwoTextureGraphParameters final
		{
			std::array<FRenderGraphTextureParameter, 2> Textures;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array Members{
					MakeRenderGraphResourceParameterMemberMetadata<
						FTwoTextureGraphParameters, decltype(Textures),
						FRenderGraphTextureParameter>("Textures",
							offsetof(FTwoTextureGraphParameters, Textures),
							ERenderGraphParameterMemberKind::Texture,
							ERenderGraphResourceKind::Texture,
							ERenderGraphParameterRangeKind::TextureSubresource,
							ERenderGraphUse::Read,
							ERHIAccess::GraphicsShaderRead),
				};
				static const auto Metadata =
					MakeInlineRenderGraphParametersMetadata<FTwoTextureGraphParameters>(
						"FTwoTextureGraphParameters", Members);
				return &Metadata;
			}
		};

		struct FComposedTextureArrayParameters final
		{
			std::array<std::optional<FRenderGraphTextureParameter>, 2> Textures;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array Members{
					MakeRenderGraphShaderResourceParameterMemberMetadata<
						FComposedTextureArrayParameters, decltype(Textures),
						FRenderGraphTextureParameter>("Textures",
							offsetof(FComposedTextureArrayParameters, Textures),
							ERenderGraphParameterMemberKind::Texture,
							ERenderGraphResourceKind::Texture,
							ERenderGraphParameterRangeKind::TextureSubresource,
							ERenderGraphUse::Read,
							ERHIAccess::GraphicsShaderRead,
							ERHIBindingType::Texture),
				};
				static const auto Metadata =
					MakeInlineRenderGraphParametersMetadata<
						FComposedTextureArrayParameters>(
							"FComposedTextureArrayParameters", Members);
				return &Metadata;
			}
		};

		struct FMalformedComposedAccessParameters final
		{
			FRenderGraphTextureParameter Texture;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array Members{
					MakeRenderGraphShaderResourceParameterMemberMetadata<
						FMalformedComposedAccessParameters, decltype(Texture),
						FRenderGraphTextureParameter>("Texture",
							offsetof(FMalformedComposedAccessParameters, Texture),
							ERenderGraphParameterMemberKind::Texture,
							ERenderGraphResourceKind::Texture,
							ERenderGraphParameterRangeKind::TextureSubresource,
							ERenderGraphUse::Read,
							ERHIAccess::GraphicsShaderRead,
							ERHIBindingType::StorageImage),
				};
				static const auto Metadata =
					MakeInlineRenderGraphParametersMetadata<
						FMalformedComposedAccessParameters>(
							"FMalformedComposedAccessParameters", Members);
				return &Metadata;
			}
		};

		struct FComposedComputeBufferParameters final
		{
			FRenderGraphBufferParameter InputBuffer;
			FRenderGraphBufferParameter OutputBuffer;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array Members{
					MakeRenderGraphShaderResourceParameterMemberMetadata<
						FComposedComputeBufferParameters, decltype(InputBuffer),
						FRenderGraphBufferParameter>("InputBuffer",
							offsetof(FComposedComputeBufferParameters, InputBuffer),
							ERenderGraphParameterMemberKind::Buffer,
							ERenderGraphResourceKind::Buffer,
							ERenderGraphParameterRangeKind::BufferBytes,
							ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead,
							ERHIBindingType::StorageBuffer),
					MakeRenderGraphShaderResourceParameterMemberMetadata<
						FComposedComputeBufferParameters, decltype(OutputBuffer),
						FRenderGraphBufferParameter>("OutputBuffer",
							offsetof(FComposedComputeBufferParameters, OutputBuffer),
							ERenderGraphParameterMemberKind::Buffer,
							ERenderGraphResourceKind::Buffer,
							ERenderGraphParameterRangeKind::BufferBytes,
							ERenderGraphUse::Write,
							ERHIAccess::ComputeShaderReadWrite,
							ERHIBindingType::StorageBuffer, nullptr, true),
				};
				static const auto Metadata =
					MakeInlineRenderGraphParametersMetadata<
						FComposedComputeBufferParameters>(
							"FComposedComputeBufferParameters", Members);
				return &Metadata;
			}
		};

		struct FLargeTokenGraphParameters final
		{
			std::array<FRenderGraphTokenParameter, 128> Tokens;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array Members{
					MakeRenderGraphResourceParameterMemberMetadata<
						FLargeTokenGraphParameters, decltype(Tokens),
						FRenderGraphTokenParameter>("Tokens",
							offsetof(FLargeTokenGraphParameters, Tokens),
							ERenderGraphParameterMemberKind::Token,
							ERenderGraphResourceKind::Token,
							ERenderGraphParameterRangeKind::None,
							ERenderGraphUse::Write, ERHIAccess::None, true),
				};
				static const auto Metadata =
					MakeInlineRenderGraphParametersMetadata<FLargeTokenGraphParameters>(
						"FLargeTokenGraphParameters", Members);
				return &Metadata;
			}
		};

		struct FComputeResolutionParameters final
		{
			FRenderGraphTextureParameter Texture;
			FRenderGraphBufferParameter Buffer;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array Members{
					MakeRenderGraphResourceParameterMemberMetadata<
						FComputeResolutionParameters, decltype(Texture),
						FRenderGraphTextureParameter>("Texture",
							offsetof(FComputeResolutionParameters, Texture),
							ERenderGraphParameterMemberKind::Texture,
							ERenderGraphResourceKind::Texture,
							ERenderGraphParameterRangeKind::TextureSubresource,
							ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead),
					MakeRenderGraphResourceParameterMemberMetadata<
						FComputeResolutionParameters, decltype(Buffer),
						FRenderGraphBufferParameter>("Buffer",
							offsetof(FComputeResolutionParameters, Buffer),
							ERenderGraphParameterMemberKind::Buffer,
							ERenderGraphResourceKind::Buffer,
							ERenderGraphParameterRangeKind::BufferBytes,
							ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead),
				};
				static const auto Metadata = MakeInlineRenderGraphParametersMetadata<
					FComputeResolutionParameters>("FComputeResolutionParameters", Members);
				return &Metadata;
			}
		};

		struct FUnavailableBufferParameters final
		{
			FRenderGraphBufferParameter Buffer;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array Members{
					MakeRenderGraphResourceParameterMemberMetadata<
						FUnavailableBufferParameters, decltype(Buffer),
						FRenderGraphBufferParameter>("Buffer",
							offsetof(FUnavailableBufferParameters, Buffer),
							ERenderGraphParameterMemberKind::Buffer,
							ERenderGraphResourceKind::Buffer,
							ERenderGraphParameterRangeKind::BufferBytes,
							ERenderGraphUse::Write,
							ERHIAccess::ComputeShaderReadWrite, true),
				};
				static const auto Metadata = MakeInlineRenderGraphParametersMetadata<
					FUnavailableBufferParameters>("FUnavailableBufferParameters", Members);
				return &Metadata;
			}
		};

		struct FCopyResolutionParameters final
		{
			FRenderGraphTextureParameter Texture;

			static auto GetRenderGraphParametersMetadata()
				-> const FRenderGraphParametersMetadata*
			{
				static const std::array Members{
					MakeRenderGraphResourceParameterMemberMetadata<
						FCopyResolutionParameters, decltype(Texture),
						FRenderGraphTextureParameter>("Texture",
							offsetof(FCopyResolutionParameters, Texture),
							ERenderGraphParameterMemberKind::Texture,
							ERenderGraphResourceKind::Texture,
							ERenderGraphParameterRangeKind::TextureSubresource,
							ERenderGraphUse::Read, ERHIAccess::TransferRead),
				};
				static const auto Metadata = MakeInlineRenderGraphParametersMetadata<
					FCopyResolutionParameters>("FCopyResolutionParameters", Members);
				return &Metadata;
			}
		};

		template<typename Argument>
		concept CTextureResolverArgument = requires(
			const FRenderGraphParameterResolver& Resolver, const Argument& Value)
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

	TEST_F(FRenderGraphTests, GraphParameterMetadataPreservesStableCompleteLayout)
	{
		const auto* Metadata =
			FGraphParameterLayoutFixture::GetRenderGraphParametersMetadata();
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
			sizeof(std::optional<FRenderGraphBufferParameter>));
		EXPECT_TRUE(Metadata->Members[1].bOptional);
		EXPECT_TRUE(Metadata->Members[2].bOptional);
		EXPECT_EQ(Metadata->Members[2].LoadAction,
			ERHIRenderTargetLoadAction::Clear);
		EXPECT_TRUE(Metadata->Members[2].bPassManagedTransition);
		EXPECT_EQ(Metadata->Members[2].ResultAccess,
			ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Metadata->Members[5].Kind,
			ERenderGraphParameterMemberKind::Nested);
		ASSERT_NE(Metadata->Members[5].NestedParameters, nullptr);
		EXPECT_STREQ(Metadata->Members[5].NestedParameters->Members[0].Name,
			"Completion");
	}

	TEST_F(FRenderGraphTests,
		ComposedGraphMetadataCapturesStableBindingAndExactArrayElements)
	{
		FRHITexture TextureA = MakeGraphTexture("ComposedA", 2);
		FRHITexture TextureB = MakeGraphTexture("ComposedB", 2);
		FRenderGraphBuilder Builder;
		const auto HandleA = Builder.ImportTexture("A", &TextureA,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto HandleB = Builder.ImportTexture("B", &TextureB,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		auto Parameters = Builder.AllocParameters<
			FComposedTextureArrayParameters>();
		Parameters->Textures[0] = FRenderGraphTextureParameter{
			HandleA, {ERHITextureAspect::Color, 0, 1, 0, 1}};
		Parameters->Textures[1] = FRenderGraphTextureParameter{
			HandleB, {ERHITextureAspect::Color, 1, 1, 0, 1}};
		bool bExecuted = false;
		Builder.AddPass("Composed", ERenderGraphPassType::Graphics,
			std::move(Parameters),
			[&](FRHICommandListImmediate&,
				const FComposedTextureArrayParameters& Values,
				const FRenderGraphParameterResolver& Resolver) {
				const auto ShaderParameters = Resolver.GetShaderParameters(Values);
				EXPECT_EQ(ShaderParameters.GetData(), &Values);
				EXPECT_EQ(Resolver.GetTexture(*Values.Textures[0]), &TextureA);
				EXPECT_EQ(Resolver.GetTexture(*Values.Textures[1]), &TextureB);
				bExecuted = true;
			});

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const auto Capture = Result.Graph->Capture();
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
		EXPECT_TRUE(Result.Graph->Execute(GetCommandList()));
		EXPECT_TRUE(bExecuted);
	}

	TEST_F(FRenderGraphTests,
		ComposedGraphMetadataRejectsAccessWeakeningBeforePassPublication)
	{
		FRenderGraphBuilder Builder;
		auto Parameters = Builder.AllocParameters<
			FMalformedComposedAccessParameters>();
		EXPECT_FALSE(Parameters);
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("incompatible graph/shader declaration"),
			std::string::npos);
	}

	TEST_F(FRenderGraphTests,
		ComposedShaderSubmissionRejectsReflectionArrayExtentBeforeRecording)
	{
		FRHITexture TextureA = MakeGraphTexture("BindingExtentA");
		FRHITexture TextureB = MakeGraphTexture("BindingExtentB");
		FRenderGraphBuilder Builder;
		const auto HandleA = Builder.ImportTexture("TextureA", &TextureA,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto HandleB = Builder.ImportTexture("TextureB", &TextureB,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		auto Parameters = Builder.AllocParameters<
			FComposedTextureArrayParameters>();
		Parameters->Textures[0] = FRenderGraphTextureParameter{
			HandleA, WholeColor()};
		Parameters->Textures[1] = FRenderGraphTextureParameter{
			HandleB, WholeColor()};
		Builder.AddPass("ComposedExtent", ERenderGraphPassType::Graphics,
			std::move(Parameters),
			[](FRHICommandListImmediate&,
				const FComposedTextureArrayParameters& Values,
				const FRenderGraphParameterResolver& Resolver) {
				FRHICommandList Commands;
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
				auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(
					EShaderFrequency::Fragment, FXxHash128{}));
				const std::array Bindings{FShaderParameterBinding{
					.Name = "Textures", .Type = ERHIBindingType::Texture,
					.ArraySize = 1}};
				const auto GraphShaderParameters =
					Resolver.GetShaderParameters(Values);
				SetRenderGraphShaderParametersImpl(Commands, Shader.GetReference(),
					"FExtentFixture", EShaderFrequency::Fragment, Bindings,
					GraphShaderParameters, nullptr, nullptr);
			});
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
			"array extent does not match");
	}

	TEST_F(FRenderGraphTests,
		ComposedShaderSubmissionRejectsUnavailableRequiredOptional)
	{
		FRenderGraphBuilder Builder;
		auto Parameters = Builder.AllocParameters<
			FComposedTextureArrayParameters>();
		Builder.AddPass("ComposedOptional", ERenderGraphPassType::Graphics,
			std::move(Parameters),
			[](FRHICommandListImmediate&,
				const FComposedTextureArrayParameters& Values,
				const FRenderGraphParameterResolver& Resolver) {
				FRHICommandList Commands;
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
				auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(
					EShaderFrequency::Fragment, FXxHash128{}));
				const std::array Bindings{FShaderParameterBinding{
					.Name = "Textures", .Type = ERHIBindingType::Texture,
					.ArraySize = 2, .bGraphResource = true}};
				const auto GraphShaderParameters =
					Resolver.GetShaderParameters(Values);
				SetRenderGraphShaderParametersImpl(Commands, Shader.GetReference(),
					"FOptionalFixture", EShaderFrequency::Fragment, Bindings,
					GraphShaderParameters, nullptr, nullptr);
			});
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
			"is unavailable for required shader");
	}

	TEST_F(FRenderGraphTests,
		ComposedShaderSubmissionRejectsMissingGraphAuthorityAndWrongDomain)
	{
		auto MakeResult = [](bool bWrongDomain) {
			FRenderGraphBuilder Builder;
			auto Parameters = Builder.AllocParameters<
				FComposedTextureArrayParameters>();
			Builder.AddPass("ComposedAuthority", ERenderGraphPassType::Graphics,
				std::move(Parameters),
				[bWrongDomain](FRHICommandListImmediate&,
					const FComposedTextureArrayParameters& Values,
					const FRenderGraphParameterResolver& Resolver) {
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
					SetRenderGraphShaderParametersImpl(Commands,
						Shader.GetReference(), "FAuthorityFixture",
						bWrongDomain ? EShaderFrequency::Compute
							: EShaderFrequency::Fragment,
						Bindings, GraphShaderParameters, nullptr, nullptr);
				});
			return Builder.Compile();
		};
		auto Missing = MakeResult(false);
		ASSERT_TRUE(Missing.IsSuccess()) << Missing.Error;
		EXPECT_DEATH(Missing.Graph->Execute(GetCommandList()),
			"has no composed graph member");
		auto WrongDomain = MakeResult(true);
		ASSERT_TRUE(WrongDomain.IsSuccess()) << WrongDomain.Error;
		EXPECT_DEATH(WrongDomain.Graph->Execute(GetCommandList()),
			"domain is incompatible");
	}

	TEST_F(FRenderGraphTests,
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
		FRenderGraphBuilder Builder;
		const auto Input = Builder.ImportBuffer("Input",
			InputBuffer.GetReference(), ERHIAccess::ComputeShaderRead,
			ERHIAccess::ComputeShaderRead);
		const auto Output = Builder.ImportBuffer("Output",
			OutputBuffer.GetReference(), ERHIAccess::ComputeShaderReadWrite,
			ERHIAccess::ComputeShaderReadWrite);
		auto Parameters = Builder.AllocParameters<
			FComposedComputeBufferParameters>();
		Parameters->InputBuffer = {Input, 16, 32};
		Parameters->OutputBuffer = {Output, 32, 64};
		Builder.AddPass("ComposedBuffers", ERenderGraphPassType::Compute,
			std::move(Parameters),
			[](FRHICommandListImmediate&,
				const FComposedComputeBufferParameters&,
				const FRenderGraphParameterResolver&) {});
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const auto Capture = Result.Graph->Capture();
		ASSERT_EQ(Capture.Uses.size(), 2u);
		EXPECT_EQ(Capture.Uses[0].BufferOffset, 16u);
		EXPECT_EQ(Capture.Uses[0].BufferSize, 32u);
		EXPECT_EQ(Capture.Uses[1].BufferOffset, 32u);
		EXPECT_EQ(Capture.Uses[1].BufferSize, 64u);
	}

	TEST_F(FRenderGraphTests, AllocatesAlignedParametersWithExactRuntimeValues)
	{
		FRenderGraphBuilder Builder;
		auto Parameters = Builder.AllocParameters<FGraphParameterLayoutFixture>();
		ASSERT_TRUE(Parameters.IsValid());
		EXPECT_EQ(reinterpret_cast<uintptr_t>(&Parameters.Get()) % 64u, 0u);

		FRHITexture Texture = MakeGraphTexture("ParameterTexture", 2);
		const auto TextureHandle = Builder.CreateTexture("ParameterTexture", &Texture);
		const auto TokenHandle = Builder.CreateToken("ParameterToken");
		Parameters->Input = {TextureHandle,
			{ERHITextureAspect::Color, 1, 1, 0, 1}};
		Parameters->Buffers[0] = std::nullopt;
		Parameters->Color = FRenderGraphColorAttachmentParameter{
			TextureHandle, WholeColor(2)};
		Parameters->Nested.Completion = {TokenHandle};

		EXPECT_EQ(Parameters->Input.Texture, TextureHandle);
		EXPECT_EQ(Parameters->Input.Range.FirstMip, 1u);
		EXPECT_FALSE(Parameters->Buffers[0].has_value());
		ASSERT_TRUE(Parameters->Color.has_value());
		EXPECT_EQ(Parameters->Color->Range.NumMips, 2u);
		EXPECT_EQ(Parameters->Nested.Completion.Token, TokenHandle);
	}

	TEST_F(FRenderGraphTests, RejectsMalformedGraphParameterMetadataAtomically)
	{
		FRenderGraphBuilder Builder;
		auto Parameters = Builder.AllocParameters<FMalformedGraphParameters>();
		EXPECT_FALSE(Parameters.IsValid());
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_EQ(Result.Error,
			"render graph parameter metadata for 'FMalformedGraphParameters' member "
			"'Texture' has an invalid or unstable offset");
	}

	TEST_F(FRenderGraphTests, DestroysUncompiledParametersExactlyOnceInReverseOrder)
	{
		std::vector<int> DestructionOrder;
		GParameterDestructionOrder = &DestructionOrder;
		{
			FRenderGraphBuilder Builder;
			auto First = Builder.AllocParameters<FFirstLifetimeGraphParameters>();
			auto Second = Builder.AllocParameters<FSecondLifetimeGraphParameters>();
			ASSERT_TRUE(First.IsValid());
			ASSERT_TRUE(Second.IsValid());
		}
		GParameterDestructionOrder = nullptr;
		EXPECT_EQ(DestructionOrder, (std::vector<int>{2, 1}));
	}

	TEST_F(FRenderGraphTests, KeepsParametersWithBuilderAcrossCompileFailure)
	{
		std::vector<int> DestructionOrder;
		GParameterDestructionOrder = &DestructionOrder;
		{
			FRenderGraphBuilder Builder;
			auto Parameters =
				Builder.AllocParameters<FFirstLifetimeGraphParameters>();
			Builder.SetBudget({.MaxPasses = 0});
			Builder.AddPass("Rejected", ERenderGraphPassType::Graphics);
			auto Result = Builder.Compile();
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_TRUE(Parameters.IsValid());
			EXPECT_TRUE(DestructionOrder.empty());
		}
		GParameterDestructionOrder = nullptr;
		EXPECT_EQ(DestructionOrder, (std::vector<int>{1}));
	}

	TEST_F(FRenderGraphTests, TransfersParametersToCompiledGraphLifetime)
	{
		std::vector<int> DestructionOrder;
		GParameterDestructionOrder = &DestructionOrder;
		TRenderGraphParametersRef<FFirstLifetimeGraphParameters> Parameters;
		std::unique_ptr<FCompiledRenderGraph> Graph;
		{
			FRenderGraphBuilder Builder;
			Parameters = Builder.AllocParameters<FFirstLifetimeGraphParameters>();
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			Graph = std::move(Result.Graph);
		}
		EXPECT_TRUE(Parameters.IsValid());
		EXPECT_TRUE(DestructionOrder.empty());
		EXPECT_TRUE(Graph->Execute(GetCommandList()));
		EXPECT_TRUE(DestructionOrder.empty());
		Graph.reset();
		EXPECT_FALSE(Parameters.IsValid());
		GParameterDestructionOrder = nullptr;
		EXPECT_EQ(DestructionOrder, (std::vector<int>{1}));
	}

	TEST_F(FRenderGraphTests, KeepsTransferredParametersAcrossExecutionFailure)
	{
		std::vector<int> DestructionOrder;
		GParameterDestructionOrder = &DestructionOrder;
		FRenderGraphBuilder Builder;
		auto Parameters = Builder.AllocParameters<FFirstLifetimeGraphParameters>();
		Builder.SetBackingResolver([](auto, auto&, std::string&) { return true; });
		const auto Texture = Builder.CreateTexture("MissingBacking",
			FRenderGraphTextureDesc{
				.Texture = FRHITextureCreateDesc::Create2D("MissingBacking", 16, 16,
					EPixelFormat::RGBA8_UNORM),
				.BackingClass = "test"});
		const auto Pass = Builder.AddPass("UseMissingBacking",
			ERenderGraphPassType::Graphics);
		Builder.UseColorAttachment(Pass, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_FALSE(Result.Graph->Execute(GetCommandList()));
		EXPECT_TRUE(Parameters.IsValid());
		EXPECT_TRUE(DestructionOrder.empty());
		Result.Graph.reset();
		EXPECT_FALSE(Parameters.IsValid());
		GParameterDestructionOrder = nullptr;
		EXPECT_EQ(DestructionOrder, (std::vector<int>{1}));
	}

	TEST_F(FRenderGraphTests, ParameterizedPassMatchesEveryManualUseKind)
	{
		auto BuildCapture = [](bool bParameterized) {
			FRHITexture InputTexture = MakeGraphTexture("Input");
			FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
				"Buffer", 256, 4, EBufferUsageFlags::UnorderedAccess));
			FRHITexture ColorTexture = MakeGraphTexture("Color");
			FRHITexture DepthTexture(FRHITextureCreateDesc::Create2D(
				"Depth", 64, 64, EPixelFormat::D32)
				.SetFlags(ETextureCreateFlags::DepthStencilTargetable));
			FRHITexture ManagedColorTexture = MakeGraphTexture("ManagedColor");
			FRHITexture ManagedDepthTexture(FRHITextureCreateDesc::Create2D(
				"ManagedDepth", 64, 64, EPixelFormat::D32)
				.SetFlags(ETextureCreateFlags::DepthStencilTargetable));
			FRHITexture ManagedTexture = MakeGraphTexture("ManagedTexture");

			FRenderGraphBuilder Builder;
			const auto Input = Builder.ImportTexture("Input", &InputTexture,
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			const auto BufferHandle = Builder.ImportBuffer("Buffer", &Buffer,
				ERHIAccess::GraphicsShaderReadWrite,
				ERHIAccess::GraphicsShaderReadWrite);
			const auto Color = Builder.CreateTexture("Color", &ColorTexture);
			const auto Depth = Builder.CreateTexture("Depth", &DepthTexture);
			const auto ManagedColor = Builder.CreateTexture(
				"ManagedColor", &ManagedColorTexture,
				ERHIAccess::GraphicsShaderRead);
			const auto ManagedDepth = Builder.CreateTexture(
				"ManagedDepth", &ManagedDepthTexture,
				ERHIAccess::GraphicsShaderRead);
			const auto Managed = Builder.CreateTexture(
				"ManagedTexture", &ManagedTexture,
				ERHIAccess::GraphicsShaderRead);
			const auto Completion = Builder.CreateToken("Completion");

			if (bParameterized)
			{
				auto Parameters = Builder.AllocParameters<FAllGraphUseParameters>();
				Parameters->Inputs[0] = FRenderGraphTextureParameter{
					Input, WholeColor()};
				Parameters->Inputs[1] = std::nullopt;
				Parameters->Buffer = {BufferHandle, 32, 128};
				Parameters->Color = {Color, WholeColor()};
				Parameters->Depth = {Depth,
					{ERHITextureAspect::Depth, 0, 1, 0, 1}};
				Parameters->ManagedColor = {ManagedColor, WholeColor()};
				Parameters->ManagedDepth =
					FRenderGraphDepthStencilAttachmentParameter{ManagedDepth,
						{ERHITextureAspect::Depth, 0, 1, 0, 1}};
				Parameters->ManagedTexture = {Managed, WholeColor()};
				Parameters->Nested.Completion = {Completion};
				const auto Pass = Builder.AddPass("AllUses",
					ERenderGraphPassType::Graphics, std::move(Parameters));
				EXPECT_TRUE(Pass.IsValid());
			}
			else
			{
				const auto Pass = Builder.AddPass(
					"AllUses", ERenderGraphPassType::Graphics);
				Builder.UseTexture(Pass, Input, WholeColor(),
					ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
				Builder.UseBuffer(Pass, BufferHandle, 32, 128,
					ERenderGraphUse::ReadWrite,
					ERHIAccess::GraphicsShaderReadWrite);
				Builder.UseColorAttachment(Pass, Color, WholeColor(),
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store);
				Builder.UseDepthStencilAttachment(Pass, Depth,
					{ERHITextureAspect::Depth, 0, 1, 0, 1},
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store);
				Builder.UseManagedColorAttachment(Pass, ManagedColor, WholeColor(),
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store,
					ERHIAccess::GraphicsShaderRead);
				Builder.UseManagedDepthStencilAttachment(Pass, ManagedDepth,
					{ERHITextureAspect::Depth, 0, 1, 0, 1},
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store,
					ERHIAccess::GraphicsShaderRead);
				Builder.UseManagedTexture(Pass, Managed, WholeColor(),
					ERenderGraphUse::Write,
					ERHIAccess::GraphicsShaderReadWrite,
					ERHIAccess::GraphicsShaderRead, true);
				Builder.UseToken(Pass, Completion, ERenderGraphUse::Write);
			}

			auto Result = Builder.Compile();
			EXPECT_TRUE(Result.IsSuccess()) << Result.Error;
			return Result.Graph->Capture();
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
			ERenderGraphParameterMemberKind::Token);
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

	TEST_F(FRenderGraphTests, ParameterizedPassRejectsExactInvalidFieldPaths)
	{
		FRHITexture Texture = MakeGraphTexture("Texture");
		FRHITexture ForeignTexture = MakeGraphTexture("ForeignTexture");
		FRenderGraphBuilder ForeignBuilder;
		const auto Foreign = ForeignBuilder.CreateTexture(
			"ForeignTexture", &ForeignTexture);

		{
			FRenderGraphBuilder Builder;
			const auto Local = Builder.ImportTexture("Texture", &Texture,
				ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
			Parameters->Textures = {{{Local, WholeColor()},
				{Foreign, WholeColor()}}};
			EXPECT_FALSE(Builder.AddPass("ForeignHandle",
				ERenderGraphPassType::Graphics, std::move(Parameters)).IsValid());
			auto Result = Builder.Compile();
			EXPECT_EQ(Result.Error,
				"pass 'ForeignHandle' parameter 'FTwoTextureGraphParameters.Textures[1]' "
				"has an invalid resource handle");
		}

		{
			FRenderGraphBuilder Builder;
			const auto Local = Builder.ImportTexture("Texture", &Texture,
				ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
			Parameters->Textures = {{{Local,
				{ERHITextureAspect::Color, 1, 1, 0, 1}},
				{Local, WholeColor()}}};
			EXPECT_FALSE(Builder.AddPass("InvalidRange",
				ERenderGraphPassType::Graphics, std::move(Parameters)).IsValid());
			auto Result = Builder.Compile();
			EXPECT_EQ(Result.Error,
				"pass 'InvalidRange' parameter 'FTwoTextureGraphParameters.Textures[0]' "
				"resource 'Texture' has invalid texture range");
		}

		{
			auto BuildOverlapError = [&] {
				FRenderGraphBuilder Builder;
				const auto Local = Builder.ImportTexture("Texture", &Texture,
					ERHIAccess::GraphicsShaderRead,
					ERHIAccess::GraphicsShaderRead);
				auto Parameters =
					Builder.AllocParameters<FTwoTextureGraphParameters>();
				Parameters->Textures = {{{Local, WholeColor()},
					{Local, {ERHITextureAspect::Color, 0, 1, 0, 1}}}};
				EXPECT_FALSE(Builder.AddPass("Overlap",
					ERenderGraphPassType::Graphics,
					std::move(Parameters)).IsValid());
				return Builder.Compile().Error;
			};
			const std::string Error = BuildOverlapError();
			EXPECT_EQ(Error,
				"pass 'Overlap' parameter 'FTwoTextureGraphParameters.Textures[1]' "
				"declares overlapping uses of resource 'Texture' with parameter "
				"'FTwoTextureGraphParameters.Textures[0]'");
			EXPECT_EQ(BuildOverlapError(), Error);
		}

		{
			FRenderGraphBuilder Builder;
			const auto Local = Builder.ImportTexture("Texture", &Texture,
				ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
			Parameters->Textures = {{{Local, WholeColor()},
				{Local, WholeColor()}}};
			EXPECT_FALSE(Builder.AddPass("WrongDomain",
				ERenderGraphPassType::Compute, std::move(Parameters)).IsValid());
			auto Result = Builder.Compile();
			EXPECT_EQ(Result.Error,
				"pass 'WrongDomain' parameter 'FTwoTextureGraphParameters.Textures[0]' "
				"resource 'Texture' access is incompatible with pass domain");
		}
	}

	TEST_F(FRenderGraphTests, ParameterizedPassRejectsMixedAndConsumedAuthority)
	{
		{
			FRenderGraphBuilder Builder;
			const auto Token = Builder.CreateToken("Token");
			auto Parameters = Builder.AllocParameters<FNestedGraphParameters>();
			Parameters->Completion = {Token};
			const auto Pass = Builder.AddPass("Parameterized",
				ERenderGraphPassType::Graphics, std::move(Parameters));
			ASSERT_TRUE(Pass.IsValid());
			Builder.UseToken(Pass, Token, ERenderGraphUse::Write);
			auto Result = Builder.Compile();
			EXPECT_EQ(Result.Error,
				"pass 'Parameterized' uses parameter declarations and cannot accept manual uses");
		}

		{
			FRenderGraphBuilder Builder;
			const auto Token = Builder.CreateToken("Token");
			auto Parameters = Builder.AllocParameters<FNestedGraphParameters>();
			Parameters->Completion = {Token};
			EXPECT_TRUE(Builder.AddPass("First", ERenderGraphPassType::Graphics,
				std::move(Parameters)).IsValid());
			EXPECT_FALSE(Parameters.IsValid());
			EXPECT_FALSE(Builder.AddPass("Second", ERenderGraphPassType::Graphics,
				std::move(Parameters)).IsValid());
			auto Result = Builder.Compile();
			EXPECT_EQ(Result.Error,
				"pass 'Second' parameter 'FNestedGraphParameters' has an invalid or "
				"foreign parameter allocation");
		}

		{
			FRenderGraphBuilder Owner;
			auto Parameters = Owner.AllocParameters<FNestedGraphParameters>();
			FRenderGraphBuilder Other;
			EXPECT_FALSE(Other.AddPass("ForeignAllocation",
				ERenderGraphPassType::Graphics, std::move(Parameters)).IsValid());
			auto Result = Other.Compile();
			EXPECT_EQ(Result.Error,
				"pass 'ForeignAllocation' parameter 'FNestedGraphParameters' has an "
				"invalid or foreign parameter allocation");
		}
	}

	TEST_F(FRenderGraphTests, ParameterizedDeclarationFailureIsCallbackAtomic)
	{
		FRHITexture Texture = MakeGraphTexture("Texture");
		FRenderGraphBuilder Builder;
		const auto Local = Builder.ImportTexture("Texture", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
		Parameters->Textures = {{{Local,
			{ERHITextureAspect::Color, 4, 1, 0, 1}}, {Local, WholeColor()}}};
		bool bExecuted = false;
		Builder.AddPass("Invalid", ERenderGraphPassType::Graphics,
			std::move(Parameters),
			[&](FRHICommandListImmediate&, const FTwoTextureGraphParameters&,
				const FRenderGraphParameterResolver&) {
				bExecuted = true;
			});
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_FALSE(bExecuted);
	}

	TEST_F(FRenderGraphTests, ParameterTraversalStaysWithinFoundationBudget)
	{
		FRenderGraphBuilder Builder;
		Builder.SetBudget({.MaxCompileMicroseconds = 1'000'000});
		const auto Started = std::chrono::steady_clock::now();
		auto Parameters = Builder.AllocParameters<FLargeTokenGraphParameters>();
		for (uint32 Index = 0; Index < Parameters->Tokens.size(); ++Index)
			Parameters->Tokens[Index] = {
				Builder.CreateToken("Token." + std::to_string(Index))};
		const auto Pass = Builder.AddPass("LargeParameters",
			ERenderGraphPassType::Graphics, std::move(Parameters));
		ASSERT_TRUE(Pass.IsValid());
		const auto DeclarationMicroseconds = std::chrono::duration_cast<
			std::chrono::microseconds>(std::chrono::steady_clock::now() - Started)
			.count();
		EXPECT_LT(DeclarationMicroseconds, 1'000'000);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_EQ(Result.Graph->Capture().Uses.size(), 128u);
		EXPECT_FALSE(Result.Graph->GetStatistics().bCompileBudgetExceeded);
		EXPECT_EQ(Result.Graph->Capture().Uses.back().ParameterPath,
			"FLargeTokenGraphParameters.Tokens[127]");
	}

	TEST_F(FRenderGraphTests, ParameterResolverResolvesDeclaredGraphicsResources)
	{
		FRHITexture InputTexture = MakeGraphTexture("Input");
		FRHITexture ColorTexture = MakeGraphTexture("Color");
		FRHITexture DepthTexture(FRHITextureCreateDesc::Create2D(
			"Depth", 64, 64, EPixelFormat::D32)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable));
		FRHITexture ManagedTexture = MakeGraphTexture("ManagedTexture");
		FRenderGraphBuilder Builder;
		const auto Input = Builder.ImportTexture("Input", &InputTexture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Color = Builder.CreateTexture("Color", &ColorTexture,
			ERHIAccess::GraphicsShaderRead);
		const auto Depth = Builder.ImportTexture("Depth", &DepthTexture,
			ERHIAccess::DepthStencilReadWrite,
			ERHIAccess::DepthStencilReadWrite);
		const auto Managed = Builder.CreateTexture("Managed", &ManagedTexture,
			ERHIAccess::GraphicsShaderRead);
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
		Builder.AddPass("ResolveGraphics", ERenderGraphPassType::Graphics,
			std::move(Parameters),
			[&](FRHICommandListImmediate&,
				const FGraphParameterLayoutFixture& Values,
				const FRenderGraphParameterResolver& Resolver) {
				static_assert(std::is_const_v<std::remove_reference_t<decltype(Values)>>);
				EXPECT_EQ(Resolver.GetTexture(Values.Input), &InputTexture);
				EXPECT_EQ(Resolver.GetBuffer(Values.Buffers[0]), nullptr);
				const auto ColorView = Resolver.GetColorAttachment(Values.Color);
				EXPECT_EQ(ColorView.Texture, &ColorTexture);
				EXPECT_EQ(ColorView.LoadAction, ERHIRenderTargetLoadAction::Clear);
				EXPECT_TRUE(ColorView.bPassManagedTransition);
				const auto DepthView = Resolver.GetDepthStencilAttachment(Values.Depth);
				EXPECT_EQ(DepthView.Texture, &DepthTexture);
				EXPECT_EQ(Resolver.GetTexture(Values.Managed), &ManagedTexture);
				++CallbackCount;
			});
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_TRUE(Result.Graph->Execute(GetCommandList()));
		EXPECT_EQ(CallbackCount, 1u);
	}

	TEST_F(FRenderGraphTests, ParameterResolverSupportsComputeAndCopyDomains)
	{
		FRHITexture ComputeTexture = MakeGraphTexture("Compute");
		FRHITexture CopyTexture = MakeGraphTexture("Copy");
		FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
			"Buffer", 64, 4, EBufferUsageFlags::UnorderedAccess));
		FRenderGraphBuilder Builder;
		const auto ComputeTextureHandle = Builder.ImportTexture("Compute",
			&ComputeTexture, ERHIAccess::ComputeShaderRead,
			ERHIAccess::ComputeShaderRead);
		const auto CopyTextureHandle = Builder.ImportTexture("Copy", &CopyTexture,
			ERHIAccess::TransferRead, ERHIAccess::TransferRead);
		const auto BufferHandle = Builder.ImportBuffer("Buffer", &Buffer,
			ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		auto ComputeParameters = Builder.AllocParameters<FComputeResolutionParameters>();
		ComputeParameters->Texture = {ComputeTextureHandle, WholeColor()};
		ComputeParameters->Buffer = {BufferHandle, 0, 64};
		uint32 CallbackCount = 0;
		Builder.AddPass("ResolveCompute", ERenderGraphPassType::Compute,
			std::move(ComputeParameters),
			[&](FRHICommandListImmediate&, const FComputeResolutionParameters& Values,
				const FRenderGraphParameterResolver& Resolver) {
				EXPECT_EQ(Resolver.GetTexture(Values.Texture), &ComputeTexture);
				EXPECT_EQ(Resolver.GetBuffer(Values.Buffer), &Buffer);
				++CallbackCount;
			});
		auto CopyParameters = Builder.AllocParameters<FCopyResolutionParameters>();
		CopyParameters->Texture = {CopyTextureHandle, WholeColor()};
		Builder.AddPass("ResolveCopy", ERenderGraphPassType::Copy,
			std::move(CopyParameters),
			[&](FRHICommandListImmediate&, const FCopyResolutionParameters& Values,
				const FRenderGraphParameterResolver& Resolver) {
				EXPECT_EQ(Resolver.GetTexture(Values.Texture), &CopyTexture);
				++CallbackCount;
			});
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_TRUE(Result.Graph->Execute(GetCommandList()));
		EXPECT_EQ(CallbackCount, 2u);
	}

	TEST_F(FRenderGraphTests, ParameterResolverRejectsRawWrongKindAndWrongPassAccess)
	{
		static_assert(!CTextureResolverArgument<FRenderGraphTextureHandle>);
		static_assert(!CTextureResolverArgument<FRenderGraphBufferParameter>);
		FRHITexture FirstTexture = MakeGraphTexture("First", 2);
		FRHITexture SecondTexture = MakeGraphTexture("Second", 2);
		FRenderGraphBuilder Builder;
		const auto First = Builder.ImportTexture("First", &FirstTexture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Second = Builder.ImportTexture("Second", &SecondTexture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		auto FirstParameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
		FirstParameters->Textures = {{{First,
			{ERHITextureAspect::Color, 0, 1, 0, 1}}, {First,
			{ERHITextureAspect::Color, 1, 1, 0, 1}}}};
		auto SecondParameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
		SecondParameters->Textures = {{{Second,
			{ERHITextureAspect::Color, 0, 1, 0, 1}}, {Second,
			{ERHITextureAspect::Color, 1, 1, 0, 1}}}};
		const auto* WrongPassMember = &SecondParameters->Textures[0];
		Builder.AddPass("FirstPass", ERenderGraphPassType::Graphics,
			std::move(FirstParameters),
			[WrongPassMember](FRHICommandListImmediate&,
				const FTwoTextureGraphParameters&,
				const FRenderGraphParameterResolver& Resolver) {
				Resolver.GetTexture(*WrongPassMember);
			});
		Builder.AddPass("SecondPass", ERenderGraphPassType::Graphics,
			std::move(SecondParameters));
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
			"pass 'FirstPass'.*requested capability 'texture'");
	}

	TEST_F(FRenderGraphTests, ParameterResolverRejectsCopiedAndForeignOptionalMembers)
	{
		FRHITexture Texture = MakeGraphTexture("Declared", 2);
		FRenderGraphBuilder Builder;
		const auto Handle = Builder.ImportTexture("Declared", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
		Parameters->Textures = {{{Handle,
			{ERHITextureAspect::Color, 0, 1, 0, 1}}, {Handle,
			{ERHITextureAspect::Color, 1, 1, 0, 1}}}};
		const FRenderGraphTextureParameter Copied = Parameters->Textures[0];
		const std::optional<FRenderGraphTextureParameter> ForeignOptional;
		Builder.AddPass("Copied", ERenderGraphPassType::Graphics,
			std::move(Parameters),
			[&](FRHICommandListImmediate&, const FTwoTextureGraphParameters&,
				const FRenderGraphParameterResolver& Resolver) {
				if (Copied.Texture.IsValid()) Resolver.GetTexture(Copied);
				else Resolver.GetTexture(ForeignOptional);
			});
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
			"not declared by the executing pass parameters");

		// An empty optional is nullable only when that exact optional object is a
		// declared field; a foreign empty optional remains an invalid capability.
		FRenderGraphBuilder OptionalBuilder;
		const auto OptionalHandle = OptionalBuilder.ImportTexture("Declared",
			&Texture, ERHIAccess::GraphicsShaderRead,
			ERHIAccess::GraphicsShaderRead);
		auto OptionalParameters =
			OptionalBuilder.AllocParameters<FTwoTextureGraphParameters>();
		OptionalParameters->Textures = {{{OptionalHandle,
			{ERHITextureAspect::Color, 0, 1, 0, 1}}, {OptionalHandle,
			{ERHITextureAspect::Color, 1, 1, 0, 1}}}};
		OptionalBuilder.AddPass("ForeignOptional", ERenderGraphPassType::Graphics,
			std::move(OptionalParameters),
			[&](FRHICommandListImmediate&, const FTwoTextureGraphParameters&,
				const FRenderGraphParameterResolver& Resolver) {
				Resolver.GetTexture(ForeignOptional);
			});
		auto OptionalResult = OptionalBuilder.Compile();
		ASSERT_TRUE(OptionalResult.IsSuccess()) << OptionalResult.Error;
		EXPECT_DEATH(OptionalResult.Graph->Execute(
			GetCommandList()),
			"not declared by the executing pass parameters");
	}

	TEST_F(FRenderGraphTests, ParameterizedCallbacksStayAtomicWhenCulledOrUnavailable)
	{
		{
			FRenderGraphBuilder Builder;
			Builder.EnablePassCulling();
			const auto Token = Builder.CreateToken("CulledToken");
			auto Parameters = Builder.AllocParameters<FNestedGraphParameters>();
			Parameters->Completion = {Token};
			bool bExecuted = false;
			Builder.AddPass("Culled", ERenderGraphPassType::Graphics,
				std::move(Parameters),
				[&](FRHICommandListImmediate&, const FNestedGraphParameters&,
					const FRenderGraphParameterResolver&) { bExecuted = true; });
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			const auto Capture = Result.Graph->Capture();
			EXPECT_TRUE(Capture.Passes.empty());
			ASSERT_EQ(Capture.Parameters.size(), 1u);
			EXPECT_EQ(Capture.Parameters[0].PassDeclarationIndex, 0u);
			EXPECT_EQ(Capture.Parameters[0].FieldPath,
				"FNestedGraphParameters.Completion");
			EXPECT_TRUE(Result.Graph->Execute(GetCommandList()));
			EXPECT_FALSE(bExecuted);
		}
		{
			FRenderGraphBuilder Builder;
			Builder.SetBackingResolver([](auto, auto&, std::string&) { return true; });
			const auto Buffer = Builder.CreateBuffer("Unavailable",
				FRenderGraphBufferDesc{.Buffer = FRHIBufferDesc(
					64, 4, EBufferUsageFlags::UnorderedAccess)});
			auto Parameters = Builder.AllocParameters<FUnavailableBufferParameters>();
			Parameters->Buffer = {Buffer, 0, 64};
			bool bExecuted = false;
			Builder.AddPass("Unavailable", ERenderGraphPassType::Compute,
				std::move(Parameters),
				[&](FRHICommandListImmediate&, const FUnavailableBufferParameters&,
					const FRenderGraphParameterResolver&) { bExecuted = true; });
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			std::string Error;
			EXPECT_FALSE(Result.Graph->Execute(
				GetCommandList(), &Error));
			EXPECT_NE(Error.find("omitted retained resource 'Unavailable'"),
				std::string::npos);
			EXPECT_FALSE(bExecuted);
		}
	}

	TEST_F(FRenderGraphTests, CompilesStableHazardOrderAndExactTextureTransitions)
	{
		FRHITexture Texture = MakeGraphTexture("SceneColor");
		FRenderGraphBuilder Builder;
		const auto SceneColor = Builder.CreateTexture(
			"SceneColor", &Texture, ERHIAccess::GraphicsShaderRead);
		const auto Independent = Builder.AddPass(
			"Independent", ERenderGraphPassType::Copy);
		const auto Produce = Builder.AddPass(
			"Produce", ERenderGraphPassType::Graphics);
		Builder.UseColorAttachment(Produce, SceneColor, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		const auto Consume = Builder.AddPass(
			"Consume", ERenderGraphPassType::Compute);
		Builder.UseTexture(Consume, SceneColor, WholeColor(),
			ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead);

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 3u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].Name, "Independent");
		EXPECT_EQ(Result.Graph->GetPasses()[1].Name, "Produce");
		EXPECT_EQ(Result.Graph->GetPasses()[2].Name, "Consume");
		ASSERT_EQ(Result.Graph->GetDependencies().size(), 1u);
		EXPECT_EQ(Result.Graph->GetDependencies()[0],
			(FRenderGraphDependency{1, 2, "SceneColor",
				ERenderGraphDependencyKind::Value}));
		ASSERT_EQ(Result.Graph->GetPasses()[1].TextureTransitions.size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[1].TextureTransitions[0],
			(FRHITextureTransition{&Texture, WholeColor(), ERHIAccess::Discard,
				ERHIAccess::ColorAttachmentReadWrite}));
		ASSERT_EQ(Result.Graph->GetPasses()[2].TextureTransitions.size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[2].TextureTransitions[0],
			(FRHITextureTransition{&Texture, WholeColor(),
				ERHIAccess::ColorAttachmentReadWrite,
				ERHIAccess::ComputeShaderRead}));
		ASSERT_EQ(Result.Graph->GetFinalTextureTransitions().size(), 1u);
		EXPECT_EQ(Result.Graph->GetFinalTextureTransitions()[0],
			(FRHITextureTransition{&Texture, WholeColor(),
				ERHIAccess::ComputeShaderRead,
				ERHIAccess::GraphicsShaderRead}));
	}

	TEST_F(FRenderGraphTests, CompilesBufferRawWarAndWawDependencies)
	{
		FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
			"Work", 64, 4, EBufferUsageFlags::UnorderedAccess
				| EBufferUsageFlags::SourceCopy));
		FRenderGraphBuilder Builder;
		const auto Work = Builder.CreateBuffer("Work", &Buffer);
		const auto Write = Builder.AddPass("Write", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Write, Work, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Read = Builder.AddPass("Read", ERenderGraphPassType::Copy);
		Builder.UseBuffer(Read, Work, 0, 64, ERenderGraphUse::Read,
			ERHIAccess::TransferRead);
		const auto Rewrite = Builder.AddPass("Rewrite", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Rewrite, Work, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite);

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetDependencies().size(), 2u);
		EXPECT_EQ(Result.Graph->GetDependencies()[0].Kind,
			ERenderGraphDependencyKind::Value);
		EXPECT_EQ(Result.Graph->GetDependencies()[1].Kind,
			ERenderGraphDependencyKind::Execution);
		EXPECT_EQ(Result.Graph->GetPasses()[0].BufferTransitions[0].ExpectedBefore,
			ERHIAccess::Discard);
		EXPECT_EQ(Result.Graph->GetPasses()[1].BufferTransitions[0].ExpectedBefore,
			ERHIAccess::ComputeShaderReadWrite);
		EXPECT_EQ(Result.Graph->GetPasses()[2].BufferTransitions[0].ExpectedBefore,
			ERHIAccess::TransferRead);
	}

	TEST_F(FRenderGraphTests, RejectsMissingProducerForeignHandleAndCycle)
	{
		FRHITexture Texture = MakeGraphTexture("Missing");
		FRenderGraphBuilder MissingProducer;
		const auto Logical = MissingProducer.CreateTexture("Missing", &Texture);
		const auto Read = MissingProducer.AddPass("Read", ERenderGraphPassType::Graphics);
		MissingProducer.UseTexture(Read, Logical, WholeColor(),
			ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		auto Missing = MissingProducer.Compile();
		EXPECT_FALSE(Missing.IsSuccess());
		EXPECT_NE(Missing.Error.find("before its producer"), std::string::npos);

		FRenderGraphBuilder ForeignOwner;
		const auto Foreign = ForeignOwner.CreateTexture("Foreign", &Texture);
		FRenderGraphBuilder ForeignUse;
		const auto Pass = ForeignUse.AddPass("Use", ERenderGraphPassType::Graphics);
		ForeignUse.UseTexture(Pass, Foreign, WholeColor(), ERenderGraphUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Invalid = ForeignUse.Compile();
		EXPECT_FALSE(Invalid.IsSuccess());
		EXPECT_NE(Invalid.Error.find("invalid resource handle"), std::string::npos);

		FRenderGraphBuilder Cyclic;
		const auto A = Cyclic.AddPass("A", ERenderGraphPassType::Compute);
		const auto B = Cyclic.AddPass("B", ERenderGraphPassType::Compute);
		Cyclic.AddDependency(A, B);
		Cyclic.AddDependency(B, A);
		auto Cycle = Cyclic.Compile();
		EXPECT_FALSE(Cycle.IsSuccess());
		EXPECT_EQ(Cycle.Error, "graph contains a dependency cycle");

		FRenderGraphBuilder SelfDependent;
		const auto Self = SelfDependent.AddPass(
			"Self", ERenderGraphPassType::Compute);
		SelfDependent.AddDependency(Self, Self);
		auto SelfCycle = SelfDependent.Compile();
		EXPECT_FALSE(SelfCycle.IsSuccess());
		EXPECT_EQ(SelfCycle.Error, "graph contains a dependency cycle");
	}

	TEST_F(FRenderGraphTests, RejectsTextureAspectsOutsideResourceFormat)
	{
		FRHITexture Texture = MakeGraphTexture("ColorOnly");
		FRenderGraphBuilder Builder;
		const auto Resource = Builder.CreateTexture("ColorOnly", &Texture);
		const auto Pass = Builder.AddPass("InvalidAspects",
			ERenderGraphPassType::Compute);
		Builder.UseTexture(Pass, Resource,
			{ERHITextureAspect::Color | ERHITextureAspect::Depth, 0, 1, 0, 1},
			ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);

		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("invalid texture range"), std::string::npos);
	}

	TEST_F(FRenderGraphTests, NormalizesDisjointAndPartiallyOverlappingSubresources)
	{
		FRHITexture Texture = MakeGraphTexture("MipChain", 4);
		FRenderGraphBuilder Builder;
		const auto Chain = Builder.CreateTexture("MipChain", &Texture);
		const auto Mip0 = Builder.AddPass("Mip0", ERenderGraphPassType::Compute);
		Builder.UseTexture(Mip0, Chain, {ERHITextureAspect::Color, 0, 1, 0, 1},
			ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto Mip1 = Builder.AddPass("Mip1", ERenderGraphPassType::Compute);
		Builder.UseTexture(Mip1, Chain, {ERHITextureAspect::Color, 1, 1, 0, 1},
			ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		auto Disjoint = Builder.Compile();
		ASSERT_TRUE(Disjoint.IsSuccess()) << Disjoint.Error;
		EXPECT_TRUE(Disjoint.Graph->GetDependencies().empty());

		FRenderGraphBuilder Partial;
		const auto PartialChain = Partial.CreateTexture("MipChain", &Texture);
		const auto Whole = Partial.AddPass("Whole", ERenderGraphPassType::Compute);
		Partial.UseTexture(Whole, PartialChain, WholeColor(4),
			ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto OneMip = Partial.AddPass("OneMip", ERenderGraphPassType::Compute);
		Partial.UseTexture(OneMip, PartialChain,
			{ERHITextureAspect::Color, 1, 1, 0, 1}, ERenderGraphUse::Read,
			ERHIAccess::ComputeShaderRead);
		auto Overlap = Partial.Compile();
		ASSERT_TRUE(Overlap.IsSuccess()) << Overlap.Error;
		ASSERT_EQ(Overlap.Graph->GetDependencies().size(), 1u);
		EXPECT_EQ(Overlap.Graph->GetDependencies()[0].Kind,
			ERenderGraphDependencyKind::Value);
		EXPECT_EQ(Overlap.Graph->GetPasses()[0].TextureTransitions.size(), 3u);
		EXPECT_EQ(Overlap.Graph->GetPasses()[1].TextureTransitions.size(), 1u);
	}

	TEST_F(FRenderGraphTests, DiscardedAttachmentStoreCannotBecomeAProducer)
	{
		FRHITexture Texture = MakeGraphTexture("Discarded");
		FRenderGraphBuilder Builder;
		const auto Target = Builder.CreateTexture("Discarded", &Texture);
		const auto Clear = Builder.AddPass("Clear", ERenderGraphPassType::Graphics);
		Builder.UseColorAttachment(Clear, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::DontCare);
		const auto Read = Builder.AddPass("Read", ERenderGraphPassType::Graphics);
		Builder.UseTexture(Read, Target, WholeColor(), ERenderGraphUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("before its producer"), std::string::npos);
	}

	TEST_F(FRenderGraphTests, PreservesImportedInitialAndFinalStates)
	{
		FRHITexture Texture = MakeGraphTexture("Imported");
		FRenderGraphBuilder Builder;
		const auto Imported = Builder.ImportTexture("Imported", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Compute = Builder.AddPass("Compute", ERenderGraphPassType::Compute);
		Builder.UseTexture(Compute, Imported, WholeColor(),
			ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses()[0].TextureTransitions.size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].TextureTransitions[0].ExpectedBefore,
			ERHIAccess::GraphicsShaderRead);
		ASSERT_EQ(Result.Graph->GetFinalTextureTransitions().size(), 1u);
		EXPECT_EQ(Result.Graph->GetFinalTextureTransitions()[0].RequiredAfter,
			ERHIAccess::GraphicsShaderRead);
	}

	TEST_F(FRenderGraphTests, RejectsAttachmentLoadWithoutPriorContents)
	{
		FRHITexture Texture = MakeGraphTexture("Load");
		FRenderGraphBuilder Builder;
		const auto Target = Builder.CreateTexture("Load", &Texture);
		const auto Load = Builder.AddPass("Load", ERenderGraphPassType::Graphics);
		Builder.UseColorAttachment(Load, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Load,
			ERHIRenderTargetStoreAction::Store);
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("before its producer"), std::string::npos);
	}

	TEST_F(FRenderGraphTests, DumpIsDeterministicAndSyntheticCompileCostIsBounded)
	{
		auto CompileFixture = [] {
			static FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
				"Fixture", 512, 4, EBufferUsageFlags::UnorderedAccess));
			FRenderGraphBuilder Builder;
			const auto Work = Builder.CreateBuffer("Fixture", &Buffer);
			for (uint32 Index = 0; Index < 128; ++Index)
			{
				const auto Pass = Builder.AddPass("Pass" + std::to_string(Index),
					ERenderGraphPassType::Compute);
				Builder.UseBuffer(Pass, Work, 0, 512, ERenderGraphUse::Write,
					ERHIAccess::ComputeShaderReadWrite, Index == 0);
			}
			return Builder.Compile();
		};
		auto First = CompileFixture();
		auto Second = CompileFixture();
		ASSERT_TRUE(First.IsSuccess()) << First.Error;
		ASSERT_TRUE(Second.IsSuccess()) << Second.Error;
		EXPECT_EQ(First.Graph->Dump(), Second.Graph->Dump());
		EXPECT_LT(First.Graph->GetCompileMicroseconds(), 250000u);
		EXPECT_LT(Second.Graph->GetCompileMicroseconds(), 250000u);
		EXPECT_EQ(First.Graph->GetDependencies().size(), 127u);
	}

	TEST_F(FRenderGraphTests, CullsUnreachableBranchesAndReportsExactLifetimes)
	{
		FRHIBuffer RetainedBuffer(FRHIBufferCreateDesc::Create(
			"Retained", 64, 4, EBufferUsageFlags::UnorderedAccess));
		FRHIBuffer CulledBuffer(FRHIBufferCreateDesc::Create(
			"Culled", 64, 4, EBufferUsageFlags::UnorderedAccess));
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		const auto Retained = Builder.CreateBuffer("Retained", &RetainedBuffer);
		const auto Culled = Builder.CreateBuffer("Culled", &CulledBuffer);
		const auto Produce = Builder.AddPass("Produce", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Produce, Retained, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Consume = Builder.AddPass("Present", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Consume, Retained, 0, 64, ERenderGraphUse::Read,
			ERHIAccess::ComputeShaderRead);
		Builder.MarkPassRoot(Consume, "present");
		const auto Unused = Builder.AddPass("Unused", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Unused, Culled, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 2u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].Name, "Produce");
		EXPECT_EQ(Result.Graph->GetPasses()[1].Name, "Present");
		ASSERT_EQ(Result.Graph->GetResourceLifetimes().size(), 2u);
		EXPECT_EQ(Result.Graph->GetResourceLifetimes()[0].FirstPass, 0u);
		EXPECT_EQ(Result.Graph->GetResourceLifetimes()[0].LastPass, 1u);
		EXPECT_FALSE(Result.Graph->GetResourceLifetimes()[0].bCulled);
		EXPECT_TRUE(Result.Graph->GetResourceLifetimes()[1].bCulled);
		ASSERT_EQ(Result.Graph->GetCullingDecisions().size(), 3u);
		EXPECT_FALSE(Result.Graph->GetCullingDecisions()[0].bCulled);
		EXPECT_EQ(Result.Graph->GetCullingDecisions()[0].Reason, "value dependency");
		EXPECT_EQ(Result.Graph->GetCullingDecisions()[1].Reason, "present");
		EXPECT_TRUE(Result.Graph->GetCullingDecisions()[2].bCulled);
	}

	TEST_F(FRenderGraphTests, CanonicalizesEquivalentImportedIdentity)
	{
		FRHITexture Texture = MakeGraphTexture("Shared");
		FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
			"SharedBuffer", 64, 4, EBufferUsageFlags::UnorderedAccess));
		FRenderGraphBuilder Builder;
		const auto FirstTexture = Builder.ImportTexture("First", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto SecondTexture = Builder.ImportTexture("Second", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto FirstBuffer = Builder.ImportBuffer("FirstBuffer", &Buffer,
			ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		const auto SecondBuffer = Builder.ImportBuffer("SecondBuffer", &Buffer,
			ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		EXPECT_EQ(FirstTexture, SecondTexture);
		EXPECT_EQ(FirstBuffer, SecondBuffer);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const auto Capture = Result.Graph->Capture();
		ASSERT_EQ(Capture.Resources.size(), 2u);
		EXPECT_EQ(Capture.Resources[0].Name, "First");
		EXPECT_EQ(Capture.Resources[1].Name, "FirstBuffer");

		FRenderGraphBuilder OtherBuilder;
		const auto Other = OtherBuilder.ImportTexture("Other", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		EXPECT_NE(FirstTexture, Other);
	}

	TEST_F(FRenderGraphTests, RejectsConflictingImportedIdentityAndDomainMismatch)
	{
		FRHITexture Texture = MakeGraphTexture("Shared");
		FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
			"SharedBuffer", 64, 4, EBufferUsageFlags::UnorderedAccess));
		FRenderGraphBuilder Duplicate;
		Duplicate.ImportTexture("First", &Texture, ERHIAccess::GraphicsShaderRead,
			ERHIAccess::GraphicsShaderRead);
		Duplicate.ImportTexture("Second", &Texture, ERHIAccess::ComputeShaderRead,
			ERHIAccess::GraphicsShaderRead);
		auto DuplicateResult = Duplicate.Compile();
		EXPECT_FALSE(DuplicateResult.IsSuccess());
		EXPECT_NE(DuplicateResult.Error.find(
			"conflicting imported physical resource: canonical 'First'"),
			std::string::npos);
		EXPECT_NE(DuplicateResult.Error.find("conflicts with 'Second'"),
			std::string::npos);

		FRenderGraphBuilder BufferConflict;
		BufferConflict.ImportBuffer("CanonicalBuffer", &Buffer,
			ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		BufferConflict.ImportBuffer("ConflictingBuffer", &Buffer,
			ERHIAccess::ComputeShaderRead, ERHIAccess::TransferRead);
		auto BufferConflictResult = BufferConflict.Compile();
		EXPECT_FALSE(BufferConflictResult.IsSuccess());
		EXPECT_NE(BufferConflictResult.Error.find(
			"canonical 'CanonicalBuffer' (kind=buffer"), std::string::npos);
		EXPECT_NE(BufferConflictResult.Error.find(
			"conflicts with 'ConflictingBuffer'"), std::string::npos);

		FRenderGraphBuilder NullImport;
		const auto NullHandle = NullImport.ImportTexture("Null", nullptr,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		EXPECT_TRUE(NullHandle.IsValid());
		auto NullResult = NullImport.Compile();
		EXPECT_FALSE(NullResult.IsSuccess());
		EXPECT_EQ(NullResult.Error, "resource 'Null' has no physical resource");

		FRenderGraphBuilder Domain;
		const auto Imported = Domain.ImportTexture("Shared", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Copy = Domain.AddPass("Copy", ERenderGraphPassType::Copy);
		Domain.UseTexture(Copy, Imported, WholeColor(), ERenderGraphUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto DomainResult = Domain.Compile();
		EXPECT_FALSE(DomainResult.IsSuccess());
		EXPECT_NE(DomainResult.Error.find("incompatible with pass domain"),
			std::string::npos);
	}

	TEST_F(FRenderGraphTests, DiscardValueCullingDoesNotRetainOverwrittenProducer)
	{
		FRHITexture Texture = MakeGraphTexture("Versioned");
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		const auto Resource = Builder.CreateTexture("Versioned", &Texture);
		const auto Old = Builder.AddPass("Old", ERenderGraphPassType::Compute);
		Builder.UseTexture(Old, Resource, WholeColor(), ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Replacement = Builder.AddPass("Replacement",
			ERenderGraphPassType::Compute);
		Builder.UseTexture(Replacement, Resource, WholeColor(),
			ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto Consume = Builder.AddPass("Consume", ERenderGraphPassType::Compute);
		Builder.UseTexture(Consume, Resource, WholeColor(), ERenderGraphUse::Read,
			ERHIAccess::ComputeShaderRead);
		Builder.MarkPassRoot(Consume, "output");
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 2u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].Name, "Replacement");
		EXPECT_TRUE(Result.Graph->GetCullingDecisions()[0].bCulled);
	}

	TEST_F(FRenderGraphTests, RetainedLogicalResourcesPublishExactPreparationCapture)
	{
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		Builder.SetBackingResolver([](auto, auto&, std::string&) { return true; });
		FRenderGraphBufferDesc Desc{
			.Buffer = FRHIBufferDesc(64, 4, EBufferUsageFlags::UnorderedAccess),
			.BackingClass = "test-pool"};
		const auto Retained = Builder.CreateBuffer("Retained", Desc);
		const auto Culled = Builder.CreateBuffer("Culled", Desc);
		const auto Produce = Builder.AddPass("Produce", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Produce, Retained, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		Builder.MarkPassRoot(Produce, "effect");
		const auto Unused = Builder.AddPass("Unused", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Unused, Culled, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const auto Capture = Result.Graph->Capture();
		ASSERT_EQ(Capture.Resources.size(), 2u);
		EXPECT_EQ(Capture.Resources[0].Preparation, "requested");
		EXPECT_EQ(Capture.Resources[0].BackingClass, "test-pool");
		EXPECT_EQ(Capture.Resources[1].Preparation, "culled");
		ASSERT_EQ(Capture.Uses.size(), 1u);
		EXPECT_EQ(Capture.Uses[0].Version, 1u);
	}

	TEST_F(FRenderGraphTests, PassResourceViewRejectsUndeclaredLookup)
	{
		FRHITexture DeclaredTexture = MakeGraphTexture("Declared");
		FRHITexture HiddenTexture = MakeGraphTexture("Hidden");
		FRenderGraphBuilder Builder;
		const auto Declared = Builder.ImportTexture("Declared", &DeclaredTexture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Hidden = Builder.ImportTexture("Hidden", &HiddenTexture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Pass = Builder.AddPass("Pass", ERenderGraphPassType::Graphics,
			[=](FRHICommandListImmediate&, const FRenderGraphPassResources& Resources) {
				Resources.GetTexture(Hidden);
			});
		Builder.UseTexture(Pass, Declared, WholeColor(), ERenderGraphUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
			"undeclared texture");
	}

	TEST_F(FRenderGraphTests, ManagedAttachmentExitStateDrivesFollowingTransition)
	{
		FRHITexture Texture = MakeGraphTexture("Managed");
		FRenderGraphBuilder Builder;
		const auto Target = Builder.CreateTexture("Managed", &Texture);
		const auto Render = Builder.AddPass("Render", ERenderGraphPassType::Graphics);
		Builder.UseManagedColorAttachment(Render, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			ERHIAccess::GraphicsShaderRead);
		const auto Consume = Builder.AddPass("Consume", ERenderGraphPassType::Compute);
		Builder.UseTexture(Consume, Target, WholeColor(), ERenderGraphUse::Read,
			ERHIAccess::ComputeShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_TRUE(Result.Graph->GetPasses()[0].TextureTransitions.empty());
		ASSERT_EQ(Result.Graph->GetPasses()[1].TextureTransitions.size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[1].TextureTransitions[0].ExpectedBefore,
			ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Result.Graph->Capture().Transitions.size(), 3u);
	}

	TEST_F(FRenderGraphTests, IncompleteBackingPublicationRecordsNoCallback)
	{
		bool bExecuted = false;
		FRenderGraphBuilder Builder;
		Builder.SetBackingResolver([](auto, auto&, std::string&) { return true; });
		const auto Buffer = Builder.CreateBuffer("Logical",
			FRenderGraphBufferDesc{.Buffer = FRHIBufferDesc(
				64, 4, EBufferUsageFlags::UnorderedAccess)});
		const auto Pass = Builder.AddPass("Write", ERenderGraphPassType::Compute,
			[&](FRHICommandListImmediate&, const FRenderGraphPassResources&) {
				bExecuted = true;
			});
		Builder.UseBuffer(Pass, Buffer, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		std::string Error;
		EXPECT_FALSE(Result.Graph->Execute(GetCommandList(), &Error));
		EXPECT_FALSE(bExecuted);
		EXPECT_NE(Error.find("omitted retained resource"), std::string::npos);
	}

	TEST_F(FRenderGraphTests, RDGAllocationIsDescriptorDrivenAndExtractionIsTransactional)
	{
		FTextureRHIRef FirstExtraction;
		FTextureRHIRef SecondExtraction;
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		const FRenderGraphTextureDesc Desc{
			.Texture = FRHITextureCreateDesc::Create2D(
				"DiagnosticOnly", 16, 16, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource)};
		const auto First = Builder.CreateTexture(Desc, "Renamed.First");
		const auto Second = Builder.CreateTexture(Desc, "Renamed.Second");
		const auto FirstPass = Builder.AddPass(
			"First", ERenderGraphPassType::Graphics);
		Builder.UseColorAttachment(FirstPass, First, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		const auto SecondPass = Builder.AddPass(
			"Second", ERenderGraphPassType::Graphics);
		Builder.UseColorAttachment(SecondPass, Second, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		Builder.QueueTextureExtraction(First, &FirstExtraction,
			ERHIAccess::GraphicsShaderRead);
		Builder.QueueTextureExtraction(Second, &SecondExtraction,
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_FALSE(FirstExtraction);
		EXPECT_FALSE(SecondExtraction);
		ASSERT_EQ(Result.Graph->GetPasses().size(), 2u);

		FTestRDGAllocator Allocator;
		FRDGExecutionContext Context{Allocator};
		std::string Error;
		ASSERT_TRUE(Result.Graph->Execute(GetCommandList(), Context, &Error))
			<< Error;
		EXPECT_EQ(Allocator.AllocationCount, 2u);
		ASSERT_TRUE(FirstExtraction);
		ASSERT_TRUE(SecondExtraction);
		EXPECT_NE(FirstExtraction.GetReference(), SecondExtraction.GetReference());
		const auto Capture = Result.Graph->Capture();
		EXPECT_EQ(Capture.Resources[0].AllocationDisposition, "allocated");
		EXPECT_NE(Capture.Resources[0].PhysicalAllocationId, 0u);
		EXPECT_NE(Capture.Resources[0].PhysicalAllocationId,
			Capture.Resources[1].PhysicalAllocationId);
	}

	TEST_F(FRenderGraphTests, RDGAllocationFailurePublishesNoExtractionOrPass)
	{
		auto Original = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D(
				"Original", 4, 4, EPixelFormat::RGBA8_UNORM));
		FTextureRHIRef Destination = Original;
		bool bExecuted = false;
		FRenderGraphBuilder Builder;
		const auto Texture = Builder.CreateTexture(
			FRenderGraphTextureDesc{.Texture =
				FRHITextureCreateDesc::Create2D(
					"Logical", 16, 16, EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable)},
			"Logical");
		const auto Pass = Builder.AddPass("Write", ERenderGraphPassType::Graphics,
			[&](FRHICommandListImmediate&, const FRenderGraphPassResources&) {
				bExecuted = true;
			});
		Builder.UseColorAttachment(Pass, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		Builder.QueueTextureExtraction(Texture, &Destination,
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		FTestRDGAllocator Allocator;
		Allocator.bFail = true;
		FRDGExecutionContext Context{Allocator};
		std::string Error;
		EXPECT_FALSE(Result.Graph->Execute(GetCommandList(), Context, &Error));
		EXPECT_FALSE(bExecuted);
		EXPECT_EQ(Destination.GetReference(), Original.GetReference());
		EXPECT_NE(Error.find("injected allocation failure"), std::string::npos);
	}

	TEST_F(FRenderGraphTests, ExternalRegistrationRetainsPhysicalResource)
	{
		auto Texture = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D(
				"External", 8, 8, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource));
		const int32 InitialReferences = Texture.GetRefCount();
		{
			FRenderGraphBuilder Builder;
			const auto External = Builder.RegisterExternalTexture(Texture,
				"External", ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
			const auto Pass = Builder.AddPass("Read", ERenderGraphPassType::Graphics);
			Builder.UseTexture(Pass, External, WholeColor(), ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			EXPECT_GT(Texture.GetRefCount(), InitialReferences);
			EXPECT_TRUE(Result.Graph->Execute(GetCommandList()));
		}
		EXPECT_EQ(Texture.GetRefCount(), InitialReferences);
	}

	TEST_F(FRenderGraphTests, ExternalExtractionRoundTripPublishesAfterExecution)
	{
		auto Texture = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D(
				"External", 8, 8, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource));
		FTextureRHIRef Extracted;
		FRenderGraphBuilder Builder;
		const auto External = Builder.RegisterExternalTexture(Texture,
			"External", ERHIAccess::GraphicsShaderRead,
			ERHIAccess::GraphicsShaderRead);
		Builder.QueueTextureExtraction(External, &Extracted,
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_FALSE(Extracted);
		ASSERT_TRUE(Result.Graph->Execute(GetCommandList()));
		EXPECT_EQ(Extracted.GetReference(), Texture.GetReference());
	}

	TEST_F(FRenderGraphTests, DuplicateExtractionFailsWithoutPublishing)
	{
		FTextureRHIRef First;
		FTextureRHIRef Second;
		FRenderGraphBuilder Builder;
		const auto Texture = Builder.CreateTexture(
			FRenderGraphTextureDesc{.Texture =
				FRHITextureCreateDesc::Create2D(
					"Logical", 8, 8, EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable)},
			"Logical");
		Builder.QueueTextureExtraction(Texture, &First,
			ERHIAccess::GraphicsShaderRead);
		Builder.QueueTextureExtraction(Texture, &Second,
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("duplicate or conflicting texture extraction"),
			std::string::npos);
		EXPECT_FALSE(First);
		EXPECT_FALSE(Second);
	}

	TEST_F(FRenderGraphTests, BufferExtractionPublishesCountedAllocation)
	{
		FBufferRHIRef Extracted;
		FRenderGraphBuilder Builder;
		const auto Buffer = Builder.CreateBuffer(
			FRenderGraphBufferDesc{.Buffer = FRHIBufferDesc(
				64, 4, EBufferUsageFlags::UnorderedAccess)}, "LogicalBuffer");
		const auto Pass = Builder.AddPass("Write", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Pass, Buffer, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		Builder.QueueBufferExtraction(Buffer, &Extracted,
			ERHIAccess::ComputeShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		FTestRDGAllocator Allocator;
		FRDGExecutionContext Context{Allocator};
		std::string Error;
		ASSERT_TRUE(Result.Graph->Execute(GetCommandList(), Context, &Error))
			<< Error;
		EXPECT_TRUE(Extracted);
		EXPECT_EQ(Extracted->GetDesc().Size, 64u);
	}

	TEST_F(FRenderGraphTests, RejectsBackingWithIncompatibleUsageFlags)
	{
		FRHITexture TextureBacking(FRHITextureCreateDesc::Create2D(
			"TextureBacking", 16, 16, EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::ShaderResource));
		FRenderGraphBuilder TextureBuilder;
		TextureBuilder.SetBackingResolver(
			[&](auto Requests, auto& Backings, std::string&) {
				return Backings.SetTexture(Requests.front().Texture,
					&TextureBacking);
			});
		const auto Texture = TextureBuilder.CreateTexture("LogicalTexture",
			FRenderGraphTextureDesc{
				.Texture = FRHITextureCreateDesc::Create2D(
					"LogicalTexture", 16, 16, EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable),
				.BackingClass = "test"});
		const auto TexturePass = TextureBuilder.AddPass(
			"TextureWrite", ERenderGraphPassType::Graphics);
		TextureBuilder.UseColorAttachment(TexturePass, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		auto TextureResult = TextureBuilder.Compile();
		ASSERT_TRUE(TextureResult.IsSuccess()) << TextureResult.Error;
		std::string Error;
		EXPECT_FALSE(TextureResult.Graph->Execute(
			GetCommandList(), &Error));
		EXPECT_NE(Error.find("incompatible texture"), std::string::npos);

		FRHIBuffer BufferBacking(FRHIBufferCreateDesc::Create(
			"BufferBacking", 64, 4, EBufferUsageFlags::StructuredBuffer));
		FRenderGraphBuilder BufferBuilder;
		BufferBuilder.SetBackingResolver(
			[&](auto Requests, auto& Backings, std::string&) {
				return Backings.SetBuffer(Requests.front().Buffer, &BufferBacking);
			});
		const auto Buffer = BufferBuilder.CreateBuffer("LogicalBuffer",
			FRenderGraphBufferDesc{
				.Buffer = FRHIBufferDesc(
					64, 4, EBufferUsageFlags::UnorderedAccess),
				.BackingClass = "test"});
		const auto BufferPass = BufferBuilder.AddPass(
			"BufferWrite", ERenderGraphPassType::Compute);
		BufferBuilder.UseBuffer(BufferPass, Buffer, 0, 64,
			ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		auto BufferResult = BufferBuilder.Compile();
		ASSERT_TRUE(BufferResult.IsSuccess()) << BufferResult.Error;
		EXPECT_FALSE(BufferResult.Graph->Execute(
			GetCommandList(), &Error));
		EXPECT_NE(Error.find("incompatible buffer"), std::string::npos);
	}

	TEST_F(FRenderGraphTests, AcceptsBackingWithSupersetUsageFlags)
	{
		static const auto TextureBacking = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D(
			"TextureBacking", 16, 16, EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource));
		FRenderGraphBuilder TextureBuilder;
		TextureBuilder.SetBackingResolver(
			[&](auto Requests, auto& Backings, std::string&) {
				return Backings.SetTexture(Requests.front().Texture,
					TextureBacking.GetReference());
			});
		const auto Texture = TextureBuilder.CreateTexture("LogicalTexture",
			FRenderGraphTextureDesc{
				.Texture = FRHITextureCreateDesc::Create2D(
					"LogicalTexture", 16, 16, EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable),
				.BackingClass = "test"});
		const auto TexturePass = TextureBuilder.AddPass(
			"TextureWrite", ERenderGraphPassType::Graphics);
		TextureBuilder.UseColorAttachment(TexturePass, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		auto TextureResult = TextureBuilder.Compile();
		ASSERT_TRUE(TextureResult.IsSuccess()) << TextureResult.Error;
		std::string Error;
		EXPECT_TRUE(TextureResult.Graph->Execute(
			GetCommandList(), &Error)) << Error;

		static const auto BufferBacking = MakeRefCount<FRHIBuffer>(
			FRHIBufferCreateDesc::Create(
			"BufferBacking", 64, 4, EBufferUsageFlags::UnorderedAccess
				| EBufferUsageFlags::StructuredBuffer));
		FRenderGraphBuilder BufferBuilder;
		BufferBuilder.SetBackingResolver(
			[&](auto Requests, auto& Backings, std::string&) {
				return Backings.SetBuffer(Requests.front().Buffer,
					BufferBacking.GetReference());
			});
		const auto Buffer = BufferBuilder.CreateBuffer("LogicalBuffer",
			FRenderGraphBufferDesc{
				.Buffer = FRHIBufferDesc(
					64, 4, EBufferUsageFlags::UnorderedAccess),
				.BackingClass = "test"});
		const auto BufferPass = BufferBuilder.AddPass(
			"BufferWrite", ERenderGraphPassType::Compute);
		BufferBuilder.UseBuffer(BufferPass, Buffer, 0, 64,
			ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		auto BufferResult = BufferBuilder.Compile();
		ASSERT_TRUE(BufferResult.IsSuccess()) << BufferResult.Error;
		EXPECT_TRUE(BufferResult.Graph->Execute(
			GetCommandList(), &Error)) << Error;
	}

	TEST_F(FRenderGraphTests, ExplicitEffectRootSurvivesWithoutResourceOutputs)
	{
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		const auto Timestamp = Builder.AddPass(
			"Timestamp", ERenderGraphPassType::Graphics);
		Builder.MarkPassRoot(Timestamp, "timestamp");
		Builder.AddPass("Unused", ERenderGraphPassType::Graphics);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].Name, "Timestamp");
	}

	TEST_F(FRenderGraphTests, LogicalTokensDriveDependenciesAndLifetimesWithoutRHIState)
	{
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		const auto Prepared = Builder.CreateToken("Prepared");
		const auto Output = Builder.CreateToken("Output");
		const auto Prepare = Builder.AddPass("Prepare", ERenderGraphPassType::Graphics);
		Builder.UseToken(Prepare, Prepared, ERenderGraphUse::Write);
		const auto Render = Builder.AddPass("Render", ERenderGraphPassType::Graphics);
		Builder.UseToken(Render, Prepared, ERenderGraphUse::Read);
		Builder.UseToken(Render, Output, ERenderGraphUse::Write);
		Builder.MarkPassRoot(Render, "present");
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 2u);
		ASSERT_EQ(Result.Graph->GetDependencies().size(), 1u);
		EXPECT_EQ(Result.Graph->GetDependencies()[0].Cause, "Prepared");
		EXPECT_TRUE(Result.Graph->GetPasses()[0].BufferTransitions.empty());
		EXPECT_TRUE(Result.Graph->GetPasses()[0].TextureTransitions.empty());
		EXPECT_EQ(Result.Graph->GetResourceLifetimes()[0].FirstPass, 0u);
		EXPECT_EQ(Result.Graph->GetResourceLifetimes()[0].LastPass, 1u);
	}

	TEST_F(FRenderGraphTests, GBufferManualDeclarationOracleFreezesCompletePassShape)
	{
		std::array<FRHITexture, 4> ColorTextures{
			MakeGraphTexture("Scene.GBuffer.Material"),
			MakeGraphTexture("Scene.GBuffer.Normals"),
			MakeGraphTexture("Scene.GBuffer.Surface"),
			MakeGraphTexture("Scene.GBuffer.Emissive"),
		};
		FRHITexture DepthTexture(FRHITextureCreateDesc::Create2D(
			"Scene.Depth", 64, 64, EPixelFormat::D32)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable
				| ETextureCreateFlags::ShaderResource));
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		std::array<FRenderGraphTextureHandle, 4> Colors{};
		const std::array Names{"Scene.GBuffer.Material", "Scene.GBuffer.Normals",
			"Scene.GBuffer.Surface", "Scene.GBuffer.Emissive"};
		for (uint32 Index = 0; Index < Colors.size(); ++Index)
			Colors[Index] = Builder.CreateTexture(Names[Index], &ColorTextures[Index],
				ERHIAccess::GraphicsShaderRead);
		const auto Depth = Builder.CreateTexture("Scene.Depth", &DepthTexture,
			ERHIAccess::GraphicsShaderRead);
		const auto Completion = Builder.CreateToken("Scene.GBuffer.Result");
		uint32 CallbackCount = 0;
		const auto Pass = Builder.AddPass("Scene.GBuffer",
			ERenderGraphPassType::Graphics,
			[&](FRHICommandListImmediate&,
				const FRenderGraphPassResources& Resources) {
				for (const auto Color : Colors)
					EXPECT_NE(Resources.GetTexture(Color), nullptr);
				EXPECT_EQ(Resources.GetTexture(Depth), &DepthTexture);
				++CallbackCount;
			});
		Builder.UseToken(Pass, Completion, ERenderGraphUse::Write);
		for (const auto Color : Colors)
			Builder.UseManagedColorAttachment(Pass, Color, WholeColor(),
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead);
		Builder.UseManagedDepthStencilAttachment(Pass, Depth,
			{ERHITextureAspect::Depth, 0, 1, 0, 1},
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			ERHIAccess::GraphicsShaderRead);
		Builder.MarkPassRoot(Pass, "gbuffer-pilot");

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const FRenderGraphCapture Capture = Result.Graph->Capture();
		ASSERT_EQ(Capture.Passes.size(), 1u);
		EXPECT_EQ(Capture.Passes[0].Name, "Scene.GBuffer");
		EXPECT_EQ(Capture.Passes[0].Type, ERenderGraphPassType::Graphics);
		EXPECT_EQ(Capture.Statistics.DeclaredPasses, 1u);
		EXPECT_EQ(Capture.Statistics.ScheduledPasses, 1u);
		EXPECT_EQ(Capture.Statistics.Dependencies, 0u);
		EXPECT_EQ(Capture.Statistics.TextureTransitions, 0u);
		ASSERT_EQ(Capture.Uses.size(), 6u);
		EXPECT_EQ(Capture.Uses[0].ResourceId, 5u);
		EXPECT_EQ(Capture.Uses[0].Use, ERenderGraphUse::Write);
		EXPECT_EQ(Capture.Uses[0].Access, ERHIAccess::None);
		EXPECT_TRUE(Capture.Uses[0].bDiscard);
		for (uint32 Index = 0; Index < Colors.size(); ++Index)
		{
			const auto& Use = Capture.Uses[Index + 1];
			EXPECT_EQ(Use.ResourceId, Index);
			EXPECT_EQ(Use.Use, ERenderGraphUse::ReadWrite);
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
		EXPECT_TRUE(Result.Graph->Execute(GetCommandList()));
		EXPECT_EQ(CallbackCount, 1u);
	}

	TEST_F(FRenderGraphTests, GBufferManualDeclarationOracleKeepsBackingFailureAtomic)
	{
		FRenderGraphBuilder Builder;
		Builder.SetBackingResolver([](auto, auto&, std::string&) { return true; });
		const auto Material = Builder.CreateTexture("Scene.GBuffer.Material",
			FRenderGraphTextureDesc{
				.Texture = FRHITextureCreateDesc::Create2D("Scene.GBuffer.Material",
					64, 64, EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource),
				.BackingClass = "renderer.gbuffer"},
			ERHIAccess::GraphicsShaderRead);
		bool bExecuted = false;
		const auto Pass = Builder.AddPass("Scene.GBuffer",
			ERenderGraphPassType::Graphics,
			[&](FRHICommandListImmediate&,
				const FRenderGraphPassResources&) { bExecuted = true; });
		Builder.UseManagedColorAttachment(Pass, Material, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		std::string Error;
		EXPECT_FALSE(Result.Graph->Execute(GetCommandList(), &Error));
		EXPECT_FALSE(bExecuted);
		EXPECT_NE(Error.find("omitted retained resource 'Scene.GBuffer.Material'"),
			std::string::npos);
	}

	TEST_F(FRenderGraphTests, EnforcesDeterministicStructuralBudgets)
	{
		FRenderGraphBuilder Builder;
		Builder.SetBudget({.MaxPasses = 1});
		Builder.AddPass("First", ERenderGraphPassType::Graphics);
		Builder.AddPass("Second", ERenderGraphPassType::Graphics);
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_EQ(Result.Error,
			"render graph safety limit exceeded: passes actual=2 limit=1");
	}

	TEST_F(FRenderGraphTests, ReportsStructuralRegressionBudgetsWithoutRejectingGraph)
	{
		FRenderGraphBuilder Builder;
		Builder.SetBudget({
			.MaxPasses = 8,
			.RegressionMaxPasses = 1,
		});
		Builder.AddPass("First", ERenderGraphPassType::Graphics);
		Builder.AddPass("Second", ERenderGraphPassType::Graphics);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const FRenderGraphStatistics Statistics = Result.Graph->GetStatistics();
		EXPECT_TRUE(Statistics.bPassRegressionBudgetExceeded);
		EXPECT_TRUE(Statistics.IsStructuralRegressionBudgetExceeded());
		EXPECT_EQ(Result.Graph->Capture().Budget.RegressionMaxPasses, 1u);
	}

	TEST_F(FRenderGraphTests, CaptureOwnsPointerFreeDiagnosticsBeyondGraphLifetime)
	{
		FRenderGraphCapture Capture;
		{
			FRenderGraphBuilder Builder;
			Builder.EnablePassCulling();
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"Value", "scene-result");
			auto Write = Builder.AllocParameters<FTypedValueWriteParameters>();
			Write->Output = {Value};
			Builder.AddPass("Produce", ERenderGraphPassType::Compute,
				std::move(Write));
			auto Read = Builder.AllocParameters<FTypedValueReadParameters>();
			Read->Input = {Value};
			const auto Consume = Builder.AddPass("Consume",
				ERenderGraphPassType::Graphics, std::move(Read));
			Builder.MarkPassRoot(Consume, "present");
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			Capture = Result.Graph->Capture();
			EXPECT_EQ(Capture.Dump, Result.Graph->Dump());
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
			ERenderGraphParameterMemberKind::ValueWrite);
		EXPECT_EQ(Capture.Parameters[1].FieldPath,
			"FTypedValueReadParameters.Input");
		EXPECT_NE(Capture.Dump.find("name=Consume"), std::string::npos);
	}

	TEST_F(FRenderGraphTests, TypedValuesReuseTokenDependencyAndCullingSemantics)
	{
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		const auto Value = Builder.CreateValue<FTypedValuePayload>(
			"Scene.Result", "scene-result");
		bool bProduced = false;
		bool bConsumed = false;
		const auto Produce = Builder.AddPass("Produce",
			ERenderGraphPassType::Compute,
			[Value, &bProduced](FRHICommandListImmediate&,
				const FRenderGraphPassResources& Resources) {
				auto& Payload = Resources.WriteValue(Value);
				EXPECT_EQ(reinterpret_cast<uintptr_t>(&Payload) % alignof(
					FTypedValuePayload), 0u);
				Payload.Value = 41;
				bProduced = true;
			});
		Builder.UseValue(Produce, Value, ERenderGraphUse::Write);
		const auto Consume = Builder.AddPass("Consume",
			ERenderGraphPassType::Graphics,
			[Value, &bConsumed](FRHICommandListImmediate&,
				const FRenderGraphPassResources& Resources) {
				EXPECT_EQ(Resources.ReadValue(Value).Value, 41);
				bConsumed = true;
			});
		Builder.UseValue(Consume, Value, ERenderGraphUse::Read);
		Builder.MarkPassRoot(Consume, "publish");

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetDependencies().size(), 1u);
		EXPECT_EQ(Result.Graph->GetDependencies()[0].Kind,
			ERenderGraphDependencyKind::Value);
		EXPECT_EQ(Result.Graph->GetDependencies()[0].Cause, "Scene.Result");
		const auto Capture = Result.Graph->Capture();
		ASSERT_EQ(Capture.Resources.size(), 1u);
		EXPECT_EQ(Capture.Resources[0].ValueType, "scene-result");
		EXPECT_EQ(Capture.Uses.size(), 2u);
		EXPECT_TRUE(Result.Graph->Execute(GetCommandList()));
		EXPECT_TRUE(bProduced);
		EXPECT_TRUE(bConsumed);
	}

	TEST_F(FRenderGraphTests, ParameterizedTypedValuesExposeExactCapabilities)
	{
		FRenderGraphBuilder Builder;
		const auto Value = Builder.CreateValue<FTypedValuePayload>(
			"Scene.ParameterResult", "scene-result");
		auto Write = Builder.AllocParameters<FTypedValueWriteParameters>();
		Write->Output = {Value};
		Builder.AddPass("Write", ERenderGraphPassType::Compute,
			std::move(Write), [](FRHICommandListImmediate&,
				const FTypedValueWriteParameters& Parameters,
				const FRenderGraphParameterResolver& Resolver) {
				Resolver.WriteValue(Parameters.Output).Value = 73;
			});
		auto Read = Builder.AllocParameters<FTypedValueReadParameters>();
		Read->Input = {Value};
		const auto ReadPass = Builder.AddPass("Read",
			ERenderGraphPassType::Graphics, std::move(Read),
			[](FRHICommandListImmediate&,
				const FTypedValueReadParameters& Parameters,
				const FRenderGraphParameterResolver& Resolver) {
				EXPECT_EQ(Resolver.ReadValue(Parameters.Input).Value, 73);
			});
		Builder.MarkPassRoot(ReadPass, "publish");

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const auto Capture = Result.Graph->Capture();
		ASSERT_EQ(Capture.Uses.size(), 2u);
		ASSERT_EQ(Capture.Parameters.size(), 2u);
		EXPECT_EQ(Capture.Parameters[0].Kind,
			ERenderGraphParameterMemberKind::ValueWrite);
		EXPECT_EQ(Capture.Parameters[0].ResourceId, Capture.Uses[0].ResourceId);
		EXPECT_EQ(Capture.Parameters[1].Kind,
			ERenderGraphParameterMemberKind::ValueRead);
		EXPECT_EQ(Capture.Uses[0].ParameterPath,
			"FTypedValueWriteParameters.Output");
		EXPECT_EQ(Capture.Uses[1].ParameterPath,
			"FTypedValueReadParameters.Input");
		EXPECT_TRUE(Result.Graph->Execute(GetCommandList()));
	}

	TEST_F(FRenderGraphTests, TypedValuesRejectInvalidWriterAndTypeContracts)
	{
		{
			FRenderGraphBuilder Builder;
			const auto Value = Builder.CreateValue<int>(
				"MissingWriter", "signed-int", 0);
			const auto Read = Builder.AddPass("Read",
				ERenderGraphPassType::Graphics);
			Builder.UseValue(Read, Value, ERenderGraphUse::Read);
			auto Result = Builder.Compile();
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_EQ(Result.Error, "typed value 'MissingWriter' type 'signed-int' "
				"requires exactly one writer; actual=0");
		}
		{
			FRenderGraphBuilder Builder;
			const auto Value = Builder.CreateValue<int>(
				"DuplicateWriter", "signed-int", 0);
			for (const char* Name : {"First", "Second"})
			{
				const auto Pass = Builder.AddPass(Name,
					ERenderGraphPassType::Compute);
				Builder.UseValue(Pass, Value, ERenderGraphUse::Write);
			}
			auto Result = Builder.Compile();
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_EQ(Result.Error, "typed value 'DuplicateWriter' type 'signed-int' "
				"requires exactly one writer; actual=2");
		}
		{
			FRenderGraphBuilder Builder;
			const auto Value = Builder.CreateValue<int>(
				"WrongType", "signed-int", 0);
			const auto Wrong = std::bit_cast<TRenderGraphValueHandle<float>>(Value);
			const auto Pass = Builder.AddPass("Write",
				ERenderGraphPassType::Compute);
			Builder.UseValue(Pass, Wrong, ERenderGraphUse::Write);
			auto Result = Builder.Compile();
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_EQ(Result.Error, "pass 'Write' declares an invalid, foreign, or "
				"wrongly typed graph value");
		}
	}

	TEST_F(FRenderGraphTests, TypedValueStorageTransfersAndDestroysExactlyOnce)
	{
		int BuilderDestructions = 0;
		{
			FRenderGraphBuilder Builder;
			Builder.CreateValue<FTypedValuePayload>("BuilderOwned", "tracked",
				&BuilderDestructions);
		}
		EXPECT_EQ(BuilderDestructions, 1);

		int CompileFailureDestructions = 0;
		{
			FRenderGraphBuilder Builder;
			Builder.CreateValue<FTypedValuePayload>("CompileFailure", "tracked",
				&CompileFailureDestructions);
			EXPECT_FALSE(Builder.Compile().IsSuccess());
			EXPECT_EQ(CompileFailureDestructions, 0);
		}
		EXPECT_EQ(CompileFailureDestructions, 1);

		int GraphDestructions = 0;
		{
			FRenderGraphBuilder Builder;
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"GraphOwned", "tracked",
				&GraphDestructions);
			const auto Write = Builder.AddPass("Write",
				ERenderGraphPassType::Compute);
			Builder.UseValue(Write, Value, ERenderGraphUse::Write);
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			EXPECT_EQ(GraphDestructions, 0);
			Result.Graph.reset();
			EXPECT_EQ(GraphDestructions, 1);
		}
		EXPECT_EQ(GraphDestructions, 1);

		int CulledDestructions = 0;
		{
			FRenderGraphBuilder Builder;
			Builder.EnablePassCulling();
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"Culled", "tracked", &CulledDestructions);
			const auto Write = Builder.AddPass("CulledWrite",
				ERenderGraphPassType::Compute);
			Builder.UseValue(Write, Value, ERenderGraphUse::Write);
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			EXPECT_TRUE(Result.Graph->GetPasses().empty());
			EXPECT_EQ(CulledDestructions, 0);
		}
		EXPECT_EQ(CulledDestructions, 1);

		int PreparationFailureDestructions = 0;
		{
			FRenderGraphBuilder Builder;
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"PreparationFailure", "tracked",
				&PreparationFailureDestructions);
			const auto Write = Builder.AddPass("Write",
				ERenderGraphPassType::Compute);
			Builder.UseValue(Write, Value, ERenderGraphUse::Write);
			Builder.MarkPassRoot(Write, "publish");
			Builder.SetExecutionPreparation([](std::string& Error) {
				Error = "injected preparation failure";
				return false;
			});
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			std::string Error;
			EXPECT_FALSE(Result.Graph->Execute(
				GetCommandList(), &Error));
			EXPECT_EQ(Error, "injected preparation failure");
			EXPECT_EQ(PreparationFailureDestructions, 0);
		}
		EXPECT_EQ(PreparationFailureDestructions, 1);
	}

	TEST_F(FRenderGraphTests, TypedValueResolutionRejectsWrongDirectionAndCopies)
	{
		{
			FRenderGraphBuilder Builder;
			const auto Value = Builder.CreateValue<int>(
				"WrongDirection", "signed-int", 0);
			const auto Write = Builder.AddPass("Write",
				ERenderGraphPassType::Compute,
				[Value](FRHICommandListImmediate&,
					const FRenderGraphPassResources& Resources) {
					(void)Resources.ReadValue(Value);
				});
			Builder.UseValue(Write, Value, ERenderGraphUse::Write);
			Builder.MarkPassRoot(Write, "publish");
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
				"wrong-direction capability");
		}
		{
			FRenderGraphBuilder Builder;
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"CopiedParameter", "scene-result");
			auto Parameters =
				Builder.AllocParameters<FTypedValueWriteParameters>();
			Parameters->Output = {Value};
			const auto Write = Builder.AddPass("Write",
				ERenderGraphPassType::Compute, std::move(Parameters),
				[](FRHICommandListImmediate&,
					const FTypedValueWriteParameters& Submitted,
					const FRenderGraphParameterResolver& Resolver) {
					auto Copy = Submitted.Output;
					(void)Resolver.WriteValue(Copy);
				});
			Builder.MarkPassRoot(Write, "publish");
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
				"not declared by the executing pass parameters");
		}
	}

	TEST_F(FRenderGraphTests, PrecompileFallbackSelectionCapturesOnlyChosenImport)
	{
		for (const bool bCandidateReady : {false, true})
		{
			auto Candidate = MakeGraphTexture("Candidate");
			auto Fallback = MakeGraphTexture("Fallback");
			FRHITexture* Selected = bCandidateReady ? &Candidate : &Fallback;
			FRenderGraphBuilder Builder;
			const auto Input = Builder.ImportTexture("Selected.Environment",
				Selected, ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<
				FComposedTextureArrayParameters>();
			Parameters->Textures[0] = FRenderGraphTextureParameter{
				Input, WholeColor()};
			Parameters->Textures[1] = std::nullopt;
			const auto Pass = Builder.AddPass("Consume",
				ERenderGraphPassType::Graphics, std::move(Parameters));
			Builder.MarkPassRoot(Pass, "publish");
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			const auto Capture = Result.Graph->Capture();
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
