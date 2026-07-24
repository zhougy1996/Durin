#include "Texture/Texture2D.h"

#include "AssetCore.h"
#include "AssetSystem.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "ImageDecoder.h"
#include "Misc/Paths.h"
#include "DynamicRHI.h"
#include "Texture/Texture2DRenderResource.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 TextureChannelCount = 4;

		auto IsValidTextureUsage(ETextureUsage Usage) -> bool
		{
			return Usage == ETextureUsage::Color || Usage == ETextureUsage::Normal || Usage == ETextureUsage::DataMask;
		}

		auto GetDefaultSRGB(ETextureUsage Usage) -> bool
		{
			return Usage == ETextureUsage::Color;
		}

		auto SelectTexturePixelFormat(ETextureUsage Usage, bool bSRGB, bool bHasTransparency) -> EPixelFormat
		{
			// Usage and alpha are deliberately part of this decision even while the
			// uncompressed backend has only one RGBA choice; compression plugs in here.
			(void)Usage;
			(void)bHasTransparency;
			return bSRGB ? EPixelFormat::SRGBA8_UNORM : EPixelFormat::RGBA8_UNORM;
		}

		auto DecodeSRGB(uint8 Value) -> double
		{
			const double Encoded = static_cast<double>(Value) / 255.0;
			return Encoded <= 0.04045 ? Encoded / 12.92 : std::pow((Encoded + 0.055) / 1.055, 2.4);
		}

		auto EncodeUNorm(double Value) -> uint8
		{
			return static_cast<uint8>(std::clamp(Value, 0.0, 1.0) * 255.0 + 0.5);
		}

		auto EncodeSRGB(double Value) -> uint8
		{
			const double Linear = std::clamp(Value, 0.0, 1.0);
			return EncodeUNorm(Linear <= 0.0031308 ? Linear * 12.92 : 1.055 * std::pow(Linear, 1.0 / 2.4) - 0.055);
		}

		auto BuildNextMip(const FTexture2DMipData& Source, ETextureUsage Usage, bool bSRGB) -> FTexture2DMipData
		{
			FTexture2DMipData Result;
			Result.Width = std::max(Source.Width / 2, 1u);
			Result.Height = std::max(Source.Height / 2, 1u);
			Result.RowPitch = Result.Width * TextureChannelCount;
			Result.Pixels.resize(static_cast<size_t>(Result.RowPitch) * Result.Height);

			for (uint32 DestY = 0; DestY < Result.Height; ++DestY)
			{
				const uint32 BeginY = DestY * Source.Height / Result.Height;
				const uint32 EndY = (DestY + 1) * Source.Height / Result.Height;
				for (uint32 DestX = 0; DestX < Result.Width; ++DestX)
				{
					const uint32 BeginX = DestX * Source.Width / Result.Width;
					const uint32 EndX = (DestX + 1) * Source.Width / Result.Width;
					const uint32 SampleCount = (EndX - BeginX) * (EndY - BeginY);
					std::array<double, TextureChannelCount> Sum{};
					for (uint32 SourceY = BeginY; SourceY < EndY; ++SourceY)
					{
						for (uint32 SourceX = BeginX; SourceX < EndX; ++SourceX)
						{
							const size_t SourceOffset = static_cast<size_t>(SourceY) * Source.RowPitch + SourceX * TextureChannelCount;
							for (uint32 Channel = 0; Channel < TextureChannelCount; ++Channel)
							{
								const uint8 Value = Source.Pixels[SourceOffset + Channel];
								if (Usage == ETextureUsage::Color && bSRGB && Channel < 3) Sum[Channel] += DecodeSRGB(Value);
								else if (Usage == ETextureUsage::Normal && Channel < 3) Sum[Channel] += static_cast<double>(Value) / 127.5 - 1.0;
								else Sum[Channel] += static_cast<double>(Value) / 255.0;
							}
						}
					}

					const size_t DestOffset = static_cast<size_t>(DestY) * Result.RowPitch + DestX * TextureChannelCount;
					if (Usage == ETextureUsage::Normal)
					{
						double X = Sum[0] / SampleCount;
						double Y = Sum[1] / SampleCount;
						double Z = Sum[2] / SampleCount;
						const double LengthSquared = X * X + Y * Y + Z * Z;
						if (LengthSquared > std::numeric_limits<double>::epsilon())
						{
							const double InverseLength = 1.0 / std::sqrt(LengthSquared);
							X *= InverseLength;
							Y *= InverseLength;
							Z *= InverseLength;
						}
						else
						{
							X = 0.0;
							Y = 0.0;
							Z = 1.0;
						}
						Result.Pixels[DestOffset] = EncodeUNorm(X * 0.5 + 0.5);
						Result.Pixels[DestOffset + 1] = EncodeUNorm(Y * 0.5 + 0.5);
						Result.Pixels[DestOffset + 2] = EncodeUNorm(Z * 0.5 + 0.5);
					}
					else
					{
						for (uint32 Channel = 0; Channel < 3; ++Channel)
						{
							const double Average = Sum[Channel] / SampleCount;
							Result.Pixels[DestOffset + Channel] = Usage == ETextureUsage::Color && bSRGB ? EncodeSRGB(Average) : EncodeUNorm(Average);
						}
					}
					Result.Pixels[DestOffset + 3] = EncodeUNorm(Sum[3] / SampleCount);
				}
			}
			return Result;
		}

		auto ResolveMountedFile(std::string_view VirtualPath) -> std::filesystem::path
		{
			for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
			{
				if (VirtualPath.starts_with(Mount.VirtualRoot))
				{
					return (std::filesystem::path(Mount.PhysicalPath) / std::string(VirtualPath.substr(Mount.VirtualRoot.size()))).lexically_normal();
				}
			}
			return std::filesystem::path(VirtualPath).lexically_normal();
		}

		auto ResolveTextureSource(const DTexture2D& Texture) -> std::filesystem::path
		{
			const std::filesystem::path StoredPath(Texture.GetSourceFile());
			const std::filesystem::path PackageFile = ResolveMountedFile(Texture.GetPackage()->GetPackagePath());
			if (!StoredPath.is_absolute() && !Texture.GetSourceFile().starts_with('/'))
			{
				return (PackageFile.parent_path() / StoredPath).lexically_normal();
			}

			const std::filesystem::path LegacyPath = ResolveMountedFile(Texture.GetSourceFile());
			if (std::filesystem::is_regular_file(LegacyPath)) return LegacyPath;
			return (PackageFile.parent_path() / StoredPath.filename()).lexically_normal();
		}
	} // namespace

	auto FTextureSourceData::IsValid() const -> bool
	{
		return Format == ETextureSourceFormat::RGBA8
			&& Width > 0
			&& Height > 0
			&& static_cast<uint64>(Width) * Height * TextureChannelCount == Pixels.size();
	}

	auto FTexture2DMipData::IsValid(EPixelFormat PixelFormat) const -> bool
	{
		if (PixelFormat == EPixelFormat::Unknown || Width == 0 || Height == 0) return false;
		const FPixelFormatInfo& FormatInfo = GetPixelFormatInfo(PixelFormat);
		const uint64 BlocksWide = (static_cast<uint64>(Width) + FormatInfo.BlockSize - 1) / FormatInfo.BlockSize;
		const uint64 BlocksHigh = (static_cast<uint64>(Height) + FormatInfo.BlockSize - 1) / FormatInfo.BlockSize;
		const uint64 ExpectedRowPitch = BlocksWide * FormatInfo.BytesPerBlock;
		return RowPitch == ExpectedRowPitch && static_cast<uint64>(RowPitch) * BlocksHigh == Pixels.size();
	}

	auto FTexturePlatformData::IsValid() const -> bool
	{
		if (PixelFormat == EPixelFormat::Unknown || Mips.empty() || Mips.size() > std::numeric_limits<uint8>::max()) return false;
		for (size_t MipIndex = 0; MipIndex < Mips.size(); ++MipIndex)
		{
			if (!Mips[MipIndex].IsValid(PixelFormat)) return false;
			if (MipIndex > 0)
			{
				const FTexture2DMipData& Previous = Mips[MipIndex - 1];
				if (Mips[MipIndex].Width != std::max(Previous.Width / 2, 1u)
					|| Mips[MipIndex].Height != std::max(Previous.Height / 2, 1u)) return false;
			}
		}
		return true;
	}

	DTexture2D::DTexture2D(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, RenderResource(std::make_shared<FTexture2DRenderResource>())
	{
		static const bool RegisteredAssetContributors = [] {
			Asset::RegisterAssetMoveContributor(DTexture2D::StaticClass(), [](DObject* Object, const FAssetPath& OldPath, const FAssetPath& NewPath, Asset::FAssetMoveContribution& Out) -> Asset::FAssetResult {
				auto* Texture = Cast<DTexture2D>(Object);
				if (!Texture || Texture->SourceFile.empty()) return {};
				const std::string Original = Texture->SourceFile;
				const std::filesystem::path OldPackage = ResolveMountedFile(OldPath.ToString());
				const std::filesystem::path NewPackage = ResolveMountedFile(NewPath.ToString());
				const std::filesystem::path SourceName(Original);
				const std::filesystem::path OldSource = SourceName.is_absolute() ? SourceName : OldPackage.parent_path() / SourceName;
				const std::string NewFileName = OldPath.GetAssetName() == NewPath.GetAssetName()
					? SourceName.filename().generic_string()
					: std::string(NewPath.GetAssetName()) + SourceName.extension().generic_string();
				const std::filesystem::path NewSource = NewPackage.parent_path() / NewFileName;
				if (OldSource.lexically_normal() != NewSource.lexically_normal()) Out.Files.emplace_back(OldSource.lexically_normal(), NewSource.lexically_normal());
				if (NewFileName != Original)
				{
					Out.Apply = [Texture, NewFileName] { Texture->SourceFile = NewFileName; };
					Out.Rollback = [Texture, Original] { Texture->SourceFile = Original; };
				}
				return {};
			});
			Asset::RegisterAssetDeleteContributor(DTexture2D::StaticClass(), [](DObject* Object, Asset::FAssetDeleteContribution& Out) -> Asset::FAssetResult {
				auto* Texture = Cast<DTexture2D>(Object);
				if (Texture && !Texture->SourceFile.empty()) Out.Files.push_back(ResolveTextureSource(*Texture));
				return {};
			});
			return true;
		}();
		(void)RegisteredAssetContributors;
	}

	DTexture2D::~DTexture2D()
	{
		const uint64 ReleaseRevision = ++BuildRevision;
		if (GDynamicRHI != nullptr)
		{
			RenderResource->QueueRelease(ReleaseRevision);
		}
	}

	auto DTexture2D::InvalidatePlatformData() -> void
	{
		PlatformData.reset();
		const uint64 ReleaseRevision = ++BuildRevision;
		if (GDynamicRHI != nullptr) RenderResource->QueueRelease(ReleaseRevision);
	}

	auto DTexture2D::QueueRenderResourceBuild() -> void
	{
		check(PlatformData && PlatformData->IsValid());
		const uint64 Revision = ++BuildRevision;
		if (GDynamicRHI == nullptr) return;
		// The immutable value snapshot decouples queued uploads from subsequent imports/rebuilds.
		RenderResource->QueueBuild(std::make_shared<const FTexturePlatformData>(*PlatformData), Revision);
	}

	auto DTexture2D::BuildSourceData(std::string_view PhysicalFilePath, std::string& OutError) -> bool
	{
		Asset::FDecodedImage DecodedImage;
		if (!Asset::DecodeImageFromFile(PhysicalFilePath, DecodedImage, OutError))
		{
			SourceData.reset();
			InvalidatePlatformData();
			return false;
		}

		auto NewSourceData = std::make_unique<FTextureSourceData>();
		NewSourceData->Pixels = std::move(DecodedImage.Pixels);
		NewSourceData->Width = DecodedImage.Width;
		NewSourceData->Height = DecodedImage.Height;
		NewSourceData->SourceChannelCount = DecodedImage.SourceChannelCount;
		NewSourceData->Format = ETextureSourceFormat::RGBA8;
		NewSourceData->bHasTransparency = DecodedImage.bHasTransparency;
		if (!NewSourceData->IsValid())
		{
			OutError = "Decoded texture source data is invalid.";
			SourceData.reset();
			InvalidatePlatformData();
			return false;
		}

		SourceData = std::move(NewSourceData);
		return RebuildPlatformData(OutError);
	}

	auto DTexture2D::RebuildPlatformData(std::string& OutError) -> bool
	{
		std::unique_ptr<FTexturePlatformData> NewPlatformData;
		if (!BuildPlatformData(Usage, bSRGB, NewPlatformData, OutError))
		{
			InvalidatePlatformData();
			return false;
		}
		PlatformData = std::move(NewPlatformData);
		QueueRenderResourceBuild();
		return true;
	}

	auto DTexture2D::BuildPlatformData(ETextureUsage InUsage, bool bInSRGB,
		std::unique_ptr<FTexturePlatformData>& OutPlatformData, std::string& OutError) const -> bool
	{
		OutError.clear();
		OutPlatformData.reset();
		if (!SourceData || !SourceData->IsValid())
		{
			OutError = "Texture source data is unavailable or invalid.";
			return false;
		}
		if (!IsValidTextureUsage(InUsage))
		{
			OutError = "Texture usage preset is invalid.";
			return false;
		}

		auto NewPlatformData = std::make_unique<FTexturePlatformData>();
		NewPlatformData->PixelFormat = SelectTexturePixelFormat(InUsage, bInSRGB, SourceData->bHasTransparency);
		FTexture2DMipData& BaseMip = NewPlatformData->Mips.emplace_back();
		BaseMip.Pixels = SourceData->Pixels;
		BaseMip.Width = SourceData->Width;
		BaseMip.Height = SourceData->Height;
		BaseMip.RowPitch = SourceData->Width * TextureChannelCount;
		while (NewPlatformData->Mips.back().Width > 1 || NewPlatformData->Mips.back().Height > 1)
		{
			NewPlatformData->Mips.push_back(BuildNextMip(NewPlatformData->Mips.back(), InUsage, bInSRGB));
		}
		if (!NewPlatformData->IsValid())
		{
			OutError = "Failed to build texture platform data.";
			return false;
		}
		OutPlatformData = std::move(NewPlatformData);
		return true;
	}

	auto DTexture2D::SetUsage(ETextureUsage InUsage, std::string& OutError) -> bool
	{
		OutError.clear();
		if (!IsValidTextureUsage(InUsage))
		{
			OutError = "Texture usage preset is invalid.";
			return false;
		}
		if (Usage == InUsage) return true;
		if (!SourceData || !SourceData->IsValid())
		{
			OutError = "Texture source data is unavailable or invalid.";
			return false;
		}
		const ETextureUsage PreviousUsage = Usage;
		const bool bPreviousSRGB = bSRGB;
		Usage = InUsage;
		bSRGB = GetDefaultSRGB(Usage);
		if (RebuildPlatformData(OutError))
		{
			MarkPackageDirty();
			return true;
		}
		Usage = PreviousUsage;
		bSRGB = bPreviousSRGB;
		std::string RestoreError;
		RebuildPlatformData(RestoreError);
		return false;
	}

	auto DTexture2D::SetSRGB(bool bInSRGB, std::string& OutError) -> bool
	{
		OutError.clear();
		if (bSRGB == bInSRGB) return true;
		if (!SourceData || !SourceData->IsValid())
		{
			OutError = "Texture source data is unavailable or invalid.";
			return false;
		}
		const bool bPreviousSRGB = bSRGB;
		bSRGB = bInSRGB;
		if (RebuildPlatformData(OutError))
		{
			MarkPackageDirty();
			return true;
		}
		bSRGB = bPreviousSRGB;
		std::string RestoreError;
		RebuildPlatformData(RestoreError);
		return false;
	}

	auto DTexture2D::PostLoad(std::string& OutError) -> bool
	{
		if (SourceFile.empty())
		{
			OutError = "Texture asset has no source file.";
			return false;
		}
		const std::filesystem::path PhysicalPath = ResolveTextureSource(*this);
		if (!std::filesystem::is_regular_file(PhysicalPath))
		{
			OutError = std::format("Texture source file does not exist: {}", SourceFile);
			return false;
		}
		return BuildSourceData(PhysicalPath.generic_string(), OutError);
	}

	auto DTexture2D::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return true;

		ETextureUsage CandidateUsage = Usage;
		bool bCandidateSRGB = bSRGB;
		const FName PropertyName = Proposal.MemberProperty->NamePrivate;
		if (PropertyName == FName("Usage"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Enum)
			{
				OutError = "The texture usage metadata is unavailable.";
				return false;
			}
			CandidateUsage = static_cast<ETextureUsage>(static_cast<const FEnumProperty*>(Proposal.DraftRootProperty)->GetValueAsUInt64(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex));
			bCandidateSRGB = GetDefaultSRGB(CandidateUsage);
		}
		else if (PropertyName == FName("bSRGB"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Bool)
			{
				OutError = "The texture color-space metadata is unavailable.";
				return false;
			}
			bCandidateSRGB = *Proposal.DraftRootProperty->ContainerPtrToValuePtr<bool>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		}
		else return true;

		std::unique_ptr<FTexturePlatformData> CandidatePlatformData;
		if (!BuildPlatformData(CandidateUsage, bCandidateSRGB, CandidatePlatformData, OutError)) return false;
		PendingEditUsage = CandidateUsage;
		bPendingEditSRGB = bCandidateSRGB;
		PendingEditPlatformData = std::move(CandidatePlatformData);
		return true;
	}

	auto DTexture2D::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		const FName PropertyName = Event.MemberProperty->NamePrivate;
		if (PropertyName != FName("Usage") && PropertyName != FName("bSRGB")) return;
		if (Event.Phase == EPropertyChangePhase::Committed && Event.Origin == EPropertyChangeOrigin::Edit) return;
		if (!PendingEditPlatformData || PendingEditUsage != Usage) return;

		bSRGB = bPendingEditSRGB;
		PlatformData = std::move(PendingEditPlatformData);
		QueueRenderResourceBuild();
	}

	auto DTexture2D::ImportAsset(std::string_view FilePath, std::string_view AssetPath, const FTexture2DImportSettings& Settings) -> FTexture2DImportResult
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input)) return {false, "Source file does not exist.", nullptr};
		if (!Asset::IsSupportedImageExtension(Input.extension().generic_string())) return {false, "Unsupported texture source format.", nullptr};
		if (!IsValidTextureUsage(Settings.Usage)) return {false, "Texture usage preset is invalid.", nullptr};

		FAssetPath ParsedAssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &PathError)) return {false, std::move(PathError), nullptr};
		if (Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};

		const std::string Extension = Input.extension().generic_string();
		const std::string SourceFileName = std::string(ParsedAssetPath.GetAssetName()) + Extension;
		const std::filesystem::path Destination = std::filesystem::path(ResolveMountedFile(ParsedAssetPath.ToString())).replace_extension(Extension);
		if (std::filesystem::exists(Destination)) return {false, std::format("Imported source already exists: {}", Destination.generic_string()), nullptr};

		DTexture2D* Texture = nullptr;
		Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!CreateResult) return {false, CreateResult.Message, nullptr};
		Texture->Usage = Settings.Usage;
		Texture->bSRGB = Settings.bSRGB.value_or(GetDefaultSRGB(Settings.Usage));
		std::string BuildError;
		if (!Texture->BuildSourceData(Input.generic_string(), BuildError))
		{
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::move(BuildError), nullptr};
		}

		std::error_code ErrorCode;
		std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
		if (ErrorCode || !std::filesystem::copy_file(Input, Destination, std::filesystem::copy_options::none, ErrorCode))
		{
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::format("Failed to copy source file to {}: {}", Destination.generic_string(), ErrorCode.message()), nullptr};
		}
		Texture->SourceFile = SourceFileName;
		Asset::FAssetResult SaveResult = Asset::SavePackage(Texture->GetPackage());
		if (!SaveResult)
		{
			std::filesystem::remove(Destination, ErrorCode);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, SaveResult.Message, nullptr};
		}
		return {true, {}, Texture};
	}
}
