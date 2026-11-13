#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "DObject/SoftObjectPtr.h"
#include "WaterSurfaceTypes.h"
#include "WaterSurfaceComponent.gen.h"

namespace Durin
{
	struct FStaticMeshRenderData;

	DCLASS(DisplayName = "Water Surface")
	class DWaterSurfaceComponent final : public DMeshComponent
	{
		GENERATED_BODY()
	public:
		WATER_API explicit DWaterSurfaceComponent(const FObjectInitializer& Initializer);
		WATER_API auto SetSettings(const FWaterSurfaceSettings& Value) -> std::expected<void, std::string>;
		auto GetSettings() const -> const FWaterSurfaceSettings& { return Settings; }
		auto GetDiagnostic() const -> const std::string& { return Diagnostic; }
		// Projects the point along component-local Z. Returns world position/normal.
		WATER_API auto QuerySurface(FVector3 WorldPoint, double MaterialSeconds) const -> std::optional<FWaterSurfaceSample>;
		WATER_API auto OnRegister() -> void override;
		WATER_API auto OnUnregister() -> void override;
		WATER_API auto CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy> override;
		auto GetNumMaterials() const -> uint32 override { return 1; }
		WATER_API auto GetMaterialIndex(FName SlotName) const -> std::optional<uint32> override;
		WATER_API auto GetDefaultMaterial(uint32 SlotIndex) const -> DMaterialInterface* override;
		WATER_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError> override;
		WATER_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		WATER_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError> override;
	protected:
		WATER_API auto OnUpdateTransform() -> void override;
		WATER_API auto BuildMaterialRenderProxyBindingUpdate(FMaterialRenderProxyBindingUpdate& Update) -> bool override;
	private:
		auto SynchronizeSurface() -> void;
		auto ResolveMaterial() -> FMaterialRenderProxyRef;
		auto HasSupportedTransform() const -> bool;
		DPROPERTY(Category = "Water", Edit)
		FWaterSurfaceSettings Settings;
		// Null uses the plugin's default Lit material. Overrides must implement its custom-data wave contract.
		DPROPERTY(Category = "Water", Edit)
		TObjectPtr<DMaterialInterface> Material;
		std::shared_ptr<FStaticMeshRenderData> Geometry;
		// Serialized reference participates in ordinary Cook reachability, without
		// loading material content during reflected default construction.
		DPROPERTY(AlwaysSerialize)
		TSoftObjectPtr<DMaterialInterface> DefaultMaterialAsset;
		DPROPERTY(Transient)
		TObjectPtr<DMaterialInterface> LoadedDefaultMaterial;
		std::string Diagnostic;
	};
}
