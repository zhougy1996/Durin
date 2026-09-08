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
	namespace
	{
		std::vector<DTexture*> UpdatingTextures;
		FTextureResourceChangedEvent ResourceChanged;
		bool bPumpingTextureResources = false;

		auto RetireTextureResource(std::unique_ptr<FTextureAssetResource> Resource) -> void
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
		, TextureReference(std::make_unique<FTextureReference>())
	{
		Source.BindOwner(this);
	}

	DTexture::~DTexture()
	{
		check(!bAcceptingRenderResourceBuilds);
		check(RenderResource == nullptr);
		check(TextureReference == nullptr);
	}

	auto DTexture::BeginDestroy() -> void
	{
		bAcceptingRenderResourceBuilds = false;
		ReleaseRenderResources();
		ResourceChanged.Broadcast(*this, ETextureResourceChange::Closed);
		Super::BeginDestroy();
	}

	auto DTexture::ReleaseRenderResources() -> void
	{
		std::erase(UpdatingTextures, this);
		NextInput.reset();
		if (PendingUpdate)
		{
			PendingUpdate->Close();
			PendingUpdate->Wait();
			RetireTextureResource(PendingUpdate->TakeCandidate());
			PendingUpdate.reset();
		}
		ResourceSnapshot.reset();
		RetireTextureResource(std::move(RenderResource));
		LastUpdateState = ETextureResourceUpdateState::Closed;
		if (bTextureReferenceInitializationQueued)
		{
			TextureReference->BeginRelease_GameThread();
			BeginCleanupRenderResource(
				FDeferredRenderResourceCleanup(std::move(TextureReference)));
		}
		else
		{
			TextureReference.reset();
		}
		bTextureReferenceInitializationQueued = false;
	}

	auto DTexture::GetTextureReferenceRHI() const
		-> FRHITextureReferenceRef
	{
		return TextureReference
			? TextureReference->GetTextureReferenceRHI()
			: FRHITextureReferenceRef{};
	}

	auto DTexture::GetResourceUpdateState() const -> ETextureResourceUpdateState
	{
		return PendingUpdate ? PendingUpdate->GetState() : LastUpdateState;
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
		if (!bAcceptingRenderResourceBuilds || IsPendingKill())
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
		auto Candidate = CreateRenderResourceCandidate(TextureReference.get());
		check(Candidate != nullptr);
#if DURIN_BUILD_DEBUG
		Candidate->SetDebugOwner(GetPackage()
			? FName(GetPackage()->GetPackagePath()) : FName("<transient DTexture>"));
#endif
		if (PendingUpdate) NextInput = std::move(Candidate);
		else StartResourceUpdate(std::move(Candidate));
		ResourceChanged.Broadcast(*this, ETextureResourceChange::Input);
	}

	auto DTexture::StartResourceUpdate(std::unique_ptr<FTextureAssetResource> Candidate) -> void
	{
		check(!PendingUpdate);
		PendingUpdate = std::make_shared<FTextureResourceUpdate>(std::move(Candidate));
		if (std::ranges::find(UpdatingTextures, this) == UpdatingTextures.end())
			UpdatingTextures.push_back(this);
		if (!GDynamicRHI)
		{
			PendingUpdate->Reject(ETextureRenderFailure::UnavailableRHI);
			return;
		}
		const bool bInitializeReference = !bTextureReferenceInitializationQueued;
		// One admission owns both initialization steps, so rejection leaves no half-admitted reference.
		const bool bAccepted = TryEnqueueRenderCommand("TextureResourceUpdate",
			[Update = PendingUpdate, Reference = TextureReference.get(), bInitializeReference]
			(FRHICommandListImmediate& Commands) {
				Update->Execute_RenderThread(Commands, *Reference, bInitializeReference);
			});
		if (bAccepted) bTextureReferenceInitializationQueued = true;
		else PendingUpdate->Reject(ETextureRenderFailure::AdmissionRejected);
	}

	auto DTexture::ConsumeResourceUpdate() -> void
	{
		if (!PendingUpdate || !PendingUpdate->IsComplete()) return;
		LastFailure = PendingUpdate->GetFailure();
		LastUpdateState = PendingUpdate->GetState();
		auto Candidate = PendingUpdate->TakeCandidate();
		if (LastUpdateState == ETextureResourceUpdateState::Succeeded)
		{
			ResourceSnapshot = PendingUpdate->GetSnapshot();
			RetireTextureResource(std::move(RenderResource));
			RenderResource = std::move(Candidate);
		}
		else RetireTextureResource(std::move(Candidate));
		PendingUpdate.reset();
		std::erase(UpdatingTextures, this);
		if (NextInput) StartResourceUpdate(std::move(NextInput));
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
