#pragma once

#include "EngineAPI.h"
#include "Asset/AssetImportData.h"
#include "Asset/BulkData.h"
#include "Asset/EditorBulkData.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "Delegates/Delegate.h"
#include "RHIResources.h"
#include "Texture/TextureSource.h"

#include "Texture.gen.h"

namespace Durin
{
	class FTextureResource;
	class FTextureReference;
	struct FTextureResourceState;
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

	// Distinguishes input invalidation from terminal resource readiness notifications.
	enum class ETextureResourceChange : uint8 { Input, Completed, Closed };

	// GameThread notification for input admission, consumed completion and close.
	DECLARE_MULTICAST_DELEGATE_TwoParams(FTextureResourceChangedEvent, DTexture&, ETextureResourceChange)
	ENGINE_API auto OnTextureResourceChanged() -> FTextureResourceChangedEvent&;
	// Called by the Engine frame loop, including frames with rendering disabled.
	ENGINE_API auto PumpTextureResourceUpdates() -> void;

	// Common reflected boundary and render-resource lifecycle owner for texture assets.
	DCLASS(Abstract)
	class DTexture : public DObject
	{
		GENERATED_BODY()

	public:
		ENGINE_API ~DTexture() override;
		ENGINE_API auto BeginDestroy() -> void override;

		ENGINE_API auto GetTextureReferenceRHI() const
			-> FRHITextureReferenceRef;
		ENGINE_API auto GetResourceUpdateState() const -> ETextureResourceUpdateState;
		ENGINE_API auto HasUsableResource() const -> bool;
		ENGINE_API auto IsResourceUpdatePending() const -> bool;
		// GameThread only. Captures the last consumed successful allocation, retaining no UObject.
		// Render commands may retain this value for fixed-input work; ordinary bindings use the stable reference.
		ENGINE_API auto GetPublishedTexture() const -> FTextureRHIRef;
		auto GetSource() const -> const FTextureSource& { return Source; }
		auto GetAssetImportData() const -> const DAssetImportData*
		{
			return AssetImportData.Get();
		}
		auto GetAssetImportData() -> DAssetImportData*
		{
			return AssetImportData.Get();
		}
		// Accepts validated import data owned by this texture as an inner object.
		ENGINE_API auto SetAssetImportData(
			DAssetImportData& Value, std::string& OutError) -> bool;
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

	protected:
		ENGINE_API explicit DTexture(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto SetSource(FTextureSource Value, std::string& OutError) -> bool;
		ENGINE_API auto InvalidateAuthoredBuild() -> void;
		ENGINE_API auto BindTextureSourceOwner() -> void;
		virtual auto ValidateSettingsAfterImportOrEdit(
			const FTextureSource& ProposedSource) const -> bool = 0;
		// Restricted to family serializers and blocking loaders.
		auto GetMutableCookedPlatformData() -> FBulkData&
		{
			return CookedPlatformData;
		}

		virtual auto CreateRenderResourceCandidate(
			FTextureReference* TextureReference)
			-> std::unique_ptr<FTextureResource> = 0;
		// Installs family-specific cooked data and queues its resource update.
		virtual auto LoadCookedPlatformData(std::string& OutError) -> bool = 0;

	private:
		auto ReleaseRenderResources() -> void;
		auto StartResourceUpdate(std::unique_ptr<FTextureResource> Candidate) -> void;
		auto ConsumeResourceUpdate() -> void;
		friend ENGINE_API auto PumpTextureResourceUpdates() -> void;

		std::unique_ptr<FTextureResourceState> ResourceState;

		DPROPERTY(EditorOnly)
		TObjectPtr<DAssetImportData> AssetImportData;

		DPROPERTY(EditorOnly)
		FTextureSource Source;

		FBulkData CookedPlatformData;

	};
}
