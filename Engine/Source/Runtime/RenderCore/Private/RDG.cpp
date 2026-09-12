#include "RDG.h"
#include "Misc/Time.h"

#include "RHICommandList.h"
#include "DynamicRHI.h"
#include "RHIGlobals.h"
#include "RHIQueueTransfer.h"

#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	namespace
	{
		struct FGraphResource
		{
			std::string Name;
			ERDGResourceKind Kind = ERDGResourceKind::Texture;
			FTextureRHIRef Texture;
			FBufferRHIRef Buffer;
			FRHITextureDesc TextureDesc;
			FRHIBufferDesc BufferDesc;
			uint32 ObservationTag = 0;
			ERHIAccess InitialAccess = ERHIAccess::Discard;
			ERHIAccess FinalAccess = ERHIAccess::None;
			bool bExternal = false;
			// Caller-owned publication slots are written only after successful execution.
			FTextureRHIRef* TextureDestination = nullptr;
			FBufferRHIRef* BufferDestination = nullptr;

			const void* ValueTypeIdentity = nullptr;
			std::string ValueTypeName;
			uint32 ValueStorageIndex = std::numeric_limits<uint32>::max();

			auto IsExported() const -> bool
			{
				return TextureDestination != nullptr || BufferDestination != nullptr;
			}

			auto HasInitialContents() const -> bool
			{
				return bExternal && InitialAccess != ERHIAccess::None
					&& !EnumHasAnyFlags(InitialAccess, ERHIAccess::Discard);
			}
		};

		// Physical backing is execution state; declarations stay frozen after Building.
		struct FGraphResourceBacking final
		{
			FTextureRHIRef Texture;
			FBufferRHIRef Buffer;
			uint64 PhysicalAllocationId = 0;
			std::string AllocationDisposition;
		};

		// Execution-local physical data; never stored in the immutable logical plan.
		struct FPreparedTransitions final
		{
			std::vector<FRHIBufferTransition> Buffers;
			std::vector<FRHITextureTransition> Textures;
			std::vector<bool> TransferredBuffers, TransferredTextures;
		};
		struct FPreparedBarrierBatch final
		{
			size_t FirstBuffer = 0, NumBuffers = 0;
			size_t FirstTexture = 0, NumTextures = 0;
		};

		// Resolve every barrier before recording callbacks or emitting any graph work.
		auto PrepareBarrierBatch(
			const FRDGBarrierBatch& Batch, std::span<const FGraphResourceBacking> Backings,
			FPreparedTransitions& Scratch) -> FPreparedBarrierBatch
		{
			const FPreparedBarrierBatch Result{Scratch.Buffers.size(), Batch.GetBufferTransitions().size(),
				Scratch.Textures.size(), Batch.GetTextureTransitions().size()};
			for (const auto& Transition : Batch.GetBufferTransitions())
			{
				check(Transition.ResourceId < Backings.size());
				Scratch.Buffers.push_back({Backings[Transition.ResourceId].Buffer.GetReference(),
					Transition.Offset, Transition.Size, Transition.ExpectedBefore,
					Transition.RequiredAfter, Transition.bDiscardContents});
			}
			for (const auto& Transition : Batch.GetTextureTransitions())
			{
				check(Transition.ResourceId < Backings.size());
				Scratch.Textures.push_back({Backings[Transition.ResourceId].Texture.GetReference(),
					Transition.Range, Transition.ExpectedBefore,
					Transition.RequiredAfter, Transition.bDiscardContents});
			}
			return Result;
		}

		auto RecordBarrierBatch(FRHICommandListImmediate& CommandList,
			const FPreparedBarrierBatch& Batch, const FPreparedTransitions& Prepared) -> void
		{
			auto Record = [](size_t First, size_t Count, const auto& Transferred, auto&& Emit) {
				const size_t End = First + Count;
				while (First < End)
				{
					while (First < End && !Transferred.empty() && Transferred[First]) ++First;
					const size_t Begin = First;
					while (First < End && (Transferred.empty() || !Transferred[First])) ++First;
					if (First != Begin) Emit(Begin, First - Begin);
				}
			};
			Record(Batch.FirstBuffer, Batch.NumBuffers, Prepared.TransferredBuffers,
				[&](size_t First, size_t Count) { CommandList.TransitionBuffers(std::span{Prepared.Buffers}.subspan(First, Count)); });
			Record(Batch.FirstTexture, Batch.NumTextures, Prepared.TransferredTextures,
				[&](size_t First, size_t Count) { CommandList.TransitionTextures(std::span{Prepared.Textures}.subspan(First, Count)); });
		}

		struct FGraphUse
		{
			uint32 ResourceIndex = 0;
			ERDGResourceKind Kind = ERDGResourceKind::Texture;
			ERDGUse Use = ERDGUse::Read;
			ERHIAccess Access = ERHIAccess::None;
			bool bDiscard = false;
			FRHITextureSubresourceRange TextureRange{};
			uint64 BufferOffset = 0;
			uint64 BufferSize = 0;
			bool bStore = true;
			bool bPassManagedTransition = false;
			ERHIAccess ResultAccess = ERHIAccess::None;
			std::string_view ParameterPath;
			std::string_view ShaderBindingName;
			ERHIBindingType ShaderBindingType = ERHIBindingType::Texture;
		};

		using FOptionalAlias = std::pair<uint32, uint32>;
		static_assert(sizeof(FOptionalAlias) == 8);

		struct FOptionalAliasTable final
		{
			FOptionalAliasTable() = default;
			explicit FOptionalAliasTable(std::span<const FOptionalAlias> Aliases)
				: Data(Aliases.empty()
					? nullptr : std::make_unique<FOptionalAlias[]>(Aliases.size())),
				  Count(static_cast<uint32>(Aliases.size()))
			{
				std::ranges::copy(Aliases, Data.get());
			}
			FOptionalAliasTable(const FOptionalAliasTable&) = delete;
			auto operator=(const FOptionalAliasTable&)
				-> FOptionalAliasTable& = delete;
			FOptionalAliasTable(FOptionalAliasTable&&) noexcept = default;
			auto operator=(FOptionalAliasTable&&) noexcept
				-> FOptionalAliasTable& = default;

			auto View() const -> std::span<const FOptionalAlias>
			{
				return {Data.get(), Count};
			}

			std::unique_ptr<FOptionalAlias[]> Data;
			uint32 Count = 0;
		};
		static_assert(sizeof(FOptionalAliasTable) == 16);

		struct FGraphPass
		{
			std::string Name;
			ERDGPassType Type = ERDGPassType::Graphics;
			bool bAsyncComputeEligible = false;
			std::vector<FGraphUse> Uses;
			std::vector<uint32> Prerequisites;
			FRDGParameterizedPassExecute ParameterizedExecute;
			bool bRoot = false;
			// Terminal exports consume contents but may hand off a writable access state.
			bool bExport = false;
			std::string RootReason;
			// Only frozen uses may retain validation across later graph declarations.
			bool bDeclarationsValidated = false;
			const FRDGParameterLayout* ParameterLayout = nullptr;
			const void* Parameters = nullptr;
			FOptionalAliasTable OptionalAliases;
		};

		// Borrows frozen builder declarations and a separately owned terminal export.
		// Neither owner may mutate its passes while compiler views are in use.
		struct FGraphPassView final
		{
			std::span<const FGraphPass> Declarations;
			const FGraphPass* Export = nullptr;

			auto size() const -> size_t
			{ return Declarations.size() + (Export != nullptr ? 1 : 0); }

			auto operator[](size_t Index) const -> const FGraphPass&
			{ return Index < Declarations.size() ? Declarations[Index] : *Export; }
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
			const FRDGParameterLayout* Layout = nullptr;
			bool bConstructed = false;
			bool bFrozen = false;
		};

		struct FGraphParameterStorage final
		{
			FGraphParameterStorage() = default;
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

		// Immutable geometry shared by analysis and execution-plan traversal.
		struct FRangeCell final
		{
			ERDGResourceKind Kind;
			uint32 ResourceIndex;
			FRHITextureSubresourceRange TextureRange;
			uint64 BufferOffset;
			uint64 BufferSize;
		};

		// Exists only while constructing dependency edges in declaration order.
		struct FDependencyCellState final
		{
			bool bProduced = false;
			uint32 Producer = std::numeric_limits<uint32>::max();
			std::vector<uint32> Readers;
		};

		// Fresh access history for each traversal of retained passes.
		struct FBarrierCellState final
		{
			ERHIAccess Access = ERHIAccess::Discard;
			ERDGQueueAssignment Queue = ERDGQueueAssignment::Graphics;
			bool bUsed = false;
		};

		struct FLoweredParameterUse final
		{
			FGraphUse Use;
			bool bPresent = false;
		};

		auto MakeParameterUse(const FRDGParameterMemberMetadata& Member,
			std::string_view FieldPath) -> FGraphUse
		{
			FGraphUse Use;
			Use.Kind = Member.ResourceKind;
			Use.Use = Member.Use;
			Use.Access = Member.Access;
			Use.bDiscard = Member.bDiscard;
			Use.bPassManagedTransition = Member.bPassManagedTransition;
			Use.ResultAccess = Member.ResultAccess;
			Use.ParameterPath = FieldPath;
			if (Member.bShaderBinding)
			{
				Use.ShaderBindingName = Member.ShaderBindingName;
				Use.ShaderBindingType = Member.ShaderBindingType;
			}
			if (Member.Kind == ERDGParameterMemberKind::ColorAttachment
				|| Member.Kind == ERDGParameterMemberKind::ManagedColorAttachment
				|| Member.Kind == ERDGParameterMemberKind::DepthStencilAttachment
				|| Member.Kind == ERDGParameterMemberKind::ManagedDepthStencilAttachment)
			{
				Use.bDiscard = Member.LoadAction != ERHIRenderTargetLoadAction::Load;
				Use.bStore = Member.StoreAction == ERHIRenderTargetStoreAction::Store;
			}
			if (Member.Kind == ERDGParameterMemberKind::ValueRead
				|| Member.Kind == ERDGParameterMemberKind::ValueWrite)
				Use.bDiscard = Member.Use == ERDGUse::Write;
			return Use;
		}

		template<typename TextureIndex, typename BufferIndex, typename TokenIndex>
		auto LowerParameterUse(const FRDGParameterMemberMetadata& Member,
			const void* ElementData, std::string_view FieldPath, uint64 Owner,
			std::span<const FGraphResource> Resources,
			TextureIndex&& GetTextureIndex, BufferIndex&& GetBufferIndex,
			TokenIndex&& GetTokenIndex)
			-> FLoweredParameterUse
		{
			FLoweredParameterUse Result;
			auto& Use = Result.Use;
			Use = MakeParameterUse(Member, FieldPath);

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

			switch (Member.Kind)
			{
			case ERDGParameterMemberKind::Texture:
				Result.bPresent = Visit.template operator()<
					FRDGTextureParameter>([&](const auto& Value) {
						Use.ResourceIndex = GetTextureIndex(Value.Texture);
						Use.TextureRange = Value.Range;
					});
				break;
			case ERDGParameterMemberKind::Buffer:
				Result.bPresent = Visit.template operator()<
					FRDGBufferParameter>([&](const auto& Value) {
						Use.ResourceIndex = GetBufferIndex(Value.Buffer);
						Use.BufferOffset = Value.Offset;
						Use.BufferSize = Value.Size;
					});
				break;
			case ERDGParameterMemberKind::Token:
				Result.bPresent = Visit.template operator()<
					FRDGTokenParameter>([&](const auto& Value) {
						Use.ResourceIndex = GetTokenIndex(Value.Token);
						Use.bDiscard = Member.Use != ERDGUse::Read;
					});
				break;
			case ERDGParameterMemberKind::ColorAttachment:
			case ERDGParameterMemberKind::ManagedColorAttachment:
				Result.bPresent = Visit.template operator()<
					FRDGColorAttachmentParameter>([&](const auto& Value) {
						Use.ResourceIndex = GetTextureIndex(Value.Texture);
						Use.TextureRange = Value.Range;
					});
				break;
			case ERDGParameterMemberKind::DepthStencilAttachment:
			case ERDGParameterMemberKind::ManagedDepthStencilAttachment:
				Result.bPresent = Visit.template operator()<
					FRDGDepthStencilAttachmentParameter>([&](const auto& Value) {
						Use.ResourceIndex = GetTextureIndex(Value.Texture);
						Use.TextureRange = Value.Range;
					});
				break;
			case ERDGParameterMemberKind::ManagedTexture:
				Result.bPresent = Visit.template operator()<
					FRDGManagedTextureParameter>([&](const auto& Value) {
						Use.ResourceIndex = GetTextureIndex(Value.Texture);
						Use.TextureRange = Value.Range;
					});
				break;
			case ERDGParameterMemberKind::ValueRead:
			case ERDGParameterMemberKind::ValueWrite:
			{
				uint64 ValueOwner = 0;
				uint32 Index = 0;
				Result.bPresent = Member.ReadValueHandle != nullptr
					&& Member.ReadValueHandle(ElementData, ValueOwner, Index);
				Use.ResourceIndex = ValueOwner == Owner
					? Index : std::numeric_limits<uint32>::max();
				Use.bDiscard = Member.Use == ERDGUse::Write;
				if (Result.bPresent && (Index >= Resources.size()
					|| Resources[Index].ValueTypeIdentity
						!= Member.ValueTypeIdentity))
					Use.ResourceIndex = std::numeric_limits<uint32>::max();
				break;
			}
			case ERDGParameterMemberKind::Nested: break;
			}
			return Result;
		}

		auto IsWriteUse(ERDGUse Use) -> bool
		{
			return Use != ERDGUse::Read;
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

		auto NeedsRangeBarrier(const FBarrierCellState& Cell, const FGraphUse& Use) -> bool
		{
			// Equal writable access still requires a memory dependency.
			return Cell.Access != Use.Access || AccessHasWrite(Cell.Access) || Use.bDiscard;
		}

		auto AdvanceBarrierState(FBarrierCellState& Cell, const FGraphUse& Use) -> void
		{
			Cell.Access = Use.Kind == ERDGResourceKind::Token ? ERHIAccess::None
				: (Use.bPassManagedTransition ? Use.ResultAccess : Use.Access);
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

		auto TextureDescriptionsEqual(const FRHITextureDesc& Left,
			const FRHITextureDesc& Right) -> bool
		{
			return Left.Dimension == Right.Dimension
				&& Left.Flags == Right.Flags && Left.Format == Right.Format
				&& Left.Extent == Right.Extent && Left.Depth == Right.Depth
				&& Left.ArraySize == Right.ArraySize
				&& Left.NumMips == Right.NumMips
				&& Left.NumSamples == Right.NumSamples;
		}

		auto BufferDescriptionsEqual(const FRHIBufferDesc& Left,
			const FRHIBufferDesc& Right) -> bool
		{
			return Left.Size == Right.Size && Left.Stride == Right.Stride
				&& Left.Usage == Right.Usage;
		}

		auto TextureBackingIsCompatible(const FRHITextureDesc& Actual,
			const FRHITextureDesc& Required) -> bool
		{
			FRHITextureDesc NormalizedActual = Actual;
			NormalizedActual.Flags = Required.Flags;
			return EnumHasAllFlags(Actual.Flags, Required.Flags)
				&& TextureDescriptionsEqual(NormalizedActual, Required);
		}

		auto BufferBackingIsCompatible(const FRHIBufferDesc& Actual,
			const FRHIBufferDesc& Required) -> bool
		{
			FRHIBufferDesc NormalizedActual = Actual;
			NormalizedActual.Usage = Required.Usage;
			return EnumHasAllFlags(Actual.Usage, Required.Usage)
				&& BufferDescriptionsEqual(NormalizedActual, Required);
		}

		auto DescribeExternalContract(const FGraphResource& Resource) -> std::string
		{
			std::ostringstream Stream;
			Stream << "kind=";
			switch (Resource.Kind)
			{
			case ERDGResourceKind::Texture:
				Stream << "texture desc=(dimension="
					<< static_cast<uint32>(Resource.TextureDesc.Dimension)
					<< ",extent=" << Resource.TextureDesc.Extent.x << 'x'
					<< Resource.TextureDesc.Extent.y << ",depth="
					<< Resource.TextureDesc.Depth << ",array="
					<< Resource.TextureDesc.ArraySize << ",mips="
					<< static_cast<uint32>(Resource.TextureDesc.NumMips)
					<< ",samples="
					<< static_cast<uint32>(Resource.TextureDesc.NumSamples)
					<< ",format="
					<< static_cast<uint32>(Resource.TextureDesc.Format)
					<< ",flags="
					<< static_cast<uint64>(Resource.TextureDesc.Flags) << ')';
				break;
			case ERDGResourceKind::Buffer:
				Stream << "buffer desc=(size=" << Resource.BufferDesc.Size
					<< ",stride=" << Resource.BufferDesc.Stride << ",usage="
					<< static_cast<uint64>(Resource.BufferDesc.Usage) << ')';
				break;
			case ERDGResourceKind::Token: Stream << "token"; break;
			}
			Stream << " initial=" << static_cast<uint64>(Resource.InitialAccess)
				<< " final=" << static_cast<uint64>(Resource.FinalAccess);
			return Stream.str();
		}

		auto ExternalContractsEqual(const FGraphResource& Left, const FGraphResource& Right) -> bool
		{
			if (Left.Kind != Right.Kind
				|| Left.InitialAccess != Right.InitialAccess
				|| Left.FinalAccess != Right.FinalAccess)
				return false;
			if (Left.Kind == ERDGResourceKind::Texture)
				return TextureDescriptionsEqual(Left.TextureDesc, Right.TextureDesc);
			if (Left.Kind == ERDGResourceKind::Buffer)
				return BufferDescriptionsEqual(Left.BufferDesc, Right.BufferDesc);
			return false;
		}

		auto IsAccessAllowed(ERDGPassType Type, ERHIAccess Access) -> bool
		{
			ERHIAccess Allowed = ERHIAccess::None;
			switch (Type)
			{
			case ERDGPassType::Graphics:
				Allowed = ERHIAccess::VertexBufferRead | ERHIAccess::IndexBufferRead
					| ERHIAccess::GraphicsUniformRead | ERHIAccess::GraphicsShaderRead
					| ERHIAccess::ColorAttachmentReadWrite
					| ERHIAccess::DepthStencilReadWrite
					| ERHIAccess::GraphicsShaderReadWrite;
				break;
			case ERDGPassType::Compute:
				Allowed = ERHIAccess::ComputeUniformRead | ERHIAccess::ComputeShaderRead
					| ERHIAccess::ComputeShaderReadWrite;
				break;
			case ERDGPassType::Copy:
				Allowed = ERHIAccess::TransferRead | ERHIAccess::TransferWrite;
				break;
			}
			return !EnumHasAnyFlags(Access, static_cast<ERHIAccess>(
				static_cast<uint32>(~static_cast<uint32>(Allowed))));
		}

		auto IsExportAccessAllowed(ERDGResourceKind Kind, ERHIAccess Access) -> bool
		{
			constexpr ERHIAccess Allowed = ERHIAccess::VertexBufferRead
				| ERHIAccess::IndexBufferRead | ERHIAccess::GraphicsUniformRead
				| ERHIAccess::ComputeUniformRead | ERHIAccess::GraphicsShaderRead
				| ERHIAccess::ComputeShaderRead | ERHIAccess::TransferRead
				| ERHIAccess::HostRead | ERHIAccess::ColorAttachmentReadWrite
				| ERHIAccess::DepthStencilReadWrite | ERHIAccess::GraphicsShaderReadWrite
				| ERHIAccess::ComputeShaderReadWrite | ERHIAccess::TransferWrite
				| ERHIAccess::HostWrite | ERHIAccess::Present;
			const ERHIAccess TextureOnly = ERHIAccess::Present
				| ERHIAccess::ColorAttachmentReadWrite | ERHIAccess::DepthStencilReadWrite;
			const ERHIAccess BufferOnly = ERHIAccess::VertexBufferRead
				| ERHIAccess::IndexBufferRead | ERHIAccess::GraphicsUniformRead
				| ERHIAccess::ComputeUniformRead;
			return Access != ERHIAccess::None && EnumHasAllFlags(Allowed, Access)
				&& !EnumHasAnyFlags(Access,
					Kind == ERDGResourceKind::Texture ? BufferOnly : TextureOnly);
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
				&& (A.Kind == ERDGResourceKind::Token
					|| (A.Kind == ERDGResourceKind::Texture
						? TextureRangesOverlap(A, B) : BufferRangesOverlap(A, B)));
		}

		auto RangesEqual(const FGraphUse& A, const FGraphUse& B) -> bool
		{
			if (A.ResourceIndex != B.ResourceIndex || A.Kind != B.Kind)
				return false;
			return A.Kind == ERDGResourceKind::Token
				? true : A.Kind == ERDGResourceKind::Texture
				? A.TextureRange == B.TextureRange
				: A.BufferOffset == B.BufferOffset && A.BufferSize == B.BufferSize;
		}

		auto DependencyKindName(ERDGDependencyKind Kind) -> const char*
		{
			switch (Kind)
			{
			case ERDGDependencyKind::Value: return "value";
			case ERDGDependencyKind::Execution: return "execution";
			case ERDGDependencyKind::Explicit: return "explicit";
			}
			return "unknown";
		}

		auto PassTypeName(ERDGPassType Type) -> const char*
		{
			switch (Type)
			{
			case ERDGPassType::Graphics: return "graphics";
			case ERDGPassType::Compute: return "compute";
			case ERDGPassType::Copy: return "copy";
			}
			return "unknown";
		}

		auto GraphUseName(ERDGUse Use) -> const char*
		{
			switch (Use)
			{
			case ERDGUse::Read: return "read";
			case ERDGUse::Write: return "write";
			case ERDGUse::ReadWrite: return "read-write";
			}
			return "unknown";
		}

		auto ParameterMemberKindName(ERDGParameterMemberKind Kind)
			-> const char*
		{
			switch (Kind)
			{
			case ERDGParameterMemberKind::Texture: return "texture";
			case ERDGParameterMemberKind::Buffer: return "buffer";
			case ERDGParameterMemberKind::Token: return "token";
			case ERDGParameterMemberKind::ColorAttachment:
				return "color-attachment";
			case ERDGParameterMemberKind::DepthStencilAttachment:
				return "depth-stencil-attachment";
			case ERDGParameterMemberKind::ManagedColorAttachment:
				return "managed-color-attachment";
			case ERDGParameterMemberKind::ManagedDepthStencilAttachment:
				return "managed-depth-stencil-attachment";
			case ERDGParameterMemberKind::ManagedTexture:
				return "managed-texture";
			case ERDGParameterMemberKind::ValueRead: return "value-read";
			case ERDGParameterMemberKind::ValueWrite: return "value-write";
			case ERDGParameterMemberKind::Nested: return "nested";
			}
			return "unknown";
		}

		auto ValidateParameterMetadata(
			const FRDGParametersMetadata* Metadata,
			uint32 ExpectedSize, uint32 ExpectedAlignment,
			uint32 Depth = 0) -> FRDGResult
		{
			if (Metadata == nullptr)
			{
				return {ERDGError::InvalidParameterMetadata, "render graph parameter metadata is null"};
			}
			if (Metadata->StructName == nullptr || Metadata->StructName[0] == '\0')
			{
				return {ERDGError::InvalidParameterMetadata, "render graph parameter metadata has an empty struct name"};
			}
			if (Metadata->StructSize != ExpectedSize
				|| Metadata->StructAlignment != ExpectedAlignment)
			{
				return {ERDGError::InvalidParameterMetadata, "render graph parameter metadata for '"
					+ std::string(Metadata->StructName) + "' has a mismatched layout"};
			}
			if (Depth >= 32)
			{
				return {ERDGError::InvalidParameterMetadata, "render graph parameter metadata nesting exceeds 32 levels"};
			}

			uint64 PreviousEnd = 0;
			std::vector<std::string_view> MemberNames;
			std::vector<std::string_view> ShaderBindingNames;
			MemberNames.reserve(Metadata->Members.size());
			ShaderBindingNames.reserve(Metadata->Members.size());
			for (const auto& Member : Metadata->Members)
			{
				const std::string Prefix = "render graph parameter metadata for '"
					+ std::string(Metadata->StructName) + "'";
				if (Member.Name == nullptr || Member.Name[0] == '\0')
				{
					return {ERDGError::InvalidParameterMetadata, Prefix + " has an empty member name"};
				}
				if (std::ranges::find(MemberNames, Member.Name)
					!= MemberNames.end())
				{
					return {ERDGError::InvalidParameterMetadata, Prefix + " has duplicate member name '"
						+ Member.Name + "'"};
				}
				MemberNames.emplace_back(Member.Name);
				if (Member.ElementSize == 0 || Member.ArraySize == 0)
				{
					return {ERDGError::InvalidParameterMetadata, Prefix + " member '" + Member.Name
						+ "' has an empty layout"};
				}
				const uint64 End = static_cast<uint64>(Member.Offset)
					+ static_cast<uint64>(Member.ElementSize) * Member.ArraySize;
				if (Member.Offset < PreviousEnd || End > Metadata->StructSize)
				{
					return {ERDGError::InvalidParameterMetadata, Prefix + " member '" + Member.Name
						+ "' has an invalid or unstable offset"};
				}
				PreviousEnd = End;

				if (Member.Kind == ERDGParameterMemberKind::Nested)
				{
					if (Member.bOptional || Member.NestedParameters == nullptr
						|| Member.ElementSize != Member.NestedParameters->StructSize)
					{
						return {ERDGError::InvalidParameterMetadata, Prefix + " member '"
							+ Member.Name + "' has invalid nested metadata"};
					}
					if (auto Error = ValidateParameterMetadata(Member.NestedParameters,
						Member.NestedParameters->StructSize,
						Member.NestedParameters->StructAlignment, Depth + 1); !Error.IsSuccess())
						return Error;
					continue;
				}
				if (Member.NestedParameters != nullptr)
				{
					return {ERDGError::InvalidParameterMetadata, Prefix + " member '" + Member.Name
						+ "' unexpectedly has nested metadata"};
				}
				uint32 ExpectedElementSize = 0;
				switch (Member.Kind)
				{
				case ERDGParameterMemberKind::Texture:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRDGTextureParameter>)
						: sizeof(FRDGTextureParameter);
					break;
				case ERDGParameterMemberKind::Buffer:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRDGBufferParameter>)
						: sizeof(FRDGBufferParameter);
					break;
				case ERDGParameterMemberKind::Token:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRDGTokenParameter>)
						: sizeof(FRDGTokenParameter);
					break;
				case ERDGParameterMemberKind::ColorAttachment:
				case ERDGParameterMemberKind::ManagedColorAttachment:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRDGColorAttachmentParameter>)
						: sizeof(FRDGColorAttachmentParameter);
					break;
				case ERDGParameterMemberKind::DepthStencilAttachment:
				case ERDGParameterMemberKind::ManagedDepthStencilAttachment:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<
							FRDGDepthStencilAttachmentParameter>)
						: sizeof(FRDGDepthStencilAttachmentParameter);
					break;
				case ERDGParameterMemberKind::ManagedTexture:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRDGManagedTextureParameter>)
						: sizeof(FRDGManagedTextureParameter);
					break;
				case ERDGParameterMemberKind::ValueRead:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<TRDGValueRead<std::byte>>)
						: sizeof(TRDGValueRead<std::byte>);
					break;
				case ERDGParameterMemberKind::ValueWrite:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<TRDGValueWrite<std::byte>>)
						: sizeof(TRDGValueWrite<std::byte>);
					break;
				case ERDGParameterMemberKind::Nested: break;
				}
				if (Member.ElementSize != ExpectedElementSize)
				{
					return {ERDGError::InvalidParameterMetadata, Prefix + " member '" + Member.Name
						+ "' has a mismatched wrapper layout"};
				}
				if (Member.bOptional
					!= (Member.ReadOptionalValueAddress != nullptr))
				{
					return {ERDGError::InvalidParameterMetadata, Prefix + " member '" + Member.Name
						+ "' has inconsistent optional layout"};
				}

				const bool bTextureKind = Member.ResourceKind
					== ERDGResourceKind::Texture;
				const bool bBufferKind = Member.ResourceKind
					== ERDGResourceKind::Buffer;
				const bool bTokenKind = Member.ResourceKind
					== ERDGResourceKind::Token;
				bool bShapeValid = false;
				switch (Member.Kind)
				{
				case ERDGParameterMemberKind::Texture:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERDGParameterRangeKind::TextureSubresource
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::Buffer:
					bShapeValid = bBufferKind
						&& Member.RangeKind == ERDGParameterRangeKind::BufferBytes
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::Token:
					bShapeValid = bTokenKind
						&& Member.RangeKind == ERDGParameterRangeKind::None
						&& Member.Access == ERHIAccess::None
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::ColorAttachment:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERDGParameterRangeKind::TextureSubresource
						&& Member.Use == ERDGUse::ReadWrite
						&& Member.Access == ERHIAccess::ColorAttachmentReadWrite
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::DepthStencilAttachment:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERDGParameterRangeKind::TextureSubresource
						&& Member.Use == ERDGUse::ReadWrite
						&& Member.Access == ERHIAccess::DepthStencilReadWrite
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::ManagedColorAttachment:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERDGParameterRangeKind::TextureSubresource
						&& Member.Use == ERDGUse::ReadWrite
						&& Member.Access == ERHIAccess::ColorAttachmentReadWrite
						&& Member.bPassManagedTransition
						&& Member.ResultAccess != ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::ManagedDepthStencilAttachment:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERDGParameterRangeKind::TextureSubresource
						&& Member.Use == ERDGUse::ReadWrite
						&& Member.Access == ERHIAccess::DepthStencilReadWrite
						&& Member.bPassManagedTransition
						&& Member.ResultAccess != ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::ManagedTexture:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERDGParameterRangeKind::TextureSubresource
						&& Member.bPassManagedTransition
						&& Member.ResultAccess != ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::ValueRead:
				case ERDGParameterMemberKind::ValueWrite:
					bShapeValid = bTokenKind
						&& Member.RangeKind == ERDGParameterRangeKind::None
						&& Member.Access == ERHIAccess::None
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None
						&& Member.ValueTypeIdentity != nullptr
						&& Member.ReadValueHandle != nullptr
						&& Member.Use == (Member.Kind
							== ERDGParameterMemberKind::ValueRead
							? ERDGUse::Read : ERDGUse::Write);
					break;
				case ERDGParameterMemberKind::Nested: break;
				}
				if (!bShapeValid)
				{
					return {ERDGError::InvalidParameterMetadata, Prefix + " member '" + Member.Name
						+ "' has inconsistent declaration semantics"};
				}

				if (Member.bShaderBinding)
				{
					if (Member.ShaderBindingName == nullptr
						|| Member.ShaderBindingName[0] == '\0')
					{
						return {ERDGError::InvalidParameterMetadata, Prefix + " member '" + Member.Name
							+ "' has an empty shader binding name"};
					}
					if (std::ranges::find(ShaderBindingNames,
						Member.ShaderBindingName) != ShaderBindingNames.end())
					{
						return {ERDGError::InvalidParameterMetadata, Prefix + " has duplicate shader binding '"
							+ Member.ShaderBindingName + "'"};
					}
					ShaderBindingNames.emplace_back(Member.ShaderBindingName);

					const bool bTextureBinding = Member.Kind
						== ERDGParameterMemberKind::Texture
						&& (Member.ShaderBindingType == ERHIBindingType::Texture
							|| Member.ShaderBindingType
								== ERHIBindingType::StorageImage);
					const bool bBufferBinding = Member.Kind
						== ERDGParameterMemberKind::Buffer
						&& Member.ShaderBindingType
							== ERHIBindingType::StorageBuffer;
					const bool bUav = Member.ShaderBindingType
						== ERHIBindingType::StorageImage
						|| (Member.ShaderBindingType == ERHIBindingType::StorageBuffer
							&& Member.Use != ERDGUse::Read);
					const ERHIAccess ShaderRead = ERHIAccess::GraphicsShaderRead
						| ERHIAccess::ComputeShaderRead;
					const ERHIAccess ShaderReadWrite =
						ERHIAccess::GraphicsShaderReadWrite
						| ERHIAccess::ComputeShaderReadWrite;
					const bool bAccessCompatible = bUav
						? Member.Use != ERDGUse::Read
							&& EnumHasAnyFlags(Member.Access, ShaderReadWrite)
						: Member.Use != ERDGUse::Write
							&& EnumHasAnyFlags(Member.Access,
								ShaderRead | ShaderReadWrite);
					if ((!bTextureBinding && !bBufferBinding)
						|| !bAccessCompatible)
					{
						return {ERDGError::InvalidParameterMetadata, Prefix + " member '" + Member.Name
							+ "' has an incompatible graph/shader declaration"};
					}
				}
				else if (Member.ShaderBindingName != nullptr)
				{
					return {ERDGError::InvalidParameterMetadata, Prefix + " member '" + Member.Name
						+ "' has shader metadata without binding authority"};
				}
			}
			return {};
		}

		auto PassUsePrefix(const FGraphPass& Pass, const FGraphUse& Use)
			-> std::string
		{
			std::string Prefix = "pass '" + Pass.Name + "'";
			if (!Use.ParameterPath.empty())
			{
				Prefix += " parameter '";
				Prefix.append(Use.ParameterPath);
				Prefix += "'";
			}
			return Prefix;
		}

		auto ValidateShaderCompositionMetadata(
			const FRDGParametersMetadata* Metadata)
			-> FRDGResult
		{
			std::vector<std::pair<std::string_view, std::string>> Bindings;
			std::function<FRDGResult(const FRDGParametersMetadata*,
				const std::string&)> Traverse;
			Traverse = [&](const FRDGParametersMetadata* StructMetadata,
				const std::string& ParentPath) -> FRDGResult {
				for (const auto& Member : StructMetadata->Members)
				{
					const std::string Path = ParentPath.empty()
						? std::string(StructMetadata->StructName) + "." + Member.Name
						: ParentPath + "." + Member.Name;
					if (Member.Kind == ERDGParameterMemberKind::Nested)
					{
						if (auto Error = Traverse(Member.NestedParameters, Path);
							!Error.IsSuccess()) return Error;
						continue;
					}
					if (!Member.bShaderBinding) continue;
					const auto Existing = std::ranges::find_if(Bindings,
						[&](const auto& Binding) {
							return Binding.first == Member.ShaderBindingName;
						});
					if (Existing != Bindings.end())
					{
						return {ERDGError::InvalidParameterMetadata, "render graph parameter metadata for '"
							+ std::string(Metadata->StructName)
							+ "' has duplicate shader binding '"
							+ Member.ShaderBindingName + "' at '" + Existing->second
							+ "' and '" + Path + "'"};
					}
					Bindings.emplace_back(Member.ShaderBindingName, Path);
				}
				return {};
			};
			return Traverse(Metadata, {});
		}

		// Resource indices, kinds and shapes remain stable after declaration.
		// Resource final states and graph topology are validated separately.
		auto ValidatePassDeclarations(const FGraphPass& Pass,
			std::span<const FGraphResource> Resources)
			-> FRDGResult
		{
			std::unordered_map<uint32, std::vector<uint32>> ResourceUses;
			for (uint32 UseIndex = 0; UseIndex < Pass.Uses.size(); ++UseIndex)
			{
				const auto& Use = Pass.Uses[UseIndex];
				if (Use.ResourceIndex >= Resources.size()
					|| Resources[Use.ResourceIndex].Kind != Use.Kind)
				{
					return {ERDGError::InvalidDeclaration, PassUsePrefix(Pass, Use) + " has an invalid resource handle"};
				}
				const auto& Resource = Resources[Use.ResourceIndex];
				if (Pass.bExport && !IsExportAccessAllowed(Use.Kind, Use.Access))
				{
					return {ERDGError::InvalidDeclaration, PassUsePrefix(Pass, Use) + " resource '" + Resource.Name
						+ "' has invalid final access"};
				}
				if (Use.Kind != ERDGResourceKind::Token
					&& (Use.Access == ERHIAccess::None
						|| EnumHasAnyFlags(Use.Access, ERHIAccess::Discard)))
				{
					return {ERDGError::InvalidDeclaration, PassUsePrefix(Pass, Use) + " resource '" + Resource.Name
						+ "' has invalid required access"};
				}
				if (Use.Kind != ERDGResourceKind::Token
					&& !Pass.bExport && !IsAccessAllowed(Pass.Type, Use.Access))
				{
					return {ERDGError::InvalidDeclaration, PassUsePrefix(Pass, Use) + " resource '" + Resource.Name
						+ "' access is incompatible with pass domain"};
				}
				if (Use.Kind != ERDGResourceKind::Token
					&& !Pass.bExport && ((Use.Use == ERDGUse::Read
							&& AccessHasWrite(Use.Access))
						|| (Use.Use == ERDGUse::Write
							&& !AccessHasWrite(Use.Access))))
				{
					return {ERDGError::InvalidDeclaration, PassUsePrefix(Pass, Use) + " resource '" + Resource.Name
						+ "' access disagrees with use mode"};
				}
				if (Use.bDiscard && Use.Use == ERDGUse::Read)
				{
					return {ERDGError::InvalidDeclaration, PassUsePrefix(Pass, Use) + " cannot discard a read"};
				}
				if (Use.bPassManagedTransition
					&& (Use.ResultAccess == ERHIAccess::None
						|| EnumHasAnyFlags(Use.ResultAccess, ERHIAccess::Discard)))
				{
					return {ERDGError::InvalidDeclaration, PassUsePrefix(Pass, Use) + " resource '" + Resource.Name
						+ "' has invalid managed attachment result access"};
				}
				if (Use.Kind == ERDGResourceKind::Buffer
					&& (Use.BufferSize == 0
						|| Use.BufferOffset > Resource.BufferDesc.Size
						|| Use.BufferSize > Resource.BufferDesc.Size
							- Use.BufferOffset))
				{
					return {ERDGError::InvalidDeclaration, PassUsePrefix(Pass, Use) + " resource '" + Resource.Name
						+ "' has invalid buffer range"};
				}
				if (Use.Kind == ERDGResourceKind::Texture
					&& (Use.TextureRange.Aspects == ERHITextureAspect::None
						|| Use.TextureRange.NumMips == 0
						|| Use.TextureRange.NumArrayLayers == 0
						|| Use.TextureRange.FirstMip + Use.TextureRange.NumMips
							> Resource.TextureDesc.NumMips
						|| Use.TextureRange.FirstArrayLayer
							+ Use.TextureRange.NumArrayLayers
							> Resource.TextureDesc.ArraySize
						|| !EnumHasAllFlags(
							GetTextureAspects(Resource.TextureDesc.Format),
							Use.TextureRange.Aspects)))
				{
					return {ERDGError::InvalidDeclaration, PassUsePrefix(Pass, Use) + " resource '" + Resource.Name
						+ "' has invalid texture range"};
				}
				auto& EarlierUses = ResourceUses[Use.ResourceIndex];
				for (uint32 OtherUse : EarlierUses)
					if (RangesOverlap(Use, Pass.Uses[OtherUse]))
					{
						std::string Error = PassUsePrefix(Pass, Use)
							+ " declares overlapping uses of resource '"
							+ Resource.Name + "' with ";
						if (!Pass.Uses[OtherUse].ParameterPath.empty())
						{
							Error += "parameter '";
							Error.append(Pass.Uses[OtherUse].ParameterPath);
							Error += "'";
						}
						else Error += "an earlier manual declaration";
						return {ERDGError::InvalidDeclaration, std::move(Error)};
					}
				EarlierUses.push_back(UseIndex);
			}
			return {};
		}

		// Borrows frozen uses in declaration order within each resource's contiguous slice.
		struct FResourceUseTable final
		{
			std::vector<size_t> Offsets;
			std::vector<const FGraphUse*> Uses;

			auto operator[](uint32 ResourceIndex) const -> std::span<const FGraphUse* const>
			{
				return std::span(Uses).subspan(Offsets[ResourceIndex],
					Offsets[ResourceIndex + 1] - Offsets[ResourceIndex]);
			}
		};

		auto SafetyLimit(std::string_view Name, size_t Actual, size_t Limit)
			-> FRDGResult
		{
			return {ERDGError::SafetyLimitExceeded, "render graph safety limit exceeded: " + std::string(Name)
				+ " actual=" + std::to_string(Actual) + " limit="
				+ std::to_string(Limit)};
		}

		struct FRangeWork final
		{
			const FRDGBudget& Budget;
			size_t Candidates = 0;
			size_t Visits = 0;
			auto Visit() -> bool { return ++Visits <= Budget.MaxCellVisits; }
			auto Error() const -> FRDGResult
			{ return SafetyLimit("cell-visits", Visits, Budget.MaxCellVisits); }
		};

		// Fixed texture subresource indices and one tracking cell per buffer or token.
		// All phase consumers borrow this layout as const data.
		struct FTrackingLayout final
		{
			struct FResourceLayout final
			{
				size_t Begin = 0;
				uint32 Mips = 0;
				uint32 Layers = 0;
				ERHITextureAspect Aspects = ERHITextureAspect::None;
			};
			std::vector<FRangeCell> Ranges;
			std::vector<FResourceLayout> Resources;

			template <typename FVisitor>
			auto VisitUse(const FGraphUse& Use, FVisitor&& Visitor) const -> FRDGResult
			{
				const auto& Layout = Resources[Use.ResourceIndex];
				if (Use.Kind != ERDGResourceKind::Texture)
					return Visitor(Layout.Begin, Ranges[Layout.Begin]);
				size_t AspectBegin = Layout.Begin;
				for (ERHITextureAspect Aspect : {ERHITextureAspect::Color,
					ERHITextureAspect::Depth, ERHITextureAspect::Stencil})
				{
					if (!EnumHasAnyFlags(Layout.Aspects, Aspect)) continue;
					if (EnumHasAnyFlags(Use.TextureRange.Aspects, Aspect))
						for (uint32 Mip = Use.TextureRange.FirstMip;
							Mip < Use.TextureRange.FirstMip + Use.TextureRange.NumMips; ++Mip)
							for (uint32 Layer = Use.TextureRange.FirstArrayLayer;
								Layer < Use.TextureRange.FirstArrayLayer + Use.TextureRange.NumArrayLayers; ++Layer)
							{
								const size_t Index = AspectBegin + static_cast<size_t>(Mip) * Layout.Layers + Layer;
								if (auto Error = Visitor(Index, Ranges[Index]); !Error.IsSuccess()) return Error;
							}
					AspectBegin += static_cast<size_t>(Layout.Mips) * Layout.Layers;
				}
				return {};
			}
		};

		// Buffer declarations retain their binding ranges, but dependency and barrier
		// analysis consume one combined access per resource and pass.
		struct FPassTrackingUses final
		{
			std::vector<FGraphUse> Uses;
			std::unordered_map<uint32, std::vector<const FGraphUse*>> BufferDeclarations;
		};

		auto BuildTrackingUses(std::span<const FGraphUse> Uses) -> FPassTrackingUses
		{
			FPassTrackingUses Result;
			Result.Uses.reserve(Uses.size());
			std::unordered_map<uint32, size_t> Buffers;
			for (const auto& Use : Uses)
			{
				if (Use.Kind == ERDGResourceKind::Buffer)
				{
					Result.BufferDeclarations[Use.ResourceIndex].push_back(&Use);
					const auto [It, bInserted] = Buffers.try_emplace(Use.ResourceIndex, Result.Uses.size());
					if (!bInserted)
					{
						auto& Combined = Result.Uses[It->second];
						Combined.Access |= Use.Access;
						if (Combined.Use != Use.Use) Combined.Use = ERDGUse::ReadWrite;
						Combined.bDiscard = false;
						continue;
					}
				}
				Result.Uses.push_back(Use);
			}
			return Result;
		}

		// Both compilation and lazy diagnostics consume this event stream. The caller
		// owns the layout; no diagnostic records are retained by normal compilation.
		template <typename FTransitionVisitor, typename FUseVisitor>
		auto TraverseExecutionStates(const FTrackingLayout& Cells,
			std::span<const FGraphResource> Resources, const FGraphPassView& Passes,
			std::span<const FRDGCompiledPass> ScheduledPasses,
			std::span<const FRDGResourceLifetime> Lifetimes, FRangeWork* Work, bool bAsyncEnabled,
			FTransitionVisitor&& OnTransition, FUseVisitor&& OnUse) -> FRDGResult
		{
			std::vector<FBarrierCellState> States(Cells.Ranges.size());
			for (size_t Index = 0; Index < Cells.Ranges.size(); ++Index)
				States[Index].Access = Resources[Cells.Ranges[Index].ResourceIndex].InitialAccess;

			for (uint32 PassIndex = 0; PassIndex < ScheduledPasses.size(); ++PassIndex)
			{
				const uint32 DeclarationIndex = ScheduledPasses[PassIndex].DeclarationIndex;
				const auto Queue = bAsyncEnabled && Passes[DeclarationIndex].bAsyncComputeEligible
					? ERDGQueueAssignment::AsyncCompute : ERDGQueueAssignment::Graphics;
				const auto Tracking = BuildTrackingUses(Passes[DeclarationIndex].Uses);
				for (const auto& Use : Tracking.Uses)
				{
					if (auto Error = Cells.VisitUse(Use, [&](size_t CellIndex, const FRangeCell& Range) -> FRDGResult
					{
						auto& Cell = States[CellIndex];
						if (Work != nullptr && !Work->Visit()) return Work->Error();
						auto Emit = [&](ERHIAccess Before, ERHIAccess After,
							ERDGTransitionKind Kind, bool bDiscard) -> FRDGResult {
							return OnTransition(FRDGTransitionCapture{Use.ResourceIndex,
								PassIndex, Before, After, Range.TextureRange,
								Range.BufferOffset, Range.BufferSize, false, bDiscard, Kind,
								Kind == ERDGTransitionKind::RHIBarrier ? Cell.Queue : Queue, Queue}, CellIndex);
						};
						if (Use.Kind != ERDGResourceKind::Token && (NeedsRangeBarrier(Cell, Use) || Cell.Queue != Queue))
							if (auto Error = Emit(Cell.Access, Use.Access,
								ERDGTransitionKind::RHIBarrier, Use.bDiscard
									&& (Use.Kind != ERDGResourceKind::Buffer
										|| (Use.BufferOffset == 0 && Use.BufferSize == Range.BufferSize))); !Error.IsSuccess())
								return Error;
						if (Use.bPassManagedTransition)
							if (auto Error = Emit(Use.Access, Use.ResultAccess,
								ERDGTransitionKind::PassManaged, false); !Error.IsSuccess())
								return Error;
						AdvanceBarrierState(Cell, Use);
						Cell.Queue = Queue;
						Cell.bUsed = true;
						if (Use.Kind == ERDGResourceKind::Buffer)
						{
							for (const auto* Declared : Tracking.BufferDeclarations.at(Use.ResourceIndex))
								OnUse(DeclarationIndex, *Declared, CellIndex, Range, IsWriteUse(Use.Use));
						}
						else OnUse(DeclarationIndex, Use, CellIndex, Range, IsWriteUse(Use.Use));
						return {};
					}); !Error.IsSuccess()) return Error;
				}
			}
			for (size_t CellIndex = 0; CellIndex < Cells.Ranges.size(); ++CellIndex)
			{
				const auto& Range = Cells.Ranges[CellIndex];
				const auto& Cell = States[CellIndex];
				const auto& Resource = Resources[Range.ResourceIndex];
				if (!Cell.bUsed || Range.Kind == ERDGResourceKind::Token
					|| Lifetimes[Range.ResourceIndex].bCulled
					|| ((Resource.FinalAccess == ERHIAccess::None || Resource.FinalAccess == Cell.Access)
						&& Cell.Queue == ERDGQueueAssignment::Graphics)) continue;
				if (auto Error = OnTransition(FRDGTransitionCapture{Range.ResourceIndex,
					std::numeric_limits<uint32>::max(), Cell.Access,
					Resource.FinalAccess == ERHIAccess::None ? Cell.Access : Resource.FinalAccess,
					Range.TextureRange, Range.BufferOffset, Range.BufferSize, true, false,
					ERDGTransitionKind::RHIBarrier, Cell.Queue, ERDGQueueAssignment::Graphics}, CellIndex);
					!Error.IsSuccess()) return Error;
			}
			return {};
		}

		// Owns canonical typed edges and endpoint deduplication during analysis.
		struct FDependencyGraph final
		{
			std::vector<FRDGDependency> Dependencies;
			std::unordered_map<uint64, size_t> EdgeIndices;
			uint32 MaxDependencies = 0;
		};

		auto AddDependencyEdge(FDependencyGraph& Graph, uint32 Before,
			uint32 After, const std::string& Cause, ERDGDependencyKind Kind) -> FRDGResult
		{
			if (Before >= After)
			{
				return {ERDGError::InvalidDependency, "dependency must point forward: producer["
					+ std::to_string(Before) + "] consumer[" + std::to_string(After) + "]"};
			}
			const uint64 Key = (static_cast<uint64>(Before) << 32) | After;
			const auto Found = Graph.EdgeIndices.find(Key);
			if (Found != Graph.EdgeIndices.end())
			{
				auto& Existing = Graph.Dependencies[Found->second];
				if (Existing.Kind == ERDGDependencyKind::Execution
					&& Kind != ERDGDependencyKind::Execution)
				{
					Existing.Kind = Kind;
					Existing.Cause = Cause;
				}
				return {};
			}
			if (Graph.Dependencies.size() >= Graph.MaxDependencies)
			{
				return SafetyLimit("dependencies", Graph.Dependencies.size() + 1,
					Graph.MaxDependencies);
			}
			Graph.EdgeIndices.emplace(Key, Graph.Dependencies.size());
			Graph.Dependencies.push_back({Before, After, Cause, Kind});
			return {};
		}

		auto ValidateGraphResources(std::span<const FGraphResource> Resources)
			-> FRDGResult
		{
			std::unordered_set<std::string_view> Names;
			Names.reserve(Resources.size());
			for (uint32 ResourceIndex = 0; ResourceIndex < Resources.size();
				++ResourceIndex)
			{
				const auto& Resource = Resources[ResourceIndex];
				if (Resource.Name.empty())
					return {ERDGError::InvalidDeclaration, "resource[" + std::to_string(ResourceIndex)
						+ "] has an empty name"};
				if (Resource.bExternal && !Resource.Texture && !Resource.Buffer)
					return {ERDGError::InvalidDeclaration, "resource '" + Resource.Name
						+ "' has no physical resource"};
				if (Resource.bExternal && Resource.FinalAccess == ERHIAccess::None)
					return {ERDGError::InvalidDeclaration, "external resource '" + Resource.Name
						+ "' has no final access"};
				if (EnumHasAnyFlags(Resource.FinalAccess, ERHIAccess::Discard))
					return {ERDGError::InvalidDeclaration, "resource '" + Resource.Name
						+ "' has invalid final access"};
				if (!Names.insert(Resource.Name).second)
					return {ERDGError::InvalidDeclaration, "duplicate resource name '" + Resource.Name + "'"};
			}
			return {};
		}

		auto ValidateGraphPasses(FGraphPassView Passes,
			std::span<const FGraphResource> Resources,
			FDependencyGraph& Graph) -> FRDGResult
		{
			std::unordered_set<std::string_view> Names;
			Names.reserve(Passes.size());
			for (uint32 PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
			{
				const auto& Pass = Passes[PassIndex];
				if (Pass.Name.empty())
					return {ERDGError::InvalidDeclaration, "pass[" + std::to_string(PassIndex)
						+ "] has an empty name"};
				if (!Names.insert(Pass.Name).second)
					return {ERDGError::InvalidDeclaration, "duplicate pass name '" + Pass.Name + "'"};
				for (uint32 Prerequisite : Pass.Prerequisites)
				{
					if (Prerequisite >= Passes.size())
						return {ERDGError::InvalidDeclaration, "pass '" + Pass.Name
							+ "' has an invalid producer pass handle"};
					if (auto Error = AddDependencyEdge(Graph, Prerequisite, PassIndex, "explicit",
						ERDGDependencyKind::Explicit); !Error.IsSuccess()) return Error;
				}
				if (!Pass.bDeclarationsValidated)
					if (auto Error = ValidatePassDeclarations(Pass, Resources);
						!Error.IsSuccess()) return Error;
			}
			return {};
		}

		auto BuildResourceUseTable(FGraphPassView Passes,
			uint32 ResourceCount) -> FResourceUseTable
		{
			FResourceUseTable ResourceUses;
			ResourceUses.Offsets.resize(static_cast<size_t>(ResourceCount) + 1, 0);
			for (size_t Index = 0; Index < Passes.size(); ++Index)
				for (const auto& Use : Passes[Index].Uses)
					++ResourceUses.Offsets[Use.ResourceIndex + 1];
			std::partial_sum(ResourceUses.Offsets.begin(), ResourceUses.Offsets.end(),
				ResourceUses.Offsets.begin());
			ResourceUses.Uses.resize(ResourceUses.Offsets.back());
			auto Cursors = ResourceUses.Offsets;
			for (size_t Index = 0; Index < Passes.size(); ++Index)
				for (const auto& Use : Passes[Index].Uses)
					ResourceUses.Uses[Cursors[Use.ResourceIndex]++] = &Use;
			return ResourceUses;
		}

		auto ValidateTypedValueWriters(
			std::span<const FGraphResource> Resources,
			const FResourceUseTable& ResourceUses) -> FRDGResult
		{
			for (uint32 ResourceIndex = 0; ResourceIndex < Resources.size();
				++ResourceIndex)
			{
				const auto& Resource = Resources[ResourceIndex];
				if (Resource.ValueTypeIdentity == nullptr) continue;
				const size_t Writers = std::ranges::count_if(
					ResourceUses[ResourceIndex], [](const FGraphUse* Use) {
						return Use->Use == ERDGUse::Write;
					});
				if (Writers != 1)
					return {ERDGError::InvalidDeclaration, "typed value '" + Resource.Name + "' type '"
						+ Resource.ValueTypeName
						+ "' requires exactly one writer; actual="
						+ std::to_string(Writers)};
			}
			return {};
		}

		// Layout size depends only on resource descriptions, never on use endpoints.
		auto BuildTrackingLayout(std::span<const FGraphResource> Resources,
			const FResourceUseTable& ResourceUses, FRangeWork& Work,
			FTrackingLayout& Result) -> FRDGResult
		{
			Result.Resources.resize(Resources.size());
			for (uint32 ResourceIndex = 0; ResourceIndex < Resources.size(); ++ResourceIndex)
			{
				const auto& Resource = Resources[ResourceIndex];
				if (ResourceUses[ResourceIndex].empty()) continue;
				auto& Layout = Result.Resources[ResourceIndex];
				Layout.Begin = Result.Ranges.size();
				auto Add = [&](FRHITextureSubresourceRange Range) -> FRDGResult {
					if (!Work.Visit()) return Work.Error();
					if (Result.Ranges.size() >= Work.Budget.MaxRangeCells)
						return SafetyLimit("range-cells", Result.Ranges.size() + 1, Work.Budget.MaxRangeCells);
					if (++Work.Candidates > Work.Budget.MaxRangeCellCandidates)
						return SafetyLimit("range-cell-candidates", Work.Candidates, Work.Budget.MaxRangeCellCandidates);
					Result.Ranges.push_back({Resource.Kind, ResourceIndex, Range, 0,
						Resource.Kind == ERDGResourceKind::Buffer ? Resource.BufferDesc.Size : 0});
					return {};
				};
				if (Resource.Kind != ERDGResourceKind::Texture)
				{
					if (auto Error = Add({}); !Error.IsSuccess()) return Error;
					continue;
				}
				Layout.Mips = Resource.TextureDesc.NumMips;
				Layout.Layers = Resource.TextureDesc.ArraySize;
				Layout.Aspects = GetTextureAspects(Resource.TextureDesc.Format);
				for (ERHITextureAspect Aspect : {ERHITextureAspect::Color,
					ERHITextureAspect::Depth, ERHITextureAspect::Stencil})
					if (EnumHasAnyFlags(Layout.Aspects, Aspect))
						for (uint32 Mip = 0; Mip < Layout.Mips; ++Mip)
							for (uint32 Layer = 0; Layer < Layout.Layers; ++Layer)
								if (auto Error = Add({Aspect, Mip, 1, Layer, 1}); !Error.IsSuccess()) return Error;
			}
			return {};
		}

		auto BuildHazardDependencies(FGraphPassView Passes,
			std::span<const FGraphResource> Resources,
			const FTrackingLayout& Cells, FDependencyGraph& Graph, FRangeWork& Work)
			-> FRDGResult
		{
			std::vector<FDependencyCellState> States(Cells.Ranges.size());
			for (size_t Index = 0; Index < Cells.Ranges.size(); ++Index)
				States[Index].bProduced = Resources[Cells.Ranges[Index].ResourceIndex].HasInitialContents();
			for (uint32 PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
				for (const auto& Use : BuildTrackingUses(Passes[PassIndex].Uses).Uses)
					if (auto Error = Cells.VisitUse(Use, [&](size_t CellIndex, const FRangeCell&) -> FRDGResult
					{
						auto& Cell = States[CellIndex];
						if (!Work.Visit()) return Work.Error();
						const auto& Resource = Resources[Use.ResourceIndex];
						if (Use.Use != ERDGUse::Write && !Use.bDiscard
							&& !Cell.bProduced)
							return {ERDGError::MissingProducer, PassUsePrefix(Passes[PassIndex], Use)
								+ " reads resource '" + Resource.Name
								+ "' before its producer"};
						if (Use.Use == ERDGUse::Read)
						{
							if (Cell.Producer != std::numeric_limits<uint32>::max())
								if (auto Error = AddDependencyEdge(Graph, Cell.Producer, PassIndex,
									Resource.Name, ERDGDependencyKind::Value); !Error.IsSuccess()) return Error;
							// Passes are visited in declaration order; repeated reads
							// by this pass can only be the last reader.
							if (Cell.Readers.empty() || Cell.Readers.back() != PassIndex)
								Cell.Readers.push_back(PassIndex);
							return {};
						}
						// Resource-level buffer writes preserve earlier producers: a
						// partial write cannot prove that their contents are dead.
						if (((Use.Use == ERDGUse::ReadWrite && !Use.bDiscard)
							|| Use.Kind == ERDGResourceKind::Buffer)
							&& Cell.Producer != std::numeric_limits<uint32>::max())
							if (auto Error = AddDependencyEdge(Graph, Cell.Producer, PassIndex,
								Resource.Name, ERDGDependencyKind::Value); !Error.IsSuccess()) return Error;
						for (uint32 Reader : Cell.Readers)
							if (auto Error = AddDependencyEdge(Graph, Reader, PassIndex, Resource.Name,
								ERDGDependencyKind::Execution); !Error.IsSuccess()) return Error;
						if (Cell.Readers.empty()
							&& Cell.Producer != std::numeric_limits<uint32>::max())
							if (auto Error = AddDependencyEdge(Graph, Cell.Producer, PassIndex,
								Resource.Name, ERDGDependencyKind::Execution); !Error.IsSuccess()) return Error;
						Cell.Producer = Use.bStore
							? PassIndex : std::numeric_limits<uint32>::max();
						Cell.bProduced = Use.bStore;
						Cell.Readers.clear();
						return {};
					}); !Error.IsSuccess()) return Error;
			return {};
		}

		auto FindRetainedPasses(FGraphPassView Passes,
			std::span<const FRDGDependency> Dependencies, bool bEnableCulling)
			-> std::vector<bool>
		{
			std::vector<bool> Retained(Passes.size(), !bEnableCulling);
			if (!bEnableCulling) return Retained;

			// Index finalized kinds so Execution-to-Value upgrades propagate retention.
			std::vector<size_t> Offsets(Passes.size() + 1, 0);
			for (const auto& Edge : Dependencies)
				if (Edge.Kind != ERDGDependencyKind::Execution)
					++Offsets[Edge.AfterPass + 1];
			std::partial_sum(Offsets.begin(), Offsets.end(), Offsets.begin());
			std::vector<uint32> Predecessors(Offsets.back());
			{
				auto Cursors = Offsets;
				for (const auto& Edge : Dependencies)
					if (Edge.Kind != ERDGDependencyKind::Execution)
						Predecessors[Cursors[Edge.AfterPass]++] = Edge.BeforePass;
			}

			std::vector<uint32> Pending;
			Pending.reserve(Passes.size());
			for (uint32 Index = 0; Index < Passes.size(); ++Index)
				if (Passes[Index].bRoot)
				{
					Retained[Index] = true;
					Pending.push_back(Index);
				}
			while (!Pending.empty())
			{
				const uint32 After = Pending.back();
				Pending.pop_back();
				for (uint32 Before : std::span(Predecessors).subspan(
					Offsets[After], Offsets[After + 1] - Offsets[After]))
					if (!Retained[Before])
					{
						Retained[Before] = true;
						Pending.push_back(Before);
					}
			}
			return Retained;
		}
	} // namespace

	auto BuildRDGParameterLayout(const FRDGParametersMetadata* Metadata,
		uint32 ExpectedSize, uint32 ExpectedAlignment)
		-> FRDGParameterLayoutBuildResult
	{
		FRDGParameterLayoutBuildResult Result;
		Result.Result = ValidateParameterMetadata(Metadata, ExpectedSize, ExpectedAlignment);
		if (!Result.Result.IsSuccess()) return Result;
		Result.Result = ValidateShaderCompositionMetadata(Metadata);
		if (!Result.Result.IsSuccess()) return Result;

		auto Layout = std::make_unique<FRDGParameterLayout>();
		Layout->Metadata = Metadata;
		std::function<void(const FRDGParametersMetadata*, uint32,
			const std::string&)> Flatten;
		Flatten = [&](const FRDGParametersMetadata* StructMetadata,
			uint32 BaseOffset, const std::string& ParentPath) {
			for (const FRDGParameterMemberMetadata& Member : StructMetadata->Members)
			{
				const uint32 MemberOffset = BaseOffset + Member.Offset;
				const std::string MemberPath = ParentPath.empty()
					? std::string(Member.Name) : ParentPath + "." + Member.Name;
				if (Member.Kind == ERDGParameterMemberKind::Nested)
				{
					for (uint32 ElementIndex = 0;
						ElementIndex < Member.ArraySize; ++ElementIndex)
					{
						std::string ElementPath = MemberPath;
						if (Member.ArraySize > 1)
							ElementPath += "[" + std::to_string(ElementIndex) + "]";
						Flatten(Member.NestedParameters,
							MemberOffset + ElementIndex * Member.ElementSize,
							ElementPath);
					}
					continue;
				}

				const uint32 LeafIndex = static_cast<uint32>(Layout->Leaves.size());
				const uint32 FirstElementIndex =
					static_cast<uint32>(Layout->Elements.size());
				Layout->Leaves.push_back({&Member, MemberOffset,
					FirstElementIndex, MemberPath});
				for (uint32 ElementIndex = 0;
					ElementIndex < Member.ArraySize; ++ElementIndex)
				{
					const uint32 FlatElementIndex =
						static_cast<uint32>(Layout->Elements.size());
					std::string FieldPath = std::string(Metadata->StructName)
						+ "." + MemberPath;
					if (Member.ArraySize > 1)
						FieldPath += "[" + std::to_string(ElementIndex) + "]";
					Layout->Elements.push_back({
						.Offset = MemberOffset + ElementIndex * Member.ElementSize,
						.LeafIndex = LeafIndex,
						.ArrayElementIndex = ElementIndex,
						.FieldPath = std::move(FieldPath),
					});
					auto AddCategory = [&](std::vector<uint32>& Category) {
						Category.push_back(FlatElementIndex);
					};
					switch (Member.Kind)
					{
					case ERDGParameterMemberKind::Texture:
					case ERDGParameterMemberKind::ManagedTexture:
						AddCategory(Layout->TextureElements); break;
					case ERDGParameterMemberKind::Buffer:
						AddCategory(Layout->BufferElements); break;
					case ERDGParameterMemberKind::ValueRead:
					case ERDGParameterMemberKind::ValueWrite:
						AddCategory(Layout->ValueElements); break;
					case ERDGParameterMemberKind::Token:
						AddCategory(Layout->TokenElements); break;
					case ERDGParameterMemberKind::ColorAttachment:
					case ERDGParameterMemberKind::DepthStencilAttachment:
					case ERDGParameterMemberKind::ManagedColorAttachment:
					case ERDGParameterMemberKind::ManagedDepthStencilAttachment:
						AddCategory(Layout->AttachmentElements); break;
					case ERDGParameterMemberKind::Nested: break;
					}
				}
				if (Member.bShaderBinding)
					Layout->ShaderBindings.push_back({Member.ShaderBindingName,
						LeafIndex});
			}
		};
		Flatten(Metadata, 0, {});
		Layout->OffsetIndex.resize(Layout->Elements.size());
		std::iota(Layout->OffsetIndex.begin(), Layout->OffsetIndex.end(), 0u);
		std::ranges::sort(Layout->OffsetIndex, {},
			[&](uint32 Index) { return Layout->Elements[Index].Offset; });
		std::ranges::sort(Layout->ShaderBindings, {},
			&FRDGParameterShaderBinding::Name);
		Result.Layout = std::move(Layout);
		return Result;
	}

	struct FRDGBuilder::FState
	{
		bool bAsyncComputeEnabled = false;
		uint64 CompileMicroseconds = 0;
		uint64 ExecuteMicroseconds = 0;
		FRDGPhaseTimings Phases;
		uint64 Owner = 0;
		std::vector<FGraphResource> Resources;
		std::unordered_map<const void*, uint32> ExternalResources;
		std::vector<FGraphPass> Passes;
		std::vector<FRDGResult> DeclarationErrors;
		bool bEnableCulling = false;
		FRDGBudget Budget;
		ERDGBuilderState Lifecycle = ERDGBuilderState::Building;
		FRDGExecutionResult ExecutionResult;
		std::vector<FRHIGPUSubmissionReceipt> SubmissionReceipts;
		std::shared_ptr<FRDGAllocationRetirement> AllocationRetirement;
		bool bCompiled = false;
		uint32 PendingConstructions = 0;
		FGraphParameterStorage ParameterStorage;
		FGraphParameterStorage ValueStorage;
	};

	struct FRDGBuilder::FDiagnostics final
	{
		std::vector<FRDGResourceCapture> Resources;
		std::vector<FRDGParameterCapture> Parameters;
		std::vector<FRDGUseCapture> Uses;
		std::vector<FRDGTransitionCapture> Transitions;
		std::vector<FRDGCullingDecision> CullingDecisions;
	};

	struct FRDGBuilder::FCompiledState
	{
		// Keeps all execution-only state for one scheduled pass in one record.
		struct FCompiledPassRuntime final
		{
			const FRDGParameterizedPassExecute* ParameterizedExecute = nullptr;
			const FRDGParameterLayout* ParameterLayout = nullptr;
			const void* Parameters = nullptr;
			// Borrows immutable declaration storage for the builder execution lifetime.
			std::span<const FOptionalAlias> OptionalAliases;
			std::vector<uint32> ResourceIndices;
			std::vector<std::pair<uint32, ERDGUse>> ValueUses;
		};

		uint64 Owner = 0;
		// Borrows declarations owned by this single-use builder.
		std::span<const FGraphResource> Resources;
		std::vector<FGraphResourceBacking> Backings;
		std::vector<FRDGCompiledPass> Passes;
		std::vector<FCompiledPassRuntime> RuntimePasses;
		std::vector<FRDGDependency> Dependencies;
		std::vector<FRDGResourceLifetime> ResourceLifetimes;
		std::vector<bool> Retained;
		FGraphPass ExportPass;
		FRDGBarrierBatch FinalBarriers;
		FRDGExecutionPlan ExecutionPlan;
		std::vector<FRDGAllocationRequest> AllocationRequests;
		FRDGBudget Budget;
		FRDGAllocationStatistics AllocationStatistics;
	};

	FRDGBuilder::FRDGBuilder()
		: Compiled(std::make_unique<FCompiledState>()), State(std::make_unique<FState>())
	{
		static std::atomic<uint64> NextOwner{1};
		State->Owner = NextOwner.fetch_add(1, std::memory_order_relaxed);
	}

	FRDGBuilder::~FRDGBuilder() = default;

	auto FRDGBuilder::RequireBuilding() const -> void
	{
		requiref(State->Lifecycle == ERDGBuilderState::Building,
			"render graph declarations require Building state");
	}

	auto FRDGBuilder::BeginStorageConstruction() -> void
	{
		RequireBuilding();
		++State->PendingConstructions;
	}

	auto FRDGBuilder::EndStorageConstruction() -> void
	{
		--State->PendingConstructions;
	}

	auto FRDGBuilder::GetState() const -> ERDGBuilderState
	{ return State->Lifecycle; }

	auto FRDGBuilder::GetExecutionResult() const -> const FRDGExecutionResult&
	{ return State->ExecutionResult; }

	auto FRDGBuilder::HasCompiledPlan() const -> bool
	{ return State->bCompiled; }

	auto FRDGBuilder::CompileForTesting() -> FRDGResult
	{
		if (State->Lifecycle != ERDGBuilderState::Building)
			return {ERDGError::InvalidState, "render graph is already consumed"};
		State->Lifecycle = ERDGBuilderState::Compiling;
		struct FFailureGuard
		{
			ERDGBuilderState& State;
			~FFailureGuard()
			{
				if (State == ERDGBuilderState::Compiling) State = ERDGBuilderState::Failed;
			}
		} Guard{State->Lifecycle};
		auto Error = Compile();
		State->Lifecycle = Error.IsSuccess() ? ERDGBuilderState::Preparing : ERDGBuilderState::Failed;
		return Error;
	}

	auto FRDGBuilder::Execute(FRHICommandListImmediate& CommandList,
		FRDGExecutionContext* Context) -> FRDGExecutionResult
	{
		if (State->Lifecycle != ERDGBuilderState::Building)
			return {ERDGExecutionStatus::InvalidState,
				{ERDGError::InvalidState, "render graph is already consumed"}};
		State->Lifecycle = ERDGBuilderState::Compiling;
		// Preserve terminal state during supported unwinding without swallowing exceptions.
		struct FFailureGuard
		{
			ERDGBuilderState& State;
			~FFailureGuard()
			{
				if (State != ERDGBuilderState::Recorded) State = ERDGBuilderState::Failed;
			}
		} Guard{State->Lifecycle};
		State->ExecutionResult = {ERDGExecutionStatus::CompileFailed,
			{ERDGError::InvalidState, "render graph compilation did not complete"}};
		State->ExecutionResult.Result = Compile();
		if (!State->ExecutionResult.Result.IsSuccess()) return State->ExecutionResult;
		State->Lifecycle = ERDGBuilderState::Preparing;
		State->ExecutionResult.Status = ERDGExecutionStatus::PreparationFailed;
		State->ExecutionResult.Result =
			{ERDGError::InvalidState, "render graph preparation did not complete"};
		State->ExecutionResult.Result = Record(CommandList, Context);
		if (!State->ExecutionResult.Result.IsSuccess()) return State->ExecutionResult;
		State->Lifecycle = ERDGBuilderState::Recorded;
		State->ExecutionResult.Status = ERDGExecutionStatus::Recorded;
		return State->ExecutionResult;
	}

	auto FRDGBuilder::AllocateParameterStorage(size_t Size,
		size_t Alignment, const FRDGParametersMetadata* Metadata,
		const FRDGParameterLayoutBuildResult& LayoutResult,
		void (*Destroy)(void*), std::weak_ptr<void>& OutLifetime,
		size_t& OutAllocationIndex) -> void*
	{
		RequireBuilding();
		if (LayoutResult.Layout == nullptr)
		{
			State->DeclarationErrors.push_back(LayoutResult.Result);
			return nullptr;
		}
		if (LayoutResult.Layout->Metadata != Metadata
			|| Metadata->StructSize != Size
			|| Metadata->StructAlignment != Alignment)
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidParameterMetadata,
				"render graph parameter layout does not match its typed metadata"});
			return nullptr;
		}
		auto Allocation = std::make_shared<FGraphParameterAllocation>(
			Size, Alignment, Destroy);
		Allocation->Layout = LayoutResult.Layout.get();
		void* Data = Allocation->Data;
		OutLifetime = Allocation;
		OutAllocationIndex = State->ParameterStorage.Allocations.size();
		State->ParameterStorage.Allocations.push_back(std::move(Allocation));
		return Data;
	}

	auto FRDGBuilder::MarkParameterStorageConstructed(size_t AllocationIndex)
		-> void
	{
		require(AllocationIndex < State->ParameterStorage.Allocations.size());
		State->ParameterStorage.Allocations[AllocationIndex]->bConstructed = true;
	}

	auto FRDGBuilder::StateOwner() const -> uint64
	{
		return State->Owner;
	}

	auto FRDGBuilder::AllocateValueStorage(std::string_view Name,
		std::string_view StableTypeName, const void* TypeIdentity, size_t Size,
		size_t Alignment, void (*Destroy)(void*), uint32& OutIndex) -> void*
	{
		RequireBuilding();
		if (Name.empty() || StableTypeName.empty() || TypeIdentity == nullptr
			|| Size == 0 || Alignment == 0 || Destroy == nullptr)
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				"render graph value has invalid storage metadata"});
			return nullptr;
		}
		for (const auto& Existing : State->Resources)
		{
			if (Existing.ValueTypeIdentity == nullptr) continue;
			if (Existing.ValueTypeIdentity == TypeIdentity
				&& Existing.ValueTypeName != StableTypeName)
			{
				State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
					"typed value '"
					+ std::string(Name) + "' changes stable type name from '"
					+ Existing.ValueTypeName + "' to '"
					+ std::string(StableTypeName) + "'"});
				return nullptr;
			}
			if (Existing.ValueTypeIdentity != TypeIdentity
				&& Existing.ValueTypeName == StableTypeName)
			{
				State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
					"typed value '"
					+ std::string(Name) + "' reuses stable type name '"
					+ std::string(StableTypeName) + "' for a different C++ type"});
				return nullptr;
			}
		}
		auto Allocation = std::make_shared<FGraphParameterAllocation>(
			Size, Alignment, Destroy);
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERDGResourceKind::Token;
		Resource.ValueTypeIdentity = TypeIdentity;
		Resource.ValueTypeName = StableTypeName;
		Resource.ValueStorageIndex = static_cast<uint32>(
			State->ValueStorage.Allocations.size());
		OutIndex = static_cast<uint32>(State->Resources.size());
		State->Resources.push_back(std::move(Resource));
		void* Data = Allocation->Data;
		State->ValueStorage.Allocations.push_back(std::move(Allocation));
		return Data;
	}

	auto FRDGBuilder::MarkValueStorageConstructed(uint32 ResourceIndex)
		-> void
	{
		if (ResourceIndex >= State->Resources.size()) return;
		const uint32 StorageIndex = State->Resources[ResourceIndex].ValueStorageIndex;
		if (StorageIndex < State->ValueStorage.Allocations.size())
			State->ValueStorage.Allocations[StorageIndex]->bConstructed = true;
	}

	auto FRDGBuilder::RegisterExternalTexture(
		const FTextureRHIRef& Texture, std::string_view Name,
		ERHIAccess InitialAccess, ERHIAccess FinalAccess)
		-> FRDGTextureHandle
	{
		RequireBuilding();
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERDGResourceKind::Texture;
		Resource.Texture = Texture;
		if (Texture) Resource.TextureDesc = DescribeTexture(*Texture);
		Resource.InitialAccess = InitialAccess;
		Resource.FinalAccess = FinalAccess;
		Resource.bExternal = true;
		if (Texture)
		{
			const auto Existing = State->ExternalResources.find(Texture.GetReference());
			if (Existing != State->ExternalResources.end())
			{
				const uint32 ExistingIndex = Existing->second;
				const auto& Canonical = State->Resources[ExistingIndex];
				if (!ExternalContractsEqual(Canonical, Resource))
					State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
						"conflicting external physical resource: canonical '"
						+ Canonical.Name + "' (" + DescribeExternalContract(Canonical)
						+ ") conflicts with '" + std::string(Name) + "' ("
						+ DescribeExternalContract(Resource) + ")"});
				return {State->Owner, ExistingIndex};
			}
		}
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		State->Resources.push_back(std::move(Resource));
		if (Texture) State->ExternalResources.emplace(Texture.GetReference(), Index);
		return {State->Owner, Index};
	}

	auto FRDGBuilder::CreateTexture(
		const FRDGTextureDesc& Desc, std::string_view Name,
		ERHIAccess FinalAccess) -> FRDGTextureHandle
	{
		RequireBuilding();
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERDGResourceKind::Texture;
		Resource.TextureDesc = Desc.Texture;
		Resource.ObservationTag = Desc.ObservationTag;
		Resource.FinalAccess = FinalAccess;
		State->Resources.push_back(std::move(Resource));
		return {State->Owner, Index};
	}

	auto FRDGBuilder::RegisterExternalBuffer(const FBufferRHIRef& Buffer,
		std::string_view Name, ERHIAccess InitialAccess,
		ERHIAccess FinalAccess) -> FRDGBufferHandle
	{
		RequireBuilding();
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERDGResourceKind::Buffer;
		Resource.Buffer = Buffer;
		if (Buffer) Resource.BufferDesc = Buffer->GetDesc();
		Resource.InitialAccess = InitialAccess;
		Resource.FinalAccess = FinalAccess;
		Resource.bExternal = true;
		if (Buffer)
		{
			const auto Existing = State->ExternalResources.find(Buffer.GetReference());
			if (Existing != State->ExternalResources.end())
			{
				const uint32 ExistingIndex = Existing->second;
				const auto& Canonical = State->Resources[ExistingIndex];
				if (!ExternalContractsEqual(Canonical, Resource))
					State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
						"conflicting external physical resource: canonical '"
						+ Canonical.Name + "' (" + DescribeExternalContract(Canonical)
						+ ") conflicts with '" + std::string(Name) + "' ("
						+ DescribeExternalContract(Resource) + ")"});
				return {State->Owner, ExistingIndex};
			}
		}
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		State->Resources.push_back(std::move(Resource));
		if (Buffer) State->ExternalResources.emplace(Buffer.GetReference(), Index);
		return {State->Owner, Index};
	}

	auto FRDGBuilder::CreateBuffer(const FRDGBufferDesc& Desc,
		std::string_view Name, ERHIAccess FinalAccess)
		-> FRDGBufferHandle
	{
		RequireBuilding();
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERDGResourceKind::Buffer;
		Resource.BufferDesc = Desc.Buffer;
		Resource.ObservationTag = Desc.ObservationTag;
		Resource.FinalAccess = FinalAccess;
		State->Resources.push_back(std::move(Resource));
		return {State->Owner, Index};
	}

	auto FRDGBuilder::CreateToken(std::string_view Name)
		-> FRDGTokenHandle
	{
		RequireBuilding();
		const uint32 Index = static_cast<uint32>(State->Resources.size());
		FGraphResource Resource;
		Resource.Name = Name;
		Resource.Kind = ERDGResourceKind::Token;
		State->Resources.push_back(std::move(Resource));
		return {State->Owner, Index};
	}

	auto FRDGBuilder::QueueTextureExtraction(
		FRDGTextureHandle Texture, FTextureRHIRef* Destination,
		ERHIAccess FinalAccess) -> void
	{
		RequireBuilding();
		if (Texture.Owner != State->Owner || Texture.Index >= State->Resources.size()
			|| State->Resources[Texture.Index].Kind
				!= ERDGResourceKind::Texture)
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				"texture extraction uses a foreign or invalid handle"});
			return;
		}
		if (Destination == nullptr || FinalAccess == ERHIAccess::None
			|| EnumHasAnyFlags(FinalAccess, ERHIAccess::Discard))
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				"texture extraction requires a destination and valid final access"});
			return;
		}
		if (std::ranges::any_of(State->Resources,
			[&](const FGraphResource& Existing) {
				return (&Existing == &State->Resources[Texture.Index]
						&& Existing.IsExported())
					|| Existing.TextureDestination == Destination;
			}))
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				"duplicate or conflicting texture extraction"});
			return;
		}
		State->Resources[Texture.Index].FinalAccess = FinalAccess;
		State->Resources[Texture.Index].TextureDestination = Destination;
	}

	auto FRDGBuilder::QueueBufferExtraction(
		FRDGBufferHandle Buffer, FBufferRHIRef* Destination,
		ERHIAccess FinalAccess) -> void
	{
		RequireBuilding();
		if (Buffer.Owner != State->Owner || Buffer.Index >= State->Resources.size()
			|| State->Resources[Buffer.Index].Kind
				!= ERDGResourceKind::Buffer)
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				"buffer extraction uses a foreign or invalid handle"});
			return;
		}
		if (Destination == nullptr || FinalAccess == ERHIAccess::None
			|| EnumHasAnyFlags(FinalAccess, ERHIAccess::Discard))
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				"buffer extraction requires a destination and valid final access"});
			return;
		}
		if (std::ranges::any_of(State->Resources,
			[&](const FGraphResource& Existing) {
				return (&Existing == &State->Resources[Buffer.Index]
						&& Existing.IsExported())
					|| Existing.BufferDestination == Destination;
			}))
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				"duplicate or conflicting buffer extraction"});
			return;
		}
		State->Resources[Buffer.Index].FinalAccess = FinalAccess;
		State->Resources[Buffer.Index].BufferDestination = Destination;
	}

	auto FRDGBuilder::AddTestPass(std::string_view Name,
		ERDGPassType Type, FRDGPassExecute Execute)
		-> FRDGPassHandle
	{
		RequireBuilding();
		const uint32 Index = static_cast<uint32>(State->Passes.size());
		FGraphPass Pass;
		Pass.Name = Name;
		Pass.Type = Type;
		if (Execute)
			Pass.ParameterizedExecute = [Callback = std::move(Execute)](
				FRHICommandListImmediate& CommandList, const FRDGParameterResolver& Resolver) {
				Callback(CommandList, Resolver.Resources);
			};
		State->Passes.push_back(std::move(Pass));
		return {State->Owner, Index};
	}

	auto FRDGBuilder::AddParameterizedPass(std::string_view Name,
		ERDGPassType Type,
		const FRDGParameterLayout* Layout, void* Parameters, size_t AllocationIndex,
		std::shared_ptr<void> Lifetime,
		FRDGParameterizedPassExecute ParameterizedExecute)
		-> FRDGPassHandle
	{
		RequireBuilding();
		const FRDGParametersMetadata* Metadata = Layout != nullptr
			? Layout->Metadata : nullptr;
		const auto RootPrefix = [&] {
			const char* StructName = Metadata != nullptr
				&& Metadata->StructName != nullptr ? Metadata->StructName : "FParameters";
			return "pass '" + std::string(Name)
				+ "' parameter '" + StructName + "'";
		};
		auto* Allocation = AllocationIndex < State->ParameterStorage.Allocations.size()
			? State->ParameterStorage.Allocations[AllocationIndex].get() : nullptr;
		if (Parameters == nullptr || Lifetime == nullptr || Layout == nullptr
			|| Allocation == nullptr || Allocation != Lifetime.get()
			|| Allocation->Data != Parameters || Allocation->Layout != Layout
			|| !Allocation->bConstructed)
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				RootPrefix() + " has an invalid or foreign parameter allocation"});
			return {};
		}
		if (Allocation->bFrozen)
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				RootPrefix() + " was already submitted"});
			return {};
		}
		Allocation->bFrozen = true;

		FGraphPass ParameterizedPass;
		ParameterizedPass.Name = Name;
		ParameterizedPass.Type = Type;
		ParameterizedPass.ParameterizedExecute = std::move(ParameterizedExecute);
		ParameterizedPass.ParameterLayout = Layout;
		ParameterizedPass.Parameters = Parameters;

		std::vector<FOptionalAlias> OptionalAliases;
		const auto* Bytes = static_cast<const std::byte*>(Parameters);
		for (uint32 LayoutElementIndex = 0;
			LayoutElementIndex < Layout->Elements.size(); ++LayoutElementIndex)
		{
			const FRDGParameterLayoutElement& Element =
				Layout->Elements[LayoutElementIndex];
			const FRDGParameterMemberMetadata& Member =
				*Layout->Leaves[Element.LeafIndex].Metadata;
			const void* ElementData = Bytes + Element.Offset;
			const std::string& FieldPath = Element.FieldPath;
			if (Member.bOptional && Member.ReadOptionalValueAddress != nullptr)
			{
				const void* ValueAddress = Member.ReadOptionalValueAddress(ElementData);
				if (ValueAddress != nullptr)
				{
					const auto AliasOffset = static_cast<uint32>(
						static_cast<const std::byte*>(ValueAddress) - Bytes);
					OptionalAliases.emplace_back(
						AliasOffset, LayoutElementIndex);
				}
			}

			FLoweredParameterUse Lowered = LowerParameterUse(Member,
				ElementData, FieldPath, State->Owner, State->Resources,
				[&](FRDGTextureHandle Handle) {
					return Handle.Owner == State->Owner ? Handle.Index
						: std::numeric_limits<uint32>::max();
				},
				[&](FRDGBufferHandle Handle) {
					return Handle.Owner == State->Owner ? Handle.Index
						: std::numeric_limits<uint32>::max();
				},
				[&](FRDGTokenHandle Handle) {
					return Handle.Owner == State->Owner ? Handle.Index
						: std::numeric_limits<uint32>::max();
				});
			if (Lowered.bPresent)
				ParameterizedPass.Uses.push_back(std::move(Lowered.Use));
		}
		std::ranges::sort(OptionalAliases);
		ParameterizedPass.OptionalAliases = FOptionalAliasTable(OptionalAliases);

		if (auto UseError = ValidatePassDeclarations(ParameterizedPass, State->Resources);
			!UseError.IsSuccess())
		{
			State->DeclarationErrors.push_back(std::move(UseError));
			return {};
		}
		ParameterizedPass.bDeclarationsValidated = true;
		const uint32 Index = static_cast<uint32>(State->Passes.size());
		State->Passes.push_back(std::move(ParameterizedPass));
		return {State->Owner, Index};
	}

	auto FRDGBuilder::CanDeclareManualUse(FRDGPassHandle Pass,
		std::string_view InvalidHandleError) -> bool
	{
		RequireBuilding();
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size())
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				std::string(InvalidHandleError)});
			return false;
		}
		if (State->Passes[Pass.Index].ParameterLayout != nullptr)
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				"pass '"
				+ State->Passes[Pass.Index].Name
				+ "' uses parameter declarations and cannot accept manual uses"});
			return false;
		}
		return true;
	}

	auto FRDGBuilder::MarkPassRoot(FRDGPassHandle Pass,
		std::string_view Reason) -> void
	{
		RequireBuilding();
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size())
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				"root has an invalid pass handle"});
			return;
		}
		State->Passes[Pass.Index].bRoot = true;
		State->Passes[Pass.Index].RootReason = std::string(Reason);
	}

	auto FRDGBuilder::SetPassAsyncComputeEligible(FRDGPassHandle Pass, bool bEligible) -> void
	{
		RequireBuilding();
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size()
			|| (bEligible && State->Passes[Pass.Index].Type != ERDGPassType::Compute))
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				"async eligibility requires a compute pass owned by this graph"});
			return;
		}
		State->Passes[Pass.Index].bAsyncComputeEligible = bEligible;
	}

	auto FRDGBuilder::SetAsyncComputeEnabled(bool bEnabled) -> void
	{
		RequireBuilding();
		State->bAsyncComputeEnabled = bEnabled;
	}

	auto FRDGBuilder::EnablePassCulling() -> void
	{
		RequireBuilding();
		State->bEnableCulling = true;
	}

	auto FRDGBuilder::SetBudget(const FRDGBudget& Budget) -> void
	{
		RequireBuilding();
		State->Budget = Budget;
	}

	auto FRDGBuilder::AddPassDependency(FRDGPassHandle Producer,
		FRDGPassHandle Consumer) -> void
	{
		RequireBuilding();
		if (Consumer.Owner != State->Owner || Consumer.Index >= State->Passes.size())
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDependency,
				"dependency has an invalid consumer pass handle"});
			return;
		}
		State->Passes[Consumer.Index].Prerequisites.push_back(
			Producer.Owner == State->Owner && Producer.Index < State->Passes.size()
				? Producer.Index : std::numeric_limits<uint32>::max());
	}

	auto FRDGBuilder::DeclareTextureUse(FRDGPassHandle Pass,
		FRDGTextureHandle Texture, const FRHITextureSubresourceRange& Range,
		ERDGUse Use, ERHIAccess Access, bool bDiscard, bool bStore,
		bool bPassManagedTransition, ERHIAccess ResultAccess) -> void
	{
		if (!CanDeclareManualUse(Pass,
			"texture use has an invalid pass handle")) return;
		FGraphUse DeclaredUse;
		DeclaredUse.ResourceIndex = Texture.Owner == State->Owner ? Texture.Index
			: std::numeric_limits<uint32>::max();
		DeclaredUse.Kind = ERDGResourceKind::Texture;
		DeclaredUse.Use = Use;
		DeclaredUse.Access = Access;
		DeclaredUse.bDiscard = bDiscard;
		DeclaredUse.TextureRange = Range;
		DeclaredUse.bStore = bStore;
		DeclaredUse.bPassManagedTransition = bPassManagedTransition;
		DeclaredUse.ResultAccess = ResultAccess;
		State->Passes[Pass.Index].Uses.push_back(DeclaredUse);
	}

	auto FRDGBuilder::UseTexture(FRDGPassHandle Pass,
		FRDGTextureHandle Texture,
		const FRHITextureSubresourceRange& Range, ERDGUse Use,
		ERHIAccess Access, bool bDiscard) -> void
	{
		DeclareTextureUse(Pass, Texture, Range, Use, Access, bDiscard);
	}

	auto FRDGBuilder::UseBuffer(FRDGPassHandle Pass,
		FRDGBufferHandle Buffer, uint64 Offset, uint64 Size,
		ERDGUse Use, ERHIAccess Access, bool bDiscard) -> void
	{
		if (!CanDeclareManualUse(Pass,
			"buffer use has an invalid pass handle")) return;
		FGraphUse DeclaredUse;
		DeclaredUse.ResourceIndex = Buffer.Owner == State->Owner ? Buffer.Index
			: std::numeric_limits<uint32>::max();
		DeclaredUse.Kind = ERDGResourceKind::Buffer;
		DeclaredUse.Use = Use;
		DeclaredUse.Access = Access;
		DeclaredUse.bDiscard = bDiscard;
		DeclaredUse.BufferOffset = Offset;
		DeclaredUse.BufferSize = Size;
		State->Passes[Pass.Index].Uses.push_back(DeclaredUse);
	}

	auto FRDGBuilder::UseColorAttachment(FRDGPassHandle Pass,
		FRDGTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction) -> void
	{
		DeclareTextureUse(Pass, Texture, Range, ERDGUse::ReadWrite,
			ERHIAccess::ColorAttachmentReadWrite,
			LoadAction != ERHIRenderTargetLoadAction::Load,
			StoreAction == ERHIRenderTargetStoreAction::Store);
	}

	auto FRDGBuilder::UseDepthStencilAttachment(
		FRDGPassHandle Pass, FRDGTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction) -> void
	{
		DeclareTextureUse(Pass, Texture, Range, ERDGUse::ReadWrite,
			ERHIAccess::DepthStencilReadWrite,
			LoadAction != ERHIRenderTargetLoadAction::Load,
			StoreAction == ERHIRenderTargetStoreAction::Store);
	}

	auto FRDGBuilder::UseManagedColorAttachment(
		FRDGPassHandle Pass, FRDGTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction, ERHIAccess ResultAccess) -> void
	{
		DeclareTextureUse(Pass, Texture, Range, ERDGUse::ReadWrite,
			ERHIAccess::ColorAttachmentReadWrite,
			LoadAction != ERHIRenderTargetLoadAction::Load,
			StoreAction == ERHIRenderTargetStoreAction::Store, true, ResultAccess);
	}

	auto FRDGBuilder::UseManagedDepthStencilAttachment(
		FRDGPassHandle Pass, FRDGTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction, ERHIAccess ResultAccess) -> void
	{
		DeclareTextureUse(Pass, Texture, Range, ERDGUse::ReadWrite,
			ERHIAccess::DepthStencilReadWrite,
			LoadAction != ERHIRenderTargetLoadAction::Load,
			StoreAction == ERHIRenderTargetStoreAction::Store, true, ResultAccess);
	}

	auto FRDGBuilder::UseManagedTexture(FRDGPassHandle Pass,
		FRDGTextureHandle Texture,
		const FRHITextureSubresourceRange& Range, ERDGUse Use,
		ERHIAccess EntryAccess, ERHIAccess ResultAccess, bool bDiscard) -> void
	{
		DeclareTextureUse(Pass, Texture, Range, Use, EntryAccess, bDiscard,
			true, true, ResultAccess);
	}

	auto FRDGBuilder::UseToken(FRDGPassHandle Pass,
		FRDGTokenHandle Token, ERDGUse Use) -> void
	{
		if (!CanDeclareManualUse(Pass,
			"token use has an invalid pass handle")) return;
		FGraphUse DeclaredUse;
		DeclaredUse.ResourceIndex = Token.Owner == State->Owner ? Token.Index
			: std::numeric_limits<uint32>::max();
		DeclaredUse.Kind = ERDGResourceKind::Token;
		DeclaredUse.Use = Use;
		DeclaredUse.bDiscard = Use != ERDGUse::Read;
		State->Passes[Pass.Index].Uses.push_back(DeclaredUse);
	}

	auto FRDGBuilder::UseValueErased(FRDGPassHandle Pass,
		uint64 Owner, uint32 Index, const void* TypeIdentity,
		ERDGUse Use) -> void
	{
		if (!CanDeclareManualUse(Pass,
			"typed value use has an invalid pass handle")) return;
		if (Use != ERDGUse::Read && Use != ERDGUse::Write)
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				"pass '"
				+ State->Passes[Pass.Index].Name
				+ "' declares a typed value with invalid read/write direction"});
			return;
		}
		if (Owner != State->Owner || Index >= State->Resources.size()
			|| State->Resources[Index].ValueTypeIdentity == nullptr
			|| State->Resources[Index].ValueTypeIdentity != TypeIdentity)
		{
			State->DeclarationErrors.push_back({ERDGError::InvalidDeclaration,
				"pass '"
				+ State->Passes[Pass.Index].Name
				+ "' declares an invalid, foreign, or wrongly typed graph value"});
			return;
		}
		FGraphUse DeclaredUse;
		DeclaredUse.ResourceIndex = Index;
		DeclaredUse.Kind = ERDGResourceKind::Token;
		DeclaredUse.Use = Use;
		DeclaredUse.bDiscard = Use == ERDGUse::Write;
		State->Passes[Pass.Index].Uses.push_back(std::move(DeclaredUse));
	}

	auto FRDGBuilder::Compile() -> FRDGResult
	{
		FScopedMicrosecondTimer CompileTimer(State->CompileMicroseconds);
		FScopedMicrosecondTimer ValidationTimer(State->Phases.ValidationMicroseconds);
		if (State->PendingConstructions != 0)
			return {ERDGError::InvalidState, "render graph storage construction is incomplete"};
		if (!State->DeclarationErrors.empty())
			return State->DeclarationErrors.front();
		if (State->Resources.size() > State->Budget.MaxResources)
			return SafetyLimit("resources", State->Resources.size(), State->Budget.MaxResources);
		size_t TotalUses = 0;
		size_t ExplicitDependencyCount = 0;
		for (const auto& Pass : State->Passes)
		{
			TotalUses += Pass.Uses.size();
			ExplicitDependencyCount += Pass.Prerequisites.size();
			if (TotalUses > State->Budget.MaxUses)
				return SafetyLimit("uses", TotalUses, State->Budget.MaxUses);
		}
		bool bHasExport = false;
		for (const auto& Resource : State->Resources)
			if (Resource.IsExported())
			{
				bHasExport = true;
				if (++TotalUses > State->Budget.MaxUses)
					return SafetyLimit("uses", TotalUses, State->Budget.MaxUses);
			}
		const size_t TotalPasses = State->Passes.size() + (bHasExport ? 1 : 0);
		if (TotalPasses > State->Budget.MaxPasses)
			return SafetyLimit("passes", TotalPasses, State->Budget.MaxPasses);
		if (auto Error = ValidateGraphResources(State->Resources);
			!Error.IsSuccess())
			return Error;

		FGraphPass Export;
		Export.Name = "RDG.Export";
		Export.bRoot = true;
		Export.bExport = true;
		Export.RootReason = "graph output";
		for (uint32 Index = 0; Index < State->Resources.size(); ++Index)
		{
			const auto& Resource = State->Resources[Index];
			if (!Resource.IsExported()) continue;
			FGraphUse Use;
			Use.ResourceIndex = Index;
			Use.Kind = Resource.Kind;
			Use.Access = Resource.FinalAccess;
			if (Resource.Kind == ERDGResourceKind::Texture)
				Use.TextureRange = {GetTextureAspects(Resource.TextureDesc.Format),
					0, Resource.TextureDesc.NumMips, 0, Resource.TextureDesc.ArraySize};
			else
				Use.BufferSize = Resource.BufferDesc.Size;
			Export.Uses.push_back(Use);
		}
		if (!Export.Uses.empty())
		{
			while (std::ranges::any_of(State->Passes, [&](const FGraphPass& Pass) {
				return Pass.Name == Export.Name;
			}))
				Export.Name += ".Output";
		}

		const FGraphPassView Passes{State->Passes, bHasExport ? &Export : nullptr};
		const uint32 PassCount = static_cast<uint32>(Passes.size());
		const uint32 ResourceCount = static_cast<uint32>(State->Resources.size());
		FDependencyGraph DependencyGraph{
			.MaxDependencies = State->Budget.MaxDependencies,
		};
		DependencyGraph.Dependencies.reserve(
			std::min<size_t>(ExplicitDependencyCount + TotalUses,
				State->Budget.MaxDependencies));
		if (auto Error = ValidateGraphPasses(Passes,
			State->Resources, DependencyGraph); !Error.IsSuccess())
			return Error;

		const FResourceUseTable ResourceUses = BuildResourceUseTable(
			Passes, ResourceCount);
		if (auto Error = ValidateTypedValueWriters(State->Resources,
			ResourceUses); !Error.IsSuccess())
			return Error;

		ValidationTimer.Stop();
		FScopedMicrosecondTimer RangeTimer(State->Phases.RangeMicroseconds);
		FRangeWork Work{State->Budget};
		FTrackingLayout Cells;
		if (auto Error = BuildTrackingLayout(State->Resources, ResourceUses, Work, Cells);
			!Error.IsSuccess()) return Error;

		RangeTimer.Stop();
		FScopedMicrosecondTimer DependencyTimer(State->Phases.DependencyMicroseconds);
		if (auto Error = BuildHazardDependencies(Passes,
			State->Resources, Cells, DependencyGraph, Work); !Error.IsSuccess())
			return Error;

		DependencyTimer.Stop();
		FScopedMicrosecondTimer CullingTimer(State->Phases.CullingMicroseconds);
		const std::vector<bool> Retained = FindRetainedPasses(Passes,
			DependencyGraph.Dependencies,
			State->bEnableCulling);

		CullingTimer.Stop();
		FScopedMicrosecondTimer PlanTimer(State->Phases.PlanMicroseconds);
		auto CompiledState = std::make_unique<FRDGBuilder::FCompiledState>();
		CompiledState->Owner = State->Owner;
		CompiledState->Resources = State->Resources;
		CompiledState->Backings.resize(ResourceCount);
		for (uint32 Index = 0; Index < ResourceCount; ++Index)
		{
			CompiledState->Backings[Index].Texture = State->Resources[Index].Texture;
			CompiledState->Backings[Index].Buffer = State->Resources[Index].Buffer;
		}
		CompiledState->Budget = State->Budget;
		CompiledState->Passes.reserve(PassCount);
		CompiledState->RuntimePasses.reserve(PassCount);
		CompiledState->Dependencies.reserve(
			DependencyGraph.Dependencies.size());
		CompiledState->ResourceLifetimes.reserve(ResourceCount);
		CompiledState->AllocationRequests.reserve(ResourceCount);
		for (const auto& Edge : DependencyGraph.Dependencies)
			if (Retained[Edge.BeforePass] && Retained[Edge.AfterPass])
				CompiledState->Dependencies.push_back(Edge);
		for (uint32 ResourceIndex = 0; ResourceIndex < State->Resources.size(); ++ResourceIndex)
		{
			const auto& Resource = State->Resources[ResourceIndex];
			CompiledState->ResourceLifetimes.push_back({Resource.Name, std::numeric_limits<uint32>::max(), 0, Resource.bExternal, true});
		}

		std::vector<uint32> LastResourcePass(ResourceCount, std::numeric_limits<uint32>::max());
		size_t BufferTransitionCount = 0;
		size_t TextureTransitionCount = 0;
		for (uint32 ScheduledIndex = 0; ScheduledIndex < PassCount; ++ScheduledIndex)
		{
			if (!Retained[ScheduledIndex]) continue;
			const auto& Pass = Passes[ScheduledIndex];
			const uint32 CompiledPassIndex = static_cast<uint32>(CompiledState->Passes.size());
			FRDGCompiledPass CompiledPass{.Name = Pass.Name, .Type = Pass.Type,
				.DeclarationIndex = ScheduledIndex,
				.ParameterStructName = Pass.ParameterLayout != nullptr
					&& Pass.ParameterLayout->Metadata->StructName != nullptr
					? Pass.ParameterLayout->Metadata->StructName : ""};
			FRDGBuilder::FCompiledState::FCompiledPassRuntime Runtime{
				.ParameterizedExecute = ScheduledIndex < State->Passes.size()
					? &State->Passes[ScheduledIndex].ParameterizedExecute : nullptr,
				.ParameterLayout = Pass.ParameterLayout,
				.Parameters = Pass.Parameters,
				.OptionalAliases = Pass.OptionalAliases.View()};
			Runtime.ResourceIndices.reserve(Pass.Uses.size());
			size_t ValueUseCount = 0;
			size_t BufferUseCount = 0;
			size_t TextureUseCount = 0;
			for (const auto& Use : Pass.Uses)
			{
				if (State->Resources[Use.ResourceIndex].ValueTypeIdentity != nullptr)
					++ValueUseCount;
				if (Use.Kind == ERDGResourceKind::Buffer) ++BufferUseCount;
				else if (Use.Kind == ERDGResourceKind::Texture) ++TextureUseCount;
			}
			Runtime.ValueUses.reserve(ValueUseCount);
			CompiledPass.Barriers.Reserve(BufferUseCount, TextureUseCount);
			for (const auto& Use : Pass.Uses)
			{
				if (LastResourcePass[Use.ResourceIndex] != CompiledPassIndex)
				{
					LastResourcePass[Use.ResourceIndex] = CompiledPassIndex;
					Runtime.ResourceIndices.push_back(Use.ResourceIndex);
				}
				if (State->Resources[Use.ResourceIndex].ValueTypeIdentity != nullptr)
					Runtime.ValueUses.emplace_back(Use.ResourceIndex, Use.Use);
				auto& Lifetime = CompiledState->ResourceLifetimes[Use.ResourceIndex];
				Lifetime.FirstPass = std::min(Lifetime.FirstPass, CompiledPassIndex);
				Lifetime.LastPass = CompiledPassIndex;
				Lifetime.bCulled = false;
			}
			CompiledState->Passes.push_back(std::move(CompiledPass));
			CompiledState->RuntimePasses.push_back(std::move(Runtime));
		}

		auto& Execution = CompiledState->ExecutionPlan;
		const uint32 ScheduledCount = static_cast<uint32>(CompiledState->Passes.size());
		std::vector<uint32> DeclarationToSubmission(PassCount, UINT32_MAX);
		for (uint32 Index = 0; Index < ScheduledCount; ++Index)
			DeclarationToSubmission[CompiledState->Passes[Index].DeclarationIndex] = Index;
		std::vector<std::array<uint32, 2>> RangeUsers(Cells.Ranges.size(), {UINT32_MAX, UINT32_MAX});
		const auto TransitionError = TraverseExecutionStates(Cells, State->Resources,
			Passes, CompiledState->Passes, CompiledState->ResourceLifetimes, &Work, State->bAsyncComputeEnabled,
			[&](const FRDGTransitionCapture& Event, size_t CellIndex) -> FRDGResult
			{
				if (Event.Kind != ERDGTransitionKind::RHIBarrier) return {};
				const auto& Resource = State->Resources[Event.ResourceId];
				auto& Barriers = Event.bFinal ? CompiledState->FinalBarriers
					: CompiledState->Passes[Event.PassIndex].Barriers;
				FRDGResourceHandoff Handoff{Event.ResourceId,
					{Event.bFinal ? ScheduledCount : Event.PassIndex},
					static_cast<uint32>(Resource.Kind == ERDGResourceKind::Texture
						? Barriers.GetTextureTransitions().size() : Barriers.GetBufferTransitions().size()),
					Resource.Kind == ERDGResourceKind::Texture};
				Handoff.SourceQueue = Event.SourceQueue;
				for (uint32 Producer : RangeUsers[CellIndex])
					if (Producer != UINT32_MAX && Producer != Handoff.Consumer.Index)
						Handoff.Producers.push_back({Producer});
				Execution.Handoffs.push_back(std::move(Handoff));
				if (Resource.Kind == ERDGResourceKind::Texture)
				{
					if (++TextureTransitionCount > State->Budget.MaxTextureTransitions)
						return SafetyLimit("texture-transitions", TextureTransitionCount, State->Budget.MaxTextureTransitions);
					Barriers.AddTransition(FRDGTextureTransition{Event.ResourceId, Event.TextureRange,
						Event.Before, Event.After, Event.bDiscardContents});
				}
				else
				{
					if (++BufferTransitionCount > State->Budget.MaxBufferTransitions)
						return SafetyLimit("buffer-transitions", BufferTransitionCount, State->Budget.MaxBufferTransitions);
					Barriers.AddTransition(FRDGBufferTransition{Event.ResourceId, Event.BufferOffset,
						Event.BufferSize, Event.Before, Event.After, Event.bDiscardContents});
				}
				return {};
			}, [&](uint32 Declaration, const FGraphUse&, size_t CellIndex, const FRangeCell&, bool) {
				const bool bAsync = State->bAsyncComputeEnabled && Passes[Declaration].bAsyncComputeEligible;
				RangeUsers[CellIndex][bAsync ? 1 : 0] = DeclarationToSubmission[Declaration];
			});
		if (!TransitionError.IsSuccess()) return TransitionError;

		Execution.Batches.reserve(ScheduledCount + (ScheduledCount != 0));
		for (uint32 Index = 0; Index < ScheduledCount; ++Index)
		{
			const auto Declaration = CompiledState->Passes[Index].DeclarationIndex;
			const bool bAsync = State->bAsyncComputeEnabled && Passes[Declaration].bAsyncComputeEligible;
			Execution.Batches.push_back({.Id = {Index},
				.Queue = bAsync ? ERDGQueueAssignment::AsyncCompute : ERDGQueueAssignment::Graphics,
				.FirstPass = Index, .NumPasses = 1});
		}
		if (ScheduledCount != 0 || !CompiledState->FinalBarriers.GetBufferTransitions().empty()
			|| !CompiledState->FinalBarriers.GetTextureTransitions().empty())
			Execution.Batches.push_back({.Id = {ScheduledCount},
				.FirstPass = ScheduledCount, .bEpilogue = true});
		Execution.Dependencies.reserve(CompiledState->Dependencies.size() + ScheduledCount);
		for (const auto& Edge : CompiledState->Dependencies)
		{
			const uint32 Before = DeclarationToSubmission[Edge.BeforePass];
			const uint32 After = DeclarationToSubmission[Edge.AfterPass];
			require(Before != UINT32_MAX && After != UINT32_MAX && Before < After);
			Execution.Dependencies.push_back({{Before}, {After}, Edge.Kind, Edge.Cause});
		}
		// Preserve FIFO within each logical queue without serializing independent
		// branches. Publication joins both terminal queue prefixes.
		std::array<uint32, 2> QueueTails{UINT32_MAX, UINT32_MAX};
		for (const auto& Batch : Execution.Batches)
		{
			auto& Tail = QueueTails[static_cast<size_t>(Batch.Queue)];
			if (Tail != UINT32_MAX)
				Execution.Dependencies.push_back({{Tail}, Batch.Id, ERDGDependencyKind::Execution, "queue-order"});
			if (Batch.bEpilogue && QueueTails[1] != UINT32_MAX)
				Execution.Dependencies.push_back({{QueueTails[1]}, Batch.Id, ERDGDependencyKind::Execution, "queue-join"});
			Tail = Batch.Id.Index;
		}
		for (const auto& Handoff : Execution.Handoffs)
			for (const auto Producer : Handoff.Producers)
				if (Execution.Batches[Producer.Index].Queue != Execution.Batches[Handoff.Consumer.Index].Queue)
					Execution.Dependencies.push_back({Producer, Handoff.Consumer,
						ERDGDependencyKind::Execution, "resource-handoff"});

		for (uint32 ResourceIndex = 0; ResourceIndex < State->Resources.size(); ++ResourceIndex)
		{
			const auto& Resource = State->Resources[ResourceIndex];
			const auto& Lifetime = CompiledState->ResourceLifetimes[ResourceIndex];
			if (!Lifetime.bCulled && !Resource.bExternal
				&& Resource.Kind != ERDGResourceKind::Token)
			{
				CompiledState->AllocationRequests.push_back({
					.ResourceId = ResourceIndex,
					.Kind = Resource.Kind,
					.TextureDesc = Resource.TextureDesc,
					.BufferDesc = Resource.BufferDesc,
					.FirstPass = Lifetime.FirstPass,
					.LastPass = Lifetime.LastPass,
					.ObservationTag = Resource.ObservationTag,
					.bExtracted = Resource.IsExported()});
			}
		}
		std::ranges::sort(CompiledState->Dependencies,
			[](const auto& A, const auto& B) {
				return std::tie(A.BeforePass, A.AfterPass, A.Cause)
					< std::tie(B.BeforePass, B.AfterPass, B.Cause);
			});
		CompiledState->Retained = Retained;
		if (bHasExport)
			CompiledState->ExportPass = std::move(Export);
		Diagnostics.reset();
		Compiled = std::move(CompiledState);
		State->bCompiled = true;
		return {};
	}

	auto FRDGBuilder::EnsureDiagnostics() const -> void
	{
		if (Diagnostics) return;
		auto Result = std::make_unique<FDiagnostics>();
		if (!State->bCompiled)
		{
			Diagnostics = std::move(Result);
			return;
		}
		const FGraphPassView Passes{State->Passes,
			Compiled->Retained.size() > State->Passes.size() ? &Compiled->ExportPass : nullptr};
		const auto ResourceUses = BuildResourceUseTable(Passes,
			static_cast<uint32>(Compiled->Resources.size()));
		for (uint32 Index = 0; Index < Compiled->Retained.size(); ++Index)
		{
			const auto& Pass = Passes[Index];
			Result->CullingDecisions.push_back({Pass.Name, !Compiled->Retained[Index],
				Compiled->Retained[Index] ? (Pass.bRoot ? Pass.RootReason : "value dependency")
					: "unreachable from an explicit root"});
			if (Pass.ParameterLayout == nullptr) continue;
			size_t UseIndex = 0;
			for (const auto& Element : Pass.ParameterLayout->Elements)
			{
				const auto& Member = *Pass.ParameterLayout->Leaves[Element.LeafIndex].Metadata;
				// Uses are frozen in layout order. Never reread mutable parameter payloads.
				const bool bPresent = UseIndex < Pass.Uses.size()
					&& Pass.Uses[UseIndex].ParameterPath == Element.FieldPath;
				const FGraphUse Use = bPresent ? Pass.Uses[UseIndex++]
					: MakeParameterUse(Member, Element.FieldPath);
				Result->Parameters.push_back({
					.PassDeclarationIndex = Index,
					.FieldPath = Element.FieldPath,
					.Kind = Member.Kind,
					.ResourceKind = Member.ResourceKind,
					.bPresent = bPresent,
					.ResourceId = bPresent ? Use.ResourceIndex : std::numeric_limits<uint32>::max(),
					.Use = Use.Use,
					.Access = Use.Access,
					.TextureRange = Use.TextureRange,
					.BufferOffset = Use.BufferOffset,
					.BufferSize = Use.BufferSize,
					.bDiscard = Use.bDiscard,
					.bStore = Use.bStore,
					.bPassManagedTransition = Use.bPassManagedTransition,
					.ResultAccess = Use.ResultAccess,
					.ShaderBindingName = std::string(Use.ShaderBindingName),
					.ShaderBindingType = Use.ShaderBindingType});
			}
		}
		for (uint32 Index = 0; Index < Compiled->Resources.size(); ++Index)
		{
			const auto& Resource = Compiled->Resources[Index];
			const auto& Lifetime = Compiled->ResourceLifetimes[Index];
			Result->Resources.push_back({Index, Resource.Name, Resource.Kind, Resource.bExternal, "unused"});
			auto& Capture = Result->Resources.back();
			Capture.ValueType = Resource.ValueTypeName;
			Capture.TextureFormat = Resource.TextureDesc.Format;
			Capture.TextureExtent = Resource.TextureDesc.Extent;
			Capture.TextureArraySize = Resource.TextureDesc.ArraySize;
			Capture.TextureMips = Resource.TextureDesc.NumMips;
			Capture.BufferSize = Resource.BufferDesc.Size;
			Capture.BufferStride = Resource.BufferDesc.Stride;
			Capture.Preparation = Lifetime.bCulled ? "culled"
				: Resource.bExternal ? "external"
				: Resource.Kind == ERDGResourceKind::Token ? "logical" : "requested";
			Capture.AllocationDisposition = Lifetime.bCulled ? "culled"
				: Resource.bExternal ? "external"
				: Resource.Kind == ERDGResourceKind::Token ? "none"
				: Compiled->Backings[Index].AllocationDisposition.empty() ? "pending"
				: Compiled->Backings[Index].AllocationDisposition;
			Capture.PhysicalAllocationId = Compiled->Backings[Index].PhysicalAllocationId;
		}

		// Reconstruct the same deterministic layout only on explicit inspection.
		// This does not schedule, allocate, execute, or alter the compiled plan.
		FRangeWork Work{Compiled->Budget};
		FTrackingLayout Cells;
		const auto Error = BuildTrackingLayout(Compiled->Resources, ResourceUses, Work, Cells);
		requiref(Error.IsSuccess(), "compiled RDG diagnostic layout failed: {}", Error.Message);
		std::vector<uint32> Versions(Cells.Ranges.size(), 0);
		std::vector<uint32> VersionPasses(Cells.Ranges.size(), std::numeric_limits<uint32>::max());
		const auto VisitError = TraverseExecutionStates(Cells, Compiled->Resources,
			Passes, Compiled->Passes, Compiled->ResourceLifetimes, nullptr, State->bAsyncComputeEnabled,
			[&](const FRDGTransitionCapture& Event, size_t) -> FRDGResult
			{
				Result->Transitions.push_back(Event);
				return {};
			}, [&](uint32 DeclarationIndex, const FGraphUse& Use, size_t CellIndex, const FRangeCell& Cell, bool bWrites)
			{
				if (bWrites && VersionPasses[CellIndex] != DeclarationIndex)
				{
					++Versions[CellIndex];
					VersionPasses[CellIndex] = DeclarationIndex;
				}
				Result->Uses.push_back({DeclarationIndex, Use.ResourceIndex,
					Use.Use, Use.Access, Cell.TextureRange, Use.BufferOffset,
					Use.BufferSize, Versions[CellIndex], Use.bDiscard, Use.bStore,
					std::string(Use.ParameterPath), std::string(Use.ShaderBindingName),
					Use.ShaderBindingType});
			});
		requiref(VisitError.IsSuccess(), "compiled RDG diagnostic traversal failed: {}", VisitError.Message);
		Diagnostics = std::move(Result);
	}

	auto FRDGBuilder::GetPasses() const
		-> std::span<const FRDGCompiledPass> { return Compiled->Passes; }
	auto FRDGBuilder::GetDependencies() const
		-> std::span<const FRDGDependency> { return Compiled->Dependencies; }
	auto FRDGBuilder::GetResourceLifetimes() const
		-> std::span<const FRDGResourceLifetime>
	{
		return Compiled->ResourceLifetimes;
	}
	auto FRDGBuilder::GetCullingDecisions() const
		-> std::span<const FRDGCullingDecision>
	{
		EnsureDiagnostics();
		return Diagnostics->CullingDecisions;
	}
	auto FRDGBuilder::GetFinalBarriers() const -> const FRDGBarrierBatch&
	{ return Compiled->FinalBarriers; }
	auto FRDGBuilder::GetExecutionPlan() const -> const FRDGExecutionPlan&
	{ return Compiled->ExecutionPlan; }

	auto FRDGBuilder::GetSubmissionReceipts() const -> std::span<const FRHIGPUSubmissionReceipt>
	{ return State->SubmissionReceipts; }

	auto FRDGBuilder::GetCompileMicroseconds() const -> uint64
	{
		return State->CompileMicroseconds;
	}

	auto FRDGBuilder::GetBudget() const -> const FRDGBudget&
	{
		return State->Budget;
	}

	auto FRDGBuilder::GetStatistics() const -> FRDGStatistics
	{
		FRDGStatistics Result;
		Result.DeclaredPasses = static_cast<uint32>(Compiled->Retained.size());
		Result.ScheduledPasses = static_cast<uint32>(Compiled->Passes.size());
		Result.CulledPasses = Result.DeclaredPasses - Result.ScheduledPasses;
		Result.Dependencies = static_cast<uint32>(Compiled->Dependencies.size());
		Result.BufferTransitions = static_cast<uint32>(
			Compiled->FinalBarriers.GetBufferTransitions().size());
		Result.TextureTransitions = static_cast<uint32>(
			Compiled->FinalBarriers.GetTextureTransitions().size());
		for (const auto& Pass : Compiled->Passes)
		{
			Result.BufferTransitions += static_cast<uint32>(Pass.Barriers.GetBufferTransitions().size());
			Result.TextureTransitions += static_cast<uint32>(Pass.Barriers.GetTextureTransitions().size());
		}
		Result.Phases = State->Phases;
		Result.CompileMicroseconds = State->CompileMicroseconds;
		Result.ExecuteMicroseconds =
			State->ExecuteMicroseconds;
		Result.bPassRegressionBudgetExceeded = Result.DeclaredPasses
			> Compiled->Budget.RegressionMaxPasses;
		Result.bDependencyRegressionBudgetExceeded = Result.Dependencies
			> Compiled->Budget.RegressionMaxDependencies;
		Result.bBufferTransitionRegressionBudgetExceeded = Result.BufferTransitions
			> Compiled->Budget.RegressionMaxBufferTransitions;
		Result.bTextureTransitionRegressionBudgetExceeded = Result.TextureTransitions
			> Compiled->Budget.RegressionMaxTextureTransitions;
		Result.bCompileBudgetExceeded = Result.CompileMicroseconds
			> GetBudget().MaxCompileMicroseconds;
		Result.bExecuteBudgetExceeded = Result.ExecuteMicroseconds
			> GetBudget().MaxExecuteMicroseconds;
		return Result;
	}

	auto FRDGBuilder::Capture() const -> FRDGCapture
	{
		EnsureDiagnostics();
		FRDGCapture Result;
		Result.bCompiled = State->bCompiled;
		Result.Budget = State->Budget;
		Result.Statistics = GetStatistics();
		Result.AllocationStatistics = Compiled->AllocationStatistics;
		Result.Resources = Diagnostics->Resources;
		Result.Parameters = Diagnostics->Parameters;
		Result.Uses = Diagnostics->Uses;
		Result.Transitions = Diagnostics->Transitions;
		Result.Dependencies = Compiled->Dependencies;
		Result.ResourceLifetimes = Compiled->ResourceLifetimes;
		Result.CullingDecisions = Diagnostics->CullingDecisions;
		Result.ExecutionPlan = Compiled->ExecutionPlan;
		Result.Dump = Dump();
		Result.Passes.reserve(Compiled->Passes.size());
		for (const auto& Pass : Compiled->Passes)
			Result.Passes.push_back({Pass.Name, Pass.Type, Pass.DeclarationIndex,
				Pass.ParameterStructName,
				static_cast<uint32>(Pass.Barriers.GetBufferTransitions().size()),
				static_cast<uint32>(Pass.Barriers.GetTextureTransitions().size())});
		return Result;
	}

	auto FRDGBuilder::Dump() const -> std::string
	{
		if (!State->bCompiled) return "render-graph: no compiled plan";
		EnsureDiagnostics();
		std::ostringstream Output;
		Output << "render-graph passes=" << Compiled->Passes.size()
			<< " edges=" << Compiled->Dependencies.size() << '\n';
		for (const auto& Batch : Compiled->ExecutionPlan.Batches)
			Output << "submission " << Batch.Id.Index << " queue="
				<< (Batch.Queue == ERDGQueueAssignment::Graphics ? "graphics" : "async-compute")
				<< " passes=" << Batch.FirstPass << '+' << Batch.NumPasses
				<< " epilogue=" << Batch.bEpilogue << '\n';
		for (const auto& Edge : Compiled->ExecutionPlan.Dependencies)
			Output << "submission-dependency " << Edge.Before.Index << " -> " << Edge.After.Index
				<< " kind=" << static_cast<uint32>(Edge.Kind) << " cause=" << Edge.Cause << '\n';
		for (const auto& Handoff : Compiled->ExecutionPlan.Handoffs)
		{
			Output << "handoff resource=" << Handoff.ResourceId << " submission=" << Handoff.Consumer.Index
				<< " texture=" << Handoff.bTexture << " transition=" << Handoff.TransitionIndex
				<< " source-queue=" << static_cast<uint32>(Handoff.SourceQueue) << " producers=";
			for (const auto Producer : Handoff.Producers) Output << Producer.Index << ',';
			Output << '\n';
		}
		Output << "allocation active-resources="
			<< Compiled->AllocationStatistics.ActiveResources
			<< " retained-resources="
			<< Compiled->AllocationStatistics.RetainedResources
			<< " active-bytes=" << Compiled->AllocationStatistics.ActiveBytes
			<< " retained-bytes=" << Compiled->AllocationStatistics.RetainedBytes
			<< " peak-active-bytes="
			<< Compiled->AllocationStatistics.PeakActiveBytes
			<< " hits=" << Compiled->AllocationStatistics.ReuseHits
			<< " misses=" << Compiled->AllocationStatistics.ReuseMisses
			<< " evictions=" << Compiled->AllocationStatistics.Evictions
			<< " failures=" << Compiled->AllocationStatistics.Failures << '\n';
		for (uint32 Index = 0; Index < Compiled->Passes.size(); ++Index)
		{
			const auto& Pass = Compiled->Passes[Index];
			Output << "pass " << Index << " decl=" << Pass.DeclarationIndex
				<< " type=" << PassTypeName(Pass.Type) << " name=" << Pass.Name
				<< " buffers=" << Pass.Barriers.GetBufferTransitions().size()
				<< " textures=" << Pass.Barriers.GetTextureTransitions().size();
			if (!Pass.ParameterStructName.empty())
				Output << " parameters=" << Pass.ParameterStructName;
			Output << '\n';
		}
		for (const auto& Edge : Compiled->Dependencies)
			Output << "edge " << Edge.BeforePass << "->" << Edge.AfterPass
				<< " kind=" << DependencyKindName(Edge.Kind)
				<< " cause=" << Edge.Cause << '\n';
		Output << "final buffers=" << Compiled->FinalBarriers.GetBufferTransitions().size()
			<< " textures=" << Compiled->FinalBarriers.GetTextureTransitions().size() << '\n';
		for (const auto& Lifetime : Compiled->ResourceLifetimes)
			Output << "lifetime name=" << Lifetime.Name << " first="
				   << Lifetime.FirstPass << " last=" << Lifetime.LastPass
				   << " external=" << Lifetime.bExternal
				   << " culled=" << Lifetime.bCulled << '\n';
		for (const auto& Decision : Diagnostics->CullingDecisions)
			Output << "culling name=" << Decision.Name << " culled="
				<< Decision.bCulled << " reason=" << Decision.Reason << '\n';
		for (const auto& Resource : Diagnostics->Resources)
			Output << "resource id=" << Resource.ResourceId << " name="
				   << Resource.Name << " kind=" << static_cast<uint32>(Resource.Kind)
				   << " external=" << Resource.bExternal << " preparation="
				   << Resource.Preparation << " allocation="
				   << Resource.AllocationDisposition << " allocation-id="
				   << Resource.PhysicalAllocationId << " value-type="
				   << Resource.ValueType << " format="
				   << static_cast<uint32>(Resource.TextureFormat) << " extent="
				   << Resource.TextureExtent.x << 'x' << Resource.TextureExtent.y
				   << " layers=" << Resource.TextureArraySize << " mips="
				   << static_cast<uint32>(Resource.TextureMips) << " buffer-size="
				   << Resource.BufferSize << " stride=" << Resource.BufferStride << '\n';
		for (const auto& Parameter : Diagnostics->Parameters)
		{
			Output << "parameter pass=" << Parameter.PassDeclarationIndex
				<< " field=" << Parameter.FieldPath << " kind="
				<< ParameterMemberKindName(Parameter.Kind) << " present="
				<< Parameter.bPresent << " resource=";
			if (Parameter.bPresent) Output << Parameter.ResourceId;
			else Output << "none";
			Output << " direction=" << GraphUseName(Parameter.Use)
				<< " access=" << static_cast<uint32>(Parameter.Access)
				<< " aspects="
				<< static_cast<uint32>(Parameter.TextureRange.Aspects) << " mip="
				<< Parameter.TextureRange.FirstMip << '+'
				<< Parameter.TextureRange.NumMips << " layer="
				<< Parameter.TextureRange.FirstArrayLayer << '+'
				<< Parameter.TextureRange.NumArrayLayers << " offset="
				<< Parameter.BufferOffset << " size=" << Parameter.BufferSize
				<< " discard=" << Parameter.bDiscard << " store="
				<< Parameter.bStore << " managed="
				<< Parameter.bPassManagedTransition << " result-access="
				<< static_cast<uint32>(Parameter.ResultAccess);
			if (!Parameter.ShaderBindingName.empty())
				Output << " shader-binding=" << Parameter.ShaderBindingName
					<< " binding-type="
					<< static_cast<uint32>(Parameter.ShaderBindingType);
			Output << '\n';
		}
		for (const auto& Use : Diagnostics->Uses)
		{
			Output << "use pass=" << Use.PassDeclarationIndex << " resource="
				<< Use.ResourceId << " direction=" << GraphUseName(Use.Use)
				<< " version=" << Use.Version << " access="
				<< static_cast<uint32>(Use.Access) << " aspects="
				<< static_cast<uint32>(Use.TextureRange.Aspects) << " mip="
				<< Use.TextureRange.FirstMip << '+' << Use.TextureRange.NumMips
				<< " layer=" << Use.TextureRange.FirstArrayLayer << '+'
				<< Use.TextureRange.NumArrayLayers << " offset=" << Use.BufferOffset
				<< " size=" << Use.BufferSize << " discard=" << Use.bDiscard
				<< " store=" << Use.bStore;
			if (!Use.ParameterPath.empty())
				Output << " field=" << Use.ParameterPath;
			if (!Use.ShaderBindingName.empty())
				Output << " shader-binding=" << Use.ShaderBindingName
					<< " binding-type="
					<< static_cast<uint32>(Use.ShaderBindingType);
			Output << '\n';
		}
		for (const auto& Transition : Diagnostics->Transitions)
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
				<< " discard=" << Transition.bDiscardContents
				<< " kind=" << (Transition.Kind == ERDGTransitionKind::RHIBarrier
					? "rhi-barrier" : "pass-managed")
				<< " final=" << Transition.bFinal << '\n';
		return Output.str();
	}

	auto FRDGBuilder::Record(
		FRHICommandListImmediate& CommandList, FRDGExecutionContext* Context) -> FRDGResult
	{
		FScopedMicrosecondTimer ExecuteTimer(State->ExecuteMicroseconds);
		FScopedMicrosecondTimer PreparationTimer(State->Phases.PreparationMicroseconds);
		if (Context != nullptr && !Compiled->AllocationRequests.empty())
		{
			FRDGAllocatedResources Candidate(
				static_cast<uint32>(Compiled->Resources.size()));
			std::string Error;
			std::span<const FRDGAllocationRequest> Requests = Compiled->AllocationRequests;
			std::vector<FRDGAllocationRequest> AsyncRequests;
			if (State->bAsyncComputeEnabled && GDynamicRHI
				&& GDynamicRHI->RHIGetQueueCapabilities().bIndependentCompute
				&& Context->Allocator.SupportsAsyncCompute())
			{
				State->AllocationRetirement = std::make_shared<FRDGAllocationRetirement>();
				AsyncRequests = Compiled->AllocationRequests;
				for (auto& Request : AsyncRequests) Request.Retirement = State->AllocationRetirement;
				Requests = AsyncRequests;
			}
			const bool bAllocated = Context->Allocator.Allocate(
				Requests, Candidate, Error);
			Compiled->AllocationStatistics = Candidate.Statistics;
			if (!bAllocated)
			{
				return {ERDGError::AllocationFailed, std::move(Error)};
			}
			for (const FRDGAllocationRequest& Request : Compiled->AllocationRequests)
			{
				const bool bReady = Request.Kind == ERDGResourceKind::Texture
					? static_cast<bool>(Candidate.Textures[Request.ResourceId])
					: static_cast<bool>(Candidate.Buffers[Request.ResourceId]);
				if (!bReady)
				{
					return {ERDGError::MissingAllocation, "RDG allocator omitted retained resource id="
						+ std::to_string(Request.ResourceId)};
				}
				if (Request.Kind == ERDGResourceKind::Texture)
				{
					const FRHITextureDesc Actual = DescribeTexture(
						*Candidate.Textures[Request.ResourceId]);
					if (!TextureBackingIsCompatible(Actual, Request.TextureDesc))
					{
						return {ERDGError::IncompatibleAllocation, "RDG allocator returned incompatible texture id="
							+ std::to_string(Request.ResourceId)};
					}
				}
				else if (!BufferBackingIsCompatible(
					Candidate.Buffers[Request.ResourceId]->GetDesc(),
					Request.BufferDesc))
				{
					return {ERDGError::IncompatibleAllocation, "RDG allocator returned incompatible buffer id="
						+ std::to_string(Request.ResourceId)};
				}
			}
			for (const FRDGAllocationRequest& Request : Compiled->AllocationRequests)
			{
				auto& Resource = Compiled->Backings[Request.ResourceId];
				Resource.AllocationDisposition =
					Candidate.AllocationDispositions[Request.ResourceId];
				Resource.PhysicalAllocationId =
					Candidate.AllocationIds[Request.ResourceId];
				if (Diagnostics)
				{
					auto& Capture = Diagnostics->Resources[Request.ResourceId];
					Capture.AllocationDisposition = Resource.AllocationDisposition;
					Capture.PhysicalAllocationId = Resource.PhysicalAllocationId;
				}
				if (Request.Kind == ERDGResourceKind::Texture)
					Resource.Texture = std::move(
						Candidate.Textures[Request.ResourceId]
					);
				else
					Resource.Buffer = std::move(
						Candidate.Buffers[Request.ResourceId]
					);
			}
		}
		else if (!Compiled->AllocationRequests.empty())
		{
			return {ERDGError::AllocationFailed, "retained graph resources require an RDG execution allocator"};
		}
		FPreparedTransitions PreparedTransitions;
		std::vector<FPreparedBarrierBatch> PreparedPassBarriers;
		PreparedPassBarriers.reserve(Compiled->Passes.size());
		for (const auto& Pass : Compiled->Passes)
			PreparedPassBarriers.push_back(PrepareBarrierBatch(Pass.Barriers, Compiled->Backings, PreparedTransitions));
		const auto PreparedEpilogue = PrepareBarrierBatch(Compiled->FinalBarriers, Compiled->Backings, PreparedTransitions);
		const auto* Queues = GDynamicRHI ? &GDynamicRHI->RHIGetQueueCapabilities() : nullptr;
		const bool bExplicitSubmissions = Queues && !Queues->Queues.empty();
		const bool bAsync = State->bAsyncComputeEnabled && bExplicitSubmissions
			&& Queues->bIndependentCompute && Queues->Compute != Queues->Graphics
			&& (Compiled->AllocationRequests.empty() || (Context && Context->Allocator.SupportsAsyncCompute()));
		auto PhysicalQueue = [&](ERDGQueueAssignment Queue) {
			return bAsync && Queue == ERDGQueueAssignment::AsyncCompute ? Queues->Compute : Queues->Graphics;
		};
		using FTransfers = std::vector<std::shared_ptr<FRHIQueueTransfer>>;
		std::vector<FTransfers> Acquires(Compiled->ExecutionPlan.Batches.size()), Releases(Acquires.size());
		FTransfers InitialReleases;
		std::vector<bool> WaitForInitial(Acquires.size(), false);
		if (bAsync)
		{
			PreparedTransitions.TransferredBuffers.resize(PreparedTransitions.Buffers.size(), false);
			PreparedTransitions.TransferredTextures.resize(PreparedTransitions.Textures.size(), false);
			for (const auto& Handoff : Compiled->ExecutionPlan.Handoffs)
			{
				const auto& Consumer = Compiled->ExecutionPlan.Batches[Handoff.Consumer.Index];
				if (Handoff.SourceQueue == Consumer.Queue) continue;
				FRHIQueueTransferDesc Desc{.Source = PhysicalQueue(Handoff.SourceQueue), .Destination = PhysicalQueue(Consumer.Queue)};
				const auto& Barrier = Consumer.bEpilogue ? PreparedEpilogue : PreparedPassBarriers[Consumer.FirstPass];
				if (Handoff.bTexture)
				{
					const size_t Index = Barrier.FirstTexture + Handoff.TransitionIndex;
					Desc.Textures.push_back(PreparedTransitions.Textures[Index]);
					PreparedTransitions.TransferredTextures[Index] = true;
				}
				else
				{
					const size_t Index = Barrier.FirstBuffer + Handoff.TransitionIndex;
					Desc.Buffers.push_back(PreparedTransitions.Buffers[Index]);
					PreparedTransitions.TransferredBuffers[Index] = true;
				}
				auto Transfer = GDynamicRHI->RHICreateQueueTransfer(Desc);
				if (!Transfer) return {ERDGError::AllocationFailed, "failed to prepare render graph queue transfer"};
				Acquires[Consumer.Id.Index].push_back(Transfer);
				const auto Producer = std::ranges::find_if(Handoff.Producers, [&](const auto Id) {
					return Compiled->ExecutionPlan.Batches[Id.Index].Queue == Handoff.SourceQueue;
				});
				if (Producer != Handoff.Producers.end()) Releases[Producer->Index].push_back(std::move(Transfer));
				else
				{
					require(Handoff.SourceQueue == ERDGQueueAssignment::Graphics);
					InitialReleases.push_back(std::move(Transfer));
					WaitForInitial[Consumer.Id.Index] = true;
				}
			}
		}
		std::vector<std::vector<uint32>> Predecessors(Compiled->ExecutionPlan.Batches.size());
		if (bExplicitSubmissions)
		{
			State->SubmissionReceipts.resize(Compiled->ExecutionPlan.Batches.size());
			for (const auto& Edge : Compiled->ExecutionPlan.Dependencies)
				Predecessors[Edge.After.Index].push_back(Edge.Before.Index);
			for (auto& Inputs : Predecessors)
			{
				std::ranges::sort(Inputs);
				Inputs.erase(std::unique(Inputs.begin(), Inputs.end()), Inputs.end());
			}
		}
		PreparationTimer.Stop();
		FScopedMicrosecondTimer RecordingTimer(State->Phases.RecordingMicroseconds);
		State->Lifecycle = ERDGBuilderState::Recording;
		State->ExecutionResult.Status = ERDGExecutionStatus::InvalidState;
		State->ExecutionResult.Result =
			{ERDGError::InvalidState, "render graph recording did not complete"};
		FRHIGPUSubmissionReceipt InitialSignal;
		if (!InitialReleases.empty())
		{
			InitialSignal = CommandList.BeginGPUSubmission({.Queue = Queues->Graphics});
			for (const auto& Transfer : InitialReleases) CommandList.ReleaseQueueOwnership(Transfer);
			CommandList.EndGPUSubmission();
		}
		for (const auto& Batch : Compiled->ExecutionPlan.Batches)
		{
			if (bExplicitSubmissions)
			{
				FRHIGPUSubmissionDesc Desc{.Queue = PhysicalQueue(Batch.Queue)};
				if (WaitForInitial[Batch.Id.Index]) Desc.Waits.push_back(InitialSignal);
				for (uint32 Input : Predecessors[Batch.Id.Index])
					Desc.Waits.push_back(State->SubmissionReceipts[Input]);
				State->SubmissionReceipts[Batch.Id.Index] = CommandList.BeginGPUSubmission(Desc);
			}
			struct FCloseSubmission
			{
				FRHICommandListImmediate& Commands;
				bool bEnabled;
				~FCloseSubmission() { if (bEnabled) Commands.EndGPUSubmission(); }
			} CloseSubmission{CommandList, bExplicitSubmissions};
			for (const auto& Transfer : Acquires[Batch.Id.Index]) CommandList.AcquireQueueOwnership(Transfer);
			if (Batch.bEpilogue)
			{
				RecordBarrierBatch(CommandList, PreparedEpilogue, PreparedTransitions);
				for (const auto& Transfer : Releases[Batch.Id.Index]) CommandList.ReleaseQueueOwnership(Transfer);
				continue;
			}
			for (uint32 Index = Batch.FirstPass; Index < Batch.FirstPass + Batch.NumPasses; ++Index)
			{
				const auto& Pass = Compiled->Passes[Index];
				const auto& Runtime = Compiled->RuntimePasses[Index];
				RecordBarrierBatch(CommandList, PreparedPassBarriers[Index], PreparedTransitions);
				if (Runtime.ParameterizedExecute != nullptr && *Runtime.ParameterizedExecute)
				{
					const FRDGPassResources Resources(*this, Index);
					const FRDGParameterResolver Resolver(Resources,
						Runtime.ParameterLayout, Runtime.OptionalAliases,
						Runtime.Parameters,
						Pass.Name, Pass.Type);
					(*Runtime.ParameterizedExecute)(CommandList, Resolver);
				}
			}
			for (const auto& Transfer : Releases[Batch.Id.Index]) CommandList.ReleaseQueueOwnership(Transfer);
		}
		if (State->AllocationRetirement)
		{
			require(!State->SubmissionReceipts.empty());
			State->AllocationRetirement->Completion = State->SubmissionReceipts.back();
		}
		for (uint32 Index = 0; Index < Compiled->Resources.size(); ++Index)
		{
			const auto& Resource = Compiled->Resources[Index];
			const auto& Backing = Compiled->Backings[Index];
			if (Resource.TextureDestination != nullptr)
				*Resource.TextureDestination = Backing.Texture;
			if (Resource.BufferDestination != nullptr)
				*Resource.BufferDestination = Backing.Buffer;
		}
		return {};
	}

	auto FRDGPassResources::GetTexture(
		FRDGTextureHandle Handle) const -> FRHITexture*
	{
		requiref(Handle.Owner == Graph.Compiled->Owner
			&& Handle.Index < Graph.Compiled->Resources.size(),
			"Render graph callback used an invalid texture handle.");
		requiref(PassIndex < Graph.Compiled->RuntimePasses.size()
			&& std::ranges::find(Graph.Compiled->RuntimePasses[PassIndex].ResourceIndices,
				Handle.Index) != Graph.Compiled->RuntimePasses[PassIndex].ResourceIndices.end(),
			"Render graph pass '{}' accessed undeclared texture resource {} ('{}').",
			PassIndex < Graph.Compiled->Passes.size()
				? Graph.Compiled->Passes[PassIndex].Name : "<invalid>", Handle.Index,
			Handle.Index < Graph.Compiled->Resources.size()
				? Graph.Compiled->Resources[Handle.Index].Name : "<invalid>");
		const auto& Resource = Graph.Compiled->Resources[Handle.Index];
		const auto& Backing = Graph.Compiled->Backings[Handle.Index];
		requiref(Resource.Kind == ERDGResourceKind::Texture && Backing.Texture, "Render graph callback resolved an unavailable texture.");
		return Backing.Texture.GetReference();
	}

	auto FRDGPassResources::GetBuffer(
		FRDGBufferHandle Handle) const -> FRHIBuffer*
	{
		requiref(Handle.Owner == Graph.Compiled->Owner
			&& Handle.Index < Graph.Compiled->Resources.size(),
			"Render graph callback used an invalid buffer handle.");
		requiref(PassIndex < Graph.Compiled->RuntimePasses.size()
			&& std::ranges::find(Graph.Compiled->RuntimePasses[PassIndex].ResourceIndices,
				Handle.Index) != Graph.Compiled->RuntimePasses[PassIndex].ResourceIndices.end(),
			"Render graph pass '{}' accessed undeclared buffer resource {} ('{}').",
			PassIndex < Graph.Compiled->Passes.size()
				? Graph.Compiled->Passes[PassIndex].Name : "<invalid>", Handle.Index,
			Handle.Index < Graph.Compiled->Resources.size()
				? Graph.Compiled->Resources[Handle.Index].Name : "<invalid>");
		const auto& Resource = Graph.Compiled->Resources[Handle.Index];
		const auto& Backing = Graph.Compiled->Backings[Handle.Index];
		requiref(Resource.Kind == ERDGResourceKind::Buffer && Backing.Buffer, "Render graph callback resolved an unavailable buffer.");
		return Backing.Buffer.GetReference();
	}

	auto FRDGPassResources::ResolveValue(uint64 Owner, uint32 Index,
		const void* TypeIdentity, bool bWrite) const -> void*
	{
		requiref(Owner == Graph.Compiled->Owner
			&& Index < Graph.Compiled->Resources.size(),
			"Render graph callback used an invalid typed value handle.");
		const auto& Resource = Graph.Compiled->Resources[Index];
		requiref(Resource.ValueTypeIdentity != nullptr
			&& Resource.ValueTypeIdentity == TypeIdentity,
			"Render graph callback used a wrongly typed value handle.");
		requiref(PassIndex < Graph.Compiled->RuntimePasses.size()
			&& std::ranges::any_of(Graph.Compiled->RuntimePasses[PassIndex].ValueUses,
				[&](const auto& Use) {
					return Use.first == Index && Use.second == (bWrite
						? ERDGUse::Write : ERDGUse::Read);
				}),
			"Render graph pass '{}' accessed typed value '{}' with an undeclared "
			"or wrong-direction capability.",
			PassIndex < Graph.Compiled->Passes.size()
				? Graph.Compiled->Passes[PassIndex].Name : "<invalid>", Resource.Name);
		requiref(Resource.ValueStorageIndex
			< Graph.State->ValueStorage.Allocations.size(),
			"Render graph callback resolved unavailable typed value storage.");
		return Graph.State->ValueStorage.Allocations[
			Resource.ValueStorageIndex]->Data;
	}

	auto FRDGParameterResolver::FindMember(const void* Address,
		ERDGParameterMemberKind ExpectedKind,
		ERDGParameterMemberKind AlternateKind, bool bOptional) const
		-> const FRDGParameterMemberMetadata&
	{
		const FRDGParameterMemberMetadata* Found = nullptr;
		if (Layout != nullptr && Parameters != nullptr && Address != nullptr)
		{
			const uintptr_t RootAddress = reinterpret_cast<uintptr_t>(Parameters);
			const uintptr_t RequestedAddress = reinterpret_cast<uintptr_t>(Address);
			if (RequestedAddress >= RootAddress
				&& RequestedAddress - RootAddress < Layout->Metadata->StructSize)
			{
				const uint32 Offset = static_cast<uint32>(RequestedAddress - RootAddress);
				uint32 ElementIndex = std::numeric_limits<uint32>::max();
				if (bOptional)
				{
					const auto It = std::lower_bound(Layout->OffsetIndex.begin(),
						Layout->OffsetIndex.end(), Offset,
						[&](uint32 Index, uint32 Value) {
							return Layout->Elements[Index].Offset < Value;
						});
					if (It != Layout->OffsetIndex.end()
						&& Layout->Elements[*It].Offset == Offset)
						ElementIndex = *It;
				}
				else
				{
					const auto Alias = std::lower_bound(OptionalAliases.begin(),
						OptionalAliases.end(), Offset,
						[](const auto& Entry, uint32 Value) {
							return Entry.first < Value;
						});
					if (Alias != OptionalAliases.end() && Alias->first == Offset)
						ElementIndex = Alias->second;
					else
					{
						const auto It = std::lower_bound(Layout->OffsetIndex.begin(),
							Layout->OffsetIndex.end(), Offset,
							[&](uint32 Index, uint32 Value) {
								return Layout->Elements[Index].Offset < Value;
							});
						if (It != Layout->OffsetIndex.end()
							&& Layout->Elements[*It].Offset == Offset
							&& !Layout->Leaves[Layout->Elements[*It].LeafIndex]
								.Metadata->bOptional)
							ElementIndex = *It;
					}
				}
				if (ElementIndex < Layout->Elements.size())
				{
					const auto& Member = *Layout->Leaves[
						Layout->Elements[ElementIndex].LeafIndex].Metadata;
					if (Member.bOptional == bOptional
						&& (Member.Kind == ExpectedKind
							|| Member.Kind == AlternateKind))
						Found = &Member;
					else if (!bOptional && Member.bOptional
						&& (Member.Kind == ExpectedKind
							|| Member.Kind == AlternateKind))
						Found = &Member;
				}
			}
		}
		requiref(Found != nullptr,
			"Render graph pass '{}' parameter resolver accessed a member that is not "
			"declared by the executing pass parameters (requested capability '{}', "
			"optional={}).", PassName, ParameterMemberKindName(ExpectedKind), bOptional);
		return *Found;
	}

	auto FRDGParameterResolver::ValidateShaderParametersIdentity(
		const void* Data, const FRDGParametersMetadata* InMetadata) const
		-> void
	{
		requiref(Data == Parameters && Layout != nullptr
			&& InMetadata == Layout->Metadata,
			"Render graph pass '{}' attempted composed shader submission from a "
			"copied or foreign parameter object.", PassName);
	}

	auto FRDGParameterResolver::GetTexture(
		const FRDGTextureParameter& Parameter) const -> FRHITexture*
	{
		FindMember(&Parameter, ERDGParameterMemberKind::Texture,
			ERDGParameterMemberKind::Texture, false);
		return Resources.GetTexture(Parameter.Texture);
	}

	auto FRDGParameterResolver::GetTexture(const std::optional<
		FRDGTextureParameter>& Parameter) const -> FRHITexture*
	{
		FindMember(&Parameter, ERDGParameterMemberKind::Texture,
			ERDGParameterMemberKind::Texture, true);
		return Parameter ? Resources.GetTexture(Parameter->Texture) : nullptr;
	}

	auto FRDGParameterResolver::GetTexture(
		const FRDGManagedTextureParameter& Parameter) const -> FRHITexture*
	{
		FindMember(&Parameter, ERDGParameterMemberKind::ManagedTexture,
			ERDGParameterMemberKind::ManagedTexture, false);
		return Resources.GetTexture(Parameter.Texture);
	}

	auto FRDGParameterResolver::GetTexture(const std::optional<
		FRDGManagedTextureParameter>& Parameter) const -> FRHITexture*
	{
		FindMember(&Parameter, ERDGParameterMemberKind::ManagedTexture,
			ERDGParameterMemberKind::ManagedTexture, true);
		return Parameter ? Resources.GetTexture(Parameter->Texture) : nullptr;
	}

	auto FRDGParameterResolver::GetBuffer(
		const FRDGBufferParameter& Parameter) const -> FRHIBuffer*
	{
		FindMember(&Parameter, ERDGParameterMemberKind::Buffer,
			ERDGParameterMemberKind::Buffer, false);
		return Resources.GetBuffer(Parameter.Buffer);
	}

	auto FRDGParameterResolver::GetBuffer(const std::optional<
		FRDGBufferParameter>& Parameter) const -> FRHIBuffer*
	{
		FindMember(&Parameter, ERDGParameterMemberKind::Buffer,
			ERDGParameterMemberKind::Buffer, true);
		return Parameter ? Resources.GetBuffer(Parameter->Buffer) : nullptr;
	}

	namespace
	{
		auto MakeAttachmentView(const FRDGPassResources& Resources,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			const FRDGParameterMemberMetadata& Member)
			-> FRDGAttachmentView
		{
			return {Resources.GetTexture(Texture), Range, Member.LoadAction,
				Member.StoreAction, Member.bPassManagedTransition,
				Member.ResultAccess};
		}
	}

	auto FRDGParameterResolver::GetColorAttachment(
		const FRDGColorAttachmentParameter& Parameter) const
		-> FRDGAttachmentView
	{
		const auto& Member = FindMember(&Parameter,
			ERDGParameterMemberKind::ColorAttachment,
			ERDGParameterMemberKind::ManagedColorAttachment, false);
		return MakeAttachmentView(Resources, Parameter.Texture, Parameter.Range,
			Member);
	}

	auto FRDGParameterResolver::GetColorAttachment(const std::optional<
		FRDGColorAttachmentParameter>& Parameter) const
		-> FRDGAttachmentView
	{
		const auto& Member = FindMember(&Parameter,
			ERDGParameterMemberKind::ColorAttachment,
			ERDGParameterMemberKind::ManagedColorAttachment, true);
		return Parameter ? MakeAttachmentView(Resources, Parameter->Texture,
			Parameter->Range, Member) : FRDGAttachmentView{};
	}

	auto FRDGParameterResolver::GetDepthStencilAttachment(
		const FRDGDepthStencilAttachmentParameter& Parameter) const
		-> FRDGAttachmentView
	{
		const auto& Member = FindMember(&Parameter,
			ERDGParameterMemberKind::DepthStencilAttachment,
			ERDGParameterMemberKind::ManagedDepthStencilAttachment, false);
		return MakeAttachmentView(Resources, Parameter.Texture, Parameter.Range,
			Member);
	}

	auto FRDGParameterResolver::GetDepthStencilAttachment(
		const std::optional<FRDGDepthStencilAttachmentParameter>& Parameter)
		const -> FRDGAttachmentView
	{
		const auto& Member = FindMember(&Parameter,
			ERDGParameterMemberKind::DepthStencilAttachment,
			ERDGParameterMemberKind::ManagedDepthStencilAttachment, true);
		return Parameter ? MakeAttachmentView(Resources, Parameter->Texture,
			Parameter->Range, Member) : FRDGAttachmentView{};
	}

	FRDGAllocatedResources::FRDGAllocatedResources(uint32 Count)
		: Textures(Count), Buffers(Count), AllocationIds(Count),
		  AllocationDispositions(Count)
	{
	}

	auto FRDGAllocatedResources::SetTexture(uint32 ResourceId,
		FTextureRHIRef Texture, uint64 AllocationId,
		std::string_view Disposition) -> bool
	{
		if (ResourceId >= Textures.size() || !Texture) return false;
		Textures[ResourceId] = std::move(Texture);
		AllocationIds[ResourceId] = AllocationId;
		AllocationDispositions[ResourceId] = Disposition;
		return true;
	}

	auto FRDGAllocatedResources::SetBuffer(uint32 ResourceId,
		FBufferRHIRef Buffer, uint64 AllocationId,
		std::string_view Disposition) -> bool
	{
		if (ResourceId >= Buffers.size() || !Buffer) return false;
		Buffers[ResourceId] = std::move(Buffer);
		AllocationIds[ResourceId] = AllocationId;
		AllocationDispositions[ResourceId] = Disposition;
		return true;
	}

} // namespace Durin
