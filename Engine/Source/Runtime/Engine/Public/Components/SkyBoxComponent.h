#pragma once

#include "CoreMinimal.h"

#include "Components/StaticMeshComponent.h"

#ifdef _DHT_PARSER
namespace Durin
{
	struct FLinearColor
	{
		float R, G, B, A;
	};
}
#else
#include "Math/Color.h"
#endif

#include "SkyBoxComponent.gen.h"

namespace Durin
{
	class DMaterialInstance;
	class DTextureCube;
	class DMaterialInterface;

	// Binds cubemap appearance to the default material of a static mesh.
	DCLASS()
	class DSkyBoxComponent : public DStaticMeshComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DSkyBoxComponent(const FObjectInitializer& ObjectInitializer);

		ENGINE_API auto SetStaticMesh(DStaticMesh* InStaticMesh) -> void override;
		ENGINE_API auto GetDefaultMaterial(uint32 SlotIndex) const -> DMaterialInterface* override;
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError> override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

		ENGINE_API auto GetTextureCube() const -> DTextureCube*;
		ENGINE_API auto SetMaterialOverride(DMaterialInterface* Material) -> void;
		auto GetMaterialOverride() const -> DMaterialInterface* { return MaterialOverride.Get(); }
		auto GetTint() const -> const FLinearColor& { return Tint; }
		auto GetIntensity() const -> float { return Intensity; }
		ENGINE_API auto SetTextureCube(DTextureCube* InTextureCube) -> void;
		ENGINE_API auto SetTint(const FLinearColor& InTint) -> void;
		ENGINE_API auto SetIntensity(float InIntensity) -> void;

	private:
		auto RefreshAppearance(bool bRefreshMeshBinding = false) -> void;
		auto UpdateCubeMaterial() -> void;
		DPROPERTY(Transient, NonTransactional)
		TObjectPtr<DMaterialInstance> CubeMaterial;

		DPROPERTY(Category = "Source", Edit)
		TObjectPtr<DTextureCube> TextureCube;

		DPROPERTY(Category = "Appearance", Edit)
		TObjectPtr<DMaterialInterface> MaterialOverride;

		DPROPERTY(Category = "Appearance", Edit, MetaData="HideAlpha")
		FLinearColor Tint{1.0f, 1.0f, 1.0f, 1.0f};

		DPROPERTY(Category = "Appearance", Edit)
		float Intensity = 1.0f;
	};
}
