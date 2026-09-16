#include "Materials/MaterialInstance.h"
#include "Materials/MaterialCustomVersion.h"
#include "Logging/LogMacros.h"

#include "Asset/Asset.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Archive.h"

namespace Durin
{
	namespace
	{
		template <typename TValue, typename TReadValue>
		auto GetTypedParameterValue(
			const DMaterialInstance& Material, FName Name, EMaterialParameterType Type,
			TValue& OutValue, TReadValue ReadValue) -> bool
		{
			const auto* Definition = Material.FindParameterDefinition(Name);
			if (!Definition || Definition->Type != Type) return false;
			FResolvedMaterialParameter Resolved;
			if (!Material.ResolveParameterValue(Definition->Id, Resolved)) return false;
			OutValue = ReadValue(Resolved.Value);
			return true;
		}

		template <typename TInstance, typename TOperation>
		auto ApplyTypedParameterOperation(
			TInstance& Instance, FName Name, EMaterialParameterType Type,
			TOperation Operation) -> bool
		{
			const auto* Definition = Instance.FindParameterDefinition(Name);
			return Definition && Definition->Type == Type
				&& (Instance.*Operation)(Definition->Id);
		}

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
	}

	DMaterialInstance::DMaterialInstance(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		if (!IsTemplateConstructionPurpose(ObjectInitializer.Purpose)) PublishMaterialRenderProxyState();
	}

