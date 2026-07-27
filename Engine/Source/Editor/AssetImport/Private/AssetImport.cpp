#include "AssetImport.h"

#include "Json/Json.h"
#include "Logging/LogMacros.h"
#include "Threading/Task.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/matrix3x3.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <fstream>

namespace Durin
{
	namespace Asset
	{
		struct FAsyncMeshImportSharedState
		{
			FTaskHandle Task;
			mutable std::mutex Mutex;
			std::optional<FAsyncMeshImportResult> Result;
		};

		namespace
		{
			constexpr float TransformDeterminantTolerance = 1.0e-8f;
			constexpr uint32 GlbMagic = 0x46546C67;
			constexpr uint32 GlbJsonChunk = 0x4E4F534A;
			constexpr uint32 GlbBinaryChunk = 0x004E4942;

			struct FGltfSource
			{
				FJsonDocument Document;
				std::vector<std::vector<uint8>> Buffers;
				std::vector<uint8> GlbBinaryChunkBytes;
				std::vector<uint32> ImageIndices;
			};

			auto AddDiagnostic(
				FImportedSceneData& Scene,
				EImportDiagnosticSeverity Severity,
				EImportDiagnosticCategory Category,
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
						EImportDiagnosticCategory::ResourceLimitExceeded,
						"root",
						"diagnostics",
						"Import diagnostic limit exceeded."};
				}
				return false;
			}

