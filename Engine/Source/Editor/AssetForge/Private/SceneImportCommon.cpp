#include "ImportedSceneInternal.h"

#include "AsyncImport.h"

namespace Durin::Asset::Forge::Private
{
	auto AddDiagnostic(
		FImportedSceneData& Scene,
		EImportDiagnosticSeverity Severity,
		ESceneImportDiagnosticCategory Category,
		std::string SourceIdentity,
		std::string Subject,
		std::string Message) -> bool
	{
		if (Scene.Diagnostics.size() < MaxImportDiagnostics)
		{
			Scene.Diagnostics.push_back({
				Severity, Category, std::move(SourceIdentity), std::move(Subject), std::move(Message)});
			return Severity != EImportDiagnosticSeverity::Error;
		}
		if (Scene.Diagnostics.size() == MaxImportDiagnostics)
		{
			Scene.Diagnostics.back() = {
				EImportDiagnosticSeverity::Error,
				ESceneImportDiagnosticCategory::ResourceLimitExceeded,
				"root",
				"diagnostics",
				"Import diagnostic limit exceeded."};
		}
		return false;
	}

	auto FailImport(
		FSceneDecodeResult& Result,
		ESceneImportDiagnosticCategory Category,
		std::string Subject,
		std::string Message,
		std::string SourceIdentity) -> bool
	{
		AddDiagnostic(Result.Scene, EImportDiagnosticSeverity::Error, Category,
			std::move(SourceIdentity), std::move(Subject), Message);
		Result.ErrorMessage = std::move(Message);
		Result.Scene.Images.clear();
		Result.Scene.Materials.clear();
		Result.Scene.MaterialSlots.clear();
		Result.Scene.Meshes.clear();
		return false;
	}

