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
	namespace
	{
		auto GetTextureSourceBytesPerTexel(ETextureSourceFormat Format) -> uint32
		{
			switch (Format)
			{
			case ETextureSourceFormat::R8_UNORM: return 1;
			case ETextureSourceFormat::RG8_UNORM:
			case ETextureSourceFormat::R16_FLOAT: return 2;
			case ETextureSourceFormat::RGBA8: return 4;
			case ETextureSourceFormat::RGBA16_FLOAT: return 8;
			default: return 0;
			}
		}
	}

	auto FTextureSource::IsValid() const -> bool
	{
		const uint32 BytesPerTexel = GetTextureSourceBytesPerTexel(Format);
		if (SchemaVersion != TextureSourceSchemaVersion || BytesPerTexel == 0
			|| Width == 0 || Height == 0 || Depth == 0 || NumSlices == 0)
			return false;
		if (Kind == ETextureSourceKind::Texture2D
			&& (Depth != 1 || NumSlices != 1 || Format != ETextureSourceFormat::RGBA8))
			return false;
		if (Kind == ETextureSourceKind::TextureCube
			&& (Depth != 1 || NumSlices != 6 || Width != Height
				|| Format != ETextureSourceFormat::RGBA8))
			return false;
		if (Kind == ETextureSourceKind::Volume && NumSlices != 1) return false;
		const uint64 TexelCount = static_cast<uint64>(Width) * Height * Depth * NumSlices;
		return TexelCount <= MaximumTextureSourceBytes / BytesPerTexel
			&& Payload.GetPayloadSize() == TexelCount * BytesPerTexel;
	}

	auto FTextureSource::SetPayload(std::span<const std::byte> Bytes) -> bool
	{
		return Bytes.size() <= MaximumTextureSourceBytes
			&& Payload.UpdatePayload(Bytes);
	}

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
		if (!Value.IsValid())
		{
			OutError = "Texture source must be complete and valid.";
			return false;
		}
		GetTextureSourceStorage() = std::move(Value);
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
