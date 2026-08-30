#include "RDG.h"

#include "RHICommandList.h"
#include "Shader/Shader.h"

#include <gtest/gtest.h>

#include <chrono>
#include <bit>

namespace Durin
{
	namespace
	{
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
			bool bOmitResources = false;
			uint32 AllocationCount = 0;
			std::vector<FTextureRHIRef> CreatedTextures;
			FTextureRHIRef TextureOverride;
			FBufferRHIRef BufferOverride;

			auto Allocate(std::span<const FRDGAllocationRequest> Requests,
				FRDGAllocatedResources& OutResources, std::string& OutError)
				-> bool override
			{
				if (bFail)
				{
					OutError = "injected allocation failure";
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
						auto Texture = TextureOverride
							? TextureOverride : MakeRefCount<FRHITexture>(Desc);
						CreatedTextures.push_back(Texture);
						if (!OutResources.SetTexture(Request.ResourceId,
							std::move(Texture), AllocationCount)) return false;
					}
					else
					{
						auto Buffer = BufferOverride
							? BufferOverride : MakeRefCount<FRHIBuffer>(
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
					MakeRDGShaderResourceParameterMemberMetadata<
						FComposedTextureArrayParameters, decltype(Textures),
						FRDGTextureParameter>("Textures",
							offsetof(FComposedTextureArrayParameters, Textures),
							ERDGParameterMemberKind::Texture,
							ERDGResourceKind::Texture,
							ERDGParameterRangeKind::TextureSubresource,
							ERDGUse::Read,
							ERHIAccess::GraphicsShaderRead,
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
					MakeRDGShaderResourceParameterMemberMetadata<
						FMalformedComposedAccessParameters, decltype(Texture),
						FRDGTextureParameter>("Texture",
							offsetof(FMalformedComposedAccessParameters, Texture),
							ERDGParameterMemberKind::Texture,
							ERDGResourceKind::Texture,
							ERDGParameterRangeKind::TextureSubresource,
							ERDGUse::Read,
							ERHIAccess::GraphicsShaderRead,
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
					MakeRDGShaderResourceParameterMemberMetadata<
						FComposedComputeBufferParameters, decltype(InputBuffer),
						FRDGBufferParameter>("InputBuffer",
							offsetof(FComposedComputeBufferParameters, InputBuffer),
							ERDGParameterMemberKind::Buffer,
							ERDGResourceKind::Buffer,
							ERDGParameterRangeKind::BufferBytes,
							ERDGUse::Read, ERHIAccess::ComputeShaderRead,
							ERHIBindingType::StorageBuffer),
					MakeRDGShaderResourceParameterMemberMetadata<
						FComposedComputeBufferParameters, decltype(OutputBuffer),
						FRDGBufferParameter>("OutputBuffer",
							offsetof(FComposedComputeBufferParameters, OutputBuffer),
							ERDGParameterMemberKind::Buffer,
							ERDGResourceKind::Buffer,
							ERDGParameterRangeKind::BufferBytes,
							ERDGUse::Write,
							ERHIAccess::ComputeShaderReadWrite,
							ERHIBindingType::StorageBuffer, nullptr, true),
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
		FRHITexture TextureA = MakeGraphTexture("ComposedA", 2);
		FRHITexture TextureB = MakeGraphTexture("ComposedB", 2);
		FRDGBuilder Builder;
		const auto HandleA = Builder.ImportTexture("A", &TextureA,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto HandleB = Builder.ImportTexture("B", &TextureB,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
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

	TEST_F(FRDGTests,
		ComposedGraphMetadataRejectsAccessWeakeningBeforePassPublication)
	{
		FRDGBuilder Builder;
		auto Parameters = Builder.AllocParameters<
			FMalformedComposedAccessParameters>();
		EXPECT_FALSE(Parameters);
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("incompatible graph/shader declaration"),
			std::string::npos);
	}

	TEST_F(FRDGTests,
		ComposedShaderSubmissionRejectsReflectionArrayExtentBeforeRecording)
	{
		FRHITexture TextureA = MakeGraphTexture("BindingExtentA");
		FRHITexture TextureB = MakeGraphTexture("BindingExtentB");
		FRDGBuilder Builder;
		const auto HandleA = Builder.ImportTexture("TextureA", &TextureA,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto HandleB = Builder.ImportTexture("TextureB", &TextureB,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
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
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
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
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
			"is unavailable for required shader");
	}

	TEST_F(FRDGTests,
		ComposedShaderSubmissionRejectsMissingGraphAuthorityAndWrongDomain)
	{
		auto MakeResult = [](bool bWrongDomain) {
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
		Builder.AddPass("ComposedBuffers", ERDGPassType::Compute,
			std::move(Parameters),
			[](FRHICommandListImmediate&,
				const FComposedComputeBufferParameters&,
				const FRDGParameterResolver&) {});
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const auto Capture = Result.Graph->Capture();
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

		FRHITexture Texture = MakeGraphTexture("ParameterTexture", 2);
		const auto TextureHandle = Builder.CreateTexture("ParameterTexture", &Texture);
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

	TEST_F(FRDGTests, RejectsMalformedGraphParameterMetadataAtomically)
	{
		FRDGBuilder Builder;
		auto Parameters = Builder.AllocParameters<FMalformedGraphParameters>();
		EXPECT_FALSE(Parameters.IsValid());
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_EQ(Result.Error,
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
			Builder.AddPass("Rejected", ERDGPassType::Graphics);
			auto Result = Builder.Compile();
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_TRUE(Parameters.IsValid());
			EXPECT_TRUE(DestructionOrder.empty());
		}
		GParameterDestructionOrder = nullptr;
		EXPECT_EQ(DestructionOrder, (std::vector<int>{1}));
	}

	TEST_F(FRDGTests, TransfersParametersToCompiledGraphLifetime)
	{
		std::vector<int> DestructionOrder;
		GParameterDestructionOrder = &DestructionOrder;
		TRDGParametersRef<FFirstLifetimeGraphParameters> Parameters;
		std::unique_ptr<FRDGCompiledGraph> Graph;
		{
			FRDGBuilder Builder;
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

	TEST_F(FRDGTests, KeepsTransferredParametersAcrossExecutionFailure)
	{
		std::vector<int> DestructionOrder;
		GParameterDestructionOrder = &DestructionOrder;
		FRDGBuilder Builder;
		auto Parameters = Builder.AllocParameters<FFirstLifetimeGraphParameters>();
		const auto Texture = Builder.CreateTexture(FRDGTextureDesc{
				.Texture = FRHITextureCreateDesc::Create2D("MissingBacking", 16, 16,
					EPixelFormat::RGBA8_UNORM)}, "MissingBacking");
		const auto Pass = Builder.AddPass("UseMissingBacking",
			ERDGPassType::Graphics);
		Builder.UseColorAttachment(Pass, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		FTestRDGAllocator Allocator;
		Allocator.bOmitResources = true;
		FRDGExecutionContext Context{Allocator};
		EXPECT_FALSE(Result.Graph->Execute(GetCommandList(), Context));
		EXPECT_TRUE(Parameters.IsValid());
		EXPECT_TRUE(DestructionOrder.empty());
		Result.Graph.reset();
		EXPECT_FALSE(Parameters.IsValid());
		GParameterDestructionOrder = nullptr;
		EXPECT_EQ(DestructionOrder, (std::vector<int>{1}));
	}

	TEST_F(FRDGTests, ParameterizedPassMatchesEveryManualUseKind)
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

			FRDGBuilder Builder;
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
				const auto Pass = Builder.AddPass("AllUses",
					ERDGPassType::Graphics, std::move(Parameters));
				EXPECT_TRUE(Pass.IsValid());
			}
			else
			{
				const auto Pass = Builder.AddPass(
					"AllUses", ERDGPassType::Graphics);
				Builder.UseTexture(Pass, Input, WholeColor(),
					ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
				Builder.UseBuffer(Pass, BufferHandle, 32, 128,
					ERDGUse::ReadWrite,
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
					ERDGUse::Write,
					ERHIAccess::GraphicsShaderReadWrite,
					ERHIAccess::GraphicsShaderRead, true);
				Builder.UseToken(Pass, Completion, ERDGUse::Write);
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
		FRHITexture Texture = MakeGraphTexture("Texture");
		FRHITexture ForeignTexture = MakeGraphTexture("ForeignTexture");
		FRDGBuilder ForeignBuilder;
		const auto Foreign = ForeignBuilder.CreateTexture(
			"ForeignTexture", &ForeignTexture);

		{
			FRDGBuilder Builder;
			const auto Local = Builder.ImportTexture("Texture", &Texture,
				ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
			Parameters->Textures = {{{Local, WholeColor()},
				{Foreign, WholeColor()}}};
			EXPECT_FALSE(Builder.AddPass("ForeignHandle",
				ERDGPassType::Graphics, std::move(Parameters)).IsValid());
			auto Result = Builder.Compile();
			EXPECT_EQ(Result.Error,
				"pass 'ForeignHandle' parameter 'FTwoTextureGraphParameters.Textures[1]' "
				"has an invalid resource handle");
		}

		{
			FRDGBuilder Builder;
			const auto Local = Builder.ImportTexture("Texture", &Texture,
				ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
			Parameters->Textures = {{{Local,
				{ERHITextureAspect::Color, 1, 1, 0, 1}},
				{Local, WholeColor()}}};
			EXPECT_FALSE(Builder.AddPass("InvalidRange",
				ERDGPassType::Graphics, std::move(Parameters)).IsValid());
			auto Result = Builder.Compile();
			EXPECT_EQ(Result.Error,
				"pass 'InvalidRange' parameter 'FTwoTextureGraphParameters.Textures[0]' "
				"resource 'Texture' has invalid texture range");
		}

		{
			auto BuildOverlapError = [&] {
				FRDGBuilder Builder;
				const auto Local = Builder.ImportTexture("Texture", &Texture,
					ERHIAccess::GraphicsShaderRead,
					ERHIAccess::GraphicsShaderRead);
				auto Parameters =
					Builder.AllocParameters<FTwoTextureGraphParameters>();
				Parameters->Textures = {{{Local, WholeColor()},
					{Local, {ERHITextureAspect::Color, 0, 1, 0, 1}}}};
				EXPECT_FALSE(Builder.AddPass("Overlap",
					ERDGPassType::Graphics,
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
			FRDGBuilder Builder;
			const auto Local = Builder.ImportTexture("Texture", &Texture,
				ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<FTwoTextureGraphParameters>();
			Parameters->Textures = {{{Local, WholeColor()},
				{Local, WholeColor()}}};
			EXPECT_FALSE(Builder.AddPass("WrongDomain",
				ERDGPassType::Compute, std::move(Parameters)).IsValid());
			auto Result = Builder.Compile();
			EXPECT_EQ(Result.Error,
				"pass 'WrongDomain' parameter 'FTwoTextureGraphParameters.Textures[0]' "
				"resource 'Texture' access is incompatible with pass domain");
		}
	}

	TEST_F(FRDGTests, ParameterizedPassRejectsMixedAndConsumedAuthority)
	{
		{
			FRDGBuilder Builder;
			const auto Token = Builder.CreateToken("Token");
			auto Parameters = Builder.AllocParameters<FNestedGraphParameters>();
			Parameters->Completion = {Token};
			const auto Pass = Builder.AddPass("Parameterized",
				ERDGPassType::Graphics, std::move(Parameters));
			ASSERT_TRUE(Pass.IsValid());
			Builder.UseToken(Pass, Token, ERDGUse::Write);
			auto Result = Builder.Compile();
			EXPECT_EQ(Result.Error,
				"pass 'Parameterized' uses parameter declarations and cannot accept manual uses");
		}

		{
			FRDGBuilder Builder;
			const auto Token = Builder.CreateToken("Token");
			auto Parameters = Builder.AllocParameters<FNestedGraphParameters>();
			Parameters->Completion = {Token};
			EXPECT_TRUE(Builder.AddPass("First", ERDGPassType::Graphics,
				std::move(Parameters)).IsValid());
			EXPECT_FALSE(Parameters.IsValid());
			EXPECT_FALSE(Builder.AddPass("Second", ERDGPassType::Graphics,
				std::move(Parameters)).IsValid());
			auto Result = Builder.Compile();
			EXPECT_EQ(Result.Error,
				"pass 'Second' parameter 'FNestedGraphParameters' has an invalid or "
				"foreign parameter allocation");
		}

		{
			FRDGBuilder Owner;
			auto Parameters = Owner.AllocParameters<FNestedGraphParameters>();
			FRDGBuilder Other;
			EXPECT_FALSE(Other.AddPass("ForeignAllocation",
				ERDGPassType::Graphics, std::move(Parameters)).IsValid());
			auto Result = Other.Compile();
			EXPECT_EQ(Result.Error,
				"pass 'ForeignAllocation' parameter 'FNestedGraphParameters' has an "
				"invalid or foreign parameter allocation");
		}
	}

	TEST_F(FRDGTests, ParameterizedDeclarationFailureIsCallbackAtomic)
	{
		FRHITexture Texture = MakeGraphTexture("Texture");
		FRDGBuilder Builder;
		const auto Local = Builder.ImportTexture("Texture", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
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
		auto Result = Builder.Compile();
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
		const auto Pass = Builder.AddPass("LargeParameters",
			ERDGPassType::Graphics, std::move(Parameters));
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

	TEST_F(FRDGTests, ParameterResolverResolvesDeclaredGraphicsResources)
	{
		FRHITexture InputTexture = MakeGraphTexture("Input");
		FRHITexture ColorTexture = MakeGraphTexture("Color");
		FRHITexture DepthTexture(FRHITextureCreateDesc::Create2D(
			"Depth", 64, 64, EPixelFormat::D32)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable));
		FRHITexture ManagedTexture = MakeGraphTexture("ManagedTexture");
		FRDGBuilder Builder;
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
		Builder.AddPass("ResolveGraphics", ERDGPassType::Graphics,
			std::move(Parameters),
			[&](FRHICommandListImmediate&,
				const FGraphParameterLayoutFixture& Values,
				const FRDGParameterResolver& Resolver) {
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

	TEST_F(FRDGTests, ParameterResolverSupportsComputeAndCopyDomains)
	{
		FRHITexture ComputeTexture = MakeGraphTexture("Compute");
		FRHITexture CopyTexture = MakeGraphTexture("Copy");
		FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
			"Buffer", 64, 4, EBufferUsageFlags::UnorderedAccess));
		FRDGBuilder Builder;
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
		Builder.AddPass("ResolveCompute", ERDGPassType::Compute,
			std::move(ComputeParameters),
			[&](FRHICommandListImmediate&, const FComputeResolutionParameters& Values,
				const FRDGParameterResolver& Resolver) {
				EXPECT_EQ(Resolver.GetTexture(Values.Texture), &ComputeTexture);
				EXPECT_EQ(Resolver.GetBuffer(Values.Buffer), &Buffer);
				++CallbackCount;
			});
		auto CopyParameters = Builder.AllocParameters<FCopyResolutionParameters>();
		CopyParameters->Texture = {CopyTextureHandle, WholeColor()};
		Builder.AddPass("ResolveCopy", ERDGPassType::Copy,
			std::move(CopyParameters),
			[&](FRHICommandListImmediate&, const FCopyResolutionParameters& Values,
				const FRDGParameterResolver& Resolver) {
				EXPECT_EQ(Resolver.GetTexture(Values.Texture), &CopyTexture);
				++CallbackCount;
			});
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_TRUE(Result.Graph->Execute(GetCommandList()));
		EXPECT_EQ(CallbackCount, 2u);
	}

	TEST_F(FRDGTests, ParameterResolverRejectsRawWrongKindAndWrongPassAccess)
	{
		static_assert(!CTextureResolverArgument<FRDGTextureHandle>);
		static_assert(!CTextureResolverArgument<FRDGBufferParameter>);
		FRHITexture FirstTexture = MakeGraphTexture("First", 2);
		FRHITexture SecondTexture = MakeGraphTexture("Second", 2);
		FRDGBuilder Builder;
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
		Builder.AddPass("FirstPass", ERDGPassType::Graphics,
			std::move(FirstParameters),
			[WrongPassMember](FRHICommandListImmediate&,
				const FTwoTextureGraphParameters&,
				const FRDGParameterResolver& Resolver) {
				Resolver.GetTexture(*WrongPassMember);
			});
		Builder.AddPass("SecondPass", ERDGPassType::Graphics,
			std::move(SecondParameters));
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
			"pass 'FirstPass'.*requested capability 'texture'");
	}

	TEST_F(FRDGTests, ParameterResolverRejectsCopiedAndForeignOptionalMembers)
	{
		FRHITexture Texture = MakeGraphTexture("Declared", 2);
		FRDGBuilder Builder;
		const auto Handle = Builder.ImportTexture("Declared", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
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
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
			"not declared by the executing pass parameters");

		// An empty optional is nullable only when that exact optional object is a
		// declared field; a foreign empty optional remains an invalid capability.
		FRDGBuilder OptionalBuilder;
		const auto OptionalHandle = OptionalBuilder.ImportTexture("Declared",
			&Texture, ERHIAccess::GraphicsShaderRead,
			ERHIAccess::GraphicsShaderRead);
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
		auto OptionalResult = OptionalBuilder.Compile();
		ASSERT_TRUE(OptionalResult.IsSuccess()) << OptionalResult.Error;
		EXPECT_DEATH(OptionalResult.Graph->Execute(
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
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			FTestRDGAllocator Allocator;
			Allocator.bOmitResources = true;
			FRDGExecutionContext Context{Allocator};
			std::string Error;
			EXPECT_FALSE(Result.Graph->Execute(
				GetCommandList(), Context, &Error));
			EXPECT_NE(Error.find("omitted retained resource id="),
				std::string::npos);
			EXPECT_FALSE(bExecuted);
		}
	}

	TEST_F(FRDGTests, CompilesStableHazardOrderAndExactTextureTransitions)
	{
		FRHITexture Texture = MakeGraphTexture("SceneColor");
		FRDGBuilder Builder;
		const auto SceneColor = Builder.CreateTexture(
			"SceneColor", &Texture, ERHIAccess::GraphicsShaderRead);
		const auto Independent = Builder.AddPass(
			"Independent", ERDGPassType::Copy);
		const auto Produce = Builder.AddPass(
			"Produce", ERDGPassType::Graphics);
		Builder.UseColorAttachment(Produce, SceneColor, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		const auto Consume = Builder.AddPass(
			"Consume", ERDGPassType::Compute);
		Builder.UseTexture(Consume, SceneColor, WholeColor(),
			ERDGUse::Read, ERHIAccess::ComputeShaderRead);

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 3u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].Name, "Independent");
		EXPECT_EQ(Result.Graph->GetPasses()[1].Name, "Produce");
		EXPECT_EQ(Result.Graph->GetPasses()[2].Name, "Consume");
		ASSERT_EQ(Result.Graph->GetDependencies().size(), 1u);
		EXPECT_EQ(Result.Graph->GetDependencies()[0],
			(FRDGDependency{1, 2, "SceneColor",
				ERDGDependencyKind::Value}));
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

	TEST_F(FRDGTests, CompilesBufferRawWarAndWawDependencies)
	{
		FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
			"Work", 64, 4, EBufferUsageFlags::UnorderedAccess
				| EBufferUsageFlags::SourceCopy));
		FRDGBuilder Builder;
		const auto Work = Builder.CreateBuffer("Work", &Buffer);
		const auto Write = Builder.AddPass("Write", ERDGPassType::Compute);
		Builder.UseBuffer(Write, Work, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Read = Builder.AddPass("Read", ERDGPassType::Copy);
		Builder.UseBuffer(Read, Work, 0, 64, ERDGUse::Read,
			ERHIAccess::TransferRead);
		const auto Rewrite = Builder.AddPass("Rewrite", ERDGPassType::Compute);
		Builder.UseBuffer(Rewrite, Work, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite);

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetDependencies().size(), 2u);
		EXPECT_EQ(Result.Graph->GetDependencies()[0].Kind,
			ERDGDependencyKind::Value);
		EXPECT_EQ(Result.Graph->GetDependencies()[1].Kind,
			ERDGDependencyKind::Execution);
		EXPECT_EQ(Result.Graph->GetPasses()[0].BufferTransitions[0].ExpectedBefore,
			ERHIAccess::Discard);
		EXPECT_EQ(Result.Graph->GetPasses()[1].BufferTransitions[0].ExpectedBefore,
			ERHIAccess::ComputeShaderReadWrite);
		EXPECT_EQ(Result.Graph->GetPasses()[2].BufferTransitions[0].ExpectedBefore,
			ERHIAccess::TransferRead);
	}

	TEST_F(FRDGTests, RejectsMissingProducerForeignHandleAndCycle)
	{
		FRHITexture Texture = MakeGraphTexture("Missing");
		FRDGBuilder MissingProducer;
		const auto Logical = MissingProducer.CreateTexture("Missing", &Texture);
		const auto Read = MissingProducer.AddPass("Read", ERDGPassType::Graphics);
		MissingProducer.UseTexture(Read, Logical, WholeColor(),
			ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
		auto Missing = MissingProducer.Compile();
		EXPECT_FALSE(Missing.IsSuccess());
		EXPECT_NE(Missing.Error.find("before its producer"), std::string::npos);

		FRDGBuilder ForeignOwner;
		const auto Foreign = ForeignOwner.CreateTexture("Foreign", &Texture);
		FRDGBuilder ForeignUse;
		const auto Pass = ForeignUse.AddPass("Use", ERDGPassType::Graphics);
		ForeignUse.UseTexture(Pass, Foreign, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Invalid = ForeignUse.Compile();
		EXPECT_FALSE(Invalid.IsSuccess());
		EXPECT_NE(Invalid.Error.find("invalid resource handle"), std::string::npos);

		FRDGBuilder Cyclic;
		const auto A = Cyclic.AddPass("A", ERDGPassType::Compute);
		const auto B = Cyclic.AddPass("B", ERDGPassType::Compute);
		Cyclic.AddDependency(A, B);
		Cyclic.AddDependency(B, A);
		auto Cycle = Cyclic.Compile();
		EXPECT_FALSE(Cycle.IsSuccess());
		EXPECT_EQ(Cycle.Error, "graph contains a dependency cycle");

		FRDGBuilder SelfDependent;
		const auto Self = SelfDependent.AddPass(
			"Self", ERDGPassType::Compute);
		SelfDependent.AddDependency(Self, Self);
		auto SelfCycle = SelfDependent.Compile();
		EXPECT_FALSE(SelfCycle.IsSuccess());
		EXPECT_EQ(SelfCycle.Error, "graph contains a dependency cycle");
	}

	TEST_F(FRDGTests, RejectsTextureAspectsOutsideResourceFormat)
	{
		FRHITexture Texture = MakeGraphTexture("ColorOnly");
		FRDGBuilder Builder;
		const auto Resource = Builder.CreateTexture("ColorOnly", &Texture);
		const auto Pass = Builder.AddPass("InvalidAspects",
			ERDGPassType::Compute);
		Builder.UseTexture(Pass, Resource,
			{ERHITextureAspect::Color | ERHITextureAspect::Depth, 0, 1, 0, 1},
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);

		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("invalid texture range"), std::string::npos);
	}

	TEST_F(FRDGTests, NormalizesDisjointAndPartiallyOverlappingSubresources)
	{
		FRHITexture Texture = MakeGraphTexture("MipChain", 4);
		FRDGBuilder Builder;
		const auto Chain = Builder.CreateTexture("MipChain", &Texture);
		const auto Mip0 = Builder.AddPass("Mip0", ERDGPassType::Compute);
		Builder.UseTexture(Mip0, Chain, {ERHITextureAspect::Color, 0, 1, 0, 1},
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto Mip1 = Builder.AddPass("Mip1", ERDGPassType::Compute);
		Builder.UseTexture(Mip1, Chain, {ERHITextureAspect::Color, 1, 1, 0, 1},
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		auto Disjoint = Builder.Compile();
		ASSERT_TRUE(Disjoint.IsSuccess()) << Disjoint.Error;
		EXPECT_TRUE(Disjoint.Graph->GetDependencies().empty());

		FRDGBuilder Partial;
		const auto PartialChain = Partial.CreateTexture("MipChain", &Texture);
		const auto Whole = Partial.AddPass("Whole", ERDGPassType::Compute);
		Partial.UseTexture(Whole, PartialChain, WholeColor(4),
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto OneMip = Partial.AddPass("OneMip", ERDGPassType::Compute);
		Partial.UseTexture(OneMip, PartialChain,
			{ERHITextureAspect::Color, 1, 1, 0, 1}, ERDGUse::Read,
			ERHIAccess::ComputeShaderRead);
		auto Overlap = Partial.Compile();
		ASSERT_TRUE(Overlap.IsSuccess()) << Overlap.Error;
		ASSERT_EQ(Overlap.Graph->GetDependencies().size(), 1u);
		EXPECT_EQ(Overlap.Graph->GetDependencies()[0].Kind,
			ERDGDependencyKind::Value);
		EXPECT_EQ(Overlap.Graph->GetPasses()[0].TextureTransitions.size(), 3u);
		EXPECT_EQ(Overlap.Graph->GetPasses()[1].TextureTransitions.size(), 1u);
	}

	TEST_F(FRDGTests, DiscardedAttachmentStoreCannotBecomeAProducer)
	{
		FRHITexture Texture = MakeGraphTexture("Discarded");
		FRDGBuilder Builder;
		const auto Target = Builder.CreateTexture("Discarded", &Texture);
		const auto Clear = Builder.AddPass("Clear", ERDGPassType::Graphics);
		Builder.UseColorAttachment(Clear, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::DontCare);
		const auto Read = Builder.AddPass("Read", ERDGPassType::Graphics);
		Builder.UseTexture(Read, Target, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("before its producer"), std::string::npos);
	}

	TEST_F(FRDGTests, PreservesImportedInitialAndFinalStates)
	{
		FRHITexture Texture = MakeGraphTexture("Imported");
		FRDGBuilder Builder;
		const auto Imported = Builder.ImportTexture("Imported", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Compute = Builder.AddPass("Compute", ERDGPassType::Compute);
		Builder.UseTexture(Compute, Imported, WholeColor(),
			ERDGUse::Read, ERHIAccess::ComputeShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses()[0].TextureTransitions.size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].TextureTransitions[0].ExpectedBefore,
			ERHIAccess::GraphicsShaderRead);
		ASSERT_EQ(Result.Graph->GetFinalTextureTransitions().size(), 1u);
		EXPECT_EQ(Result.Graph->GetFinalTextureTransitions()[0].RequiredAfter,
			ERHIAccess::GraphicsShaderRead);
	}

	TEST_F(FRDGTests, RejectsAttachmentLoadWithoutPriorContents)
	{
		FRHITexture Texture = MakeGraphTexture("Load");
		FRDGBuilder Builder;
		const auto Target = Builder.CreateTexture("Load", &Texture);
		const auto Load = Builder.AddPass("Load", ERDGPassType::Graphics);
		Builder.UseColorAttachment(Load, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Load,
			ERHIRenderTargetStoreAction::Store);
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("before its producer"), std::string::npos);
	}

	TEST_F(FRDGTests, DumpIsDeterministicAndSyntheticCompileCostIsBounded)
	{
		auto CompileFixture = [] {
			static FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
				"Fixture", 512, 4, EBufferUsageFlags::UnorderedAccess));
			FRDGBuilder Builder;
			const auto Work = Builder.CreateBuffer("Fixture", &Buffer);
			for (uint32 Index = 0; Index < 128; ++Index)
			{
				const auto Pass = Builder.AddPass("Pass" + std::to_string(Index),
					ERDGPassType::Compute);
				Builder.UseBuffer(Pass, Work, 0, 512, ERDGUse::Write,
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

	TEST_F(FRDGTests, CullsUnreachableBranchesAndReportsExactLifetimes)
	{
		FRHIBuffer RetainedBuffer(FRHIBufferCreateDesc::Create(
			"Retained", 64, 4, EBufferUsageFlags::UnorderedAccess));
		FRHIBuffer CulledBuffer(FRHIBufferCreateDesc::Create(
			"Culled", 64, 4, EBufferUsageFlags::UnorderedAccess));
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Retained = Builder.CreateBuffer("Retained", &RetainedBuffer);
		const auto Culled = Builder.CreateBuffer("Culled", &CulledBuffer);
		const auto Produce = Builder.AddPass("Produce", ERDGPassType::Compute);
		Builder.UseBuffer(Produce, Retained, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Consume = Builder.AddPass("Present", ERDGPassType::Compute);
		Builder.UseBuffer(Consume, Retained, 0, 64, ERDGUse::Read,
			ERHIAccess::ComputeShaderRead);
		Builder.MarkPassRoot(Consume, "present");
		const auto Unused = Builder.AddPass("Unused", ERDGPassType::Compute);
		Builder.UseBuffer(Unused, Culled, 0, 64, ERDGUse::Write,
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

	TEST_F(FRDGTests, CanonicalizesEquivalentImportedIdentity)
	{
		FRHITexture Texture = MakeGraphTexture("Shared");
		FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
			"SharedBuffer", 64, 4, EBufferUsageFlags::UnorderedAccess));
		FRDGBuilder Builder;
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

		FRDGBuilder OtherBuilder;
		const auto Other = OtherBuilder.ImportTexture("Other", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		EXPECT_NE(FirstTexture, Other);
	}

	TEST_F(FRDGTests, RejectsConflictingImportedIdentityAndDomainMismatch)
	{
		FRHITexture Texture = MakeGraphTexture("Shared");
		FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
			"SharedBuffer", 64, 4, EBufferUsageFlags::UnorderedAccess));
		FRDGBuilder Duplicate;
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

		FRDGBuilder BufferConflict;
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

		FRDGBuilder NullImport;
		const auto NullHandle = NullImport.ImportTexture("Null", nullptr,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		EXPECT_TRUE(NullHandle.IsValid());
		auto NullResult = NullImport.Compile();
		EXPECT_FALSE(NullResult.IsSuccess());
		EXPECT_EQ(NullResult.Error, "resource 'Null' has no physical resource");

		FRDGBuilder Domain;
		const auto Imported = Domain.ImportTexture("Shared", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Copy = Domain.AddPass("Copy", ERDGPassType::Copy);
		Domain.UseTexture(Copy, Imported, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto DomainResult = Domain.Compile();
		EXPECT_FALSE(DomainResult.IsSuccess());
		EXPECT_NE(DomainResult.Error.find("incompatible with pass domain"),
			std::string::npos);
	}

	TEST_F(FRDGTests, DiscardValueCullingDoesNotRetainOverwrittenProducer)
	{
		FRHITexture Texture = MakeGraphTexture("Versioned");
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Resource = Builder.CreateTexture("Versioned", &Texture);
		const auto Old = Builder.AddPass("Old", ERDGPassType::Compute);
		Builder.UseTexture(Old, Resource, WholeColor(), ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Replacement = Builder.AddPass("Replacement",
			ERDGPassType::Compute);
		Builder.UseTexture(Replacement, Resource, WholeColor(),
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto Consume = Builder.AddPass("Consume", ERDGPassType::Compute);
		Builder.UseTexture(Consume, Resource, WholeColor(), ERDGUse::Read,
			ERHIAccess::ComputeShaderRead);
		Builder.MarkPassRoot(Consume, "output");
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 2u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].Name, "Replacement");
		EXPECT_TRUE(Result.Graph->GetCullingDecisions()[0].bCulled);
	}

	TEST_F(FRDGTests, RetainedLogicalResourcesPublishExactPreparationCapture)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		FRDGBufferDesc Desc{
			.Buffer = FRHIBufferDesc(64, 4, EBufferUsageFlags::UnorderedAccess)};
		const auto Retained = Builder.CreateBuffer(Desc, "Retained");
		const auto Culled = Builder.CreateBuffer(Desc, "Culled");
		const auto Produce = Builder.AddPass("Produce", ERDGPassType::Compute);
		Builder.UseBuffer(Produce, Retained, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		Builder.MarkPassRoot(Produce, "effect");
		const auto Unused = Builder.AddPass("Unused", ERDGPassType::Compute);
		Builder.UseBuffer(Unused, Culled, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const auto Capture = Result.Graph->Capture();
		ASSERT_EQ(Capture.Resources.size(), 2u);
		EXPECT_EQ(Capture.Resources[0].Preparation, "requested");
		EXPECT_EQ(Capture.Resources[1].Preparation, "culled");
		ASSERT_EQ(Capture.Uses.size(), 1u);
		EXPECT_EQ(Capture.Uses[0].Version, 1u);
	}

	TEST_F(FRDGTests, PassResourceViewRejectsUndeclaredLookup)
	{
		FRHITexture DeclaredTexture = MakeGraphTexture("Declared");
		FRHITexture HiddenTexture = MakeGraphTexture("Hidden");
		FRDGBuilder Builder;
		const auto Declared = Builder.ImportTexture("Declared", &DeclaredTexture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Hidden = Builder.ImportTexture("Hidden", &HiddenTexture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Pass = Builder.AddPass("Pass", ERDGPassType::Graphics,
			[=](FRHICommandListImmediate&, const FRDGPassResources& Resources) {
				Resources.GetTexture(Hidden);
			});
		Builder.UseTexture(Pass, Declared, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
			"undeclared texture");
	}

	TEST_F(FRDGTests, ManagedAttachmentExitStateDrivesFollowingTransition)
	{
		FRHITexture Texture = MakeGraphTexture("Managed");
		FRDGBuilder Builder;
		const auto Target = Builder.CreateTexture("Managed", &Texture);
		const auto Render = Builder.AddPass("Render", ERDGPassType::Graphics);
		Builder.UseManagedColorAttachment(Render, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			ERHIAccess::GraphicsShaderRead);
		const auto Consume = Builder.AddPass("Consume", ERDGPassType::Compute);
		Builder.UseTexture(Consume, Target, WholeColor(), ERDGUse::Read,
			ERHIAccess::ComputeShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_TRUE(Result.Graph->GetPasses()[0].TextureTransitions.empty());
		ASSERT_EQ(Result.Graph->GetPasses()[1].TextureTransitions.size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[1].TextureTransitions[0].ExpectedBefore,
			ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Result.Graph->Capture().Transitions.size(), 3u);
	}

	TEST_F(FRDGTests, IncompleteBackingPublicationRecordsNoCallback)
	{
		bool bExecuted = false;
		FRDGBuilder Builder;
		const auto Buffer = Builder.CreateBuffer(
			FRDGBufferDesc{.Buffer = FRHIBufferDesc(
				64, 4, EBufferUsageFlags::UnorderedAccess)}, "Logical");
		const auto Pass = Builder.AddPass("Write", ERDGPassType::Compute,
			[&](FRHICommandListImmediate&, const FRDGPassResources&) {
				bExecuted = true;
			});
		Builder.UseBuffer(Pass, Buffer, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		FTestRDGAllocator Allocator;
		Allocator.bOmitResources = true;
		FRDGExecutionContext Context{Allocator};
		std::string Error;
		EXPECT_FALSE(Result.Graph->Execute(GetCommandList(), Context, &Error));
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
		const auto FirstPass = Builder.AddPass(
			"First", ERDGPassType::Graphics);
		Builder.UseColorAttachment(FirstPass, First, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		const auto SecondPass = Builder.AddPass(
			"Second", ERDGPassType::Graphics);
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
		const auto Pass = Builder.AddPass("Write", ERDGPassType::Graphics,
			[&](FRHICommandListImmediate&, const FRDGPassResources&) {
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
			const auto Pass = Builder.AddPass("Read", ERDGPassType::Graphics);
			Builder.UseTexture(Pass, External, WholeColor(), ERDGUse::Read,
				ERHIAccess::GraphicsShaderRead);
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			EXPECT_GT(Texture.GetRefCount(), InitialReferences);
			EXPECT_TRUE(Result.Graph->Execute(GetCommandList()));
		}
		EXPECT_EQ(Texture.GetRefCount(), InitialReferences);
	}

	TEST_F(FRDGTests, ExternalAndPreboundCapturesDoNotInventAllocationIdentity)
	{
		auto ExternalTexture = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D(
				"External", 8, 8, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource));
		auto PreboundTexture = MakeRefCount<FRHITexture>(
			FRHITextureCreateDesc::Create2D(
				"Prebound", 8, 8, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable));
		FRDGBuilder Builder;
		const auto External = Builder.RegisterExternalTexture(ExternalTexture,
			"External", ERHIAccess::GraphicsShaderRead,
			ERHIAccess::GraphicsShaderRead);
		const auto Prebound = Builder.CreateTexture(
			"Prebound", PreboundTexture.GetReference(),
			ERHIAccess::GraphicsShaderRead);
		const auto Pass = Builder.AddPass("Read", ERDGPassType::Graphics);
		Builder.UseTexture(Pass, External, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);
		Builder.UseColorAttachment(Pass, Prebound, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const auto Capture = Result.Graph->Capture();
		ASSERT_EQ(Capture.Resources.size(), 2u);
		EXPECT_EQ(Capture.Resources[0].AllocationDisposition, "external");
		EXPECT_EQ(Capture.Resources[0].PhysicalAllocationId, 0u);
		EXPECT_EQ(Capture.Resources[1].AllocationDisposition, "prebound");
		EXPECT_EQ(Capture.Resources[1].PhysicalAllocationId, 0u);
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
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_FALSE(Extracted);
		ASSERT_TRUE(Result.Graph->Execute(GetCommandList()));
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
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("duplicate or conflicting texture extraction"),
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
		const auto Pass = Builder.AddPass("Write", ERDGPassType::Compute);
		Builder.UseBuffer(Pass, Buffer, 0, 64, ERDGUse::Write,
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
		const auto TexturePass = TextureBuilder.AddPass(
			"TextureWrite", ERDGPassType::Graphics);
		TextureBuilder.UseColorAttachment(TexturePass, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		auto TextureResult = TextureBuilder.Compile();
		ASSERT_TRUE(TextureResult.IsSuccess()) << TextureResult.Error;
		FTestRDGAllocator TextureAllocator;
		TextureAllocator.TextureOverride = TextureBacking;
		FRDGExecutionContext TextureContext{TextureAllocator};
		std::string Error;
		EXPECT_FALSE(TextureResult.Graph->Execute(
			GetCommandList(), TextureContext, &Error));
		EXPECT_NE(Error.find("incompatible texture"), std::string::npos);

		auto BufferBacking = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"BufferBacking", 64, 4, EBufferUsageFlags::StructuredBuffer));
		FRDGBuilder BufferBuilder;
		const auto Buffer = BufferBuilder.CreateBuffer(FRDGBufferDesc{
				.Buffer = FRHIBufferDesc(
					64, 4, EBufferUsageFlags::UnorderedAccess)}, "LogicalBuffer");
		const auto BufferPass = BufferBuilder.AddPass(
			"BufferWrite", ERDGPassType::Compute);
		BufferBuilder.UseBuffer(BufferPass, Buffer, 0, 64,
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		auto BufferResult = BufferBuilder.Compile();
		ASSERT_TRUE(BufferResult.IsSuccess()) << BufferResult.Error;
		FTestRDGAllocator BufferAllocator;
		BufferAllocator.BufferOverride = BufferBacking;
		FRDGExecutionContext BufferContext{BufferAllocator};
		EXPECT_FALSE(BufferResult.Graph->Execute(
			GetCommandList(), BufferContext, &Error));
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
		const auto TexturePass = TextureBuilder.AddPass(
			"TextureWrite", ERDGPassType::Graphics);
		TextureBuilder.UseColorAttachment(TexturePass, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		auto TextureResult = TextureBuilder.Compile();
		ASSERT_TRUE(TextureResult.IsSuccess()) << TextureResult.Error;
		FTestRDGAllocator TextureAllocator;
		TextureAllocator.TextureOverride = TextureBacking;
		FRDGExecutionContext TextureContext{TextureAllocator};
		std::string Error;
		EXPECT_TRUE(TextureResult.Graph->Execute(
			GetCommandList(), TextureContext, &Error)) << Error;

		static const auto BufferBacking = MakeRefCount<FRHIBuffer>(
			FRHIBufferCreateDesc::Create(
			"BufferBacking", 64, 4, EBufferUsageFlags::UnorderedAccess
				| EBufferUsageFlags::StructuredBuffer));
		FRDGBuilder BufferBuilder;
		const auto Buffer = BufferBuilder.CreateBuffer(FRDGBufferDesc{
				.Buffer = FRHIBufferDesc(
					64, 4, EBufferUsageFlags::UnorderedAccess)}, "LogicalBuffer");
		const auto BufferPass = BufferBuilder.AddPass(
			"BufferWrite", ERDGPassType::Compute);
		BufferBuilder.UseBuffer(BufferPass, Buffer, 0, 64,
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		auto BufferResult = BufferBuilder.Compile();
		ASSERT_TRUE(BufferResult.IsSuccess()) << BufferResult.Error;
		FTestRDGAllocator BufferAllocator;
		BufferAllocator.BufferOverride = BufferBacking;
		FRDGExecutionContext BufferContext{BufferAllocator};
		EXPECT_TRUE(BufferResult.Graph->Execute(
			GetCommandList(), BufferContext, &Error)) << Error;
	}

	TEST_F(FRDGTests, ExplicitEffectRootSurvivesWithoutResourceOutputs)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Timestamp = Builder.AddPass(
			"Timestamp", ERDGPassType::Graphics);
		Builder.MarkPassRoot(Timestamp, "timestamp");
		Builder.AddPass("Unused", ERDGPassType::Graphics);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].Name, "Timestamp");
	}

	TEST_F(FRDGTests, LogicalTokensDriveDependenciesAndLifetimesWithoutRHIState)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Prepared = Builder.CreateToken("Prepared");
		const auto Output = Builder.CreateToken("Output");
		const auto Prepare = Builder.AddPass("Prepare", ERDGPassType::Graphics);
		Builder.UseToken(Prepare, Prepared, ERDGUse::Write);
		const auto Render = Builder.AddPass("Render", ERDGPassType::Graphics);
		Builder.UseToken(Render, Prepared, ERDGUse::Read);
		Builder.UseToken(Render, Output, ERDGUse::Write);
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

	TEST_F(FRDGTests, GBufferManualDeclarationOracleFreezesCompletePassShape)
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
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		std::array<FRDGTextureHandle, 4> Colors{};
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
			ERDGPassType::Graphics,
			[&](FRHICommandListImmediate&,
				const FRDGPassResources& Resources) {
				for (const auto Color : Colors)
					EXPECT_NE(Resources.GetTexture(Color), nullptr);
				EXPECT_EQ(Resources.GetTexture(Depth), &DepthTexture);
				++CallbackCount;
			});
		Builder.UseToken(Pass, Completion, ERDGUse::Write);
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
		const FRDGCapture Capture = Result.Graph->Capture();
		ASSERT_EQ(Capture.Passes.size(), 1u);
		EXPECT_EQ(Capture.Passes[0].Name, "Scene.GBuffer");
		EXPECT_EQ(Capture.Passes[0].Type, ERDGPassType::Graphics);
		EXPECT_EQ(Capture.Statistics.DeclaredPasses, 1u);
		EXPECT_EQ(Capture.Statistics.ScheduledPasses, 1u);
		EXPECT_EQ(Capture.Statistics.Dependencies, 0u);
		EXPECT_EQ(Capture.Statistics.TextureTransitions, 0u);
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
		const auto Pass = Builder.AddPass("Scene.GBuffer",
			ERDGPassType::Graphics,
			[&](FRHICommandListImmediate&,
				const FRDGPassResources&) { bExecuted = true; });
		Builder.UseManagedColorAttachment(Pass, Material, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		FTestRDGAllocator Allocator;
		Allocator.bOmitResources = true;
		FRDGExecutionContext Context{Allocator};
		std::string Error;
		EXPECT_FALSE(Result.Graph->Execute(GetCommandList(), Context, &Error));
		EXPECT_FALSE(bExecuted);
		EXPECT_NE(Error.find("omitted retained resource id="),
			std::string::npos);
	}

	TEST_F(FRDGTests, EnforcesDeterministicStructuralBudgets)
	{
		FRDGBuilder Builder;
		Builder.SetBudget({.MaxPasses = 1});
		Builder.AddPass("First", ERDGPassType::Graphics);
		Builder.AddPass("Second", ERDGPassType::Graphics);
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_EQ(Result.Error,
			"render graph safety limit exceeded: passes actual=2 limit=1");
	}

	TEST_F(FRDGTests, ReportsStructuralRegressionBudgetsWithoutRejectingGraph)
	{
		FRDGBuilder Builder;
		Builder.SetBudget({
			.MaxPasses = 8,
			.RegressionMaxPasses = 1,
		});
		Builder.AddPass("First", ERDGPassType::Graphics);
		Builder.AddPass("Second", ERDGPassType::Graphics);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const FRDGStatistics Statistics = Result.Graph->GetStatistics();
		EXPECT_TRUE(Statistics.bPassRegressionBudgetExceeded);
		EXPECT_TRUE(Statistics.IsStructuralRegressionBudgetExceeded());
		EXPECT_EQ(Result.Graph->Capture().Budget.RegressionMaxPasses, 1u);
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
			Builder.AddPass("Produce", ERDGPassType::Compute,
				std::move(Write));
			auto Read = Builder.AllocParameters<FTypedValueReadParameters>();
			Read->Input = {Value};
			const auto Consume = Builder.AddPass("Consume",
				ERDGPassType::Graphics, std::move(Read));
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
		const auto Produce = Builder.AddPass("Produce",
			ERDGPassType::Compute,
			[Value, &bProduced](FRHICommandListImmediate&,
				const FRDGPassResources& Resources) {
				auto& Payload = Resources.WriteValue(Value);
				EXPECT_EQ(reinterpret_cast<uintptr_t>(&Payload) % alignof(
					FTypedValuePayload), 0u);
				Payload.Value = 41;
				bProduced = true;
			});
		Builder.UseValue(Produce, Value, ERDGUse::Write);
		const auto Consume = Builder.AddPass("Consume",
			ERDGPassType::Graphics,
			[Value, &bConsumed](FRHICommandListImmediate&,
				const FRDGPassResources& Resources) {
				EXPECT_EQ(Resources.ReadValue(Value).Value, 41);
				bConsumed = true;
			});
		Builder.UseValue(Consume, Value, ERDGUse::Read);
		Builder.MarkPassRoot(Consume, "publish");

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetDependencies().size(), 1u);
		EXPECT_EQ(Result.Graph->GetDependencies()[0].Kind,
			ERDGDependencyKind::Value);
		EXPECT_EQ(Result.Graph->GetDependencies()[0].Cause, "Scene.Result");
		const auto Capture = Result.Graph->Capture();
		ASSERT_EQ(Capture.Resources.size(), 1u);
		EXPECT_EQ(Capture.Resources[0].ValueType, "scene-result");
		EXPECT_EQ(Capture.Uses.size(), 2u);
		EXPECT_TRUE(Result.Graph->Execute(GetCommandList()));
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

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const auto Capture = Result.Graph->Capture();
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
		EXPECT_TRUE(Result.Graph->Execute(GetCommandList()));
	}

	TEST_F(FRDGTests, TypedValuesRejectInvalidWriterAndTypeContracts)
	{
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<int>(
				"MissingWriter", "signed-int", 0);
			const auto Read = Builder.AddPass("Read",
				ERDGPassType::Graphics);
			Builder.UseValue(Read, Value, ERDGUse::Read);
			auto Result = Builder.Compile();
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_EQ(Result.Error, "typed value 'MissingWriter' type 'signed-int' "
				"requires exactly one writer; actual=0");
		}
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<int>(
				"DuplicateWriter", "signed-int", 0);
			for (const char* Name : {"First", "Second"})
			{
				const auto Pass = Builder.AddPass(Name,
					ERDGPassType::Compute);
				Builder.UseValue(Pass, Value, ERDGUse::Write);
			}
			auto Result = Builder.Compile();
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_EQ(Result.Error, "typed value 'DuplicateWriter' type 'signed-int' "
				"requires exactly one writer; actual=2");
		}
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<int>(
				"WrongType", "signed-int", 0);
			const auto Wrong = std::bit_cast<TRDGValueHandle<float>>(Value);
			const auto Pass = Builder.AddPass("Write",
				ERDGPassType::Compute);
			Builder.UseValue(Pass, Wrong, ERDGUse::Write);
			auto Result = Builder.Compile();
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_EQ(Result.Error, "pass 'Write' declares an invalid, foreign, or "
				"wrongly typed graph value");
		}
	}

	TEST_F(FRDGTests, TypedValueStorageTransfersAndDestroysExactlyOnce)
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
			EXPECT_FALSE(Builder.Compile().IsSuccess());
			EXPECT_EQ(CompileFailureDestructions, 0);
		}
		EXPECT_EQ(CompileFailureDestructions, 1);

		int GraphDestructions = 0;
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"GraphOwned", "tracked",
				&GraphDestructions);
			const auto Write = Builder.AddPass("Write",
				ERDGPassType::Compute);
			Builder.UseValue(Write, Value, ERDGUse::Write);
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			EXPECT_EQ(GraphDestructions, 0);
			Result.Graph.reset();
			EXPECT_EQ(GraphDestructions, 1);
		}
		EXPECT_EQ(GraphDestructions, 1);

		int CulledDestructions = 0;
		{
			FRDGBuilder Builder;
			Builder.EnablePassCulling();
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"Culled", "tracked", &CulledDestructions);
			const auto Write = Builder.AddPass("CulledWrite",
				ERDGPassType::Compute);
			Builder.UseValue(Write, Value, ERDGUse::Write);
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			EXPECT_TRUE(Result.Graph->GetPasses().empty());
			EXPECT_EQ(CulledDestructions, 0);
		}
		EXPECT_EQ(CulledDestructions, 1);

		int PreparationFailureDestructions = 0;
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<FTypedValuePayload>(
				"PreparationFailure", "tracked",
				&PreparationFailureDestructions);
			const auto Write = Builder.AddPass("Write",
				ERDGPassType::Compute);
			Builder.UseValue(Write, Value, ERDGUse::Write);
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

	TEST_F(FRDGTests, TypedValueResolutionRejectsWrongDirectionAndCopies)
	{
		{
			FRDGBuilder Builder;
			const auto Value = Builder.CreateValue<int>(
				"WrongDirection", "signed-int", 0);
			const auto Write = Builder.AddPass("Write",
				ERDGPassType::Compute,
				[Value](FRHICommandListImmediate&,
					const FRDGPassResources& Resources) {
					(void)Resources.ReadValue(Value);
				});
			Builder.UseValue(Write, Value, ERDGUse::Write);
			Builder.MarkPassRoot(Write, "publish");
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
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
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			EXPECT_DEATH(Result.Graph->Execute(GetCommandList()),
				"not declared by the executing pass parameters");
		}
	}

	TEST_F(FRDGTests, PrecompileFallbackSelectionCapturesOnlyChosenImport)
	{
		for (const bool bCandidateReady : {false, true})
		{
			auto Candidate = MakeGraphTexture("Candidate");
			auto Fallback = MakeGraphTexture("Fallback");
			FRHITexture* Selected = bCandidateReady ? &Candidate : &Fallback;
			FRDGBuilder Builder;
			const auto Input = Builder.ImportTexture("Selected.Environment",
				Selected, ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
			auto Parameters = Builder.AllocParameters<
				FComposedTextureArrayParameters>();
			Parameters->Textures[0] = FRDGTextureParameter{
				Input, WholeColor()};
			Parameters->Textures[1] = std::nullopt;
			const auto Pass = Builder.AddPass("Consume",
				ERDGPassType::Graphics, std::move(Parameters));
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