	auto DMaterialInstance::ValidateParameterStorage(const FPropertyEditProposal* Proposal) const -> bool
	{
		std::unordered_set<FGuid> Ids;
		bool bValid = true;
		VisitParameterValueArrays([&](const auto& Stored) {
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
				if constexpr (std::is_same_v<TRecord, FMaterialTextureParameterValue>)
					if (!IsValidMaterialSampling(Record.Value.SamplerState, Record.Value.TextureFallback)) bValid = false;
			}
		});
		return bValid;
	}

	auto DMaterialInstance::Serialize(FArchive& Ar) -> void
	{
		if (!FMaterialInstanceVersion::Serialize(Ar)) return;
		Super::Serialize(Ar);
		if (Ar.HasError()) return;
		if (!ValidateParameterStorage())
			Ar.Fail(EArchiveFailureCode::InvalidData, "Invalid or duplicate material parameter value.");
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
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::AllRenderState, bParentChanged);
		return true;
	}

	auto DMaterialInstance::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!ValidateParameterStorage(&Proposal))
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
		auto DirtyFlags = EMaterialRenderDirtyFlags::DynamicParameters;
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("PropertyOverrides"))
		{
			InvalidateMaterialCompilation(true, true);
			DirtyFlags = DirtyFlags | EMaterialRenderDirtyFlags::AllRenderState;
		}
		else if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("Parent"))
		{
			InvalidateMaterialCompilation();
			DirtyFlags = DirtyFlags | EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::AllRenderState;
		}
		// Publish once, after compilation invalidation has observed the completed edit.
		MarkRenderDataDirty(DirtyFlags,
			!Event.MemberProperty || Event.MemberProperty->NamePrivate != FName("PropertyOverrides"));
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

	auto DMaterialInstance::GetLocalParameterValue(const FGuid& Id, FMaterialParameterValue& OutValue) const -> bool
	{
		bool bFound = false;
		VisitParameterValueArrays([&](const auto& Records) {
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
			FMaterialParameterValue LocalValue;
			if (Instance->GetLocalParameterValue(Id, LocalValue) && LocalValue.GetType() == Definition->Type)
			{
				OutParameter.Definition = Definition;
				OutParameter.Value = std::move(LocalValue);
				OutParameter.Source = const_cast<DMaterialInstance*>(Instance);
				OutParameter.bHasLocalOverride = Instance == this;
				return true;
			}
			Owner = Instance->GetParent();
		}
		return false;
	}

	auto DMaterialInstance::SetParameterValue(
		const FGuid& Id,
		const FMaterialParameterValue& Value
	) -> bool
	{
		const auto Type = Value.GetType();
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		if (!Definition || Definition->Type != Type
			|| !GetParameterReachability()->ParameterIds.contains(Id)) return false;
		if (Type == EMaterialParameterType::Texture && !IsValidMaterialSampling(Value.GetTexture().SamplerState, Value.GetTexture().TextureFallback)) return false;
		FMaterialParameterValue StoredValue = Value;
		if (FMaterialVectorParameterValue::SupportsType(Type))
		{
			FMaterialVectorParameterValue Vector;
			Vector.SetValue(Value);
			StoredValue = Vector.GetValue();
		}
		FMaterialParameterValue Existing;
		if (GetLocalParameterValue(Id, Existing))
		{
			if (Existing.GetType() != Type) return false;
			if (Existing == StoredValue) return true;
		}
		VisitParameterValueArrays([&](auto& Records) {
			using TRecord = typename std::decay_t<decltype(Records)>::value_type;
			if (!TRecord::SupportsType(Type)) return;
			auto It = std::ranges::find(Records, Id, &TRecord::ParameterId);
			if (It == Records.end())
			{
				Records.emplace_back();
				It = std::prev(Records.end());
				It->ParameterId = Id;
			}
			It->SetValue(StoredValue);
		});
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters, true);
		return true;
	}

	auto DMaterialInstance::ClearParameterValue(const FGuid& Id) -> bool
	{
		bool bRemoved = false;
		VisitParameterValueArrays([&](auto& Records) {
			bRemoved |= std::erase_if(Records, [&](const auto& Record) { return Record.ParameterId == Id; }) != 0;
		});
		if (!bRemoved) return false;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters, true);
		return true;
	}

	auto DMaterialInstance::HasLocalParameterValue(const FGuid& Id) const -> bool
	{
		FMaterialParameterValue Value;
		return GetLocalParameterValue(Id, Value);
	}

	auto DMaterialInstance::IsParameterValueOrphan(const FGuid& Id) const -> bool
	{
		FMaterialParameterValue LocalValue;
		if (!GetLocalParameterValue(Id, LocalValue)) return false;
		const auto* Definition = FindParameterDefinition(Id);
		return !Definition || Definition->Type != LocalValue.GetType()
			|| !GetParameterReachability()->ParameterIds.contains(Id);
	}

	auto DMaterialInstance::SetScalarParameterValue(FName Name, float Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		return SetParameterValue(
			Definition->Id, FMaterialParameterValue::MakeScalar(Value));
	}

	auto DMaterialInstance::SetVector2ParameterValue(FName Name, const FVector2& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector4) return false;
		return SetParameterValue(
			Definition->Id, FMaterialParameterValue::MakeVector4(FVector4(Value, 0, 0)));
	}

	auto DMaterialInstance::SetVectorParameterValue(FName Name, const FVector3& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector4) return false;
		return SetParameterValue(
			Definition->Id, FMaterialParameterValue::MakeVector4(FVector4(Value, 0)));
	}

	auto DMaterialInstance::SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		Resolved.Value.GetTexture().Texture = Value;
		return SetParameterValue(Definition->Id, Resolved.Value);
	}

	auto DMaterialInstance::ClearScalarParameterValue(FName Name) -> bool
	{
		return ApplyTypedParameterOperation(*this, Name, EMaterialParameterType::Scalar,
			&DMaterialInstance::ClearParameterValue);
	}

	auto DMaterialInstance::ClearVector2ParameterValue(FName Name) -> bool
	{
		return ApplyTypedParameterOperation(*this, Name, EMaterialParameterType::Vector4,
			&DMaterialInstance::ClearParameterValue);
	}

	auto DMaterialInstance::ClearVectorParameterValue(FName Name) -> bool
	{
		return ApplyTypedParameterOperation(*this, Name, EMaterialParameterType::Vector4,
			&DMaterialInstance::ClearParameterValue);
	}

	auto DMaterialInstance::ClearTextureParameterValue(FName Name) -> bool
	{
		return ApplyTypedParameterOperation(*this, Name, EMaterialParameterType::Texture,
			&DMaterialInstance::ClearParameterValue);
	}

	auto DMaterialInstance::HasLocalScalarParameterValue(FName Name) const -> bool
	{
		return ApplyTypedParameterOperation(*this, Name, EMaterialParameterType::Scalar,
			&DMaterialInstance::HasLocalParameterValue);
	}

	auto DMaterialInstance::HasLocalVector2ParameterValue(FName Name) const -> bool
	{
		return ApplyTypedParameterOperation(*this, Name, EMaterialParameterType::Vector4,
			&DMaterialInstance::HasLocalParameterValue);
	}

	auto DMaterialInstance::HasLocalVectorParameterValue(FName Name) const -> bool
	{
		return ApplyTypedParameterOperation(*this, Name, EMaterialParameterType::Vector4,
			&DMaterialInstance::HasLocalParameterValue);
	}

	auto DMaterialInstance::HasLocalTextureParameterValue(FName Name) const -> bool
	{
		return ApplyTypedParameterOperation(*this, Name, EMaterialParameterType::Texture,
			&DMaterialInstance::HasLocalParameterValue);
	}

	auto DMaterialInstance::GetScalarParameterValue(FName Name, float& OutValue) const -> bool
	{
		return GetTypedParameterValue(*this, Name, EMaterialParameterType::Scalar, OutValue,
			[](const FMaterialParameterValue& Value) { return Value.GetScalar(); });
	}

	auto DMaterialInstance::GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool
	{
		return GetTypedParameterValue(*this, Name, EMaterialParameterType::Vector4, OutValue,
			[](const FMaterialParameterValue& Value) { return FVector2(Value.GetVector4()); });
	}

	auto DMaterialInstance::GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool
	{
		return GetTypedParameterValue(*this, Name, EMaterialParameterType::Vector4, OutValue,
			[](const FMaterialParameterValue& Value) { return FVector3(Value.GetVector4()); });
	}

	auto DMaterialInstance::GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool
	{
		return GetTypedParameterValue(*this, Name, EMaterialParameterType::Texture, OutValue,
			[](const FMaterialParameterValue& Value) { return Value.GetTexture().Texture.Get(); });
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
		NotifyParameterChanges();
	}
}
