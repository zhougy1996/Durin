#pragma once

#include "DObject/Object.h"
#include "Asset/BulkData.h"
#include "Materials/MaterialCookedProgram.h"
#include "EngineAPI.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialTypes.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialFunctionInterface.h"
#include <chrono>
#include "Delegates/Delegate.h"

#include "MaterialInterface.gen.h"

namespace Durin
{
	class FObjectCacheContext;
	template<typename T> class TObjectCacheIterator;
	// Owning-thread invalidation of parameter definitions, values, ancestry or reachability.
	DECLARE_MULTICAST_DELEGATE(FMaterialParameterChangedEvent)

	class DMaterialInstance;
	class DTexture2D;
	inline constexpr uint32 MaterialMaximumParentDepth = 64;

	// Immutable editing query; an unsuccessful analysis is distinct from an empty graph.
	// Contains identities only and does not retain material or function objects.
	struct FMaterialParameterReachability
	{
		FMaterialProgramValidationResult Validation;
		std::unordered_set<FGuid> ParameterIds;
	};

	// GameThread resolution reports source owners without retaining their lifetimes.
	struct FResolvedMaterialProperties
	{
		FObjectKey Root;
		FMaterialStaticProperties Properties;
		FMaterialStaticProperties ShaderProperties;
		// Blend, shading, cutoff, culling, depth, in that order.
		std::array<FObjectKey, 5> Sources{};
	};

	[[nodiscard]] ENGINE_API auto ResolveMaterialProperties(const DMaterialInterface& Material,
		FResolvedMaterialProperties& OutProperties) -> FMaterialOperationResult;

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
		uint64 ParentTableBuildCount = 0;
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
		// Request-local dependency versions; excluded from shared shader artifacts.
		std::vector<FMaterialFunctionOwnerStamp> RequestedFunctionOwners;
		std::vector<MIR::FSource> RequestedExpressionSources;
		std::vector<MIR::FSource> AcceptedExpressionSources;
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
		friend ENGINE_API auto NotifyMaterialFunctionChanged(const DMaterialFunctionInterface& Function) -> void;
	public:
		ENGINE_API explicit DMaterialInterface(const FObjectInitializer& ObjectInitializer);
		virtual auto IsDynamicInstance() const -> bool { return false; }
		auto GetImportProvenance() const -> const FMaterialImportProvenance& { return ImportProvenance; }
		// Changes only persisted editor metadata, without invalidating compiled material state.
		ENGINE_API auto SetImportProvenance(FMaterialImportProvenance InProvenance) -> bool;

		ENGINE_API virtual auto GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>;
		ENGINE_API auto FindParameterDefinition(const FGuid& Id) const -> const FMaterialParameterDefinition*;
		ENGINE_API auto FindParameterDefinition(FName Name) const -> const FMaterialParameterDefinition*;
		// GameThread only. Share one result across a batch. Authored queries follow the
		// current graph, including transitive function revisions, independently of compilation.
		// Cooked queries follow the accepted program. Failed analyses are not cached.
		ENGINE_API auto GetParameterReachability() const -> std::shared_ptr<const FMaterialParameterReachability>;
		ENGINE_API virtual auto ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool;

