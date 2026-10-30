#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Asset/AssetImportData.h"
#include "Asset/Cook.h"
#include "Asset/BulkData.h"
#include "Asset/EditorBulkData.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "Texture/TextureSource.h"

#include "Texture.gen.h"

namespace Durin
{
	struct FTextureSource;
	class DAssetImportData;
	class FTextureResource;
	class FTextureReference;
	class DTexture;
	namespace TexturePrivate
	{
		template<class TPlatformData, class TTexture>
		auto LoadCookedPlatformData(TTexture&, bool) -> void;
	}

	// Common reflected boundary and render-resource lifecycle owner for texture assets.
	DCLASS(Abstract)
	class DTexture : public DObject
	{
		GENERATED_BODY()

	public:
		ENGINE_API ~DTexture() override;
		ENGINE_API auto BeginCacheForCookedPlatformData(FObjectCookContext& Context) -> void override;
		ENGINE_API auto IsCachedCookedPlatformDataLoaded(const FObjectCookContext& Context) const -> bool override;
		ENGINE_API auto ClearCachedCookedPlatformData(const FObjectCookContext& Context) -> void override;
		ENGINE_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError> override;
		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API auto Modify(bool bAlwaysMarkDirty = true) -> bool override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError> override;
		ENGINE_API auto PreEditUndo() -> void override;
		// Full-object edits retire prior work and begin independent authored compilation.
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		// Prepares the current texture's platform data; false completes synchronously.
		// Reuses existing work. Source import and saving remain separate operations.
		ENGINE_API auto CachePlatformData(bool bAsyncCache = false) -> void;
		// Authored data: schedules cache lookup/build without reading source pixels.
		// Cooked families schedule worker reads/decode when the scheduler is available.
		// Repeated calls while installed or pending are no-ops.
		ENGINE_API auto BeginCachePlatformData() -> void;
		// CPU cache completion, including failure; does not imply GPU readiness.
		ENGINE_API auto IsAsyncCacheComplete() const -> bool;
		// Explicit CPU-cache completion boundary; GPU upload remains asynchronous.
		// Completes existing work and installs it directly; never starts a new cache request.
		// Reports installed CPU-data readiness; per-request outcomes are delivered through completion.
		// Zero only queries installed data. A finite nonnegative timeout bounds the wait,
		// not executing callbacks. Expiry does not cancel work; no GameThread queue pumping.
		ENGINE_API auto FinishCachePlatformData(double TimeoutSeconds = 300.0) -> bool;
		// GameThread only. Completes admitted compilation and applies its result.
		// Reentrant same-owner submissions are completed before return; GPU initialization remains asynchronous.
		ENGINE_API auto BlockOnAnyAsyncBuild() -> void;
		ENGINE_API auto BeginDestroy() -> void override;

#if DURIN_WITH_EDITORONLY_DATA
		auto GetSource() const -> const FTextureSource& { return Source; }
		// GameThread only. Adopts prepared source compatible with this texture family,
		// binds ownership and invalidates authored builds, CPU data and GPU publication; does not read payloads.
		ENGINE_API auto SetSource(FTextureSource Value) -> void;
		// GameThread storage-only commit. Rejects different pixels, descriptors or bulk
		// instance identity; preserves build state, ownership and acquired byte buffers.
		ENGINE_API auto ReplaceSourceStorage(FTextureSource Value) -> bool;

		auto GetAssetImportData() const -> const DAssetImportData*
		{
			return AssetImportData.Get();
		}
		auto GetAssetImportData() -> DAssetImportData*
		{
			return AssetImportData.Get();
		}
		// GameThread only. Accepts validated import data owned by this texture as an inner object.
		ENGINE_API auto SetAssetImportData(
			DAssetImportData& Value) -> void;
#endif

		// Queries installed CPU data without loading bulk data or updating resources.
		virtual auto HasPlatformData() const -> bool = 0;
		auto GetCookedPlatformData() const -> const FBulkData&
		{
			return CookedPlatformData;
		}
		// Asynchronously uploads installed platform data; direct updates invalidate the prior allocation.
		ENGINE_API auto UpdateResource() -> void;
		// Releases GPU resources while retaining CPU data, cache work and the reusable reference.
		ENGINE_API auto ReleaseResource() -> void;
		// GameThread-only borrowed render representation, valid until replacement or release.
		ENGINE_API auto GetResource() -> FTextureResource*;
		ENGINE_API auto GetResource() const -> const FTextureResource*;

		ENGINE_API auto GetTextureReferenceRHI() const
			-> FRHITextureReferenceRef;
		// GPU initialization only. CPU caching is queried through IsAsyncCacheComplete().
		ENGINE_API auto HasPendingRenderResourceInitialization() const -> bool;

	protected:
		ENGINE_API explicit DTexture(const FObjectInitializer& ObjectInitializer);
#if DURIN_WITH_EDITORONLY_DATA
		ENGINE_API auto InvalidateAuthoredBuild() -> void;
		// Family source commit only, after prior compilation has retired.
		ENGINE_API auto AdoptBuildSource(FTextureSource Value) -> void;
#endif
		ENGINE_API auto InvalidateRenderResource() -> void;
		// Restricted to family serializers and loaders.
		auto GetMutableCookedPlatformData() -> FBulkData&
		{
			return CookedPlatformData;
		}

		std::shared_ptr<FPackageResourceRequest> CookedLoad;
		// One prepared result's installation, shared by explicit finish and automatic wakeup.
		std::function<void()> CookedPublication;
		template<class TPlatformData, class TTexture>
		friend auto TexturePrivate::LoadCookedPlatformData(TTexture&, bool) -> void;

		// Drops installed CPU data for lazy cooked loading; leaves published GPU resources intact.
		virtual auto ResetPlatformData() -> void = 0;
		// Begins authored caching with load-time mutation policy and updates resources on completion.
		// Logs failures locally, including the texture path.
		virtual auto BuildPlatformDataForLoad(bool bAsyncCache) -> void = 0;

		virtual auto CreateResource(
			FTextureReference* TextureReference)
			-> std::unique_ptr<FTextureResource> = 0;
		// Starts family-specific cooked decoding and publishes on GameThread.
		virtual auto LoadCookedPlatformData(bool bAsync) -> void = 0;

	private:


		auto ReleaseRenderResources() -> void;
		friend struct FTextureCookTestAccess;

		// The asset owns its render representation and the stable identity used by consumers.
		std::unique_ptr<FTextureReference> TextureReference;
		std::unique_ptr<FTextureResource> RenderResource;
		FRenderCommandFence ResourceFence;
		bool bTextureReferenceInitializationQueued = false;

#if DURIN_WITH_EDITORONLY_DATA
		DPROPERTY(EditorOnly)
		TObjectPtr<DAssetImportData> AssetImportData;

		DPROPERTY(EditorOnly)
		FTextureSource Source;
#endif

		FBulkData CookedPlatformData;

	};
}
