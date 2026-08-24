#pragma once

#include "EngineAPI.h"
#include "Materials/MaterialRenderTypes.h"
#include "Materials/MaterialTypes.h"
#include "Templates/RefCounting.h"

#include <mutex>
#include <optional>

namespace Durin
{
	// Stores one render-safe local parameter value without retaining reflected objects.
	struct FMaterialLocalRenderParameter
	{
		FGuid Id;
		EMaterialParameterType Type = EMaterialParameterType::Scalar;
		float ScalarValue = 0.0f;
		FVector2 Vector2Value{0.0};
		FVector3 VectorValue{0.0};
		FRHITextureReferenceRef TextureValue;
	};

	// Publishes either a complete base layer or sparse instance overrides.
	struct FMaterialLocalRenderLayer
	{
		std::vector<FMaterialLocalRenderParameter> Parameters;
		std::optional<FMaterialStaticProperties> StaticProperties;
		std::shared_ptr<const FMaterialCompilerResult> CompiledProgram;
	};

	class FMaterialRenderProxy;
	using FMaterialRenderProxyRef = TRefCountPtr<FMaterialRenderProxy>;

	// Rebinds one stable primitive slot without copying material content.
	struct FMaterialRenderProxyBindingUpdate
	{
		uint32 SlotIndex = 0;
		FMaterialRenderProxyRef MaterialProxy;
		uint64 ComponentRevision = 0;
	};

	struct FMaterialRenderProxyPublication
	{
		FMaterialLocalRenderLayer LocalLayer;
		FMaterialRenderProxyRef ParentProxy;
		uint64 LocalVersion = 0;
	};

	// Reports proxy-owned publication and render-thread resolution work.
	struct FMaterialRenderProxyCounters
	{
		uint64 PublicationCount = 0;
		uint64 CoalescedPublicationCount = 0;
		uint64 ResolutionCacheHitCount = 0;
		uint64 ResolutionCacheMissCount = 0;
		uint64 StalePublicationCount = 0;
		uint64 BindingUpdateCount = 0;
		uint64 RepresentationValidationFailureCount = 0;
	};

	ENGINE_API auto GetMaterialRenderProxyCounters()
		-> FMaterialRenderProxyCounters;
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

		// Applies only strictly newer publications on the rendering thread.
		ENGINE_API auto ApplyPublication_RenderThread(
			FMaterialRenderProxyPublication Publication
			) -> bool;

		// Queues the newest game-thread publication. At most one render command
		// owns the pending wave for this proxy; later publications replace that
		// wave until the render thread takes ownership.
		ENGINE_API auto QueuePublication_GameThread(
			FMaterialRenderProxyPublication Publication
			) -> bool;

		// Resolves parent-first and reuses the cached snapshot only for the exact
		// (local version, parent identity, parent resolved version) key.
		ENGINE_API auto Resolve_RenderThread() -> const FMaterialRenderData&;

		ENGINE_API auto GetLocalVersion_RenderThread() const -> uint64;
		ENGINE_API auto GetResolvedVersion_RenderThread() const -> uint64;
		ENGINE_API auto GetObservedParentResolvedVersion_RenderThread() const
			-> uint64;
		ENGINE_API auto GetParentProxyIdentity_RenderThread() const
			-> const FMaterialRenderProxy*;
		ENGINE_API auto GetStalePublicationCount_RenderThread() const
			-> uint64;

	private:
		ENGINE_API auto ApplyPendingPublication_RenderThread() -> bool;

		~FMaterialRenderProxy() = default;

		mutable std::atomic<uint32> ReferenceCount = 0;
		FMaterialLocalRenderLayer LocalLayer;
		FMaterialRenderProxyRef ParentProxy;
		uint64 LocalVersion = 0;

		FMaterialRenderData CachedResolvedData;
		const FMaterialRenderProxy* CachedParentIdentity = nullptr;
		uint64 CachedLocalVersion = 0;
		uint64 ObservedParentResolvedVersion = 0;
		uint64 ResolvedVersion = 0;
		uint64 StalePublicationCount = 0;
		bool bHasResolvedData = false;
		bool bIsResolving = false;

		// PendingPublication is the game-thread publication wave. It is never
		// read by render code without taking this mutex; the render-thread state
		// above remains owned exclusively by the rendering thread.
		mutable std::mutex PublicationMutex;
		std::optional<FMaterialRenderProxyPublication> PendingPublication;
		bool bPublicationCommandQueued = false;
	};

	// Converts one reflected value to its counted render-safe representation.
	ENGINE_API auto BuildMaterialLocalRenderParameter(
		const FGuid& Id,
		EMaterialParameterType Type,
		const FMaterialParameterValue& Value
		) -> FMaterialLocalRenderParameter;

	// Applies one already normalized render-safe value by its exact canonical
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
