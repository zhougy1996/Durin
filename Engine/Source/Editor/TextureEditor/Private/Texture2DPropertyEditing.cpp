#include "Texture2DPropertyEditing.h"

#include "DObject/DurinPropertyTypes.h"
#include "DObject/Property.h"
#include "PropertyEditor/PropertyEditing.h"
#include "Texture/Texture2DCompilation.h"

namespace Durin::TextureEditor
{
	namespace
	{
		FPropertyEditExtensionHandle GTexture2DPropertyEditExtension = 0;

		auto MakeTexture2DBuildSettings(const DTexture2D& Texture)
			-> FTexture2DBuildSettings
		{
			return {
				.Usage = Texture.GetUsage(),
				.CompressionQuality = Texture.GetCompressionQuality(),
				.AlphaMipMode = Texture.GetAlphaMipMode(),
				.AlphaCoverageThreshold = Texture.GetAlphaCoverageThreshold(),
				.MaxResolution = Texture.GetMaxResolution(),
				.bSRGB = Texture.IsSRGB()};
		}

		auto PrepareTexture2DPropertyEdit(
			DObject& Object,
			FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError>
		{
			DTexture2D* Texture = Cast<DTexture2D>(&Object);
			if (!Texture || !Proposal.MemberProperty
				|| !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return {};

			auto Reject = [&](FTexture2DPropertyEditCause Cause) {
				return RejectPropertyEdit(Object, Proposal, EPropertyEditRejection::ModuleRejected,
					std::make_shared<FTexture2DPropertyEditCause>(std::move(Cause)));
			};
			auto RejectMetadata = [&](DurinCodeGen::EPropertyGenFlags Expected) {
				FTexture2DPropertyEditCause Cause;
				Cause.Code = ETexture2DPropertyEditError::Metadata;
				Cause.ActualKind = static_cast<uint64>(Proposal.DraftRootProperty->GetKind());
				Cause.ExpectedKind = static_cast<uint64>(Expected);
				return Reject(std::move(Cause));
			};

			FTexture2DBuildSettings Settings =
				MakeTexture2DBuildSettings(*Texture);
			const FName PropertyName = Proposal.MemberProperty->NamePrivate;
			if (PropertyName == FName("Usage"))
			{
				if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Enum)
				{
					return RejectMetadata(DurinCodeGen::EPropertyGenFlags::Enum);
				}
				Settings.Usage = static_cast<ETextureUsage>(
					static_cast<const FEnumProperty*>(Proposal.DraftRootProperty)->GetValueAsUInt64(
						Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex));
				Settings.bSRGB = GetDefaultTextureSRGB(Settings.Usage);
			}
			else if (PropertyName == FName("bSRGB"))
			{
				if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Bool)
				{
					return RejectMetadata(DurinCodeGen::EPropertyGenFlags::Bool);
				}
				Settings.bSRGB = *Proposal.DraftRootProperty->ContainerPtrToValuePtr<bool>(
					Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			}
			else if (PropertyName == FName("MaxResolution"))
			{
				if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::UInt32)
				{
					return RejectMetadata(DurinCodeGen::EPropertyGenFlags::UInt32);
				}
				Settings.MaxResolution = *Proposal.DraftRootProperty->ContainerPtrToValuePtr<uint32>(
					Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			}
			else if (PropertyName == FName("CompressionQuality"))
			{
				if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Enum)
				{
					return RejectMetadata(DurinCodeGen::EPropertyGenFlags::Enum);
				}
				Settings.CompressionQuality = static_cast<ETextureCompressionQuality>(
					static_cast<const FEnumProperty*>(Proposal.DraftRootProperty)->GetValueAsUInt64(
						Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex));
			}
			else if (PropertyName == FName("AlphaMipMode"))
			{
				if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Enum)
				{
					return RejectMetadata(DurinCodeGen::EPropertyGenFlags::Enum);
				}
				Settings.AlphaMipMode = static_cast<ETextureAlphaMipMode>(
					static_cast<const FEnumProperty*>(Proposal.DraftRootProperty)->GetValueAsUInt64(
						Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex));
			}
			else if (PropertyName == FName("AlphaCoverageThreshold"))
			{
				if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Float)
				{
					return RejectMetadata(DurinCodeGen::EPropertyGenFlags::Float);
				}
				Settings.AlphaCoverageThreshold =
					*Proposal.DraftRootProperty->ContainerPtrToValuePtr<float>(
						Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			}
			else return {};

			if (const auto Validation = ValidateTexture2DBuildSettings(Settings); !Validation)
			{
				FTexture2DPropertyEditCause Cause;
				Cause.Code = ETexture2DPropertyEditError::Settings;
				Cause.InputCause = Validation.error();
				return Reject(std::move(Cause));
			}

			return {};
		}
		auto OnTexture2DPropertyEdited(DObject& Object, const FPropertyChangedEvent& Event, bool bValueChanged) -> void
		{
			if (!bValueChanged || !Event.MemberProperty || Event.Phase != EPropertyChangePhase::Committed) return;
			auto* Texture = Cast<DTexture2D>(&Object);
			if (!Texture) return;
			const FName Name = Event.MemberProperty->NamePrivate;
			const bool bUsageChanged = Name == FName("Usage");
			if (bUsageChanged || Name == FName("bSRGB") || Name == FName("MaxResolution")
				|| Name == FName("CompressionQuality") || Name == FName("AlphaMipMode") || Name == FName("AlphaCoverageThreshold"))
				Texture->OnBuildSettingsEdited(bUsageChanged);
		}

	}

	auto FTexture2DPropertyEditCause::Format() const -> std::string
	{
		switch (Code)
		{
		case ETexture2DPropertyEditError::Metadata:
			return std::format("Texture2D property metadata kind {} does not match {}.", ActualKind, ExpectedKind);
		case ETexture2DPropertyEditError::Settings:
			return InputCause ? FormatTexture2DInputError(*InputCause) : "Texture2D build settings are invalid.";

		}
		return "Unknown Texture2D property edit failure.";
	}

	auto RegisterTexture2DPropertyEditing() -> bool
	{
		if (GTexture2DPropertyEditExtension != 0) return true;
		GTexture2DPropertyEditExtension = RegisterPropertyEditExtension({
			.PreEdit = PrepareTexture2DPropertyEdit, .PostEdit = OnTexture2DPropertyEdited});
		return GTexture2DPropertyEditExtension != 0;
	}

	auto UnregisterTexture2DPropertyEditing() -> void
	{
		UnregisterPropertyEditExtension(GTexture2DPropertyEditExtension);
		GTexture2DPropertyEditExtension = 0;
	}
}
