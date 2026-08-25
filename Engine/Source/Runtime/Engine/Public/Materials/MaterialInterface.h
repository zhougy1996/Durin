#pragma once

#include "DObject/Object.h"
#include "EngineAPI.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialTypes.h"

#include "MaterialInterface.gen.h"

namespace Durin
{
	class DMaterialInstance;
	class DTexture2D;
	struct FMaterialProgram;

	enum class EMaterialLoadedQueryOperation : uint8
	{
		None,
		DirectChildren,
		Dependents,
	};

	struct FMaterialLoadedQueryDiagnostics
	{
		EMaterialLoadedQueryOperation LastOperation = EMaterialLoadedQueryOperation::None;
		uint64 QueryCount = 0;
		uint64 SnapshotCount = 0;
		uint64 ScannedObjectCount = 0;
		uint64 ScannedMaterialCount = 0;
		uint64 LastResultCount = 0;
	};

	ENGINE_API auto GetMaterialLoadedQueryDiagnostics()
		-> FMaterialLoadedQueryDiagnostics;
	ENGINE_API auto ResetMaterialLoadedQueryDiagnostics() -> void;

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
		ENGINE_API virtual auto GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool;
		ENGINE_API virtual auto GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool;
		ENGINE_API virtual auto GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool;
		ENGINE_API virtual auto GetParent() const -> DMaterialInterface*;
		ENGINE_API virtual auto GetStaticProperties() const -> const FMaterialStaticProperties&;
		ENGINE_API virtual auto GetRenderableStaticProperties() const
			-> FMaterialStaticProperties;
		ENGINE_API virtual auto GetMaterialProgram() const
			-> const FMaterialProgram*;
		ENGINE_API virtual auto GetAcceptedCompiledProgram() const
			-> std::shared_ptr<const FMaterialCompilerResult>;

		// Tests the canonical Parent chain without relying on reverse registration state.
		ENGINE_API auto IsDependent(const DMaterialInterface* TestDependency) const -> bool;
		ENGINE_API auto GetRenderData() const -> FMaterialRenderData;
		ENGINE_API auto GetMaterialRenderProxy() const
			-> FMaterialRenderProxyRef;
		auto GetRenderStateVersion() const -> uint64 { return RenderStateVersion; }
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	protected:
		ENGINE_API virtual auto BuildMaterialLocalRenderLayer() const
			-> FMaterialLocalRenderLayer;
		ENGINE_API auto PublishMaterialRenderProxyState() -> void;
		ENGINE_API auto MarkRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags) -> void;

	private:
		auto SubmitMaterialRenderProxyState() const -> void;

		uint64 RenderStateVersion = 1;
		mutable FMaterialRenderProxyRef MaterialRenderProxy;
		mutable uint64 MaterialProxyLocalVersion = 0;
		mutable uint64 LastSubmittedMaterialProxyLocalVersion = 0;
		bool bAcceptingMaterialProxyPublications = true;

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
