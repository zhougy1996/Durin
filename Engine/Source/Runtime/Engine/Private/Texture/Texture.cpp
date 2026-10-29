#include "Texture/Texture.h"
#include "DObject/PropertyChange.h"

#include "DObject/Package.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Load.h"
#include "Asset/Asset.h"
#include "Asset/AssetCook.h"
#include "Logging/LogMacros.h"
#include "Asset/BulkData.h"

#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RenderResource.h"
#include "Texture/TexturePlatformCache.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"

namespace Durin
{
	auto DTexture::BeginCacheForCookedPlatformData(FObjectCookContext& Context) -> void
	{
		Super::BeginCacheForCookedPlatformData(Context);
		auto* Preparation = FCookPreparationContext::Get(*this, Context);
		if (!Preparation) { Context.Result = {EObjectCookError::InvalidInput, "Missing texture Cook preparation context."}; return; }
		if (Context.CheckCancellation()) return;
		BeginCachePlatformData();
		Context.WaitForCompletion = [this, Preparation, &Context] {
			if (!FinishCachePlatformData() || !HasPlatformData())
				Preparation->Failure = {.Status = ECookContributionStatus::Error, .Error = ECookContributionError::PlatformData,
					.ObjectPath = GetObjectPath(), .VirtualPath = Context.Package,
					.TargetPlatform = Preparation->GetTargetPlatform(), .TargetProfile = Preparation->GetTargetProfile()};
			(void)Context.CheckCancellation();
		};
	}
	auto DTexture::IsCachedCookedPlatformDataLoaded(const FObjectCookContext&) const -> bool { return IsAsyncCacheComplete(); }
	auto DTexture::ClearCachedCookedPlatformData(const FObjectCookContext& Context) -> void
	{
		if (!IsAsyncCacheComplete()) BlockOnAnyAsyncBuild();
		Super::ClearCachedCookedPlatformData(Context);
	}
	namespace
	{
		auto RetireTextureResource(std::unique_ptr<FTextureResource> Resource) -> void
		{
			if (!Resource) return;
			EnqueueRenderCommand("ReleaseTextureResource",
				[Resource = Resource.get()](FRHICommandListImmediate&) {
					if (Resource->IsInitialized()) Resource->ReleaseResource();
				});
			BeginCleanupRenderResource(FDeferredRenderResourceCleanup(std::move(Resource)));
		}
	}

	DTexture::DTexture(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, TextureReference(std::make_unique<FTextureReference>())
	{
#if DURIN_WITH_EDITORONLY_DATA
		Source.BindOwner(this);
#endif
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
#if DURIN_WITH_EDITORONLY_DATA
		return Source.IsValid() ? std::expected<void, FObjectValidationError>{}
			: RejectLoadedObjectGraph(GetObjectPath(), "Invalid or unsupported texture source.");
#else
		return RejectLoadedObjectGraph(GetObjectPath(),
			"Authored texture packages require editor-only data support.");
#endif
	}

	auto DTexture::PostLoad() -> void
	{
#if DURIN_WITH_EDITORONLY_DATA
		Source.BindOwner(this);
#endif
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (CookedPlatformData.GetMetadata().LogicalSize == 0)
			{
				DURIN_ERROR("PostLoad '{}': required cooked PlatformData field is missing.", GetObjectPath());
				return;
			}
			ResetPlatformData();
			BeginCachePlatformData();
			return;
		}
#if DURIN_WITH_EDITORONLY_DATA
		if (!Source.IsValid())
		{
			DURIN_ERROR("PostLoad '{}': invalid or unsupported texture source.", GetObjectPath());
			return;
		}
		BeginCachePlatformData();
#else
		DURIN_ERROR("PostLoad '{}': authored texture packages require editor-only data support.", GetObjectPath());
#endif
	}

	auto DTexture::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
#if DURIN_WITH_EDITORONLY_DATA
		// Named-property edits have their own validation/application path. Its
		// completion must not reopen compilation after applying the product.
		if (Event.MemberProperty || Event.Phase != EPropertyChangePhase::Committed || IsTemplateObject()
			|| GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
		BlockOnAnyAsyncBuild();
		InvalidateAuthoredBuild();
		BeginCachePlatformData();
#endif
	}

