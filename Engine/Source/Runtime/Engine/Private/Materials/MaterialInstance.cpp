#include "Materials/MaterialInstance.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Logging/LogMacros.h"

#include "Asset/Asset.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Archive.h"

namespace Durin
{
	namespace
	{
		auto WouldCreateParentCycle(
			const DMaterialInstance* Instance,
			const DMaterialInterface* CandidateParent
		) -> bool
		{
			std::unordered_set<const DMaterialInterface*> Visited;
			for (const DMaterialInterface* Candidate = CandidateParent;
				Candidate != nullptr;
				Candidate = Candidate->GetParent())
			{
				if (Candidate == Instance || !Visited.insert(Candidate).second
					|| Visited.size() >= MaterialMaximumParentDepth) return true;
			}
			return false;
		}

		auto IsParameterAvailableForOverride(
			const DMaterialInterface& Material, const FGuid& Id) -> bool
		{
			// Authored edits may precede asynchronous compilation. This query governs
			// editing/orphan diagnostics only; render proxies use the accepted contract.
			if (!GetAssetRuntimeConfiguration().RequiresCookedPayload())
			{
				FMaterialIRCompilerInput Snapshot;
				if (!SnapshotMaterialCompilerInput(Material, {}, Snapshot)) return false;
				std::vector<uint32> Pending;
				if (Snapshot.IR.SurfaceRoot.bAggregate) Pending.push_back(Snapshot.IR.SurfaceRoot.AggregateExpressionIndex);
				else for (const auto& Input : Snapshot.IR.SurfaceRoot.Inputs)
					if (Input.bExpression) Pending.push_back(Input.ExpressionIndex);
				std::vector<bool> Visited(Snapshot.IR.Nodes.size());
				while (!Pending.empty())
				{
					const auto Index = Pending.back(); Pending.pop_back();
					if (Visited[Index]) continue;
					Visited[Index] = true;
					const auto& Node = Snapshot.IR.Nodes[Index];
					if (Node.GetParameterId() == Id) return true;
					Pending.insert(Pending.end(), Node.Inputs.begin(), Node.Inputs.end());
				}
				return false;
			}
			const auto Program = Material.GetAcceptedCompiledProgram();
			if (!Program) return false;
			return std::ranges::find(Program->ActiveParameters, Id,
				&FMaterialCompilerParameterDeclaration::Id) != Program->ActiveParameters.end();
		}

	}

	DMaterialInstance::DMaterialInstance(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		if (!IsTemplateConstructionPurpose(ObjectInitializer.Purpose)) PublishMaterialRenderProxyState();
	}

	auto DMaterialInstance::ValidateOverrideStorage(const FPropertyEditProposal* Proposal) const -> bool
	{
		std::unordered_set<FGuid> Ids;
		bool bValid = true;
		VisitOverrideArrays([&](const auto& Stored) {
			using TArray = std::decay_t<decltype(Stored)>;
			using TRecord = typename TArray::value_type;
			const TArray* Records = &Stored;
			if (Proposal && Proposal->MemberProperty && Proposal->MemberProperty->NamePrivate == TRecord::PropertyName())
			{
				if (Proposal->DraftRootProperty != Proposal->MemberProperty || !Proposal->DraftRootContainer)
				{
					bValid = false;
					return;
				}
				Records = Proposal->DraftRootProperty->ContainerPtrToValuePtr<TArray>(
					Proposal->DraftRootContainer, Proposal->DraftRootArrayIndex);
			}
			for (const auto& Record : *Records)
			{
				if (!Record.ParameterId.IsValid() || !Ids.insert(Record.ParameterId).second) bValid = false;
				if constexpr (TRecord::Type == EMaterialParameterType::Texture)
					if (!IsValidMaterialSampling(Record.Value.SamplerState, Record.Value.TextureFallback)) bValid = false;
			}
		});
		return bValid;
	}

