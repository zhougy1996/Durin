#include "Materials/MaterialInstance.h"
#include "Logging/LogMacros.h"

#include "Asset/Asset.h"
#include "DObject/DurinPropertyTypes.h"

namespace Durin
{
	namespace
	{
		auto FindMutableOverride(
			std::vector<FMaterialParameterOverride>& Overrides,
			const FGuid& Id
		) -> FMaterialParameterOverride*
		{
			const auto It = std::ranges::find(Overrides, Id, &FMaterialParameterOverride::ParameterId);
			return It == Overrides.end() ? nullptr : &*It;
		}

		auto FindOverride(
			const std::vector<FMaterialParameterOverride>& Overrides,
			const FGuid& Id
		) -> const FMaterialParameterOverride*
		{
			const auto It = std::ranges::find(Overrides, Id, &FMaterialParameterOverride::ParameterId);
			return It == Overrides.end() ? nullptr : &*It;
		}

		auto CanonicalizeParameterValue(
			EMaterialParameterType Type,
			const FMaterialParameterValue& Value
		) -> FMaterialParameterValue
		{
			switch (Type)
			{
			case EMaterialParameterType::Scalar:
				return FMaterialParameterValue::MakeScalar(Value.ScalarValue);
			case EMaterialParameterType::Vector2:
				return FMaterialParameterValue::MakeVector2(Value.Vector2Value);
			case EMaterialParameterType::Vector4:
				return FMaterialParameterValue::MakeVector4(Value.Vector4Value);
			case EMaterialParameterType::Vector:
				return FMaterialParameterValue::MakeVector(Value.VectorValue);
			case EMaterialParameterType::Texture:
				return FMaterialParameterValue::MakeTexture(Value.TextureValue.Get(), Value.SamplerState, Value.TextureFallback);
			}
			return {};
		}

		auto IsValidParameterType(EMaterialParameterType Type) -> bool
		{
			switch (Type)
			{
			case EMaterialParameterType::Scalar:
			case EMaterialParameterType::Vector2:
			case EMaterialParameterType::Vector4:
			case EMaterialParameterType::Vector:
			case EMaterialParameterType::Texture:
				return true;
			}
			return false;
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

		auto IsParameterAvailableForOverride(
			const DMaterialInterface& Material, const FGuid& Id) -> bool
		{
			// Authored edits may precede asynchronous compilation. This query governs
			// editing/orphan diagnostics only; render proxies use the accepted contract.
			if (!GetAssetRuntimeConfiguration().RequiresCookedPayload())
			{
				const FMaterialProgram* AuthoredProgram = Material.GetMaterialProgram();
				if (!AuthoredProgram) return false;
				const auto Dependencies = InspectMaterialParameterDependencies(
					*AuthoredProgram, Material.GetParameterDefinitions());
				return std::ranges::find(Dependencies, Id,
					&FMaterialParameterDependency::ParameterId) != Dependencies.end();
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

	auto DMaterialInstance::SetParent(DMaterialInterface* InParent) -> bool
	{
		if (WouldCreateParentCycle(this, InParent)) return false;
		if (Parent == InParent) return true;
		Parent = InParent;
		InvalidateMaterialCompilation();
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParentChain | EMaterialRenderDirtyFlags::AllRenderState);
		return true;
	}

	auto DMaterialInstance::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
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
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("PropertyOverrides"))
		{
			if (CanonicalizeMaterialShaderProperties(GetStaticProperties()) != CompilationOwner.LastRequestedShaderProperties)
				InvalidateMaterialCompilation();
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
		FResolvedMaterialProperties Resolved;
		std::string Error;
		if (!ResolveMaterialProperties(*this, Resolved, Error)) return {};
		FMaterialStaticProperties Result = Parent != nullptr
			? Parent->GetRenderableStaticProperties()
			: Super::GetRenderableStaticProperties();
		PropertyOverrides.ApplyTo(Result);
		const auto Shader = CanonicalizeMaterialShaderProperties(Result);
		Result.OpacityMaskThreshold = Shader.OpacityMaskThreshold;
		return Result;
	}

	auto DMaterialInstance::GetMaterialProgram() const
		-> const FMaterialProgram*
	{
		FResolvedMaterialProperties Resolved;
		std::string Error;
		if (!ResolveMaterialProperties(*this, Resolved, Error)) return nullptr;
		return Parent ? Parent->GetMaterialProgram() : nullptr;
	}

	auto DMaterialInstance::GetAcceptedCompiledProgram() const
		-> std::shared_ptr<const FMaterialCompilerResult>
	{
		FResolvedMaterialProperties Resolved;
		std::string Error;
		if (!ResolveMaterialProperties(*this, Resolved, Error)) return nullptr;
		if (!Parent) return nullptr;
		const auto Program = Parent->GetAcceptedCompiledProgram();
		if (!Program) return nullptr;
		const FMaterialStaticProperties Compiled =
			Parent->GetRenderableStaticProperties();
		// Pipeline-only state may reuse the parent's code. Shader-affecting state
		// must never publish that code as a compatible instance permutation.
		if (CanonicalizeMaterialShaderProperties(GetRenderableStaticProperties())
			!= CanonicalizeMaterialShaderProperties(Compiled))
			return nullptr;
		return Program;
	}

	auto DMaterialInstance::SetStaticPropertiesOverride(
		const FMaterialStaticProperties& InProperties) -> bool
	{
		return SetPropertyOverrides({true, true, true, true, true, InProperties});
	}

	auto DMaterialInstance::SetPropertyOverrides(const FMaterialPropertyOverrides& Overrides) -> bool
	{
		std::string Error;
		if (!ValidateMaterialStaticProperties(Overrides.Values, Error)) return false;
		if (PropertyOverrides == Overrides) return true;
		PropertyOverrides = Overrides;
		InvalidateMaterialCompilation(true, true);
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::AllRenderState);
		return true;
	}

