#include "RDGBuilderInternal.h"
#include "Misc/Time.h"

namespace Durin::RDGPrivate
{
	namespace
	{
		struct FLoweredParameterUse final
		{
			FGraphUse Use;
			bool bPresent = false;
		};
		auto ResourceContract(const FGraphResource& Resource) -> FRDGResourceContractContext;
		auto ExternalContractsEqual(const FGraphResource& Left, const FGraphResource& Right) -> bool;

		auto ResourceContract(const FGraphResource& Resource) -> FRDGResourceContractContext
		{
			return {Resource.Name, Resource.Kind, Resource.TextureDesc, Resource.BufferDesc, Resource.InitialAccess, Resource.FinalAccess};
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

}

namespace Durin
{
	using namespace RDGPrivate;

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

	auto FRDGBuilder::GetExecutionResult() const -> const std::optional<FRDGExecutionResult>&
	{ return State->ExecutionResult; }

	auto FRDGBuilder::HasCompiledPlan() const -> bool
	{ return State->bCompiled; }

	auto FRDGBuilder::CompileForTesting() -> FRDGCompileResult
	{
		if (State->Lifecycle != ERDGBuilderState::Building)
			return std::unexpected(ERDGStateError::BuilderConsumed);
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
		State->Lifecycle = Error.has_value() ? ERDGBuilderState::Preparing : ERDGBuilderState::Failed;
		return Error;
	}

