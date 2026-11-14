#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Materials/MaterialRenderTypes.h"
#include "Materials/MaterialTypes.h"
#include "Materials/MaterialParameterValues.h"
#include "Templates/RefCounting.h"

namespace Durin
{
	class FSceneInterface;
	class FMaterialShaderMap;

	class FMaterialRenderProxy;
	using FMaterialRenderProxyRef = TRefCountPtr<FMaterialRenderProxy>;

	// Rebinds one stable primitive slot without copying material content.
	// Submit immediately on the game thread through the scene, in proxy lifecycle order;
	// this value is not a deferred/asynchronous result or a replayable command.
	struct FMaterialRenderProxyBindingUpdate
	{
		uint32 SlotIndex = 0;
		FMaterialRenderProxyRef MaterialProxy;

	};

	// Reports proxy-owned publication and render-thread resolution work.
	struct FMaterialRenderProxyCounters
	{
		uint64 QueuedPublicationCount = 0;
		uint64 PublicationCount = 0;
		uint64 ProgramUpdateCount = 0;
		uint64 ParameterUpdateCount = 0;
		uint64 PropertyUpdateCount = 0;
		uint64 UniformCacheInvalidationCount = 0;
		uint64 PendingPublicationCount = 0;
		uint64 CopiedMaterialPayloadBytes = 0;
		uint64 ResolutionCacheHitCount = 0;
		uint64 ResolutionCacheMissCount = 0;
		uint64 BindingUpdateCount = 0;
		uint64 RepresentationValidationFailureCount = 0;
		uint64 ParameterBindingBuildCount = 0;
		uint64 IndexedParameterWriteCount = 0;
	};

	ENGINE_API auto GetMaterialRenderProxyCounters()
		-> FMaterialRenderProxyCounters;
	// Reset only on GameThread after relevant publication commands are drained.
	ENGINE_API auto ResetMaterialRenderProxyCounters() -> void;
	ENGINE_API auto RecordMaterialBindingUpdate() -> void;

	// Owns render-thread material state behind one stable counted identity.
	class FMaterialRenderProxy final : public FRenderResource
	{
	public:
		FMaterialRenderProxy() = default;
		ENGINE_API auto AddSceneReference(FSceneInterface& Scene) -> void;
		ENGINE_API auto RemoveSceneReference(FSceneInterface& Scene) -> void;
		ENGINE_API auto ReleaseRHI() -> void override;
		ENGINE_API static auto ReleaseRHIForAllResources() -> void;
		auto GetFriendlyName() const -> std::string override { return "MaterialRenderProxy"; }

		ENGINE_API auto AddRef() const -> uint32;
		ENGINE_API auto Release() const -> uint32;
		auto GetRefCount() const -> uint32
		{
			return ReferenceCount.load(std::memory_order_relaxed);
		}

		ENGINE_API auto SetProgram_GameThread(std::shared_ptr<const FMaterialProgram> Program) -> void;
		ENGINE_API auto SetProgram_RenderThread(std::shared_ptr<const FMaterialProgram> Program) -> void;
		ENGINE_API auto InitParameters_GameThread(FMaterialInstanceParameterSet Parameters) -> void;
		ENGINE_API auto InitParameters_RenderThread(FMaterialInstanceParameterSet Parameters) -> void;
		ENGINE_API auto UpdateParameter_GameThread(const FGuid& Id, const FMaterialParameterValue& Value) -> void;
		ENGINE_API auto UpdateParameter_RenderThread(const FGuid& Id, FMaterialParameterValue Value) -> void;
		ENGINE_API auto SetProperties_GameThread(std::optional<FMaterialStaticProperties> Properties) -> void;
		ENGINE_API auto SetProperties_RenderThread(std::optional<FMaterialStaticProperties> Properties) -> void;
		ENGINE_API auto SetCompiling_GameThread(bool Compiling) -> void;
		ENGINE_API auto SetCompiling_RenderThread(bool Compiling) -> void;
		ENGINE_API auto InvalidateUniformExpressionCache_GameThread(bool RecreateUniformBuffer = false) -> void;
		ENGINE_API auto InvalidateUniformExpressionCache_RenderThread(bool RecreateUniformBuffer = false) -> void;

		// RenderThread only. A missing/invalid program returns null without selecting a fallback.
		ENGINE_API auto GetMaterialNoFallback() -> const FMaterialRenderData*;
		// RenderThread only. Returns the accepted material or the explicit error material.
		ENGINE_API auto GetMaterialWithFallback() -> const FMaterialRenderData&;
		ENGINE_API auto IsCompiling() const -> bool;

		ENGINE_API auto GetPublicationCount_RenderThread() const -> uint64;
		ENGINE_API auto GetResolutionCount_RenderThread() const -> uint64;

	private:
		auto Resolve_RenderThread() -> const FMaterialRenderData&;
		// Bindings retain the exact program and parameter sequence they were built for.
		struct FParameterBinding
		{
			FGuid Id;
			EMaterialParameterType Type;
			uint32 FieldIndex = std::numeric_limits<uint32>::max();
			bool bActive = false;
		};
		auto UpdateParameterBindings() -> void;
		std::shared_ptr<const FMaterialShaderMap> BindingArtifact;
		std::vector<FParameterBinding> ParameterBindings;

		~FMaterialRenderProxy() override = default;

		mutable std::atomic<uint32> ReferenceCount = 0;
		const std::shared_ptr<FUniformExpressionCache> UniformExpressionCache = std::make_shared<FUniformExpressionCache>();
		std::shared_ptr<const FMaterialProgram> RenderingThreadProgram;
		FMaterialInstanceParameterSet ParameterValues;
		std::optional<FMaterialStaticProperties> Properties;
		bool bCompiling = false;
		bool bGameThreadCompiling = false;
		bool bCompilingInitialized = false;
		auto RecordUpdate_RenderThread() -> void;
		auto InvalidateStaticDrawLists_RenderThread() -> void;
		std::unordered_map<FSceneInterface*, uint32> SceneReferences;

		FMaterialRenderData CachedResolvedData;
		// Diagnostic counts never participate in ordering or cache validity.
		uint64 PublicationCount = 0;
		uint64 ResolutionCount = 0;
		bool bHasResolvedData = false;

	};

	// Transfers a game-thread owner reference into the accepted render stream.
	// If admission is already closed, ordinary counted release happens locally.
	ENGINE_API auto ReleaseMaterialRenderProxy_GameThread(
		FMaterialRenderProxyRef Proxy
		) -> void;
}
