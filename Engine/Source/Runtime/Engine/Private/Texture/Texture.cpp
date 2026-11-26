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
#if DURIN_WITH_EDITORONLY_DATA
#include "TextureCompilingManager.h"
#endif
#include "Threading/RunnableThread.h"
#include "Threading/TaskRuntime.h"

namespace Durin
{
	namespace
	{
		auto FinishTextureBuildBeforeEdit(DTexture& Texture) -> void
		{
#if DURIN_WITH_EDITORONLY_DATA
			if (AssetPrivate::HasPendingTextureSettingsBuild(Texture))
				FAssetCompilingManager::Get().MarkCompilationAsCanceled(Texture);
#endif
			Texture.BlockOnAnyAsyncBuild();
		}
	}

	auto DTexture::BeginCacheForCookedPlatformData(const ITargetPlatform* TargetPlatform) -> void
	{
		Super::BeginCacheForCookedPlatformData(TargetPlatform);
		FCookedPlatformData::Begin(*this, TargetPlatform, [this](FCookedPlatformData& Context) {
			if (Context.CheckCancellation()) return;
			if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
			{
				if (GetAssetRuntimeConfiguration().GetCookTargetPlatform() == Context.GetTargetPlatform() && CookedPlatformData.HasData())
					Context.Primary = CookedPlatformData;
				else Context.Result = {EObjectCookError::InvalidInput, "Cooked texture is missing or targets another platform."};
				return;
			}
#if DURIN_WITH_EDITORONLY_DATA
			Context.ValidatePreparedInputs = [this, Revision = AuthoredBuildRevision] {
				return Revision == AuthoredBuildRevision;
			};
			PrepareCookedPlatformData(Context);
#else
			Context.Result = {EObjectCookError::InvalidInput, "Authored texture Cook requires editor data."};
#endif
		});
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
		check(HasAnyInternalFlags(EObjectInternalFlags::BeginDestroyed));
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
		if (IsPendingKill()) return;
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

	auto DTexture::Modify(bool bAlwaysMarkDirty) -> bool
	{
		FinishTextureBuildBeforeEdit(*this);
		return !IsPendingKill() && Super::Modify(bAlwaysMarkDirty);
	}

	auto DTexture::PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError>
	{
		FinishTextureBuildBeforeEdit(*this);
		if (IsPendingKill()) return std::unexpected(FObjectValidationError{.Code = EObjectValidationError::ModuleRejected,
			.ObjectPath = GetObjectPath(), .Message = "Texture owner is being destroyed."});
		return Super::PreEditChangeProperty(Proposal);
	}

	auto DTexture::PreEditUndo() -> void
	{
		FinishTextureBuildBeforeEdit(*this);
		Super::PreEditUndo();
	}

	auto DTexture::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
#if DURIN_WITH_EDITORONLY_DATA
		// Named-property edits have their own validation/application path. Its
		// completion must not reopen compilation after applying the product.
		if (Event.MemberProperty || (Event.ChangeType & EPropertyChangeType::Interactive) || IsTemplateObject()
			|| GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
		BlockOnAnyAsyncBuild();
		if (IsPendingKill()) return;
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
		if (IsPendingKill()) return;
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
		if (IsPendingKill()) return false;
		// A query must not consume an already-computed authored result either.
		if (TimeoutSeconds == 0.0) return HasPlatformData();
		if (!GetAssetRuntimeConfiguration().RequiresCookedPayload())
			return FinishTextureCompilation(*this, TimeoutSeconds);
		if (HasPlatformData()) return true;
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
		while (HasPendingTextureCompilation(*this))
			FAssetCompilingManager::Get().FinishCompilationForObject(*this);
	}

	auto DTexture::BeginDestroy() -> void
	{
		SetInternalFlags(EObjectInternalFlags::BeginDestroyed);
		if (CookedLoad) { CookedLoad->Cancel(); CookedLoad.reset(); }
		CookedPublication = {};
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
		ReleaseRenderResources();
		Super::BeginDestroy();
	}

	auto DTexture::ReleaseRenderResources() -> void
	{
		RetireTextureResource(std::move(RenderResource));
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

	auto DTexture::GetResource() -> FTextureResource*
	{
		CheckGameThread();
		return RenderResource.get();
	}

	auto DTexture::GetResource() const -> const FTextureResource*
	{
		CheckGameThread();
		return RenderResource.get();
	}

	auto DTexture::HasPendingRenderResourceInitialization() const -> bool
	{
		CheckGameThread();
		return RenderResource && !ResourceFence.IsFenceComplete();
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
		if (IsPendingKill()) return;
		AdoptBuildSource(std::move(Value));
		InvalidateAuthoredBuild();
	}

	auto DTexture::AdoptBuildSource(FTextureSource Value) -> void
	{
		CheckGameThread();
		Source = std::move(Value);
		Source.BindOwner(this);
	}

	auto DTexture::InvalidateAuthoredBuild(bool bKeepPreview) -> void
	{
		CheckGameThread();
		++AuthoredBuildRevision;
		CookedPlatformData = {};
		if (!bKeepPreview)
		{
			ResetPlatformData();
			InvalidateRenderResource();
		}
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
		if (IsPendingKill()) return;
		if (bTextureReferenceInitializationQueued)
		{
			// FIFO ordering clears the published allocation before the next upload.
			EnqueueRenderCommand("InvalidateTextureReference",
				[Reference = TextureReference.get()](FRHICommandListImmediate&) {
					Reference->ResetToFallback_RenderThread();
				});
		}
		RetireTextureResource(std::move(RenderResource));
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
		if (IsPendingKill())
		{
			DURIN_WARN(
				"Texture render-resource build rejected after object teardown began. (texture: {})",
				GetObjectPath());
			return;
		}
		ReleaseResource();
		if (!HasPlatformData())
		{
			DURIN_WARN(
				"Texture render-resource build rejected without valid platform data. (texture: {})",
				GetObjectPath());
			return;
		}
		if (!GDynamicRHI || !IsRenderingThreadRunning())
		{
			DURIN_WARN("Texture upload requires an RHI and open render-command admission. (texture: {})", GetObjectPath());
			return;
		}
		RenderResource = CreateResource(TextureReference.get());
		check(RenderResource != nullptr);
#if DURIN_BUILD_DEBUG
		RenderResource->SetDebugOwner(GetPackage()
			? FName(GetPackage()->GetPackagePath()) : FName("<transient DTexture>"));
#endif
		const bool bInitializeReference = !bTextureReferenceInitializationQueued;
		EnqueueRenderCommand("InitTextureResource", [Resource = RenderResource.get(), Reference = TextureReference.get(), bInitializeReference](FRHICommandListImmediate& Commands) {
			if (bInitializeReference) Reference->InitResource(Commands);
			Reference->ResetToFallback_RenderThread();
			Resource->InitResource(Commands);
			Resource->PublishTexture_RenderThread();
		});
		bTextureReferenceInitializationQueued = true;
		ResourceFence.BeginFence();
	}
}
