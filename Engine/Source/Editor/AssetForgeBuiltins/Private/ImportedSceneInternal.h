#pragma once

#include "AssetForge/Builtins/ImportedScene.h"

struct aiScene;

namespace Durin::AssetForge::Builtins
{
	struct FSceneDecodeResult
	{
		bool bSucceeded = false;
		FImportedSceneData Scene;
		std::string ErrorMessage;
		std::optional<uint32> DefaultGltfMaterialIndex;
	};
}

namespace Durin::AssetForge::Builtins::Private
{
	class FScopedSceneImportCancellation final
	{
	public:
		explicit FScopedSceneImportCancellation(
			const std::function<bool()>& IsCancellationRequested) noexcept;
		~FScopedSceneImportCancellation() noexcept;
		FScopedSceneImportCancellation(const FScopedSceneImportCancellation&) = delete;
		auto operator=(const FScopedSceneImportCancellation&)
			-> FScopedSceneImportCancellation& = delete;

	private:
		const std::function<bool()>* Previous = nullptr;
	};

	auto IsSceneImportCancellationRequested() -> bool;

	// Carries one authoritative source and result sink across a format adapter.
	struct FImportedSceneContext
	{
		const std::filesystem::path& RootPath;
		std::string_view RootSourcePath;
		FByteView RootBytes;
		const FMeshImportOptions& Options;
		FSceneDecodeResult& Result;
	};

	auto AddDiagnostic(
		FImportedSceneData& Scene,
		EImportDiagnosticSeverity Severity,
		ESceneImportDiagnosticCategory Category,
		std::string SourceIdentity,
		std::string Subject,
		std::string Message) -> bool;
	auto FailImport(
		FSceneDecodeResult& Result,
		ESceneImportDiagnosticCategory Category,
		std::string Subject,
		std::string Message,
		std::string SourceIdentity = "root") -> bool;
	auto CheckSceneDecodeCancellation(
		FSceneDecodeResult& Result,
		std::string_view Subject) -> bool;
	auto ReadFileBytes(
		const std::filesystem::path& Path,
		uint64 Limit,
		FByteBuffer& OutBytes,
		std::string& OutError) -> bool;
	auto AppendDependency(
		FImportedSceneData& Scene,
		EImportedDependencyRole Role,
		std::string StableIdentity,
		std::string SourcePath,
		FByteView Bytes,
		uint32* OutIndex = nullptr) -> bool;
	auto MakeDependencySourcePath(
		std::string_view RootSourcePath,
		std::string_view RelativeUri) -> std::string;
	auto IsValidSourcePath(std::string_view SourcePath) -> bool;
	auto ResolveDependencyPath(
		const std::filesystem::path& RootFile,
		std::string_view Uri,
		std::filesystem::path& OutPath,
		std::string& OutNormalized) -> bool;
	auto EncodingFromMimeOrPath(
		std::string_view MimeType,
		std::string_view Path,
		EImportedImageEncoding& OutEncoding) -> bool;
	auto ResolveImageEncoding(
		std::string_view MimeType,
		std::string_view Path,
		EImportedImageEncoding& OutEncoding) -> bool;
	auto ValidateImageBytes(
		EImportedImageEncoding Encoding,
		FByteView Bytes,
		std::string& OutError) -> bool;
	auto MakeUniqueName(
		std::string Name,
		uint32 Index,
		std::unordered_map<std::string, uint32>& NameCounts) -> std::string;

	auto ImportGltfFormat(
		const FImportedSceneContext& Context,
		bool bGlb,
		std::vector<uint32>& OutSourcePrimitiveMaterialIndices,
		FByteBuffer& OutAssimpProjection) -> bool;
	auto ImportAssimpFormat(
		const aiScene& Scene,
		const FImportedSceneContext& Context) -> bool;
	auto ImportAssimpGeometry(
		const aiScene& Scene,
		const FMeshImportOptions& Options,
		std::span<const uint32> SourceMaterialIndices,
		FImportedSceneData& OutScene,
		std::string& OutError) -> bool;
}
