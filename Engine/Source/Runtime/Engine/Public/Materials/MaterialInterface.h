#pragma once

#include "DObject/CoreDObject.h"
#include "EngineAPI.h"
#include "Materials/MaterialTypes.h"

#include "MaterialInterface.gen.h"

namespace Durin
{
	class DMaterialInstance;
	class DStaticMeshComponent;

	DCLASS()
	class ENGINE_API DMaterialInterface : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DMaterialInterface(const FObjectInitializer& ObjectInitializer);

		virtual auto GetScalarParameterValue(std::string_view Name, float& OutValue) const -> bool;
		virtual auto GetVectorParameterValue(std::string_view Name, FVector3& OutValue) const -> bool;
		virtual auto GetParent() const -> DMaterialInterface*;

		auto GetRenderData() const -> FMaterialRenderData;
		auto GetRenderStateVersion() const -> uint64 { return RenderStateVersion; }
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
