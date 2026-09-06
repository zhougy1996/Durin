#include "RDG.h"

#include "RHICommandList.h"
#include "Shader/Shader.h"

#include <gtest/gtest.h>

#include <chrono>
#include <bit>

namespace Durin
{
	// Test-only access to the production compiler; success seals the builder.
	class FRDGBuilderTestAccessor final
	{
	public:
		struct FEvidence
		{
			std::string Error;
			auto IsSuccess() const -> bool { return Error.empty(); }
		};
		static auto Compile(FRDGBuilder& Builder) -> FEvidence
		{ return {Builder.CompileForTesting()}; }
	};

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
				Builder.AddPass("Manual", ERDGPassType::Copy,
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
				const auto Pass = Builder.AddPass("Typed", ERDGPassType::Copy,
					[&, Value](FRHICommandListImmediate&, const FRDGPassResources& Resources) {
						Resources.WriteValue(Value) = 42;
						++Calls;
					});
				Builder.UseValue(Pass, Value, ERDGUse::Write);
			}
			const auto Result = Builder.Execute(GetCommandList());
			ASSERT_EQ(Result.Status, ERDGExecutionStatus::Recorded) << Result.Error;
			EXPECT_EQ(Builder.GetState(), ERDGBuilderState::Recorded);
			const auto Capture = Builder.Capture();
			EXPECT_EQ(Builder.Execute(GetCommandList()).Status, ERDGExecutionStatus::InvalidState);
			EXPECT_EQ(Calls, Shape == 0 ? 0 : 1);
			EXPECT_EQ(Builder.GetExecutionResult().Status, Result.Status);
			EXPECT_EQ(Builder.GetExecutionResult().Error, Result.Error);
			EXPECT_EQ(Builder.Capture().Dump, Capture.Dump);
			EXPECT_EQ(Builder.GetStatistics().ExecuteMicroseconds, Capture.Statistics.ExecuteMicroseconds);
		}
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
				EXPECT_EQ(Result.Error, "render graph storage construction is incomplete");
			}
			~FReentrantValue() { ++Destructions; }
			int& Destructions;
		};
		int Destructions = 0;
		int Calls = 0;
		{
			FRDGBuilder Builder;
			Builder.AddPass("MustNotRun", ERDGPassType::Copy,
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
		EXPECT_EQ(Builder.GetExecutionResult().Error, Result.Error);
		EXPECT_DEATH(Builder.CreateToken("Late"), "require Building state");
	}

	TEST_F(FRDGTests, CompileOnlyEvidenceSealsWithoutExecutableOwnership)
	{
		FRDGBuilder Builder;
		Builder.AddPass("Diagnostic", ERDGPassType::Copy);
		const auto Evidence = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Evidence.IsSuccess()) << Evidence.Error;
		EXPECT_TRUE(Builder.Capture().bCompiled);
		EXPECT_EQ(Builder.Execute(GetCommandList()).Status, ERDGExecutionStatus::InvalidState);
		EXPECT_FALSE(FRDGBuilderTestAccessor::Compile(Builder).IsSuccess());
	}

	TEST_F(FRDGTests, FinalizedDeclarationsRejectLateMutation)
	{
		FRDGBuilder Builder;
		const auto Token = Builder.CreateToken("Token");
		const auto Pass = Builder.AddPass("Write", ERDGPassType::Copy);
		Builder.UseToken(Pass, Token, ERDGUse::Write);
		ASSERT_TRUE(Builder.Execute(GetCommandList()).IsSuccess());
		EXPECT_DEATH(Builder.AddPass("Late", ERDGPassType::Copy), "require Building state");
		EXPECT_DEATH(Builder.CreateValue<int>("Late", "int"), "require Building state");
		EXPECT_DEATH(Builder.AllocParameters<FNestedGraphParameters>(), "require Building state");
		EXPECT_DEATH(Builder.SetBudget({}), "require Building state");
		EXPECT_DEATH(Builder.EnablePassCulling(), "require Building state");
		EXPECT_DEATH(Builder.AddDependency(Pass, Pass), "require Building state");
		EXPECT_DEATH(Builder.MarkPassRoot(Pass), "require Building state");
		EXPECT_DEATH(Builder.UseToken(Pass, Token, ERDGUse::Read), "require Building state");
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
		const auto Pass = Builder.AddPass("Write", ERDGPassType::Compute,
			[&](FRHICommandListImmediate&, const FRDGPassResources&) {
				++Calls;
				EXPECT_EQ(Builder.GetState(), ERDGBuilderState::Recording);
				EXPECT_EQ(Builder.Execute(GetCommandList(), &Context).Status, ERDGExecutionStatus::InvalidState);
				EXPECT_FALSE(Extraction);
			});
		Builder.UseBuffer(Pass, Buffer, 0, 64, ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
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
			const auto Pass = Builder.AddPass("Write", ERDGPassType::Compute,
				[&](FRHICommandListImmediate&, const FRDGPassResources&) { Builder.EnablePassCulling(); });
			Builder.UseBuffer(Pass, Buffer, 0, 64, ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
			if (InAllocator) Allocator.OnAllocate = [&] { Builder.CreateToken("Late"); };
			EXPECT_DEATH(Builder.Execute(GetCommandList(), &Context), "require Building state");
		}
	}

	TEST_F(FRDGTests, FailedPreparationRetainsCaptureAndNeverPublishesExtraction)
	{
		for (int Failure = 0; Failure < 3; ++Failure)
		{
			FRDGCapture Capture;
			int Destructions = 0;
			{
				FRDGBuilder Builder;
				FTestRDGAllocator Allocator;
				Allocator.bFail = Failure == 0;
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
				const auto Pass = Builder.AddPass("Write", ERDGPassType::Compute,
					[&](FRHICommandListImmediate&, const FRDGPassResources&) { ++Calls; });
				Builder.UseBuffer(Pass, Buffer, 0, 64, ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
				Builder.UseValue(Pass, Value, ERDGUse::Write);
				const auto CommandsBefore = GetCommandList().GetNumRecordedCommands();
				const auto Result = Builder.Execute(GetCommandList(), &Context);
				ASSERT_EQ(Result.Status, ERDGExecutionStatus::PreparationFailed);
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
				EXPECT_EQ(Builder.GetExecutionResult().Error, Result.Error);
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
				const auto Pass = Builder.AddPass("Throw", ERDGPassType::Compute,
					[](FRHICommandListImmediate&, const FRDGPassResources&) { throw std::runtime_error("callback"); });
				Builder.UseBuffer(Pass, Buffer, 0, 64, ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
				Builder.UseValue(Pass, Value, ERDGUse::Write);
				FTestRDGAllocator Allocator;
				if (InAllocator) Allocator.OnAllocate = [] { throw std::runtime_error("allocator"); };
				FRDGExecutionContext Context{Allocator};
				EXPECT_THROW(Builder.Execute(GetCommandList(), &Context), std::runtime_error);
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
		const auto Write = First.AddPass("Write", ERDGPassType::Graphics);
		First.UseColorAttachment(Write, FirstTexture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store);
		ASSERT_TRUE(First.Execute(GetCommandList()).IsSuccess());
		ASSERT_EQ(First.GetPasses()[0].TextureTransitions.size(), 1u);
		EXPECT_EQ(First.GetPasses()[0].TextureTransitions[0].ExpectedBefore, ERHIAccess::GraphicsShaderRead);
		const auto Before = First.Capture();
		const auto CommandCount = GetCommandList().GetNumRecordedCommands();
		EXPECT_EQ(First.Execute(GetCommandList()).Status, ERDGExecutionStatus::InvalidState);
		EXPECT_EQ(First.Capture().Dump, Before.Dump);
		EXPECT_EQ(GetCommandList().GetNumRecordedCommands(), CommandCount);
		FRDGBuilder Second;
		const auto SecondTexture = Second.RegisterExternalTexture(Texture, "External",
			ERHIAccess::TransferRead, ERHIAccess::GraphicsShaderRead);
		const auto Read = Second.AddPass("Read", ERDGPassType::Graphics);
		Second.UseTexture(Read, SecondTexture, WholeColor(), ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
		ASSERT_TRUE(Second.Execute(GetCommandList()).IsSuccess());
		ASSERT_EQ(Second.GetPasses()[0].TextureTransitions.size(), 1u);
		EXPECT_EQ(Second.GetPasses()[0].TextureTransitions[0].ExpectedBefore, ERHIAccess::TransferRead);
		EXPECT_EQ(Second.GetPasses()[0].TextureTransitions[0].RequiredAfter, ERHIAccess::GraphicsShaderRead);
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
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
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
		EXPECT_TRUE(Result.IsSuccess()) << Result.Error;
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
		EXPECT_NE(Result.Error.find("incompatible graph/shader declaration"),
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
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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

	TEST_F(FRDGTests, RejectsMalformedGraphParameterMetadataAtomically)
	{
		FRDGBuilder Builder;
		auto Parameters = Builder.AllocParameters<FMalformedGraphParameters>();
		EXPECT_FALSE(Parameters.IsValid());
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_EQ(Result.Error,
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
		EXPECT_EQ(InvalidFirst.Error,
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
			const auto Pass = Builder.AddPass("UseMissingBacking", ERDGPassType::Graphics);
			Builder.UseColorAttachment(Pass, Texture, WholeColor(),
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

			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_TRUE(Result.IsSuccess()) << Result.Error;
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
			EXPECT_FALSE(Builder.AddPass("ForeignHandle",
				ERDGPassType::Graphics, std::move(Parameters)).IsValid());
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_EQ(Result.Error,
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
			EXPECT_FALSE(Builder.AddPass("InvalidRange",
				ERDGPassType::Graphics, std::move(Parameters)).IsValid());
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_EQ(Result.Error,
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
				EXPECT_FALSE(Builder.AddPass("Overlap",
					ERDGPassType::Graphics,
					std::move(Parameters)).IsValid());
				return FRDGBuilderTestAccessor::Compile(Builder).Error;
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
			EXPECT_FALSE(Builder.AddPass("WrongDomain",
				ERDGPassType::Compute, std::move(Parameters)).IsValid());
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
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
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
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
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
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
			auto Result = FRDGBuilderTestAccessor::Compile(Other);
			EXPECT_EQ(Result.Error,
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
		const auto Pass = Builder.AddPass("LargeParameters",
			ERDGPassType::Graphics, std::move(Parameters));
		ASSERT_TRUE(Pass.IsValid());
		const auto DeclarationMicroseconds = std::chrono::duration_cast<
			std::chrono::microseconds>(std::chrono::steady_clock::now() - Started)
			.count();
		EXPECT_LT(DeclarationMicroseconds, 1'000'000);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
		EXPECT_TRUE(Result.IsSuccess()) << Result.Error;
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
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
		EXPECT_TRUE(Result.IsSuccess()) << Result.Error;
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
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
		EXPECT_TRUE(Result.IsSuccess()) << Result.Error;
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
		Builder.AddPass("SecondPass", ERDGPassType::Graphics,
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
			ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
			const auto Capture = Builder.Capture();
			EXPECT_TRUE(Capture.Passes.empty());
			ASSERT_EQ(Capture.Parameters.size(), 1u);
			EXPECT_EQ(Capture.Parameters[0].PassDeclarationIndex, 0u);
			EXPECT_EQ(Capture.Parameters[0].FieldPath,
				"FNestedGraphParameters.Completion");
			EXPECT_TRUE(Result.IsSuccess()) << Result.Error;
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
			ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
			std::string Error;
			Error = Result.Error;
			EXPECT_FALSE(Result.IsSuccess()) << Result.Error;
			EXPECT_NE(Error.find("omitted retained resource id="),
				std::string::npos);
			EXPECT_FALSE(bExecuted);
		}
	}

	TEST_F(FRDGTests, CompilesStableHazardOrderAndExactTextureTransitions)
	{
		auto Texture = MakeGraphTexture("SceneColor");
		FRDGBuilder Builder;
		const auto SceneColor = CreateTestTexture(Builder, "SceneColor", Texture, ERHIAccess::GraphicsShaderRead);
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

		FTestRDGAllocator Allocator;
		Allocator.TextureOverrides.emplace(0, Texture);
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Builder.GetPasses().size(), 3u);
		EXPECT_EQ(Builder.GetPasses()[0].Name, "Independent");
		EXPECT_EQ(Builder.GetPasses()[1].Name, "Produce");
		EXPECT_EQ(Builder.GetPasses()[2].Name, "Consume");
		ASSERT_EQ(Builder.GetDependencies().size(), 1u);
		EXPECT_EQ(Builder.GetDependencies()[0],
			(FRDGDependency{1, 2, "SceneColor",
				ERDGDependencyKind::Value}));
		ASSERT_EQ(Builder.GetPasses()[1].TextureTransitions.size(), 1u);
		EXPECT_EQ(Builder.GetPasses()[1].TextureTransitions[0], (FRHITextureTransition{Texture.GetReference(), WholeColor(), ERHIAccess::Discard, ERHIAccess::ColorAttachmentReadWrite, true}));
		ASSERT_EQ(Builder.GetPasses()[2].TextureTransitions.size(), 1u);
		EXPECT_EQ(Builder.GetPasses()[2].TextureTransitions[0], (FRHITextureTransition{Texture.GetReference(), WholeColor(), ERHIAccess::ColorAttachmentReadWrite, ERHIAccess::ComputeShaderRead}));
		ASSERT_EQ(Builder.GetFinalTextureTransitions().size(), 1u);
		EXPECT_EQ(Builder.GetFinalTextureTransitions()[0], (FRHITextureTransition{Texture.GetReference(), WholeColor(), ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead}));
	}

	TEST_F(FRDGTests, CompilesBufferRawWarAndWawDependencies)
	{
		auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
			"Work", 64, 4, EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy
		));
		FRDGBuilder Builder;
		const auto Work = CreateTestBuffer(Builder, "Work", Buffer);
		const auto Write = Builder.AddPass("Write", ERDGPassType::Compute);
		Builder.UseBuffer(Write, Work, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Read = Builder.AddPass("Read", ERDGPassType::Copy);
		Builder.UseBuffer(Read, Work, 0, 64, ERDGUse::Read,
			ERHIAccess::TransferRead);
		const auto Rewrite = Builder.AddPass("Rewrite", ERDGPassType::Compute);
		Builder.UseBuffer(Rewrite, Work, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);

		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Builder.GetDependencies().size(), 2u);
		EXPECT_EQ(Builder.GetDependencies()[0].Kind,
			ERDGDependencyKind::Value);
		EXPECT_EQ(Builder.GetDependencies()[1].Kind,
			ERDGDependencyKind::Execution);
		EXPECT_EQ(Builder.GetPasses()[0].BufferTransitions[0].ExpectedBefore,
			ERHIAccess::Discard);
		EXPECT_EQ(Builder.GetPasses()[1].BufferTransitions[0].ExpectedBefore,
			ERHIAccess::ComputeShaderReadWrite);
		EXPECT_EQ(Builder.GetPasses()[2].BufferTransitions[0].ExpectedBefore,
			ERHIAccess::TransferRead);
		EXPECT_TRUE(Builder.GetPasses()[2].BufferTransitions[0].bDiscardContents);
	}

	TEST_F(FRDGTests, SameStateWritesSynchronizeExactBufferAndTextureRanges)
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
			const auto Write = Builder.AddPass("Write", ERDGPassType::Compute);
			Builder.UseBuffer(Write, Buffer, 0, 64, ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
			Builder.UseTexture(Write, Texture, WholeColor(2), ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
			const auto Consume = Builder.AddPass("Consume", ERDGPassType::Compute);
			Builder.UseBuffer(Consume, Buffer, 16, 16, NextUse,
				ERHIAccess::ComputeShaderReadWrite);
			Builder.UseTexture(Consume, Texture, Mip, NextUse,
				ERHIAccess::ComputeShaderReadWrite);
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			const auto& Pass = Builder.GetPasses()[1];
			ASSERT_EQ(Pass.BufferTransitions.size(), 1u);
			EXPECT_EQ(Pass.BufferTransitions[0], (FRHIBufferTransition{
				nullptr, 16, 16, ERHIAccess::ComputeShaderReadWrite,
				ERHIAccess::ComputeShaderReadWrite}));
			ASSERT_EQ(Pass.TextureTransitions.size(), 1u);
			EXPECT_EQ(Pass.TextureTransitions[0], (FRHITextureTransition{
				nullptr, Mip, ERHIAccess::ComputeShaderReadWrite,
				ERHIAccess::ComputeShaderReadWrite}));
			const auto Capture = Builder.Capture();
			EXPECT_EQ(Capture.Statistics.BufferTransitions, 4u);
			EXPECT_EQ(Capture.Statistics.TextureTransitions, 3u);
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
			const auto Pass = Builder.AddPass(Name, ERDGPassType::Compute);
			Builder.UseBuffer(Pass, Input, 0, 64, ERDGUse::Read,
				ERHIAccess::ComputeShaderRead);
			Builder.UseTexture(Pass, Texture, WholeColor(), ERDGUse::Read,
				ERHIAccess::ComputeShaderRead);
		}
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		for (const auto& Pass : Builder.GetPasses())
		{
			EXPECT_TRUE(Pass.BufferTransitions.empty());
			EXPECT_TRUE(Pass.TextureTransitions.empty());
		}
		EXPECT_TRUE(Builder.Capture().Transitions.empty());
	}

	TEST_F(FRDGTests, ManagedAttachmentLoadSynchronizesSameStateWrites)
	{
		FRDGBuilder Builder;
		const auto Texture = CreateTestTexture(Builder, "Color", MakeGraphTexture("Color"));
		const auto Clear = Builder.AddPass("Clear", ERDGPassType::Graphics);
		Builder.UseManagedColorAttachment(Clear, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store,
			ERHIAccess::ColorAttachmentReadWrite);
		const auto Load = Builder.AddPass("Load", ERDGPassType::Graphics);
		Builder.UseManagedColorAttachment(Load, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Load, ERHIRenderTargetStoreAction::Store,
			ERHIAccess::ColorAttachmentReadWrite);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Builder.GetPasses()[0].TextureTransitions.size(), 1u);
		EXPECT_TRUE(Builder.GetPasses()[0].TextureTransitions[0].bDiscardContents);
		const auto& Transitions = Builder.GetPasses()[1].TextureTransitions;
		ASSERT_EQ(Transitions.size(), 1u);
		EXPECT_EQ(Transitions[0], (FRHITextureTransition{nullptr, WholeColor(),
			ERHIAccess::ColorAttachmentReadWrite, ERHIAccess::ColorAttachmentReadWrite}));
	}

	TEST_F(FRDGTests, RejectsMissingProducerForeignHandleAndCycle)
	{
		auto Texture = MakeGraphTexture("Missing");
		FRDGBuilder MissingProducer;
		const auto Logical = CreateTestTexture(MissingProducer, "Missing", Texture);
		const auto Read = MissingProducer.AddPass("Read", ERDGPassType::Graphics);
		MissingProducer.UseTexture(Read, Logical, WholeColor(),
			ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
		auto Missing = FRDGBuilderTestAccessor::Compile(MissingProducer);
		EXPECT_FALSE(Missing.IsSuccess());
		EXPECT_NE(Missing.Error.find("before its producer"), std::string::npos);

		FRDGBuilder ForeignOwner;
		const auto Foreign = CreateTestTexture(ForeignOwner, "Foreign", Texture);
		FRDGBuilder ForeignUse;
		const auto Pass = ForeignUse.AddPass("Use", ERDGPassType::Graphics);
		ForeignUse.UseTexture(Pass, Foreign, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Invalid = FRDGBuilderTestAccessor::Compile(ForeignUse);
		EXPECT_FALSE(Invalid.IsSuccess());
		EXPECT_NE(Invalid.Error.find("invalid resource handle"), std::string::npos);

		FRDGBuilder Cyclic;
		const auto A = Cyclic.AddPass("A", ERDGPassType::Compute);
		const auto B = Cyclic.AddPass("B", ERDGPassType::Compute);
		Cyclic.AddDependency(A, B);
		Cyclic.AddDependency(B, A);
		auto Cycle = FRDGBuilderTestAccessor::Compile(Cyclic);
		EXPECT_FALSE(Cycle.IsSuccess());
		EXPECT_EQ(Cycle.Error, "graph contains a dependency cycle");

		FRDGBuilder SelfDependent;
		const auto Self = SelfDependent.AddPass(
			"Self", ERDGPassType::Compute);
		SelfDependent.AddDependency(Self, Self);
		auto SelfCycle = FRDGBuilderTestAccessor::Compile(SelfDependent);
		EXPECT_FALSE(SelfCycle.IsSuccess());
		EXPECT_EQ(SelfCycle.Error, "graph contains a dependency cycle");
	}

	TEST_F(FRDGTests, RejectsTextureAspectsOutsideResourceFormat)
	{
		auto Texture = MakeGraphTexture("ColorOnly");
		FRDGBuilder Builder;
		const auto Resource = CreateTestTexture(Builder, "ColorOnly", Texture);
		const auto Pass = Builder.AddPass("InvalidAspects",
			ERDGPassType::Compute);
		Builder.UseTexture(Pass, Resource,
			{ERHITextureAspect::Color | ERHITextureAspect::Depth, 0, 1, 0, 1},
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);

		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("invalid texture range"), std::string::npos);
	}

	TEST_F(FRDGTests, NormalizesDisjointAndPartiallyOverlappingSubresources)
	{
		auto Texture = MakeGraphTexture("MipChain", 4);
		FRDGBuilder Builder;
		const auto Chain = CreateTestTexture(Builder, "MipChain", Texture);
		const auto Mip0 = Builder.AddPass("Mip0", ERDGPassType::Compute);
		Builder.UseTexture(Mip0, Chain, {ERHITextureAspect::Color, 0, 1, 0, 1},
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto Mip1 = Builder.AddPass("Mip1", ERDGPassType::Compute);
		Builder.UseTexture(Mip1, Chain, {ERHITextureAspect::Color, 1, 1, 0, 1},
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		auto Disjoint = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Disjoint.IsSuccess()) << Disjoint.Error;
		EXPECT_TRUE(Builder.GetDependencies().empty());

		FRDGBuilder Partial;
		const auto PartialChain = CreateTestTexture(Partial, "MipChain", Texture);
		const auto Whole = Partial.AddPass("Whole", ERDGPassType::Compute);
		Partial.UseTexture(Whole, PartialChain, WholeColor(4),
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto OneMip = Partial.AddPass("OneMip", ERDGPassType::Compute);
		Partial.UseTexture(OneMip, PartialChain,
			{ERHITextureAspect::Color, 1, 1, 0, 1}, ERDGUse::Read,
			ERHIAccess::ComputeShaderRead);
		auto Overlap = FRDGBuilderTestAccessor::Compile(Partial);
		ASSERT_TRUE(Overlap.IsSuccess()) << Overlap.Error;
		ASSERT_EQ(Partial.GetDependencies().size(), 1u);
		EXPECT_EQ(Partial.GetDependencies()[0].Kind,
			ERDGDependencyKind::Value);
		EXPECT_EQ(Partial.GetPasses()[0].TextureTransitions.size(), 3u);
		EXPECT_EQ(Partial.GetPasses()[1].TextureTransitions.size(), 1u);
	}

	TEST_F(FRDGTests, DiscardedAttachmentStoreCannotBecomeAProducer)
	{
		auto Texture = MakeGraphTexture("Discarded");
		FRDGBuilder Builder;
		const auto Target = CreateTestTexture(Builder, "Discarded", Texture);
		const auto Clear = Builder.AddPass("Clear", ERDGPassType::Graphics);
		Builder.UseColorAttachment(Clear, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::DontCare);
		const auto Read = Builder.AddPass("Read", ERDGPassType::Graphics);
		Builder.UseTexture(Read, Target, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("before its producer"), std::string::npos);
	}

	TEST_F(FRDGTests, PreservesExternalInitialAndFinalStates)
	{
		auto Texture = MakeGraphTexture("Imported");
		FRDGBuilder Builder;
		const auto External = Builder.RegisterExternalTexture(Texture, "External", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Compute = Builder.AddPass("Compute", ERDGPassType::Compute);
		Builder.UseTexture(Compute, External, WholeColor(), ERDGUse::Read, ERHIAccess::ComputeShaderRead);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Builder.GetPasses()[0].TextureTransitions.size(), 1u);
		EXPECT_EQ(Builder.GetPasses()[0].TextureTransitions[0].ExpectedBefore,
			ERHIAccess::GraphicsShaderRead);
		ASSERT_EQ(Builder.GetFinalTextureTransitions().size(), 1u);
		EXPECT_EQ(Builder.GetFinalTextureTransitions()[0].RequiredAfter,
			ERHIAccess::GraphicsShaderRead);
	}

	TEST_F(FRDGTests, RejectsAttachmentLoadWithoutPriorContents)
	{
		auto Texture = MakeGraphTexture("Load");
		FRDGBuilder Builder;
		const auto Target = CreateTestTexture(Builder, "Load", Texture);
		const auto Load = Builder.AddPass("Load", ERDGPassType::Graphics);
		Builder.UseColorAttachment(Load, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Load,
			ERHIRenderTargetStoreAction::Store);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("before its producer"), std::string::npos);
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
				const auto Pass = Builder.AddPass("Pass" + std::to_string(Index),
					ERDGPassType::Compute);
				Builder.UseBuffer(Pass, Work, 0, 512, ERDGUse::Write,
					ERHIAccess::ComputeShaderReadWrite, Index == 0);
			}
			const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_TRUE(Result.IsSuccess()) << Result.Error;
			return Builder.Capture();
		};
		auto First = CompileFixture();
		auto Second = CompileFixture();
		EXPECT_EQ(First.Dump, Second.Dump);
		EXPECT_LT(First.Statistics.CompileMicroseconds, 250000u);
		EXPECT_LT(Second.Statistics.CompileMicroseconds, 250000u);
		EXPECT_EQ(First.Dependencies.size(), 127u);
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

		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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
		EXPECT_NE(DuplicateResult.Error.find("conflicting external physical resource: canonical 'First'"), std::string::npos);
		EXPECT_NE(DuplicateResult.Error.find("conflicts with 'Second'"),
			std::string::npos);

		FRDGBuilder BufferConflict;
		BufferConflict.RegisterExternalBuffer(Buffer, "CanonicalBuffer", ERHIAccess::ComputeShaderRead, ERHIAccess::ComputeShaderRead);
		BufferConflict.RegisterExternalBuffer(Buffer, "ConflictingBuffer", ERHIAccess::ComputeShaderRead, ERHIAccess::TransferRead);
		auto BufferConflictResult = FRDGBuilderTestAccessor::Compile(BufferConflict);
		EXPECT_FALSE(BufferConflictResult.IsSuccess());
		EXPECT_NE(BufferConflictResult.Error.find(
			"canonical 'CanonicalBuffer' (kind=buffer"), std::string::npos);
		EXPECT_NE(BufferConflictResult.Error.find(
			"conflicts with 'ConflictingBuffer'"), std::string::npos);

		FRDGBuilder NullExternal;
		const auto NullHandle = NullExternal.RegisterExternalTexture({}, "Null", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		EXPECT_TRUE(NullHandle.IsValid());
		auto NullResult = FRDGBuilderTestAccessor::Compile(NullExternal);
		EXPECT_FALSE(NullResult.IsSuccess());
		EXPECT_EQ(NullResult.Error, "resource 'Null' has no physical resource");

		FRDGBuilder Domain;
		const auto External = Domain.RegisterExternalTexture(Texture, "Shared", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Copy = Domain.AddPass("Copy", ERDGPassType::Copy);
		Domain.UseTexture(Copy, External, WholeColor(), ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
		auto DomainResult = FRDGBuilderTestAccessor::Compile(Domain);
		EXPECT_FALSE(DomainResult.IsSuccess());
		EXPECT_NE(DomainResult.Error.find("incompatible with pass domain"),
			std::string::npos);
	}

	TEST_F(FRDGTests, DiscardAndDontCareStorePreserveRetainedTextureAccess)
	{
		auto Texture = MakeGraphTexture("DiscardSync");
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Target = Builder.RegisterExternalTexture(Texture, "DiscardSync",
			ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Read = Builder.AddPass("Read", ERDGPassType::Compute);
		Builder.UseTexture(Read, Target, WholeColor(), ERDGUse::Read,
			ERHIAccess::ComputeShaderRead);
		Builder.MarkPassRoot(Read, "read effect");
		const auto Clear = Builder.AddPass("Clear", ERDGPassType::Graphics);
		Builder.UseColorAttachment(Clear, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::DontCare);
		Builder.MarkPassRoot(Clear, "write effect");
		const auto Rewrite = Builder.AddPass("Rewrite", ERDGPassType::Compute);
		Builder.UseTexture(Rewrite, Target, WholeColor(), ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		Builder.MarkPassRoot(Rewrite, "replacement");
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Builder.GetPasses().size(), 3u);
		const auto& ClearTransitions = Builder.GetPasses()[1].TextureTransitions;
		ASSERT_EQ(ClearTransitions.size(), 1u);
		EXPECT_EQ(ClearTransitions[0].ExpectedBefore, ERHIAccess::ComputeShaderRead);
		EXPECT_TRUE(ClearTransitions[0].bDiscardContents);
		const auto& RewriteTransitions = Builder.GetPasses()[2].TextureTransitions;
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
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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
		const auto Produce = Builder.AddPass("Produce", ERDGPassType::Compute);
		Builder.UseBuffer(Produce, Retained, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		Builder.MarkPassRoot(Produce, "effect");
		const auto Unused = Builder.AddPass("Unused", ERDGPassType::Compute);
		Builder.UseBuffer(Unused, Culled, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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
		const auto Pass = Builder.AddPass("Pass", ERDGPassType::Graphics,
			[=](FRHICommandListImmediate&, const FRDGPassResources& Resources) {
				Resources.GetTexture(Hidden);
			});
		Builder.UseTexture(Pass, Declared, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);

		EXPECT_DEATH(Builder.Execute(GetCommandList()),
			"undeclared texture");
	}

	TEST_F(FRDGTests, ManagedAttachmentExitStateDrivesFollowingTransition)
	{
		auto Texture = MakeGraphTexture("Managed");
		FRDGBuilder Builder;
		const auto Target = CreateTestTexture(Builder, "Managed", Texture);
		const auto Render = Builder.AddPass("Render", ERDGPassType::Graphics);
		Builder.UseManagedColorAttachment(Render, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			ERHIAccess::GraphicsShaderRead);
		const auto Consume = Builder.AddPass("Consume", ERDGPassType::Compute);
		Builder.UseTexture(Consume, Target, WholeColor(), ERDGUse::Read,
			ERHIAccess::ComputeShaderRead);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Builder.GetPasses()[0].TextureTransitions.size(), 1u);
		EXPECT_TRUE(Builder.GetPasses()[0].TextureTransitions[0].bDiscardContents);
		ASSERT_EQ(Builder.GetPasses()[1].TextureTransitions.size(), 1u);
		EXPECT_EQ(Builder.GetPasses()[1].TextureTransitions[0].ExpectedBefore,
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
		const auto Pass = Builder.AddPass("Write", ERDGPassType::Compute,
			[&](FRHICommandListImmediate&, const FRDGPassResources&) {
				bExecuted = true;
			});
		Builder.UseBuffer(Pass, Buffer, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);

		FTestRDGAllocator Allocator;
		Allocator.bOmitResources = true;
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
		std::string Error;
		Error = Result.Error;
		EXPECT_FALSE(Result.IsSuccess()) << Result.Error;
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

		FTestRDGAllocator Allocator;
		FRDGExecutionContext Context{Allocator};
		EXPECT_FALSE(FirstExtraction);
		EXPECT_FALSE(SecondExtraction);
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
		ASSERT_EQ(Builder.GetPasses().size(), 3u);

		std::string Error;
		Error = Result.Error;
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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
		const auto Graphics = Builder.AddPass("Graphics", ERDGPassType::Graphics);
		Builder.UseColorAttachment(Graphics, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store);
		const auto Compute = Builder.AddPass("Compute", ERDGPassType::Compute);
		Builder.UseBuffer(Compute, Buffer, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		Builder.UseBuffer(Compute, Transient, 0, 64, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		Builder.QueueTextureExtraction(Texture, &ExportedTexture,
			ERHIAccess::ColorAttachmentReadWrite);
		Builder.QueueBufferExtraction(Buffer, &ExportedBuffer,
			ERHIAccess::ComputeShaderReadWrite);

		FTestRDGAllocator Allocator;
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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
		const auto Pass = Builder.AddPass("Write", ERDGPassType::Graphics,
			[&](FRHICommandListImmediate&, const FRDGPassResources&) {
				bExecuted = true;
			});
		Builder.UseColorAttachment(Pass, Texture, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		Builder.QueueTextureExtraction(Texture, &Destination,
			ERHIAccess::GraphicsShaderRead);

		FTestRDGAllocator Allocator;
		Allocator.bFail = true;
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
		std::string Error;
		Error = Result.Error;
		EXPECT_FALSE(Result.IsSuccess()) << Result.Error;
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

			const auto Result = Builder.Execute(GetCommandList());
			ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
			EXPECT_GT(Texture.GetRefCount(), InitialReferences);
			EXPECT_TRUE(Result.IsSuccess()) << Result.Error;
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
		const auto Pass = Builder.AddPass("Read", ERDGPassType::Graphics);
		Builder.UseTexture(Pass, External, WholeColor(), ERDGUse::Read,
			ERHIAccess::GraphicsShaderRead);
		Builder.UseColorAttachment(Pass, Allocated, WholeColor(), ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store);

		FTestRDGAllocator Allocator;
		Allocator.TextureOverrides.emplace(1, AllocatedTexture);
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Resources.size(), 2u);
		EXPECT_EQ(Capture.Resources[0].AllocationDisposition, "external");
		EXPECT_EQ(Capture.Resources[0].PhysicalAllocationId, 0u);
		EXPECT_EQ(Capture.Resources[1].AllocationDisposition, "allocated");
		EXPECT_NE(Capture.Resources[1].PhysicalAllocationId, 0u);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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
					const auto Pass = Builder.AddPass("Write", ERDGPassType::Graphics);
					Builder.UseColorAttachment(Pass, Texture, WholeColor(Mode == 1 ? 1 : 2),
						ERHIRenderTargetLoadAction::Clear,
						Mode == 1 ? ERHIRenderTargetStoreAction::Store
							: ERHIRenderTargetStoreAction::DontCare);
				}
				const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
				EXPECT_FALSE(Result.IsSuccess());
				EXPECT_NE(Result.Error.find("RDG.Export"), std::string::npos);
				EXPECT_NE(Result.Error.find("before its producer"), std::string::npos);
				EXPECT_EQ(Destination.GetReference(), Previous);
			}
	}

	TEST_F(FRDGTests, ExtractionRequiresCompleteBufferContents)
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
					const auto Pass = Builder.AddPass("Write", ERDGPassType::Compute);
					Builder.UseBuffer(Pass, Buffer, 0, WrittenSize, ERDGUse::Write,
						ERHIAccess::ComputeShaderReadWrite, true);
				}

				FTestRDGAllocator Allocator;
				FRDGExecutionContext Context{Allocator};
				const auto Result = Builder.Execute(GetCommandList(), &Context);
				EXPECT_EQ(Result.IsSuccess(), WrittenSize == 64) << Result.Error;
				if (Result.IsSuccess())
				{
					EXPECT_EQ(Builder.GetPasses().back().Name, "RDG.Export");
					EXPECT_EQ(Builder.GetResourceLifetimes()[0].LastPass, 1u);
					EXPECT_EQ(Builder.GetDependencies().size(), 1u);
					ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
					EXPECT_TRUE(Destination);
				}
				else
					EXPECT_NE(Result.Error.find("before its producer"), std::string::npos);
			}
	}

	TEST_F(FRDGTests, ExtractionRetainsOnlyFinalRangeProducers)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		FBufferRHIRef Destination;
		const auto Buffer = Builder.CreateBuffer(FRDGBufferDesc{
			.Buffer = FRHIBufferDesc(64, 4, EBufferUsageFlags::UnorderedAccess)}, "Output");
		for (uint32 Index = 0; Index < 3; ++Index)
		{
			const auto Pass = Builder.AddPass("Write" + std::to_string(Index), ERDGPassType::Compute);
			Builder.UseBuffer(Pass, Buffer, Index == 2 ? 32 : 0, Index == 0 ? 64 : 32,
				ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		}
		Builder.QueueBufferExtraction(Buffer, &Destination, ERHIAccess::ComputeShaderRead);
		const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Builder.GetPasses().size(), 3u);
		EXPECT_EQ(Builder.GetPasses()[0].Name, "Write1");
		EXPECT_EQ(Builder.GetPasses()[1].Name, "Write2");
		EXPECT_EQ(Builder.GetResourceLifetimes()[0].LastPass, 2u);
		EXPECT_EQ(Builder.GetDependencies().size(), 2u);
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
			const auto Pass = Builder.AddPass("Write", ERDGPassType::Compute);
			Builder.UseBuffer(Pass, Buffer, 0, 64, ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
			Builder.QueueBufferExtraction(Buffer, &Destination, Access);
			const auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_NE(Result.Error.find("final access"), std::string::npos);
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
		EXPECT_NE(Result.Error.find("before its producer"), std::string::npos);
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
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Builder.GetPasses().size(), 1u);
		const auto& Export = Builder.GetPasses()[0];
		ASSERT_EQ(Export.TextureTransitions.size(), 1u);
		EXPECT_EQ(Export.TextureTransitions[0].RequiredAfter, ERHIAccess::ComputeShaderRead);
		EXPECT_FALSE(Builder.GetResourceLifetimes()[0].bCulled);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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

		FTestRDGAllocator Allocator;
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
		std::string Error;
		Error = Result.Error;
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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

		FTestRDGAllocator TextureAllocator;
		TextureAllocator.TextureOverride = TextureBacking;
		FRDGExecutionContext TextureContext{TextureAllocator};
		const auto TextureResult = TextureBuilder.Execute(GetCommandList(), &TextureContext);
		ASSERT_TRUE(TextureBuilder.HasCompiledPlan()) << TextureResult.Error;
		std::string Error;
		Error = TextureResult.Error;
		EXPECT_FALSE(TextureResult.IsSuccess()) << TextureResult.Error;
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

		FTestRDGAllocator BufferAllocator;
		BufferAllocator.BufferOverride = BufferBacking;
		FRDGExecutionContext BufferContext{BufferAllocator};
		const auto BufferResult = BufferBuilder.Execute(GetCommandList(), &BufferContext);
		ASSERT_TRUE(BufferBuilder.HasCompiledPlan()) << BufferResult.Error;
		Error = BufferResult.Error;
		EXPECT_FALSE(BufferResult.IsSuccess()) << BufferResult.Error;
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

		FTestRDGAllocator TextureAllocator;
		TextureAllocator.TextureOverride = TextureBacking;
		FRDGExecutionContext TextureContext{TextureAllocator};
		const auto TextureResult = TextureBuilder.Execute(GetCommandList(), &TextureContext);
		ASSERT_TRUE(TextureBuilder.HasCompiledPlan()) << TextureResult.Error;
		std::string Error;
		Error = TextureResult.Error;
		EXPECT_TRUE(TextureResult.IsSuccess()) << TextureResult.Error;

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

		FTestRDGAllocator BufferAllocator;
		BufferAllocator.BufferOverride = BufferBacking;
		FRDGExecutionContext BufferContext{BufferAllocator};
		const auto BufferResult = BufferBuilder.Execute(GetCommandList(), &BufferContext);
		ASSERT_TRUE(BufferBuilder.HasCompiledPlan()) << BufferResult.Error;
		Error = BufferResult.Error;
		EXPECT_TRUE(BufferResult.IsSuccess()) << BufferResult.Error;
	}

	TEST_F(FRDGTests, ExplicitEffectRootSurvivesWithoutResourceOutputs)
	{
		FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Timestamp = Builder.AddPass(
			"Timestamp", ERDGPassType::Graphics);
		Builder.MarkPassRoot(Timestamp, "timestamp");
		Builder.AddPass("Unused", ERDGPassType::Graphics);
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Builder.GetPasses().size(), 1u);
		EXPECT_EQ(Builder.GetPasses()[0].Name, "Timestamp");
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
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Builder.GetPasses().size(), 2u);
		ASSERT_EQ(Builder.GetDependencies().size(), 1u);
		EXPECT_EQ(Builder.GetDependencies()[0].Cause, "Prepared");
		EXPECT_TRUE(Builder.GetPasses()[0].BufferTransitions.empty());
		EXPECT_TRUE(Builder.GetPasses()[0].TextureTransitions.empty());
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
		const auto Pass = Builder.AddPass("Scene.GBuffer",
			ERDGPassType::Graphics,
			[&](FRHICommandListImmediate&,
				const FRDGPassResources& Resources) {
				for (const auto Color : Colors)
					EXPECT_NE(Resources.GetTexture(Color), nullptr);
				EXPECT_EQ(Resources.GetTexture(Depth), DepthTexture.GetReference());
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

		FTestRDGAllocator Allocator;
		for (uint32 Index = 0; Index < ColorTextures.size(); ++Index)
			Allocator.TextureOverrides.emplace(Index, ColorTextures[Index]);
		Allocator.TextureOverrides.emplace(4, DepthTexture);
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
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
		EXPECT_TRUE(Result.IsSuccess()) << Result.Error;
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

		FTestRDGAllocator Allocator;
		Allocator.bOmitResources = true;
		FRDGExecutionContext Context{Allocator};
		const auto Result = Builder.Execute(GetCommandList(), &Context);
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
		std::string Error;
		Error = Result.Error;
		EXPECT_FALSE(Result.IsSuccess()) << Result.Error;
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
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
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
		auto Result = FRDGBuilderTestAccessor::Compile(Builder);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const FRDGStatistics Statistics = Builder.GetStatistics();
		EXPECT_TRUE(Statistics.bPassRegressionBudgetExceeded);
		EXPECT_TRUE(Statistics.IsStructuralRegressionBudgetExceeded());
		EXPECT_EQ(Builder.Capture().Budget.RegressionMaxPasses, 1u);
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
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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

		const auto Result = Builder.Execute(GetCommandList());
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
		ASSERT_EQ(Builder.GetDependencies().size(), 1u);
		EXPECT_EQ(Builder.GetDependencies()[0].Kind,
			ERDGDependencyKind::Value);
		EXPECT_EQ(Builder.GetDependencies()[0].Cause, "Scene.Result");
		const auto Capture = Builder.Capture();
		ASSERT_EQ(Capture.Resources.size(), 1u);
		EXPECT_EQ(Capture.Resources[0].ValueType, "scene-result");
		EXPECT_EQ(Capture.Uses.size(), 2u);
		EXPECT_TRUE(Result.IsSuccess()) << Result.Error;
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
		ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
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
		EXPECT_TRUE(Result.IsSuccess()) << Result.Error;
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
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
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
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
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
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			EXPECT_FALSE(Result.IsSuccess());
			EXPECT_EQ(Result.Error, "pass 'Write' declares an invalid, foreign, or "
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
			const auto Write = Builder.AddPass("Write",
				ERDGPassType::Compute);
			Builder.UseValue(Write, Value, ERDGUse::Write);
			auto Result = Builder.Execute(GetCommandList());
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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
			const auto Write = Builder.AddPass("CulledWrite",
				ERDGPassType::Compute);
			Builder.UseValue(Write, Value, ERDGUse::Write);
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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
			const auto Write = Builder.AddPass("Write",
				ERDGPassType::Compute);
			Builder.UseValue(Write, Value, ERDGUse::Write);
			Builder.UseBuffer(Write, Buffer, 0, 64, ERDGUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);
			Builder.MarkPassRoot(Write, "publish");

			FTestRDGAllocator Allocator;
			Allocator.bFail = true;
			FRDGExecutionContext Context{Allocator};
			const auto Result = Builder.Execute(GetCommandList(), &Context);
			ASSERT_TRUE(Builder.HasCompiledPlan()) << Result.Error;
			std::string Error;
			Error = Result.Error;
			EXPECT_FALSE(Result.IsSuccess()) << Result.Error;
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
			const auto Write = Builder.AddPass("Write",
				ERDGPassType::Compute,
				[Value](FRHICommandListImmediate&,
					const FRDGPassResources& Resources) {
					(void)Resources.ReadValue(Value);
				});
			Builder.UseValue(Write, Value, ERDGUse::Write);
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
			const auto Pass = Builder.AddPass("Consume",
				ERDGPassType::Graphics, std::move(Parameters));
			Builder.MarkPassRoot(Pass, "publish");
			auto Result = FRDGBuilderTestAccessor::Compile(Builder);
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
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
