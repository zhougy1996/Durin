#include "Texture/Texture.h"

#include "DObject/Package.h"

#include "Asset/Load.h"
#include "Asset/BulkData.h"

#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "Texture/TextureRenderResource.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	DTexture::DTexture(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, TextureReference(std::make_unique<FTextureReference>())
		, RenderCompletion(std::make_shared<FTextureResourceCompletion>())
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
		Super::BeginDestroy();
	}

	auto DTexture::ReleaseRenderResources() -> void
	{
		RenderCompletion->BeginRequest(++BuildRevision);
		if (RenderResource)
		{
			RenderResource->PrepareForRelease(BuildRevision);
			RenderResource->BeginRelease_GameThread();
			BeginCleanupRenderResource(
				FDeferredRenderResourceCleanup(std::move(RenderResource)));
		}
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

	auto DTexture::GetRenderResourceState() const
		-> ERenderResourceState
	{
		return RenderCompletion->GetResourceState();
	}

	auto DTexture::GetRenderFailure() const -> ETextureRenderFailure
	{
		return RenderCompletion->GetFailedRevision() == BuildRevision
			? RenderCompletion->GetFailureReason()
			: ETextureRenderFailure::None;
	}

	auto DTexture::GetAppliedRenderRevision() const -> uint64
	{
		return RenderCompletion->GetAppliedRevision();
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
		++AuthoredGeneration;
		OutError.clear();
		return true;
	}

	auto DTexture::ResetSource() -> void
	{
		CheckGameThread();
		Source.Reset();
		Source.BindOwner(this);
		++AuthoredGeneration;
	}

	auto DTexture::BindTextureSourceOwner() -> void
	{
		Source.BindOwner(this);
	}

	auto DTexture::AdvanceAuthoredGeneration() -> void
	{
		CheckGameThread();
		++AuthoredGeneration;
	}

	auto DTexture::CreateSourceSnapshotBlocking() const
		-> FTextureSourceSnapshot
	{
		return Source.CreateSnapshotBlocking(AuthoredGeneration);
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
		const uint64 Revision = ++BuildRevision;
#if DURIN_BUILD_DEBUG
		const FName DebugOwner = GetPackage()
			? FName(GetPackage()->GetPackagePath())
			: FName("<transient DTexture>");
#endif
		RenderCompletion->BeginRequest(Revision);
		if (GDynamicRHI == nullptr) return;
		if (!bTextureReferenceInitializationQueued)
		{
#if DURIN_BUILD_DEBUG
			TextureReference->SetDebugOwner(DebugOwner);
#endif
			TextureReference->BeginInit_GameThread();
			bTextureReferenceInitializationQueued = true;
		}

		std::unique_ptr<FTextureAssetResource> Candidate =
			CreateRenderResourceCandidate(
				TextureReference.get(), Revision, RenderCompletion);
		check(Candidate != nullptr);
#if DURIN_BUILD_DEBUG
		Candidate->SetDebugOwner(DebugOwner);
#endif
		FTextureAssetResource* CandidateView = Candidate.get();
		std::unique_ptr<FTextureAssetResource> Previous =
			std::move(RenderResource);
		RenderResource = std::move(Candidate);
		CandidateView->BeginInit_GameThread();
		if (Previous)
		{
			Previous->BeginRelease_GameThread();
			BeginCleanupRenderResource(
				FDeferredRenderResourceCleanup(std::move(Previous)));
		}
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
