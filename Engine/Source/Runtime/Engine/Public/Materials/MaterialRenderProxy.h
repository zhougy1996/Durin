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
		auto operator==(const FMaterialLocalTextureValue&) const -> bool = default;
	};

	// The selected alternative is the only type authority; no live objects cross this boundary.
	struct FMaterialLocalRenderParameter
	{
		FGuid Id;
		std::variant<float, FVector2, FVector3, FVector4, FMaterialLocalTextureValue> Value = 0.0f;
		ENGINE_API auto GetType() const -> EMaterialParameterType;
		auto operator==(const FMaterialLocalRenderParameter&) const -> bool = default;
	};

	// Effective resource for the selected material configuration. Publication and
	// PSO collection consume this same program/property/parameter snapshot.
	struct FMaterialResource
	{
		std::vector<FMaterialLocalRenderParameter> Parameters;
		std::optional<FMaterialStaticProperties> StaticProperties;
		std::shared_ptr<const FMaterialCompilerResult> CompiledProgram;
		// Only meaningful without an accepted program; retained programs stay usable.
		bool bIsCompiling = false;
		ENGINE_API auto HasValidGameThreadShaderMap() const -> bool;
		auto GetStaticProperties() const -> FMaterialStaticProperties
		{ return StaticProperties.value_or(FMaterialStaticProperties{}); }
		auto GetBlendMode() const -> EMaterialBlendMode { return GetStaticProperties().BlendMode; }
		auto IsTwoSided() const -> bool { return GetStaticProperties().bTwoSided; }
		auto operator==(const FMaterialResource&) const -> bool = default;
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

	// Reports proxy-owned publication and render-thread resolution work.
	struct FMaterialRenderProxyCounters
	{
		uint64 QueuedPublicationCount = 0;
		uint64 PublicationCount = 0;
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
			FMaterialResource Publication
			) -> void;

		// Every command owns an immutable layer; consumers observe command order.
		ENGINE_API auto QueuePublication_GameThread(
			FMaterialResource Publication
			) -> void;

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

		~FMaterialRenderProxy() = default;

		mutable std::atomic<uint32> ReferenceCount = 0;
		FMaterialResource LocalLayer;

		FMaterialRenderData CachedResolvedData;
		// Diagnostic counts never participate in ordering or cache validity.
		uint64 PublicationCount = 0;
		uint64 ResolutionCount = 0;
		bool bHasResolvedData = false;

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
