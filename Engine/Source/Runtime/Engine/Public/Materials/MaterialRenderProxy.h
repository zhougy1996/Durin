#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Materials/MaterialRenderTypes.h"
#include "Materials/MaterialTypes.h"
#include "Templates/RefCounting.h"

namespace Durin
{
	class FMaterialShaderMap;
	// Owns counted RHI resources and sampling policy after leaving the object thread.
	struct FMaterialLocalTextureValue
	{
		FRHITextureReferenceRef Texture;
		FMaterialSamplerState SamplerState;
		EMaterialTextureFallback TextureFallback = EMaterialTextureFallback::White;
	};

	// The selected alternative is the only type authority; no live objects cross this boundary.
	struct FMaterialLocalRenderParameter
	{
		FGuid Id;
		std::variant<float, FVector2, FVector3, FVector4, FMaterialLocalTextureValue> Value = 0.0f;
		ENGINE_API auto GetType() const -> EMaterialParameterType;
	};

	// Publishes a complete accepted contract for either material asset kind.
	struct FMaterialLocalRenderLayer
	{
		std::vector<FMaterialLocalRenderParameter> Parameters;
		std::optional<FMaterialStaticProperties> StaticProperties;
		std::shared_ptr<const FMaterialCompilerResult> CompiledProgram;
	};

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

	struct FMaterialRenderProxyPublication
	{
		FMaterialLocalRenderLayer LocalLayer;
	};

	// Reports proxy-owned publication and render-thread resolution work.
	struct FMaterialRenderProxyCounters
	{
		uint64 QueuedPublicationCount = 0;
		uint64 QueuedPublicationWaveCount = 0;
		uint64 PublicationCount = 0;
		uint64 CoalescedPublicationCount = 0;
		uint64 PendingPublicationCount = 0;
		uint64 CopiedMaterialPayloadBytes = 0;
		uint64 ResolutionCacheHitCount = 0;
		uint64 ResolutionCacheMissCount = 0;
		uint64 BindingUpdateCount = 0;
		uint64 RepresentationValidationFailureCount = 0;
		uint64 ParameterBindingBuildCount = 0;
		uint64 IndexedParameterWriteCount = 0;
	};

	// Monotonic accepted-publication epoch; independent of resettable diagnostics.
	ENGINE_API auto GetMaterialRenderProxyPublicationRevision() -> uint64;

	ENGINE_API auto GetMaterialRenderProxyCounters()
		-> FMaterialRenderProxyCounters;
	// Reset only on GameThread after relevant publication commands are drained.
	ENGINE_API auto ResetMaterialRenderProxyCounters() -> void;
	ENGINE_API auto RecordMaterialBindingUpdate() -> void;

	// Owns render-thread material state behind one stable counted identity.
	class FMaterialRenderProxy final
	{
	public:
		FMaterialRenderProxy() = default;

		ENGINE_API auto AddRef() const -> uint32;
		ENGINE_API auto Release() const -> uint32;
		auto GetRefCount() const -> uint32
		{
			return ReferenceCount.load(std::memory_order_relaxed);
		}

		// Applies an owned snapshot in render-command order and invalidates resolution.
		ENGINE_API auto ApplyPublication_RenderThread(
			FMaterialRenderProxyPublication Publication
			) -> bool;

		// Accepts an immediate game-thread snapshot; true means accepted, not executed.
		// Queues the latest game-thread publication. At most one render command
		// owns the pending wave for this proxy; later publications replace that
		// wave until the render thread takes ownership.
		ENGINE_API auto QueuePublication_GameThread(
			FMaterialRenderProxyPublication Publication
			) -> bool;

		// Builds the accepted layout once after each applied publication.
		ENGINE_API auto Resolve_RenderThread() -> const FMaterialRenderData&;

		ENGINE_API auto GetPublicationCount_RenderThread() const -> uint64;
		ENGINE_API auto GetResolutionCount_RenderThread() const -> uint64;

	private:
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
		ENGINE_API auto ApplyPendingPublication_RenderThread() -> bool;

		~FMaterialRenderProxy() = default;

		mutable std::atomic<uint32> ReferenceCount = 0;
		FMaterialLocalRenderLayer LocalLayer;

		FMaterialRenderData CachedResolvedData;
		// Diagnostic counts never participate in ordering or cache validity.
		uint64 PublicationCount = 0;
		uint64 ResolutionCount = 0;
		bool bHasResolvedData = false;

		// PendingPublication is the game-thread publication wave. It is never
		// read by render code without taking this mutex; the render-thread state
		// above remains owned exclusively by the rendering thread.
		mutable std::mutex PublicationMutex;
		std::optional<FMaterialRenderProxyPublication> PendingPublication;
		bool bPublicationCommandQueued = false;
	};

	// Converts one selected value to its counted render-safe representation without
	// changing authored values or interpreting parameter identities as semantics.
	ENGINE_API auto BuildMaterialLocalRenderParameter(
		const FGuid& Id,
		const FMaterialParameterValue& Value
		) -> FMaterialLocalRenderParameter;

	// Applies one render-safe value by its exact canonical
	// identity and type.
	ENGINE_API auto ApplyMaterialLocalRenderParameter(
		FMaterialRenderRepresentationBuilder& RepresentationBuilder,
		const FMaterialLocalRenderParameter& Parameter
		) -> bool;

	// Transfers a game-thread owner reference into the accepted render stream.
	// If admission is already closed, ordinary counted release happens locally.
	ENGINE_API auto ReleaseMaterialRenderProxy_GameThread(
		FMaterialRenderProxyRef Proxy
		) -> void;
}
