#include "Texture/TextureBuildOperations.h"

#include "AssetBuild/BuildSession.h"
#include "Hash/XxHash.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "Asset/SourcePath.h"
#include "Texture/TextureBuilder.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/Texture2DDerivedData.h"

namespace Durin::Asset::Build
{
	namespace
	{
		constexpr uint64 TextureDerivedDataBudgetBytes = 4ull * 1024ull * 1024ull * 1024ull;
		constexpr uint32 TextureDerivedDataCleanupDeleteLimit = 16;

		auto IsCanonicalTextureHash(std::string_view Hash) -> bool
		{
			return Hash.size() == 32 && std::ranges::all_of(Hash, [](char Character) {
				return Character >= '0' && Character <= '9'
					|| Character >= 'a' && Character <= 'f';
			});
		}

		const FBuildFunctionIdentity Texture2DFunctionIdentity{
			"Durin.TextureBuild.Texture2D", 1};
		inline constexpr std::string_view Texture2DInputName = "Texture2DInput";
		inline constexpr std::string_view Texture2DValueName = "Texture2DPayload";

		auto AppendU32(std::vector<uint8>& Bytes, uint32 Value) -> void
		{
			for (uint32 Byte = 0; Byte < 4; ++Byte)
				Bytes.push_back(static_cast<uint8>(Value >> (Byte * 8)));
		}
		auto AppendU64(std::vector<uint8>& Bytes, uint64 Value) -> void
		{
			for (uint32 Byte = 0; Byte < 8; ++Byte)
				Bytes.push_back(static_cast<uint8>(Value >> (Byte * 8)));
		}
		auto ReadU32(std::span<const uint8> Bytes, size_t& Offset, uint32& Value) -> bool
		{
			if (Offset + 4 > Bytes.size()) return false;
			Value = 0;
			for (uint32 Byte = 0; Byte < 4; ++Byte) Value |= uint32(Bytes[Offset++]) << (Byte * 8);
			return true;
		}
		auto ReadU64(std::span<const uint8> Bytes, size_t& Offset, uint64& Value) -> bool
		{
			if (Offset + 8 > Bytes.size()) return false;
			Value = 0;
			for (uint32 Byte = 0; Byte < 8; ++Byte) Value |= uint64(Bytes[Offset++]) << (Byte * 8);
			return true;
		}

		auto EncodeTextureInput(const FTexture2DBuildRequest& Request, bool bSRGB)
			-> std::vector<uint8>
		{
			std::vector<uint8> Bytes;
			AppendU32(Bytes, Request.SourceData.Width);
			AppendU32(Bytes, Request.SourceData.Height);
			Bytes.push_back(Request.SourceData.SourceChannelCount);
			Bytes.push_back(static_cast<uint8>(Request.SourceData.Format));
			Bytes.push_back(Request.SourceData.bHasTransparency);
			Bytes.push_back(bSRGB);
			AppendU32(Bytes, static_cast<uint32>(Request.Settings.Usage));
			AppendU32(Bytes, static_cast<uint32>(Request.Settings.CompressionQuality));
			AppendU32(Bytes, static_cast<uint32>(Request.Settings.AlphaMipMode));
			AppendU32(Bytes, std::bit_cast<uint32>(Request.Settings.AlphaCoverageThreshold));
			AppendU32(Bytes, Request.Settings.MaxResolution);
			AppendU64(Bytes, Request.SourceData.Pixels.size());
			Bytes.insert(Bytes.end(), Request.SourceData.Pixels.begin(), Request.SourceData.Pixels.end());
			return Bytes;
		}

		auto DecodeTextureInput(std::span<const uint8> Bytes,
			FTextureSourceData& Source, FTexture2DBuildSettings& Settings,
			bool& bSRGB, std::string& OutError) -> bool
		{
			size_t Offset = 0;
			uint32 Usage = 0, Quality = 0, AlphaMode = 0, AlphaThreshold = 0;
			uint64 PixelCount = 0;
			if (!ReadU32(Bytes, Offset, Source.Width) || !ReadU32(Bytes, Offset, Source.Height)
				|| Offset + 4 > Bytes.size()) goto Invalid;
			Source.SourceChannelCount = Bytes[Offset++];
			Source.Format = static_cast<ETextureSourceFormat>(Bytes[Offset++]);
			Source.bHasTransparency = Bytes[Offset++] != 0;
			bSRGB = Bytes[Offset++] != 0;
			if (!ReadU32(Bytes, Offset, Usage) || !ReadU32(Bytes, Offset, Quality)
				|| !ReadU32(Bytes, Offset, AlphaMode) || !ReadU32(Bytes, Offset, AlphaThreshold)
				|| !ReadU32(Bytes, Offset, Settings.MaxResolution)
				|| !ReadU64(Bytes, Offset, PixelCount) || PixelCount > Bytes.size() - Offset
				|| Offset + PixelCount != Bytes.size()) goto Invalid;
			Settings.Usage = static_cast<ETextureUsage>(Usage);
			Settings.CompressionQuality = static_cast<ETextureCompressionQuality>(Quality);
			Settings.AlphaMipMode = static_cast<ETextureAlphaMipMode>(AlphaMode);
			Settings.AlphaCoverageThreshold = std::bit_cast<float>(AlphaThreshold);
			Settings.bSRGB = bSRGB;
			Source.Pixels.assign(Bytes.begin() + Offset, Bytes.end());
			if (Source.IsValid()) return true;
		Invalid:
			OutError = "Texture2D local build input is malformed.";
			return false;
		}