	auto DMaterialInstance::ClearStaticPropertiesOverride() -> bool
	{
		if (!PropertyOverrides.HasAnyOverride()) return false;
		FMaterialPropertyOverrides Cleared;
		Cleared.Values = PropertyOverrides.Values;
		return SetPropertyOverrides(Cleared);
	}

	auto DMaterialInstance::GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>
	{
		FResolvedMaterialProperties Resolved;
		std::string Error;
		if (!ResolveMaterialProperties(*this, Resolved, Error)) return {};
		return Parent != nullptr ? Parent->GetParameterDefinitions() : std::span<const FMaterialParameterDefinition>{};
	}

	auto DMaterialInstance::GetParameterOverrides() const -> std::span<const FMaterialParameterOverride>
	{
		return ParameterOverrides;
	}

	auto DMaterialInstance::ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		if (!Definition) return false;
		if (const FMaterialParameterOverride* Override = FindOverride(ParameterOverrides, Id);
			Override && Override->Type == Definition->Type)
		{
			OutParameter.Definition = Definition;
			OutParameter.Value = Override->Value;
			OutParameter.Source = const_cast<DMaterialInstance*>(this);
			OutParameter.bHasLocalOverride = true;
			return true;
		}
		if (Parent == nullptr || !Parent->ResolveParameterValue(Id, OutParameter)) return false;
		OutParameter.bHasLocalOverride = false;
		return true;
	}

	auto DMaterialInstance::SetParameterOverride(
		const FGuid& Id,
		EMaterialParameterType Type,
		const FMaterialParameterValue& Value
	) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		if (!Definition || Definition->Type != Type
			|| !IsParameterAvailableForOverride(*this, Id)) return false;
		if (Type == EMaterialParameterType::Texture && !IsValidMaterialSampling(Value.SamplerState, Value.TextureFallback)) return false;
		const FMaterialParameterValue CanonicalValue = CanonicalizeParameterValue(Type, Value);
		if (FMaterialParameterOverride* Override = FindMutableOverride(ParameterOverrides, Id))
		{
			if (Override->Type == Type && Override->Value == CanonicalValue) return true;
			Override->Type = Type;
			Override->Value = CanonicalValue;
		}
		else
		{
			ParameterOverrides.push_back({
				.ParameterId = Id,
				.Type = Type,
				.Value = CanonicalValue});
		}
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterialInstance::ClearParameterOverride(const FGuid& Id) -> bool
	{
		const size_t PreviousSize = ParameterOverrides.size();
		std::erase_if(ParameterOverrides, [&Id](const FMaterialParameterOverride& Override) {
			return Override.ParameterId == Id;
		});
		if (ParameterOverrides.size() == PreviousSize) return false;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterialInstance::HasLocalParameterOverride(const FGuid& Id) const -> bool
	{
		return FindOverride(ParameterOverrides, Id) != nullptr;
	}

	auto DMaterialInstance::IsParameterOverrideOrphan(const FGuid& Id) const -> bool
	{
		const auto* Override = FindOverride(ParameterOverrides, Id);
		if (!Override) return false;
		const auto* Definition = FindParameterDefinition(Id);
		return !Definition || Definition->Type != Override->Type
			|| !IsParameterAvailableForOverride(*this, Id);
	}

	auto DMaterialInstance::SetScalarParameterValue(FName Name, float Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		return SetParameterOverride(
			Definition->Id, EMaterialParameterType::Scalar, FMaterialParameterValue::MakeScalar(Value));
	}

	auto DMaterialInstance::SetVector2ParameterValue(FName Name, const FVector2& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector2) return false;
		return SetParameterOverride(
			Definition->Id, EMaterialParameterType::Vector2, FMaterialParameterValue::MakeVector2(Value));
	}