			auto FailImport(
				FAsyncMeshImportResult& Result,
				EImportDiagnosticCategory Category,
				std::string Subject,
				std::string Message,
				std::string SourceIdentity = "root") -> bool
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
				if (Size > 0) Stream.read(reinterpret_cast<char*>(OutBytes.data()), static_cast<std::streamsize>(Size));
				if (!Stream.good() && !Stream.eof())
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
				uint32* OutIndex = nullptr) -> bool
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

			auto ReadU32LittleEndian(std::span<const uint8> Bytes, size_t Offset, uint32& OutValue) -> bool
			{
				if (Offset > Bytes.size() || Bytes.size() - Offset < sizeof(uint32)) return false;
				OutValue =
					static_cast<uint32>(Bytes[Offset]) |
					(static_cast<uint32>(Bytes[Offset + 1]) << 8) |
					(static_cast<uint32>(Bytes[Offset + 2]) << 16) |
					(static_cast<uint32>(Bytes[Offset + 3]) << 24);
				return true;
			}

			auto DecodeBase64(
				std::string_view Text,
				uint64 MaxOutputBytes,
				std::vector<uint8>& OutBytes) -> bool
			{
				static constexpr std::string_view Alphabet =
					"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
				if (Text.size() % 4 != 0) return false;
				const size_t Padding = Text.ends_with("==") ? 2 : Text.ends_with("=") ? 1 : 0;
				const size_t OutputSize = Text.size() / 4 * 3 - Padding;
				if (OutputSize > MaxOutputBytes) return false;
				OutBytes.clear();
				OutBytes.reserve(OutputSize);
				uint32 Accumulator = 0;
				uint32 Bits = 0;
				for (const char Character : Text)
				{
					if (Character == '=') break;
					const size_t Value = Alphabet.find(Character);
					if (Value == std::string_view::npos) return false;
					Accumulator = (Accumulator << 6) | static_cast<uint32>(Value);
					Bits += 6;
					if (Bits >= 8)
					{
						Bits -= 8;
						OutBytes.push_back(static_cast<uint8>((Accumulator >> Bits) & 0xff));
					}
				}
				return OutBytes.size() == OutputSize;
			}

			auto DecodeDataUri(
				std::string_view Uri,
				uint64 MaxOutputBytes,
				std::string& OutMimeType,
				std::vector<uint8>& OutBytes) -> bool
			{
				if (!Uri.starts_with("data:")) return false;
				const size_t Comma = Uri.find(',');
				if (Comma == std::string_view::npos) return false;
				const std::string_view Metadata = Uri.substr(5, Comma - 5);
				if (!Metadata.ends_with(";base64")) return false;
				OutMimeType = std::string(Metadata.substr(0, Metadata.size() - 7));
				return DecodeBase64(Uri.substr(Comma + 1), MaxOutputBytes, OutBytes);
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

			auto ReadFiniteFloat(
				FJsonNodeView Object,
				std::string_view Key,
				float DefaultValue,
				float& OutValue) -> bool
			{
				const FJsonNodeView Node = Object.GetView(Key);
				if (!Node.IsValid())
				{
					OutValue = DefaultValue;
					return true;
				}
				if (!Node.IsNumber()) return false;
				const double Value = Node.GetDouble();
				if (!std::isfinite(Value) || Value < -std::numeric_limits<float>::max() ||
					Value > std::numeric_limits<float>::max()) return false;
				OutValue = static_cast<float>(Value);
				return true;
			}

			template<glm::length_t Length>
			auto ReadFiniteVector(
				FJsonNodeView Object,
				std::string_view Key,
				glm::vec<Length, float>& OutValue) -> bool
			{
				const FJsonNodeView Node = Object.GetView(Key);
				if (!Node.IsValid()) return true;
				if (!Node.IsArray() || Node.Num() != Length) return false;
				for (glm::length_t Index = 0; Index < Length; ++Index)
				{
					const FJsonNodeView ValueNode = Node.GetView(static_cast<size_t>(Index));
					if (!ValueNode.IsNumber()) return false;
					const double Value = ValueNode.GetDouble();
					if (!std::isfinite(Value) || Value < -std::numeric_limits<float>::max() ||
						Value > std::numeric_limits<float>::max()) return false;
					OutValue[Index] = static_cast<float>(Value);
				}
				return true;
			}

			auto LoadGltfDocument(
				std::span<const uint8> RootBytes,
				bool bGlb,
				FGltfSource& OutSource,
				std::string& OutError) -> bool
			{
				std::string_view JsonText;
				if (!bGlb)
				{
					JsonText = std::string_view(
						reinterpret_cast<const char*>(RootBytes.data()), RootBytes.size());
				}
				else
				{
					uint32 Magic = 0;
					uint32 Version = 0;
					uint32 DeclaredLength = 0;
					if (!ReadU32LittleEndian(RootBytes, 0, Magic) ||
						!ReadU32LittleEndian(RootBytes, 4, Version) ||
						!ReadU32LittleEndian(RootBytes, 8, DeclaredLength) ||
						Magic != GlbMagic || Version != 2 || DeclaredLength != RootBytes.size())
					{
						OutError = "GLB header is invalid.";
						return false;
					}
					size_t Offset = 12;
					bool bFoundJson = false;
					while (Offset < RootBytes.size())
					{
						uint32 ChunkLength = 0;
						uint32 ChunkType = 0;
						if (!ReadU32LittleEndian(RootBytes, Offset, ChunkLength) ||
							!ReadU32LittleEndian(RootBytes, Offset + 4, ChunkType) ||
							ChunkLength > RootBytes.size() - Offset - 8)
						{
							OutError = "GLB chunk range is invalid.";
							return false;
						}
						const std::span<const uint8> Chunk = RootBytes.subspan(Offset + 8, ChunkLength);
						if (ChunkType == GlbJsonChunk && !bFoundJson)
						{
							JsonText = std::string_view(reinterpret_cast<const char*>(Chunk.data()), Chunk.size());
							bFoundJson = true;
						}
						else if (ChunkType == GlbBinaryChunk && OutSource.GlbBinaryChunkBytes.empty())
						{
							OutSource.GlbBinaryChunkBytes.assign(Chunk.begin(), Chunk.end());
						}
						Offset += 8 + ChunkLength;
					}
					if (!bFoundJson)
					{
						OutError = "GLB does not contain a JSON chunk.";
						return false;
					}
				}
				FJsonParseError ParseError;
				if (!OutSource.Document.Parse(JsonText, &ParseError))
				{
					OutError = std::format("glTF JSON is invalid: {}", ParseError.Message);
					return false;
				}
				return OutSource.Document.GetRootView().IsObject();
			}

			auto IsSupportedGltfExtension(std::string_view Extension) -> bool
			{
				return Extension == "KHR_texture_transform";
			}

			auto ValidateGltfExtensions(FJsonNodeView Root, FAsyncMeshImportResult& Result) -> bool
			{
				std::unordered_set<std::string> Required;
				const FJsonNodeView RequiredNode = Root.GetView("extensionsRequired");
				if (RequiredNode.IsValid() && !RequiredNode.IsArray())
				{
					return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
						"extensionsRequired", "glTF extensionsRequired must be an array.");
				}
				for (size_t Index = 0; Index < RequiredNode.Num(); ++Index)
				{
					const FJsonNodeView ExtensionNode = RequiredNode.GetView(Index);
					if (!ExtensionNode.IsString())
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
							"extensionsRequired", "glTF extensionsRequired contains a non-string value.");
					}
					const std::string Extension = ExtensionNode.GetString();
					Required.insert(Extension);
					if (!IsSupportedGltfExtension(Extension))
					{
						return FailImport(Result, EImportDiagnosticCategory::UnsupportedRequiredExtension,
							Extension, std::format("Required glTF extension '{}' is unsupported.", Extension));
					}
				}
				const FJsonNodeView UsedNode = Root.GetView("extensionsUsed");
				if (UsedNode.IsValid() && !UsedNode.IsArray())
				{
					return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
						"extensionsUsed", "glTF extensionsUsed must be an array.");
				}
				for (size_t Index = 0; Index < UsedNode.Num(); ++Index)
				{
					const FJsonNodeView ExtensionNode = UsedNode.GetView(Index);
					if (!ExtensionNode.IsString())
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
							"extensionsUsed", "glTF extensionsUsed contains a non-string value.");
					}
					const std::string Extension = ExtensionNode.GetString();
					if (!IsSupportedGltfExtension(Extension) && !Required.contains(Extension))
					{
						if (!AddDiagnostic(Result.Scene, EImportDiagnosticSeverity::Warning,
							EImportDiagnosticCategory::UnsupportedOptionalExtension,
							"root", Extension,
							std::format("Optional glTF extension '{}' is unsupported; core fallback is used.", Extension)))
						{
							return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
								"diagnostics", "Import diagnostic limit exceeded.");
						}
					}
				}
				return true;
			}

			auto LoadGltfBuffers(
				FJsonNodeView Root,
				const std::filesystem::path& RootPath,
				std::string_view RootSourcePath,
				FGltfSource& Source,
				FAsyncMeshImportResult& Result) -> bool
			{
				const FJsonNodeView Buffers = Root.GetView("buffers");
				if (Buffers.IsValid() && !Buffers.IsArray())
				{
					return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
						"buffers", "glTF buffers must be an array.");
				}
				Source.Buffers.resize(Buffers.Num());
				for (size_t Index = 0; Index < Buffers.Num(); ++Index)
				{
					const FJsonNodeView Buffer = Buffers.GetView(Index);
					const uint64 DeclaredLength = Buffer.GetView("byteLength").GetUInt(std::numeric_limits<uint64>::max());
					if (!Buffer.IsObject() || DeclaredLength == std::numeric_limits<uint64>::max() ||
						DeclaredLength > MaxImportedSourceModelBytes)
					{
						return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
							std::format("buffer:{}", Index), "glTF buffer byte length is invalid or exceeds the limit.");
					}
					const FJsonNodeView UriNode = Buffer.GetView("uri");
					std::vector<uint8>& Bytes = Source.Buffers[Index];
					if (!UriNode.IsValid())
					{
						if (Index != 0 || Source.GlbBinaryChunkBytes.empty())
						{
							return FailImport(Result, EImportDiagnosticCategory::InvalidReference,
								std::format("buffer:{}", Index), "glTF buffer has no URI or GLB binary chunk.");
						}
						Bytes = Source.GlbBinaryChunkBytes;
					}
					else if (!UriNode.IsString())
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
							std::format("buffer:{}", Index), "glTF buffer URI must be a string.");
					}
					else
					{
						const std::string Uri = UriNode.GetString();
						if (Uri.starts_with("data:"))
						{
							std::string MimeType;
							if (!DecodeDataUri(Uri, MaxImportedSourceModelBytes, MimeType, Bytes))
							{
								return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
									std::format("buffer:{}", Index), "glTF buffer data URI is invalid.");
							}
						}
						else
						{
							std::filesystem::path DependencyPath;
							std::string NormalizedUri;
							if (!ResolveDependencyPath(RootPath, Uri, DependencyPath, NormalizedUri))
							{
								return FailImport(Result, EImportDiagnosticCategory::UnsafeDependencyPath,
									Uri, std::format("glTF buffer URI '{}' escapes the source directory.", Uri));
							}
							std::string Error;
							if (!ReadFileBytes(DependencyPath, MaxImportedSourceModelBytes, Bytes, Error))
							{
								return FailImport(Result, EImportDiagnosticCategory::MissingDependency,
									NormalizedUri, Error);
							}
							if (!AppendDependency(Result.Scene, EImportedDependencyRole::GeometryBuffer,
								std::format("buffer:{}", NormalizedUri),
								MakeDependencySourcePath(RootSourcePath, NormalizedUri), Bytes))
							{
								return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
									"dependencies", "Imported dependency count exceeds the limit.");
							}
						}
					}
					if (Bytes.size() < DeclaredLength)
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidReference,
							std::format("buffer:{}", Index), "glTF buffer is shorter than its declared byte length.");
					}
				}
				return true;
			}

			auto GetBufferViewBytes(
				FJsonNodeView Root,
				const FGltfSource& Source,
				uint32 BufferViewIndex,
				std::span<const uint8>& OutBytes) -> bool
			{
				const FJsonNodeView Views = Root.GetView("bufferViews");
				if (!Views.IsArray() || BufferViewIndex >= Views.Num()) return false;
				const FJsonNodeView View = Views.GetView(BufferViewIndex);
				const uint64 BufferIndex = View.GetView("buffer").GetUInt(std::numeric_limits<uint64>::max());
				const uint64 Offset = View.GetView("byteOffset").GetUInt(0);
				const uint64 Length = View.GetView("byteLength").GetUInt(std::numeric_limits<uint64>::max());
				if (BufferIndex >= Source.Buffers.size() || Length == std::numeric_limits<uint64>::max()) return false;
				const std::vector<uint8>& Buffer = Source.Buffers[static_cast<size_t>(BufferIndex)];
				if (Offset > Buffer.size() || Length > Buffer.size() - Offset) return false;
				OutBytes = std::span<const uint8>(Buffer).subspan(static_cast<size_t>(Offset), static_cast<size_t>(Length));
				return true;
			}

			auto ImportGltfImages(
				FJsonNodeView Root,
				const std::filesystem::path& RootPath,
				std::string_view RootSourcePath,
				FGltfSource& Source,
				FAsyncMeshImportResult& Result) -> bool
			{
				const FJsonNodeView Images = Root.GetView("images");
				if (Images.IsValid() && !Images.IsArray())
				{
					return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
						"images", "glTF images must be an array.");
				}
				if (Images.Num() > MaxImportedImages)
				{
					return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
						"images", "Imported image count exceeds the limit.");
				}
				uint64 EmbeddedByteCount = 0;
				std::unordered_map<std::string, uint32> ImportedImageIndices;
				Source.ImageIndices.resize(Images.Num());
				Result.Scene.Images.reserve(Images.Num());
				for (size_t Index = 0; Index < Images.Num(); ++Index)
				{
					const FJsonNodeView Image = Images.GetView(Index);
					if (!Image.IsObject())
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
							std::format("image:{}", Index), "glTF image entry must be an object.");
					}
					FImportedImage Imported;
					Imported.SuggestedName = Image.GetView("name").GetString(std::format("Image_{}", Index));
					const std::string DeclaredMime = Image.GetView("mimeType").GetString();
					std::span<const uint8> EncodedBytes;
					std::vector<uint8> OwnedBytes;
					const FJsonNodeView UriNode = Image.GetView("uri");
					if (UriNode.IsValid())
					{
						if (!UriNode.IsString())
						{
							return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
								std::format("image:{}", Index), "glTF image URI must be a string.");
						}
						const std::string Uri = UriNode.GetString();
						std::string EffectiveMime = DeclaredMime;
						if (Uri.starts_with("data:"))
						{
							std::string DataMime;
							if (!DecodeDataUri(Uri, MaxImportedImageEncodedBytes, DataMime, OwnedBytes) ||
								(!DeclaredMime.empty() && DeclaredMime != DataMime))
							{
								return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
									std::format("image:{}", Index), "glTF image data URI or declared MIME type is invalid.");
							}
							EffectiveMime = std::move(DataMime);
							Imported.StableIdentity = std::format("data-uri:{}", Index);
							EncodedBytes = OwnedBytes;
						}
						else
						{
							std::filesystem::path DependencyPath;
							std::string NormalizedUri;
							if (!ResolveDependencyPath(RootPath, Uri, DependencyPath, NormalizedUri))
							{
								return FailImport(Result, EImportDiagnosticCategory::UnsafeDependencyPath,
									Uri, std::format("glTF image URI '{}' escapes the source directory.", Uri));
							}
							Imported.StableIdentity = std::format("external:{}", NormalizedUri);
							if (!ResolveImageEncoding(EffectiveMime, NormalizedUri, Imported.Encoding))
							{
								return FailImport(Result, EImportDiagnosticCategory::UnsupportedEncoding,
									NormalizedUri, "External image MIME type and extension are unsupported or disagree.");
							}
							if (const auto Existing = ImportedImageIndices.find(Imported.StableIdentity);
								Existing != ImportedImageIndices.end())
							{
								Source.ImageIndices[Index] = Existing->second;
								continue;
							}
							std::string Error;
							if (!ReadFileBytes(DependencyPath, MaxImportedImageEncodedBytes, OwnedBytes, Error))
							{
								return FailImport(Result, EImportDiagnosticCategory::MissingDependency,
									NormalizedUri, Error);
							}
							uint32 DependencyIndex = 0;
							if (!AppendDependency(Result.Scene, EImportedDependencyRole::Image,
								std::format("image:{}", NormalizedUri),
								MakeDependencySourcePath(RootSourcePath, NormalizedUri), OwnedBytes, &DependencyIndex))
							{
								return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
									"dependencies", "Imported dependency count exceeds the limit.");
							}
							Imported.ExternalDependencyIndex = DependencyIndex;
							EncodedBytes = OwnedBytes;
						}
						if (!Imported.ExternalDependencyIndex.has_value() &&
							!ResolveImageEncoding(EffectiveMime, Uri, Imported.Encoding))
						{
							return FailImport(Result, EImportDiagnosticCategory::UnsupportedEncoding,
								Imported.StableIdentity, "Embedded image encoding is unsupported.");
						}
					}
					else
					{
						const uint64 BufferViewIndex = Image.GetView("bufferView").GetUInt(std::numeric_limits<uint64>::max());
						if (BufferViewIndex > std::numeric_limits<uint32>::max() ||
							!GetBufferViewBytes(Root, Source, static_cast<uint32>(BufferViewIndex), EncodedBytes))
						{
							return FailImport(Result, EImportDiagnosticCategory::InvalidReference,
								std::format("image:{}", Index), "glTF image buffer view is invalid.");
						}
						if (!ResolveImageEncoding(DeclaredMime, {}, Imported.Encoding))
						{
							return FailImport(Result, EImportDiagnosticCategory::UnsupportedEncoding,
								std::format("image:{}", Index), "Buffer-view image MIME type is unsupported.");
						}
						Imported.StableIdentity = std::format("glb-buffer-view:{}", BufferViewIndex);
						if (const auto Existing = ImportedImageIndices.find(Imported.StableIdentity);
							Existing != ImportedImageIndices.end())
						{
							if (Result.Scene.Images[Existing->second].Encoding != Imported.Encoding)
							{
								return FailImport(Result, EImportDiagnosticCategory::UnsupportedEncoding,
									Imported.StableIdentity, "Repeated buffer-view image MIME types disagree.");
							}
							Source.ImageIndices[Index] = Existing->second;
							continue;
						}
						OwnedBytes.assign(EncodedBytes.begin(), EncodedBytes.end());
					}
					std::string ImageError;
					if (!ValidateImageBytes(Imported.Encoding, EncodedBytes, ImageError))
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
							Imported.StableIdentity, ImageError);
					}
					Imported.EncodedByteCount = EncodedBytes.size();
					if (!Imported.ExternalDependencyIndex.has_value())
					{
						EmbeddedByteCount += Imported.EncodedByteCount;
						if (EmbeddedByteCount > MaxImportedEmbeddedImageBytes)
						{
							return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
								"embedded-images", "Embedded image bytes exceed the aggregate limit.");
						}
						Imported.EmbeddedEncodedBytes = std::move(OwnedBytes);
					}
					const uint32 ImportedIndex = static_cast<uint32>(Result.Scene.Images.size());
					Source.ImageIndices[Index] = ImportedIndex;
					ImportedImageIndices.emplace(Imported.StableIdentity, ImportedIndex);
					Result.Scene.Images.push_back(std::move(Imported));
				}
				return true;
			}

			auto ParseSamplerFilter(int64 Value, bool bMinification, EImportedSamplerFilter& OutFilter) -> bool
			{
				switch (Value)
				{
				case 9728: OutFilter = EImportedSamplerFilter::Nearest; return true;
				case 9729: OutFilter = EImportedSamplerFilter::Linear; return true;
				case 9984: if (bMinification) { OutFilter = EImportedSamplerFilter::NearestMipmapNearest; return true; } break;
				case 9985: if (bMinification) { OutFilter = EImportedSamplerFilter::LinearMipmapNearest; return true; } break;
				case 9986: if (bMinification) { OutFilter = EImportedSamplerFilter::NearestMipmapLinear; return true; } break;
				case 9987: if (bMinification) { OutFilter = EImportedSamplerFilter::LinearMipmapLinear; return true; } break;
				default: break;
				}
				return false;
			}

			auto ParseSamplerWrap(int64 Value, EImportedSamplerWrap& OutWrap) -> bool
			{
				switch (Value)
				{
				case 10497: OutWrap = EImportedSamplerWrap::Repeat; return true;
				case 33648: OutWrap = EImportedSamplerWrap::MirroredRepeat; return true;
				case 33071: OutWrap = EImportedSamplerWrap::ClampToEdge; return true;
				default: return false;
				}
			}

			auto ParseGltfTextureBinding(
				FJsonNodeView Root,
				std::span<const uint32> SourceImageIndices,
				FJsonNodeView TextureInfo,
				EImportedTextureSemantic Semantic,
				float DefaultStrength,
				FImportedTextureBinding& OutBinding,
				FAsyncMeshImportResult& Result,
				std::string_view Subject) -> bool
			{
				if (!TextureInfo.IsObject())
				{
					return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
						std::string(Subject), "glTF texture info must be an object.");
				}
				const uint64 TextureIndex = TextureInfo.GetView("index").GetUInt(std::numeric_limits<uint64>::max());
				const FJsonNodeView Textures = Root.GetView("textures");
				if (!Textures.IsArray() || TextureIndex >= Textures.Num())
				{
					return FailImport(Result, EImportDiagnosticCategory::InvalidReference,
						std::string(Subject), "glTF texture index is invalid.");
				}
				const FJsonNodeView Texture = Textures.GetView(static_cast<size_t>(TextureIndex));
				const uint64 ImageIndex = Texture.GetView("source").GetUInt(std::numeric_limits<uint64>::max());
				if (ImageIndex >= SourceImageIndices.size() ||
					SourceImageIndices[static_cast<size_t>(ImageIndex)] >= Result.Scene.Images.size())
				{
					return FailImport(Result, EImportDiagnosticCategory::InvalidReference,
						std::string(Subject), "glTF texture source image index is invalid.");
				}
				OutBinding.Semantic = Semantic;
				OutBinding.ImageIndex = SourceImageIndices[static_cast<size_t>(ImageIndex)];
				OutBinding.Strength = DefaultStrength;
				const uint64 UVChannel = TextureInfo.GetView("texCoord").GetUInt(0);
				if (UVChannel >= MaxImportedUVChannels)
				{
					return FailImport(Result, EImportDiagnosticCategory::InvalidReference,
						std::string(Subject), "glTF texture UV channel exceeds the imported UV limit.");
				}
				OutBinding.UVChannel = static_cast<uint32>(UVChannel);

				const FJsonNodeView Transform =
					TextureInfo.GetView("extensions").GetView("KHR_texture_transform");
				if (Transform.IsValid())
				{
					if (!Transform.IsObject() ||
						!ReadFiniteVector(Transform, "offset", OutBinding.Offset) ||
						!ReadFiniteVector(Transform, "scale", OutBinding.Scale) ||
						!ReadFiniteFloat(Transform, "rotation", 0.0f, OutBinding.RotationRadians))
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
							std::string(Subject), "KHR_texture_transform contains an invalid value.");
					}
					if (Transform.Contains("texCoord"))
					{
						const uint64 TransformedUVChannel = Transform.GetView("texCoord").GetUInt(
							std::numeric_limits<uint64>::max());
						if (TransformedUVChannel >= MaxImportedUVChannels)
						{
							return FailImport(Result, EImportDiagnosticCategory::InvalidReference,
								std::string(Subject), "KHR_texture_transform UV channel is invalid.");
						}
						OutBinding.UVChannel = static_cast<uint32>(TransformedUVChannel);
					}
				}

				const FJsonNodeView Samplers = Root.GetView("samplers");
				const FJsonNodeView SamplerIndexNode = Texture.GetView("sampler");
				if (SamplerIndexNode.IsValid())
				{
					const uint64 SamplerIndex = SamplerIndexNode.GetUInt(std::numeric_limits<uint64>::max());
					if (!Samplers.IsArray() || SamplerIndex >= Samplers.Num())
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidReference,
							std::string(Subject), "glTF sampler index is invalid.");
					}
					const FJsonNodeView Sampler = Samplers.GetView(static_cast<size_t>(SamplerIndex));
					if (!Sampler.IsObject())
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
							std::string(Subject), "glTF sampler must be an object.");
					}
					if (Sampler.Contains("minFilter") &&
						!ParseSamplerFilter(Sampler.GetView("minFilter").GetInt(-1), true, OutBinding.Sampler.MinFilter))
					{
						return FailImport(Result, EImportDiagnosticCategory::UnsupportedSampler,
							std::string(Subject), "glTF minification filter is unsupported.");
					}
					if (Sampler.Contains("magFilter") &&
						!ParseSamplerFilter(Sampler.GetView("magFilter").GetInt(-1), false, OutBinding.Sampler.MagFilter))
					{
						return FailImport(Result, EImportDiagnosticCategory::UnsupportedSampler,
							std::string(Subject), "glTF magnification filter is unsupported.");
					}
					if (Sampler.Contains("wrapS") &&
						!ParseSamplerWrap(Sampler.GetView("wrapS").GetInt(-1), OutBinding.Sampler.WrapU))
					{
						return FailImport(Result, EImportDiagnosticCategory::UnsupportedSampler,
							std::string(Subject), "glTF U wrapping mode is unsupported.");
					}
					if (Sampler.Contains("wrapT") &&
						!ParseSamplerWrap(Sampler.GetView("wrapT").GetInt(-1), OutBinding.Sampler.WrapV))
					{
						return FailImport(Result, EImportDiagnosticCategory::UnsupportedSampler,
							std::string(Subject), "glTF V wrapping mode is unsupported.");
					}
				}
				return true;
			}

			auto AddGltfBinding(
				FJsonNodeView Root,
				std::span<const uint32> SourceImageIndices,
				FJsonNodeView TextureInfo,
				EImportedTextureSemantic Semantic,
				float Strength,
				FImportedMaterial& Material,
				FAsyncMeshImportResult& Result,
				std::string_view Subject) -> bool
			{
				if (!TextureInfo.IsValid()) return true;
				if (Material.TextureBindings.size() >= MaxImportedTextureBindingsPerMaterial)
				{
					return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
						std::string(Subject), "Material texture binding count exceeds the limit.");
				}
				FImportedTextureBinding Binding;
				if (!ParseGltfTextureBinding(
					Root, SourceImageIndices, TextureInfo, Semantic, Strength, Binding, Result, Subject))
					return false;
				Material.TextureBindings.push_back(std::move(Binding));
				return true;
			}

			auto ImportGltfMaterials(
				FJsonNodeView Root,
				std::span<const uint32> SourceImageIndices,
				FAsyncMeshImportResult& Result) -> bool
			{
				const FJsonNodeView Materials = Root.GetView("materials");
				if (Materials.IsValid() && !Materials.IsArray())
				{
					return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
						"materials", "glTF materials must be an array.");
				}
				if (Materials.Num() > MaxImportedSourceMaterials)
				{
					return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
						"materials", "Source material count exceeds the limit.");
				}
				Result.Scene.Materials.reserve(Materials.Num());
				for (size_t Index = 0; Index < Materials.Num(); ++Index)
				{
					const FJsonNodeView MaterialNode = Materials.GetView(Index);
					if (!MaterialNode.IsObject())
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
							std::format("material:{}", Index), "glTF material must be an object.");
					}
					FImportedMaterial Material;
					Material.SourceMaterialIndex = static_cast<uint32>(Index);
					Material.SourceName = MaterialNode.GetView("name").GetString();
					const FJsonNodeView Pbr = MaterialNode.GetView("pbrMetallicRoughness");
					if (Pbr.IsValid() && !Pbr.IsObject())
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
							std::format("material:{}", Index), "pbrMetallicRoughness must be an object.");
					}
					if (!ReadFiniteVector(Pbr, "baseColorFactor", Material.BaseColorFactor) ||
						!ReadFiniteFloat(Pbr, "metallicFactor", 1.0f, Material.MetallicFactor) ||
						!ReadFiniteFloat(Pbr, "roughnessFactor", 1.0f, Material.RoughnessFactor) ||
						!ReadFiniteVector(MaterialNode, "emissiveFactor", Material.EmissiveFactor) ||
						!ReadFiniteFloat(MaterialNode, "alphaCutoff", 0.5f, Material.AlphaCutoff))
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
							std::format("material:{}", Index), "glTF material contains a non-finite or malformed factor.");
					}
					const std::string AlphaMode = MaterialNode.GetView("alphaMode").GetString("OPAQUE");
					if (AlphaMode == "OPAQUE") Material.AlphaMode = EImportedAlphaMode::Opaque;
					else if (AlphaMode == "MASK") Material.AlphaMode = EImportedAlphaMode::Mask;
					else if (AlphaMode == "BLEND") Material.AlphaMode = EImportedAlphaMode::Blend;
					else
					{
						return FailImport(Result, EImportDiagnosticCategory::UnsupportedAlphaMode,
							std::format("material:{}", Index), std::format("glTF alpha mode '{}' is unsupported.", AlphaMode));
					}
					Material.bDoubleSided = MaterialNode.GetView("doubleSided").GetBool(false);

					float NormalScale = 1.0f;
					float OcclusionStrength = 1.0f;
					if (!ReadFiniteFloat(MaterialNode.GetView("normalTexture"), "scale", 1.0f, NormalScale) ||
						!ReadFiniteFloat(MaterialNode.GetView("occlusionTexture"), "strength", 1.0f, OcclusionStrength))
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
							std::format("material:{}", Index), "glTF texture strength is non-finite.");
					}
					if (!AddGltfBinding(Root, SourceImageIndices,
							Pbr.GetView("baseColorTexture"), EImportedTextureSemantic::BaseColor,
							1.0f, Material, Result, std::format("material:{}:baseColorTexture", Index)) ||
						!AddGltfBinding(Root, SourceImageIndices,
							Pbr.GetView("metallicRoughnessTexture"), EImportedTextureSemantic::MetallicRoughness,
							1.0f, Material, Result, std::format("material:{}:metallicRoughnessTexture", Index)) ||
						!AddGltfBinding(Root, SourceImageIndices,
							MaterialNode.GetView("normalTexture"), EImportedTextureSemantic::Normal,
							NormalScale, Material, Result, std::format("material:{}:normalTexture", Index)) ||
						!AddGltfBinding(Root, SourceImageIndices,
							MaterialNode.GetView("occlusionTexture"), EImportedTextureSemantic::Occlusion,
							OcclusionStrength, Material, Result, std::format("material:{}:occlusionTexture", Index)) ||
						!AddGltfBinding(Root, SourceImageIndices,
							MaterialNode.GetView("emissiveTexture"), EImportedTextureSemantic::Emissive,
							1.0f, Material, Result, std::format("material:{}:emissiveTexture", Index)))
					{
						return false;
					}
					Result.Scene.Materials.push_back(std::move(Material));
				}
				return true;
			}

			auto ImportGltfMetadata(
				const std::filesystem::path& RootPath,
				std::string_view RootSourcePath,
				std::span<const uint8> RootBytes,
				bool bGlb,
				FAsyncMeshImportResult& Result,
				std::vector<uint32>& OutMeshMaterialIndices) -> bool
			{
				FGltfSource Source;
				std::string Error;
				if (!LoadGltfDocument(RootBytes, bGlb, Source, Error))
					return FailImport(Result, EImportDiagnosticCategory::InvalidValue, "root", Error);
				const FJsonNodeView Root = Source.Document.GetRootView();
				if (!(ValidateGltfExtensions(Root, Result) &&
					LoadGltfBuffers(Root, RootPath, RootSourcePath, Source, Result) &&
					ImportGltfImages(Root, RootPath, RootSourcePath, Source, Result) &&
					ImportGltfMaterials(Root, Source.ImageIndices, Result))) return false;
				const FJsonNodeView Meshes = Root.GetView("meshes");
				for (size_t MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
				{
					const FJsonNodeView Primitives = Meshes.GetView(MeshIndex).GetView("primitives");
					if (!Primitives.IsArray())
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
							std::format("mesh:{}", MeshIndex), "glTF mesh primitives must be an array.");
					}
					for (size_t PrimitiveIndex = 0; PrimitiveIndex < Primitives.Num(); ++PrimitiveIndex)
					{
						const uint64 MaterialIndex = Primitives.GetView(PrimitiveIndex).GetView("material").GetUInt(0);
						if (MaterialIndex >= Result.Scene.Materials.size())
						{
							return FailImport(Result, EImportDiagnosticCategory::InvalidReference,
								std::format("mesh:{}:primitive:{}", MeshIndex, PrimitiveIndex),
								"glTF primitive material index is invalid.");
						}
						OutMeshMaterialIndices.push_back(static_cast<uint32>(MaterialIndex));
					}
				}
				return true;
			}

			auto ToAssimpMatrix(const glm::mat4& Matrix) -> aiMatrix4x4
			{
				// GLM stores columns while Assimp's constructor is expressed as rows.
				return {
					Matrix[0][0], Matrix[1][0], Matrix[2][0], Matrix[3][0],
					Matrix[0][1], Matrix[1][1], Matrix[2][1], Matrix[3][1],
					Matrix[0][2], Matrix[1][2], Matrix[2][2], Matrix[3][2],
					Matrix[0][3], Matrix[1][3], Matrix[2][3], Matrix[3][3]
				};
			}

			auto ToVector3(const aiVector3D& Value) -> FVector3f
			{
				return {Value.x, Value.y, Value.z};
			}

			auto IsFinite(const aiVector3D& Value) -> bool
			{
				return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
			}

			auto MakeUniqueName(std::string Name, uint32 Index, std::unordered_map<std::string, uint32>& NameCounts) -> std::string
			{
				if (Name.empty()) Name = std::format("Material_{}", Index);
				uint32& Count = NameCounts[Name];
				const std::string Result = Count == 0 ? Name : std::format("{}_{}", Name, Count);
				++Count;
				return Result;
			}

			auto ImportAssimpImage(
				const aiScene& Scene,
				const std::filesystem::path& RootPath,
				std::string_view RootSourcePath,
				std::string TexturePath,
				FAsyncMeshImportResult& Result,
				std::unordered_map<std::string, uint32>& ImageIndices,
				uint64& EmbeddedByteCount,
				uint32& OutImageIndex) -> bool
			{
				std::ranges::replace(TexturePath, '\\', '/');
				if (const auto Existing = ImageIndices.find(TexturePath); Existing != ImageIndices.end())
				{
					OutImageIndex = Existing->second;
					return true;
				}
				if (Result.Scene.Images.size() >= MaxImportedImages)
				{
					return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
						"images", "Imported image count exceeds the limit.");
				}

				FImportedImage Imported;
				std::span<const uint8> EncodedBytes;
				std::vector<uint8> OwnedBytes;
				std::string EncodingHint;
				if (const aiTexture* Embedded = Scene.GetEmbeddedTexture(TexturePath.c_str()))
				{
					if (Embedded->mHeight != 0)
					{
						return FailImport(Result, EImportDiagnosticCategory::UnsupportedEncoding,
							TexturePath, "Uncompressed Assimp embedded textures are not accepted as encoded image sources.");
					}
					if (Embedded->mWidth > MaxImportedImageEncodedBytes)
					{
						return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
							TexturePath, "Embedded image exceeds the per-image byte limit.");
					}
					OwnedBytes.assign(
						reinterpret_cast<const uint8*>(Embedded->pcData),
						reinterpret_cast<const uint8*>(Embedded->pcData) + Embedded->mWidth);
					EncodedBytes = OwnedBytes;
					EmbeddedByteCount += OwnedBytes.size();
					if (EmbeddedByteCount > MaxImportedEmbeddedImageBytes)
					{
						return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
							TexturePath, "Embedded image bytes exceed the aggregate limit.");
					}
					EncodingHint = Embedded->achFormatHint;
					if (!EncodingHint.empty() && EncodingHint.front() != '.')
						EncodingHint.insert(EncodingHint.begin(), '.');
					Imported.StableIdentity = std::format("assimp-embedded:{}", TexturePath);
					Imported.SuggestedName = std::format("Embedded_{}", Result.Scene.Images.size());
				}
				else
				{
					std::filesystem::path DependencyPath;
					std::string NormalizedUri;
					if (!ResolveDependencyPath(RootPath, TexturePath, DependencyPath, NormalizedUri))
					{
						return FailImport(Result, EImportDiagnosticCategory::UnsafeDependencyPath,
							TexturePath, std::format("Material texture path '{}' escapes the source directory.", TexturePath));
					}
					std::string Error;
					if (!ReadFileBytes(DependencyPath, MaxImportedImageEncodedBytes, OwnedBytes, Error))
					{
						return FailImport(Result, EImportDiagnosticCategory::MissingDependency,
							NormalizedUri, Error);
					}
					uint32 DependencyIndex = 0;
					if (!AppendDependency(Result.Scene, EImportedDependencyRole::Image,
						std::format("image:{}", NormalizedUri),
						MakeDependencySourcePath(RootSourcePath, NormalizedUri), OwnedBytes, &DependencyIndex))
					{
						return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
							"dependencies", "Imported dependency count exceeds the limit.");
					}
					Imported.StableIdentity = std::format("external:{}", NormalizedUri);
					Imported.SuggestedName = DependencyPath.stem().string();
					Imported.ExternalDependencyIndex = DependencyIndex;
					EncodingHint = NormalizedUri;
					EncodedBytes = OwnedBytes;
				}
				if (!EncodingFromMimeOrPath({}, EncodingHint, Imported.Encoding))
				{
					return FailImport(Result, EImportDiagnosticCategory::UnsupportedEncoding,
						TexturePath, "Material image encoding is unsupported.");
				}
				std::string ImageError;
				if (!ValidateImageBytes(Imported.Encoding, EncodedBytes, ImageError))
				{
					return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
						TexturePath, ImageError);
				}
				Imported.EncodedByteCount = EncodedBytes.size();
				if (!Imported.ExternalDependencyIndex.has_value())
					Imported.EmbeddedEncodedBytes = std::move(OwnedBytes);
				OutImageIndex = static_cast<uint32>(Result.Scene.Images.size());
				ImageIndices.emplace(TexturePath, OutImageIndex);
				Result.Scene.Images.push_back(std::move(Imported));
				return true;
			}

			auto ImportAssimpMaterials(
				const aiScene& Scene,
				const std::filesystem::path& RootPath,
				std::string_view RootSourcePath,
				std::span<const uint8> RootBytes,
				FAsyncMeshImportResult& Result) -> bool
			{
				if (Scene.mNumMaterials > MaxImportedSourceMaterials)
				{
					return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
						"materials", "Source material count exceeds the limit.");
				}
				const std::string_view RootText(
					reinterpret_cast<const char*>(RootBytes.data()), RootBytes.size());
				if (RootText.find("3dsMax|main") != std::string_view::npos)
				{
					if (!AddDiagnostic(Result.Scene, EImportDiagnosticSeverity::Warning,
						EImportDiagnosticCategory::UnsupportedMaterialProperty,
						"root", "3dsMax|main",
						"DCC-specific 3ds Max material properties use the Lambert fallback."))
					{
						return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
							"diagnostics", "Import diagnostic limit exceeded.");
					}
				}
				std::unordered_map<std::string, uint32> ImageIndices;
				uint64 EmbeddedByteCount = 0;
				for (uint32 TextureIndex = 0; TextureIndex < Scene.mNumTextures; ++TextureIndex)
				{
					uint32 ImportedImageIndex = 0;
					if (!ImportAssimpImage(Scene, RootPath, RootSourcePath,
						std::format("*{}", TextureIndex), Result, ImageIndices,
						EmbeddedByteCount, ImportedImageIndex)) return false;
				}
				Result.Scene.Materials.reserve(Scene.mNumMaterials);
				for (uint32 MaterialIndex = 0; MaterialIndex < Scene.mNumMaterials; ++MaterialIndex)
				{
					const aiMaterial* SourceMaterial = Scene.mMaterials[MaterialIndex];
					if (SourceMaterial == nullptr)
					{
						return FailImport(Result, EImportDiagnosticCategory::InvalidReference,
							std::format("material:{}", MaterialIndex), "Assimp returned a null material.");
					}
					FImportedMaterial Material;
					Material.SourceMaterialIndex = MaterialIndex;
					aiString Name;
					SourceMaterial->Get(AI_MATKEY_NAME, Name);
					Material.SourceName = Name.C_Str();

					aiColor4D Diffuse(1.0f, 1.0f, 1.0f, 1.0f);
					if (SourceMaterial->Get(AI_MATKEY_COLOR_DIFFUSE, Diffuse) == aiReturn_SUCCESS)
					{
						if (!std::isfinite(Diffuse.r) || !std::isfinite(Diffuse.g) ||
							!std::isfinite(Diffuse.b) || !std::isfinite(Diffuse.a))
						{
							return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
								std::format("material:{}", MaterialIndex), "Material diffuse color is non-finite.");
						}
						Material.BaseColorFactor = {Diffuse.r, Diffuse.g, Diffuse.b, Diffuse.a};
					}
					float Opacity = 1.0f;
					if (SourceMaterial->Get(AI_MATKEY_OPACITY, Opacity) == aiReturn_SUCCESS)
					{
						if (!std::isfinite(Opacity))
						{
							return FailImport(Result, EImportDiagnosticCategory::InvalidValue,
								std::format("material:{}", MaterialIndex), "Material opacity is non-finite.");
						}
						Material.BaseColorFactor.a *= Opacity;
					}

					int ShadingModel = aiShadingMode_NoShading;
					if (SourceMaterial->Get(AI_MATKEY_SHADING_MODEL, ShadingModel) == aiReturn_SUCCESS &&
						(ShadingModel == aiShadingMode_Phong || ShadingModel == aiShadingMode_Blinn))
					{
						if (!AddDiagnostic(Result.Scene, EImportDiagnosticSeverity::Warning,
							EImportDiagnosticCategory::UnsupportedMaterialProperty,
							std::format("material:{}", MaterialIndex), "Phong",
							"Phong/specular properties are not converted to metallic/roughness values."))
						{
							return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
								"diagnostics", "Import diagnostic limit exceeded.");
						}
					}

					aiTextureType TextureType = aiTextureType_NONE;
					if (SourceMaterial->GetTextureCount(aiTextureType_BASE_COLOR) == 1)
						TextureType = aiTextureType_BASE_COLOR;
					else if (SourceMaterial->GetTextureCount(aiTextureType_DIFFUSE) == 1)
						TextureType = aiTextureType_DIFFUSE;
					else if (SourceMaterial->GetTextureCount(aiTextureType_BASE_COLOR) > 1 ||
						SourceMaterial->GetTextureCount(aiTextureType_DIFFUSE) > 1)
					{
						if (!AddDiagnostic(Result.Scene, EImportDiagnosticSeverity::Warning,
							EImportDiagnosticCategory::UnsupportedMaterialProperty,
							std::format("material:{}", MaterialIndex), "layered-diffuse-texture",
							"Layered diffuse textures are not mapped to base color."))
						{
							return FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
								"diagnostics", "Import diagnostic limit exceeded.");
						}
					}
					if (TextureType != aiTextureType_NONE)
					{
						aiString TexturePath;
						unsigned int UVChannel = 0;
						if (SourceMaterial->GetTexture(TextureType, 0, &TexturePath, nullptr, &UVChannel) != aiReturn_SUCCESS)
						{
							return FailImport(Result, EImportDiagnosticCategory::InvalidReference,
								std::format("material:{}", MaterialIndex), "Assimp material texture reference is invalid.");
						}
						if (UVChannel >= MaxImportedUVChannels)
						{
							return FailImport(Result, EImportDiagnosticCategory::InvalidReference,
								TexturePath.C_Str(), "Material texture UV channel exceeds the imported UV limit.");
						}
						FImportedTextureBinding Binding;
						Binding.Semantic = EImportedTextureSemantic::BaseColor;
						Binding.UVChannel = UVChannel;
						if (!ImportAssimpImage(Scene, RootPath, RootSourcePath,
							TexturePath.C_Str(), Result, ImageIndices,
							EmbeddedByteCount, Binding.ImageIndex))
							return false;
						Material.TextureBindings.push_back(Binding);
					}
					Result.Scene.Materials.push_back(std::move(Material));
				}
				return true;
			}

			auto ImportMeshInstance(
				const aiScene& Scene,
				const aiNode& Node,
				uint32 MeshIndex,
				std::span<const uint32> SourceMaterialIndices,
				const aiMatrix4x4& Transform,
				FImportedSceneData& OutScene,
				std::string& OutError) -> bool
			{
				if (MeshIndex >= Scene.mNumMeshes || Scene.mMeshes[MeshIndex] == nullptr)
				{
					OutError = std::format("Scene node '{}' references invalid mesh index {}.", Node.mName.C_Str(), MeshIndex);
					return false;
				}

				const aiMesh& Mesh = *Scene.mMeshes[MeshIndex];
				const aiMatrix3x3 LinearTransform(Transform);
				const float Determinant = LinearTransform.Determinant();
				if (!std::isfinite(Determinant) || std::abs(Determinant) <= TransformDeterminantTolerance)
				{
					OutError = std::format("Mesh '{}' is under a singular node transform.", Mesh.mName.C_Str());
					return false;
				}

				aiMatrix3x3 NormalTransform(LinearTransform);
				NormalTransform.Inverse().Transpose();
				const bool bMirrored = Determinant < 0.0f;

				FImportedMeshData OutMesh;
				OutMesh.Name = Mesh.mName.length > 0 ? Mesh.mName.C_Str() : Node.mName.C_Str();
				if (OutMesh.Name.empty()) OutMesh.Name = std::format("Mesh_{}", MeshIndex);
				OutMesh.SourceMaterialIndex = SourceMaterialIndices.empty()
					? Mesh.mMaterialIndex
					: SourceMaterialIndices[MeshIndex];
				if (Mesh.GetNumUVChannels() > MaxImportedUVChannels)
				{
					DURIN_WARN("Mesh '{}' has {} UV channels; only the first {} are imported.", OutMesh.Name, Mesh.GetNumUVChannels(), MaxImportedUVChannels);
				}
				OutMesh.Positions.reserve(Mesh.mNumVertices);
				if (Mesh.HasNormals()) OutMesh.Normals.reserve(Mesh.mNumVertices);
				if (Mesh.HasTangentsAndBitangents()) OutMesh.Tangents.reserve(Mesh.mNumVertices);
				if (Mesh.HasVertexColors(0)) OutMesh.Colors.reserve(Mesh.mNumVertices);
				for (uint32 Channel = 0; Channel < MaxImportedUVChannels; ++Channel)
				{
					if (Mesh.HasTextureCoords(Channel)) OutMesh.UVChannels[Channel].reserve(Mesh.mNumVertices);
				}

				for (unsigned int VertexIndex = 0; VertexIndex < Mesh.mNumVertices; ++VertexIndex)
				{
					const aiVector3D Position = Transform * Mesh.mVertices[VertexIndex];
					if (!IsFinite(Position))
					{
						OutError = std::format("Mesh '{}' produced a non-finite transformed position.", OutMesh.Name);
						return false;
					}
					OutMesh.Positions.emplace_back(ToVector3(Position));

					if (Mesh.HasNormals())
					{
						aiVector3D Normal = NormalTransform * Mesh.mNormals[VertexIndex];
						Normal.NormalizeSafe();
						if (IsFinite(Normal)) OutMesh.Normals.emplace_back(ToVector3(Normal));
					}

					if (Mesh.HasTangentsAndBitangents())
					{
						aiVector3D Tangent = LinearTransform * Mesh.mTangents[VertexIndex];
						aiVector3D Bitangent = LinearTransform * Mesh.mBitangents[VertexIndex];
						aiVector3D Normal = Mesh.HasNormals() ? NormalTransform * Mesh.mNormals[VertexIndex] : aiVector3D(0.0f, 0.0f, 1.0f);
						Normal.NormalizeSafe();
						Tangent -= Normal * (Normal * Tangent);
						Tangent.NormalizeSafe();
						Bitangent.NormalizeSafe();
						if (IsFinite(Tangent) && IsFinite(Bitangent) && IsFinite(Normal))
						{
							const float Sign = ((Normal ^ Tangent) * Bitangent) < 0.0f ? -1.0f : 1.0f;
							OutMesh.Tangents.emplace_back(Tangent.x, Tangent.y, Tangent.z, Sign);
						}
					}

					if (Mesh.HasVertexColors(0))
					{
						const aiColor4D& Color = Mesh.mColors[0][VertexIndex];
						OutMesh.Colors.emplace_back(Color.r, Color.g, Color.b, Color.a);
					}

					for (uint32 Channel = 0; Channel < MaxImportedUVChannels; ++Channel)
					{
						if (Mesh.HasTextureCoords(Channel))
						{
							const aiVector3D& UV = Mesh.mTextureCoords[Channel][VertexIndex];
							OutMesh.UVChannels[Channel].emplace_back(UV.x, UV.y);
						}
					}
				}

				if (Mesh.mNumFaces > std::numeric_limits<uint32>::max() / 3u)
				{
					OutError = std::format("Mesh '{}' exceeds the uint32 index limit.", OutMesh.Name);
					return false;
				}
				OutMesh.Indices.reserve(Mesh.mNumFaces * 3u);
				for (unsigned int FaceIndex = 0; FaceIndex < Mesh.mNumFaces; ++FaceIndex)
				{
					const aiFace& Face = Mesh.mFaces[FaceIndex];
					if (Face.mNumIndices != 3)
					{
						OutError = std::format("Mesh '{}' contains a non-triangle face after triangulation.", OutMesh.Name);
						return false;
					}
					const uint32 I0 = Face.mIndices[0];
					const uint32 I1 = Face.mIndices[bMirrored ? 2 : 1];
					const uint32 I2 = Face.mIndices[bMirrored ? 1 : 2];
					if (I0 >= Mesh.mNumVertices || I1 >= Mesh.mNumVertices || I2 >= Mesh.mNumVertices)
					{
						OutError = std::format("Mesh '{}' contains an out-of-range index.", OutMesh.Name);
						return false;
					}
					OutMesh.Indices.insert(OutMesh.Indices.end(), {I0, I1, I2});
				}

				OutScene.Meshes.emplace_back(std::move(OutMesh));
				return true;
			}

			auto ImportNodeMeshes(
				const aiScene& Scene,
				const aiNode& Node,
				std::span<const uint32> SourceMaterialIndices,
				const aiMatrix4x4& ParentTransform,
				FImportedSceneData& OutScene,
				std::string& OutError) -> bool
			{
				const aiMatrix4x4 Transform = ParentTransform * Node.mTransformation;
				for (unsigned int MeshReferenceIndex = 0; MeshReferenceIndex < Node.mNumMeshes; ++MeshReferenceIndex)
				{
					if (!ImportMeshInstance(Scene, Node, Node.mMeshes[MeshReferenceIndex],
						SourceMaterialIndices, Transform, OutScene, OutError)) return false;
				}
				for (unsigned int ChildIndex = 0; ChildIndex < Node.mNumChildren; ++ChildIndex)
				{
					if (Node.mChildren[ChildIndex] == nullptr) continue;
					if (!ImportNodeMeshes(Scene, *Node.mChildren[ChildIndex],
						SourceMaterialIndices, Transform, OutScene, OutError)) return false;
				}
				return true;
			}

			auto ImportMeshesFromFile(std::string_view FilePath, const FMeshImportOptions& Options) -> FAsyncMeshImportResult
			{
				FAsyncMeshImportResult Result;
				const std::filesystem::path RootPath = std::filesystem::path(std::string(FilePath));
				if (!IsValidSourcePath(Options.RootSource.Path))
				{
					FailImport(Result, EImportDiagnosticCategory::UnsafeDependencyPath,
						Options.RootSource.Path, "Root source path is not a normalized mounted virtual path.");
					return Result;
				}
				std::vector<uint8> RootBytes;
				std::string ReadError;
				if (!ReadFileBytes(RootPath, MaxImportedSourceModelBytes, RootBytes, ReadError))
				{
					const bool bExists = std::filesystem::exists(RootPath);
					FailImport(Result,
						bExists ? EImportDiagnosticCategory::ResourceLimitExceeded : EImportDiagnosticCategory::MissingDependency,
						"root", ReadError);
					return Result;
				}
				if (!AppendDependency(Result.Scene, EImportedDependencyRole::RootModel,
					"root", Options.RootSource.Path, RootBytes))
				{
					FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
						"dependencies", "Imported dependency count exceeds the limit.");
					return Result;
				}

				std::string Extension = RootPath.extension().string();
				std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
					return static_cast<char>(std::tolower(Character));
				});
				const bool bGltf = Extension == ".gltf";
				const bool bGlb = Extension == ".glb";
				std::vector<uint32> GltfMeshMaterialIndices;
				if ((bGltf || bGlb) &&
					!ImportGltfMetadata(RootPath, Options.RootSource.Path,
						RootBytes, bGlb, Result, GltfMeshMaterialIndices))
				{
					DURIN_ERROR("Asset import failed. (file: {}, error: {})", FilePath, Result.ErrorMessage);
					return Result;
				}

				std::string OwnedFilePath(FilePath);
				Assimp::Importer Importer;

				const aiScene* Scene = Importer.ReadFile(OwnedFilePath.c_str(),
					aiProcess_Triangulate |
					aiProcess_FlipUVs |
					aiProcess_GenSmoothNormals |
					aiProcess_CalcTangentSpace |
					aiProcess_JoinIdenticalVertices);

				if (!Scene || (Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 || !Scene->mRootNode)
				{
					std::string ErrorMessage = Importer.GetErrorString();
					if (ErrorMessage.empty())
					{
						ErrorMessage = "Unknown mesh import failure.";
					}
					FailImport(Result, EImportDiagnosticCategory::InvalidValue, "root", ErrorMessage);
					DURIN_ERROR("Asset import failed. (file: {}, error: {})", FilePath, ErrorMessage);
					return Result;
				}

				if (!bGltf && !bGlb &&
					!ImportAssimpMaterials(*Scene, RootPath, Options.RootSource.Path, RootBytes, Result))
				{
					DURIN_ERROR("Asset import failed. (file: {}, error: {})", FilePath, Result.ErrorMessage);
					return Result;
				}
				if (Result.Scene.Materials.empty())
				{
					Result.Scene.Materials.push_back({.SourceMaterialIndex = 0, .SourceName = {}});
				}
				if ((bGltf || bGlb) && GltfMeshMaterialIndices.size() != Scene->mNumMeshes)
				{
					FailImport(Result, EImportDiagnosticCategory::InvalidReference,
						"meshes", "Assimp mesh projection does not match glTF primitive material mapping.");
					return Result;
				}

				if (!ImportNodeMeshes(*Scene, *Scene->mRootNode, GltfMeshMaterialIndices,
					ToAssimpMatrix(Options.SourceToEngine), Result.Scene, Result.ErrorMessage))
				{
					const std::string ErrorMessage = Result.ErrorMessage;
					FailImport(Result, EImportDiagnosticCategory::InvalidReference, "meshes", ErrorMessage);
					DURIN_ERROR("Asset import failed. (file: {}, error: {})", FilePath, Result.ErrorMessage);
					return Result;
				}
				for (const FImportedMeshData& Mesh : Result.Scene.Meshes)
				{
					const auto MaterialIt = std::ranges::find(
						Result.Scene.Materials, Mesh.SourceMaterialIndex, &FImportedMaterial::SourceMaterialIndex);
					if (MaterialIt == Result.Scene.Materials.end())
					{
						FailImport(Result, EImportDiagnosticCategory::InvalidReference,
							std::format("material:{}", Mesh.SourceMaterialIndex),
							"Imported mesh references a source material that was not normalized.");
						return Result;
					}
				}
				std::unordered_map<std::string, uint32> MaterialNameCounts;
				Result.Scene.MaterialSlots.reserve(Result.Scene.Materials.size());
				for (const FImportedMaterial& Material : Result.Scene.Materials)
				{
					if (std::ranges::none_of(Result.Scene.Meshes, [&Material](const FImportedMeshData& Mesh) {
						return Mesh.SourceMaterialIndex == Material.SourceMaterialIndex;
					})) continue;
					Result.Scene.MaterialSlots.push_back({
						MakeUniqueName(Material.SourceName, Material.SourceMaterialIndex, MaterialNameCounts),
						Material.SourceMaterialIndex,
						Material.SourceName});
				}
				if (Result.Scene.MaterialSlots.empty()) Result.Scene.MaterialSlots.push_back({"Default", 0, {}});

				Result.bSucceeded = true;
				return Result;
			}
		}

		FAsyncMeshImportHandle::FAsyncMeshImportHandle() = default;

		FAsyncMeshImportHandle::FAsyncMeshImportHandle(std::shared_ptr<FAsyncMeshImportSharedState> InState)
			: State(std::move(InState))
		{
		}

		auto FAsyncMeshImportHandle::IsValid() const -> bool
		{
			return State && State->Task.IsValid();
		}

		auto FAsyncMeshImportHandle::IsComplete() const -> bool
		{
			return State && State->Task.IsComplete();
		}

		auto FAsyncMeshImportHandle::Wait() const -> void
		{
			if (!State)
			{
				return;
			}

			WaitTask(State->Task);
		}

		auto FAsyncMeshImportHandle::GetDebugName() const -> const char*
		{
			return State ? State->Task.GetDebugName() : "";
		}

		auto FAsyncMeshImportHandle::TryGetResult(FAsyncMeshImportResult& OutResult) const -> bool
		{
			if (!State || !State->Task.IsComplete())
			{
				return false;
			}

			std::lock_guard Lock(State->Mutex);
			if (!State->Result.has_value())
			{
				return false;
			}

			OutResult = *State->Result;
			return true;
		}

		auto ImportFromFile(std::string_view FilePath, FImportedSceneData& OutData, const FMeshImportOptions& Options) -> bool
		{
			FAsyncMeshImportResult Result = ImportMeshesFromFile(FilePath, Options);
			if (!Result.bSucceeded)
			{
				OutData = std::move(Result.Scene);
				return false;
			}

			OutData = std::move(Result.Scene);
			return true;
		}

		auto ImportFromFileAsync(std::string_view FilePath, const FMeshImportOptions& Options) -> FAsyncMeshImportHandle
		{
			auto SharedState = std::make_shared<FAsyncMeshImportSharedState>();
			std::string OwnedFilePath(FilePath);
			SharedState->Task = LaunchTask("AssetImport.Mesh", [SharedState, FilePath = std::move(OwnedFilePath), Options]() mutable {
				FAsyncMeshImportResult Result = ImportMeshesFromFile(FilePath, Options);

				std::lock_guard Lock(SharedState->Mutex);
				SharedState->Result = std::move(Result);
			});

			if (!SharedState->Task.IsValid())
			{
				return {};
			}

			return FAsyncMeshImportHandle(std::move(SharedState));
		}
	}
} // namespace Durin
