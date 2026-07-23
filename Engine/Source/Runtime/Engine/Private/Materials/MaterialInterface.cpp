#include "Materials/MaterialInterface.h"

#include "Components/StaticMeshComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Materials/MaterialInstance.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	DMaterialInterface::DMaterialInterface(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DMaterialInterface::GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>
	{
		return {};
	}

	auto DMaterialInterface::FindParameterDefinition(const FGuid& Id) const -> const FMaterialParameterDefinition*
	{
		const std::span Definitions = GetParameterDefinitions();
		const auto It = std::ranges::find(Definitions, Id, &FMaterialParameterDefinition::Id);
		return It == Definitions.end() ? nullptr : &*It;
	}

	auto DMaterialInterface::FindParameterDefinition(FName Name) const -> const FMaterialParameterDefinition*
	{
		const std::span Definitions = GetParameterDefinitions();
		const auto It = std::ranges::find(Definitions, Name, &FMaterialParameterDefinition::Name);
		return It == Definitions.end() ? nullptr : &*It;
	}

	auto DMaterialInterface::ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetScalarParameterValue(FName Name, float& OutValue) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetParent() const -> DMaterialInterface*
	{
		return nullptr;
	}

	auto DMaterialInterface::GetRenderData() const -> FMaterialRenderData
	{
		FMaterialRenderData Result;
		FResolvedMaterialParameter Parameter;
		if (ResolveParameterValue(MaterialParameters::BaseColorId, Parameter))
		{
			const FVector3& BaseColor = Parameter.Value.VectorValue;
			Result.BaseColor.r = static_cast<float>(std::clamp(BaseColor.x, 0.0, 1.0));
			Result.BaseColor.g = static_cast<float>(std::clamp(BaseColor.y, 0.0, 1.0));
			Result.BaseColor.b = static_cast<float>(std::clamp(BaseColor.z, 0.0, 1.0));
		}

		if (ResolveParameterValue(MaterialParameters::BaseColorTextureId, Parameter)
			&& Parameter.Value.TextureValue != nullptr)
		{
			Result.BaseColorTexture = Parameter.Value.TextureValue->GetRenderResource();
		}

		if (ResolveParameterValue(MaterialParameters::OpacityId, Parameter))
		{
			Result.BaseColor.a = std::clamp(Parameter.Value.ScalarValue, 0.0f, 1.0f);
		}

		if (ResolveParameterValue(MaterialParameters::SpecularStrengthId, Parameter))
		{
			Result.SpecularStrength = std::clamp(Parameter.Value.ScalarValue, 0.0f, 1.0f);
		}
		if (ResolveParameterValue(MaterialParameters::ShininessId, Parameter))
		{
			Result.Shininess = std::clamp(Parameter.Value.ScalarValue, 1.0f, 256.0f);
		}
		return Result;
	}

	auto DMaterialInterface::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("ParameterDefinitions") || Name == FName("ParameterOverrides"))
		{
			// Reflected editor transactions restore collection storage directly. Route every
			// phase through the same render invalidation normally supplied by setters.
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
		}
	}

	auto DMaterialInterface::BeginDestroy() -> void
	{
		BoundComponents.clear();
		DependentInstances.clear();
		Super::BeginDestroy();
	}

	auto DMaterialInterface::MarkRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags) -> void
	{
		if (DirtyFlags == EMaterialRenderDirtyFlags::None) return;
		++RenderStateVersion;

		std::erase_if(BoundComponents, [this, DirtyFlags](FObjectHandle Handle) {
			auto* Component = Cast<DStaticMeshComponent>(ResolveObjectHandle(Handle));
			if (!IsValid(Component)) return true;
			Component->HandleMaterialRenderDataChanged(this, DirtyFlags);
			return false;
		});
		std::erase_if(DependentInstances, [DirtyFlags](FObjectHandle Handle) {
			auto* Instance = Cast<DMaterialInstance>(ResolveObjectHandle(Handle));
			if (!IsValid(Instance)) return true;
			Instance->OnParentRenderDataDirty(DirtyFlags);
			return false;
		});
	}

	auto DMaterialInterface::AddBoundComponent(DStaticMeshComponent* Component) -> void
	{
		if (!Component) return;
		// Dependency edges must not keep assets or components alive; generations also make stale edges harmless after GC.
		const FObjectHandle Handle = MakeObjectHandle(Component);
		const auto Matches = [Handle](FObjectHandle Candidate) { return Candidate.Index == Handle.Index && Candidate.Generation == Handle.Generation; };
		if (std::ranges::find_if(BoundComponents, Matches) == BoundComponents.end()) BoundComponents.push_back(Handle);
	}

	auto DMaterialInterface::RemoveBoundComponent(DStaticMeshComponent* Component) -> void
	{
		if (!Component) return;
		const FObjectHandle Handle = MakeObjectHandle(Component);
		std::erase_if(BoundComponents, [Handle](FObjectHandle Candidate) { return Candidate.Index == Handle.Index && Candidate.Generation == Handle.Generation; });
	}

	auto DMaterialInterface::AddDependentInstance(DMaterialInstance* Instance) -> void
	{
		if (!Instance) return;
		const FObjectHandle Handle = MakeObjectHandle(Instance);
		const auto Matches = [Handle](FObjectHandle Candidate) { return Candidate.Index == Handle.Index && Candidate.Generation == Handle.Generation; };
		if (std::ranges::find_if(DependentInstances, Matches) == DependentInstances.end()) DependentInstances.push_back(Handle);
	}

	auto DMaterialInterface::RemoveDependentInstance(DMaterialInstance* Instance) -> void
	{
		if (!Instance) return;
		const FObjectHandle Handle = MakeObjectHandle(Instance);
		std::erase_if(DependentInstances, [Handle](FObjectHandle Candidate) { return Candidate.Index == Handle.Index && Candidate.Generation == Handle.Generation; });
	}
}
