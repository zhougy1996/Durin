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

	DCLASS()
	class ENGINE_API DMaterialInterface : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DMaterialInterface(const FObjectInitializer& ObjectInitializer);

		virtual auto GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>;
		auto FindParameterDefinition(const FGuid& Id) const -> const FMaterialParameterDefinition*;
		auto FindParameterDefinition(FName Name) const -> const FMaterialParameterDefinition*;
		virtual auto ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool;

		virtual auto GetScalarParameterValue(FName Name, float& OutValue) const -> bool;
		virtual auto GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool;
		virtual auto GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool;
		virtual auto GetParent() const -> DMaterialInterface*;

		auto GetRenderData() const -> FMaterialRenderData;
		auto GetRenderStateVersion() const -> uint64 { return RenderStateVersion; }
		auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		auto BeginDestroy() -> void override;

	protected:
		auto MarkRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags) -> void;

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
}
