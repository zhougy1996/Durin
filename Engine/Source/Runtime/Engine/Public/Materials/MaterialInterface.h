#pragma once

#include "DObject/CoreDObject.h"
#include "EngineAPI.h"
#include "Materials/MaterialTypes.h"

#include "MaterialInterface.gen.h"

namespace Durin
{
	class DMaterialInstance;
	class DStaticMeshComponent;
	class DTexture2D;

	// Defines the shared parameter-resolution and render-update contract for materials.
	DCLASS()
	class DMaterialInterface : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DMaterialInterface(const FObjectInitializer& ObjectInitializer);

		ENGINE_API virtual auto GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>;
		ENGINE_API auto FindParameterDefinition(const FGuid& Id) const -> const FMaterialParameterDefinition*;
		ENGINE_API auto FindParameterDefinition(FName Name) const -> const FMaterialParameterDefinition*;
		ENGINE_API virtual auto ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool;

		ENGINE_API virtual auto GetScalarParameterValue(FName Name, float& OutValue) const -> bool;
		ENGINE_API virtual auto GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool;
		ENGINE_API virtual auto GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool;
		ENGINE_API virtual auto GetParent() const -> DMaterialInterface*;

		// Tests the canonical Parent chain without relying on reverse registration state.
		ENGINE_API auto IsDependent(const DMaterialInterface* TestDependency) const -> bool;
		ENGINE_API auto GetRenderData() const -> FMaterialRenderData;
		auto GetRenderStateVersion() const -> uint64 { return RenderStateVersion; }
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API auto BeginDestroy() -> void override;

	protected:
		ENGINE_API auto MarkRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags) -> void;

	private:
		auto AddBoundComponent(DStaticMeshComponent* Component) -> void;
		auto RemoveBoundComponent(DStaticMeshComponent* Component) -> void;
		auto AddDependentInstance(DMaterialInstance* Instance) -> void;
		auto RemoveDependentInstance(DMaterialInstance* Instance) -> void;

		uint64 RenderStateVersion = 1;
		std::vector<FObjectHandle> BoundComponents;
		std::vector<FObjectHandle> DependentInstances;

		friend class DMaterialInstance;
		friend class DStaticMeshComponent;
	};

	// Returns loaded material instances whose canonical Parent is exactly Parent.
	ENGINE_API auto GetLoadedDirectMaterialChildren(
		const DMaterialInterface* Parent
	) -> std::vector<FObjectHandle>;

	// Returns loaded materials whose canonical Parent chain contains Dependency, including itself.
	ENGINE_API auto GetLoadedMaterialDependents(
		const DMaterialInterface* Dependency
	) -> std::vector<FObjectHandle>;
}
