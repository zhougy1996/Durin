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
	class FTextureAssetResource;
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

	// Retains the concrete allocation rendered by an asynchronous consumer.
	// FixedReference never changes its target after construction on RenderThread.
	struct FTextureResourceSnapshot
	{
		FTextureRHIRef Texture;
		FRHITextureReferenceRef FixedReference;
	};

	// Distinguishes input invalidation from terminal resource readiness notifications.
	enum class ETextureResourceChange : uint8 { Input, Completed, Closed };

	// GameThread notification for input admission, consumed completion and close.
	DECLARE_MULTICAST_DELEGATE_TwoParams(FTextureResourceChangedEvent, DTexture&, ETextureResourceChange)
	ENGINE_API auto OnTextureResourceChanged() -> FTextureResourceChangedEvent&;
	// Called by the Engine frame loop, including frames with rendering disabled.
	ENGINE_API auto PumpTextureResourceUpdates() -> void;

	// Identifies the latest completed resource update's actionable failure boundary.
	enum class ETextureRenderFailure : uint8
	{
		None,
		UnsupportedFormat,
		CreateOrUpload,
		UnavailableRHI,
		AdmissionRejected,
	};

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
		auto HasUsableResource() const -> bool { return ResourceSnapshot != nullptr; }
		auto IsResourceUpdatePending() const -> bool { return PendingUpdate != nullptr; }
		auto GetRenderFailure() const -> ETextureRenderFailure { return LastFailure; }
		// GameThread only. Retaining this snapshot does not retain the UObject.
		auto GetResourceSnapshot() const -> std::shared_ptr<const FTextureResourceSnapshot>
		{
			return ResourceSnapshot;
		}
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
		// Replaces the current concrete resource from installed platform data.
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
			-> std::unique_ptr<FTextureAssetResource> = 0;
		// Installs family-specific cooked data and queues its resource update.
		virtual auto LoadCookedPlatformData(std::string& OutError) -> bool = 0;

	private:
		auto ReleaseRenderResources() -> void;
		auto StartResourceUpdate(std::unique_ptr<FTextureAssetResource> Candidate) -> void;
		auto ConsumeResourceUpdate() -> void;
		friend ENGINE_API auto PumpTextureResourceUpdates() -> void;

		std::unique_ptr<FTextureReference> TextureReference;
		std::unique_ptr<FTextureAssetResource> RenderResource;
		std::shared_ptr<FTextureResourceUpdate> PendingUpdate;
		// An uninitialized family wrapper retains only immutable input, never GPU work.
		std::unique_ptr<FTextureAssetResource> NextInput;
		std::shared_ptr<const FTextureResourceSnapshot> ResourceSnapshot;
		ETextureRenderFailure LastFailure = ETextureRenderFailure::None;
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
