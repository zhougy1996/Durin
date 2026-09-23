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
#include "Texture/TexturePlatformCache.h"
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

	auto DTexture::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError>
	{
		if (auto Result = Super::ValidateLoadedObjectGraph(Context); !Result) return Result;
		if (Context.bCooked)
			return CookedPlatformData.GetMetadata().LogicalSize != 0 ? std::expected<void, FObjectValidationError>{}
				: RejectLoadedObjectGraph(GetObjectPath(), "Required cooked PlatformData field is missing.");
		return Source.IsValid() ? std::expected<void, FObjectValidationError>{}
			: RejectLoadedObjectGraph(GetObjectPath(), "Invalid or unsupported texture source.");
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
		if (!Source.IsValid())
		{
			DURIN_ERROR("PostLoad '{}': invalid or unsupported texture source.", GetObjectPath());
			return;
		}
		BeginCachePlatformData();
	}

	auto DTexture::BeginCachePlatformData() -> void
	{
		CheckGameThread();
		if (!bAcceptingRenderResourceBuilds || IsPendingKill() || HasPlatformData()
			|| (bCacheRequestCurrent && !IsAsyncCacheComplete()) || GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
		bCacheRequestCurrent = true;
		BuildPlatformDataForLoad();
	}

	auto DTexture::IsAsyncCacheComplete() const -> bool
	{
		CheckGameThread();
		return !HasPendingTextureCompilation(*this);
	}

	auto DTexture::FinishCachePlatformData() -> bool
	{
		CheckGameThread();
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return EnsurePlatformDataLoadedBlocking();
		return FinishTextureCompilation(*this);
	}

	auto DTexture::ContributeToCook(FCookContext& Context,
		std::string_view VirtualPackagePath) -> FCookContributionResult
	{
		auto Reject = [&](ECookContributionError Error) -> FCookContributionResult {
			return {.Error = Error, .ObjectPath = GetObjectPath(), .VirtualPath = std::string(VirtualPackagePath),
				.TargetPlatform = Context.GetTargetPlatform(), .TargetProfile = Context.GetTargetProfile()};
		};
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game) return Reject(ECookContributionError::Target);
		if (!HasPlatformData()) PostLoad();
		if (!FinishCachePlatformData()) return Reject(ECookContributionError::PlatformData);
		if (!HasPlatformData()) return Reject(ECookContributionError::PlatformData);
		const auto Added = Context.AddPackage(std::string(VirtualPackagePath), GetPackage());
		if (!Added)
		{
			auto Result = Reject(ECookContributionError::Plan);
			Result.PlanCause = Added.Error;
			return Result;
		}
		return {};
	}

	auto DTexture::BeginDestroy() -> void
	{
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
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
		return PendingUpdate && !PendingUpdate->IsDiscarded() ? PendingUpdate->GetState() : LastUpdateState;
	}

	auto DTexture::HasUsableResource() const -> bool { return RenderResource != nullptr; }
	auto DTexture::IsResourceUpdatePending() const -> bool { return PendingUpdate != nullptr; }
	auto DTexture::FinishReloadResourcePreparation() -> bool
	{
		CheckGameThread();
		if (!FinishCachePlatformData()) return false;
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

	auto DTexture::ReplaceSourceStorage(FTextureSource Value) -> bool
	{
		CheckGameThread();
		if (!Value.IsValid() || Value.GetIdentity() != Source.GetIdentity()
			|| Value.GetBulkData().GetInstanceId() != Source.GetBulkData().GetInstanceId()) return false;
		const auto Before = Source.GetMipData();
		const auto After = Value.GetMipData();
		if (!Before.IsValid() || !After.IsValid()
			|| !std::ranges::equal(Before.GetData().GetBytes(), After.GetData().GetBytes())) return false;
		Source = std::move(Value);
		Source.BindOwner(this);
		return true;
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
		bCacheRequestCurrent = false;
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
		ResetPlatformData();
		CookedPlatformData = {};
		InvalidateRenderResource();
	}

	auto DTexture::InvalidateRenderResource() -> void
	{
		CheckGameThread();
		if (PendingUpdate) PendingUpdate->Discard();
		if (bTextureReferenceInitializationQueued)
		{
			// FIFO ordering clears an already-published candidate before any successor upload.
			EnqueueRenderCommand("InvalidateTextureReference",
				[Reference = TextureReference.get()](FRHICommandListImmediate&) {
					Reference->ResetToFallback_RenderThread();
				});
		}
		RetireTextureResource(std::move(RenderResource));
		ResourceUpdateError.clear();
		LastUpdateState = ETextureResourceUpdateState::Idle;
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
		InvalidateRenderResource();
		if (!HasPlatformData())
		{
			LastUpdateState = ETextureResourceUpdateState::Failed;
			ResourceUpdateError = "Texture platform data is unavailable or invalid.";
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
		if (PendingUpdate)
		{
			PendingUpdate->SetSuccessor(std::move(Candidate));
			LastUpdateState = ETextureResourceUpdateState::Pending;
		}
		else StartResourceUpdate(std::move(Candidate));
	}

	auto DTexture::StartResourceUpdate(std::unique_ptr<FTextureResource> Candidate) -> void
	{
		check(!PendingUpdate);
		PendingUpdate = std::make_shared<FTextureResourceUpdate>(std::move(Candidate));
		if (!GDynamicRHI || !IsTaskSchedulerRunning()
			|| GetRenderCommandAdmissionState() != ERenderCommandAdmissionState::Running
			|| !GetGameThreadDeferredWorkQueueDiagnostics().bAccepting)
		{
			DURIN_WARN("Texture update rejected: RHI, render admission, or GameThread task executor is unavailable. (texture: {})", GetObjectPath());
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
		// One command owns both initialization steps.
		EnqueueRenderCommand("TextureResourceUpdate",
			[Update = PendingUpdate, Reference = TextureReference.get(), bInitializeReference, Completion]
			(FRHICommandListImmediate& Commands) {
				Update->Execute_RenderThread(Commands, *Reference, bInitializeReference);
				Completion.TrySetValue();
			});
		bTextureReferenceInitializationQueued = true;
	}

	auto DTexture::ConsumeResourceUpdate() -> void
	{
		if (!PendingUpdate || !PendingUpdate->IsComplete()) return;
		const auto CompletedState = PendingUpdate->GetState();
		// A discarded completion must not overwrite a newer direct failure or idle state.
		if (CompletedState != ETextureResourceUpdateState::Closed) LastUpdateState = CompletedState;
		auto Candidate = PendingUpdate->TakeCandidate();
		if (CompletedState == ETextureResourceUpdateState::Succeeded)
		{
			RetireTextureResource(std::move(RenderResource));
			RenderResource = std::move(Candidate);
		}
		else if (CompletedState == ETextureResourceUpdateState::Failed)
		{
			ResourceUpdateError = "Texture upload failed; see render initialization diagnostics.";
			DURIN_WARN("Texture resource update failed; using fallback. (texture: {})", GetObjectPath());
		}
		RetireTextureResource(std::move(Candidate));
		auto Successor = PendingUpdate->TakeSuccessor();
		PendingUpdate.reset();
		if (Successor)
		{
			ResourceUpdateError.clear();
			StartResourceUpdate(std::move(Successor));
		}
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
