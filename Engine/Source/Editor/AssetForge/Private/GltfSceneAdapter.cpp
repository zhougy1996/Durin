#include "ImportedSceneInternal.h"
#include "GltfSkeletalDecoder.h"

#include "Json/Json.h"

namespace Durin::Asset::Forge::Private
{
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

	auto EncodeBase64(std::span<const uint8> Bytes) -> std::string
	{
		static constexpr char Alphabet[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string Result;
		Result.reserve(((Bytes.size() + 2) / 3) * 4);
		for (size_t Offset = 0; Offset < Bytes.size(); Offset += 3)
		{
			const uint32 A = Bytes[Offset];
			const uint32 B = Offset + 1 < Bytes.size() ? Bytes[Offset + 1] : 0;
			const uint32 C = Offset + 2 < Bytes.size() ? Bytes[Offset + 2] : 0;
			const uint32 Value = (A << 16) | (B << 8) | C;
			Result.push_back(Alphabet[(Value >> 18) & 63]);
			Result.push_back(Alphabet[(Value >> 12) & 63]);
			Result.push_back(Offset + 1 < Bytes.size() ? Alphabet[(Value >> 6) & 63] : '=');
			Result.push_back(Offset + 2 < Bytes.size() ? Alphabet[Value & 63] : '=');
		}
		return Result;
	}

	auto ShouldCopyProjectionMember(
		std::string_view Path,
		std::string_view Key) -> bool
	{
		if (Path.empty() && (Key == "skins" || Key == "animations")) return false;
		if (Path.starts_with("nodes/") && Path.find('/', 6) == std::string_view::npos
			&& Key == "skin") return false;
		if (Path.find("/primitives/") != std::string_view::npos && Key == "targets") return false;
		if (Path.ends_with("/attributes")
			&& (Key == "JOINTS_0" || Key == "WEIGHTS_0"
				|| Key == "JOINTS_1" || Key == "WEIGHTS_1")) return false;
		return true;
	}

	auto CopyProjectionJson(
		FJsonNodeView Source,
		FJsonNodeRef Destination,
		const std::string& Path) -> bool
	{
		if (Source.IsNull())
		{
			Destination.SetValue(nullptr);
			return true;
		}
		if (Source.IsString())
		{
			Destination.SetValue(Source.GetString());
			return true;
		}
		if (Source.IsBool())
		{
			Destination.SetValue(Source.GetBool());
			return true;
		}
		if (Source.IsUInt())
		{
			Destination.SetValue(Source.GetUInt());
			return true;
		}
		if (Source.IsInt())
		{
			Destination.SetValue(Source.GetInt());
			return true;
		}
		if (Source.IsNumber())
		{
			Destination.SetValue(Source.GetDouble());
			return true;
		}
		if (Source.IsArray())
		{
			Destination.EnsureArray();
			for (size_t Index = 0; Index < Source.Num(); ++Index)
			{
				FJsonNodeRef Child;
				if (Source.GetView(Index).IsObject()) Child = Destination.AppendObject();
				else if (Source.GetView(Index).IsArray()) Child = Destination.AppendArray();
				else
				{
					Destination.AppendValue(nullptr);
					Child = Destination.GetRef(Destination.Num() - 1);
				}
				if (!CopyProjectionJson(
					Source.GetView(Index), Child,
					Path.empty() ? std::to_string(Index) : std::format("{}/{}", Path, Index))) return false;
			}
			return true;
		}
		if (!Source.IsObject()) return false;
		Destination.EnsureObject();
		bool bSucceeded = true;
		Source.ForEachObjectMember([&](std::string_view Key, FJsonNodeView Value) {
			if (!bSucceeded || !ShouldCopyProjectionMember(Path, Key)) return;
			FJsonNodeRef Child;
			if (Value.IsObject()) Child = Destination.AddObject(Key);
			else if (Value.IsArray()) Child = Destination.AddArray(Key);
			else
			{
				Destination.SetChildValue(Key, nullptr);
				Child = Destination.GetRef(Key);
			}
			bSucceeded = CopyProjectionJson(
				Value, Child, Path.empty() ? std::string(Key) : std::format("{}/{}", Path, Key));
		});
		return bSucceeded;
	}

	auto NormalizeProjectionTexCoords(
		FJsonDocument& Projection,
		FJsonNodeView SourceRoot,
		const std::vector<std::vector<uint8>>& Buffers) -> bool
	{
		FJsonNodeRef ProjectionRoot = Projection.GetMutableRoot();
		FJsonNodeRef Meshes = ProjectionRoot.GetRef("meshes");
		FJsonNodeRef ProjectionBuffers = ProjectionRoot.GetRef("buffers");
		FJsonNodeRef ProjectionViews = ProjectionRoot.GetRef("bufferViews");
		FJsonNodeRef ProjectionAccessors = ProjectionRoot.GetRef("accessors");
		const FJsonNodeView SourceViews = SourceRoot.GetView("bufferViews");
		const FJsonNodeView SourceAccessors = SourceRoot.GetView("accessors");
		if (!Meshes.IsArray() || !ProjectionBuffers.IsArray() || !ProjectionViews.IsArray()
			|| !ProjectionAccessors.IsArray() || !SourceViews.IsArray() || !SourceAccessors.IsArray()) return false;

		std::unordered_map<uint64, uint64> ConvertedAccessors;
		for (size_t MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
		{
			FJsonNodeRef Primitives = Meshes.GetRef(MeshIndex).GetRef("primitives");
			if (!Primitives.IsArray()) return false;
			std::array<std::optional<uint64>, MaxImportedUVChannels> FloatAccessors;
			for (size_t PrimitiveIndex = 0; PrimitiveIndex < Primitives.Num(); ++PrimitiveIndex)
			{
				FJsonNodeRef Attributes = Primitives.GetRef(PrimitiveIndex).GetRef("attributes");
				if (!Attributes.IsObject()) return false;
				for (uint32 Channel = 0; Channel < MaxImportedUVChannels; ++Channel)
				{
					const std::string Semantic = std::format("TEXCOORD_{}", Channel);
					const FJsonNodeView Attribute = Attributes.GetView(Semantic);
					if (!Attribute.IsValid()) continue;
					const uint64 AccessorIndex = Attribute.GetUInt(std::numeric_limits<uint64>::max());
					if (AccessorIndex >= SourceAccessors.Num()) return false;
					const FJsonNodeView Accessor = SourceAccessors.GetView(static_cast<size_t>(AccessorIndex));
					const uint64 ComponentType = Accessor.GetView("componentType").GetUInt();
					if (Accessor.GetView("type").GetString() == "VEC2" && ComponentType == 5126
						&& !Accessor.GetView("normalized").GetBool(false))
					{
						if (!FloatAccessors[Channel]) FloatAccessors[Channel] = AccessorIndex;
						continue;
					}
					if (!Accessor.GetView("normalized").GetBool(false)
						|| (ComponentType != 5121 && ComponentType != 5123)) continue;
					if (Accessor.GetView("type").GetString() != "VEC2") return false;
					if (FloatAccessors[Channel])
					{
						Attributes.SetChildValue(Semantic, *FloatAccessors[Channel]);
						continue;
					}

					if (const auto Existing = ConvertedAccessors.find(AccessorIndex);
						Existing != ConvertedAccessors.end())
					{
						Attributes.SetChildValue(Semantic, Existing->second);
						continue;
					}

					const uint64 ViewIndex = Accessor.GetView("bufferView").GetUInt(std::numeric_limits<uint64>::max());
					const uint64 Count = Accessor.GetView("count").GetUInt(std::numeric_limits<uint64>::max());
					if (ViewIndex >= SourceViews.Num() || Count == std::numeric_limits<uint64>::max()
						|| Count > MaximumSkeletalMeshVertices) return false;
					const FJsonNodeView View = SourceViews.GetView(static_cast<size_t>(ViewIndex));
					const uint64 BufferIndex = View.GetView("buffer").GetUInt(std::numeric_limits<uint64>::max());
					const uint64 ViewOffset = View.GetView("byteOffset").GetUInt(0);
					const uint64 ViewLength = View.GetView("byteLength").GetUInt(std::numeric_limits<uint64>::max());
					const uint64 AccessorOffset = Accessor.GetView("byteOffset").GetUInt(0);
					const uint64 ComponentSize = ComponentType == 5121 ? 1 : 2;
					const uint64 ElementSize = ComponentSize * 2;
					const uint64 Stride = View.GetView("byteStride").GetUInt(ElementSize);
					if (BufferIndex >= Buffers.size() || ViewLength == std::numeric_limits<uint64>::max()
						|| Stride < ElementSize || AccessorOffset > ViewLength
						|| Count > 0 && (Count - 1 > (std::numeric_limits<uint64>::max() - AccessorOffset - ElementSize) / Stride
							|| AccessorOffset + (Count - 1) * Stride + ElementSize > ViewLength)
						|| ViewOffset > Buffers[BufferIndex].size()
						|| ViewLength > Buffers[BufferIndex].size() - ViewOffset) return false;

					std::vector<uint8> Converted(static_cast<size_t>(Count) * sizeof(float) * 2);
					for (uint64 ElementIndex = 0; ElementIndex < Count; ++ElementIndex)
					{
						const size_t SourceOffset = static_cast<size_t>(ViewOffset + AccessorOffset + ElementIndex * Stride);
						for (uint64 Component = 0; Component < 2; ++Component)
						{
							uint32 Value = Buffers[BufferIndex][SourceOffset + Component * ComponentSize];
							if (ComponentSize == 2)
								Value |= static_cast<uint32>(Buffers[BufferIndex][SourceOffset + Component * ComponentSize + 1]) << 8;
							const float Normalized = static_cast<float>(Value)
								/ static_cast<float>(ComponentSize == 1 ? 255u : 65535u);
							std::memcpy(Converted.data() + (ElementIndex * 2 + Component) * sizeof(float),
								&Normalized, sizeof(float));
						}
					}

					const uint64 NewBufferIndex = ProjectionBuffers.Num();
					FJsonNodeRef NewBuffer = ProjectionBuffers.AppendObject();
					NewBuffer.SetChildValue("byteLength", static_cast<uint64>(Converted.size()));
					NewBuffer.SetChildValue("uri", std::format(
						"data:application/octet-stream;base64,{}", EncodeBase64(Converted)));
					const uint64 NewViewIndex = ProjectionViews.Num();
					FJsonNodeRef NewView = ProjectionViews.AppendObject();
					NewView.SetChildValue("buffer", NewBufferIndex);
					NewView.SetChildValue("byteLength", static_cast<uint64>(Converted.size()));
					const uint64 NewAccessorIndex = ProjectionAccessors.Num();
					FJsonNodeRef NewAccessor = ProjectionAccessors.AppendObject();
					NewAccessor.SetChildValue("bufferView", NewViewIndex);
					NewAccessor.SetChildValue("componentType", static_cast<uint64>(5126));
					NewAccessor.SetChildValue("count", Count);
					NewAccessor.SetChildValue("type", "VEC2");
					ConvertedAccessors.emplace(AccessorIndex, NewAccessorIndex);
					Attributes.SetChildValue(Semantic, NewAccessorIndex);
				}
			}
		}
		return true;
	}

	auto BuildAssimpProjection(
		FJsonNodeView Root,
		const std::vector<std::vector<uint8>>& Buffers,
		std::vector<uint8>& OutBytes) -> bool
	{
		FJsonDocument Projection;
		if (!CopyProjectionJson(Root, Projection.GetMutableRoot(), {})) return false;
		FJsonNodeRef ProjectionBuffers = Projection.GetMutableRoot().GetRef("buffers");
		if (!ProjectionBuffers.IsValid() || !ProjectionBuffers.IsArray()
			|| ProjectionBuffers.Num() != Buffers.size()) return false;
		for (size_t Index = 0; Index < Buffers.size(); ++Index)
		{
			FJsonNodeRef Buffer = ProjectionBuffers.GetRef(Index);
			Buffer.SetChildValue("byteLength", static_cast<uint64>(Buffers[Index].size()));
			Buffer.SetChildValue(
				"uri",
				std::format("data:application/octet-stream;base64,{}", EncodeBase64(Buffers[Index])));
		}
		if ((Root.GetView("skins").IsValid() || Root.GetView("animations").IsValid())
			&& !NormalizeProjectionTexCoords(Projection, Root, Buffers)) return false;
		const std::string Json = Projection.ToString();
		OutBytes.assign(Json.begin(), Json.end());
		return !OutBytes.empty();
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

	auto ValidateGltfExtensions(FJsonNodeView Root, FSceneDecodeResult& Result) -> bool
	{
		const bool bSkeletalDocument = Root.GetView("skins").IsValid()
			|| Root.GetView("animations").IsValid();
		std::unordered_set<std::string> Required;
		const FJsonNodeView RequiredNode = Root.GetView("extensionsRequired");
		if (RequiredNode.IsValid() && !RequiredNode.IsArray())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
				"extensionsRequired", "glTF extensionsRequired must be an array.");
		}
		for (size_t Index = 0; Index < RequiredNode.Num(); ++Index)
		{
			if (CheckSceneDecodeCancellation(Result, "extensionsRequired")) return false;
			const FJsonNodeView ExtensionNode = RequiredNode.GetView(Index);
			if (!ExtensionNode.IsString())
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
					"extensionsRequired", "glTF extensionsRequired contains a non-string value.");
			}
			const std::string Extension = ExtensionNode.GetString();
			Required.insert(Extension);
			if (!IsSupportedGltfExtension(Extension))
			{
				return FailImport(Result,
					bSkeletalDocument
						? ESceneImportDiagnosticCategory::UnsupportedFeature
						: ESceneImportDiagnosticCategory::UnsupportedRequiredExtension,
					Extension, std::format("Required glTF extension '{}' is unsupported.", Extension));
			}
		}
		const FJsonNodeView UsedNode = Root.GetView("extensionsUsed");
		if (UsedNode.IsValid() && !UsedNode.IsArray())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
				"extensionsUsed", "glTF extensionsUsed must be an array.");
		}
		for (size_t Index = 0; Index < UsedNode.Num(); ++Index)
		{
			if (CheckSceneDecodeCancellation(Result, "extensionsUsed")) return false;
			const FJsonNodeView ExtensionNode = UsedNode.GetView(Index);
			if (!ExtensionNode.IsString())
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
					"extensionsUsed", "glTF extensionsUsed contains a non-string value.");
			}
			const std::string Extension = ExtensionNode.GetString();
			if (!IsSupportedGltfExtension(Extension) && !Required.contains(Extension))
			{
				if (!AddDiagnostic(Result.Scene, EImportDiagnosticSeverity::Warning,
					ESceneImportDiagnosticCategory::UnsupportedOptionalExtension,
					"root", Extension,
					std::format("Optional glTF extension '{}' is unsupported; core fallback is used.", Extension)))
				{
					return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
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
		FSceneDecodeResult& Result) -> bool
	{
		const FJsonNodeView Buffers = Root.GetView("buffers");
		if (Buffers.IsValid() && !Buffers.IsArray())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
				"buffers", "glTF buffers must be an array.");
		}
		Source.Buffers.resize(Buffers.Num());
		for (size_t Index = 0; Index < Buffers.Num(); ++Index)
		{
			if (CheckSceneDecodeCancellation(Result, "buffers")) return false;
			const FJsonNodeView Buffer = Buffers.GetView(Index);
			const uint64 DeclaredLength = Buffer.GetView("byteLength").GetUInt(std::numeric_limits<uint64>::max());
			if (!Buffer.IsObject() || DeclaredLength == std::numeric_limits<uint64>::max() ||
				DeclaredLength > MaxImportedSceneSourceBytes)
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
					std::format("buffer:{}", Index), "glTF buffer byte length is invalid or exceeds the limit.");
			}
			const FJsonNodeView UriNode = Buffer.GetView("uri");
			std::vector<uint8>& Bytes = Source.Buffers[Index];
			if (!UriNode.IsValid())
			{
				if (Index != 0 || Source.GlbBinaryChunkBytes.empty())
				{
					return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
						std::format("buffer:{}", Index), "glTF buffer has no URI or GLB binary chunk.");
				}
				Bytes = Source.GlbBinaryChunkBytes;
			}
			else if (!UriNode.IsString())
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
					std::format("buffer:{}", Index), "glTF buffer URI must be a string.");
			}
			else
			{
				const std::string Uri = UriNode.GetString();
				if (Uri.starts_with("data:"))
				{
					std::string MimeType;
					if (!DecodeDataUri(Uri, MaxImportedSceneSourceBytes, MimeType, Bytes))
					{
						return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
							std::format("buffer:{}", Index), "glTF buffer data URI is invalid.");
					}
				}
				else
				{
					std::filesystem::path DependencyPath;
					std::string NormalizedUri;
					if (!ResolveDependencyPath(RootPath, Uri, DependencyPath, NormalizedUri))
					{
						return FailImport(Result, ESceneImportDiagnosticCategory::UnsafeDependencyPath,
							Uri, std::format("glTF buffer URI '{}' escapes the source directory.", Uri));
					}
					std::string Error;
					if (!ReadFileBytes(DependencyPath, MaxImportedSceneSourceBytes, Bytes, Error))
					{
						return FailImport(Result, ESceneImportDiagnosticCategory::MissingDependency,
							NormalizedUri, Error);
					}
					if (!AppendDependency(Result.Scene, EImportedDependencyRole::GeometryBuffer,
						std::format("buffer:{}", NormalizedUri),
						MakeDependencySourcePath(RootSourcePath, NormalizedUri), Bytes))
					{
						return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
							"dependencies", "Imported dependency count exceeds the limit.");
					}
				}
			}
			if (Bytes.size() < DeclaredLength)
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
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
		FSceneDecodeResult& Result) -> bool
	{
		const FJsonNodeView Images = Root.GetView("images");
		if (Images.IsValid() && !Images.IsArray())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
				"images", "glTF images must be an array.");
		}
		if (Images.Num() > MaxImportedImages)
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
				"images", "Imported image count exceeds the limit.");
		}
		uint64 EmbeddedByteCount = 0;
		std::unordered_map<std::string, uint32> ImportedImageIndices;
		Source.ImageIndices.resize(Images.Num());
		Result.Scene.Images.reserve(Images.Num());
		for (size_t Index = 0; Index < Images.Num(); ++Index)
		{
			if (CheckSceneDecodeCancellation(Result, "images")) return false;
			const FJsonNodeView Image = Images.GetView(Index);
			if (!Image.IsObject())
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
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
					return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
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
						return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
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
						return FailImport(Result, ESceneImportDiagnosticCategory::UnsafeDependencyPath,
							Uri, std::format("glTF image URI '{}' escapes the source directory.", Uri));
					}
					Imported.StableIdentity = std::format("external:{}", NormalizedUri);
					if (!ResolveImageEncoding(EffectiveMime, NormalizedUri, Imported.Encoding))
					{
						return FailImport(Result, ESceneImportDiagnosticCategory::UnsupportedEncoding,
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
						return FailImport(Result, ESceneImportDiagnosticCategory::MissingDependency,
							NormalizedUri, Error);
					}
					uint32 DependencyIndex = 0;
					if (!AppendDependency(Result.Scene, EImportedDependencyRole::Image,
						std::format("image:{}", NormalizedUri),
						MakeDependencySourcePath(RootSourcePath, NormalizedUri), OwnedBytes, &DependencyIndex))
					{
						return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
							"dependencies", "Imported dependency count exceeds the limit.");
					}
					Imported.ExternalDependencyIndex = DependencyIndex;
					EncodedBytes = OwnedBytes;
				}
				if (!Imported.ExternalDependencyIndex.has_value() &&
					!ResolveImageEncoding(EffectiveMime, Uri, Imported.Encoding))
				{
					return FailImport(Result, ESceneImportDiagnosticCategory::UnsupportedEncoding,
						Imported.StableIdentity, "Embedded image encoding is unsupported.");
				}
			}
			else
			{
				const uint64 BufferViewIndex = Image.GetView("bufferView").GetUInt(std::numeric_limits<uint64>::max());
				if (BufferViewIndex > std::numeric_limits<uint32>::max() ||
					!GetBufferViewBytes(Root, Source, static_cast<uint32>(BufferViewIndex), EncodedBytes))
				{
					return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
						std::format("image:{}", Index), "glTF image buffer view is invalid.");
				}
				if (!ResolveImageEncoding(DeclaredMime, {}, Imported.Encoding))
				{
					return FailImport(Result, ESceneImportDiagnosticCategory::UnsupportedEncoding,
						std::format("image:{}", Index), "Buffer-view image MIME type is unsupported.");
				}
				Imported.StableIdentity = std::format("glb-buffer-view:{}", BufferViewIndex);
				if (const auto Existing = ImportedImageIndices.find(Imported.StableIdentity);
					Existing != ImportedImageIndices.end())
				{
					if (Result.Scene.Images[Existing->second].Encoding != Imported.Encoding)
					{
						return FailImport(Result, ESceneImportDiagnosticCategory::UnsupportedEncoding,
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
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
					Imported.StableIdentity, ImageError);
			}
			Imported.EncodedByteCount = EncodedBytes.size();
			if (!Imported.ExternalDependencyIndex.has_value())
			{
				EmbeddedByteCount += Imported.EncodedByteCount;
				if (EmbeddedByteCount > MaxImportedEmbeddedImageBytes)
				{
					return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
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
		FSceneDecodeResult& Result,
		std::string_view Subject) -> bool
	{
		if (!TextureInfo.IsObject())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
				std::string(Subject), "glTF texture info must be an object.");
		}
		const uint64 TextureIndex = TextureInfo.GetView("index").GetUInt(std::numeric_limits<uint64>::max());
		const FJsonNodeView Textures = Root.GetView("textures");
		if (!Textures.IsArray() || TextureIndex >= Textures.Num())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
				std::string(Subject), "glTF texture index is invalid.");
		}
		const FJsonNodeView Texture = Textures.GetView(static_cast<size_t>(TextureIndex));
		const uint64 ImageIndex = Texture.GetView("source").GetUInt(std::numeric_limits<uint64>::max());
		if (ImageIndex >= SourceImageIndices.size() ||
			SourceImageIndices[static_cast<size_t>(ImageIndex)] >= Result.Scene.Images.size())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
				std::string(Subject), "glTF texture source image index is invalid.");
		}
		OutBinding.Semantic = Semantic;
		OutBinding.ImageIndex = SourceImageIndices[static_cast<size_t>(ImageIndex)];
		OutBinding.Strength = DefaultStrength;
		const uint64 UVChannel = TextureInfo.GetView("texCoord").GetUInt(0);
		if (UVChannel >= MaxImportedUVChannels)
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
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
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
					std::string(Subject), "KHR_texture_transform contains an invalid value.");
			}
			if (Transform.Contains("texCoord"))
			{
				const uint64 TransformedUVChannel = Transform.GetView("texCoord").GetUInt(
					std::numeric_limits<uint64>::max());
				if (TransformedUVChannel >= MaxImportedUVChannels)
				{
					return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
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
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
					std::string(Subject), "glTF sampler index is invalid.");
			}
			const FJsonNodeView Sampler = Samplers.GetView(static_cast<size_t>(SamplerIndex));
			if (!Sampler.IsObject())
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
					std::string(Subject), "glTF sampler must be an object.");
			}
			if (Sampler.Contains("minFilter") &&
				!ParseSamplerFilter(Sampler.GetView("minFilter").GetInt(-1), true, OutBinding.Sampler.MinFilter))
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::UnsupportedSampler,
					std::string(Subject), "glTF minification filter is unsupported.");
			}
			if (Sampler.Contains("magFilter") &&
				!ParseSamplerFilter(Sampler.GetView("magFilter").GetInt(-1), false, OutBinding.Sampler.MagFilter))
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::UnsupportedSampler,
					std::string(Subject), "glTF magnification filter is unsupported.");
			}
			if (Sampler.Contains("wrapS") &&
				!ParseSamplerWrap(Sampler.GetView("wrapS").GetInt(-1), OutBinding.Sampler.WrapU))
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::UnsupportedSampler,
					std::string(Subject), "glTF U wrapping mode is unsupported.");
			}
			if (Sampler.Contains("wrapT") &&
				!ParseSamplerWrap(Sampler.GetView("wrapT").GetInt(-1), OutBinding.Sampler.WrapV))
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::UnsupportedSampler,
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
		FSceneDecodeResult& Result,
		std::string_view Subject) -> bool
	{
		if (!TextureInfo.IsValid()) return true;
		if (Material.TextureBindings.size() >= MaxImportedTextureBindingsPerMaterial)
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
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
		FSceneDecodeResult& Result) -> bool
	{
		const FJsonNodeView Materials = Root.GetView("materials");
		if (Materials.IsValid() && !Materials.IsArray())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
				"materials", "glTF materials must be an array.");
		}
		if (Materials.Num() > MaxImportedSourceMaterials)
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
				"materials", "Source material count exceeds the limit.");
		}
		Result.Scene.Materials.reserve(Materials.Num());
		for (size_t Index = 0; Index < Materials.Num(); ++Index)
		{
			if (CheckSceneDecodeCancellation(Result, "materials")) return false;
			const FJsonNodeView MaterialNode = Materials.GetView(Index);
			if (!MaterialNode.IsObject())
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
					std::format("material:{}", Index), "glTF material must be an object.");
			}
			FImportedMaterial Material;
			Material.SourceMaterialIndex = static_cast<uint32>(Index);
			Material.SourceName = MaterialNode.GetView("name").GetString();
			const FJsonNodeView Pbr = MaterialNode.GetView("pbrMetallicRoughness");
			if (Pbr.IsValid() && !Pbr.IsObject())
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
					std::format("material:{}", Index), "pbrMetallicRoughness must be an object.");
			}
			if (!ReadFiniteVector(Pbr, "baseColorFactor", Material.BaseColorFactor) ||
				!ReadFiniteFloat(Pbr, "metallicFactor", 1.0f, Material.MetallicFactor) ||
				!ReadFiniteFloat(Pbr, "roughnessFactor", 1.0f, Material.RoughnessFactor) ||
				!ReadFiniteVector(MaterialNode, "emissiveFactor", Material.EmissiveFactor) ||
				!ReadFiniteFloat(MaterialNode, "alphaCutoff", 0.5f, Material.AlphaCutoff))
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
					std::format("material:{}", Index), "glTF material contains a non-finite or malformed factor.");
			}
			const std::string AlphaMode = MaterialNode.GetView("alphaMode").GetString("OPAQUE");
			if (AlphaMode == "OPAQUE") Material.AlphaMode = EImportedAlphaMode::Opaque;
			else if (AlphaMode == "MASK") Material.AlphaMode = EImportedAlphaMode::Mask;
			else if (AlphaMode == "BLEND") Material.AlphaMode = EImportedAlphaMode::Blend;
			else
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::UnsupportedAlphaMode,
					std::format("material:{}", Index), std::format("glTF alpha mode '{}' is unsupported.", AlphaMode));
			}
			Material.bDoubleSided = MaterialNode.GetView("doubleSided").GetBool(false);

			float NormalScale = 1.0f;
			float OcclusionStrength = 1.0f;
			if (!ReadFiniteFloat(MaterialNode.GetView("normalTexture"), "scale", 1.0f, NormalScale) ||
				!ReadFiniteFloat(MaterialNode.GetView("occlusionTexture"), "strength", 1.0f, OcclusionStrength))
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
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

	auto AppendGltfMeshProjection(
		FJsonNodeView Meshes,
		uint64 MeshIndex,
		FSceneDecodeResult& Result,
		std::vector<uint32>& OutMeshMaterialIndices) -> bool
	{
		if (MeshIndex >= Meshes.Num())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
				std::format("mesh:{}", MeshIndex), "glTF node references an invalid mesh index.");
		}
		const FJsonNodeView Primitives = Meshes.GetView(MeshIndex).GetView("primitives");
		if (!Primitives.IsArray())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
				std::format("mesh:{}", MeshIndex), "glTF mesh primitives must be an array.");
		}
		for (size_t PrimitiveIndex = 0; PrimitiveIndex < Primitives.Num(); ++PrimitiveIndex)
		{
			if (CheckSceneDecodeCancellation(Result, "mesh-primitives")) return false;
			const FJsonNodeView Material = Primitives.GetView(PrimitiveIndex).GetView("material");
			uint64 MaterialIndex = 0;
			if (!Material.IsValid())
			{
				if (!Result.DefaultGltfMaterialIndex)
				{
					if (Result.Scene.Materials.size() >= MaxImportedSourceMaterials)
					{
						return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
							"materials", "The implicit glTF default material exceeds the material limit.");
					}
					Result.DefaultGltfMaterialIndex = static_cast<uint32>(Result.Scene.Materials.size());
					Result.Scene.Materials.push_back({
						.SourceMaterialIndex = *Result.DefaultGltfMaterialIndex,
						.SourceName = "Default"});
				}
				MaterialIndex = *Result.DefaultGltfMaterialIndex;
			}
			else if (!Material.GetValue(MaterialIndex))
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
					std::format("mesh:{}:primitive:{}", MeshIndex, PrimitiveIndex),
					"glTF primitive material index is invalid.");
			}
			if (MaterialIndex >= Result.Scene.Materials.size())
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
					std::format("mesh:{}:primitive:{}", MeshIndex, PrimitiveIndex),
					"glTF primitive material index is invalid.");
			}
			OutMeshMaterialIndices.push_back(static_cast<uint32>(MaterialIndex));
		}
		return true;
	}

	auto VisitGltfProjectionNode(
		FJsonNodeView Nodes,
		FJsonNodeView Meshes,
		uint64 NodeIndex,
		std::vector<uint8>& NodeStates,
		std::vector<bool>& SeenMeshes,
		FSceneDecodeResult& Result,
		std::vector<uint32>& OutMeshMaterialIndices) -> bool
	{
		if (NodeIndex >= Nodes.Num())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
				std::format("node:{}", NodeIndex), "glTF scene references an invalid node index.");
		}
		if (NodeStates[NodeIndex] == 1)
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
				std::format("node:{}", NodeIndex), "glTF node hierarchy contains a cycle.");
		}
		if (NodeStates[NodeIndex] == 2) return true;
		NodeStates[NodeIndex] = 1;

		const FJsonNodeView Node = Nodes.GetView(NodeIndex);
		if (!Node.IsObject())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
				std::format("node:{}", NodeIndex), "glTF node must be an object.");
		}
		const FJsonNodeView Mesh = Node.GetView("mesh");
		if (Mesh.IsValid())
		{
			uint64 MeshIndex = 0;
			if (!Mesh.GetValue(MeshIndex) || MeshIndex >= SeenMeshes.size())
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
					std::format("node:{}", NodeIndex), "glTF node mesh index is invalid.");
			}
			if (!SeenMeshes[MeshIndex])
			{
				SeenMeshes[MeshIndex] = true;
				if (!AppendGltfMeshProjection(
					Meshes, MeshIndex, Result, OutMeshMaterialIndices))
				{
					return false;
				}
			}
		}

		const FJsonNodeView Children = Node.GetView("children");
		if (Children.IsValid() && !Children.IsArray())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
				std::format("node:{}", NodeIndex), "glTF node children must be an array.");
		}
		for (size_t ChildIndex = 0; ChildIndex < Children.Num(); ++ChildIndex)
		{
			if (CheckSceneDecodeCancellation(Result, "nodes")) return false;
			uint64 ChildNodeIndex = 0;
			if (!Children.GetView(ChildIndex).GetValue(ChildNodeIndex)
				|| !VisitGltfProjectionNode(
					Nodes, Meshes, ChildNodeIndex, NodeStates, SeenMeshes,
					Result, OutMeshMaterialIndices))
			{
				return false;
			}
		}
		NodeStates[NodeIndex] = 2;
		return true;
	}

	auto BuildGltfAssimpMeshProjection(
		FJsonNodeView Root,
		FSceneDecodeResult& Result,
		std::vector<uint32>& OutMeshMaterialIndices) -> bool
	{
		const FJsonNodeView Meshes = Root.GetView("meshes");
		const FJsonNodeView Nodes = Root.GetView("nodes");
		const FJsonNodeView Scenes = Root.GetView("scenes");
		if (!Meshes.IsArray() || !Nodes.IsArray() || !Scenes.IsArray())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
				"root", "glTF meshes, nodes, and scenes must be arrays.");
		}
		const uint64 SceneIndex = Root.GetView("scene").GetUInt(0);
		if (SceneIndex >= Scenes.Num())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
				"scene", "glTF default scene index is invalid.");
		}
		const FJsonNodeView RootNodes = Scenes.GetView(SceneIndex).GetView("nodes");
		if (!RootNodes.IsArray())
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
				std::format("scene:{}", SceneIndex), "glTF scene nodes must be an array.");
		}

		std::vector<uint8> NodeStates(Nodes.Num());
		std::vector<bool> SeenMeshes(Meshes.Num());
		for (size_t RootNodeIndex = 0; RootNodeIndex < RootNodes.Num(); ++RootNodeIndex)
		{
			if (CheckSceneDecodeCancellation(Result, "scene-nodes")) return false;
			uint64 NodeIndex = 0;
			if (!RootNodes.GetView(RootNodeIndex).GetValue(NodeIndex)
				|| !VisitGltfProjectionNode(
					Nodes, Meshes, NodeIndex, NodeStates, SeenMeshes,
					Result, OutMeshMaterialIndices))
			{
				return false;
			}
		}
		return true;
	}

	auto ImportGltfMetadata(
		const std::filesystem::path& RootPath,
		std::string_view RootSourcePath,
		std::span<const uint8> RootBytes,
		bool bGlb,
		FSceneDecodeResult& Result,
		std::vector<uint32>& OutMeshMaterialIndices,
		std::vector<uint8>& OutAssimpProjection) -> bool
	{
		FGltfSource Source;
		std::string Error;
		if (!LoadGltfDocument(RootBytes, bGlb, Source, Error))
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue, "root", Error);
		const FJsonNodeView Root = Source.Document.GetRootView();
		if (!(ValidateGltfExtensions(Root, Result) &&
			LoadGltfBuffers(Root, RootPath, RootSourcePath, Source, Result) &&
			ImportGltfImages(Root, RootPath, RootSourcePath, Source, Result) &&
			ImportGltfMaterials(Root, Source.ImageIndices, Result) &&
			ImportGltfSkeletalData(Root, Source.Buffers, Result) &&
			BuildGltfAssimpMeshProjection(Root, Result, OutMeshMaterialIndices) &&
			BuildAssimpProjection(Root, Source.Buffers, OutAssimpProjection)))
		{
			return false;
		}
		return true;
	}

	auto ImportGltfFormat(
		const FImportedSceneContext& Context,
		bool bGlb,
		std::vector<uint32>& OutSourcePrimitiveMaterialIndices,
		std::vector<uint8>& OutAssimpProjection) -> bool
	{
		return ImportGltfMetadata(
			Context.RootPath,
			Context.RootSourcePath,
			Context.RootBytes,
			bGlb,
			Context.Result,
			OutSourcePrimitiveMaterialIndices,
			OutAssimpProjection);
	}
}