	auto DTexture::BeginCachePlatformData() -> void
	{
		CachePlatformData(true);
	}

	auto DTexture::CachePlatformData(bool bAsyncCache) -> void
	{
		CheckGameThread();
		if (!bAcceptingRenderResourceBuilds || IsPendingKill()) return;
		if (!IsAsyncCacheComplete())
		{
			if (!bAsyncCache) (void)FinishCachePlatformData();
			return;
		}
		if (HasPlatformData()) return;
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (!bAsyncCache || IsTaskSchedulerRunning())
			{
				ResourceUpdateError.clear();
				LastUpdateState = ETextureResourceUpdateState::Pending;
				LoadCookedPlatformData(bAsyncCache);
			}
			return;
		}
		BuildPlatformDataForLoad(bAsyncCache);
	}

	auto DTexture::IsAsyncCacheComplete() const -> bool
	{
		CheckGameThread();
		if (CookedLoad) return false;
		return !HasPendingTextureCompilation(*this);
	}

	auto DTexture::FinishCachePlatformData(double TimeoutSeconds) -> bool
	{
		CheckGameThread();
		if (!std::isfinite(TimeoutSeconds) || TimeoutSeconds < 0.0) return false;
		if (!GetAssetRuntimeConfiguration().RequiresCookedPayload())
			return FinishTextureCompilation(*this, TimeoutSeconds);
		if (HasPlatformData()) return true;
		if (!bAcceptingRenderResourceBuilds || IsPendingKill()) return false;
		// Zero only observes installed data, without starting work or applying results.
		if (TimeoutSeconds == 0.0) return false;
		if (const auto Request = CookedLoad)
		{
			(void)Request->Wait(TimeoutSeconds);
			if (!Request->IsReady()) return false;
			// The family's setter retires the request and publication action.
			const auto Publish = CookedPublication;
			if (Publish) Publish();
		}
		return !IsPendingKill() && HasPlatformData();
	}

	auto DTexture::BlockOnAnyAsyncBuild() -> void
	{
		CheckGameThread();
#if DURIN_WITH_EDITORONLY_DATA
		PrepareTextureForMutation(*this);
#endif
	}

	auto DTexture::BeginDestroy() -> void
	{
		if (CookedLoad) { CookedLoad->Cancel(); CookedLoad.reset(); }
		CookedPublication = {};
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
		bAcceptingRenderResourceBuilds = false;
		ReleaseRenderResources();
		Super::BeginDestroy();
	}

	auto DTexture::ReleaseRenderResources() -> void
	{
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
		CheckGameThread();
		if (LastUpdateState == ETextureResourceUpdateState::Closed
			|| LastUpdateState == ETextureResourceUpdateState::Failed || !RenderResource) return LastUpdateState;
		if (CookedLoad || !ResourceFence.IsFenceComplete()) return ETextureResourceUpdateState::Pending;
		return HasUsableResource() ? ETextureResourceUpdateState::Succeeded : ETextureResourceUpdateState::Failed;
	}

	auto DTexture::GetResourceUpdateError() const -> const std::string&
	{
		CheckGameThread();
		if (ResourceUpdateError.empty() && RenderResource && ResourceFence.IsFenceComplete() && !HasUsableResource())
		{
			static const std::string UploadFailure = "Texture upload failed; see render initialization diagnostics.";
			return UploadFailure;
		}
		return ResourceUpdateError;
	}

	auto DTexture::HasUsableResource() const -> bool
	{
		CheckGameThread();
		return RenderResource && ResourceFence.IsFenceComplete() && RenderResource->IsInitialized()
			&& RenderResource->GetTextureRHI_GameThread();
	}

	auto DTexture::IsResourceUpdatePending() const -> bool
	{
		CheckGameThread();
		return CookedLoad != nullptr || (RenderResource && !ResourceFence.IsFenceComplete());
	}

	auto DTexture::FinishReloadResourcePreparation() -> bool
	{
		CheckGameThread();
		CachePlatformData(false);
		if (!FinishCachePlatformData()) return false;
		if (!GDynamicRHI) return HasPlatformData();
		ResourceFence.Wait();
		return GetResourceUpdateState() == ETextureResourceUpdateState::Succeeded && HasUsableResource();
	}

	auto DTexture::GetPublishedTexture() const -> FTextureRHIRef
	{
		CheckGameThread();
		return HasUsableResource() ? RenderResource->GetTextureRHI_GameThread() : FTextureRHIRef{};
	}

