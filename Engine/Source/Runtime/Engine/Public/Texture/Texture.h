#pragma once

#include "EngineAPI.h"
#include "Asset/AssetImportData.h"
#include "Asset/Cook.h"
#include "Asset/BulkData.h"
#include "Asset/EditorBulkData.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "RHIResources.h"
#include "Texture/TextureSource.h"

#include "Texture.gen.h"

namespace Durin
{
	class FTextureResource;
	class FTextureReference;
	class FTextureResourceUpdate;
	class DTexture;

	// Progress of CPU initialization/publication, independent of usable fallback.
	DENUM()
	enum class ETextureResourceUpdateState : uint8
	{
		Idle,
		Pending,
		Building,
		Succeeded,
		Failed,
		Closed,
	};

	// Common reflected boundary and render-resource lifecycle owner for texture assets.
	DCLASS(Abstract)
	class DTexture : public DObject
	{
		GENERATED_BODY()

	public:
		ENGINE_API ~DTexture() override;
		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API auto BeginDestroy() -> void override;

		auto GetSource() const -> const FTextureSource& { return Source; }
		// GameThread only. Adopts prepared source compatible with this texture family,
		// binds ownership and cancels pending authored builds; does not read or validate payloads.
		ENGINE_API auto SetSource(FTextureSource Value) -> void;

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

		// Queries installed CPU data without loading bulk data or updating resources.
		virtual auto HasPlatformData() const -> bool = 0;
		auto GetCookedPlatformData() const -> const FBulkData&
		{
			return CookedPlatformData;
		}
		// GameThread only. Loads and installs cooked data synchronously when absent,
		// then calls UpdateResource (GPU completion is asynchronous). Does not build
		// authored data. Already-installed data succeeds without another update.
		// On failure, logs the texture path and reason and returns false.
		ENGINE_API auto EnsurePlatformDataLoadedBlocking() -> bool;

		// Asynchronously uploads installed platform data; failed replacement retains the prior allocation.
		ENGINE_API auto UpdateResource() -> void;

		ENGINE_API auto GetTextureReferenceRHI() const
			-> FRHITextureReferenceRef;
		ENGINE_API auto GetResourceUpdateState() const -> ETextureResourceUpdateState;
		ENGINE_API auto HasUsableResource() const -> bool;
		ENGINE_API auto IsResourceUpdatePending() const -> bool;
		// Reload-only prepublication receipt. With an active RHI, drains this
		// texture's admitted upload and reports whether a usable allocation exists.
		// A headless runtime accepts the prepared CPU product without GPU evidence.
		ENGINE_API auto FinishReloadResourcePreparation() -> bool;
		// GameThread only. Captures the last consumed successful allocation, retaining no UObject.
		// Render commands may retain this value for fixed-input work; ordinary bindings use the stable reference.
		ENGINE_API auto GetPublishedTexture() const -> FTextureRHIRef;

	protected:
		ENGINE_API explicit DTexture(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto InvalidateAuthoredBuild() -> void;
		// Restricted to family serializers and blocking loaders.
		auto GetMutableCookedPlatformData() -> FBulkData&
		{
			return CookedPlatformData;
		}

		// Drops installed CPU data for lazy cooked loading; leaves published GPU resources intact.
		virtual auto ResetPlatformData() -> void = 0;
		// Builds authored data synchronously with load-time mutation policy and updates resources.
		// Logs failures locally, including the texture path.
		virtual auto BuildPlatformDataForLoad() -> void = 0;

		virtual auto CreateRenderResourceCandidate(
			FTextureReference* TextureReference)
			-> std::unique_ptr<FTextureResource> = 0;
		// Installs family-specific cooked data and queues its resource update.
		// Logs failures locally and returns false without updating resources.
		virtual auto LoadCookedPlatformData() -> bool = 0;

	private:
		friend auto ::Durin::ContributeEngineCookAsset(
			DObject&, std::string_view, FCookContext&, std::string&) -> bool;

		auto ContributeToCook(FCookContext& Context,
			std::string_view VirtualPackagePath, std::string& OutError) -> bool;

		auto ReleaseRenderResources() -> void;
		auto StartResourceUpdate(std::unique_ptr<FTextureResource> Candidate) -> void;
		auto ConsumeResourceUpdate() -> void;

		// The asset owns its render representation and the stable identity used by consumers.
		std::unique_ptr<FTextureReference> TextureReference;
		std::unique_ptr<FTextureResource> RenderResource;
		std::shared_ptr<FTextureResourceUpdate> PendingUpdate;
		ETextureResourceUpdateState LastUpdateState = ETextureResourceUpdateState::Idle;
		bool bTextureReferenceInitializationQueued = false;
		bool bAcceptingRenderResourceBuilds = true;

		DPROPERTY(EditorOnly)
		TObjectPtr<DAssetImportData> AssetImportData;

		DPROPERTY(EditorOnly)
		FTextureSource Source;

		FBulkData CookedPlatformData;

	};
}