	auto CheckSceneDecodeCancellation(
		FSceneDecodeResult& Result,
		std::string_view Subject) -> bool
	{
		if (!IsImportCancellationRequested()) return false;
		(void)FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
			std::string(Subject), "Scene decoding was canceled.");
		return true;
	}

	auto ReadFileBytes(
		const std::filesystem::path& Path,
		uint64 Limit,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		std::error_code ErrorCode;
		const uint64 Size = std::filesystem::file_size(Path, ErrorCode);
		if (ErrorCode)
		{
			OutError = std::format("Could not read source file '{}'.", Path.generic_string());
			return false;
		}
		if (Size > Limit || Size > static_cast<uint64>(std::numeric_limits<size_t>::max()))
		{
			OutError = std::format("Source file '{}' exceeds the byte limit.", Path.generic_string());
			return false;
		}
		std::ifstream Stream(Path, std::ios::binary);
		if (!Stream)
		{
			OutError = std::format("Could not open source file '{}'.", Path.generic_string());
			return false;
		}
		OutBytes.resize(static_cast<size_t>(Size));
		constexpr size_t CancellationChunkBytes = 4ull * 1024ull * 1024ull;
		bool bRead = true;
		for (size_t Offset = 0; Offset < OutBytes.size();
			Offset += CancellationChunkBytes)
		{
			if (IsImportCancellationRequested())
			{
				OutError = "Scene source read was canceled.";
				OutBytes.clear();
				return false;
			}
			const size_t Count = std::min(
				CancellationChunkBytes, OutBytes.size() - Offset);
			Stream.read(reinterpret_cast<char*>(OutBytes.data() + Offset),
				static_cast<std::streamsize>(Count));
			bRead = Stream.gcount() == static_cast<std::streamsize>(Count);
			if (!bRead) break;
		}
		if (!bRead)
		{
			OutError = std::format("Could not read all bytes from '{}'.", Path.generic_string());
			OutBytes.clear();
			return false;
		}
		return true;
	}

	auto AppendDependency(
		FImportedSceneData& Scene,
		EImportedDependencyRole Role,
		std::string StableIdentity,
		std::string SourcePath,
		std::span<const uint8> Bytes,
		uint32* OutIndex) -> bool
	{
		if (Scene.Dependencies.size() >= MaxImportedDependencies) return false;
		if (OutIndex != nullptr) *OutIndex = static_cast<uint32>(Scene.Dependencies.size());
		Scene.Dependencies.push_back({
			Role,
			std::move(StableIdentity),
			FSourcePath{std::move(SourcePath)},
			FXxHash128::HashBuffer(Bytes),
			static_cast<uint64>(Bytes.size())});
		return true;
	}

	auto MakeDependencySourcePath(
		std::string_view RootSourcePath,
		std::string_view RelativeUri) -> std::string
	{
		if (RootSourcePath.empty()) return {};
		if (RelativeUri.empty()) return std::string(RootSourcePath);
		return (std::filesystem::path(RootSourcePath).parent_path() /
			std::filesystem::path(RelativeUri)).lexically_normal().generic_string();
	}

	auto IsValidSourcePath(std::string_view SourcePath) -> bool
	{
		if (SourcePath.empty()) return true;
		if (!SourcePath.starts_with('/') || SourcePath.starts_with("//") ||
			SourcePath.find('\\') != std::string_view::npos) return false;
		const std::filesystem::path Path(SourcePath);
		if (Path.lexically_normal().generic_string() != SourcePath) return false;
		for (const auto& Part : Path)
		{
			if (Part == "..") return false;
		}
		return true;
	}

	auto NormalizeRelativeUri(std::string Uri, std::string& OutNormalized) -> bool
	{
		std::ranges::replace(Uri, '\\', '/');
		if (Uri.empty() || Uri.starts_with('/') || Uri.starts_with("//") || Uri.find("://") != std::string::npos)
			return false;
		const std::filesystem::path Path(Uri);
		if (Path.has_root_name() || Path.is_absolute()) return false;
		const std::filesystem::path Normal = Path.lexically_normal();
		if (Normal.empty() || Normal == "." || Normal.begin() == Normal.end()) return false;
		for (const auto& Part : Normal)
		{
			if (Part == "..") return false;
		}
		OutNormalized = Normal.generic_string();
		return !OutNormalized.starts_with("../");
	}

	auto ResolveDependencyPath(
		const std::filesystem::path& RootFile,
		std::string_view Uri,
		std::filesystem::path& OutPath,
		std::string& OutNormalized) -> bool
	{
		if (!NormalizeRelativeUri(std::string(Uri), OutNormalized)) return false;
		std::error_code ErrorCode;
		const std::filesystem::path RootDirectory = std::filesystem::weakly_canonical(
			std::filesystem::absolute(RootFile).parent_path(), ErrorCode);
		if (ErrorCode) return false;
		const std::filesystem::path Candidate = std::filesystem::weakly_canonical(
			RootDirectory / std::filesystem::path(OutNormalized), ErrorCode);
		if (ErrorCode) return false;
		auto RootIt = RootDirectory.begin();
		auto CandidateIt = Candidate.begin();
		for (; RootIt != RootDirectory.end(); ++RootIt, ++CandidateIt)
		{
			if (CandidateIt == Candidate.end()) return false;
#if defined(_WIN32)
			std::string RootPart = RootIt->string();
			std::string CandidatePart = CandidateIt->string();
			std::ranges::transform(RootPart, RootPart.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			std::ranges::transform(CandidatePart, CandidatePart.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			if (RootPart != CandidatePart) return false;
#else
			if (*RootIt != *CandidateIt) return false;
#endif
		}
		OutPath = Candidate;
		return true;
	}

	auto EncodingFromMimeOrPath(
		std::string_view MimeType,
		std::string_view Path,
		EImportedImageEncoding& OutEncoding) -> bool
	{
		std::string Value = !MimeType.empty()
			? std::string(MimeType)
			: std::filesystem::path(Path).extension().string();
		std::ranges::transform(Value, Value.begin(), [](unsigned char Character) {
			return static_cast<char>(std::tolower(Character));
		});
		if (Value == "image/png" || Value == ".png") OutEncoding = EImportedImageEncoding::Png;
		else if (Value == "image/jpeg" || Value == ".jpg" || Value == ".jpeg") OutEncoding = EImportedImageEncoding::Jpeg;
		else if (Value == "image/bmp" || Value == ".bmp") OutEncoding = EImportedImageEncoding::Bmp;
		else if (Value == "image/tga" || Value == "image/x-tga" || Value == ".tga") OutEncoding = EImportedImageEncoding::Tga;
		else return false;
		return true;
	}

	auto ResolveImageEncoding(
		std::string_view MimeType,
		std::string_view Path,
		EImportedImageEncoding& OutEncoding) -> bool
	{
		EImportedImageEncoding MimeEncoding;
		EImportedImageEncoding PathEncoding;
		const bool bHasMime = !MimeType.empty();
		const bool bHasPath = !std::filesystem::path(Path).extension().empty();
		if (bHasMime && !EncodingFromMimeOrPath(MimeType, {}, MimeEncoding)) return false;
		if (bHasPath && !EncodingFromMimeOrPath({}, Path, PathEncoding)) return false;
		if (!bHasMime && !bHasPath) return false;
		if (bHasMime && bHasPath && MimeEncoding != PathEncoding) return false;
		OutEncoding = bHasMime ? MimeEncoding : PathEncoding;
		return true;
	}

	auto ValidateImageBytes(
		EImportedImageEncoding Encoding,
		std::span<const uint8> Bytes,
		std::string& OutError) -> bool
	{
		if (Bytes.size() > MaxImportedImageEncodedBytes)
		{
			OutError = "Encoded image exceeds the per-image byte limit.";
			return false;
		}
		uint64 Width = 0;
		uint64 Height = 0;
		switch (Encoding)
		{
		case EImportedImageEncoding::Png:
			if (Bytes.size() < 24 ||
				!std::equal(Bytes.begin(), Bytes.begin() + 8,
					std::array<uint8, 8>{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}.begin()))
			{
				OutError = "Declared PNG image does not have a valid PNG signature.";
				return false;
			}
			Width = (static_cast<uint32>(Bytes[16]) << 24) | (static_cast<uint32>(Bytes[17]) << 16)
				| (static_cast<uint32>(Bytes[18]) << 8) | Bytes[19];
			Height = (static_cast<uint32>(Bytes[20]) << 24) | (static_cast<uint32>(Bytes[21]) << 16)
				| (static_cast<uint32>(Bytes[22]) << 8) | Bytes[23];
			break;
		case EImportedImageEncoding::Bmp:
			if (Bytes.size() < 26 || Bytes[0] != 'B' || Bytes[1] != 'M')
			{
				OutError = "Declared BMP image does not have a valid BMP signature.";
				return false;
			}
			Width = static_cast<uint32>(Bytes[18]) | (static_cast<uint32>(Bytes[19]) << 8)
				| (static_cast<uint32>(Bytes[20]) << 16) | (static_cast<uint32>(Bytes[21]) << 24);
			{
				const uint32 RawHeight = static_cast<uint32>(Bytes[22]) | (static_cast<uint32>(Bytes[23]) << 8)
				| (static_cast<uint32>(Bytes[24]) << 16) | (static_cast<uint32>(Bytes[25]) << 24);
				const int32 SignedHeight = static_cast<int32>(RawHeight);
				if (SignedHeight == std::numeric_limits<int32>::min())
				{
					OutError = "BMP image height is invalid.";
					return false;
				}
				Height = static_cast<uint64>(std::abs(SignedHeight));
			}
			break;
		case EImportedImageEncoding::Jpeg:
			if (Bytes.size() < 4 || Bytes[0] != 0xff || Bytes[1] != 0xd8)
			{
				OutError = "Declared JPEG image does not have a valid JPEG signature.";
				return false;
			}
			for (size_t Offset = 2; Offset + 3 < Bytes.size();)
			{
				if (Bytes[Offset] != 0xff)
				{
					++Offset;
					continue;
				}
				while (Offset < Bytes.size() && Bytes[Offset] == 0xff) ++Offset;
				if (Offset >= Bytes.size()) break;
				const uint8 Marker = Bytes[Offset++];
				if (Marker == 0xd8 || Marker == 0xd9 || Marker == 0x01 ||
					(Marker >= 0xd0 && Marker <= 0xd7)) continue;
				if (Offset + 2 > Bytes.size()) break;
				const uint16 SegmentLength =
					(static_cast<uint16>(Bytes[Offset]) << 8) | Bytes[Offset + 1];
				if (SegmentLength < 2 || SegmentLength > Bytes.size() - Offset)
				{
					OutError = "JPEG image contains an invalid segment range.";
					return false;
				}
				const bool bStartOfFrame =
					(Marker >= 0xc0 && Marker <= 0xc3) ||
					(Marker >= 0xc5 && Marker <= 0xc7) ||
					(Marker >= 0xc9 && Marker <= 0xcb) ||
					(Marker >= 0xcd && Marker <= 0xcf);
				if (bStartOfFrame)
				{
					if (SegmentLength < 7)
					{
						OutError = "JPEG start-of-frame segment is truncated.";
						return false;
					}
					Height = (static_cast<uint16>(Bytes[Offset + 3]) << 8) | Bytes[Offset + 4];
					Width = (static_cast<uint16>(Bytes[Offset + 5]) << 8) | Bytes[Offset + 6];
					break;
				}
				Offset += SegmentLength;
			}
			if (Width == 0 || Height == 0)
			{
				OutError = "JPEG image dimensions could not be determined.";
				return false;
			}
			break;
		case EImportedImageEncoding::Tga:
			if (Bytes.size() < 18)
			{
				OutError = "Declared TGA image is truncated.";
				return false;
			}
			Width = static_cast<uint32>(Bytes[12]) | (static_cast<uint32>(Bytes[13]) << 8);
			Height = static_cast<uint32>(Bytes[14]) | (static_cast<uint32>(Bytes[15]) << 8);
			break;
		}
		if ((Width != 0 || Height != 0) &&
			(Width == 0 || Height == 0 || Width > MaxImportedTextureDimension ||
				Height > MaxImportedTextureDimension || Width * Height > MaxImportedDecodedPixels))
		{
			OutError = "Image dimensions exceed the imported texture limits.";
			return false;
		}
		return true;
	}

	auto MakeUniqueName(std::string Name, uint32 Index, std::unordered_map<std::string, uint32>& NameCounts) -> std::string
	{
		if (Name.empty()) Name = std::format("Material_{}", Index);
		uint32& Count = NameCounts[Name];
		const std::string Result = Count == 0 ? Name : std::format("{}_{}", Name, Count);
		++Count;
		return Result;
	}
}
