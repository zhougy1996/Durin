#include "Texture/Texture.h"

#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"
#include "Threading/TaskComposition.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Load.h"
#include "Asset/AssetCook.h"
#include "Logging/LogMacros.h"
#include "Asset/BulkData.h"

#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "Texture/TextureRenderResource.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		auto RetireTextureResource(std::unique_ptr<FTextureResource> Resource) -> void
		{
			if (!Resource) return;
			if (!Resource->IsInitialized()) return;
			Resource->BeginRelease_GameThread();
			BeginCleanupRenderResource(FDeferredRenderResourceCleanup(std::move(Resource)));
		}
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
		check(TextureReference == nullptr);
		check(RenderResource == nullptr);
	}

	auto DTexture::PostLoad() -> void
	{
		Source.BindOwner(this);
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (CookedPlatformData.GetMetadata().LogicalSize == 0)
			{
				DURIN_ERROR("PostLoad '{}': required cooked PlatformData field is missing.", GetObjectPath());
				return;
			}
			ResetPlatformData();
			return;
		}
		if (Source.GetSchemaVersion() != TextureSourceSchemaVersion)
		{
			FTextureSource Migrated = Source;
			if (!Migrated.MigrateLegacy())
			{
				DURIN_ERROR("PostLoad '{}': Texture source migration failed.", GetObjectPath());
				return;
			}
			SetSource(std::move(Migrated));
		}
		BuildPlatformDataForLoad();
	}

	auto DTexture::ContributeToCook(FCookContext& Context,
		std::string_view VirtualPackagePath, std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
		{
			OutError = std::format("Texture '{}' supports only the Win64 game cook target.", GetObjectPath());
			return false;
		}
		if (!HasPlatformData()) PostLoad();
		if (!HasPlatformData())
		{
			OutError = std::format("Failed to cook texture '{}': platform data is unavailable.", GetObjectPath());
			return false;
		}
		return Context.AddPackage(std::string(VirtualPackagePath), GetPackage(), &OutError);
	}

	auto DTexture::BeginDestroy() -> void
	{
		bAcceptingRenderResourceBuilds = false;
		ReleaseRenderResources();
		Super::BeginDestroy();
	}

	auto DTexture::ReleaseRenderResources() -> void
	{
		if (PendingUpdate)
		{
			PendingUpdate->Close();
			PendingUpdate->Wait();
			RetireTextureResource(PendingUpdate->TakeCandidate());
			PendingUpdate.reset();
		}
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

	auto DTexture::HasUsableResource() const -> bool { return RenderResource != nullptr; }
	auto DTexture::IsResourceUpdatePending() const -> bool { return PendingUpdate != nullptr; }
	auto DTexture::FinishReloadResourcePreparation() -> bool
	{
		CheckGameThread();
		if (!GDynamicRHI) return HasPlatformData();
		if (PendingUpdate)
		{
			if (GRenderingThread) FlushRenderingCommands();
			PumpGameThreadDeferredWork();
			ConsumeResourceUpdate();
		}
		return !PendingUpdate && LastUpdateState == ETextureResourceUpdateState::Succeeded
			&& HasUsableResource();
	}
	auto DTexture::GetPublishedTexture() const -> FTextureRHIRef
	{
		CheckGameThread();
		return RenderResource ? RenderResource->GetTextureRHI_GameThread() : FTextureRHIRef{};
	}

	auto DTexture::SetSource(FTextureSource Value) -> void
	{
		CheckGameThread();
		Source = std::move(Value);
		Source.BindOwner(this);
		InvalidateAuthoredBuild();
	}

	auto DTexture::InvalidateAuthoredBuild() -> void
	{
		CheckGameThread();
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
	}

	auto DTexture::SetAssetImportData(DAssetImportData& Value) -> void
	{
		CheckGameThread();
		check(Value.GetOuter() == this);
		AssetImportData = &Value;
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
		if (PendingUpdate) PendingUpdate->SetSuccessor(std::move(Candidate));
		else StartResourceUpdate(std::move(Candidate));
	}

	auto DTexture::StartResourceUpdate(std::unique_ptr<FTextureResource> Candidate) -> void
	{
		check(!PendingUpdate);
		PendingUpdate = std::make_shared<FTextureResourceUpdate>(std::move(Candidate));
		if (!GDynamicRHI || !IsTaskSchedulerRunning()
			|| !GetGameThreadDeferredWorkQueueDiagnostics().bAccepting)
		{
			DURIN_WARN("Texture update rejected: RHI or GameThread task executor is unavailable. (texture: {})", GetObjectPath());
			PendingUpdate->Reject();
			ConsumeResourceUpdate();
			return;
		}
		// Register both nodes before render admission; shutdown never needs a new completion submission.
		auto Completion = Tasks::TCompletionSource<void>::Create({.DebugName = "TextureInitialization"});
		auto Handoff = Tasks::Then(Completion.TakeTask(), Tasks::ETaskExecutor::GameThreadDeferred,
			{.DebugName = "TextureResourceHandoff"},
			[Owner = TWeakObjectPtr<DTexture>(this), Update = std::weak_ptr(PendingUpdate)] {
				if (auto* Texture = Owner.Get(); Texture && Texture->PendingUpdate == Update.lock())
					Texture->ConsumeResourceUpdate();
			});
		PendingUpdate->SetCompletionTask(Handoff.GetCompletion().GetTaskHandle());
		const bool bInitializeReference = !bTextureReferenceInitializationQueued;
		// One admission owns both initialization steps, so rejection leaves no half-admitted reference.
		const bool bAccepted = TryEnqueueRenderCommand("TextureResourceUpdate",
			[Update = PendingUpdate, Reference = TextureReference.get(), bInitializeReference, Completion]
			(FRHICommandListImmediate& Commands) {
				Update->Execute_RenderThread(Commands, *Reference, bInitializeReference);
				Completion.TrySetValue();
			});
		if (bAccepted) bTextureReferenceInitializationQueued = true;
		else
		{
			DURIN_WARN("Texture update rejected: render command admission is closed. (texture: {})", GetObjectPath());
			PendingUpdate->Reject();
			Completion.TrySetValue();
		}
	}

	auto DTexture::ConsumeResourceUpdate() -> void
	{
		if (!PendingUpdate || !PendingUpdate->IsComplete()) return;
		LastUpdateState = PendingUpdate->GetState();
		auto Candidate = PendingUpdate->TakeCandidate();
		if (LastUpdateState == ETextureResourceUpdateState::Succeeded)
		{
			RetireTextureResource(std::move(RenderResource));
			RenderResource = std::move(Candidate);
		}
		else
			DURIN_WARN("Texture resource update failed; retaining any previous allocation. See preceding diagnostics. (texture: {})", GetObjectPath());
		RetireTextureResource(std::move(Candidate));
		auto Successor = PendingUpdate->TakeSuccessor();
		PendingUpdate.reset();
		if (Successor) StartResourceUpdate(std::move(Successor));
	}

	auto DTexture::EnsurePlatformDataLoadedBlocking() -> bool
	{
		CheckGameThread();
		if (HasPlatformData()) return true;
		if (!GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			DURIN_WARN(
				"Texture '{}': platform data has not been built.", GetObjectPath());
			return false;
		}
		if (GetCookedPlatformData().GetMetadata().LogicalSize == 0)
		{
			DURIN_WARN(
				"Cooked texture '{}': required PlatformData field is missing.", GetObjectPath());
			return false;
		}
		return LoadCookedPlatformData();
	}
}