		auto DecodePlatformValue(const FBuildValue& Value,
			FTexturePlatformData& OutData, std::string& OutError) -> bool
		{
			FCanonicalMemoryReader Ar(Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
			OutData.Serialize(Ar, {.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			if (!Ar.HasError() && RequireArchiveEnd(Ar) && OutData.IsValid()) return true;
			OutError = Ar.GetError().empty() ? "Texture2D payload is invalid." : Ar.GetError();
			return false;
		}

		class FTexture2DBuildFunction final : public IBuildFunction
		{
		public:
			auto GetConfig() const -> FBuildFunctionConfig override
			{
				return {.CacheRoot = "Textures/Objects",
					.ExpectedValueName = std::string(Texture2DValueName),
					.MaximumValueBytes = MaximumTexturePayloadBytes,
					.CleanupBudgetBytes = TextureDerivedDataBudgetBytes,
					.CleanupDeleteLimit = TextureDerivedDataCleanupDeleteLimit};
			}
			auto Validate(const FBuildDefinition&, const FBuildValue& Value,
				std::string& OutError) const -> bool override
			{
				FTexturePlatformData Data;
				return Value.GetName() == Texture2DValueName
					&& DecodePlatformValue(Value, Data, OutError);
			}
			auto Build(const FBuildContext& Context, FBuildValue& OutValue,
				std::string& OutError) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(Texture2DInputName);
				FTextureSourceData Source;
				FTexture2DBuildSettings Settings;
				bool bSRGB = true;
				if (!Input || !DecodeTextureInput(Input->GetBytes(), Source, Settings, bSRGB, OutError))
					return false;
				FTexturePlatformData PlatformData;
				const TextureBuilder::FBuildExecutionControl Control{
					.ShouldCancel = [&Context] { return Context.IsCanceled(); }};
				if (!TextureBuilder::BuildMipChain(Source, Settings.Usage, bSRGB,
					PlatformData, OutError, Settings.MaxResolution,
					Settings.CompressionQuality, Settings.AlphaMipMode,
					Settings.AlphaCoverageThreshold, &Control)) return false;
				if (Context.IsCanceled()) { OutError = "Texture2D build was cancelled."; return false; }
				std::vector<uint8> Bytes;
				FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
				PlatformData.Serialize(Ar, {.TargetPlatform = Asset::ECookTargetPlatform::Win64,
					.TargetProfile = Asset::ECookTargetProfile::Game});
				if (Ar.HasError()) { OutError = Ar.GetError(); return false; }
				OutValue = FBuildValue::FromOwned(std::string(Texture2DValueName), std::move(Bytes));
				return true;
			}
		};
		std::mutex GTextureFunctionMutex;
		FBuildFunctionRegistration GTextureFunctionRegistration;

		auto EnsureTexture2DBuildFunction(std::string* OutError,
			FModuleOwnedCallbackGate Gate = {}) -> bool
		{
			std::lock_guard Lock(GTextureFunctionMutex);
			if (GTextureFunctionRegistration.IsValid()) return true;
			GTextureFunctionRegistration = RegisterBuildFunction(Texture2DFunctionIdentity,
				std::make_shared<FTexture2DBuildFunction>(), std::move(Gate), OutError);
			return GTextureFunctionRegistration.IsValid();
		}
		}

	auto InitializeTexture2DBuildFunction(FModuleOwnedCallbackGate Gate,
		std::string* OutError) -> bool
	{
		return EnsureTexture2DBuildFunction(OutError, std::move(Gate));
	}

	auto ShutdownTexture2DBuildFunction() -> void
	{
		std::lock_guard Lock(GTextureFunctionMutex);
		GTextureFunctionRegistration.Reset();
	}

	auto BuildTexture2D(
		FTexture2DBuildRequest Request,
		FTexture2DBuildProduct& OutProduct,
		std::string& OutError,
		const FTexture2DBuildExecutionControl* ExecutionControl) -> bool
	{
		OutProduct = {};
		if (!EnsureTexture2DBuildFunction(&OutError)) return false;
		const FTexture2DBuildSettings& Settings = Request.Settings;
		if (!Request.SourceData.IsValid())
		{
			OutError = "Texture2D build requires valid normalized RGBA8 source data.";
			return false;
		}
		if (Request.SourceContentHashLow == 0 && Request.SourceContentHashHigh == 0)
		{
			OutError = "Texture2D build requires a captured source-content identity.";
			return false;
		}
		if (!TextureBuilder::IsValidUsage(Settings.Usage)
			|| !TextureBuilder::IsValidCompressionQuality(Settings.CompressionQuality)
			|| !TextureBuilder::IsValidAlphaMipMode(Settings.AlphaMipMode)
			|| !TextureBuilder::IsValidAlphaCoverageThreshold(Settings.AlphaCoverageThreshold))
		{
			OutError = "Texture2D build settings are invalid.";
			return false;
		}

		const bool bSRGB = Settings.bSRGB.value_or(
			TextureBuilder::GetDefaultSRGB(Settings.Usage));
		const FXxHash128 SourceHash{
			.HashLow = Request.SourceContentHashLow,
			.HashHigh = Request.SourceContentHashHigh};
		const FTexture2DBuildKeyInput KeyInput{
			.SourceContentHash = SourceHash,
			.Usage = Settings.Usage,
			.bSRGB = bSRGB,
			.CompressionQuality = Settings.CompressionQuality,
			.AlphaMipMode = Settings.AlphaMipMode,
			.MaximumResolution = Settings.MaxResolution,
			.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game};
		const std::vector<uint8> KeyBytes = BuildTexture2DDerivedDataKeyBytes(KeyInput);
		const std::string Key = BuildTexture2DDerivedDataKey(KeyInput);
		FBuildDefinition Definition;
		FBuildDefinitionBuilder DefinitionBuilder(
			Texture2DFunctionIdentity, std::string(Texture2DValueName));
		DefinitionBuilder.SetKey(FBuildKey::FromString(Key), KeyBytes)
			.AddTargetFact("Platform", "Win64")
			.AddTargetFact("Profile", "Game")
			.AddInput(FBuildValue::FromOwned(std::string(Texture2DInputName),
				EncodeTextureInput(Request, bSRGB)));
		if (!DefinitionBuilder.Build(Definition, &OutError)) return false;
		const FBuildCancellationToken Cancellation(
			ExecutionControl ? ExecutionControl->ShouldCancel : std::function<bool()>{});
		if (Request.bPersistDerivedData && ExecutionControl && ExecutionControl->OnPersisting)
			ExecutionControl->OnPersisting();
		const auto PersistenceStart = std::chrono::steady_clock::now();
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true,
			.bAllowLocalBuild = true,
			.bStoreBuildResult = Request.bPersistDerivedData,
			.bRequireStoreSuccess = Request.bPersistDerivedData,
			.bReturnData = true}, ExecutionControl ? &Cancellation : nullptr);
		if (ExecutionControl && ExecutionControl->Metrics && Request.bPersistDerivedData)
		{
			ExecutionControl->Metrics->PersistenceNanoseconds =
				static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - PersistenceStart).count());
		}
		if (!Output.Succeeded()) { OutError = Output.Diagnostic; return false; }
		FTexturePlatformData PlatformData;
		if (!DecodePlatformValue(Output.Value, PlatformData, OutError)) return false;
		if (ExecutionControl && ExecutionControl->Metrics)
			ExecutionControl->Metrics->PeakIntermediateBytes = std::max<uint64>(
				Request.SourceData.Pixels.size(), Output.Value.GetSize());

		OutProduct = {
			.SourceData = std::move(Request.SourceData),
			.PlatformData = std::move(PlatformData),
			.DerivedDataKey = Key,
			.SourceContentHashLow = SourceHash.HashLow,
			.SourceContentHashHigh = SourceHash.HashHigh,
			.Settings = Settings,
			.bSRGB = bSRGB};
		OutError.clear();
		return true;
	}

	auto PublishTexture2DProduct(
		DTexture2D& Texture,
		FTexture2DBuildProduct Product,
		const FTexture2DPublicationContext& Context,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Texture2D product publication requires a package.";
			return false;
		}
		if (!Product.SourceData.IsValid() || !Product.PlatformData.IsValid()
			|| Product.DerivedDataKey.empty())
		{
			OutError = "Texture2D product publication requires a complete detached product.";
			return false;
		}
		const PathUtilities::FSourcePathResult Resolved =
			PathUtilities::ResolveSourcePath(
				Context.SourcePath.Path, PathUtilities::EPathExistence::AllowMissing);
		if (!Resolved)
		{
			OutError = Resolved.Message;
			return false;
		}
		const PathUtilities::FMountPolicyResult Dependency =
			PathUtilities::CheckMountDependency(
				Texture.GetPackage()->GetPackagePath(), Resolved.NormalizedVirtualPath);
		if (!Dependency)
		{
			OutError = Dependency.Message;
			return false;
		}

		const FXxHash128 SourceHash{
			.HashLow = Product.SourceContentHashLow,
			.HashHigh = Product.SourceContentHashHigh};
		return Texture.PublishImportedState({
			.SourceImportData = {
				.Source = {
					.SourcePath = {.Path = Resolved.NormalizedVirtualPath},
					.SourceContentHashLow = SourceHash.HashLow,
					.SourceContentHashHigh = SourceHash.HashHigh},
				.DecoderId = Context.DecoderId,
				.DecoderVersion = Context.DecoderVersion},
			.SourceContentHash = SourceHash.ToString(),
			.SourceFileSize = Context.SourceFileSize,
			.SourceLastWriteTime = Context.SourceLastWriteTime,
			.SourceData = std::make_unique<FTextureSourceData>(std::move(Product.SourceData)),
			.PlatformData = std::make_unique<FTexturePlatformData>(std::move(Product.PlatformData)),
			.DerivedDataKey = std::move(Product.DerivedDataKey),
			.Usage = Product.Settings.Usage,
			.bSRGB = Product.bSRGB,
			.MaxResolution = Product.Settings.MaxResolution,
			.CompressionQuality = Product.Settings.CompressionQuality,
			.AlphaMipMode = Product.Settings.AlphaMipMode,
			.AlphaCoverageThreshold = Product.Settings.AlphaCoverageThreshold,
			.bMarkPackageDirty = Context.bMarkPackageDirty,
			.bReportLoadMutation = Context.bReportLoadMutation}, OutError);
	}

	auto MakeTexture2DDerivedDataKey(
		const DTexture2D& Texture,
		std::string& OutKey,
		std::string& OutError) -> bool
	{
		FXxHash128 SourceHash;
		if (Texture.GetSourceImportData().Source.HasContentHash())
		{
			const FTextureSourceFile& Source = Texture.GetSourceImportData().Source;
			SourceHash.HashLow = Source.SourceContentHashLow;
			SourceHash.HashHigh = Source.SourceContentHashHigh;
		}
		else if (IsCanonicalTextureHash(Texture.GetSourceContentHash()))
			SourceHash = FXxHash128::FromString(Texture.GetSourceContentHash());
		else
		{
			OutError = "Texture source content hash is missing or invalid.";
			return false;
		}
		OutKey = BuildTexture2DDerivedDataKey({
			.SourceContentHash = SourceHash,
			.Usage = Texture.GetUsage(),
			.bSRGB = Texture.IsSRGB(),
			.CompressionQuality = Texture.GetCompressionQuality(),
			.AlphaMipMode = Texture.GetAlphaMipMode(),
			.MaximumResolution = Texture.GetMaxResolution(),
			.AlphaCoverageThreshold = Texture.GetAlphaCoverageThreshold(),
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		OutError.clear();
		return true;
	}

	auto LoadTexture2DDerivedData(
		std::string_view Key,
		std::unique_ptr<FTexturePlatformData>& OutPlatformData,
		ETextureDerivedDataStatus& OutStatus,
		std::string& OutMessage) -> bool
	{
		std::string Error;
		if (!EnsureTexture2DBuildFunction(&Error))
		{
			OutStatus = ETextureDerivedDataStatus::Corrupt;
			OutMessage = Error;
			return false;
		}
		FBuildDefinition Definition;
		FBuildDefinitionBuilder Builder(Texture2DFunctionIdentity,
			std::string(Texture2DValueName));
		Builder.SetKey(FBuildKey::FromString(Key))
			.AddTargetFact("Platform", "Win64")
			.AddTargetFact("Profile", "Game");
		if (!Builder.Build(Definition, &Error))
		{
			OutStatus = ETextureDerivedDataStatus::Incompatible;
			OutMessage = Error;
			return false;
		}
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true, .bAllowLocalBuild = false,
			.bStoreBuildResult = false, .bReturnData = true});
		if (!Output.Succeeded())
		{
			OutStatus = Output.Status == EBuildStatus::CacheMiss
				? ETextureDerivedDataStatus::Missing : ETextureDerivedDataStatus::Corrupt;
			OutMessage = Output.Diagnostic;
			return false;
		}
		auto Candidate = std::make_unique<FTexturePlatformData>();
		if (!DecodePlatformValue(Output.Value, *Candidate, OutMessage))
		{
			OutStatus = ETextureDerivedDataStatus::Corrupt;
			return false;
		}
		OutPlatformData = std::move(Candidate);
		OutStatus = ETextureDerivedDataStatus::Hit;
		OutMessage.clear();
		return true;
	}
}
