#include "RenderGraph.h"

#include "RHICommandList.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <ranges>
#include <sstream>
#include <tuple>

namespace Durin
{
	namespace
	{
		struct FGraphResource
		{
			std::string Name;
			ERenderGraphResourceKind Kind = ERenderGraphResourceKind::Texture;
			FRHITexture* Texture = nullptr;
			FRHIBuffer* Buffer = nullptr;
			FRHITextureDesc TextureDesc;
			FRHIBufferDesc BufferDesc;
			std::string BackingClass;
			ERHIAccess InitialAccess = ERHIAccess::Discard;
			ERHIAccess FinalAccess = ERHIAccess::None;
			bool bImported = false;
			bool bRequiresBacking = false;
		};

		struct FGraphUse
		{
			uint32 ResourceIndex = 0;
			ERenderGraphResourceKind Kind = ERenderGraphResourceKind::Texture;
			ERenderGraphUse Use = ERenderGraphUse::Read;
			ERHIAccess Access = ERHIAccess::None;
			bool bDiscard = false;
			FRHITextureSubresourceRange TextureRange{};
			uint64 BufferOffset = 0;
			uint64 BufferSize = 0;
			bool bStore = true;
			bool bPassManagedTransition = false;
			ERHIAccess ResultAccess = ERHIAccess::None;
			std::string ParameterPath;
		};

		struct FGraphPass
		{
			std::string Name;
			ERenderGraphPassType Type = ERenderGraphPassType::Graphics;
			std::vector<FGraphUse> Uses;
			std::vector<uint32> Prerequisites;
			FRenderGraphExecute Execute;
			FRenderGraphParameterizedExecute ParameterizedExecute;
			bool bRoot = false;
			std::string RootReason;
			bool bParameterized = false;
			const FRenderGraphParametersMetadata* ParametersMetadata = nullptr;
			const void* Parameters = nullptr;
		};

		struct FGraphParameterAllocation final
		{
			FGraphParameterAllocation(size_t Size, size_t Alignment,
				void (*InDestroy)(void*))
				: Data(::operator new(Size, std::align_val_t(Alignment))),
				  Alignment(Alignment), Destroy(InDestroy)
			{
			}

			~FGraphParameterAllocation()
			{
				if (bConstructed) Destroy(Data);
				::operator delete(Data, std::align_val_t(Alignment));
			}

			FGraphParameterAllocation(const FGraphParameterAllocation&) = delete;
			auto operator=(const FGraphParameterAllocation&)
				-> FGraphParameterAllocation& = delete;

			void* Data = nullptr;
			size_t Alignment = 0;
			void (*Destroy)(void*) = nullptr;
			bool bConstructed = false;
			bool bFrozen = false;
		};

		struct FGraphParameterStorage final
		{
			FGraphParameterStorage() = default;
			FGraphParameterStorage(FGraphParameterStorage&& Other) noexcept
				: Allocations(std::move(Other.Allocations))
			{
				Other.Allocations.clear();
			}
			auto operator=(FGraphParameterStorage&& Other) noexcept
				-> FGraphParameterStorage&
			{
				if (this != &Other)
				{
					Reset();
					Allocations = std::move(Other.Allocations);
					Other.Allocations.clear();
				}
				return *this;
			}
			~FGraphParameterStorage() { Reset(); }

			FGraphParameterStorage(const FGraphParameterStorage&) = delete;
			auto operator=(const FGraphParameterStorage&)
				-> FGraphParameterStorage& = delete;

			auto Reset() -> void
			{
				for (auto It = Allocations.rbegin(); It != Allocations.rend(); ++It)
					It->reset();
				Allocations.clear();
			}

			std::vector<std::shared_ptr<FGraphParameterAllocation>> Allocations;
		};

		struct FRangeState
		{
			FGraphUse Use;
			ERHIAccess Access = ERHIAccess::Discard;
			bool bProduced = false;
			uint32 Producer = std::numeric_limits<uint32>::max();
			std::vector<uint32> Readers;
			uint32 Version = 0;
		};

		auto IsWriteUse(ERenderGraphUse Use) -> bool
		{
			return Use != ERenderGraphUse::Read;
		}

		auto AccessHasWrite(ERHIAccess Access) -> bool
		{
			constexpr ERHIAccess Writes = ERHIAccess::ColorAttachmentReadWrite
				| ERHIAccess::DepthStencilReadWrite
				| ERHIAccess::GraphicsShaderReadWrite
				| ERHIAccess::ComputeShaderReadWrite | ERHIAccess::TransferWrite
				| ERHIAccess::HostWrite;
			return EnumHasAnyFlags(Access, Writes);
		}

		auto DescribeTexture(const FRHITexture& Texture) -> FRHITextureDesc
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
			return Desc;
		}

		auto IsAccessAllowed(ERenderGraphPassType Type, ERHIAccess Access) -> bool
		{
			ERHIAccess Allowed = ERHIAccess::None;
			switch (Type)
			{
			case ERenderGraphPassType::Graphics:
				Allowed = ERHIAccess::VertexBufferRead | ERHIAccess::IndexBufferRead
					| ERHIAccess::GraphicsUniformRead | ERHIAccess::GraphicsShaderRead
					| ERHIAccess::ColorAttachmentReadWrite
					| ERHIAccess::DepthStencilReadWrite
					| ERHIAccess::GraphicsShaderReadWrite;
				break;
			case ERenderGraphPassType::Compute:
				Allowed = ERHIAccess::ComputeUniformRead | ERHIAccess::ComputeShaderRead
					| ERHIAccess::ComputeShaderReadWrite;
				break;
			case ERenderGraphPassType::Copy:
				Allowed = ERHIAccess::TransferRead | ERHIAccess::TransferWrite;
				break;
			}
			return !EnumHasAnyFlags(Access, static_cast<ERHIAccess>(
				static_cast<uint32>(~static_cast<uint32>(Allowed))));
		}

		auto BufferRangesOverlap(const FGraphUse& A, const FGraphUse& B) -> bool
		{
			return A.BufferOffset < B.BufferOffset + B.BufferSize
				&& B.BufferOffset < A.BufferOffset + A.BufferSize;
		}

		auto TextureRangesOverlap(const FGraphUse& A, const FGraphUse& B) -> bool
		{
			const auto& X = A.TextureRange;
			const auto& Y = B.TextureRange;
			return EnumHasAnyFlags(X.Aspects, Y.Aspects)
				&& X.FirstMip < Y.FirstMip + Y.NumMips
				&& Y.FirstMip < X.FirstMip + X.NumMips
				&& X.FirstArrayLayer < Y.FirstArrayLayer + Y.NumArrayLayers
				&& Y.FirstArrayLayer < X.FirstArrayLayer + X.NumArrayLayers;
		}

		auto RangesOverlap(const FGraphUse& A, const FGraphUse& B) -> bool
		{
			return A.ResourceIndex == B.ResourceIndex && A.Kind == B.Kind
				&& (A.Kind == ERenderGraphResourceKind::Token
					|| (A.Kind == ERenderGraphResourceKind::Texture
						? TextureRangesOverlap(A, B) : BufferRangesOverlap(A, B)));
		}

		auto RangesEqual(const FGraphUse& A, const FGraphUse& B) -> bool
		{
			if (A.ResourceIndex != B.ResourceIndex || A.Kind != B.Kind)
				return false;
			return A.Kind == ERenderGraphResourceKind::Token
				? true : A.Kind == ERenderGraphResourceKind::Texture
				? A.TextureRange == B.TextureRange
				: A.BufferOffset == B.BufferOffset && A.BufferSize == B.BufferSize;
		}

		auto ContainsRange(const FGraphUse& Outer, const FGraphUse& Inner) -> bool
		{
			if (Outer.ResourceIndex != Inner.ResourceIndex || Outer.Kind != Inner.Kind)
				return false;
			if (Outer.Kind == ERenderGraphResourceKind::Token) return true;
			if (Outer.Kind == ERenderGraphResourceKind::Buffer)
				return Outer.BufferOffset <= Inner.BufferOffset
					&& Inner.BufferOffset + Inner.BufferSize
						<= Outer.BufferOffset + Outer.BufferSize;
			const auto& A = Outer.TextureRange;
			const auto& B = Inner.TextureRange;
			return EnumHasAnyFlags(A.Aspects, B.Aspects)
				&& (static_cast<uint8>(A.Aspects) & static_cast<uint8>(B.Aspects))
					== static_cast<uint8>(B.Aspects)
				&& A.FirstMip <= B.FirstMip
				&& B.FirstMip + B.NumMips <= A.FirstMip + A.NumMips
				&& A.FirstArrayLayer <= B.FirstArrayLayer
				&& B.FirstArrayLayer + B.NumArrayLayers
					<= A.FirstArrayLayer + A.NumArrayLayers;
		}

		auto DependencyKindName(ERenderGraphDependencyKind Kind) -> const char*
		{
			switch (Kind)
			{
			case ERenderGraphDependencyKind::Value: return "value";
			case ERenderGraphDependencyKind::Execution: return "execution";
			case ERenderGraphDependencyKind::Explicit: return "explicit";
			}
			return "unknown";
		}

		auto PassTypeName(ERenderGraphPassType Type) -> const char*
		{
			switch (Type)
			{
			case ERenderGraphPassType::Graphics: return "graphics";
			case ERenderGraphPassType::Compute: return "compute";
			case ERenderGraphPassType::Copy: return "copy";
			}
			return "unknown";
		}