#if DURIN_WITH_EDITORONLY_DATA
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
		BlockOnAnyAsyncBuild();
		AdoptBuildSource(std::move(Value));
		InvalidateAuthoredBuild();
	}

	auto DTexture::AdoptBuildSource(FTextureSource Value) -> void
	{
		CheckGameThread();
		Source = std::move(Value);
		Source.BindOwner(this);
	}

	auto DTexture::InvalidateAuthoredBuild() -> void
	{
		CheckGameThread();
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
		ResetPlatformData();
		CookedPlatformData = {};
		InvalidateRenderResource();
	}
#endif

	auto DTexture::InvalidateRenderResource() -> void
	{
		CheckGameThread();
		if (CookedLoad) { CookedLoad->Cancel(); CookedLoad.reset(); }
		CookedPublication = {};
		ReleaseResource();
	}

	auto DTexture::ReleaseResource() -> void
	{
		CheckGameThread();
		if (!bAcceptingRenderResourceBuilds) return;
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

#if DURIN_WITH_EDITORONLY_DATA
	auto DTexture::SetAssetImportData(DAssetImportData& Value) -> void
	{
		CheckGameThread();
		check(Value.GetOuter() == this);
		AssetImportData = &Value;
	}
#endif

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
		ReleaseResource();
		if (!HasPlatformData())
		{
			LastUpdateState = ETextureResourceUpdateState::Failed;
			ResourceUpdateError = "Texture platform data is unavailable or invalid.";
			DURIN_WARN(
				"Texture render-resource build rejected without valid platform data. (texture: {})",
				GetObjectPath());
			return;
		}
		if (!GDynamicRHI || GetRenderCommandAdmissionState() != ERenderCommandAdmissionState::Running)
		{
			LastUpdateState = ETextureResourceUpdateState::Failed;
			ResourceUpdateError = "Texture upload requires an RHI and open render-command admission.";
			DURIN_WARN("Texture resource update rejected: {} (texture: {})", ResourceUpdateError, GetObjectPath());
			return;
		}
		RenderResource = CreateRenderResourceCandidate(TextureReference.get());
		check(RenderResource != nullptr);
#if DURIN_BUILD_DEBUG
		RenderResource->SetDebugOwner(GetPackage()
			? FName(GetPackage()->GetPackagePath()) : FName("<transient DTexture>"));
#endif
		const bool bInitializeReference = !bTextureReferenceInitializationQueued;
		EnqueueRenderCommand("InitTextureResource",
			[Resource = RenderResource.get(), Reference = TextureReference.get(), bInitializeReference,
				TextureName = GetObjectPath()]
			(FRHICommandListImmediate& Commands) {
				try
				{
					if (bInitializeReference) Reference->InitResource(Commands);
					Reference->ResetToFallback_RenderThread();
					Resource->InitResource(Commands);
					if (Resource->GetTextureRHI_RenderThread()) Resource->PublishTexture_RenderThread();
					else DURIN_WARN("Texture '{}' upload failed; using fallback.", TextureName);
				}
				catch (...)
				{
					if (Resource->IsInitialized()) Resource->ReleaseResource();
					Reference->ResetToFallback_RenderThread();
					DURIN_WARN("Texture '{}' initialization threw an exception; using fallback.", TextureName);
				}
			});
		bTextureReferenceInitializationQueued = true;
		ResourceFence.BeginFence();
	}
}