		ENGINE_API virtual auto GetScalarParameterValue(FName Name, float& OutValue) const -> bool;
		ENGINE_API virtual auto GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool;
		ENGINE_API virtual auto GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool;
		ENGINE_API virtual auto GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool;
		ENGINE_API virtual auto GetParent() const -> DMaterialInterface*;
		ENGINE_API virtual auto GetStaticProperties() const -> const FMaterialStaticProperties&;
		ENGINE_API virtual auto GetRenderableStaticProperties() const
			-> FMaterialStaticProperties;
		ENGINE_API virtual auto GetAcceptedCompiledProgram() const
			-> std::shared_ptr<const FMaterialCompilerResult>;
		auto GetAcceptedExpressionSources() const -> std::span<const MIR::FSource>
		{
			return CompilationOwner.AcceptedExpressionSources;
		}

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
		auto GetMaterialCookDiagnostic() const -> const FMaterialError&
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
		// Includes inherited changes; callbacks should invalidate snapshots, not edit owners.
		auto GetParameterChanges() -> FMaterialParameterChangedEvent& { return ParameterChanges; }
		auto GetRenderStateVersion() const -> uint64 { return RenderStateVersion; }
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	protected:
		ENGINE_API auto PostEditChangePropertyWithContext(const FPropertyChangedEvent& Event, FObjectCacheContext& Context) -> void;
		ENGINE_API auto LoadCookedProgram() -> FMaterialOperationResult;
		// A transient runtime owner can select only an already compiled compatible variant.
		ENGINE_API auto AdoptParentRuntimeProgram() -> bool;
		FBulkData CookedProgramData;
		FMaterialError MaterialCookDiagnostic;
		ENGINE_API auto RequestProgramCompile(
			const FMaterialStaticProperties& CandidateProperties,
			bool bForceRecompile = false, FObjectCacheContext* Context = nullptr) -> bool;
		// Invalidates authored dependencies before requesting detached replacements.
		ENGINE_API auto InvalidateMaterialCompilation(bool bIncludeSelf = true,
			bool bOnlyIfShaderChanged = false, FObjectCacheContext* Context = nullptr) -> void;
		FMaterialCompilationOwnerState CompilationOwner;

		ENGINE_API virtual auto BuildMaterialLocalRenderLayer() const
			-> FMaterialLocalRenderLayer;
		ENGINE_API auto PublishMaterialRenderProxyState() -> void;
		ENGINE_API auto NotifyParameterChanges() -> void;
		// Optional parameter notifications reuse the publication snapshot and run
		// only after every affected owner's render state has been updated.
		ENGINE_API auto MarkRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags,
			bool bNotifyParameterChanges = false, FObjectCacheContext* Context = nullptr) -> void;

	private:
		auto BroadcastParameterChanges(const TObjectCacheIterator<DMaterialInterface>& Dependents) -> void;
		friend auto ::Durin::ContributeEngineCookAsset(
			DObject&, std::string_view, FCookContext&) -> FCookContributionResult;
		ENGINE_API auto ContributeToCook(
			FCookContext& Context,
			std::string_view VirtualPackagePath) -> FCookContributionResult;
	private:
		friend struct Private::FMaterialCompilationLifecycle;
		DPROPERTY(EditorOnly)
		FMaterialImportProvenance ImportProvenance;
		// Retires the failed owner's complete renderable generation and publishes ErrorMaterial.
		auto RetireFailedMaterialGeneration(FObjectCacheContext* Context = nullptr) -> void;
		auto PublishMaterialRenderProxyState(FMaterialLocalRenderLayer LocalLayer) -> void;
		auto SubmitMaterialRenderProxyState(FMaterialLocalRenderLayer LocalLayer) const -> void;

		FMaterialParameterChangedEvent ParameterChanges;
		uint64 RenderStateVersion = 1;
		mutable std::shared_ptr<const FMaterialParameterReachability> ParameterReachability;
		mutable uint64 ParameterReachabilityProgramRevision = 0;
		mutable std::vector<FMaterialFunctionOwnerStamp> ParameterReachabilityFunctionOwners;
		mutable std::weak_ptr<const FMaterialCompilerResult> ParameterReachabilityCookedProgram;
		mutable FMaterialRenderProxyRef MaterialRenderProxy;
		mutable uint64 MaterialProxyLocalVersion = 0;
		mutable uint64 LastSubmittedMaterialProxyLocalVersion = 0;
		bool bAcceptingMaterialProxyPublications = true;

	};

	// Returns loaded material instances whose canonical Parent is exactly Parent.
	ENGINE_API auto GetLoadedDirectMaterialChildren(
		const DMaterialInterface* Parent
	) -> std::vector<FObjectKey>;

	// Returns loaded materials whose canonical Parent chain contains Dependency, including itself.
	ENGINE_API auto GetLoadedMaterialDependents(
		const DMaterialInterface* Dependency
	) -> std::vector<FObjectKey>;
}
