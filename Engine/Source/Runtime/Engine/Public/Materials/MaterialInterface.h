#pragma once

#include "CoreMinimal.h"
#include "Shader/ShaderDiagnostics.h"

#include "DObject/Object.h"
#include "Asset/BulkData.h"
#include "EngineAPI.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialTypes.h"
#include "Materials/MaterialCookConfiguration.h"
#include "Asset/AssetBuildTaskContext.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Delegates/Delegate.h"
#include "PSOPrecacheMaterial.h"

#include "MaterialInterface.gen.h"

namespace Durin
{
	struct FMaterialProgramPreparationState;
	class IRendererModule;
	class FObjectCacheContext;
	struct FAssetCompileProcessParams;
	struct FAssetCompileProcessResult;
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
		FResolvedMaterialProperties& OutProperties) -> std::expected<void, std::string>;

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

	// Current loaded authored family only. Dynamic instances are deliberately
	// excluded, and the query never loads a package.
	struct FLoadedMaterialFamilyStatistics
	{
		uint64 LoadedOwnerCount = 0;
		uint64 AcceptedOwnerCount = 0;
		uint64 DistinctProgramCount = 0;
		uint64 DistinctStaticConfigurationCount = 0;
		uint64 PendingOwnerCount = 0;
		uint64 FailedOwnerCount = 0;
	};

	ENGINE_API auto GetMaterialLoadedQueryDiagnostics()
		-> FMaterialLoadedQueryDiagnostics;
	ENGINE_API auto ResetMaterialLoadedQueryDiagnostics() -> void;
	ENGINE_API auto GetMaterialProgramStatistics(const DMaterialInterface& Material)
		-> FMaterialProgramStatistics;
	ENGINE_API auto GetLoadedMaterialFamilyStatistics(const DMaterialInterface& Material)
		-> FLoadedMaterialFamilyStatistics;

	struct FMaterialCompilationConfiguration
	{
		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
		EMaterialQualityLevel Quality = EMaterialQualityLevel::High;
		ERHIBackend Backend = ERHIBackend::Vulkan;
		auto operator==(const FMaterialCompilationConfiguration&) const -> bool = default;
	};

	struct FMaterialCompilationInputStamp
	{
		uint64 ContentRevision = 0;
		uint64 ShaderRevision = 0;
		uint64 BulkRevision = 0;
		bool bCooked = false;
		std::filesystem::path CookRoot;
		ECookTargetPlatform CookTarget = ECookTargetPlatform::Win64;
		auto operator==(const FMaterialCompilationInputStamp&) const -> bool = default;
	};

	// Configuration identity never changes. Selection and installed output are independent.
	struct FMaterialCompilationResource
	{
		explicit FMaterialCompilationResource(FMaterialCompilationConfiguration InConfiguration) : Configuration(InConfiguration) {}
		const FMaterialCompilationConfiguration Configuration;
		FWeakObjectPtr Material;
		FMaterialResource RenderLayer;
		FMaterialCompilationInputStamp RequestedInput;
		std::weak_ptr<const FMaterialCompileRequest> Compilation;
		std::shared_ptr<FMaterialProgramPreparationState> CookedPreparation;
		uint64 CompiledRevision = 0;
		uint64 CompiledDependencyRevision = 0;
		uint64 RequestId = 0;
		EMaterialCompileState Outcome = EMaterialCompileState::NeverRequested;
		EMaterialCompileCacheOutcome CacheOutcome = EMaterialCompileCacheOutcome::None;
		std::string Target;
		std::vector<MIR::FSource> RequestedExpressionSources;
		std::vector<MIR::FSource> AcceptedExpressionSources;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		std::string CookDiagnostic;
		FMaterialRenderProxyRef Proxy;
		bool bRequested = false;
	};

	struct FMaterialCompilationState
	{
		uint64 AuthoredRevision = 1;
		uint64 SubmittedRevision = 1;
		uint64 NextRequestId = 0;
		std::shared_ptr<FMaterialCompilationResource> SelectedResource;
		mutable std::vector<std::shared_ptr<FMaterialCompilationResource>> Resources;
		FMaterialStaticProperties LastObservedShaderProperties;
		std::vector<FMaterialCompilerParameterDeclaration> LastObservedParameters;
		std::optional<std::chrono::steady_clock::time_point> EditCompileDeadline;
	};

	// Defines the shared parameter-resolution and render-update contract for materials.
	DCLASS()
	class DMaterialInterface : public DObject
	{
		GENERATED_BODY()
		friend ENGINE_API auto NotifyMaterialFunctionChanged(const DMaterialFunctionInterface& Function) -> void;
		friend ENGINE_API auto NotifyMaterialParameterCollectionChanged(
			DMaterialParameterCollection& Collection) -> void;
	public:
		ENGINE_API explicit DMaterialInterface(const FObjectInitializer& ObjectInitializer);
		virtual auto IsDynamicInstance() const -> bool { return false; }
#if DURIN_WITH_EDITORONLY_DATA
		auto GetImportProvenance() const -> const FMaterialImportProvenance& { return ImportProvenance; }
		// Changes only persisted editor metadata, without invalidating compiled material state.
		ENGINE_API auto SetImportProvenance(FMaterialImportProvenance InProvenance) -> bool;
#endif

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
		// Returns the accepted program without initiating loading or compilation.
		ENGINE_API virtual auto GetAcceptedCompiledProgram() const
			-> std::shared_ptr<const FMaterialCompilerResult>;
		// Nonblocking GameThread queries. Completeness covers the current material
		// program, independently of texture residency and component PSOs.
		ENGINE_API virtual auto IsCompiling() const -> bool;
		ENGINE_API virtual auto IsComplete() const -> bool;
		// Asset completion covers all requested configurations; installed validity is independent.
		auto IsCompilationFinished() const -> bool { return !IsCompiling(); }
		ENGINE_API auto HasValidGameThreadShaderMap() const -> bool;
		// Background preparation publishes through the existing compiler manager.
		// Synchronous also installs the program before returning; None starts no work.
		ENGINE_API auto CacheResourceShadersForRendering(
			EMaterialShaderPrecompileMode PrecompileMode = EMaterialShaderPrecompileMode::Default) -> void;
		// Effective textures of the accepted program; replaces OutTextures.
		ENGINE_API virtual auto GetUsedTextures(std::vector<DTexture2D*>& OutTextures) const -> void;
		// Editor preview only: selects an explicit quality and a feature level no
		// greater than the initialized RHI capability.
		ENGINE_API auto RequestPreviewProgramCompile(
			EMaterialQualityLevel Quality, ERHIFeatureLevel FeatureLevel) -> bool;
		auto GetAcceptedExpressionSources() const -> std::span<const MIR::FSource>
		{
			return CompilationState.SelectedResource->AcceptedExpressionSources;
		}

		auto GetMaterialCompileDiagnostics() const
			-> std::span<const FMaterialProgramDiagnostic>
		{
			return CompilationState.SelectedResource->Diagnostics;
		}
		ENGINE_API auto GetMaterialCompileStatus() const -> FMaterialCompileStatus;

		auto GetCookedProgramData() const -> const FBulkData&
		{
			return CookedProgramData;
		}
		auto GetMaterialCookDiagnostic() const -> const std::string&
		{
			return CompilationState.SelectedResource->CookDiagnostic;
		}
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;
		ENGINE_API auto BeginCacheForCookedPlatformData(FObjectCookContext& Context) -> void override;
		ENGINE_API static auto AppendToClassSchema(FObjectCookContext& Context) -> void;
		// Owner-thread detached preparation; never applies a result to this material.
		ENGINE_API auto PrepareCookedProgram(ECookTargetPlatform Target,
			std::span<const FMaterialCookConfiguration> Configurations, const FAssetBuildTaskContext& Control = {})
			-> TShaderValueResult<FBulkData>;

		// Tests the canonical Parent chain without relying on reverse registration state.
		ENGINE_API auto IsDependent(const DMaterialInterface* TestDependency) const -> bool;
		ENGINE_API auto PrecachePSOs(ERHIFeatureLevel FeatureLevel,
			std::span<const FPSOPrecacheVertexFactoryData> VertexFactoryDataList, const FPSOPrecacheParams& PrecacheParams,
			EPSOPrecachePriority Priority, std::vector<FMaterialPSOPrecacheRequestID>& OutMaterialPSORequestIDs) -> std::expected<void, std::string>;
		ENGINE_API auto GetMaterialResource(ERHIFeatureLevel FeatureLevel,
			EMaterialQualityLevel Quality = GetMaterialQualityLevel()) const -> FMaterialResource;
		ENGINE_API auto GetAcceptedCompiledProgram(ERHIFeatureLevel FeatureLevel,
			EMaterialQualityLevel Quality = GetMaterialQualityLevel()) const -> std::shared_ptr<const FMaterialCompilerResult>;
		ENGINE_API auto CacheResourceShadersForRendering(ERHIFeatureLevel FeatureLevel,
			EMaterialShaderPrecompileMode PrecompileMode = EMaterialShaderPrecompileMode::Default) -> void;
		ENGINE_API auto CacheResourceShadersForRendering(ERHIFeatureLevel FeatureLevel,
			EMaterialQualityLevel Quality, EMaterialShaderPrecompileMode Mode = EMaterialShaderPrecompileMode::Default) -> void;
		ENGINE_API auto GetMaterialCompileStatus(ERHIFeatureLevel FeatureLevel,
			EMaterialQualityLevel Quality = GetMaterialQualityLevel()) const -> FMaterialCompileStatus;
		ENGINE_API auto GetMaterialCookDiagnostic(ERHIFeatureLevel FeatureLevel,
			EMaterialQualityLevel Quality = GetMaterialQualityLevel()) const -> std::string;
		ENGINE_API auto GetRenderProxy(ERHIFeatureLevel FeatureLevel,
			EMaterialQualityLevel Quality = GetMaterialQualityLevel()) const -> FMaterialRenderProxyRef;
		ENGINE_API auto PrecachePSOs(IRendererModule& Renderer, ERHIFeatureLevel FeatureLevel,
			std::span<const FPSOPrecacheVertexFactoryData> VertexFactoryDataList, const FPSOPrecacheParams& PrecacheParams,
			EPSOPrecachePriority Priority, std::vector<FMaterialPSOPrecacheRequestID>& OutMaterialPSORequestIDs) -> std::expected<void, std::string>;
		ENGINE_API auto GetRenderProxy() const
			-> FMaterialRenderProxyRef;
		// Refreshes native texture bindings after a referenced texture package was
		// atomically replaced. Authored material state and dirty flags are unchanged.
		ENGINE_API auto RefreshReloadedAssetBindings() -> void;
		// Includes inherited changes; callbacks should invalidate snapshots, not edit owners.
		auto GetParameterChanges() -> FMaterialParameterChangedEvent& { return ParameterChanges; }
		// GameThread only. The active configuration's effective resource, without
		// fallback. Its accepted program and properties belong to the same generation.
		auto GetMaterialResource() const -> FMaterialResource { return BuildMaterialResource(); }
		// Diagnostic change count; never used for cache validity or request lifetimes.
		auto GetRenderChangeCount() const -> uint64 { return RenderChangeCount; }
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	protected:
		ENGINE_API auto PostEditChangePropertyWithContext(const FPropertyChangedEvent& Event, FObjectCacheContext& Context) -> void;
		ENGINE_API auto AdmitCookedProgram(const FMaterialStaticProperties& Properties,
			std::shared_ptr<const FMaterialCompilerResult> Program,
			std::shared_ptr<FMaterialCompilationResource> Resource = {}) -> std::expected<void, std::string>;
		// A transient runtime owner can select only an already compiled compatible variant.
		ENGINE_API auto AdoptParentRuntimeProgram() -> bool;
		FBulkData CookedProgramData;
		ENGINE_API auto RequestProgramCompile(
			const FMaterialStaticProperties& CandidateProperties,
			bool bForceRecompile = false, FObjectCacheContext* Context = nullptr,
			std::optional<FMaterialCompilerEnvironment> RequestedEnvironment = std::nullopt,
			std::shared_ptr<FMaterialCompilationResource> Resource = {}) -> bool;
		// Invalidates authored dependencies before requesting detached replacements.
		ENGINE_API auto InvalidateMaterialCompilation(bool bIncludeSelf = true,
			bool bOnlyIfShaderChanged = false, FObjectCacheContext* Context = nullptr) -> void;
		FMaterialCompilationState CompilationState;

		ENGINE_API virtual auto BuildMaterialResource() const
			-> FMaterialResource;
		ENGINE_API auto PublishMaterialRenderProxyState() -> void;
		ENGINE_API auto NotifyParameterChanges() -> void;
		// Optional parameter notifications reuse the publication snapshot and run
		// only after every affected owner's render state has been updated.
		ENGINE_API auto MarkRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags,
			bool bNotifyParameterChanges = false, FObjectCacheContext* Context = nullptr) -> void;

	private:
		auto ResolveResourceProperties() const -> FMaterialStaticProperties;
		auto BuildMaterialResource(FMaterialResource Accepted) const -> FMaterialResource;
		auto PublishCompilationResources() const -> void;
		auto ReleaseCompilationResources() -> void;
		auto PumpResourcePreparations(const FAssetCompileProcessParams& Params) -> bool;
		auto CancelResourceCompilation() -> void;
		auto HasResourceRenderReferences() const -> bool;
		auto ValidatePreparedMaterialProgram(const FMaterialStaticProperties& Properties,
			std::shared_ptr<const FMaterialCompilerResult> Program) -> std::expected<std::shared_ptr<const FMaterialCompilerResult>, std::string>;
		friend class FShaderCompilingManager;
		friend ENGINE_API auto HasPendingMaterialCompilation(const DMaterialInterface&) -> bool;
		auto GetRenderingCompilationResource() const -> std::shared_ptr<FMaterialCompilationResource>;
		auto CaptureCompilationInputStamp() const -> FMaterialCompilationInputStamp;
		auto PollResourcePreparation(const std::shared_ptr<FMaterialCompilationResource>& Resource, bool Synchronous) -> void;
		auto GetResourceCompileStatus(const std::shared_ptr<FMaterialCompilationResource>& Resource) const -> FMaterialCompileStatus;
		auto GetCompilationResource(ERHIFeatureLevel Level, EMaterialQualityLevel Quality, bool Create = false) const -> std::shared_ptr<FMaterialCompilationResource>;
		auto CacheResourceShaders(const std::shared_ptr<FMaterialCompilationResource>& Resource,
			EMaterialShaderPrecompileMode Mode) -> FAssetCompileProcessResult;
		auto BroadcastParameterChanges(const TObjectCacheIterator<DMaterialInterface>& Dependents) -> void;
	private:
		friend struct Private::FMaterialCompilationLifecycle;
#if DURIN_WITH_EDITORONLY_DATA
		DPROPERTY(EditorOnly)
		FMaterialImportProvenance ImportProvenance;
#endif
		// Retires the failed owner's complete renderable generation and publishes ErrorMaterial.
		auto RetireFailedMaterialGeneration(FObjectCacheContext* Context = nullptr) -> void;
		auto PublishMaterialRenderProxyState(FMaterialResource LocalLayer) -> void;
		auto SubmitMaterialRenderProxyState(FMaterialResource LocalLayer) const -> void;

		FMaterialParameterChangedEvent ParameterChanges;
		uint64 RenderChangeCount = 1;
		FMaterialPSOPrecacheOwner PSOPrecacheOwner;
		mutable std::shared_ptr<const FMaterialParameterReachability> ParameterReachability;
		mutable uint64 ParameterReachabilityProgramRevision = 0;
		mutable std::vector<FMaterialFunctionOwnerStamp> ParameterReachabilityFunctionOwners;
		mutable std::weak_ptr<const FMaterialCompilerResult> ParameterReachabilityCookedProgram;
		mutable FMaterialRenderProxyRef MaterialRenderProxy;
		mutable bool bMaterialProxyPublicationDirty = true;
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
