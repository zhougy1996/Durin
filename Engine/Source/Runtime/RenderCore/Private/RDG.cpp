#include "RDG.h"

#include "RHICommandList.h"

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
			FOptionalAliasTable(const FOptionalAliasTable& Other)
				: FOptionalAliasTable(Other.View())
			{
			}
			auto operator=(const FOptionalAliasTable& Other)
				-> FOptionalAliasTable&
			{
				if (this != &Other)
					*this = FOptionalAliasTable(Other);
				return *this;
			}
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
			std::vector<FGraphUse> Uses;
			std::vector<uint32> Prerequisites;
			FRDGPassExecute Execute;
			FRDGParameterizedPassExecute ParameterizedExecute;
			bool bRoot = false;
			// Terminal exports consume contents but may hand off a writable access state.
			bool bExport = false;
			std::string RootReason;
			bool bParameterized = false;
			const FRDGParameterLayout* ParameterLayout = nullptr;
			const void* Parameters = nullptr;
			FOptionalAliasTable OptionalAliases;
			std::vector<FRDGParameterCapture> ParameterCaptures;
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

		struct FRangeState
		{
			FGraphUse Use;
			ERHIAccess Access = ERHIAccess::Discard;
			bool bProduced = false;
			uint32 Producer = std::numeric_limits<uint32>::max();
			std::vector<uint32> Readers;
			uint32 Version = 0;
		};

		struct FLoweredParameterUse final
		{
			FGraphUse Use;
			bool bPresent = false;
		};

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
				Use.bDiscard = Member.LoadAction
					!= ERHIRenderTargetLoadAction::Load;
				Use.bStore = Member.StoreAction
					== ERHIRenderTargetStoreAction::Store;
				break;
			case ERDGParameterMemberKind::DepthStencilAttachment:
			case ERDGParameterMemberKind::ManagedDepthStencilAttachment:
				Result.bPresent = Visit.template operator()<
					FRDGDepthStencilAttachmentParameter>([&](const auto& Value) {
						Use.ResourceIndex = GetTextureIndex(Value.Texture);
						Use.TextureRange = Value.Range;
					});
				Use.bDiscard = Member.LoadAction
					!= ERHIRenderTargetLoadAction::Load;
				Use.bStore = Member.StoreAction
					== ERHIRenderTargetStoreAction::Store;
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

		auto ContainsRange(const FGraphUse& Outer, const FGraphUse& Inner) -> bool
		{
			if (Outer.ResourceIndex != Inner.ResourceIndex || Outer.Kind != Inner.Kind)
				return false;
			if (Outer.Kind == ERDGResourceKind::Token) return true;
			if (Outer.Kind == ERDGResourceKind::Buffer)
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
			std::vector<std::string_view> ShaderBindingNames;
			MemberNames.reserve(Metadata->Members.size());
			ShaderBindingNames.reserve(Metadata->Members.size());
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

				if (Member.Kind == ERDGParameterMemberKind::Nested)
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
					OutError = Prefix + " member '" + Member.Name
						+ "' has a mismatched wrapper layout";
					return false;
				}
				if (Member.bOptional
					!= (Member.ReadOptionalValueAddress != nullptr))
				{
					OutError = Prefix + " member '" + Member.Name
						+ "' has inconsistent optional layout";
					return false;
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
					OutError = Prefix + " member '" + Member.Name
						+ "' has inconsistent declaration semantics";
					return false;
				}

				if (Member.bShaderBinding)
				{
					if (Member.ShaderBindingName == nullptr
						|| Member.ShaderBindingName[0] == '\0')
					{
						OutError = Prefix + " member '" + Member.Name
							+ "' has an empty shader binding name";
						return false;
					}
					if (std::ranges::find(ShaderBindingNames,
						Member.ShaderBindingName) != ShaderBindingNames.end())
					{
						OutError = Prefix + " has duplicate shader binding '"
							+ Member.ShaderBindingName + "'";
						return false;
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
						OutError = Prefix + " member '" + Member.Name
							+ "' has an incompatible graph/shader declaration";
						return false;
					}
				}
				else if (Member.ShaderBindingName != nullptr)
				{
					OutError = Prefix + " member '" + Member.Name
						+ "' has shader metadata without binding authority";
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
			{
				Prefix += " parameter '";
				Prefix.append(Use.ParameterPath);
				Prefix += "'";
			}
			return Prefix;
		}

		auto ValidateShaderCompositionMetadata(
			const FRDGParametersMetadata* Metadata, std::string& OutError)
			-> bool
		{
			std::vector<std::pair<std::string_view, std::string>> Bindings;
			std::function<bool(const FRDGParametersMetadata*,
				const std::string&)> Traverse;
			Traverse = [&](const FRDGParametersMetadata* StructMetadata,
				const std::string& ParentPath) {
				for (const auto& Member : StructMetadata->Members)
				{
					const std::string Path = ParentPath.empty()
						? std::string(StructMetadata->StructName) + "." + Member.Name
						: ParentPath + "." + Member.Name;
					if (Member.Kind == ERDGParameterMemberKind::Nested)
					{
						if (!Traverse(Member.NestedParameters, Path)) return false;
						continue;
					}
					if (!Member.bShaderBinding) continue;
					const auto Existing = std::ranges::find_if(Bindings,
						[&](const auto& Binding) {
							return Binding.first == Member.ShaderBindingName;
						});
					if (Existing != Bindings.end())
					{
						OutError = "render graph parameter metadata for '"
							+ std::string(Metadata->StructName)
							+ "' has duplicate shader binding '"
							+ Member.ShaderBindingName + "' at '" + Existing->second
							+ "' and '" + Path + "'";
						return false;
					}
					Bindings.emplace_back(Member.ShaderBindingName, Path);
				}
				return true;
			};
			return Traverse(Metadata, {});
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
				if (Pass.bExport && !IsExportAccessAllowed(Use.Kind, Use.Access))
				{
					OutError = Prefix + " resource '" + Resource.Name
						+ "' has invalid final access";
					return false;
				}
				if (Use.Kind != ERDGResourceKind::Token
					&& (Use.Access == ERHIAccess::None
						|| EnumHasAnyFlags(Use.Access, ERHIAccess::Discard)))
				{
					OutError = Prefix + " resource '" + Resource.Name
						+ "' has invalid required access";
					return false;
				}
				if (Use.Kind != ERDGResourceKind::Token
					&& !Pass.bExport && !IsAccessAllowed(Pass.Type, Use.Access))
				{
					OutError = Prefix + " resource '" + Resource.Name
						+ "' access is incompatible with pass domain";
					return false;
				}
				if (Use.Kind != ERDGResourceKind::Token
					&& !Pass.bExport && ((Use.Use == ERDGUse::Read
							&& AccessHasWrite(Use.Access))
						|| (Use.Use == ERDGUse::Write
							&& !AccessHasWrite(Use.Access))))
				{
					OutError = Prefix + " resource '" + Resource.Name
						+ "' access disagrees with use mode";
					return false;
				}
				if (Use.bDiscard && Use.Use == ERDGUse::Read)
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
				if (Use.Kind == ERDGResourceKind::Buffer
					&& (Use.BufferSize == 0
						|| Use.BufferOffset > Resource.BufferDesc.Size
						|| Use.BufferSize > Resource.BufferDesc.Size
							- Use.BufferOffset))
				{
					OutError = Prefix + " resource '" + Resource.Name
						+ "' has invalid buffer range";
					return false;
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
					OutError = Prefix + " resource '" + Resource.Name
						+ "' has invalid texture range";
					return false;
				}
				for (uint32 OtherUse = 0; OtherUse < UseIndex; ++OtherUse)
					if (RangesOverlap(Use, Pass.Uses[OtherUse]))
					{
						OutError = Prefix
							+ " declares overlapping uses of resource '"
							+ Resource.Name + "' with ";
						if (!Pass.Uses[OtherUse].ParameterPath.empty())
							{
								OutError += "parameter '";
								OutError.append(Pass.Uses[OtherUse].ParameterPath);
								OutError += "'";
							}
						else OutError += "an earlier manual declaration";
						return false;
					}
			}
			return true;
		}

		using FResourceUseTable = std::vector<std::vector<const FGraphUse*>>;

		// Carries the mutable scheduling state shared by dependency analysis and
		// stable topological ordering.
		struct FDependencyGraph final
		{
			std::vector<std::vector<uint32>> Outgoing;
			std::vector<uint32> Indegree;
			std::vector<FRDGDependency> Dependencies;
		};

		auto AddDependencyEdge(FDependencyGraph& Graph, uint32 Before,
			uint32 After, const std::string& Cause, ERDGDependencyKind Kind) -> void
		{
			auto Existing = std::ranges::find_if(Graph.Dependencies,
				[&](const auto& Edge) {
					return Edge.BeforePass == Before && Edge.AfterPass == After;
				});
			if (Existing != Graph.Dependencies.end())
			{
				if (Existing->Kind == ERDGDependencyKind::Execution
					&& Kind != ERDGDependencyKind::Execution)
				{
					Existing->Kind = Kind;
					Existing->Cause = Cause;
				}
				return;
			}
			Graph.Outgoing[Before].push_back(After);
			++Graph.Indegree[After];
			Graph.Dependencies.push_back({Before, After, Cause, Kind});
		}

		auto ValidateGraphResources(std::span<const FGraphResource> Resources)
			-> std::string
		{
			for (uint32 ResourceIndex = 0; ResourceIndex < Resources.size();
				++ResourceIndex)
			{
				const auto& Resource = Resources[ResourceIndex];
				if (Resource.Name.empty())
					return "resource[" + std::to_string(ResourceIndex)
						+ "] has an empty name";
				if (Resource.bExternal && !Resource.Texture && !Resource.Buffer)
					return "resource '" + Resource.Name
						+ "' has no physical resource";
				if (Resource.bExternal && Resource.FinalAccess == ERHIAccess::None)
					return "external resource '" + Resource.Name
						+ "' has no final access";
				if (EnumHasAnyFlags(Resource.FinalAccess, ERHIAccess::Discard))
					return "resource '" + Resource.Name
						+ "' has invalid final access";
				for (uint32 Other = 0; Other < ResourceIndex; ++Other)
					if (Resources[Other].Name == Resource.Name)
						return "duplicate resource name '" + Resource.Name + "'";
			}
			return {};
		}

		auto ValidateGraphPasses(std::span<const FGraphPass> Passes,
			std::span<const FGraphResource> Resources,
			FDependencyGraph& Graph) -> std::string
		{
			for (uint32 PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
			{
				const auto& Pass = Passes[PassIndex];
				if (Pass.Name.empty())
					return "pass[" + std::to_string(PassIndex)
						+ "] has an empty name";
				for (uint32 Other = 0; Other < PassIndex; ++Other)
					if (Passes[Other].Name == Pass.Name)
						return "duplicate pass name '" + Pass.Name + "'";
				for (uint32 Prerequisite : Pass.Prerequisites)
				{
					if (Prerequisite >= Passes.size())
						return "pass '" + Pass.Name
							+ "' has an invalid prerequisite";
					AddDependencyEdge(Graph, Prerequisite, PassIndex, "explicit",
						ERDGDependencyKind::Explicit);
				}
				std::string UseError;
				if (!ValidatePassUses(Pass, Resources, UseError))
					return UseError;
			}
			return {};
		}

		auto BuildResourceUseTable(std::span<const FGraphPass> Passes,
			uint32 ResourceCount) -> FResourceUseTable
		{
			FResourceUseTable ResourceUses(ResourceCount);
			std::vector<size_t> ResourceUseCounts(ResourceCount, 0);
			for (const auto& Pass : Passes)
				for (const auto& Use : Pass.Uses)
					++ResourceUseCounts[Use.ResourceIndex];
			for (uint32 ResourceIndex = 0; ResourceIndex < ResourceCount;
				++ResourceIndex)
				ResourceUses[ResourceIndex].reserve(
					ResourceUseCounts[ResourceIndex]);
			for (const auto& Pass : Passes)
				for (const auto& Use : Pass.Uses)
					ResourceUses[Use.ResourceIndex].push_back(&Use);
			return ResourceUses;
		}

		auto ValidateTypedValueWriters(
			std::span<const FGraphResource> Resources,
			const FResourceUseTable& ResourceUses) -> std::string
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
					return "typed value '" + Resource.Name + "' type '"
						+ Resource.ValueTypeName
						+ "' requires exactly one writer; actual="
						+ std::to_string(Writers);
			}
			return {};
		}

		auto BuildRangeCells(std::span<const FGraphResource> Resources,
			const FResourceUseTable& ResourceUses, size_t DeclaredUseCount)
			-> std::vector<FRangeState>
		{
			std::vector<FRangeState> Cells;
			Cells.reserve(DeclaredUseCount);
			for (uint32 ResourceIndex = 0; ResourceIndex < Resources.size();
				++ResourceIndex)
			{
				const auto& Resource = Resources[ResourceIndex];
				const auto& Uses = ResourceUses[ResourceIndex];
				if (Uses.empty()) continue;
				if (Resource.Kind == ERDGResourceKind::Token)
				{
					FRangeState Cell;
					Cell.Use = *Uses.front();
					Cell.bProduced = Resource.HasInitialContents();
					Cell.Access = Resource.InitialAccess;
					Cells.push_back(std::move(Cell));
					continue;
				}
				if (Resource.Kind == ERDGResourceKind::Buffer)
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
							[&](const FGraphUse* Use) {
								return ContainsRange(*Use, CellUse);
							}))
							continue;
						FRangeState Cell;
						Cell.Use = CellUse;
						Cell.bProduced = Resource.HasInitialContents();
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
							MipCuts.push_back(Use->TextureRange.FirstMip
								+ Use->TextureRange.NumMips);
							LayerCuts.push_back(Use->TextureRange.FirstArrayLayer);
							LayerCuts.push_back(Use->TextureRange.FirstArrayLayer
								+ Use->TextureRange.NumArrayLayers);
						}
					std::ranges::sort(MipCuts);
					std::ranges::sort(LayerCuts);
					MipCuts.erase(std::unique(MipCuts.begin(), MipCuts.end()),
						MipCuts.end());
					LayerCuts.erase(std::unique(LayerCuts.begin(), LayerCuts.end()),
						LayerCuts.end());
					for (size_t Mip = 1; Mip < MipCuts.size(); ++Mip)
						for (size_t Layer = 1; Layer < LayerCuts.size(); ++Layer)
						{
							FGraphUse CellUse = *Uses.front();
							CellUse.TextureRange = {Aspect, MipCuts[Mip - 1],
								MipCuts[Mip] - MipCuts[Mip - 1],
								LayerCuts[Layer - 1],
								LayerCuts[Layer] - LayerCuts[Layer - 1]};
							if (!std::ranges::any_of(Uses,
								[&](const FGraphUse* Use) {
									return ContainsRange(*Use, CellUse);
								}))
								continue;
							FRangeState Cell;
							Cell.Use = CellUse;
							Cell.bProduced = Resource.HasInitialContents();
							Cell.Access = Resource.InitialAccess;
							Cells.push_back(std::move(Cell));
						}
				}
			}
			return Cells;
		}

		auto BuildHazardDependencies(std::span<const FGraphPass> Passes,
			std::span<const FGraphResource> Resources,
			std::vector<FRangeState>& Cells, FDependencyGraph& Graph)
			-> std::string
		{
			for (uint32 PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
				for (const auto& Use : Passes[PassIndex].Uses)
					for (auto& Cell : Cells)
					{
						if (!ContainsRange(Use, Cell.Use)) continue;
						const auto& Resource = Resources[Use.ResourceIndex];
						if (Use.Use != ERDGUse::Write && !Use.bDiscard
							&& !Cell.bProduced)
							return PassUsePrefix(Passes[PassIndex], Use)
								+ " reads resource '" + Resource.Name
								+ "' before its producer";
						if (Use.Use == ERDGUse::Read)
						{
							if (Cell.Producer != std::numeric_limits<uint32>::max())
								AddDependencyEdge(Graph, Cell.Producer, PassIndex,
									Resource.Name, ERDGDependencyKind::Value);
							if (std::ranges::find(Cell.Readers, PassIndex)
								== Cell.Readers.end())
								Cell.Readers.push_back(PassIndex);
							continue;
						}
						if (Use.Use == ERDGUse::ReadWrite && !Use.bDiscard
							&& Cell.Producer != std::numeric_limits<uint32>::max())
							AddDependencyEdge(Graph, Cell.Producer, PassIndex,
								Resource.Name, ERDGDependencyKind::Value);
						for (uint32 Reader : Cell.Readers)
							AddDependencyEdge(Graph, Reader, PassIndex, Resource.Name,
								ERDGDependencyKind::Execution);
						if (Cell.Readers.empty()
							&& Cell.Producer != std::numeric_limits<uint32>::max())
							AddDependencyEdge(Graph, Cell.Producer, PassIndex,
								Resource.Name, ERDGDependencyKind::Execution);
						++Cell.Version;
						Cell.Producer = Use.bStore
							? PassIndex : std::numeric_limits<uint32>::max();
						Cell.bProduced = Use.bStore;
						Cell.Readers.clear();
					}
			return {};
		}

		auto BuildStablePassOrder(FDependencyGraph& Graph, uint32 PassCount,
			std::vector<uint32>& OutOrder) -> std::string
		{
			OutOrder.reserve(PassCount);
			std::vector<bool> Emitted(PassCount, false);
			while (OutOrder.size() < PassCount)
			{
				uint32 Selected = PassCount;
				for (uint32 Index = 0; Index < PassCount; ++Index)
					if (!Emitted[Index] && Graph.Indegree[Index] == 0)
					{
						Selected = Index;
						break;
					}
				if (Selected == PassCount)
					return "graph contains a dependency cycle";
				Emitted[Selected] = true;
				OutOrder.push_back(Selected);
				for (uint32 After : Graph.Outgoing[Selected])
					--Graph.Indegree[After];
			}
			return {};
		}

		auto FindRetainedPasses(std::span<const FGraphPass> Passes,
			std::span<const FRDGDependency> Dependencies, bool bEnableCulling)
			-> std::vector<bool>
		{
			std::vector<bool> Retained(Passes.size(), !bEnableCulling);
			if (!bEnableCulling) return Retained;

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
				for (const auto& Edge : Dependencies)
					if (Edge.AfterPass == After
						&& Edge.Kind != ERDGDependencyKind::Execution
						&& !Retained[Edge.BeforePass])
					{
						Retained[Edge.BeforePass] = true;
						Pending.push_back(Edge.BeforePass);
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
		if (!ValidateParameterMetadata(Metadata, ExpectedSize,
			ExpectedAlignment, Result.Error)
			|| !ValidateShaderCompositionMetadata(Metadata, Result.Error))
			return Result;

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
		uint64 Owner = 0;
		std::vector<FGraphResource> Resources;
		std::unordered_map<const void*, uint32> ExternalResources;
		std::vector<FGraphPass> Passes;
		std::vector<std::string> DeclarationErrors;
		bool bEnableCulling = false;
		FRDGBudget Budget;
		ERDGBuilderState Lifecycle = ERDGBuilderState::Building;
		FRDGExecutionResult ExecutionResult;
		bool bCompiled = false;
		uint32 PendingConstructions = 0;
		FGraphParameterStorage ParameterStorage;
		FGraphParameterStorage ValueStorage;
	};

	struct FRDGBuilder::FCompiledState
	{
		// Keeps all execution-only state for one scheduled pass in one record.
		struct FCompiledPassRuntime final
		{
			const FRDGPassExecute* Execute = nullptr;
			const FRDGParameterizedPassExecute* ParameterizedExecute = nullptr;
			const FRDGParameterLayout* ParameterLayout = nullptr;
			const void* Parameters = nullptr;
			FOptionalAliasTable OptionalAliases;
			std::vector<uint32> ResourceIndices;
			std::vector<std::pair<uint32, ERDGUse>> ValueUses;
			std::vector<uint32> BufferTransitionResources;
			std::vector<uint32> TextureTransitionResources;
		};

		uint64 Owner = 0;
		std::vector<FGraphResource> Resources;
		std::vector<FRDGCompiledPass> Passes;
		std::vector<FCompiledPassRuntime> RuntimePasses;
		std::vector<FRDGDependency> Dependencies;
		std::vector<FRDGResourceLifetime> ResourceLifetimes;
		std::vector<FRDGCullingDecision> CullingDecisions;
		std::vector<FRHIBufferTransition> FinalBufferTransitions;
		std::vector<FRHITextureTransition> FinalTextureTransitions;
		std::vector<uint32> FinalBufferTransitionResources;
		std::vector<uint32> FinalTextureTransitionResources;
		std::vector<FRDGResourceCapture> ResourceCaptures;
		std::vector<FRDGParameterCapture> ParameterCaptures;
		std::vector<FRDGUseCapture> UseCaptures;
		std::vector<FRDGTransitionCapture> TransitionCaptures;
		std::vector<FRDGAllocationRequest> AllocationRequests;
		FRDGBudget Budget;
		uint64 CompileMicroseconds = 0;
		uint64 ExecuteMicroseconds = 0;
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

	auto FRDGBuilder::CompileForTesting() -> std::string
	{
		if (State->Lifecycle != ERDGBuilderState::Building)
			return "render graph is already consumed";
		State->Lifecycle = ERDGBuilderState::Compiling;
		struct FFailureGuard
		{
			ERDGBuilderState& State;
			~FFailureGuard()
			{
				if (State == ERDGBuilderState::Compiling) State = ERDGBuilderState::Failed;
			}
		} Guard{State->Lifecycle};
		std::string Error = Compile();
		State->Lifecycle = Error.empty() ? ERDGBuilderState::Preparing : ERDGBuilderState::Failed;
		return Error;
	}

	auto FRDGBuilder::Execute(FRHICommandListImmediate& CommandList,
		FRDGExecutionContext* Context) -> FRDGExecutionResult
	{
		if (State->Lifecycle != ERDGBuilderState::Building)
			return {ERDGExecutionStatus::InvalidState, "render graph is already consumed"};
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
			"render graph compilation did not complete"};
		State->ExecutionResult.Error = Compile();
		if (!State->ExecutionResult.Error.empty()) return State->ExecutionResult;
		State->Lifecycle = ERDGBuilderState::Preparing;
		State->ExecutionResult.Status = ERDGExecutionStatus::PreparationFailed;
		State->ExecutionResult.Error = "render graph preparation did not complete";
		if (!Record(CommandList, Context, &State->ExecutionResult.Error))
			return State->ExecutionResult;
		State->Lifecycle = ERDGBuilderState::Recorded;
		State->ExecutionResult.Status = ERDGExecutionStatus::Recorded;
		return State->ExecutionResult;
	}

	auto FRDGBuilder::AllocateParameterStorage(size_t Size,
		size_t Alignment, const FRDGParametersMetadata* Metadata,
		const FRDGParameterLayoutBuildResult& LayoutResult,
		void (*Destroy)(void*), std::weak_ptr<void>& OutLifetime) -> void*
	{
		RequireBuilding();
		if (LayoutResult.Layout == nullptr)
		{
			State->DeclarationErrors.push_back(LayoutResult.Error);
			return nullptr;
		}
		if (LayoutResult.Layout->Metadata != Metadata
			|| Metadata->StructSize != Size
			|| Metadata->StructAlignment != Alignment)
		{
			State->DeclarationErrors.emplace_back(
				"render graph parameter layout does not match its typed metadata");
			return nullptr;
		}
		auto Allocation = std::make_shared<FGraphParameterAllocation>(
			Size, Alignment, Destroy);
		Allocation->Layout = LayoutResult.Layout.get();
		void* Data = Allocation->Data;
		OutLifetime = Allocation;
		State->ParameterStorage.Allocations.push_back(std::move(Allocation));
		return Data;
	}

	auto FRDGBuilder::MarkParameterStorageConstructed(void* Storage)
		-> void
	{
		auto Allocation = std::ranges::find_if(
			State->ParameterStorage.Allocations,
			[Storage](const auto& Candidate) { return Candidate->Data == Storage; });
		if (Allocation != State->ParameterStorage.Allocations.end())
			(*Allocation)->bConstructed = true;
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
			State->DeclarationErrors.emplace_back(
				"render graph value has invalid storage metadata");
			return nullptr;
		}
		for (const auto& Existing : State->Resources)
		{
			if (Existing.ValueTypeIdentity == nullptr) continue;
			if (Existing.ValueTypeIdentity == TypeIdentity
				&& Existing.ValueTypeName != StableTypeName)
			{
				State->DeclarationErrors.push_back("typed value '"
					+ std::string(Name) + "' changes stable type name from '"
					+ Existing.ValueTypeName + "' to '"
					+ std::string(StableTypeName) + "'");
				return nullptr;
			}
			if (Existing.ValueTypeIdentity != TypeIdentity
				&& Existing.ValueTypeName == StableTypeName)
			{
				State->DeclarationErrors.push_back("typed value '"
					+ std::string(Name) + "' reuses stable type name '"
					+ std::string(StableTypeName) + "' for a different C++ type");
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
					State->DeclarationErrors.emplace_back(
						"conflicting external physical resource: canonical '"
						+ Canonical.Name + "' (" + DescribeExternalContract(Canonical)
						+ ") conflicts with '" + std::string(Name) + "' ("
						+ DescribeExternalContract(Resource) + ")");
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
					State->DeclarationErrors.emplace_back(
						"conflicting external physical resource: canonical '"
						+ Canonical.Name + "' (" + DescribeExternalContract(Canonical)
						+ ") conflicts with '" + std::string(Name) + "' ("
						+ DescribeExternalContract(Resource) + ")");
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
			State->DeclarationErrors.emplace_back(
				"texture extraction uses a foreign or invalid handle");
			return;
		}
		if (Destination == nullptr || FinalAccess == ERHIAccess::None
			|| EnumHasAnyFlags(FinalAccess, ERHIAccess::Discard))
		{
			State->DeclarationErrors.emplace_back(
				"texture extraction requires a destination and valid final access");
			return;
		}
		if (std::ranges::any_of(State->Resources,
			[&](const FGraphResource& Existing) {
				return (&Existing == &State->Resources[Texture.Index]
						&& Existing.IsExported())
					|| Existing.TextureDestination == Destination;
			}))
		{
			State->DeclarationErrors.emplace_back(
				"duplicate or conflicting texture extraction");
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
			State->DeclarationErrors.emplace_back(
				"buffer extraction uses a foreign or invalid handle");
			return;
		}
		if (Destination == nullptr || FinalAccess == ERHIAccess::None
			|| EnumHasAnyFlags(FinalAccess, ERHIAccess::Discard))
		{
			State->DeclarationErrors.emplace_back(
				"buffer extraction requires a destination and valid final access");
			return;
		}
		if (std::ranges::any_of(State->Resources,
			[&](const FGraphResource& Existing) {
				return (&Existing == &State->Resources[Buffer.Index]
						&& Existing.IsExported())
					|| Existing.BufferDestination == Destination;
			}))
		{
			State->DeclarationErrors.emplace_back(
				"duplicate or conflicting buffer extraction");
			return;
		}
		State->Resources[Buffer.Index].FinalAccess = FinalAccess;
		State->Resources[Buffer.Index].BufferDestination = Destination;
	}

	auto FRDGBuilder::AddPass(std::string_view Name,
		ERDGPassType Type, FRDGPassExecute Execute)
		-> FRDGPassHandle
	{
		RequireBuilding();
		const uint32 Index = static_cast<uint32>(State->Passes.size());
		FGraphPass Pass;
		Pass.Name = Name;
		Pass.Type = Type;
		Pass.Execute = std::move(Execute);
		State->Passes.push_back(std::move(Pass));
		return {State->Owner, Index};
	}

	auto FRDGBuilder::AddParameterizedPass(std::string_view Name,
		ERDGPassType Type,
		const FRDGParameterLayout* Layout, void* Parameters,
		std::shared_ptr<void> Lifetime, FRDGPassExecute Execute,
		FRDGParameterizedPassExecute ParameterizedExecute)
		-> FRDGPassHandle
	{
		RequireBuilding();
		const FRDGParametersMetadata* Metadata = Layout != nullptr
			? Layout->Metadata : nullptr;
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
		if (Parameters == nullptr || Lifetime == nullptr || Layout == nullptr
			|| Allocation == State->ParameterStorage.Allocations.end()
			|| (*Allocation)->Layout != Layout)
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
			const FGraphUse& DeclaredUse = Lowered.Use;
			ParameterizedPass.ParameterCaptures.push_back({
				.FieldPath = FieldPath,
				.Kind = Member.Kind,
				.ResourceKind = Member.ResourceKind,
				.bPresent = Lowered.bPresent,
				.ResourceId = Lowered.bPresent ? DeclaredUse.ResourceIndex
					: std::numeric_limits<uint32>::max(),
				.Use = DeclaredUse.Use,
				.Access = DeclaredUse.Access,
				.TextureRange = DeclaredUse.TextureRange,
				.BufferOffset = DeclaredUse.BufferOffset,
				.BufferSize = DeclaredUse.BufferSize,
				.bDiscard = DeclaredUse.bDiscard,
				.bStore = DeclaredUse.bStore,
				.bPassManagedTransition = DeclaredUse.bPassManagedTransition,
				.ResultAccess = DeclaredUse.ResultAccess,
				.ShaderBindingName = std::string(
					DeclaredUse.ShaderBindingName),
				.ShaderBindingType = DeclaredUse.ShaderBindingType,
			});
			if (Lowered.bPresent)
				ParameterizedPass.Uses.push_back(std::move(Lowered.Use));
		}
		std::ranges::sort(OptionalAliases);
		ParameterizedPass.OptionalAliases = FOptionalAliasTable(OptionalAliases);

		std::string UseError;
		if (!ValidatePassUses(ParameterizedPass, State->Resources, UseError))
		{
			State->DeclarationErrors.push_back(std::move(UseError));
			return {};
		}
		const uint32 Index = static_cast<uint32>(State->Passes.size());
		for (auto& Capture : ParameterizedPass.ParameterCaptures)
			Capture.PassDeclarationIndex = Index;
		State->Passes.push_back(std::move(ParameterizedPass));
		return {State->Owner, Index};
	}

	auto FRDGBuilder::CanDeclareManualUse(FRDGPassHandle Pass,
		std::string_view InvalidHandleError) -> bool
	{
		RequireBuilding();
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

	auto FRDGBuilder::MarkPassRoot(FRDGPassHandle Pass,
		std::string_view Reason) -> void
	{
		RequireBuilding();
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size())
		{
			State->DeclarationErrors.emplace_back("root has an invalid pass handle");
			return;
		}
		State->Passes[Pass.Index].bRoot = true;
		State->Passes[Pass.Index].RootReason = std::string(Reason);
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

	auto FRDGBuilder::AddDependency(FRDGPassHandle Pass,
		FRDGPassHandle Prerequisite) -> void
	{
		RequireBuilding();
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

	auto FRDGBuilder::UseTexture(FRDGPassHandle Pass,
		FRDGTextureHandle Texture,
		const FRHITextureSubresourceRange& Range, ERDGUse Use,
		ERHIAccess Access, bool bDiscard) -> void
	{
		RequireBuilding();
		if (!CanDeclareManualUse(Pass,
			"texture use has an invalid pass handle")) return;
		State->Passes[Pass.Index].Uses.push_back({
			Texture.Owner == State->Owner ? Texture.Index
				: std::numeric_limits<uint32>::max(),
			ERDGResourceKind::Texture, Use, Access, bDiscard, Range});
	}

	auto FRDGBuilder::UseBuffer(FRDGPassHandle Pass,
		FRDGBufferHandle Buffer, uint64 Offset, uint64 Size,
		ERDGUse Use, ERHIAccess Access, bool bDiscard) -> void
	{
		RequireBuilding();
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
		RequireBuilding();
		if (!CanDeclareManualUse(Pass,
			"texture use has an invalid pass handle")) return;
		UseTexture(Pass, Texture, Range, ERDGUse::ReadWrite,
			ERHIAccess::ColorAttachmentReadWrite,
			LoadAction != ERHIRenderTargetLoadAction::Load);
		if (Pass.Owner == State->Owner && Pass.Index < State->Passes.size())
			State->Passes[Pass.Index].Uses.back().bStore =
				StoreAction == ERHIRenderTargetStoreAction::Store;
	}

	auto FRDGBuilder::UseDepthStencilAttachment(
		FRDGPassHandle Pass, FRDGTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction) -> void
	{
		RequireBuilding();
		if (!CanDeclareManualUse(Pass,
			"texture use has an invalid pass handle")) return;
		UseTexture(Pass, Texture, Range, ERDGUse::ReadWrite,
			ERHIAccess::DepthStencilReadWrite,
			LoadAction != ERHIRenderTargetLoadAction::Load);
		if (Pass.Owner == State->Owner && Pass.Index < State->Passes.size())
			State->Passes[Pass.Index].Uses.back().bStore =
				StoreAction == ERHIRenderTargetStoreAction::Store;
	}

	auto FRDGBuilder::UseManagedColorAttachment(
		FRDGPassHandle Pass, FRDGTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction, ERHIAccess ResultAccess) -> void
	{
		RequireBuilding();
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

	auto FRDGBuilder::UseManagedDepthStencilAttachment(
		FRDGPassHandle Pass, FRDGTextureHandle Texture,
		const FRHITextureSubresourceRange& Range,
		ERHIRenderTargetLoadAction LoadAction,
		ERHIRenderTargetStoreAction StoreAction, ERHIAccess ResultAccess) -> void
	{
		RequireBuilding();
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

	auto FRDGBuilder::UseManagedTexture(FRDGPassHandle Pass,
		FRDGTextureHandle Texture,
		const FRHITextureSubresourceRange& Range, ERDGUse Use,
		ERHIAccess EntryAccess, ERHIAccess ResultAccess, bool bDiscard) -> void
	{
		RequireBuilding();
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

	auto FRDGBuilder::UseToken(FRDGPassHandle Pass,
		FRDGTokenHandle Token, ERDGUse Use) -> void
	{
		RequireBuilding();
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
		RequireBuilding();
		if (!CanDeclareManualUse(Pass,
			"typed value use has an invalid pass handle")) return;
		if (Use != ERDGUse::Read && Use != ERDGUse::Write)
		{
			State->DeclarationErrors.push_back("pass '"
				+ State->Passes[Pass.Index].Name
				+ "' declares a typed value with invalid read/write direction");
			return;
		}
		if (Owner != State->Owner || Index >= State->Resources.size()
			|| State->Resources[Index].ValueTypeIdentity == nullptr
			|| State->Resources[Index].ValueTypeIdentity != TypeIdentity)
		{
			State->DeclarationErrors.push_back("pass '"
				+ State->Passes[Pass.Index].Name
				+ "' declares an invalid, foreign, or wrongly typed graph value");
			return;
		}
		FGraphUse DeclaredUse;
		DeclaredUse.ResourceIndex = Index;
		DeclaredUse.Kind = ERDGResourceKind::Token;
		DeclaredUse.Use = Use;
		DeclaredUse.bDiscard = Use == ERDGUse::Write;
		State->Passes[Pass.Index].Uses.push_back(std::move(DeclaredUse));
	}

	auto FRDGBuilder::Compile() -> std::string
	{
		if (State->PendingConstructions != 0)
			return "render graph storage construction is incomplete";
		const auto Started = std::chrono::steady_clock::now();
		auto Fail = [](std::string Error) {
			return Error;
		};
		if (!State->DeclarationErrors.empty())
			return Fail(State->DeclarationErrors.front());
		if (std::string Error = ValidateGraphResources(State->Resources);
			!Error.empty())
			return Fail(std::move(Error));

		// Compiler scratch copies declarations only; callbacks keep one owner.
		std::vector<FGraphPass> Passes;
		Passes.reserve(State->Passes.size() + 1);
		for (const auto& Pass : State->Passes)
			Passes.push_back({.Name = Pass.Name, .Type = Pass.Type,
				.Uses = Pass.Uses, .Prerequisites = Pass.Prerequisites,
				.bRoot = Pass.bRoot, .bExport = Pass.bExport,
				.RootReason = Pass.RootReason, .bParameterized = Pass.bParameterized,
				.ParameterLayout = Pass.ParameterLayout, .Parameters = Pass.Parameters,
				.OptionalAliases = Pass.OptionalAliases,
				.ParameterCaptures = Pass.ParameterCaptures});
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
			while (std::ranges::any_of(Passes, [&](const FGraphPass& Pass) {
				return Pass.Name == Export.Name;
			}))
				Export.Name += ".Output";
			Passes.push_back(std::move(Export));
		}

		const uint32 PassCount = static_cast<uint32>(Passes.size());
		const uint32 ResourceCount = static_cast<uint32>(State->Resources.size());
		size_t DeclaredUseCount = 0;
		size_t ExplicitDependencyCount = 0;
		for (const auto& Pass : Passes)
		{
			DeclaredUseCount += Pass.Uses.size();
			ExplicitDependencyCount += Pass.Prerequisites.size();
		}
		if (PassCount > State->Budget.MaxPasses)
			return Fail("render graph safety limit exceeded: passes actual="
				+ std::to_string(PassCount) + " limit=" + std::to_string(State->Budget.MaxPasses));
		FDependencyGraph DependencyGraph{
			.Outgoing = std::vector<std::vector<uint32>>(PassCount),
			.Indegree = std::vector<uint32>(PassCount, 0),
		};
		DependencyGraph.Dependencies.reserve(
			ExplicitDependencyCount + DeclaredUseCount);
		if (std::string Error = ValidateGraphPasses(Passes,
			State->Resources, DependencyGraph); !Error.empty())
			return Fail(std::move(Error));

		const FResourceUseTable ResourceUses = BuildResourceUseTable(
			Passes, ResourceCount);
		if (std::string Error = ValidateTypedValueWriters(State->Resources,
			ResourceUses); !Error.empty())
			return Fail(std::move(Error));

		std::vector<FRangeState> Cells = BuildRangeCells(State->Resources,
			ResourceUses, DeclaredUseCount);

		if (std::string Error = BuildHazardDependencies(Passes,
			State->Resources, Cells, DependencyGraph); !Error.empty())
			return Fail(std::move(Error));

		std::vector<uint32> Order;
		if (std::string Error = BuildStablePassOrder(DependencyGraph,
			PassCount, Order); !Error.empty())
			return Fail(std::move(Error));

		const std::vector<bool> Retained = FindRetainedPasses(Passes,
			DependencyGraph.Dependencies,
			State->bEnableCulling);

		auto CompiledState = std::make_unique<FRDGBuilder::FCompiledState>();
		CompiledState->Owner = State->Owner;
		CompiledState->Resources = State->Resources;
		CompiledState->Budget = State->Budget;
		CompiledState->Passes.reserve(PassCount);
		CompiledState->RuntimePasses.reserve(PassCount);
		CompiledState->Dependencies.reserve(
			DependencyGraph.Dependencies.size());
		CompiledState->ResourceLifetimes.reserve(ResourceCount);
		CompiledState->CullingDecisions.reserve(PassCount);
		CompiledState->ResourceCaptures.reserve(ResourceCount);
		for (const auto& Pass : Passes)
			CompiledState->ParameterCaptures.insert(
				CompiledState->ParameterCaptures.end(),
				Pass.ParameterCaptures.begin(), Pass.ParameterCaptures.end());
		CompiledState->UseCaptures.reserve(DeclaredUseCount);
		CompiledState->TransitionCaptures.reserve(DeclaredUseCount * 2);
		CompiledState->AllocationRequests.reserve(ResourceCount);
		for (const auto& Edge : DependencyGraph.Dependencies)
			if (Retained[Edge.BeforePass] && Retained[Edge.AfterPass])
				CompiledState->Dependencies.push_back(Edge);
		for (uint32 ResourceIndex = 0; ResourceIndex < State->Resources.size(); ++ResourceIndex)
		{
			const auto& Resource = State->Resources[ResourceIndex];
			CompiledState->ResourceLifetimes.push_back({Resource.Name, std::numeric_limits<uint32>::max(), 0, Resource.bExternal, true});
			CompiledState->ResourceCaptures.push_back({ResourceIndex, Resource.Name, Resource.Kind, Resource.bExternal, "unused"});
			auto& Capture = CompiledState->ResourceCaptures.back();
			Capture.ValueType = Resource.ValueTypeName;
			Capture.TextureFormat = Resource.TextureDesc.Format;
			Capture.TextureExtent = Resource.TextureDesc.Extent;
			Capture.TextureArraySize = Resource.TextureDesc.ArraySize;
			Capture.TextureMips = Resource.TextureDesc.NumMips;
			Capture.BufferSize = Resource.BufferDesc.Size;
			Capture.BufferStride = Resource.BufferDesc.Stride;
		}
		for (uint32 Index = 0; Index < PassCount; ++Index)
			CompiledState->CullingDecisions.push_back({Passes[Index].Name,
				!Retained[Index], Retained[Index]
					? (Passes[Index].bRoot ? Passes[Index].RootReason : "value dependency")
					: "unreachable from an explicit root"});

		std::vector<FRangeState> ExecutionCells = Cells;
		for (auto& Cell : ExecutionCells)
		{
			const auto& Resource = State->Resources[Cell.Use.ResourceIndex];
			Cell.Access = Resource.InitialAccess;
			Cell.bProduced = Resource.HasInitialContents();
			Cell.Version = 0;
		}
		for (uint32 ScheduledIndex : Order)
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
				.Execute = ScheduledIndex < State->Passes.size()
					? &State->Passes[ScheduledIndex].Execute : nullptr,
				.ParameterizedExecute = ScheduledIndex < State->Passes.size()
					? &State->Passes[ScheduledIndex].ParameterizedExecute : nullptr,
				.ParameterLayout = Pass.ParameterLayout,
				.Parameters = Pass.Parameters,
				.OptionalAliases = Pass.OptionalAliases};
			Runtime.ResourceIndices.reserve(Pass.Uses.size());
			Runtime.ValueUses.reserve(Pass.Uses.size());
			Runtime.BufferTransitionResources.reserve(Pass.Uses.size());
			Runtime.TextureTransitionResources.reserve(Pass.Uses.size());
			CompiledPass.BufferTransitions.reserve(Pass.Uses.size());
			CompiledPass.TextureTransitions.reserve(Pass.Uses.size());
			for (const auto& Use : Pass.Uses)
			{
				if (std::ranges::find(Runtime.ResourceIndices, Use.ResourceIndex)
					== Runtime.ResourceIndices.end())
					Runtime.ResourceIndices.push_back(Use.ResourceIndex);
				if (State->Resources[Use.ResourceIndex].ValueTypeIdentity != nullptr)
					Runtime.ValueUses.emplace_back(Use.ResourceIndex, Use.Use);
				auto& Lifetime = CompiledState->ResourceLifetimes[Use.ResourceIndex];
				Lifetime.FirstPass = std::min(Lifetime.FirstPass, CompiledPassIndex);
				Lifetime.LastPass = CompiledPassIndex;
				Lifetime.bCulled = false;
				const auto& Resource = State->Resources[Use.ResourceIndex];
				for (auto& Cell : ExecutionCells)
				{
					if (!ContainsRange(Use, Cell.Use)) continue;
					const ERHIAccess Before = Cell.Access;
					// Equal access states still need a memory dependency when the
					// preceding access can write (for example, consecutive UAV uses).
					const bool bNeedsBarrier = Before != Use.Access
						|| AccessHasWrite(Before) || Use.bDiscard;
					if (Use.Kind != ERDGResourceKind::Token
						&& !Use.bPassManagedTransition
						&& bNeedsBarrier)
					{
						if (Use.Kind == ERDGResourceKind::Texture)
						{
							CompiledPass.TextureTransitions.push_back({Resource.Texture.GetReference(), Cell.Use.TextureRange, Before, Use.Access, Use.bDiscard});
							Runtime.TextureTransitionResources.push_back(Use.ResourceIndex);
						}
						else
						{
							CompiledPass.BufferTransitions.push_back({Resource.Buffer.GetReference(), Cell.Use.BufferOffset, Cell.Use.BufferSize, Before, Use.Access, Use.bDiscard});
							Runtime.BufferTransitionResources.push_back(Use.ResourceIndex);
						}
						CompiledState->TransitionCaptures.push_back({Use.ResourceIndex,
							CompiledPassIndex, Before, Use.Access, Cell.Use.TextureRange,
							Cell.Use.BufferOffset, Cell.Use.BufferSize, false, Use.bDiscard});
					}
					else if (Use.bPassManagedTransition)
					{
						if (bNeedsBarrier)
						{
							if (Use.Kind == ERDGResourceKind::Texture)
							{
								CompiledPass.TextureTransitions.push_back({Resource.Texture.GetReference(), Cell.Use.TextureRange, Before, Use.Access, Use.bDiscard});
								Runtime.TextureTransitionResources.push_back(Use.ResourceIndex);
							}
							else
							{
								CompiledPass.BufferTransitions.push_back({Resource.Buffer.GetReference(), Cell.Use.BufferOffset, Cell.Use.BufferSize, Before, Use.Access, Use.bDiscard});
								Runtime.BufferTransitionResources.push_back(Use.ResourceIndex);
							}
						}
						CompiledState->TransitionCaptures.push_back({Use.ResourceIndex,
							CompiledPassIndex, Before, Use.Access, Cell.Use.TextureRange,
							Cell.Use.BufferOffset, Cell.Use.BufferSize, false, Use.bDiscard});
						CompiledState->TransitionCaptures.push_back({Use.ResourceIndex,
							CompiledPassIndex, Use.Access, Use.ResultAccess,
							Cell.Use.TextureRange, Cell.Use.BufferOffset,
							Cell.Use.BufferSize, false});
					}
					if (IsWriteUse(Use.Use)) ++Cell.Version;
					CompiledState->UseCaptures.push_back({ScheduledIndex, Use.ResourceIndex,
						Use.Use, Use.Access, Cell.Use.TextureRange, Cell.Use.BufferOffset,
						Cell.Use.BufferSize, Cell.Version, Use.bDiscard, Use.bStore,
						std::string(Use.ParameterPath),
						std::string(Use.ShaderBindingName),
						Use.ShaderBindingType});
					Cell.Access = Use.Kind == ERDGResourceKind::Token
						? ERHIAccess::None
						: (Use.bPassManagedTransition ? Use.ResultAccess : Use.Access);
					Cell.bProduced = Use.bStore && (Cell.bProduced || IsWriteUse(Use.Use));
				}
			}
			CompiledState->Passes.push_back(std::move(CompiledPass));
			CompiledState->RuntimePasses.push_back(std::move(Runtime));
		}

		for (const auto& Cell : ExecutionCells)
		{
			const auto& Resource = State->Resources[Cell.Use.ResourceIndex];
			if (Cell.Use.Kind == ERDGResourceKind::Token
				|| CompiledState->ResourceLifetimes[Cell.Use.ResourceIndex].bCulled
				|| Resource.FinalAccess == ERHIAccess::None
				|| Resource.FinalAccess == Cell.Access) continue;
			if (Cell.Use.Kind == ERDGResourceKind::Texture)
			{
				CompiledState->FinalTextureTransitions.push_back({Resource.Texture.GetReference(), Cell.Use.TextureRange, Cell.Access, Resource.FinalAccess});
				CompiledState->FinalTextureTransitionResources.push_back(
					Cell.Use.ResourceIndex);
			}
			else
			{
				CompiledState->FinalBufferTransitions.push_back({Resource.Buffer.GetReference(), Cell.Use.BufferOffset, Cell.Use.BufferSize, Cell.Access, Resource.FinalAccess});
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
			if (Lifetime.bCulled)
			{
				Capture.Preparation = "culled";
				Capture.AllocationDisposition = "culled";
			}
			else if (Resource.bExternal)
			{
				Capture.Preparation = "external";
				Capture.AllocationDisposition = "external";
			}
			else if (Resource.Kind == ERDGResourceKind::Token)
			{
				Capture.Preparation = "logical";
				Capture.AllocationDisposition = "none";
			}
			else
			{
				Capture.Preparation = "requested";
				Capture.AllocationDisposition = "pending";
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
			-> std::string {
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
		Compiled = std::move(CompiledState);
		State->bCompiled = true;
		return {};
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
		return Compiled->CullingDecisions;
	}
	auto FRDGBuilder::GetFinalBufferTransitions() const
		-> std::span<const FRHIBufferTransition> { return Compiled->FinalBufferTransitions; }
	auto FRDGBuilder::GetFinalTextureTransitions() const
		-> std::span<const FRHITextureTransition> { return Compiled->FinalTextureTransitions; }
	auto FRDGBuilder::GetCompileMicroseconds() const -> uint64
	{
		return Compiled->CompileMicroseconds;
	}

	auto FRDGBuilder::GetBudget() const -> const FRDGBudget&
	{
		return State->Budget;
	}

	auto FRDGBuilder::GetStatistics() const -> FRDGStatistics
	{
		FRDGStatistics Result;
		Result.DeclaredPasses = static_cast<uint32>(Compiled->CullingDecisions.size());
		Result.ScheduledPasses = static_cast<uint32>(Compiled->Passes.size());
		Result.CulledPasses = Result.DeclaredPasses - Result.ScheduledPasses;
		Result.Dependencies = static_cast<uint32>(Compiled->Dependencies.size());
		Result.BufferTransitions = static_cast<uint32>(
			Compiled->FinalBufferTransitions.size());
		Result.TextureTransitions = static_cast<uint32>(
			Compiled->FinalTextureTransitions.size());
		for (const auto& Pass : Compiled->Passes)
		{
			Result.BufferTransitions += static_cast<uint32>(Pass.BufferTransitions.size());
			Result.TextureTransitions += static_cast<uint32>(Pass.TextureTransitions.size());
		}
		Result.CompileMicroseconds = Compiled->CompileMicroseconds;
		Result.ExecuteMicroseconds =
			Compiled->ExecuteMicroseconds;
		Result.bPassRegressionBudgetExceeded = Result.DeclaredPasses
			> Compiled->Budget.RegressionMaxPasses;
		Result.bDependencyRegressionBudgetExceeded = Result.Dependencies
			> Compiled->Budget.RegressionMaxDependencies;
		Result.bBufferTransitionRegressionBudgetExceeded = Result.BufferTransitions
			> Compiled->Budget.RegressionMaxBufferTransitions;
		Result.bTextureTransitionRegressionBudgetExceeded = Result.TextureTransitions
			> Compiled->Budget.RegressionMaxTextureTransitions;
		Result.bCompileBudgetExceeded = Result.CompileMicroseconds
			> Compiled->Budget.MaxCompileMicroseconds;
		Result.bExecuteBudgetExceeded = Result.ExecuteMicroseconds
			> Compiled->Budget.MaxExecuteMicroseconds;
		return Result;
	}

	auto FRDGBuilder::Capture() const -> FRDGCapture
	{
		FRDGCapture Result;
		Result.bCompiled = State->bCompiled;
		Result.Budget = State->Budget;
		Result.Statistics = GetStatistics();
		Result.AllocationStatistics = Compiled->AllocationStatistics;
		Result.Resources = Compiled->ResourceCaptures;
		Result.Parameters = Compiled->ParameterCaptures;
		Result.Uses = Compiled->UseCaptures;
		Result.Transitions = Compiled->TransitionCaptures;
		Result.Dependencies = Compiled->Dependencies;
		Result.ResourceLifetimes = Compiled->ResourceLifetimes;
		Result.CullingDecisions = Compiled->CullingDecisions;
		Result.Dump = Dump();
		Result.Passes.reserve(Compiled->Passes.size());
		for (const auto& Pass : Compiled->Passes)
			Result.Passes.push_back({Pass.Name, Pass.Type, Pass.DeclarationIndex,
				Pass.ParameterStructName,
				static_cast<uint32>(Pass.BufferTransitions.size()),
				static_cast<uint32>(Pass.TextureTransitions.size())});
		return Result;
	}

	auto FRDGBuilder::Dump() const -> std::string
	{
		if (!State->bCompiled) return "render-graph: no compiled plan";
		std::ostringstream Output;
		Output << "render-graph passes=" << Compiled->Passes.size()
			<< " edges=" << Compiled->Dependencies.size() << '\n';
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
				<< " buffers=" << Pass.BufferTransitions.size()
				<< " textures=" << Pass.TextureTransitions.size();
			if (!Pass.ParameterStructName.empty())
				Output << " parameters=" << Pass.ParameterStructName;
			Output << '\n';
		}
		for (const auto& Edge : Compiled->Dependencies)
			Output << "edge " << Edge.BeforePass << "->" << Edge.AfterPass
				<< " kind=" << DependencyKindName(Edge.Kind)
				<< " cause=" << Edge.Cause << '\n';
		Output << "final buffers=" << Compiled->FinalBufferTransitions.size()
			<< " textures=" << Compiled->FinalTextureTransitions.size() << '\n';
		for (const auto& Lifetime : Compiled->ResourceLifetimes)
			Output << "lifetime name=" << Lifetime.Name << " first="
				   << Lifetime.FirstPass << " last=" << Lifetime.LastPass
				   << " external=" << Lifetime.bExternal
				   << " culled=" << Lifetime.bCulled << '\n';
		for (const auto& Decision : Compiled->CullingDecisions)
			Output << "culling name=" << Decision.Name << " culled="
				<< Decision.bCulled << " reason=" << Decision.Reason << '\n';
		for (const auto& Resource : Compiled->ResourceCaptures)
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
		for (const auto& Parameter : Compiled->ParameterCaptures)
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
		for (const auto& Use : Compiled->UseCaptures)
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
		for (const auto& Transition : Compiled->TransitionCaptures)
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
				<< " final=" << Transition.bFinal << '\n';
		return Output.str();
	}

	auto FRDGBuilder::Record(
		FRHICommandListImmediate& CommandList, FRDGExecutionContext* Context,
		std::string* OutError) -> bool
	{
		const auto Started = std::chrono::steady_clock::now();
		auto RecordDuration = [&] {
			Compiled->ExecuteMicroseconds = static_cast<uint64>(
				std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - Started).count());
		};
		if (Context != nullptr && !Compiled->AllocationRequests.empty())
		{
			FRDGAllocatedResources Candidate(
				static_cast<uint32>(Compiled->Resources.size()));
			std::string Error;
			const bool bAllocated = Context->Allocator.Allocate(
				Compiled->AllocationRequests, Candidate, Error);
			Compiled->AllocationStatistics = Candidate.Statistics;
			if (!bAllocated)
			{
				if (OutError != nullptr) *OutError = std::move(Error);
				RecordDuration();
				return false;
			}
			for (const FRDGAllocationRequest& Request : Compiled->AllocationRequests)
			{
				const bool bReady = Request.Kind == ERDGResourceKind::Texture
					? static_cast<bool>(Candidate.Textures[Request.ResourceId])
					: static_cast<bool>(Candidate.Buffers[Request.ResourceId]);
				if (!bReady)
				{
					if (OutError != nullptr)
						*OutError = "RDG allocator omitted retained resource id="
							+ std::to_string(Request.ResourceId);
					RecordDuration();
					return false;
				}
				if (Request.Kind == ERDGResourceKind::Texture)
				{
					const FRHITextureDesc Actual = DescribeTexture(
						*Candidate.Textures[Request.ResourceId]);
					if (!TextureBackingIsCompatible(Actual, Request.TextureDesc))
					{
						if (OutError != nullptr) *OutError =
							"RDG allocator returned incompatible texture id="
							+ std::to_string(Request.ResourceId);
						RecordDuration();
						return false;
					}
				}
				else if (!BufferBackingIsCompatible(
					Candidate.Buffers[Request.ResourceId]->GetDesc(),
					Request.BufferDesc))
				{
					if (OutError != nullptr) *OutError =
						"RDG allocator returned incompatible buffer id="
						+ std::to_string(Request.ResourceId);
					RecordDuration();
					return false;
				}
			}
			for (const FRDGAllocationRequest& Request : Compiled->AllocationRequests)
			{
				auto& Resource = Compiled->Resources[Request.ResourceId];
				auto& Capture = Compiled->ResourceCaptures[Request.ResourceId];
				Capture.AllocationDisposition =
					Candidate.AllocationDispositions[Request.ResourceId];
				Capture.PhysicalAllocationId =
					Candidate.AllocationIds[Request.ResourceId];
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
			if (OutError != nullptr)
				*OutError = "retained graph resources require an RDG execution allocator";
			RecordDuration();
			return false;
		}
		State->Lifecycle = ERDGBuilderState::Recording;
		State->ExecutionResult.Status = ERDGExecutionStatus::InvalidState;
		State->ExecutionResult.Error = "render graph recording did not complete";
		for (uint32 Index = 0; Index < Compiled->Passes.size(); ++Index)
		{
			auto& Pass = Compiled->Passes[Index];
			auto& Runtime = Compiled->RuntimePasses[Index];
			for (uint32 TransitionIndex = 0;
				TransitionIndex < Pass.BufferTransitions.size(); ++TransitionIndex)
				Pass.BufferTransitions[TransitionIndex].Buffer = Compiled->Resources[Runtime.BufferTransitionResources[TransitionIndex]].Buffer.GetReference();
			for (uint32 TransitionIndex = 0;
				TransitionIndex < Pass.TextureTransitions.size(); ++TransitionIndex)
				Pass.TextureTransitions[TransitionIndex].Texture = Compiled->Resources[Runtime.TextureTransitionResources[TransitionIndex]].Texture.GetReference();
			if (!Pass.BufferTransitions.empty())
				CommandList.TransitionBuffers(Pass.BufferTransitions);
			if (!Pass.TextureTransitions.empty())
				CommandList.TransitionTextures(Pass.TextureTransitions);
			if (Runtime.ParameterizedExecute != nullptr && *Runtime.ParameterizedExecute)
			{
				const FRDGPassResources Resources(*this, Index);
				const FRDGParameterResolver Resolver(Resources,
					Runtime.ParameterLayout, Runtime.OptionalAliases.View(),
					Runtime.Parameters,
					Pass.Name, Pass.Type);
				(*Runtime.ParameterizedExecute)(CommandList, Resolver);
			}
			else if (Runtime.Execute != nullptr && *Runtime.Execute)
			{
				const FRDGPassResources Resources(*this, Index);
				(*Runtime.Execute)(CommandList, Resources);
			}
		}
		for (uint32 Index = 0; Index < Compiled->FinalBufferTransitions.size(); ++Index)
			Compiled->FinalBufferTransitions[Index].Buffer = Compiled->Resources[Compiled->FinalBufferTransitionResources[Index]].Buffer.GetReference();
		for (uint32 Index = 0; Index < Compiled->FinalTextureTransitions.size(); ++Index)
			Compiled->FinalTextureTransitions[Index].Texture = Compiled->Resources[Compiled->FinalTextureTransitionResources[Index]].Texture.GetReference();
		if (!Compiled->FinalBufferTransitions.empty())
			CommandList.TransitionBuffers(Compiled->FinalBufferTransitions);
		if (!Compiled->FinalTextureTransitions.empty())
			CommandList.TransitionTextures(Compiled->FinalTextureTransitions);
		for (const auto& Resource : Compiled->Resources)
		{
			if (Resource.TextureDestination != nullptr)
				*Resource.TextureDestination = Resource.Texture;
			if (Resource.BufferDestination != nullptr)
				*Resource.BufferDestination = Resource.Buffer;
		}
		if (OutError != nullptr) OutError->clear();
		RecordDuration();
		return true;
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
		requiref(Resource.Kind == ERDGResourceKind::Texture && Resource.Texture, "Render graph callback resolved an unavailable texture.");
		return Resource.Texture.GetReference();
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
		requiref(Resource.Kind == ERDGResourceKind::Buffer && Resource.Buffer, "Render graph callback resolved an unavailable buffer.");
		return Resource.Buffer.GetReference();
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