		auto ValidateParameterMetadata(
			const FRenderGraphParametersMetadata* Metadata,
			uint32 ExpectedSize, uint32 ExpectedAlignment, std::string& OutError,
			uint32 Depth = 0) -> bool
		{
			if (Metadata == nullptr)
			{
				OutError = "render graph parameter metadata is null";
				return false;
			}
			if (Metadata->StructName == nullptr || Metadata->StructName[0] == '\0')
			{
				OutError = "render graph parameter metadata has an empty struct name";
				return false;
			}
			if (Metadata->StructSize != ExpectedSize
				|| Metadata->StructAlignment != ExpectedAlignment)
			{
				OutError = "render graph parameter metadata for '"
					+ std::string(Metadata->StructName) + "' has a mismatched layout";
				return false;
			}
			if (Depth >= 32)
			{
				OutError = "render graph parameter metadata nesting exceeds 32 levels";
				return false;
			}

			uint64 PreviousEnd = 0;
			std::vector<std::string_view> MemberNames;
			MemberNames.reserve(Metadata->Members.size());
			for (const auto& Member : Metadata->Members)
			{
				const std::string Prefix = "render graph parameter metadata for '"
					+ std::string(Metadata->StructName) + "'";
				if (Member.Name == nullptr || Member.Name[0] == '\0')
				{
					OutError = Prefix + " has an empty member name";
					return false;
				}
				if (std::ranges::find(MemberNames, Member.Name)
					!= MemberNames.end())
				{
					OutError = Prefix + " has duplicate member name '"
						+ Member.Name + "'";
					return false;
				}
				MemberNames.emplace_back(Member.Name);
				if (Member.ElementSize == 0 || Member.ArraySize == 0)
				{
					OutError = Prefix + " member '" + Member.Name
						+ "' has an empty layout";
					return false;
				}
				const uint64 End = static_cast<uint64>(Member.Offset)
					+ static_cast<uint64>(Member.ElementSize) * Member.ArraySize;
				if (Member.Offset < PreviousEnd || End > Metadata->StructSize)
				{
					OutError = Prefix + " member '" + Member.Name
						+ "' has an invalid or unstable offset";
					return false;
				}
				PreviousEnd = End;

				if (Member.Kind == ERenderGraphParameterMemberKind::Nested)
				{
					if (Member.bOptional || Member.NestedParameters == nullptr
						|| Member.ElementSize != Member.NestedParameters->StructSize
						|| !ValidateParameterMetadata(Member.NestedParameters,
							Member.NestedParameters->StructSize,
							Member.NestedParameters->StructAlignment, OutError, Depth + 1))
					{
						if (OutError.empty()) OutError = Prefix + " member '"
							+ Member.Name + "' has invalid nested metadata";
						return false;
					}
					continue;
				}
				if (Member.NestedParameters != nullptr)
				{
					OutError = Prefix + " member '" + Member.Name
						+ "' unexpectedly has nested metadata";
					return false;
				}
				uint32 ExpectedElementSize = 0;
				switch (Member.Kind)
				{
				case ERenderGraphParameterMemberKind::Texture:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRenderGraphTextureParameter>)
						: sizeof(FRenderGraphTextureParameter);
					break;
				case ERenderGraphParameterMemberKind::Buffer:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRenderGraphBufferParameter>)
						: sizeof(FRenderGraphBufferParameter);
					break;
				case ERenderGraphParameterMemberKind::Token:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRenderGraphTokenParameter>)
						: sizeof(FRenderGraphTokenParameter);
					break;
				case ERenderGraphParameterMemberKind::ColorAttachment:
				case ERenderGraphParameterMemberKind::ManagedColorAttachment:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRenderGraphColorAttachmentParameter>)
						: sizeof(FRenderGraphColorAttachmentParameter);
					break;
				case ERenderGraphParameterMemberKind::DepthStencilAttachment:
				case ERenderGraphParameterMemberKind::ManagedDepthStencilAttachment:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<
							FRenderGraphDepthStencilAttachmentParameter>)
						: sizeof(FRenderGraphDepthStencilAttachmentParameter);
					break;
				case ERenderGraphParameterMemberKind::ManagedTexture:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRenderGraphManagedTextureParameter>)
						: sizeof(FRenderGraphManagedTextureParameter);
					break;
				case ERenderGraphParameterMemberKind::Nested: break;
				}
				if (Member.ElementSize != ExpectedElementSize)
				{
					OutError = Prefix + " member '" + Member.Name
						+ "' has a mismatched wrapper layout";
					return false;
				}

				const bool bTextureKind = Member.ResourceKind
					== ERenderGraphResourceKind::Texture;
				const bool bBufferKind = Member.ResourceKind
					== ERenderGraphResourceKind::Buffer;
				const bool bTokenKind = Member.ResourceKind
					== ERenderGraphResourceKind::Token;
				bool bShapeValid = false;
				switch (Member.Kind)
				{
				case ERenderGraphParameterMemberKind::Texture:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERenderGraphParameterRangeKind::TextureSubresource
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERenderGraphParameterMemberKind::Buffer:
					bShapeValid = bBufferKind
						&& Member.RangeKind == ERenderGraphParameterRangeKind::BufferBytes
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERenderGraphParameterMemberKind::Token:
					bShapeValid = bTokenKind
						&& Member.RangeKind == ERenderGraphParameterRangeKind::None
						&& Member.Access == ERHIAccess::None
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERenderGraphParameterMemberKind::ColorAttachment:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERenderGraphParameterRangeKind::TextureSubresource
						&& Member.Use == ERenderGraphUse::ReadWrite
						&& Member.Access == ERHIAccess::ColorAttachmentReadWrite
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERenderGraphParameterMemberKind::DepthStencilAttachment:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERenderGraphParameterRangeKind::TextureSubresource
						&& Member.Use == ERenderGraphUse::ReadWrite
						&& Member.Access == ERHIAccess::DepthStencilReadWrite
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERenderGraphParameterMemberKind::ManagedColorAttachment:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERenderGraphParameterRangeKind::TextureSubresource
						&& Member.Use == ERenderGraphUse::ReadWrite
						&& Member.Access == ERHIAccess::ColorAttachmentReadWrite
						&& Member.bPassManagedTransition
						&& Member.ResultAccess != ERHIAccess::None;
					break;
				case ERenderGraphParameterMemberKind::ManagedDepthStencilAttachment:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERenderGraphParameterRangeKind::TextureSubresource
						&& Member.Use == ERenderGraphUse::ReadWrite
						&& Member.Access == ERHIAccess::DepthStencilReadWrite
						&& Member.bPassManagedTransition
						&& Member.ResultAccess != ERHIAccess::None;
					break;
				case ERenderGraphParameterMemberKind::ManagedTexture:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERenderGraphParameterRangeKind::TextureSubresource
						&& Member.bPassManagedTransition
						&& Member.ResultAccess != ERHIAccess::None;
					break;
				case ERenderGraphParameterMemberKind::Nested: break;
				}
				if (!bShapeValid)
				{
					OutError = Prefix + " member '" + Member.Name
						+ "' has inconsistent declaration semantics";
					return false;
				}
			}
			return true;
		}

		auto PassUsePrefix(const FGraphPass& Pass, const FGraphUse& Use)
			-> std::string
		{
			std::string Prefix = "pass '" + Pass.Name + "'";
			if (!Use.ParameterPath.empty())
				Prefix += " parameter '" + Use.ParameterPath + "'";
			return Prefix;
		}

		auto ValidatePassUses(const FGraphPass& Pass,
			std::span<const FGraphResource> Resources, std::string& OutError)
			-> bool
		{
			for (uint32 UseIndex = 0; UseIndex < Pass.Uses.size(); ++UseIndex)
			{
				const auto& Use = Pass.Uses[UseIndex];
				const std::string Prefix = PassUsePrefix(Pass, Use);
				if (Use.ResourceIndex >= Resources.size()
					|| Resources[Use.ResourceIndex].Kind != Use.Kind)
				{
					OutError = Prefix + " has an invalid resource handle";
					return false;
				}
				const auto& Resource = Resources[Use.ResourceIndex];
				if (Use.Kind != ERenderGraphResourceKind::Token
					&& (Use.Access == ERHIAccess::None
						|| EnumHasAnyFlags(Use.Access, ERHIAccess::Discard)))
				{
					OutError = Prefix + " resource '" + Resource.Name
						+ "' has invalid required access";
					return false;
				}
				if (Use.Kind != ERenderGraphResourceKind::Token
					&& !IsAccessAllowed(Pass.Type, Use.Access))
				{
					OutError = Prefix + " resource '" + Resource.Name
						+ "' access is incompatible with pass domain";
					return false;
				}
				if (Use.Kind != ERenderGraphResourceKind::Token
					&& ((Use.Use == ERenderGraphUse::Read
							&& AccessHasWrite(Use.Access))
						|| (Use.Use == ERenderGraphUse::Write
							&& !AccessHasWrite(Use.Access))))
				{
					OutError = Prefix + " resource '" + Resource.Name
						+ "' access disagrees with use mode";
					return false;
				}
				if (Use.bDiscard && Use.Use == ERenderGraphUse::Read)
				{
					OutError = Prefix + " cannot discard a read";
					return false;
				}
				if (Use.bPassManagedTransition
					&& (Use.ResultAccess == ERHIAccess::None
						|| EnumHasAnyFlags(Use.ResultAccess, ERHIAccess::Discard)))
				{
					OutError = Prefix + " resource '" + Resource.Name
						+ "' has invalid managed attachment result access";
					return false;
				}
				if (Use.Kind == ERenderGraphResourceKind::Buffer
					&& (Use.BufferSize == 0
						|| Use.BufferOffset > Resource.BufferDesc.Size
						|| Use.BufferSize > Resource.BufferDesc.Size
							- Use.BufferOffset))
				{
					OutError = Prefix + " resource '" + Resource.Name
						+ "' has invalid buffer range";
					return false;
				}
				if (Use.Kind == ERenderGraphResourceKind::Texture
					&& (Use.TextureRange.Aspects == ERHITextureAspect::None
						|| Use.TextureRange.NumMips == 0
						|| Use.TextureRange.NumArrayLayers == 0
						|| Use.TextureRange.FirstMip + Use.TextureRange.NumMips
							> Resource.TextureDesc.NumMips
						|| Use.TextureRange.FirstArrayLayer
							+ Use.TextureRange.NumArrayLayers
							> Resource.TextureDesc.ArraySize
						|| !EnumHasAnyFlags(
							GetTextureAspects(Resource.TextureDesc.Format),
							Use.TextureRange.Aspects)))
				{
					OutError = Prefix + " resource '" + Resource.Name
						+ "' has invalid texture range";
					return false;
				}
				for (uint32 OtherUse = 0; OtherUse < UseIndex; ++OtherUse)
					if (RangesOverlap(Use, Pass.Uses[OtherUse]))
					{
						OutError = Prefix
							+ " declares overlapping uses of resource '"
							+ Resource.Name + "'";
						return false;
					}
			}
			return true;
		}
	} // namespace

	struct FRenderGraphBuilder::FState
	{
		uint64 Owner = 0;
		std::vector<FGraphResource> Resources;
		std::vector<FGraphPass> Passes;
		std::vector<std::string> DeclarationErrors;
		bool bEnableCulling = false;
		FRenderGraphPrepare Prepare;
		FRenderGraphBackingResolver Resolver;
		FRenderGraphBudget Budget;
		mutable FGraphParameterStorage ParameterStorage;
		mutable bool bParameterStorageTransferred = false;
	};

	struct FCompiledRenderGraph::FState
	{
		uint64 Owner = 0;
		std::vector<FGraphResource> Resources;
		std::vector<FCompiledRenderGraphPass> Passes;
		std::vector<FRenderGraphDependency> Dependencies;
		std::vector<FRenderGraphResourceLifetime> ResourceLifetimes;
		std::vector<FRenderGraphCullingDecision> CullingDecisions;
		std::vector<FRHIBufferTransition> FinalBufferTransitions;
		std::vector<FRHITextureTransition> FinalTextureTransitions;
		std::vector<FRenderGraphExecute> ExecuteCallbacks;
		std::vector<FRenderGraphParameterizedExecute> ParameterizedExecuteCallbacks;
		std::vector<const FRenderGraphParametersMetadata*> PassParametersMetadata;
		std::vector<const void*> PassParameters;
		std::vector<std::vector<uint32>> PassResourceIndices;
		std::vector<std::vector<uint32>> PassBufferTransitionResources;
		std::vector<std::vector<uint32>> PassTextureTransitionResources;
		std::vector<uint32> FinalBufferTransitionResources;
		std::vector<uint32> FinalTextureTransitionResources;
		std::vector<FRenderGraphResourceCapture> ResourceCaptures;
		std::vector<FRenderGraphUseCapture> UseCaptures;
		std::vector<FRenderGraphTransitionCapture> TransitionCaptures;
		std::vector<FRenderGraphPreparationRequest> PreparationRequests;
		FRenderGraphPrepare Prepare;
		FRenderGraphBackingResolver Resolver;
		FRenderGraphBudget Budget;
		FGraphParameterStorage ParameterStorage;
		uint64 CompileMicroseconds = 0;
		mutable std::atomic<uint64> ExecuteMicroseconds = 0;
	};

	FRenderGraphBuilder::FRenderGraphBuilder()
		: State(std::make_unique<FState>())
	{
		static std::atomic<uint64> NextOwner{1};
		State->Owner = NextOwner.fetch_add(1, std::memory_order_relaxed);
	}

	FRenderGraphBuilder::~FRenderGraphBuilder() = default;

	auto FRenderGraphBuilder::AllocateParameterStorage(size_t Size,
		size_t Alignment, const FRenderGraphParametersMetadata* Metadata,
		void (*Destroy)(void*), std::weak_ptr<void>& OutLifetime) -> void*
	{
		if (State->bParameterStorageTransferred)
		{
			State->DeclarationErrors.emplace_back(
				"render graph parameter storage was already transferred");
			return nullptr;
		}
		std::string Error;
		if (!ValidateParameterMetadata(Metadata, static_cast<uint32>(Size),
			static_cast<uint32>(Alignment), Error))
		{
			State->DeclarationErrors.push_back(std::move(Error));
			return nullptr;
		}
		auto Allocation = std::make_shared<FGraphParameterAllocation>(
			Size, Alignment, Destroy);
		void* Data = Allocation->Data;
		OutLifetime = Allocation;
		State->ParameterStorage.Allocations.push_back(std::move(Allocation));
		return Data;
	}

	auto FRenderGraphBuilder::MarkParameterStorageConstructed(void* Storage)
		-> void
	{
		auto Allocation = std::ranges::find_if(
			State->ParameterStorage.Allocations,
			[Storage](const auto& Candidate) { return Candidate->Data == Storage; });
		if (Allocation != State->ParameterStorage.Allocations.end())
			(*Allocation)->bConstructed = true;
	}

	auto FRenderGraphBuilder::ImportTexture(std::string_view Name,
		FRHITexture* Texture, ERHIAccess InitialAccess, ERHIAccess FinalAccess)
		-> FRenderGraphTextureHandle
	{
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERenderGraphResourceKind::Texture;
		Resource.Texture = Texture;
		if (Texture != nullptr) Resource.TextureDesc = DescribeTexture(*Texture);
		Resource.InitialAccess = InitialAccess;
		Resource.FinalAccess = FinalAccess;
		Resource.bImported = true;
		State->Resources.push_back(std::move(Resource));
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::CreateTexture(std::string_view Name,
		FRHITexture* Texture, ERHIAccess FinalAccess)
		-> FRenderGraphTextureHandle
	{
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERenderGraphResourceKind::Texture;
		Resource.Texture = Texture;
		if (Texture != nullptr) Resource.TextureDesc = DescribeTexture(*Texture);
		Resource.FinalAccess = FinalAccess;
		Resource.BackingClass = "prebound";
		State->Resources.push_back(std::move(Resource));
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::CreateTexture(std::string_view Name,
		const FRenderGraphTextureDesc& Desc, ERHIAccess FinalAccess)
		-> FRenderGraphTextureHandle
	{
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERenderGraphResourceKind::Texture;
		Resource.TextureDesc = Desc.Texture;
		Resource.BackingClass = Desc.BackingClass;
		Resource.FinalAccess = FinalAccess;
		Resource.bRequiresBacking = true;
		State->Resources.push_back(std::move(Resource));
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::ImportBuffer(std::string_view Name,
		FRHIBuffer* Buffer, ERHIAccess InitialAccess, ERHIAccess FinalAccess)
		-> FRenderGraphBufferHandle
	{
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERenderGraphResourceKind::Buffer;
		Resource.Buffer = Buffer;
		if (Buffer != nullptr) Resource.BufferDesc = Buffer->GetDesc();
		Resource.InitialAccess = InitialAccess;
		Resource.FinalAccess = FinalAccess;
		Resource.bImported = true;
		State->Resources.push_back(std::move(Resource));
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::CreateBuffer(std::string_view Name,
		FRHIBuffer* Buffer, ERHIAccess FinalAccess)
		-> FRenderGraphBufferHandle
	{
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERenderGraphResourceKind::Buffer;
		Resource.Buffer = Buffer;
		if (Buffer != nullptr) Resource.BufferDesc = Buffer->GetDesc();
		Resource.FinalAccess = FinalAccess;
		Resource.BackingClass = "prebound";
		State->Resources.push_back(std::move(Resource));
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::CreateBuffer(std::string_view Name,
		const FRenderGraphBufferDesc& Desc, ERHIAccess FinalAccess)
		-> FRenderGraphBufferHandle
	{
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERenderGraphResourceKind::Buffer;
		Resource.BufferDesc = Desc.Buffer;
		Resource.BackingClass = Desc.BackingClass;
		Resource.FinalAccess = FinalAccess;
		Resource.bRequiresBacking = true;
		State->Resources.push_back(std::move(Resource));
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::CreateToken(std::string_view Name)
		-> FRenderGraphTokenHandle
	{
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERenderGraphResourceKind::Token;
		State->Resources.push_back(std::move(Resource));
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::AddPass(std::string_view Name,
		ERenderGraphPassType Type, FRenderGraphExecute Execute)
		-> FRenderGraphPassHandle
	{
		const uint32 Index = static_cast<uint32>(State->Passes.size());
		FGraphPass Pass;
		Pass.Name = Name;
		Pass.Type = Type;
		Pass.Execute = std::move(Execute);
		State->Passes.push_back(std::move(Pass));
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::AddParameterizedPass(std::string_view Name,
		ERenderGraphPassType Type,
		const FRenderGraphParametersMetadata* Metadata, void* Parameters,
		std::shared_ptr<void> Lifetime, FRenderGraphExecute Execute,
		FRenderGraphParameterizedExecute ParameterizedExecute)
		-> FRenderGraphPassHandle
	{
		const std::string StructName = Metadata != nullptr
			&& Metadata->StructName != nullptr ? Metadata->StructName : "FParameters";
		const std::string RootPrefix = "pass '" + std::string(Name)
			+ "' parameter '" + StructName + "'";
		auto Allocation = std::ranges::find_if(
			State->ParameterStorage.Allocations,
			[&](const auto& Candidate) {
				return Candidate.get() == Lifetime.get()
					&& Candidate->Data == Parameters;
			});
		if (Parameters == nullptr || Lifetime == nullptr
			|| Allocation == State->ParameterStorage.Allocations.end())
		{
			State->DeclarationErrors.push_back(
				RootPrefix + " has an invalid or foreign parameter allocation");
			return {};
		}
		if ((*Allocation)->bFrozen)
		{
			State->DeclarationErrors.push_back(
				RootPrefix + " was already submitted");
			return {};
		}
		(*Allocation)->bFrozen = true;

		FGraphPass ParameterizedPass;
		ParameterizedPass.Name = Name;
		ParameterizedPass.Type = Type;
		ParameterizedPass.Execute = std::move(Execute);
		ParameterizedPass.ParameterizedExecute = std::move(ParameterizedExecute);
		ParameterizedPass.bParameterized = true;
		ParameterizedPass.ParametersMetadata = Metadata;
		ParameterizedPass.Parameters = Parameters;

		std::function<void(const void*, const FRenderGraphParametersMetadata*,
			const std::string&)> Traverse;
		Traverse = [&](const void* StructData,
			const FRenderGraphParametersMetadata* StructMetadata,
			const std::string& ParentPath) {
			const auto* Bytes = static_cast<const std::byte*>(StructData);
			for (const auto& Member : StructMetadata->Members)
			{
				for (uint32 ElementIndex = 0;
					ElementIndex < Member.ArraySize; ++ElementIndex)
				{
					const void* ElementData = Bytes + Member.Offset
						+ static_cast<size_t>(ElementIndex) * Member.ElementSize;
					std::string FieldPath = ParentPath + "." + Member.Name;
					if (Member.ArraySize > 1)
						FieldPath += "[" + std::to_string(ElementIndex) + "]";
					if (Member.Kind == ERenderGraphParameterMemberKind::Nested)
					{
						Traverse(ElementData, Member.NestedParameters, FieldPath);
						continue;
					}

					FGraphUse DeclaredUse;
					DeclaredUse.Kind = Member.ResourceKind;
					DeclaredUse.Use = Member.Use;
					DeclaredUse.Access = Member.Access;
					DeclaredUse.bDiscard = Member.bDiscard;
					DeclaredUse.bPassManagedTransition =
						Member.bPassManagedTransition;
					DeclaredUse.ResultAccess = Member.ResultAccess;
					DeclaredUse.ParameterPath = FieldPath;

					auto Visit = [&]<typename Wrapper>(auto&& ReadWrapper) {
						if (Member.bOptional)
						{
							const auto& Optional = *static_cast<
								const std::optional<Wrapper>*>(ElementData);
							if (!Optional.has_value()) return false;
							ReadWrapper(*Optional);
						}
						else ReadWrapper(*static_cast<const Wrapper*>(ElementData));
						return true;
					};

					bool bPresent = false;
					switch (Member.Kind)
					{
					case ERenderGraphParameterMemberKind::Texture:
						bPresent = Visit.template operator()<
							FRenderGraphTextureParameter>([&](const auto& Value) {
								DeclaredUse.ResourceIndex = Value.Texture.Owner
									== State->Owner ? Value.Texture.Index
									: std::numeric_limits<uint32>::max();
								DeclaredUse.TextureRange = Value.Range;
							});
						break;
					case ERenderGraphParameterMemberKind::Buffer:
						bPresent = Visit.template operator()<
							FRenderGraphBufferParameter>([&](const auto& Value) {
								DeclaredUse.ResourceIndex = Value.Buffer.Owner
									== State->Owner ? Value.Buffer.Index
									: std::numeric_limits<uint32>::max();
								DeclaredUse.BufferOffset = Value.Offset;
								DeclaredUse.BufferSize = Value.Size;
							});
						break;
					case ERenderGraphParameterMemberKind::Token:
						bPresent = Visit.template operator()<
							FRenderGraphTokenParameter>([&](const auto& Value) {
								DeclaredUse.ResourceIndex = Value.Token.Owner
									== State->Owner ? Value.Token.Index
									: std::numeric_limits<uint32>::max();
								DeclaredUse.bDiscard = Member.Use
									!= ERenderGraphUse::Read;
							});
						break;
					case ERenderGraphParameterMemberKind::ColorAttachment:
					case ERenderGraphParameterMemberKind::ManagedColorAttachment:
						bPresent = Visit.template operator()<
							FRenderGraphColorAttachmentParameter>(
							[&](const auto& Value) {
								DeclaredUse.ResourceIndex = Value.Texture.Owner
									== State->Owner ? Value.Texture.Index
									: std::numeric_limits<uint32>::max();
								DeclaredUse.TextureRange = Value.Range;
							});
						DeclaredUse.bDiscard = Member.LoadAction
							!= ERHIRenderTargetLoadAction::Load;
						DeclaredUse.bStore = Member.StoreAction
							== ERHIRenderTargetStoreAction::Store;
						break;
					case ERenderGraphParameterMemberKind::DepthStencilAttachment:
					case ERenderGraphParameterMemberKind::ManagedDepthStencilAttachment:
						bPresent = Visit.template operator()<
							FRenderGraphDepthStencilAttachmentParameter>(
							[&](const auto& Value) {
								DeclaredUse.ResourceIndex = Value.Texture.Owner
									== State->Owner ? Value.Texture.Index
									: std::numeric_limits<uint32>::max();
								DeclaredUse.TextureRange = Value.Range;
							});
						DeclaredUse.bDiscard = Member.LoadAction
							!= ERHIRenderTargetLoadAction::Load;
						DeclaredUse.bStore = Member.StoreAction
							== ERHIRenderTargetStoreAction::Store;
						break;
					case ERenderGraphParameterMemberKind::ManagedTexture:
						bPresent = Visit.template operator()<
							FRenderGraphManagedTextureParameter>([&](const auto& Value) {
								DeclaredUse.ResourceIndex = Value.Texture.Owner
									== State->Owner ? Value.Texture.Index
									: std::numeric_limits<uint32>::max();
								DeclaredUse.TextureRange = Value.Range;
							});
						break;
					case ERenderGraphParameterMemberKind::Nested: break;
					}
					if (bPresent)
						ParameterizedPass.Uses.push_back(std::move(DeclaredUse));
				}
			}
		};
		Traverse(Parameters, Metadata, StructName);

		std::string UseError;
		if (!ValidatePassUses(ParameterizedPass, State->Resources, UseError))
		{
			State->DeclarationErrors.push_back(std::move(UseError));
			return {};
		}
		const uint32 Index = static_cast<uint32>(State->Passes.size());
		State->Passes.push_back(std::move(ParameterizedPass));
		return {State->Owner, Index};
	}

	auto FRenderGraphBuilder::CanDeclareManualUse(FRenderGraphPassHandle Pass,
		std::string_view InvalidHandleError) -> bool
	{
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size())
		{
			State->DeclarationErrors.emplace_back(InvalidHandleError);
			return false;
		}
		if (State->Passes[Pass.Index].bParameterized)
		{
			State->DeclarationErrors.push_back("pass '"
				+ State->Passes[Pass.Index].Name
				+ "' uses parameter declarations and cannot accept manual uses");
			return false;
		}
		return true;
	}

	auto FRenderGraphBuilder::MarkPassRoot(FRenderGraphPassHandle Pass,
		std::string_view Reason) -> void
	{
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size())
		{
			State->DeclarationErrors.emplace_back("root has an invalid pass handle");
			return;
		}
		State->Passes[Pass.Index].bRoot = true;
		State->Passes[Pass.Index].RootReason = std::string(Reason);
	}

	auto FRenderGraphBuilder::EnablePassCulling() -> void
	{
		State->bEnableCulling = true;
	}

	auto FRenderGraphBuilder::SetExecutionPreparation(FRenderGraphPrepare Prepare)
		-> void
	{
		State->Prepare = std::move(Prepare);
	}

	auto FRenderGraphBuilder::SetBackingResolver(
		FRenderGraphBackingResolver Resolver) -> void
	{
		State->Resolver = std::move(Resolver);
	}

	auto FRenderGraphBuilder::SetBudget(const FRenderGraphBudget& Budget) -> void
	{
		State->Budget = Budget;
	}

	auto FRenderGraphBuilder::AddDependency(FRenderGraphPassHandle Pass,
		FRenderGraphPassHandle Prerequisite) -> void
	{
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size())
		{
			State->DeclarationErrors.emplace_back(
				"dependency has an invalid destination pass handle");
			return;
		}
		State->Passes[Pass.Index].Prerequisites.push_back(
			Prerequisite.Owner == State->Owner ? Prerequisite.Index
				: std::numeric_limits<uint32>::max());
	}

	auto FRenderGraphBuilder::UseTexture(FRenderGraphPassHandle Pass,
		FRenderGraphTextureHandle Texture,
		const FRHITextureSubresourceRange& Range, ERenderGraphUse Use,
		ERHIAccess Access, bool bDiscard) -> void
	{
		if (!CanDeclareManualUse(Pass,
			"texture use has an invalid pass handle")) return;
		State->Passes[Pass.Index].Uses.push_back({
			Texture.Owner == State->Owner ? Texture.Index
				: std::numeric_limits<uint32>::max(),
			ERenderGraphResourceKind::Texture, Use, Access, bDiscard, Range});
	}

	auto FRenderGraphBuilder::UseBuffer(FRenderGraphPassHandle Pass,
		FRenderGraphBufferHandle Buffer, uint64 Offset, uint64 Size,
		ERenderGraphUse Use, ERHIAccess Access, bool bDiscard) -> void
	{
		if (!CanDeclareManualUse(Pass,
			"buffer use has an invalid pass handle")) return;
		FGraphUse DeclaredUse;
		DeclaredUse.ResourceIndex = Buffer.Owner == State->Owner ? Buffer.Index
			: std::numeric_limits<uint32>::max();
		DeclaredUse.Kind = ERenderGraphResourceKind::Buffer;
		DeclaredUse.Use = Use;
		DeclaredUse.Access = Access;
		DeclaredUse.bDiscard = bDiscard;
		DeclaredUse.BufferOffset = Offset;
		DeclaredUse.BufferSize = Size;
		State->Passes[Pass.Index].Uses.push_back(DeclaredUse);
	}

	auto FRenderGraphBuilder::UseColorAttachment(FRenderGraphPassHandle Pass,
		FRenderGraphTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction) -> void
	{
		if (!CanDeclareManualUse(Pass,
			"texture use has an invalid pass handle")) return;
		UseTexture(Pass, Texture, Range, ERenderGraphUse::ReadWrite,
			ERHIAccess::ColorAttachmentReadWrite,
			LoadAction != ERHIRenderTargetLoadAction::Load);
		if (Pass.Owner == State->Owner && Pass.Index < State->Passes.size())
			State->Passes[Pass.Index].Uses.back().bStore =
				StoreAction == ERHIRenderTargetStoreAction::Store;
	}

	auto FRenderGraphBuilder::UseDepthStencilAttachment(
		FRenderGraphPassHandle Pass, FRenderGraphTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction) -> void
	{
		if (!CanDeclareManualUse(Pass,
			"texture use has an invalid pass handle")) return;
		UseTexture(Pass, Texture, Range, ERenderGraphUse::ReadWrite,
			ERHIAccess::DepthStencilReadWrite,
			LoadAction != ERHIRenderTargetLoadAction::Load);
		if (Pass.Owner == State->Owner && Pass.Index < State->Passes.size())
			State->Passes[Pass.Index].Uses.back().bStore =
				StoreAction == ERHIRenderTargetStoreAction::Store;
	}

	auto FRenderGraphBuilder::UseManagedColorAttachment(
		FRenderGraphPassHandle Pass, FRenderGraphTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction, ERHIAccess ResultAccess) -> void
	{
		if (!CanDeclareManualUse(Pass,
			"texture use has an invalid pass handle")) return;
		UseColorAttachment(Pass, Texture, Range, LoadAction, StoreAction);
		if (Pass.Owner == State->Owner && Pass.Index < State->Passes.size())
		{
			auto& Use = State->Passes[Pass.Index].Uses.back();
			Use.bPassManagedTransition = true;
			Use.ResultAccess = ResultAccess;
		}
	}

	auto FRenderGraphBuilder::UseManagedDepthStencilAttachment(
		FRenderGraphPassHandle Pass, FRenderGraphTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction, ERHIAccess ResultAccess) -> void
	{
		if (!CanDeclareManualUse(Pass,
			"texture use has an invalid pass handle")) return;
		UseDepthStencilAttachment(Pass, Texture, Range, LoadAction, StoreAction);
		if (Pass.Owner == State->Owner && Pass.Index < State->Passes.size())
		{
			auto& Use = State->Passes[Pass.Index].Uses.back();
			Use.bPassManagedTransition = true;
			Use.ResultAccess = ResultAccess;
		}
	}

	auto FRenderGraphBuilder::UseManagedTexture(FRenderGraphPassHandle Pass,
		FRenderGraphTextureHandle Texture,
		const FRHITextureSubresourceRange& Range, ERenderGraphUse Use,
		ERHIAccess EntryAccess, ERHIAccess ResultAccess, bool bDiscard) -> void
	{
		if (!CanDeclareManualUse(Pass,
			"texture use has an invalid pass handle")) return;
		UseTexture(Pass, Texture, Range, Use, EntryAccess, bDiscard);
		if (Pass.Owner == State->Owner && Pass.Index < State->Passes.size())
		{
			auto& DeclaredUse = State->Passes[Pass.Index].Uses.back();
			DeclaredUse.bPassManagedTransition = true;
			DeclaredUse.ResultAccess = ResultAccess;
		}
	}

	auto FRenderGraphBuilder::UseToken(FRenderGraphPassHandle Pass,
		FRenderGraphTokenHandle Token, ERenderGraphUse Use) -> void
	{
		if (!CanDeclareManualUse(Pass,
			"token use has an invalid pass handle")) return;
		FGraphUse DeclaredUse;
		DeclaredUse.ResourceIndex = Token.Owner == State->Owner ? Token.Index
			: std::numeric_limits<uint32>::max();
		DeclaredUse.Kind = ERenderGraphResourceKind::Token;
		DeclaredUse.Use = Use;
		DeclaredUse.bDiscard = Use != ERenderGraphUse::Read;
		State->Passes[Pass.Index].Uses.push_back(DeclaredUse);
	}

	auto FRenderGraphBuilder::Compile() const -> FRenderGraphCompileResult
	{
		const auto Started = std::chrono::steady_clock::now();
		auto Fail = [](std::string Error) {
			return FRenderGraphCompileResult{nullptr, std::move(Error)};
		};
		if (!State->DeclarationErrors.empty())
			return Fail(State->DeclarationErrors.front());
		if (State->bParameterStorageTransferred)
			return Fail("render graph parameter storage was already transferred");
		for (uint32 ResourceIndex = 0; ResourceIndex < State->Resources.size(); ++ResourceIndex)
		{
			const auto& Resource = State->Resources[ResourceIndex];
			if (Resource.Name.empty())
				return Fail("resource[" + std::to_string(ResourceIndex) + "] has an empty name");
			if (Resource.Kind != ERenderGraphResourceKind::Token
				&& !Resource.bRequiresBacking && Resource.Texture == nullptr
				&& Resource.Buffer == nullptr)
				return Fail("resource '" + Resource.Name + "' has no physical resource");
			if (Resource.bRequiresBacking && !State->Resolver)
				return Fail("logical resource '" + Resource.Name + "' has no backing resolver");
			if (Resource.bImported && Resource.FinalAccess == ERHIAccess::None)
				return Fail("imported resource '" + Resource.Name + "' has no final access");
			if (EnumHasAnyFlags(Resource.FinalAccess, ERHIAccess::Discard))
				return Fail("resource '" + Resource.Name + "' has invalid final access");
			for (uint32 Other = 0; Other < ResourceIndex; ++Other)
			{
				const auto& Previous = State->Resources[Other];
				if (Previous.Name == Resource.Name)
					return Fail("duplicate resource name '" + Resource.Name + "'");
				if (Resource.bImported && Previous.bImported
					&& Resource.Kind == Previous.Kind
					&& ((Resource.Texture != nullptr && Resource.Texture == Previous.Texture)
						|| (Resource.Buffer != nullptr && Resource.Buffer == Previous.Buffer)))
					return Fail("duplicate imported physical resource '" + Resource.Name + "'");
			}
		}

		const uint32 PassCount = static_cast<uint32>(State->Passes.size());
		const uint32 ResourceCount = static_cast<uint32>(State->Resources.size());
		size_t DeclaredUseCount = 0;
		size_t ExplicitDependencyCount = 0;
		for (const auto& Pass : State->Passes)
		{
			DeclaredUseCount += Pass.Uses.size();
			ExplicitDependencyCount += Pass.Prerequisites.size();
		}
		if (PassCount > State->Budget.MaxPasses)
			return Fail("render graph safety limit exceeded: passes actual="
				+ std::to_string(PassCount) + " limit=" + std::to_string(State->Budget.MaxPasses));
		std::vector<std::vector<uint32>> Outgoing(PassCount);
		std::vector<uint32> Indegree(PassCount, 0);
		std::vector<FRenderGraphDependency> Dependencies;
		Dependencies.reserve(ExplicitDependencyCount + DeclaredUseCount);
		auto AddEdge = [&](uint32 Before, uint32 After, const std::string& Cause,
			ERenderGraphDependencyKind Kind) {
			if (Before == After) return;
			auto Existing = std::ranges::find_if(Dependencies, [&](const auto& Edge) {
				return Edge.BeforePass == Before && Edge.AfterPass == After;
			});
			if (Existing != Dependencies.end())
			{
				if (Existing->Kind == ERenderGraphDependencyKind::Execution
					&& Kind != ERenderGraphDependencyKind::Execution)
				{
					Existing->Kind = Kind;
					Existing->Cause = Cause;
				}
				return;
			}
			Outgoing[Before].push_back(After);
			++Indegree[After];
			Dependencies.push_back({Before, After, Cause, Kind});
		};

		for (uint32 PassIndex = 0; PassIndex < PassCount; ++PassIndex)
		{
			const auto& Pass = State->Passes[PassIndex];
			if (Pass.Name.empty())
				return Fail("pass[" + std::to_string(PassIndex) + "] has an empty name");
			for (uint32 Other = 0; Other < PassIndex; ++Other)
				if (State->Passes[Other].Name == Pass.Name)
					return Fail("duplicate pass name '" + Pass.Name + "'");
			for (uint32 Prerequisite : Pass.Prerequisites)
			{
				if (Prerequisite >= PassCount)
					return Fail("pass '" + Pass.Name + "' has an invalid prerequisite");
				AddEdge(Prerequisite, PassIndex, "explicit", ERenderGraphDependencyKind::Explicit);
			}
			std::string UseError;
			if (!ValidatePassUses(Pass, State->Resources, UseError))
				return Fail(std::move(UseError));
		}

		std::vector<std::vector<const FGraphUse*>> ResourceUses(ResourceCount);
		std::vector<size_t> ResourceUseCounts(ResourceCount, 0);
		for (const auto& Pass : State->Passes)
			for (const auto& Use : Pass.Uses)
				++ResourceUseCounts[Use.ResourceIndex];
		for (uint32 ResourceIndex = 0; ResourceIndex < ResourceCount; ++ResourceIndex)
			ResourceUses[ResourceIndex].reserve(ResourceUseCounts[ResourceIndex]);
		for (const auto& Pass : State->Passes)
			for (const auto& Use : Pass.Uses)
				ResourceUses[Use.ResourceIndex].push_back(&Use);

		std::vector<FRangeState> Cells;
		Cells.reserve(DeclaredUseCount);
		for (uint32 ResourceIndex = 0; ResourceIndex < ResourceCount; ++ResourceIndex)
		{
			const auto& Resource = State->Resources[ResourceIndex];
			const auto& Uses = ResourceUses[ResourceIndex];
			if (Uses.empty()) continue;
			if (Resource.Kind == ERenderGraphResourceKind::Token)
			{
				FRangeState Cell;
				Cell.Use = *Uses.front();
				Cell.bProduced = Resource.bImported;
				Cell.Access = Resource.InitialAccess;
				Cells.push_back(std::move(Cell));
				continue;
			}
			if (Resource.Kind == ERenderGraphResourceKind::Buffer)
			{
				std::vector<uint64> Cuts;
				for (const FGraphUse* Use : Uses)
				{
					Cuts.push_back(Use->BufferOffset);
					Cuts.push_back(Use->BufferOffset + Use->BufferSize);
				}
				std::ranges::sort(Cuts);
				Cuts.erase(std::unique(Cuts.begin(), Cuts.end()), Cuts.end());
				for (size_t Index = 1; Index < Cuts.size(); ++Index)
				{
					FGraphUse CellUse = *Uses.front();
					CellUse.BufferOffset = Cuts[Index - 1];
					CellUse.BufferSize = Cuts[Index] - Cuts[Index - 1];
					if (!std::ranges::any_of(Uses,
						[&](const FGraphUse* Use) { return ContainsRange(*Use, CellUse); }))
						continue;
					FRangeState Cell;
					Cell.Use = CellUse;
					Cell.bProduced = Resource.bImported;
					Cell.Access = Resource.InitialAccess;
					Cells.push_back(std::move(Cell));
				}
				continue;
			}
			for (ERHITextureAspect Aspect : {ERHITextureAspect::Color,
				ERHITextureAspect::Depth, ERHITextureAspect::Stencil})
			{
				std::vector<uint32> MipCuts;
				std::vector<uint32> LayerCuts;
				for (const FGraphUse* Use : Uses)
					if (EnumHasAnyFlags(Use->TextureRange.Aspects, Aspect))
					{
						MipCuts.push_back(Use->TextureRange.FirstMip);
						MipCuts.push_back(Use->TextureRange.FirstMip + Use->TextureRange.NumMips);
						LayerCuts.push_back(Use->TextureRange.FirstArrayLayer);
						LayerCuts.push_back(Use->TextureRange.FirstArrayLayer + Use->TextureRange.NumArrayLayers);
					}
				std::ranges::sort(MipCuts);
				std::ranges::sort(LayerCuts);
				MipCuts.erase(std::unique(MipCuts.begin(), MipCuts.end()), MipCuts.end());
				LayerCuts.erase(std::unique(LayerCuts.begin(), LayerCuts.end()), LayerCuts.end());
				for (size_t Mip = 1; Mip < MipCuts.size(); ++Mip)
					for (size_t Layer = 1; Layer < LayerCuts.size(); ++Layer)
					{
						FGraphUse CellUse = *Uses.front();
						CellUse.TextureRange = {Aspect, MipCuts[Mip - 1],
							MipCuts[Mip] - MipCuts[Mip - 1], LayerCuts[Layer - 1],
							LayerCuts[Layer] - LayerCuts[Layer - 1]};
						if (!std::ranges::any_of(Uses,
							[&](const FGraphUse* Use) { return ContainsRange(*Use, CellUse); }))
							continue;
						FRangeState Cell;
						Cell.Use = CellUse;
						Cell.bProduced = Resource.bImported;
						Cell.Access = Resource.InitialAccess;
						Cells.push_back(std::move(Cell));
					}
			}
		}

		for (uint32 PassIndex = 0; PassIndex < PassCount; ++PassIndex)
			for (const auto& Use : State->Passes[PassIndex].Uses)
				for (auto& Cell : Cells)
				{
					if (!ContainsRange(Use, Cell.Use)) continue;
					const auto& Resource = State->Resources[Use.ResourceIndex];
					if (Use.Use != ERenderGraphUse::Write && !Use.bDiscard && !Cell.bProduced)
						return Fail(PassUsePrefix(State->Passes[PassIndex], Use)
							+ " reads resource '" + Resource.Name + "' before its producer");
					if (Use.Use == ERenderGraphUse::Read)
					{
						if (Cell.Producer != std::numeric_limits<uint32>::max())
							AddEdge(Cell.Producer, PassIndex, Resource.Name,
								ERenderGraphDependencyKind::Value);
						if (std::ranges::find(Cell.Readers, PassIndex) == Cell.Readers.end())
							Cell.Readers.push_back(PassIndex);
						continue;
					}
					if (Use.Use == ERenderGraphUse::ReadWrite && !Use.bDiscard
						&& Cell.Producer != std::numeric_limits<uint32>::max())
						AddEdge(Cell.Producer, PassIndex, Resource.Name,
							ERenderGraphDependencyKind::Value);
					for (uint32 Reader : Cell.Readers)
						AddEdge(Reader, PassIndex, Resource.Name,
							ERenderGraphDependencyKind::Execution);
					if (Cell.Readers.empty()
						&& Cell.Producer != std::numeric_limits<uint32>::max())
						AddEdge(Cell.Producer, PassIndex, Resource.Name,
							ERenderGraphDependencyKind::Execution);
					++Cell.Version;
					Cell.Producer = Use.bStore ? PassIndex : std::numeric_limits<uint32>::max();
					Cell.bProduced = Use.bStore;
					Cell.Readers.clear();
				}

		std::vector<uint32> Order;
		Order.reserve(PassCount);
		std::vector<bool> Emitted(PassCount, false);
		while (Order.size() < PassCount)
		{
			uint32 Selected = PassCount;
			for (uint32 Index = 0; Index < PassCount; ++Index)
				if (!Emitted[Index] && Indegree[Index] == 0) { Selected = Index; break; }
			if (Selected == PassCount) return Fail("graph contains a dependency cycle");
			Emitted[Selected] = true;
			Order.push_back(Selected);
			for (uint32 After : Outgoing[Selected]) --Indegree[After];
		}

		std::vector<bool> Retained(PassCount, !State->bEnableCulling);
		if (State->bEnableCulling)
		{
			std::vector<uint32> Pending;
			Pending.reserve(PassCount);
			for (uint32 Index = 0; Index < PassCount; ++Index)
				if (State->Passes[Index].bRoot) { Retained[Index] = true; Pending.push_back(Index); }
			while (!Pending.empty())
			{
				const uint32 After = Pending.back();
				Pending.pop_back();
				for (const auto& Edge : Dependencies)
					if (Edge.AfterPass == After
						&& Edge.Kind != ERenderGraphDependencyKind::Execution
						&& !Retained[Edge.BeforePass])
					{
						Retained[Edge.BeforePass] = true;
						Pending.push_back(Edge.BeforePass);
					}
			}
		}

		auto CompiledState = std::make_unique<FCompiledRenderGraph::FState>();
		CompiledState->Owner = State->Owner;
		CompiledState->Resources = State->Resources;
		CompiledState->Prepare = State->Prepare;
		CompiledState->Resolver = State->Resolver;
		CompiledState->Budget = State->Budget;
		CompiledState->Passes.reserve(PassCount);
		CompiledState->Dependencies.reserve(Dependencies.size());
		CompiledState->ResourceLifetimes.reserve(ResourceCount);
		CompiledState->CullingDecisions.reserve(PassCount);
		CompiledState->ExecuteCallbacks.reserve(PassCount);
		CompiledState->ParameterizedExecuteCallbacks.reserve(PassCount);
		CompiledState->PassParametersMetadata.reserve(PassCount);
		CompiledState->PassParameters.reserve(PassCount);
		CompiledState->PassResourceIndices.reserve(PassCount);
		CompiledState->PassBufferTransitionResources.reserve(PassCount);
		CompiledState->PassTextureTransitionResources.reserve(PassCount);
		CompiledState->ResourceCaptures.reserve(ResourceCount);
		CompiledState->UseCaptures.reserve(DeclaredUseCount);
		CompiledState->TransitionCaptures.reserve(DeclaredUseCount * 2);
		CompiledState->PreparationRequests.reserve(ResourceCount);
		for (const auto& Edge : Dependencies)
			if (Retained[Edge.BeforePass] && Retained[Edge.AfterPass])
				CompiledState->Dependencies.push_back(Edge);
		for (uint32 ResourceIndex = 0; ResourceIndex < State->Resources.size(); ++ResourceIndex)
		{
			const auto& Resource = State->Resources[ResourceIndex];
			CompiledState->ResourceLifetimes.push_back({Resource.Name,
				std::numeric_limits<uint32>::max(), 0, Resource.bImported, true});
			CompiledState->ResourceCaptures.push_back({ResourceIndex, Resource.Name,
				Resource.Kind, Resource.bImported, Resource.BackingClass, "unused"});
			auto& Capture = CompiledState->ResourceCaptures.back();
			Capture.TextureFormat = Resource.TextureDesc.Format;
			Capture.TextureExtent = Resource.TextureDesc.Extent;
			Capture.TextureArraySize = Resource.TextureDesc.ArraySize;
			Capture.TextureMips = Resource.TextureDesc.NumMips;
			Capture.BufferSize = Resource.BufferDesc.Size;
			Capture.BufferStride = Resource.BufferDesc.Stride;
		}
		for (uint32 Index = 0; Index < PassCount; ++Index)
			CompiledState->CullingDecisions.push_back({State->Passes[Index].Name,
				!Retained[Index], Retained[Index]
					? (State->Passes[Index].bRoot ? State->Passes[Index].RootReason : "value dependency")
					: "unreachable from an explicit root"});

		std::vector<FRangeState> ExecutionCells = Cells;
		for (auto& Cell : ExecutionCells)
		{
			const auto& Resource = State->Resources[Cell.Use.ResourceIndex];
			Cell.Access = Resource.InitialAccess;
			Cell.bProduced = Resource.bImported;
			Cell.Version = 0;
		}
		for (uint32 ScheduledIndex : Order)
		{
			if (!Retained[ScheduledIndex]) continue;
			const auto& Pass = State->Passes[ScheduledIndex];
			const uint32 CompiledPassIndex = static_cast<uint32>(CompiledState->Passes.size());
			FCompiledRenderGraphPass CompiledPass{.Name = Pass.Name, .Type = Pass.Type,
				.DeclarationIndex = ScheduledIndex};
			std::vector<uint32> DeclaredResources;
			std::vector<uint32> BufferTransitionResources;
			std::vector<uint32> TextureTransitionResources;
			DeclaredResources.reserve(Pass.Uses.size());
			BufferTransitionResources.reserve(Pass.Uses.size());
			TextureTransitionResources.reserve(Pass.Uses.size());
			CompiledPass.BufferTransitions.reserve(Pass.Uses.size());
			CompiledPass.TextureTransitions.reserve(Pass.Uses.size());
			for (const auto& Use : Pass.Uses)
			{
				if (std::ranges::find(DeclaredResources, Use.ResourceIndex) == DeclaredResources.end())
					DeclaredResources.push_back(Use.ResourceIndex);
				auto& Lifetime = CompiledState->ResourceLifetimes[Use.ResourceIndex];
				Lifetime.FirstPass = std::min(Lifetime.FirstPass, CompiledPassIndex);
				Lifetime.LastPass = CompiledPassIndex;
				Lifetime.bCulled = false;
				const auto& Resource = State->Resources[Use.ResourceIndex];
				for (auto& Cell : ExecutionCells)
				{
					if (!ContainsRange(Use, Cell.Use)) continue;
					const ERHIAccess Before = Use.bDiscard ? ERHIAccess::Discard : Cell.Access;
					if (Use.Kind != ERenderGraphResourceKind::Token
						&& !Use.bPassManagedTransition
						&& (Before != Use.Access || Use.bDiscard))
					{
						if (Use.Kind == ERenderGraphResourceKind::Texture)
						{
							CompiledPass.TextureTransitions.push_back({Resource.Texture,
								Cell.Use.TextureRange, Before, Use.Access});
							TextureTransitionResources.push_back(Use.ResourceIndex);
						}
						else
						{
							CompiledPass.BufferTransitions.push_back({Resource.Buffer,
								Cell.Use.BufferOffset, Cell.Use.BufferSize, Before, Use.Access});
							BufferTransitionResources.push_back(Use.ResourceIndex);
						}
						CompiledState->TransitionCaptures.push_back({Use.ResourceIndex,
							CompiledPassIndex, Before, Use.Access, Cell.Use.TextureRange,
							Cell.Use.BufferOffset, Cell.Use.BufferSize, false});
					}
					else if (Use.bPassManagedTransition)
					{
						if (!Use.bDiscard && Before != Use.Access)
						{
							if (Use.Kind == ERenderGraphResourceKind::Texture)
							{
								CompiledPass.TextureTransitions.push_back({Resource.Texture,
									Cell.Use.TextureRange, Before, Use.Access});
								TextureTransitionResources.push_back(Use.ResourceIndex);
							}
							else
							{
								CompiledPass.BufferTransitions.push_back({Resource.Buffer,
									Cell.Use.BufferOffset, Cell.Use.BufferSize, Before, Use.Access});
								BufferTransitionResources.push_back(Use.ResourceIndex);
							}
						}
						CompiledState->TransitionCaptures.push_back({Use.ResourceIndex,
							CompiledPassIndex, Before, Use.Access, Cell.Use.TextureRange,
							Cell.Use.BufferOffset, Cell.Use.BufferSize, false});
						CompiledState->TransitionCaptures.push_back({Use.ResourceIndex,
							CompiledPassIndex, Use.Access, Use.ResultAccess,
							Cell.Use.TextureRange, Cell.Use.BufferOffset,
							Cell.Use.BufferSize, false});
					}
					if (IsWriteUse(Use.Use)) ++Cell.Version;
					CompiledState->UseCaptures.push_back({ScheduledIndex, Use.ResourceIndex,
						Use.Use, Use.Access, Cell.Use.TextureRange, Cell.Use.BufferOffset,
						Cell.Use.BufferSize, Cell.Version, Use.bDiscard, Use.bStore,
						Use.ParameterPath});
					Cell.Access = Use.Kind == ERenderGraphResourceKind::Token
						? ERHIAccess::None : (Use.bStore
							? (Use.bPassManagedTransition ? Use.ResultAccess : Use.Access)
							: ERHIAccess::Discard);
					Cell.bProduced = Use.bStore && (Cell.bProduced || IsWriteUse(Use.Use));
				}
			}
			CompiledState->Passes.push_back(std::move(CompiledPass));
			CompiledState->ExecuteCallbacks.push_back(Pass.Execute);
			CompiledState->ParameterizedExecuteCallbacks.push_back(
				Pass.ParameterizedExecute);
			CompiledState->PassParametersMetadata.push_back(Pass.ParametersMetadata);
			CompiledState->PassParameters.push_back(Pass.Parameters);
			CompiledState->PassResourceIndices.push_back(std::move(DeclaredResources));
			CompiledState->PassBufferTransitionResources.push_back(
				std::move(BufferTransitionResources));
			CompiledState->PassTextureTransitionResources.push_back(
				std::move(TextureTransitionResources));
		}

		for (const auto& Cell : ExecutionCells)
		{
			const auto& Resource = State->Resources[Cell.Use.ResourceIndex];
			if (Cell.Use.Kind == ERenderGraphResourceKind::Token
				|| CompiledState->ResourceLifetimes[Cell.Use.ResourceIndex].bCulled
				|| Resource.FinalAccess == ERHIAccess::None
				|| Resource.FinalAccess == Cell.Access) continue;
			if (Cell.Use.Kind == ERenderGraphResourceKind::Texture)
			{
				CompiledState->FinalTextureTransitions.push_back({Resource.Texture,
					Cell.Use.TextureRange, Cell.Access, Resource.FinalAccess});
				CompiledState->FinalTextureTransitionResources.push_back(
					Cell.Use.ResourceIndex);
			}
			else
			{
				CompiledState->FinalBufferTransitions.push_back({Resource.Buffer,
					Cell.Use.BufferOffset, Cell.Use.BufferSize, Cell.Access, Resource.FinalAccess});
				CompiledState->FinalBufferTransitionResources.push_back(
					Cell.Use.ResourceIndex);
			}
			CompiledState->TransitionCaptures.push_back({Cell.Use.ResourceIndex,
				std::numeric_limits<uint32>::max(), Cell.Access, Resource.FinalAccess,
				Cell.Use.TextureRange, Cell.Use.BufferOffset, Cell.Use.BufferSize, true});
		}

		for (uint32 ResourceIndex = 0; ResourceIndex < State->Resources.size(); ++ResourceIndex)
		{
			const auto& Resource = State->Resources[ResourceIndex];
			const auto& Lifetime = CompiledState->ResourceLifetimes[ResourceIndex];
			auto& Capture = CompiledState->ResourceCaptures[ResourceIndex];
			if (Lifetime.bCulled) Capture.Preparation = "culled";
			else if (Resource.bImported) Capture.Preparation = "imported";
			else if (!Resource.bRequiresBacking) Capture.Preparation = "prebound";
			else
			{
				Capture.Preparation = "requested";
				FRenderGraphPreparationRequest Request;
				Request.ResourceId = ResourceIndex;
				Request.Name = Resource.Name;
				Request.Kind = Resource.Kind;
				Request.Texture = {State->Owner, ResourceIndex};
				Request.Buffer = {State->Owner, ResourceIndex};
				Request.TextureDesc = Resource.TextureDesc;
				Request.BufferDesc = Resource.BufferDesc;
				Request.BackingClass = Resource.BackingClass;
				Request.FirstPass = Lifetime.FirstPass;
				Request.LastPass = Lifetime.LastPass;
				CompiledState->PreparationRequests.push_back(std::move(Request));
			}
		}
		std::ranges::sort(CompiledState->Dependencies,
			[](const auto& A, const auto& B) {
				return std::tie(A.BeforePass, A.AfterPass, A.Cause)
					< std::tie(B.BeforePass, B.AfterPass, B.Cause);
			});
		CompiledState->CompileMicroseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - Started).count());
		uint32 BufferTransitionCount = static_cast<uint32>(
			CompiledState->FinalBufferTransitions.size());
		uint32 TextureTransitionCount = static_cast<uint32>(
			CompiledState->FinalTextureTransitions.size());
		for (const auto& Pass : CompiledState->Passes)
		{
			BufferTransitionCount += static_cast<uint32>(Pass.BufferTransitions.size());
			TextureTransitionCount += static_cast<uint32>(Pass.TextureTransitions.size());
		}
		auto CheckLimit = [&](std::string_view Name, uint32 Actual, uint32 Limit)
			-> FRenderGraphCompileResult {
			return Fail("render graph safety limit exceeded: " + std::string(Name)
				+ " actual=" + std::to_string(Actual) + " limit="
				+ std::to_string(Limit));
		};
		if (CompiledState->Dependencies.size() > State->Budget.MaxDependencies)
			return CheckLimit("dependencies",
				static_cast<uint32>(CompiledState->Dependencies.size()),
				State->Budget.MaxDependencies);
		if (BufferTransitionCount > State->Budget.MaxBufferTransitions)
			return CheckLimit("buffer-transitions", BufferTransitionCount,
				State->Budget.MaxBufferTransitions);
		if (TextureTransitionCount > State->Budget.MaxTextureTransitions)
			return CheckLimit("texture-transitions", TextureTransitionCount,
				State->Budget.MaxTextureTransitions);
		if (!State->ParameterStorage.Allocations.empty())
		{
			CompiledState->ParameterStorage = std::move(State->ParameterStorage);
			State->bParameterStorageTransferred = true;
		}
		return {std::unique_ptr<FCompiledRenderGraph>(
			new FCompiledRenderGraph(std::move(CompiledState))), {}};
	}

	FCompiledRenderGraph::FCompiledRenderGraph(std::unique_ptr<FState> InState)
		: State(std::move(InState))
	{
	}

	FCompiledRenderGraph::FCompiledRenderGraph(FCompiledRenderGraph&&) noexcept = default;
	auto FCompiledRenderGraph::operator=(FCompiledRenderGraph&&) noexcept
		-> FCompiledRenderGraph& = default;
	FCompiledRenderGraph::~FCompiledRenderGraph() = default;

	auto FCompiledRenderGraph::GetPasses() const
		-> std::span<const FCompiledRenderGraphPass> { return State->Passes; }
	auto FCompiledRenderGraph::GetDependencies() const
		-> std::span<const FRenderGraphDependency> { return State->Dependencies; }
	auto FCompiledRenderGraph::GetResourceLifetimes() const
		-> std::span<const FRenderGraphResourceLifetime>
	{
		return State->ResourceLifetimes;
	}
	auto FCompiledRenderGraph::GetCullingDecisions() const
		-> std::span<const FRenderGraphCullingDecision>
	{
		return State->CullingDecisions;
	}
	auto FCompiledRenderGraph::GetFinalBufferTransitions() const
		-> std::span<const FRHIBufferTransition> { return State->FinalBufferTransitions; }
	auto FCompiledRenderGraph::GetFinalTextureTransitions() const
		-> std::span<const FRHITextureTransition> { return State->FinalTextureTransitions; }
	auto FCompiledRenderGraph::GetCompileMicroseconds() const -> uint64
	{
		return State->CompileMicroseconds;
	}

	auto FCompiledRenderGraph::GetBudget() const -> const FRenderGraphBudget&
	{
		return State->Budget;
	}

	auto FCompiledRenderGraph::GetStatistics() const -> FRenderGraphStatistics
	{
		FRenderGraphStatistics Result;
		Result.DeclaredPasses = static_cast<uint32>(State->CullingDecisions.size());
		Result.ScheduledPasses = static_cast<uint32>(State->Passes.size());
		Result.CulledPasses = Result.DeclaredPasses - Result.ScheduledPasses;
		Result.Dependencies = static_cast<uint32>(State->Dependencies.size());
		Result.BufferTransitions = static_cast<uint32>(
			State->FinalBufferTransitions.size());
		Result.TextureTransitions = static_cast<uint32>(
			State->FinalTextureTransitions.size());
		for (const auto& Pass : State->Passes)
		{
			Result.BufferTransitions += static_cast<uint32>(Pass.BufferTransitions.size());
			Result.TextureTransitions += static_cast<uint32>(Pass.TextureTransitions.size());
		}
		Result.CompileMicroseconds = State->CompileMicroseconds;
		Result.ExecuteMicroseconds =
			State->ExecuteMicroseconds.load(std::memory_order_relaxed);
		Result.bPassRegressionBudgetExceeded = Result.DeclaredPasses
			> State->Budget.RegressionMaxPasses;
		Result.bDependencyRegressionBudgetExceeded = Result.Dependencies
			> State->Budget.RegressionMaxDependencies;
		Result.bBufferTransitionRegressionBudgetExceeded = Result.BufferTransitions
			> State->Budget.RegressionMaxBufferTransitions;
		Result.bTextureTransitionRegressionBudgetExceeded = Result.TextureTransitions
			> State->Budget.RegressionMaxTextureTransitions;
		Result.bCompileBudgetExceeded = Result.CompileMicroseconds
			> State->Budget.MaxCompileMicroseconds;
		Result.bExecuteBudgetExceeded = Result.ExecuteMicroseconds
			> State->Budget.MaxExecuteMicroseconds;
		return Result;
	}

	auto FCompiledRenderGraph::Capture() const -> FRenderGraphCapture
	{
		FRenderGraphCapture Result;
		Result.Budget = State->Budget;
		Result.Statistics = GetStatistics();
		Result.Resources = State->ResourceCaptures;
		Result.Uses = State->UseCaptures;
		Result.Transitions = State->TransitionCaptures;
		Result.Dependencies = State->Dependencies;
		Result.ResourceLifetimes = State->ResourceLifetimes;
		Result.CullingDecisions = State->CullingDecisions;
		Result.Dump = Dump();
		Result.Passes.reserve(State->Passes.size());
		for (const auto& Pass : State->Passes)
			Result.Passes.push_back({Pass.Name, Pass.Type, Pass.DeclarationIndex,
				static_cast<uint32>(Pass.BufferTransitions.size()),
				static_cast<uint32>(Pass.TextureTransitions.size())});
		return Result;
	}

	auto FCompiledRenderGraph::Dump() const -> std::string
	{
		std::ostringstream Output;
		Output << "render-graph passes=" << State->Passes.size()
			<< " edges=" << State->Dependencies.size() << '\n';
		for (uint32 Index = 0; Index < State->Passes.size(); ++Index)
		{
			const auto& Pass = State->Passes[Index];
			Output << "pass " << Index << " decl=" << Pass.DeclarationIndex
				<< " type=" << PassTypeName(Pass.Type) << " name=" << Pass.Name
				<< " buffers=" << Pass.BufferTransitions.size()
				<< " textures=" << Pass.TextureTransitions.size() << '\n';
		}
		for (const auto& Edge : State->Dependencies)
			Output << "edge " << Edge.BeforePass << "->" << Edge.AfterPass
				<< " kind=" << DependencyKindName(Edge.Kind)
				<< " cause=" << Edge.Cause << '\n';
		Output << "final buffers=" << State->FinalBufferTransitions.size()
			<< " textures=" << State->FinalTextureTransitions.size() << '\n';
		for (const auto& Lifetime : State->ResourceLifetimes)
			Output << "lifetime name=" << Lifetime.Name << " first="
				<< Lifetime.FirstPass << " last=" << Lifetime.LastPass
				<< " imported=" << Lifetime.bImported
				<< " culled=" << Lifetime.bCulled << '\n';
		for (const auto& Decision : State->CullingDecisions)
			Output << "culling name=" << Decision.Name << " culled="
				<< Decision.bCulled << " reason=" << Decision.Reason << '\n';
		for (const auto& Resource : State->ResourceCaptures)
			Output << "resource id=" << Resource.ResourceId << " name="
				<< Resource.Name << " kind=" << static_cast<uint32>(Resource.Kind)
				<< " imported=" << Resource.bImported << " backing="
				<< Resource.BackingClass << " preparation="
				<< Resource.Preparation << " format="
				<< static_cast<uint32>(Resource.TextureFormat) << " extent="
				<< Resource.TextureExtent.x << 'x' << Resource.TextureExtent.y
				<< " layers=" << Resource.TextureArraySize << " mips="
				<< static_cast<uint32>(Resource.TextureMips) << " buffer-size="
				<< Resource.BufferSize << " stride=" << Resource.BufferStride << '\n';
		for (const auto& Use : State->UseCaptures)
		{
			Output << "use pass=" << Use.PassDeclarationIndex << " resource="
				<< Use.ResourceId << " version=" << Use.Version << " access="
				<< static_cast<uint32>(Use.Access) << " aspects="
				<< static_cast<uint32>(Use.TextureRange.Aspects) << " mip="
				<< Use.TextureRange.FirstMip << '+' << Use.TextureRange.NumMips
				<< " layer=" << Use.TextureRange.FirstArrayLayer << '+'
				<< Use.TextureRange.NumArrayLayers << " offset=" << Use.BufferOffset
				<< " size=" << Use.BufferSize << " discard=" << Use.bDiscard
				<< " store=" << Use.bStore;
			if (!Use.ParameterPath.empty())
				Output << " field=" << Use.ParameterPath;
			Output << '\n';
		}
		for (const auto& Transition : State->TransitionCaptures)
			Output << "transition resource=" << Transition.ResourceId << " pass="
				<< Transition.PassIndex << " before="
				<< static_cast<uint32>(Transition.Before) << " after="
				<< static_cast<uint32>(Transition.After) << " aspects="
				<< static_cast<uint32>(Transition.TextureRange.Aspects) << " mip="
				<< Transition.TextureRange.FirstMip << '+'
				<< Transition.TextureRange.NumMips << " layer="
				<< Transition.TextureRange.FirstArrayLayer << '+'
				<< Transition.TextureRange.NumArrayLayers << " offset="
				<< Transition.BufferOffset << " size=" << Transition.BufferSize
				<< " final=" << Transition.bFinal << '\n';
		return Output.str();
	}

	auto FCompiledRenderGraph::Execute(FRHICommandListImmediate& CommandList,
		std::string* OutError) const -> bool
	{
		const auto Started = std::chrono::steady_clock::now();
		auto RecordDuration = [&] {
			State->ExecuteMicroseconds.store(static_cast<uint64>(
				std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - Started).count()),
				std::memory_order_relaxed);
		};
		if (State->Prepare)
		{
			std::string Error;
			if (!State->Prepare(Error))
			{
				if (OutError != nullptr) *OutError = std::move(Error);
				RecordDuration();
				return false;
			}
		}
		if (State->Resolver && !State->PreparationRequests.empty())
		{
			FRenderGraphResourceBackings Candidate(State->Owner,
				static_cast<uint32>(State->Resources.size()));
			std::string Error;
			if (!State->Resolver(State->PreparationRequests, Candidate, Error))
			{
				if (OutError != nullptr) *OutError = std::move(Error);
				RecordDuration();
				return false;
			}
			for (const auto& Request : State->PreparationRequests)
			{
				const bool bReady = Request.Kind == ERenderGraphResourceKind::Texture
					? Candidate.Textures[Request.ResourceId] != nullptr
					: Candidate.Buffers[Request.ResourceId] != nullptr;
				if (!bReady)
				{
					if (OutError != nullptr)
						*OutError = "backing resolver omitted retained resource '"
							+ Request.Name + "'";
					RecordDuration();
					return false;
				}
				if (Request.Kind == ERenderGraphResourceKind::Texture)
				{
					const FRHITextureDesc Actual = DescribeTexture(
						*Candidate.Textures[Request.ResourceId]);
					if (Actual.Dimension != Request.TextureDesc.Dimension
						|| Actual.Extent != Request.TextureDesc.Extent
						|| Actual.Depth != Request.TextureDesc.Depth
						|| Actual.ArraySize != Request.TextureDesc.ArraySize
						|| Actual.NumMips != Request.TextureDesc.NumMips
						|| Actual.NumSamples != Request.TextureDesc.NumSamples
						|| Actual.Format != Request.TextureDesc.Format)
					{
						if (OutError != nullptr) *OutError =
							"backing resolver returned incompatible texture '"
							+ Request.Name + "'";
						RecordDuration();
						return false;
					}
				}
				else
				{
					const auto& Actual = Candidate.Buffers[Request.ResourceId]->GetDesc();
					if (Actual.Size != Request.BufferDesc.Size
						|| Actual.Stride != Request.BufferDesc.Stride)
					{
						if (OutError != nullptr) *OutError =
							"backing resolver returned incompatible buffer '"
							+ Request.Name + "'";
						RecordDuration();
						return false;
					}
				}
			}
			for (const auto& Request : State->PreparationRequests)
			{
				auto& Resource = State->Resources[Request.ResourceId];
				if (Request.Kind == ERenderGraphResourceKind::Texture)
					Resource.Texture = Candidate.Textures[Request.ResourceId];
				else Resource.Buffer = Candidate.Buffers[Request.ResourceId];
			}
		}
		for (uint32 Index = 0; Index < State->Passes.size(); ++Index)
		{
			auto& Pass = State->Passes[Index];
			for (uint32 TransitionIndex = 0;
				TransitionIndex < Pass.BufferTransitions.size(); ++TransitionIndex)
				Pass.BufferTransitions[TransitionIndex].Buffer = State->Resources[
					State->PassBufferTransitionResources[Index][TransitionIndex]].Buffer;
			for (uint32 TransitionIndex = 0;
				TransitionIndex < Pass.TextureTransitions.size(); ++TransitionIndex)
				Pass.TextureTransitions[TransitionIndex].Texture = State->Resources[
					State->PassTextureTransitionResources[Index][TransitionIndex]].Texture;
			if (!Pass.BufferTransitions.empty())
				CommandList.TransitionBuffers(Pass.BufferTransitions);
			if (!Pass.TextureTransitions.empty())
				CommandList.TransitionTextures(Pass.TextureTransitions);
			if (State->ParameterizedExecuteCallbacks[Index])
			{
				const FRenderGraphPassResources Resources(*this, Index);
				const FRenderGraphParameterResolver Resolver(Resources,
					State->PassParametersMetadata[Index],
					State->PassParameters[Index]);
				State->ParameterizedExecuteCallbacks[Index](CommandList, Resolver);
			}
			else if (State->ExecuteCallbacks[Index])
			{
				const FRenderGraphPassResources Resources(*this, Index);
				State->ExecuteCallbacks[Index](CommandList, Resources);
			}
		}
		for (uint32 Index = 0; Index < State->FinalBufferTransitions.size(); ++Index)
			State->FinalBufferTransitions[Index].Buffer = State->Resources[
				State->FinalBufferTransitionResources[Index]].Buffer;
		for (uint32 Index = 0; Index < State->FinalTextureTransitions.size(); ++Index)
			State->FinalTextureTransitions[Index].Texture = State->Resources[
				State->FinalTextureTransitionResources[Index]].Texture;
		if (!State->FinalBufferTransitions.empty())
			CommandList.TransitionBuffers(State->FinalBufferTransitions);
		if (!State->FinalTextureTransitions.empty())
			CommandList.TransitionTextures(State->FinalTextureTransitions);
		if (OutError != nullptr) OutError->clear();
		RecordDuration();
		return true;
	}

	auto FRenderGraphPassResources::GetTexture(
		FRenderGraphTextureHandle Handle) const -> FRHITexture*
	{
		requiref(Handle.Owner == Graph.State->Owner
			&& Handle.Index < Graph.State->Resources.size(),
			"Render graph callback used an invalid texture handle.");
		requiref(PassIndex < Graph.State->PassResourceIndices.size()
			&& std::ranges::find(Graph.State->PassResourceIndices[PassIndex],
				Handle.Index) != Graph.State->PassResourceIndices[PassIndex].end(),
			"Render graph pass '{}' accessed undeclared texture resource {} ('{}').",
			PassIndex < Graph.State->Passes.size()
				? Graph.State->Passes[PassIndex].Name : "<invalid>", Handle.Index,
			Handle.Index < Graph.State->Resources.size()
				? Graph.State->Resources[Handle.Index].Name : "<invalid>");
		const auto& Resource = Graph.State->Resources[Handle.Index];
		requiref(Resource.Kind == ERenderGraphResourceKind::Texture
			&& Resource.Texture != nullptr,
			"Render graph callback resolved an unavailable texture.");
		return Resource.Texture;
	}

	auto FRenderGraphPassResources::GetBuffer(
		FRenderGraphBufferHandle Handle) const -> FRHIBuffer*
	{
		requiref(Handle.Owner == Graph.State->Owner
			&& Handle.Index < Graph.State->Resources.size(),
			"Render graph callback used an invalid buffer handle.");
		requiref(PassIndex < Graph.State->PassResourceIndices.size()
			&& std::ranges::find(Graph.State->PassResourceIndices[PassIndex],
				Handle.Index) != Graph.State->PassResourceIndices[PassIndex].end(),
			"Render graph pass '{}' accessed undeclared buffer resource {} ('{}').",
			PassIndex < Graph.State->Passes.size()
				? Graph.State->Passes[PassIndex].Name : "<invalid>", Handle.Index,
			Handle.Index < Graph.State->Resources.size()
				? Graph.State->Resources[Handle.Index].Name : "<invalid>");
		const auto& Resource = Graph.State->Resources[Handle.Index];
		requiref(Resource.Kind == ERenderGraphResourceKind::Buffer
			&& Resource.Buffer != nullptr,
			"Render graph callback resolved an unavailable buffer.");
		return Resource.Buffer;
	}

	auto FRenderGraphParameterResolver::FindMember(const void* Address,
		ERenderGraphParameterMemberKind ExpectedKind,
		ERenderGraphParameterMemberKind AlternateKind, bool bOptional) const
		-> const FRenderGraphParameterMemberMetadata&
	{
		const FRenderGraphParameterMemberMetadata* Found = nullptr;
		std::function<void(const void*, const FRenderGraphParametersMetadata*)>
			Traverse;
		Traverse = [&](const void* StructData,
			const FRenderGraphParametersMetadata* StructMetadata) {
			if (Found != nullptr || StructData == nullptr || StructMetadata == nullptr)
				return;
			const auto* Bytes = static_cast<const std::byte*>(StructData);
			for (const auto& Member : StructMetadata->Members)
			{
				for (uint32 ElementIndex = 0;
					ElementIndex < Member.ArraySize; ++ElementIndex)
				{
					const void* ElementData = Bytes + Member.Offset
						+ static_cast<size_t>(ElementIndex) * Member.ElementSize;
					if (Member.Kind == ERenderGraphParameterMemberKind::Nested)
					{
						Traverse(ElementData, Member.NestedParameters);
						continue;
					}
					if (Member.Kind != ExpectedKind && Member.Kind != AlternateKind)
						continue;

					const void* ValueAddress = ElementData;
					if (Member.bOptional)
					{
						auto ReadOptionalAddress = [&]<typename Wrapper>() {
							const auto& Optional = *static_cast<
								const std::optional<Wrapper>*>(ElementData);
							ValueAddress = Optional ? static_cast<const void*>(&*Optional)
								: nullptr;
						};
						switch (Member.Kind)
						{
						case ERenderGraphParameterMemberKind::Texture:
							ReadOptionalAddress.template operator()<
								FRenderGraphTextureParameter>();
							break;
						case ERenderGraphParameterMemberKind::Buffer:
							ReadOptionalAddress.template operator()<
								FRenderGraphBufferParameter>();
							break;
						case ERenderGraphParameterMemberKind::ColorAttachment:
						case ERenderGraphParameterMemberKind::ManagedColorAttachment:
							ReadOptionalAddress.template operator()<
								FRenderGraphColorAttachmentParameter>();
							break;
						case ERenderGraphParameterMemberKind::DepthStencilAttachment:
						case ERenderGraphParameterMemberKind::ManagedDepthStencilAttachment:
							ReadOptionalAddress.template operator()<
								FRenderGraphDepthStencilAttachmentParameter>();
							break;
						case ERenderGraphParameterMemberKind::ManagedTexture:
							ReadOptionalAddress.template operator()<
								FRenderGraphManagedTextureParameter>();
							break;
						case ERenderGraphParameterMemberKind::Token:
						case ERenderGraphParameterMemberKind::Nested: break;
						}
					}
					const bool bAddressMatches = bOptional
						? Member.bOptional && Address == ElementData
						: Address == ValueAddress;
					if (bAddressMatches)
					{
						Found = &Member;
						return;
					}
				}
			}
		};
		Traverse(Parameters, Metadata);
		requiref(Found != nullptr,
			"Render graph parameter resolver accessed a member that is not declared "
			"by the executing pass parameters.");
		return *Found;
	}

	auto FRenderGraphParameterResolver::GetTexture(
		const FRenderGraphTextureParameter& Parameter) const -> FRHITexture*
	{
		FindMember(&Parameter, ERenderGraphParameterMemberKind::Texture,
			ERenderGraphParameterMemberKind::Texture, false);
		return Resources.GetTexture(Parameter.Texture);
	}

	auto FRenderGraphParameterResolver::GetTexture(const std::optional<
		FRenderGraphTextureParameter>& Parameter) const -> FRHITexture*
	{
		FindMember(&Parameter, ERenderGraphParameterMemberKind::Texture,
			ERenderGraphParameterMemberKind::Texture, true);
		return Parameter ? Resources.GetTexture(Parameter->Texture) : nullptr;
	}

	auto FRenderGraphParameterResolver::GetTexture(
		const FRenderGraphManagedTextureParameter& Parameter) const -> FRHITexture*
	{
		FindMember(&Parameter, ERenderGraphParameterMemberKind::ManagedTexture,
			ERenderGraphParameterMemberKind::ManagedTexture, false);
		return Resources.GetTexture(Parameter.Texture);
	}

	auto FRenderGraphParameterResolver::GetTexture(const std::optional<
		FRenderGraphManagedTextureParameter>& Parameter) const -> FRHITexture*
	{
		FindMember(&Parameter, ERenderGraphParameterMemberKind::ManagedTexture,
			ERenderGraphParameterMemberKind::ManagedTexture, true);
		return Parameter ? Resources.GetTexture(Parameter->Texture) : nullptr;
	}

	auto FRenderGraphParameterResolver::GetBuffer(
		const FRenderGraphBufferParameter& Parameter) const -> FRHIBuffer*
	{
		FindMember(&Parameter, ERenderGraphParameterMemberKind::Buffer,
			ERenderGraphParameterMemberKind::Buffer, false);
		return Resources.GetBuffer(Parameter.Buffer);
	}

	auto FRenderGraphParameterResolver::GetBuffer(const std::optional<
		FRenderGraphBufferParameter>& Parameter) const -> FRHIBuffer*
	{
		FindMember(&Parameter, ERenderGraphParameterMemberKind::Buffer,
			ERenderGraphParameterMemberKind::Buffer, true);
		return Parameter ? Resources.GetBuffer(Parameter->Buffer) : nullptr;
	}

	namespace
	{
		auto MakeAttachmentView(const FRenderGraphPassResources& Resources,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			const FRenderGraphParameterMemberMetadata& Member)
			-> FRenderGraphAttachmentView
		{
			return {Resources.GetTexture(Texture), Range, Member.LoadAction,
				Member.StoreAction, Member.bPassManagedTransition,
				Member.ResultAccess};
		}
	}

	auto FRenderGraphParameterResolver::GetColorAttachment(
		const FRenderGraphColorAttachmentParameter& Parameter) const
		-> FRenderGraphAttachmentView
	{
		const auto& Member = FindMember(&Parameter,
			ERenderGraphParameterMemberKind::ColorAttachment,
			ERenderGraphParameterMemberKind::ManagedColorAttachment, false);
		return MakeAttachmentView(Resources, Parameter.Texture, Parameter.Range,
			Member);
	}

	auto FRenderGraphParameterResolver::GetColorAttachment(const std::optional<
		FRenderGraphColorAttachmentParameter>& Parameter) const
		-> FRenderGraphAttachmentView
	{
		const auto& Member = FindMember(&Parameter,
			ERenderGraphParameterMemberKind::ColorAttachment,
			ERenderGraphParameterMemberKind::ManagedColorAttachment, true);
		return Parameter ? MakeAttachmentView(Resources, Parameter->Texture,
			Parameter->Range, Member) : FRenderGraphAttachmentView{};
	}

	auto FRenderGraphParameterResolver::GetDepthStencilAttachment(
		const FRenderGraphDepthStencilAttachmentParameter& Parameter) const
		-> FRenderGraphAttachmentView
	{
		const auto& Member = FindMember(&Parameter,
			ERenderGraphParameterMemberKind::DepthStencilAttachment,
			ERenderGraphParameterMemberKind::ManagedDepthStencilAttachment, false);
		return MakeAttachmentView(Resources, Parameter.Texture, Parameter.Range,
			Member);
	}

	auto FRenderGraphParameterResolver::GetDepthStencilAttachment(
		const std::optional<FRenderGraphDepthStencilAttachmentParameter>& Parameter)
		const -> FRenderGraphAttachmentView
	{
		const auto& Member = FindMember(&Parameter,
			ERenderGraphParameterMemberKind::DepthStencilAttachment,
			ERenderGraphParameterMemberKind::ManagedDepthStencilAttachment, true);
		return Parameter ? MakeAttachmentView(Resources, Parameter->Texture,
			Parameter->Range, Member) : FRenderGraphAttachmentView{};
	}

	FRenderGraphResourceBackings::FRenderGraphResourceBackings(
		uint64 InOwner, uint32 Count)
		: Owner(InOwner), Textures(Count), Buffers(Count)
	{
	}

	auto FRenderGraphResourceBackings::SetTexture(
		FRenderGraphTextureHandle Handle, FRHITexture* Texture) -> bool
	{
		if (Handle.Owner != Owner || Handle.Index >= Textures.size()
			|| Texture == nullptr) return false;
		Textures[Handle.Index] = Texture;
		return true;
	}

	auto FRenderGraphResourceBackings::SetBuffer(
		FRenderGraphBufferHandle Handle, FRHIBuffer* Buffer) -> bool
	{
		if (Handle.Owner != Owner || Handle.Index >= Buffers.size()
			|| Buffer == nullptr) return false;
		Buffers[Handle.Index] = Buffer;
		return true;
	}
} // namespace Durin
