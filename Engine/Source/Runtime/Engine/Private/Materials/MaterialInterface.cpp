#include "Materials/MaterialInterface.h"

#include "Components/StaticMeshComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Materials/MaterialInstance.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	DMaterialInterface::DMaterialInterface(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DMaterialInterface::GetScalarParameterValue(std::string_view Name, float& OutValue) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetVectorParameterValue(std::string_view Name, FVector3& OutValue) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetTextureParameterValue(std::string_view Name, DTexture2D*& OutValue) const -> bool
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
		FVector3 BaseColor;
		if (GetVectorParameterValue(MaterialParameterBaseColor, BaseColor))
		{
			Result.BaseColor.r = static_cast<float>(std::clamp(BaseColor.x, 0.0, 1.0));
			Result.BaseColor.g = static_cast<float>(std::clamp(BaseColor.y, 0.0, 1.0));
			Result.BaseColor.b = static_cast<float>(std::clamp(BaseColor.z, 0.0, 1.0));
		}

		DTexture2D* BaseColorTexture = nullptr;
		if (GetTextureParameterValue(MaterialParameterBaseColorTexture, BaseColorTexture) && BaseColorTexture != nullptr)
		{
			Result.BaseColorTexture = BaseColorTexture->GetRenderResource();
		}

		float Opacity = Result.BaseColor.a;
		if (GetScalarParameterValue(MaterialParameterOpacity, Opacity))
		{
			Result.BaseColor.a = std::clamp(Opacity, 0.0f, 1.0f);
		}

		float SpecularStrength = Result.SpecularStrength;
		if (GetScalarParameterValue(MaterialParameterSpecularStrength, SpecularStrength))
		{
			Result.SpecularStrength = std::clamp(SpecularStrength, 0.0f, 1.0f);
		}
		float Shininess = Result.Shininess;
		if (GetScalarParameterValue(MaterialParameterShininess, Shininess))
		{
			Result.Shininess = std::clamp(Shininess, 1.0f, 256.0f);
		}
		return Result;
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