	auto DMaterialInstance::Serialize(FArchive& Ar) -> void
	{
		if (Ar.IsLoading()) OverrideStorageVersion = 0;
		Super::Serialize(Ar);
		if (Ar.HasError()) return;
		if (OverrideStorageVersion != 1)
			Ar.Fail(EArchiveFailureCode::UnsupportedVersion, "Unsupported material instance schema; rebuild this instance.");
		else if (!ValidateOverrideStorage())
			Ar.Fail(EArchiveFailureCode::InvalidData, "Invalid or duplicate typed material override.");
	}

	auto DMaterialInstance::SetParent(DMaterialInterface* InParent) -> bool
	{
		return SetParentAndPropertyOverrides(InParent, PropertyOverrides);
	}

	auto DMaterialInstance::SetParentAndPropertyOverrides(DMaterialInterface* InParent,
		const FMaterialPropertyOverrides& Overrides) -> bool
	{
		std::string Error;
		if (WouldCreateParentCycle(this, InParent)
			|| !ValidateMaterialStaticProperties(Overrides.Values, Error)) return false;
		const bool bParentChanged = Parent != InParent;
		if (!bParentChanged && PropertyOverrides == Overrides) return true;
		Parent = InParent;
		PropertyOverrides = Overrides;
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) AdoptParentRuntimeProgram();
		else InvalidateMaterialCompilation(true, !bParentChanged);
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::AllRenderState);
		return true;
	}

	auto DMaterialInstance::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!ValidateOverrideStorage(&Proposal))
		{
			OutError = "Invalid sampling policy or duplicate typed parameter identity.";
			return false;
		}
		if (Proposal.MemberProperty && Proposal.MemberProperty->NamePrivate == FName("PropertyOverrides")
			&& Proposal.DraftRootProperty == Proposal.MemberProperty && Proposal.DraftRootContainer)
		{
			const auto* Overrides = Proposal.DraftRootProperty->ContainerPtrToValuePtr<FMaterialPropertyOverrides>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			return ValidateMaterialStaticProperties(Overrides->Values, OutError);
		}
		if (!Proposal.MemberProperty || Proposal.MemberProperty->NamePrivate != FName("Parent")
			|| !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return true;
		if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
		{
			OutError = "The material parent metadata is unavailable.";
			return false;
		}
		DObject* Value = static_cast<const FObjectProperty*>(Proposal.DraftRootProperty)->GetObjectPropertyValue(
			Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		auto* CandidateParent = Value ? Cast<DMaterialInterface>(Value) : nullptr;
		if (Value && !CandidateParent)
		{
			OutError = "Selected asset is not a material.";
			return false;
		}
		if (WouldCreateParentCycle(this, CandidateParent))
		{
			OutError = "A material instance cannot create a parent cycle.";
			return false;
		}
		return true;
	}

	auto DMaterialInstance::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("PropertyOverrides"))
		{
			InvalidateMaterialCompilation(true, true);
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::AllRenderState);
		}
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("Parent"))
		{
			InvalidateMaterialCompilation();
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::AllRenderState);
		}
	}

	auto DMaterialInstance::GetParent() const -> DMaterialInterface*
	{
		return Parent.Get();
	}

	auto DMaterialInstance::GetStaticProperties() const -> const FMaterialStaticProperties&
	{
		FResolvedMaterialProperties Resolved;
		std::string Error;
		ResolvedStaticProperties = ResolveMaterialProperties(*this, Resolved, Error)
			? Resolved.Properties : FMaterialStaticProperties{};
		return ResolvedStaticProperties;
	}

	auto DMaterialInstance::GetRenderableStaticProperties() const
		-> FMaterialStaticProperties
	{
		return Super::GetRenderableStaticProperties();
	}

	auto DMaterialInstance::GetMaterialProgram() const
		-> std::optional<FMaterialProgram>
	{
		FResolvedMaterialProperties Resolved;
		std::string Error;
		if (!ResolveMaterialProperties(*this, Resolved, Error)) return std::nullopt;
		auto* Root = Cast<DMaterialInterface>(ResolveObjectHandle(Resolved.Root));
		return Root ? Root->GetMaterialProgram() : std::nullopt;
	}

	auto DMaterialInstance::GetAcceptedCompiledProgram() const
		-> std::shared_ptr<const FMaterialCompilerResult>
	{
		FResolvedMaterialProperties Resolved;
		std::string Error;
		if (!ResolveMaterialProperties(*this, Resolved, Error)) return nullptr;
		return Super::GetAcceptedCompiledProgram();
	}

	auto DMaterialInstance::SetPropertyOverrides(const FMaterialPropertyOverrides& Overrides) -> bool
	{
		return SetParentAndPropertyOverrides(Parent.Get(), Overrides);
	}

	auto DMaterialInstance::GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>
	{
		FResolvedMaterialProperties Resolved;
		std::string Error;
		if (!ResolveMaterialProperties(*this, Resolved, Error)) return {};
		auto* Root = Cast<DMaterialInterface>(ResolveObjectHandle(Resolved.Root));
		return Root ? Root->GetParameterDefinitions() : std::span<const FMaterialParameterDefinition>{};
	}

	auto DMaterialInstance::GetLocalParameterOverride(const FGuid& Id, FMaterialParameterValue& OutValue) const -> bool
	{
		bool bFound = false;
		VisitOverrideArrays([&](const auto& Records) {
			using TRecord = typename std::decay_t<decltype(Records)>::value_type;
			const auto It = std::ranges::find(Records, Id, &TRecord::ParameterId);
			if (It != Records.end()) { OutValue = It->GetValue(); bFound = true; }
		});
		return bFound;
	}

	auto DMaterialInstance::ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool
	{
		const auto* Definition = FindParameterDefinition(Id);
		if (!Definition) return false;
		// Property resolution already bounded and validated this chain. Walk it
		// once instead of recursively repeating declaration lookup at every layer.
		const DMaterialInterface* Owner = this;
		for (uint32 Depth = 0; Owner && Depth < MaterialMaximumParentDepth; ++Depth)
		{
			const auto* Instance = Cast<DMaterialInstance>(Owner);
			if (!Instance) return Owner->ResolveParameterValue(Id, OutParameter);
			FMaterialParameterValue Override;
			if (Instance->GetLocalParameterOverride(Id, Override) && Override.GetType() == Definition->Type)
			{
				OutParameter.Definition = Definition;
				OutParameter.Value = std::move(Override);
				OutParameter.Source = const_cast<DMaterialInstance*>(Instance);
				OutParameter.bHasLocalOverride = Instance == this;
				return true;
			}
			Owner = Instance->GetParent();
		}
		return false;
	}

	auto DMaterialInstance::SetParameterOverride(
		const FGuid& Id,
		const FMaterialParameterValue& Value
	) -> bool
	{
		const auto Type = Value.GetType();
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		if (!Definition || Definition->Type != Type
			|| !IsParameterAvailableForOverride(*this, Id)) return false;
		if (Type == EMaterialParameterType::Texture && !IsValidMaterialSampling(Value.GetTexture().SamplerState, Value.GetTexture().TextureFallback)) return false;
		FMaterialParameterValue Existing;
		if (GetLocalParameterOverride(Id, Existing))
		{
			if (Existing.GetType() != Type) return false;
			if (Existing == Value) return true;
		}
		VisitOverrideArrays([&](auto& Records) {
			using TRecord = typename std::decay_t<decltype(Records)>::value_type;
			if (TRecord::Type != Type) return;
			auto It = std::ranges::find(Records, Id, &TRecord::ParameterId);
			if (It == Records.end())
			{
				Records.emplace_back();
				It = std::prev(Records.end());
				It->ParameterId = Id;
			}
			It->SetValue(Value);
		});
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterialInstance::ClearParameterOverride(const FGuid& Id) -> bool
	{
		bool bRemoved = false;
		VisitOverrideArrays([&](auto& Records) {
			bRemoved |= std::erase_if(Records, [&](const auto& Record) { return Record.ParameterId == Id; }) != 0;
		});
		if (!bRemoved) return false;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterialInstance::HasLocalParameterOverride(const FGuid& Id) const -> bool
	{
		FMaterialParameterValue Value;
		return GetLocalParameterOverride(Id, Value);
	}

	auto DMaterialInstance::IsParameterOverrideOrphan(const FGuid& Id) const -> bool
	{
		FMaterialParameterValue Override;
		if (!GetLocalParameterOverride(Id, Override)) return false;
		const auto* Definition = FindParameterDefinition(Id);
		return !Definition || Definition->Type != Override.GetType()
			|| !IsParameterAvailableForOverride(*this, Id);
	}

	auto DMaterialInstance::SetScalarParameterValue(FName Name, float Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		return SetParameterOverride(
			Definition->Id, FMaterialParameterValue::MakeScalar(Value));
	}

	auto DMaterialInstance::SetVector2ParameterValue(FName Name, const FVector2& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector2) return false;
		return SetParameterOverride(
			Definition->Id, FMaterialParameterValue::MakeVector2(Value));
	}

	auto DMaterialInstance::SetVectorParameterValue(FName Name, const FVector3& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		return SetParameterOverride(
			Definition->Id, FMaterialParameterValue::MakeVector(Value));
	}

	auto DMaterialInstance::SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		Resolved.Value.GetTexture().Texture = Value;
		return SetParameterOverride(Definition->Id, Resolved.Value);
	}

	auto DMaterialInstance::ClearScalarParameterValue(FName Name) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Scalar
			&& ClearParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::ClearVector2ParameterValue(FName Name) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Vector2
			&& ClearParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::ClearVectorParameterValue(FName Name) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Vector
			&& ClearParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::ClearTextureParameterValue(FName Name) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Texture
			&& ClearParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::HasScalarParameterOverride(FName Name) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Scalar
			&& HasLocalParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::HasVector2ParameterOverride(FName Name) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Vector2
			&& HasLocalParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::HasVectorParameterOverride(FName Name) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Vector
			&& HasLocalParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::HasTextureParameterOverride(FName Name) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Texture
			&& HasLocalParameterOverride(Definition->Id);
	}

	auto DMaterialInstance::GetScalarParameterValue(FName Name, float& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		OutValue = Resolved.Value.GetScalar();
		return true;
	}

	auto DMaterialInstance::GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector2) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		OutValue = Resolved.Value.GetVector2();
		return true;
	}

	auto DMaterialInstance::GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		OutValue = Resolved.Value.GetVector();
		return true;
	}

	auto DMaterialInstance::GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		OutValue = Resolved.Value.GetTexture().Texture.Get();
		return true;
	}

	auto DMaterialInstance::BuildMaterialLocalRenderLayer() const
		-> FMaterialLocalRenderLayer
	{
		return Super::BuildMaterialLocalRenderLayer();
	}

	auto DMaterialInstance::PostLoad() -> void
	{
		Super::PostLoad();
		// Break corrupt parent chains before any parameter lookup can recurse.
		if (WouldCreateParentCycle(this, Parent.Get()))
		{
			DURIN_ERROR("PostLoad '{}': material instance parent cycle; clearing parent.", GetObjectPath());
			Parent = nullptr;
		}
		std::string Error;
		if (!ValidateMaterialStaticProperties(PropertyOverrides.Values, Error))
		{
			DURIN_ERROR("PostLoad '{}': {}; disabling static property overrides.", GetObjectPath(), Error);
			PropertyOverrides = {};
		}
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			CompilationOwner.RenderLayer = {};
			if (CookedProgramData.GetMetadata().LogicalSize == 0)
			{
				MaterialCookDiagnostic = "Cooked material instance requires its own ProgramData field.";
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), MaterialCookDiagnostic);
			}
		}
		else RequestMaterialRecompile(*this);
		PublishMaterialRenderProxyState();
	}
}
