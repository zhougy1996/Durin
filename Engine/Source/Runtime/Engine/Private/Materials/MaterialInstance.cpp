#include "MaterialParameterMutation.h"
#include "Components/PropertyEditValidation.h"
#include "Materials/ObjectCacheContext.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialObjectValidation.h"
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

	auto DMaterialInstance::CreateDynamic(DMaterialInterface* InParent, DObject* Outer, FName Name)
		-> DMaterialInstance*
	{
		if (!IsValid(InParent) || InParent->IsDynamicInstance()) return nullptr;
		FResolvedMaterialProperties Resolved;
		if (!ResolveMaterialProperties(*InParent, Resolved)) return nullptr;
		uint32 Depth = 1;
		for (auto* Node = InParent; Node; Node = Node->GetParent())
			if (++Depth > MaterialMaximumParentDepth) return nullptr;
		auto* Instance = NewObject<DMaterialInstance>(Outer, Name, EObjectFlags::Transient);
		Instance->bDynamicInstance = true;
		Instance->Parent = InParent;
		Instance->MarkRenderDataDirty(EMaterialRenderDirtyFlags::AllRenderState);
		return Instance;
	}

	auto DMaterialInstance::GetRenderableStaticProperties() const -> FMaterialStaticProperties
	{
		if (bDynamicInstance) return IsValid(Parent.Get())
			? Parent->GetRenderableStaticProperties() : FMaterialStaticProperties{};
		return Super::GetRenderableStaticProperties();
	}

	auto DMaterialInstance::ValidateParameterStorage(const FPropertyEditProposal* Proposal) const -> FMaterialOperationResult
	{
		std::unordered_set<FGuid> Ids;
		FMaterialOperationResult Result;
		VisitParameterValueArrays([&](const auto& Stored) {
			if (!Result) return;
			using TArray = std::decay_t<decltype(Stored)>;
			using TRecord = typename TArray::value_type;
			const TArray* Records = &Stored;
			if (Proposal && Proposal->MemberProperty && Proposal->MemberProperty->NamePrivate == TRecord::PropertyName())
			{
				if (Proposal->DraftRootProperty != Proposal->MemberProperty || !Proposal->DraftRootContainer)
				{
					Result.Error = EMaterialInstanceError::IncompleteParameterDraft;
					return;
				}
				Records = Proposal->DraftRootProperty->ContainerPtrToValuePtr<TArray>(
					Proposal->DraftRootContainer, Proposal->DraftRootArrayIndex);
			}
			for (uint32 Index = 0; Index < Records->size(); ++Index)
			{
				const auto& Record = (*Records)[Index];
				if (!Record.ParameterId.IsValid()) Result.Error = EMaterialInstanceError::InvalidParameterId;
				else if (!Ids.insert(Record.ParameterId).second) Result.Error = EMaterialInstanceError::DuplicateParameterId;
				else if constexpr (std::is_same_v<TRecord, FMaterialTextureParameterValue>)
					if (!IsValidMaterialSampling(Record.Value.SamplerState, Record.Value.TextureFallback))
						Result.Error = EMaterialInstanceError::InvalidSamplingPolicy;
				if (!Result)
				{
					Result.Error.ParameterId = Record.ParameterId;
					Result.Error.Index = Index;
					return;
				}
			}
		});
		return Result;
	}

	auto DMaterialInstance::Serialize(FArchive& Ar) -> void
	{
		if (bDynamicInstance)
		{
			// Package planning needs the normal field manifest before excluding
			// transient objects from the persisted graph. Other copies are forbidden.
			if (!(Ar.IsSaving() && (Ar.GetPurpose() == EArchivePurpose::Discovery
				|| Ar.GetPurpose() == EArchivePurpose::AuthoredPackage
				|| Ar.GetPurpose() == EArchivePurpose::CookedPackage)))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, "Dynamic material instances cannot be serialized or duplicated.");
				return;
			}
		}
		if (!FMaterialInstanceVersion::Serialize(Ar)) return;
		Super::Serialize(Ar);
		if (Ar.IsError()) return;
		const auto Validation = ValidateParameterStorage();
		if (!Validation)
		{
			auto Rejection = RejectMaterialObjectGraph(GetObjectPath(), Validation.Error);
			if (auto* ObjectArchive = dynamic_cast<FObjectArchive*>(&Ar))
				ObjectArchive->FailValidation(std::move(Rejection.error()));
			else Ar.Fail(EArchiveFailureCode::InvalidData, FormatMaterialError(Validation.Error));
		}
	}

	auto DMaterialInstance::SetParent(DMaterialInterface* InParent) -> bool
	{
		return SetParentAndPropertyOverrides(InParent, PropertyOverrides);
	}

	auto DMaterialInstance::SetParentAndPropertyOverrides(DMaterialInterface* InParent,
		const FMaterialPropertyOverrides& Overrides) -> bool
	{
		if (bDynamicInstance || (InParent && InParent->IsDynamicInstance())) return false;
		Durin::FMaterialOperationResult Error;
		if (WouldCreateParentCycle(this, InParent)
			|| !(Error = ValidateMaterialStaticProperties(Overrides.Values))) return false;
		const bool bParentChanged = Parent != InParent;
		if (!bParentChanged && PropertyOverrides == Overrides) return true;
		Parent = InParent;
		PropertyOverrides = Overrides;
		FObjectCacheContext Context;
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) AdoptParentRuntimeProgram();
		else InvalidateMaterialCompilation(true, !bParentChanged, &Context);
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::AllRenderState, bParentChanged, &Context);
		return true;
	}

	auto DMaterialInstance::PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError>
	{
		if (bDynamicInstance) return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::ModuleRejected);
		if (auto Result = Super::PreEditChangeProperty(Proposal); !Result) return Result;
		if (const auto Validation = ValidateParameterStorage(&Proposal); !Validation)
		{
			return RejectEnginePropertyEdit(*this, Proposal, Validation.Error);
		}
		if (Proposal.MemberProperty && Proposal.MemberProperty->NamePrivate == FName("PropertyOverrides")
			&& Proposal.DraftRootProperty == Proposal.MemberProperty && Proposal.DraftRootContainer)
		{
			const auto* Overrides = Proposal.DraftRootProperty->ContainerPtrToValuePtr<FMaterialPropertyOverrides>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			const auto Validation = ValidateMaterialStaticProperties(Overrides->Values);
			if (!Validation) return RejectEnginePropertyEdit(*this, Proposal, Validation.Error);
			return {};
		}
		if (!Proposal.MemberProperty || Proposal.MemberProperty->NamePrivate != FName("Parent")
			|| !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return {};
		if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
		{
			return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::InvalidMetadata);
		}
		DObject* Value = static_cast<const FObjectProperty*>(Proposal.DraftRootProperty)->GetObjectPropertyValue(
			Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		auto* CandidateParent = Value ? Cast<DMaterialInterface>(Value) : nullptr;
		if (CandidateParent && CandidateParent->IsDynamicInstance())
			return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::ModuleRejected);
		if (Value && !CandidateParent)
		{
			return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::IncompatibleObject);
		}
		if (WouldCreateParentCycle(this, CandidateParent))
		{
			return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::ParentCycle);
		}
		return {};
	}

	auto DMaterialInstance::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		if (bDynamicInstance) return;
		FObjectCacheContext Context;
		Super::PostEditChangePropertyWithContext(Event, Context);
		auto DirtyFlags = EMaterialRenderDirtyFlags::DynamicParameters;
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("PropertyOverrides"))
		{
			InvalidateMaterialCompilation(true, true, &Context);
			DirtyFlags = DirtyFlags | EMaterialRenderDirtyFlags::AllRenderState;
		}
		else if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("Parent"))
		{
			InvalidateMaterialCompilation(true, false, &Context);
			DirtyFlags = DirtyFlags | EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::AllRenderState;
		}
		// Publish once, after compilation invalidation has observed the completed edit.
		MarkRenderDataDirty(DirtyFlags,
			!Event.MemberProperty || Event.MemberProperty->NamePrivate != FName("PropertyOverrides"), &Context);
	}

	auto DMaterialInstance::GetParent() const -> DMaterialInterface*
	{
		return Parent.Get();
	}

	auto DMaterialInstance::GetStaticProperties() const -> const FMaterialStaticProperties&
	{
		FResolvedMaterialProperties Resolved;
		Durin::FMaterialOperationResult Error;
		ResolvedStaticProperties = (Error = ResolveMaterialProperties(*this, Resolved))
			? Resolved.Properties : FMaterialStaticProperties{};
		return ResolvedStaticProperties;
	}


	auto DMaterialInstance::GetAcceptedCompiledProgram() const
		-> std::shared_ptr<const FMaterialCompilerResult>
	{
		if (bDynamicInstance) return IsValid(Parent.Get()) ? Parent->GetAcceptedCompiledProgram() : nullptr;
		FResolvedMaterialProperties Resolved;
		const auto Error = ResolveMaterialProperties(*this, Resolved);
		if (!Error) return nullptr;
		return Super::GetAcceptedCompiledProgram();
	}

	auto DMaterialInstance::SetPropertyOverrides(const FMaterialPropertyOverrides& Overrides) -> bool
	{
		return SetParentAndPropertyOverrides(Parent.Get(), Overrides);
	}

	auto DMaterialInstance::GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>
	{
		FResolvedMaterialProperties Resolved;
		const auto Error = ResolveMaterialProperties(*this, Resolved);
		if (!Error) return {};
		auto* Root = Cast<DMaterialInterface>(ResolveObjectKey(Resolved.Root));
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
	) -> FMaterialOperationResult
	{
		const auto Type = Value.GetType();
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		const auto Fail = [&](auto Code, std::optional<EMaterialParameterType> Expected = {}) -> FMaterialOperationResult
		{
			FMaterialError Error(Code, Id);
			Error.ExpectedParameterType = Expected;
			Error.ActualParameterType = Type;
			return {std::move(Error)};
		};
		if (!Definition) return Fail(EMaterialParameterError::NotFound);
		if (Definition->Type != Type) return Fail(EMaterialParameterError::InvalidType, Definition->Type);
		if (bDynamicInstance)
		{
			const auto Program = GetAcceptedCompiledProgram();
			if (!Program) return Fail(EMaterialParameterError::Unreachable);
			const auto Active = std::ranges::find(Program->ActiveParameters, Id, &FMaterialCompilerParameterDeclaration::Id);
			if (Active == Program->ActiveParameters.end()) return Fail(EMaterialParameterError::Unreachable);
			if (Active->Type != Type) return Fail(EMaterialParameterError::InvalidType, Active->Type);
		}
		if (!GetParameterReachability()->ParameterIds.contains(Id)) return Fail(EMaterialParameterError::Unreachable);
		if (Type == EMaterialParameterType::Texture && !IsValidMaterialSampling(Value.GetTexture().SamplerState, Value.GetTexture().TextureFallback))
			return Fail(EMaterialInstanceError::InvalidSamplingPolicy);
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
			if (Existing.GetType() != Type) return Fail(EMaterialParameterError::OverrideType, Existing.GetType());
			if (Existing == StoredValue) return {};
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
		if (!bDynamicInstance) MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters, true);
		return {};
	}

	auto DMaterialInstance::ClearParameterValue(const FGuid& Id) -> bool
	{
		bool bRemoved = false;
		VisitParameterValueArrays([&](auto& Records) {
			bRemoved |= std::erase_if(Records, [&](const auto& Record) { return Record.ParameterId == Id; }) != 0;
		});
		if (!bRemoved) return false;
		if (!bDynamicInstance) MarkPackageDirty();
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

	auto DMaterialInstance::SetScalarParameterValue(FName Name, float Value) -> FMaterialOperationResult
	{
		const auto* Definition = FindParameterDefinition(Name);
		const auto Validated = ValidateNamedMaterialParameter(Definition, Name, EMaterialParameterType::Scalar);
		if (!Validated) return Validated;
		return WithMaterialParameterName(SetParameterValue(Definition->Id, FMaterialParameterValue::MakeScalar(Value)), Name);
	}

	auto DMaterialInstance::SetVector2ParameterValue(FName Name, const FVector2& Value) -> FMaterialOperationResult
	{
		const auto* Definition = FindParameterDefinition(Name);
		const auto Validated = ValidateNamedMaterialParameter(Definition, Name, EMaterialParameterType::Vector4);
		if (!Validated) return Validated;
		return WithMaterialParameterName(SetParameterValue(Definition->Id, FMaterialParameterValue::MakeVector4(FVector4(Value, 0, 0))), Name);
	}

	auto DMaterialInstance::SetVectorParameterValue(FName Name, const FVector3& Value) -> FMaterialOperationResult
	{
		const auto* Definition = FindParameterDefinition(Name);
		const auto Validated = ValidateNamedMaterialParameter(Definition, Name, EMaterialParameterType::Vector4);
		if (!Validated) return Validated;
		return WithMaterialParameterName(SetParameterValue(Definition->Id, FMaterialParameterValue::MakeVector4(FVector4(Value, 0))), Name);
	}

	auto DMaterialInstance::SetTextureParameterValue(FName Name, DTexture2D* Value) -> FMaterialOperationResult
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		const auto Validated = ValidateNamedMaterialParameter(Definition, Name, EMaterialParameterType::Texture);
		if (!Validated) return Validated;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved))
			return WithMaterialParameterName({FMaterialError(EMaterialParameterError::UnresolvedValue, Definition->Id)}, Name);
		Resolved.Value.GetTexture().Texture = Value;
		return WithMaterialParameterName(SetParameterValue(Definition->Id, Resolved.Value), Name);
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

	auto DMaterialInstance::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError>
	{
		if (bDynamicInstance || (Parent && Parent->IsDynamicInstance()))
			return RejectLoadedObjectGraph(GetObjectPath(), "Dynamic material instances cannot participate in persistent object graphs.");
		if (auto Result = Super::ValidateLoadedObjectGraph(Context); !Result) return Result;
		if (WouldCreateParentCycle(this, Parent.Get()))
			return RejectLoadedObjectGraph(GetObjectPath(), "Material instance parent chain contains a cycle.");
		if (auto Validation = ValidateMaterialStaticProperties(PropertyOverrides.Values); !Validation)
			return RejectMaterialObjectGraph(GetObjectPath(), Validation.Error);
		if (Context.bCooked && CookedProgramData.GetMetadata().LogicalSize == 0)
			return RejectMaterialObjectGraph(GetObjectPath(), EMaterialCookError::ProgramUnavailable);
		return {};
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
		const auto Error = ValidateMaterialStaticProperties(PropertyOverrides.Values);
		if (!Error)
		{
			DURIN_ERROR("PostLoad '{}': {}; disabling static property overrides.", GetObjectPath(), FormatMaterialError(Error.Error));
			PropertyOverrides = {};
		}
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			CompilationOwner.RenderLayer = {};
			if (CookedProgramData.GetMetadata().LogicalSize == 0)
			{
				MaterialCookDiagnostic = EMaterialCookError::ProgramUnavailable;
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), FormatMaterialError(MaterialCookDiagnostic));
			}
		}
		else RequestMaterialRecompile(*this);
		PublishMaterialRenderProxyState();
		NotifyParameterChanges();
	}
}