	auto FRDGBuilder::AllocateParameterStorage(size_t Size,
		size_t Alignment, const FRDGParametersMetadata* Metadata,
		const FRDGParameterLayoutBuildResult& LayoutResult,
		void (*Destroy)(void*), std::weak_ptr<void>& OutLifetime,
		size_t& OutAllocationIndex) -> void*
	{
		RequireBuilding();
		if (!LayoutResult)
		{
			State->DeclarationErrors.push_back(LayoutResult.error());
			return nullptr;
		}
		if ((*LayoutResult)->Metadata != Metadata
			|| Metadata->StructSize != Size
			|| Metadata->StructAlignment != Alignment)
		{
			State->DeclarationErrors.push_back(FRDGMetadataError{ERDGMetadataError::ParameterLayoutMismatch,
				FRDGMetadataErrorContext{.ExpectedSize = Size,
					.ActualSize = Metadata ? Metadata->StructSize : 0,
					.ExpectedAlignment = Alignment,
					.ActualAlignment = Metadata ? Metadata->StructAlignment : 0}});
			return nullptr;
		}
		auto Allocation = std::make_shared<FGraphParameterAllocation>(
			Size, Alignment, Destroy);
		Allocation->Layout = LayoutResult->get();
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
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::ValueStorageInvalid,
				FRDGIdentityErrorContext{.Name = std::string(Name),
					.TypeName = std::string(StableTypeName),
					.Expected = Size,
					.Actual = Alignment}});
			return nullptr;
		}
		for (const auto& Existing : State->Resources)
		{
			if (Existing.ValueTypeIdentity == nullptr) continue;
			if (Existing.ValueTypeIdentity == TypeIdentity
				&& Existing.ValueTypeName != StableTypeName)
			{
				State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::ValueTypeNameChanged,
					FRDGIdentityErrorContext{.Name = std::string(Name),
						.OtherName = Existing.ValueTypeName,
						.TypeName = std::string(StableTypeName)}});
				return nullptr;
			}
			if (Existing.ValueTypeIdentity != TypeIdentity
				&& Existing.ValueTypeName == StableTypeName)
			{
				State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::ValueTypeNameReused,
					FRDGIdentityErrorContext{.Name = std::string(Name),
						.TypeName = std::string(StableTypeName)}});
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
					State->DeclarationErrors.push_back(FRDGExternalConflictError{ResourceContract(Canonical), ResourceContract(Resource)});
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
					State->DeclarationErrors.push_back(FRDGExternalConflictError{ResourceContract(Canonical), ResourceContract(Resource)});
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
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::TextureExtractionHandleInvalid,
				FRDGIdentityErrorContext{.Index = Texture.Index,
					.Actual = static_cast<uint64>(FinalAccess)}});
			return;
		}
		if (Destination == nullptr || FinalAccess == ERHIAccess::None
			|| EnumHasAnyFlags(FinalAccess, ERHIAccess::Discard))
		{
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::TextureExtractionInvalid,
				FRDGIdentityErrorContext{.Index = Texture.Index,
					.Actual = static_cast<uint64>(FinalAccess)}});
			return;
		}
		if (std::ranges::any_of(State->Resources,
			[&](const FGraphResource& Existing) {
				return (&Existing == &State->Resources[Texture.Index]
						&& Existing.IsExported())
					|| Existing.TextureDestination == Destination;
			}))
		{
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::TextureExtractionDuplicate,
				FRDGIdentityErrorContext{.Index = Texture.Index,
					.Actual = static_cast<uint64>(FinalAccess)}});
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
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::BufferExtractionHandleInvalid,
				FRDGIdentityErrorContext{.Index = Buffer.Index,
					.Actual = static_cast<uint64>(FinalAccess)}});
			return;
		}
		if (Destination == nullptr || FinalAccess == ERHIAccess::None
			|| EnumHasAnyFlags(FinalAccess, ERHIAccess::Discard))
		{
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::BufferExtractionInvalid,
				FRDGIdentityErrorContext{.Index = Buffer.Index,
					.Actual = static_cast<uint64>(FinalAccess)}});
			return;
		}
		if (std::ranges::any_of(State->Resources,
			[&](const FGraphResource& Existing) {
				return (&Existing == &State->Resources[Buffer.Index]
						&& Existing.IsExported())
					|| Existing.BufferDestination == Destination;
			}))
		{
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::BufferExtractionDuplicate,
				FRDGIdentityErrorContext{.Index = Buffer.Index,
					.Actual = static_cast<uint64>(FinalAccess)}});
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

		auto* Allocation = AllocationIndex < State->ParameterStorage.Allocations.size()
			? State->ParameterStorage.Allocations[AllocationIndex].get() : nullptr;
		if (Parameters == nullptr || Lifetime == nullptr || Layout == nullptr
			|| Allocation == nullptr || Allocation != Lifetime.get()
			|| Allocation->Data != Parameters || Allocation->Layout != Layout
			|| !Allocation->bConstructed)
		{
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::ParameterAllocationInvalid,
				FRDGIdentityErrorContext{.Name = std::string(Name),
					.TypeName = Metadata && Metadata->StructName ? Metadata->StructName : "FParameters",
					.Index = AllocationIndex}});
			return {};
		}
		if (Allocation->bFrozen)
		{
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::ParameterAllocationSubmitted,
				FRDGIdentityErrorContext{.Name = std::string(Name),
					.TypeName = Metadata && Metadata->StructName ? Metadata->StructName : "FParameters",
					.Index = AllocationIndex}});
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
			!UseError.has_value())
		{
			if (auto* Context = &UseError.error().Context)
				Context->PassIndex = static_cast<uint32>(State->Passes.size());
			State->DeclarationErrors.push_back(std::move(UseError.error()));
			return {};
		}
		ParameterizedPass.bDeclarationsValidated = true;
		const uint32 Index = static_cast<uint32>(State->Passes.size());
		State->Passes.push_back(std::move(ParameterizedPass));
		return {State->Owner, Index};
	}

	auto FRDGBuilder::CanDeclareManualUse(FRDGPassHandle Pass) -> bool
	{
		RequireBuilding();
		if (Pass.Owner != State->Owner || Pass.Index >= State->Passes.size())
		{
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::PassHandleInvalid,
				FRDGIdentityErrorContext{.Index = Pass.Index}});
			return false;
		}
		if (State->Passes[Pass.Index].ParameterLayout != nullptr)
		{
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::ManualUseOnParameterizedPass,
				FRDGIdentityErrorContext{.Name = State->Passes[Pass.Index].Name,
					.Index = Pass.Index}});
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
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::RootHandleInvalid,
				FRDGIdentityErrorContext{.Index = Pass.Index}});
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
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::AsyncPassInvalid,
				FRDGIdentityErrorContext{.Index = Pass.Index}});
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
			State->DeclarationErrors.push_back(FRDGDependencyError{ERDGDependencyError::ConsumerHandleInvalid, Producer.Index, Consumer.Index});
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
		if (!CanDeclareManualUse(Pass)) return;
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
		if (!CanDeclareManualUse(Pass)) return;
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
		if (!CanDeclareManualUse(Pass)) return;
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
		if (!CanDeclareManualUse(Pass)) return;
		if (Use != ERDGUse::Read && Use != ERDGUse::Write)
		{
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::ValueDirectionInvalid,
				FRDGIdentityErrorContext{.Name = State->Passes[Pass.Index].Name,
					.Index = Pass.Index,
					.OtherIndex = Index,
					.Actual = static_cast<uint64>(Use)}});
			return;
		}
		if (Owner != State->Owner || Index >= State->Resources.size()
			|| State->Resources[Index].ValueTypeIdentity == nullptr
			|| State->Resources[Index].ValueTypeIdentity != TypeIdentity)
		{
			State->DeclarationErrors.push_back(FRDGIdentityError{ERDGIdentityError::ValueHandleInvalid,
				FRDGIdentityErrorContext{.Name = State->Passes[Pass.Index].Name,
					.Index = Pass.Index,
					.OtherIndex = Index,
					.Actual = static_cast<uint64>(Use)}});
			return;
		}
		FGraphUse DeclaredUse;
		DeclaredUse.ResourceIndex = Index;
		DeclaredUse.Kind = ERDGResourceKind::Token;
		DeclaredUse.Use = Use;
		DeclaredUse.bDiscard = Use == ERDGUse::Write;
		State->Passes[Pass.Index].Uses.push_back(std::move(DeclaredUse));
	}

}
