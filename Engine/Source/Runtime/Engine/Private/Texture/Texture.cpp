#include "Texture/Texture.h"

#include "DObject/Package.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Load.h"
#include "Asset/BulkData.h"

#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "Texture/TextureRenderResource.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	// GameThread ownership and terminal handoff; render commands only retain the active operation.
	struct FTextureResourceState
	{
		std::unique_ptr<FTextureReference> TextureReference;
		std::shared_ptr<FTextureResourceUpdate> PendingUpdate;
		// An uninitialized family wrapper retains only immutable input, never GPU work.
		std::unique_ptr<FTextureResource> NextInput;
		// Last consumed allocation: GameThread never reads the mutable RenderThread target.
		FTextureRHIRef PublishedTexture;
		ETextureResourceUpdateState LastUpdateState = ETextureResourceUpdateState::Idle;
		bool bTextureReferenceInitializationQueued = false;
		bool bAcceptingRenderResourceBuilds = true;
	};

	namespace
	{
		std::vector<DTexture*> UpdatingTextures;
		FTextureResourceChangedEvent ResourceChanged;
		bool bPumpingTextureResources = false;

		auto RetireTextureResource(std::unique_ptr<FTextureResource> Resource) -> void
		{
			if (!Resource) return;
			if (!Resource->IsInitialized()) return;
			Resource->BeginRelease_GameThread();
			BeginCleanupRenderResource(FDeferredRenderResourceCleanup(std::move(Resource)));
		}
	}

	auto OnTextureResourceChanged() -> FTextureResourceChangedEvent& { return ResourceChanged; }

	auto PumpTextureResourceUpdates() -> void
	{
		CheckGameThread();
		if (bPumpingTextureResources) return;
		bPumpingTextureResources = true;
		const auto Pending = UpdatingTextures;
		for (DTexture* Texture : Pending)
		{
			if (std::ranges::find(UpdatingTextures, Texture) != UpdatingTextures.end())
				Texture->ConsumeResourceUpdate();
		}
		bPumpingTextureResources = false;
	}

	DTexture::DTexture(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, ResourceState(std::make_unique<FTextureResourceState>())
	{
		ResourceState->TextureReference = std::make_unique<FTextureReference>();
		Source.BindOwner(this);
	}

	DTexture::~DTexture()
	{
		check(!ResourceState->bAcceptingRenderResourceBuilds);
		check(ResourceState->TextureReference == nullptr);
	}

	auto DTexture::BeginDestroy() -> void
	{
		ResourceState->bAcceptingRenderResourceBuilds = false;
		ReleaseRenderResources();
		ResourceChanged.Broadcast(*this, ETextureResourceChange::Closed);
		Super::BeginDestroy();
	}

	auto DTexture::ReleaseRenderResources() -> void
	{
		std::erase(UpdatingTextures, this);
		ResourceState->NextInput.reset();
		if (ResourceState->PendingUpdate)
		{
			ResourceState->PendingUpdate->Close();
			ResourceState->PendingUpdate->Wait();
			RetireTextureResource(ResourceState->PendingUpdate->TakeCandidate());
			ResourceState->PendingUpdate.reset();
		}
		ResourceState->PublishedTexture = nullptr;
		ResourceState->LastUpdateState = ETextureResourceUpdateState::Closed;
		if (ResourceState->bTextureReferenceInitializationQueued)
		{
			ResourceState->TextureReference->BeginRelease_GameThread();
			BeginCleanupRenderResource(
				FDeferredRenderResourceCleanup(std::move(ResourceState->TextureReference)));
		}
		else
		{
			ResourceState->TextureReference.reset();
		}
		ResourceState->bTextureReferenceInitializationQueued = false;
	}

	auto DTexture::GetTextureReferenceRHI() const
		-> FRHITextureReferenceRef
	{
		return ResourceState->TextureReference
			? ResourceState->TextureReference->GetTextureReferenceRHI()
			: FRHITextureReferenceRef{};
	}

	auto DTexture::GetResourceUpdateState() const -> ETextureResourceUpdateState
	{
		return ResourceState->PendingUpdate ? ResourceState->PendingUpdate->GetState() : ResourceState->LastUpdateState;
	}

	auto DTexture::HasUsableResource() const -> bool { return ResourceState->PublishedTexture != nullptr; }
	auto DTexture::IsResourceUpdatePending() const -> bool { return ResourceState->PendingUpdate != nullptr; }
	auto DTexture::GetPublishedTexture() const -> FTextureRHIRef
	{
		CheckGameThread();
		return ResourceState->PublishedTexture;
	}

	auto DTexture::SetSource(FTextureSource Value, std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!Value.IsValid() || !ValidateSettingsAfterImportOrEdit(Value))
		{
			OutError = "Texture source or authored settings are invalid for this texture type.";
			return false;
		}
		Value.BindOwner(this);
		Source = std::move(Value);
		Source.BindOwner(this);
		InvalidateAuthoredBuild();
		OutError.clear();
		return true;
	}

	auto DTexture::BindTextureSourceOwner() -> void
	{
		Source.BindOwner(this);
	}

	auto DTexture::InvalidateAuthoredBuild() -> void
	{
		CheckGameThread();
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
	}

	auto DTexture::SetAssetImportData(
		DAssetImportData& Value, std::string& OutError) -> bool
	{
		if (Value.GetOuter() != this)
		{
			OutError = "Texture import data must be an owned inner object.";
			return false;
		}
		if (!Value.Validate(OutError)) return false;
		AssetImportData = &Value;
		OutError.clear();
		return true;
	}

	auto DTexture::UpdateResource() -> void
	{
		CheckGameThread();
		if (!ResourceState->bAcceptingRenderResourceBuilds || IsPendingKill())
		{
			DURIN_WARN(
				"Texture render-resource build rejected after object teardown began. (texture: {})",
				GetObjectPath());
			return;
		}
		if (!HasPlatformData())
		{
			DURIN_WARN(
				"Texture render-resource build rejected without valid platform data. (texture: {})",
				GetObjectPath());
			return;
		}
		auto Candidate = CreateRenderResourceCandidate(ResourceState->TextureReference.get());
		check(Candidate != nullptr);
#if DURIN_BUILD_DEBUG
		Candidate->SetDebugOwner(GetPackage()
			? FName(GetPackage()->GetPackagePath()) : FName("<transient DTexture>"));
#endif
		if (ResourceState->PendingUpdate) ResourceState->NextInput = std::move(Candidate);
		else StartResourceUpdate(std::move(Candidate));
		ResourceChanged.Broadcast(*this, ETextureResourceChange::Input);
	}

	auto DTexture::StartResourceUpdate(std::unique_ptr<FTextureResource> Candidate) -> void
	{
		check(!ResourceState->PendingUpdate);
		ResourceState->PendingUpdate = std::make_shared<FTextureResourceUpdate>(std::move(Candidate));
		if (std::ranges::find(UpdatingTextures, this) == UpdatingTextures.end())
			UpdatingTextures.push_back(this);
		if (!GDynamicRHI)
		{
			DURIN_WARN("Texture update rejected: RHI is unavailable. (texture: {})", GetObjectPath());
			ResourceState->PendingUpdate->Reject();
			return;
		}
		const bool bInitializeReference = !ResourceState->bTextureReferenceInitializationQueued;
		// One admission owns both initialization steps, so rejection leaves no half-admitted reference.
		const bool bAccepted = TryEnqueueRenderCommand("TextureResourceUpdate",
			[Update = ResourceState->PendingUpdate, Reference = ResourceState->TextureReference.get(), bInitializeReference]
			(FRHICommandListImmediate& Commands) {
				Update->Execute_RenderThread(Commands, *Reference, bInitializeReference);
			});
		if (bAccepted) ResourceState->bTextureReferenceInitializationQueued = true;
		else
		{
			DURIN_WARN("Texture update rejected: render command admission is closed. (texture: {})", GetObjectPath());
			ResourceState->PendingUpdate->Reject();
		}
	}

	auto DTexture::ConsumeResourceUpdate() -> void
	{
		if (!ResourceState->PendingUpdate || !ResourceState->PendingUpdate->IsComplete()) return;
		ResourceState->LastUpdateState = ResourceState->PendingUpdate->GetState();
		auto Candidate = ResourceState->PendingUpdate->TakeCandidate();
		if (ResourceState->LastUpdateState == ETextureResourceUpdateState::Succeeded)
			ResourceState->PublishedTexture = ResourceState->PendingUpdate->GetPublishedTexture();
		else
			DURIN_WARN("Texture resource update failed; retaining any previous allocation. See preceding diagnostics. (texture: {})", GetObjectPath());
		RetireTextureResource(std::move(Candidate));
		ResourceState->PendingUpdate.reset();
		std::erase(UpdatingTextures, this);
		if (ResourceState->NextInput) StartResourceUpdate(std::move(ResourceState->NextInput));
		// Callbacks may destroy this asset or admit another update. Do not touch it afterward.
		ResourceChanged.Broadcast(*this, ETextureResourceChange::Completed);
	}

	auto DTexture::EnsurePlatformDataLoadedBlocking() -> bool
	{
		CheckGameThread();
		if (HasPlatformData()) return true;
		std::string Error;
		if (!GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			Error = std::format(
				"Texture '{}': platform data has not been built.", GetObjectPath());
		}
		else if (GetCookedPlatformData().GetMetadata().LogicalSize == 0)
		{
			Error = std::format(
				"Cooked texture '{}': required PlatformData field is missing.", GetObjectPath());
		}
		else if (LoadCookedPlatformData(Error))
		{
			return true;
		}
		DURIN_WARN("{}", Error);
		return false;
	}
}
