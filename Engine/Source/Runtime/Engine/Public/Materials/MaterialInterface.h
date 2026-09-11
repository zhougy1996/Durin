#pragma once

#include "DObject/Object.h"
#include "Asset/BulkData.h"
#include "Materials/MaterialCookedProgram.h"
#include "EngineAPI.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialTypes.h"
#include "Materials/MaterialCompileLifecycle.h"
#include <chrono>

#include "MaterialInterface.gen.h"

namespace Durin
{
	class DMaterialInstance;
	class DTexture2D;
	struct FMaterialProgram;
	inline constexpr uint32 MaterialMaximumParentDepth = 64;

	// GameThread resolution reports source owners without retaining their lifetimes.
	struct FResolvedMaterialProperties
	{
		FObjectHandle Root;
		FMaterialStaticProperties Properties;
		FMaterialStaticProperties ShaderProperties;
		// Blend, shading, cutoff, culling, depth, in that order.
		std::array<FObjectHandle, 5> Sources{};
	};

	ENGINE_API auto ResolveMaterialProperties(const DMaterialInterface& Material,
		FResolvedMaterialProperties& OutProperties, std::string& OutError) -> bool;

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

	// Per-asset compilation state never retains another material owner.
	struct FMaterialCompilationOwnerState
	{
		// The render-safe contract remains available while a replacement compiles.
		FMaterialLocalRenderLayer RenderLayer;
		// Last scheduled or submitted schema, used to classify reflected dynamic edits.
		FMaterialStaticProperties LastObservedShaderProperties;
		std::vector<FMaterialCompilerParameterDeclaration> LastObservedParameters;
		FMaterialCompileStatus MaterialCompileStatus;
		std::vector<FMaterialCompileDiagnostic> MaterialCompileDiagnostics;
		bool bDeferredForceRecompile = false;
		std::chrono::steady_clock::time_point EditCompileDeadline;
	};

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

		auto GetMaterialCompileDiagnostics() const
			-> std::span<const FMaterialCompileDiagnostic>
		{
			return CompilationOwner.MaterialCompileDiagnostics;
		}
		auto GetMaterialCompileStatus() const -> const FMaterialCompileStatus&
		{
			return CompilationOwner.MaterialCompileStatus;
		}

		auto GetCookedProgramData() const -> const FBulkData&
		{
			return CookedProgramData;
		}
		auto GetMaterialCookDiagnostic() const -> std::string_view
		{
			return MaterialCookDiagnostic;
		}
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;

		// Tests the canonical Parent chain without relying on reverse registration state.
		ENGINE_API auto IsDependent(const DMaterialInterface* TestDependency) const -> bool;
		ENGINE_API auto GetRenderData() const -> FMaterialRenderData;
		ENGINE_API auto GetMaterialRenderProxy() const
			-> FMaterialRenderProxyRef;
		// Refreshes native texture bindings after a referenced texture package was
		// atomically replaced. Authored material state and dirty flags are unchanged.
		ENGINE_API auto RefreshReloadedAssetBindings() -> void;
		auto GetRenderStateVersion() const -> uint64 { return RenderStateVersion; }
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	protected:
		ENGINE_API auto LoadCookedProgram(std::string& OutError) -> bool;
		// A transient runtime owner can select only an already compiled compatible variant.
		ENGINE_API auto AdoptParentRuntimeProgram() -> bool;
		FBulkData CookedProgramData;
		std::string MaterialCookDiagnostic;
		ENGINE_API auto RequestProgramCompile(
			const FMaterialProgram& CandidateProgram,
			const FMaterialStaticProperties& CandidateProperties,
			bool bForceRecompile = false) -> bool;
		// Invalidates authored dependencies before requesting detached replacements.
		ENGINE_API auto InvalidateMaterialCompilation(bool bIncludeSelf = true,
			bool bOnlyIfShaderChanged = false) -> void;
		FMaterialCompilationOwnerState CompilationOwner;

		ENGINE_API virtual auto BuildMaterialLocalRenderLayer() const
			-> FMaterialLocalRenderLayer;
		ENGINE_API auto PublishMaterialRenderProxyState() -> void;
		ENGINE_API auto MarkRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags) -> void;

	private:
		friend auto ::Durin::ContributeEngineCookAsset(
			DObject&, std::string_view, FCookContext&, std::string&) -> bool;
		ENGINE_API auto ContributeToCook(
			FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;
	private:
		friend struct Private::FMaterialCompilationLifecycle;
		// Retires the failed owner's complete renderable generation and publishes ErrorMaterial.
		auto RetireFailedMaterialGeneration() -> void;
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
