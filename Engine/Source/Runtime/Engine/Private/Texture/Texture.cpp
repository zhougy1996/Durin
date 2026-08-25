#include "Texture/Texture.h"

#include "DObject/Package.h"

#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "Texture/TextureRenderResource.h"

namespace Durin
{
	DTexture::DTexture(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, TextureReference(std::make_unique<FTextureReference>())
		, RenderCompletion(std::make_shared<FTextureResourceCompletion>())
	{
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

	auto DTexture::GetAppliedRenderRevision() const -> uint64
	{
		return RenderCompletion->GetAppliedRevision();
	}

	auto DTexture::QueueRenderResourceBuild() -> void
	{
		if (!bAcceptingRenderResourceBuilds || IsPendingKill())
		{
			DURIN_WARN(
				"Texture render-resource build rejected after object teardown began. (texture: {})",
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

	auto DTexture::InvalidateRenderResource() -> void
	{
		RenderCompletion->BeginRequest(++BuildRevision);
		if (RenderResource)
		{
			RenderResource->PrepareForRelease(BuildRevision);
			RenderResource->BeginRelease_GameThread();
			BeginCleanupRenderResource(
				FDeferredRenderResourceCleanup(std::move(RenderResource)));
		}
		else
		{
			RenderCompletion->MarkReleased(BuildRevision);
		}
	}
}