	auto DMaterialInstance::SetVectorParameterValue(FName Name, const FVector3& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		return SetParameterOverride(
			Definition->Id, EMaterialParameterType::Vector, FMaterialParameterValue::MakeVector(Value));
	}

	auto DMaterialInstance::SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		Resolved.Value.TextureValue = Value;
		return SetParameterOverride(Definition->Id, EMaterialParameterType::Texture, Resolved.Value);
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
		OutValue = Resolved.Value.ScalarValue;
		return true;
	}

	auto DMaterialInstance::GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector2) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		OutValue = Resolved.Value.Vector2Value;
		return true;
	}

	auto DMaterialInstance::GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		OutValue = Resolved.Value.VectorValue;
		return true;
	}

	auto DMaterialInstance::GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		FResolvedMaterialParameter Resolved;
		if (!ResolveParameterValue(Definition->Id, Resolved)) return false;
		OutValue = Resolved.Value.TextureValue.Get();
		return true;
	}

	auto DMaterialInstance::BuildMaterialLocalRenderLayer() const
		-> FMaterialLocalRenderLayer
	{
		FMaterialLocalRenderLayer Result;
		if (PropertyOverrides.HasAnyOverride())
			Result.PropertyOverrides = PropertyOverrides;
		Result.Parameters.reserve(ParameterOverrides.size());
		for (const FMaterialParameterOverride& Override
			: ParameterOverrides)
		{
			// Preserve dormant overrides in the proxy; resolve against the parent's
			// current compiled contract when its generation changes.
			const FMaterialParameterDefinition* Definition =
				FindParameterDefinition(Override.ParameterId);
			if (!Definition || Definition->Type != Override.Type)
			{
				const auto Accepted = GetAcceptedCompiledProgram();
				if (!Accepted) continue;
				const auto Active = std::ranges::find(Accepted->ActiveParameters,
					Override.ParameterId, &FMaterialCompilerParameterDeclaration::Id);
				if (Active == Accepted->ActiveParameters.end() || Active->Type != Override.Type) continue;
			}
			Result.Parameters.push_back(
				BuildMaterialLocalRenderParameter(
					Override.ParameterId,
					Override.Type,
					Override.Value));
		}
		return Result;
	}

	auto DMaterialInstance::PostLoad() -> void
	{
		Super::PostLoad();
		if (WasDeprecatedPropertyLoaded(FName("bOverrideStaticProperties_DEPRECATED"))
			|| WasDeprecatedPropertyLoaded(FName("StaticPropertiesOverride_DEPRECATED")))
		{
			const bool Enabled = bOverrideStaticProperties_DEPRECATED;
			PropertyOverrides = {Enabled, Enabled, Enabled, Enabled, Enabled,
				StaticPropertiesOverride_DEPRECATED};
			ClearLoadedDeprecatedProperties();
		}
		// Break corrupt parent chains before any parameter lookup can recurse.
		if (WouldCreateParentCycle(this, Parent.Get()))
		{
			DURIN_ERROR("PostLoad '{}': material instance parent cycle; clearing parent.", GetObjectPath());
			Parent = nullptr;
		}
		std::unordered_set<FGuid> OverrideIds;
		std::erase_if(ParameterOverrides, [&](const FMaterialParameterOverride& Override) {
			if (!Override.ParameterId.IsValid() || !IsValidParameterType(Override.Type)
				|| (Override.Type == EMaterialParameterType::Texture
					&& !IsValidMaterialSampling(Override.Value.SamplerState, Override.Value.TextureFallback))
				|| !OverrideIds.insert(Override.ParameterId).second)
			{
				DURIN_ERROR("PostLoad '{}': discarding invalid or duplicate material parameter override {}.",
					GetObjectPath(), Override.ParameterId.ToString());
				return true;
			}
			return false;
		});
		std::string Error;
		if (!ValidateMaterialStaticProperties(PropertyOverrides.Values, Error))
		{
			DURIN_ERROR("PostLoad '{}': {}; disabling static property overrides.", GetObjectPath(), Error);
			PropertyOverrides = {};
		}
		if (!GetAssetRuntimeConfiguration().RequiresCookedPayload())
			RequestMaterialRecompile(*this);
		PublishMaterialRenderProxyState();
	}
}
