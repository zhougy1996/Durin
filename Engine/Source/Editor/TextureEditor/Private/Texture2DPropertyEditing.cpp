#include "Texture2DPropertyEditing.h"

#include "Asset/AssetCompilingManager.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Property.h"
#include "DObject/WeakObjectPtr.h"
#include "PropertyEditor/PropertyEditing.h"
#include "Texture/Texture2DCompilation.h"
#include "AssetForge/Builtins/Texture2DImport.h"

namespace Durin::Editor::Texture
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
			if (Proposal.Origin != EPropertyChangeOrigin::Edit)
			{
				if (!Texture->GetPackage() || !Texture->GetSource().IsValid())
				{
					FTexture2DPropertyEditCause Cause;
					Cause.Code = ETexture2DPropertyEditError::Source;
					Cause.HasPackage = Texture->GetPackage() != nullptr;
					Cause.HasSource = Texture->GetSource().IsValid();
					return Reject(std::move(Cause));
				}
				const auto Built = BuildTexture2DSynchronously(*Texture, Texture->CreateBuildRequest(Settings), {
					.bMarkPackageDirty = true,
					.bReportLoadMutation = false,
					.bSourceDecoderInvoked = false,
				});
				if (Built) return {};
				FTexture2DPropertyEditCause Cause;
				Cause.Code = ETexture2DPropertyEditError::Compilation;
				Cause.CompilationCause = Built.error();
				return Reject(std::move(Cause));
			}

			const TWeakObjectPtr<DTexture2D> WeakTexture(Texture);
			auto MakeDeferredResult = [ObjectPath = Object.GetObjectPath(),
				PropertyName = Proposal.MemberProperty->NamePrivate.ToString()](
				FTexture2DCompilationError Error) -> std::expected<void, FObjectValidationError> {
				if (!Error.HasError()) return {};
				auto Cause = std::make_shared<FTexture2DPropertyEditCause>();
				Cause->Code = ETexture2DPropertyEditError::Compilation;
				Cause->CompilationCause = std::move(Error);
				return std::unexpected(FObjectValidationError{.Code = EObjectValidationError::PropertyRejected,
					.ObjectPath = ObjectPath, .PropertyReason = EPropertyEditRejection::ModuleRejected,
					.PropertyName = PropertyName, .Cause = std::move(Cause)});
			};
			if (!Proposal.Defer(
				[WeakTexture, Settings, MakeDeferredResult](FPropertyEditDeferredCompletion Completion) {
					DTexture2D* LiveTexture = WeakTexture.Get();
					if (!LiveTexture)
					{
						Completion(MakeDeferredResult({.Code = ETexture2DCompilationError::InvalidOwner}));
						return FPropertyEditDeferredCancel{};
					}
					const auto DeferredCompletion =
						std::make_shared<FPropertyEditDeferredCompletion>(std::move(Completion));
					if (const auto Submitted = AssetForge::Builtins::RebuildTexture2DFromSource(
						*LiveTexture, Settings, ETexture2DCompilationPriority::Interactive,
						[DeferredCompletion, MakeDeferredResult](FTexture2DCompilationResult Result) {
							(*DeferredCompletion)(MakeDeferredResult(std::move(Result.Error)));
						}); !Submitted)
					{
						(*DeferredCompletion)(MakeDeferredResult(Submitted.error()));
						return FPropertyEditDeferredCancel{};
					}
					return FPropertyEditDeferredCancel([WeakTexture] {
						if (DTexture2D* PendingTexture = WeakTexture.Get())
							FAssetCompilingManager::Get().MarkCompilationAsCanceled(
								*PendingTexture);
					});
				}))
			{
				FTexture2DPropertyEditCause Cause;
				Cause.Code = ETexture2DPropertyEditError::DeferredValidation;
				return Reject(std::move(Cause));
			}
			return {};
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
		case ETexture2DPropertyEditError::Source:
			return "Only packaged Texture2D assets with canonical imported pixels can rebuild.";
		case ETexture2DPropertyEditError::Compilation:
			return CompilationCause ? FormatTexture2DCompilationError(*CompilationCause) : "Texture2D compilation failed.";
		case ETexture2DPropertyEditError::DeferredValidation:
			return "Texture2D property proposal could not retain asynchronous validation.";
		}
		return "Unknown Texture2D property edit failure.";
	}

	auto RegisterTexture2DPropertyEditing() -> bool
	{
		if (GTexture2DPropertyEditExtension != 0) return true;
		GTexture2DPropertyEditExtension = RegisterPropertyEditExtension({
			.PreEdit = PrepareTexture2DPropertyEdit});
		return GTexture2DPropertyEditExtension != 0;
	}

	auto UnregisterTexture2DPropertyEditing() -> void
	{
		UnregisterPropertyEditExtension(GTexture2DPropertyEditExtension);
		GTexture2DPropertyEditExtension = 0;
	}
}
