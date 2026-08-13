#include "Texture2DPropertyEditing.h"

#include "DObject/DurinPropertyTypes.h"
#include "DObject/Property.h"
#include "DObject/WeakObjectPtr.h"
#include "Editor/PropertyEditing.h"
#include "Texture/TextureBuilder.h"
#include "Texture2DSourceTranslation.h"

namespace Durin::Asset::Import
{
	namespace
	{
		Editor::FPropertyEditExtensionHandle GTexture2DPropertyEditExtension = 0;

		auto PrepareTexture2DPropertyEdit(
			DObject& Object,
			FPropertyEditProposal& Proposal,
			std::string& OutError) -> bool
		{
			DTexture2D* Texture = Cast<DTexture2D>(&Object);
			if (!Texture || !Proposal.MemberProperty
				|| !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return true;

			Asset::Build::FTexture2DBuildSettings Settings =
				MakeTexture2DBuildSettings(*Texture);
			const FName PropertyName = Proposal.MemberProperty->NamePrivate;
			if (PropertyName == FName("Usage"))
			{
				if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Enum)
				{
					OutError = "The texture usage metadata is unavailable.";
					return false;
				}
				Settings.Usage = static_cast<ETextureUsage>(
					static_cast<const FEnumProperty*>(Proposal.DraftRootProperty)->GetValueAsUInt64(
						Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex));
				Settings.bSRGB = Asset::Build::TextureBuilder::GetDefaultSRGB(Settings.Usage);
			}
			else if (PropertyName == FName("bSRGB"))
			{
				if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Bool)
				{
					OutError = "The texture color-space metadata is unavailable.";
					return false;
				}
				Settings.bSRGB = *Proposal.DraftRootProperty->ContainerPtrToValuePtr<bool>(
					Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			}
			else if (PropertyName == FName("MaxResolution"))
			{
				if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::UInt32)
				{
					OutError = "The texture maximum-resolution metadata is unavailable.";
					return false;
				}
				Settings.MaxResolution = *Proposal.DraftRootProperty->ContainerPtrToValuePtr<uint32>(
					Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			}
			else if (PropertyName == FName("CompressionQuality"))
			{
				if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Enum)
				{
					OutError = "The texture compression-quality metadata is unavailable.";
					return false;
				}
				Settings.CompressionQuality = static_cast<ETextureCompressionQuality>(
					static_cast<const FEnumProperty*>(Proposal.DraftRootProperty)->GetValueAsUInt64(
						Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex));
			}
			else if (PropertyName == FName("AlphaMipMode"))
			{
				if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Enum)
				{
					OutError = "The texture alpha-mip-mode metadata is unavailable.";
					return false;
				}
				Settings.AlphaMipMode = static_cast<ETextureAlphaMipMode>(
					static_cast<const FEnumProperty*>(Proposal.DraftRootProperty)->GetValueAsUInt64(
						Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex));
			}
			else if (PropertyName == FName("AlphaCoverageThreshold"))
			{
				if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Float)
				{
					OutError = "The texture alpha-coverage-threshold metadata is unavailable.";
					return false;
				}
				Settings.AlphaCoverageThreshold =
					*Proposal.DraftRootProperty->ContainerPtrToValuePtr<float>(
						Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			}
			else return true;

			if (!Asset::Build::TextureBuilder::IsValidUsage(Settings.Usage)
				|| !Asset::Build::TextureBuilder::IsValidCompressionQuality(Settings.CompressionQuality)
				|| !Asset::Build::TextureBuilder::IsValidAlphaMipMode(Settings.AlphaMipMode)
				|| !Asset::Build::TextureBuilder::IsValidAlphaCoverageThreshold(
					Settings.AlphaCoverageThreshold))
			{
				OutError = "Texture2D property proposal contains invalid build settings.";
				return false;
			}

			const TWeakObjectPtr<DTexture2D> WeakTexture(Texture);
			if (!Proposal.Defer(
				[WeakTexture, Settings](FPropertyEditDeferredCompletion Completion) {
					DTexture2D* LiveTexture = WeakTexture.Get();
					if (!LiveTexture)
					{
						Completion(false, "The Texture2D property proposal target is unavailable.");
						return FPropertyEditDeferredCancel{};
					}
					std::string Error;
					if (!RebuildTexture2DFromCurrentSource(
						*LiveTexture,
						Settings,
						Error,
						Asset::Build::ETexture2DBuildPriority::Interactive,
						Completion))
					{
						Completion(false, std::move(Error));
						return FPropertyEditDeferredCancel{};
					}
					return FPropertyEditDeferredCancel([WeakTexture] {
						if (DTexture2D* PendingTexture = WeakTexture.Get())
							Asset::Build::CancelTexture2DBuild(*PendingTexture);
					});
				}))
			{
				OutError = "Texture2D property proposal could not retain asynchronous validation.";
				return false;
			}
			return true;
		}
	}

	auto RegisterTexture2DPropertyEditing() -> bool
	{
		if (GTexture2DPropertyEditExtension != 0) return true;
		GTexture2DPropertyEditExtension = Editor::RegisterPropertyEditExtension({
			.PreEdit = PrepareTexture2DPropertyEdit});
		return GTexture2DPropertyEditExtension != 0;
	}

	auto UnregisterTexture2DPropertyEditing() -> void
	{
		Editor::UnregisterPropertyEditExtension(GTexture2DPropertyEditExtension);
		GTexture2DPropertyEditExtension = 0;
	}
}
