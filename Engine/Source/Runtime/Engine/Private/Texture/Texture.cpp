#include "Texture/Texture.h"

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
		const uint64 Revision = ++BuildRevision;
		const std::string OwnerDiagnostic = GetPackage()
			? GetPackage()->GetPackagePath()
			: "<transient DTexture>";
		RenderCompletion->BeginRequest(Revision);
		if (GDynamicRHI == nullptr) return;
		if (!bTextureReferenceInitializationQueued)
		{
			TextureReference->SetLifetimeDiagnostic(OwnerDiagnostic);
			TextureReference->BeginInit_GameThread();
			bTextureReferenceInitializationQueued = true;
		}

		std::unique_ptr<FTextureAssetResource> Candidate =
			CreateRenderResourceCandidate(
				TextureReference.get(), Revision, RenderCompletion);
		check(Candidate != nullptr);
		Candidate->SetLifetimeDiagnostic(OwnerDiagnostic, Revision);
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
