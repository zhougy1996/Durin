#pragma once

#include "CoreMinimal.h"

#include "Shader/ShaderDiagnostics.h"

#include "RHIDefinitions.h"
#include "RHIResources.h"
#include "RenderCoreAPI.h"
#include "ShaderFormat.h"

namespace Durin
{
	// Represents a shader preprocessor definition with an optional explicit value.
	struct FShaderMacroDefinition
	{
		FShaderMacroDefinition() = default;

		FShaderMacroDefinition(std::string_view InName)
			: Name(InName)
		{
		}

		FShaderMacroDefinition(std::string_view InName, std::string_view InValue)
			: Name(InName)
			, Value(InValue)
		{
		}

		auto HasValue() const -> bool
		{
			return Value.has_value();
		}

		std::string Name;
		std::optional<std::string> Value = std::nullopt;
	};

	// Owned, lexically normalized source paths used by a sealed compiler session.
	// A non-null artifact set disables filesystem fallback, including for imports.
	class FShaderSourceArtifacts
	{
	public:
		explicit FShaderSourceArtifacts(const std::map<std::string, FByteBuffer>& InFiles,
			std::vector<std::string> InSearchRoots = {})
			: Files(Adopt(InFiles)), SearchRoots(std::move(InSearchRoots)) {}
		explicit FShaderSourceArtifacts(std::map<std::string, FByteBuffer>&& InFiles,
			std::vector<std::string> InSearchRoots = {})
			: Files(Adopt(std::move(InFiles))), SearchRoots(std::move(InSearchRoots)) {}
		explicit FShaderSourceArtifacts(std::map<std::string, FSharedByteBuffer> InFiles,
			std::vector<std::string> InSearchRoots = {})
			: Files(std::move(InFiles)), SearchRoots(std::move(InSearchRoots)) {}
		auto GetFiles() const -> const std::map<std::string, FSharedByteBuffer>& { return Files; }
		auto GetSearchRoots() const -> const std::vector<std::string>& { return SearchRoots; }

	private:
		static auto Adopt(std::map<std::string, FByteBuffer> InFiles) -> std::map<std::string, FSharedByteBuffer>
		{
			std::map<std::string, FSharedByteBuffer> Result;
			for (auto& [Path, Bytes] : InFiles) Result.emplace(Path, FSharedByteBuffer::Take(std::move(Bytes)));
			return Result;
		}
		const std::map<std::string, FSharedByteBuffer> Files;
		const std::vector<std::string> SearchRoots;
	};

	// Carries source identity, entry points, variants, and cache policy into compilation.
	struct FShaderCompileOptions
	{
		RENDERCORE_API FShaderCompileOptions();
		FShaderTargetIdentity Target;
		// Stable cache identity resolved by the caller. Leave empty to disable disk-backed shader cache reads and writes.
		std::string VirtualShaderPath;
		// Requested source-level entry points, such as `vertexMain` or `fragmentMain`.
		std::vector<const char8*> EntryPoints;
		std::vector<EShaderFrequency> Frequencies;
		std::vector<FShaderMacroDefinition> Macros;
		// Backend/compiler build identity. The compile service fills this before cache lookup.
		std::string CompilerEnvironment;
		std::shared_ptr<const FShaderSourceArtifacts> SourceArtifacts;
		bool bForceRecompile = false;
	};

	RENDERCORE_API auto ShareShaderCode(const FShaderCode& Shader)
		-> std::shared_ptr<const FShaderCode>;

	// Owns compiled stages and a structured compilation outcome.
	struct FShaderCompilerOutput
	{
		std::vector<FShaderCode> CompiledShaders;
		FShaderError Error{.Code = EShaderError::CompilationNotStarted};

		operator bool() const { return Error.IsSuccess(); }
	};

	// Build completion is independent of compiler diagnostics. Only Ok owns stages.
	struct FShaderBuildResult
	{
		EShaderBuildStatus Status = EShaderBuildStatus::Error;
		std::vector<FShaderCode> CompiledShaders;
		std::string Diagnostic;

		explicit operator bool() const { return Status == EShaderBuildStatus::Ok; }
		static auto Failure(std::string Text) -> FShaderBuildResult
		{
			if (Text.size() > ShaderBuildDiagnosticMaxBytes) Text.resize(ShaderBuildDiagnosticMaxBytes);
			return {.Diagnostic = std::move(Text)};
		}
		static auto Success(FShaderCompilerOutput Output) -> FShaderBuildResult
		{
			return {.Status = EShaderBuildStatus::Ok, .CompiledShaders = std::move(Output.CompiledShaders)};
		}
	};

	struct FShaderSourceDependencyFingerprint
	{
		std::string VirtualPath;
		FXxHash128 ContentHash;

		auto operator==(const FShaderSourceDependencyFingerprint&) const
			-> bool = default;
	};

	struct FShaderSourceTreeDescription
	{
		std::string VirtualPath;
		FXxHash128 ContentHash;
		std::vector<std::string> EntryPoints;
		std::vector<EShaderFrequency> Frequencies;
		std::vector<FShaderMacroDefinition> Macros;
		// The verified tree declares every import available to the generated root.
		bool bCompleteClosure = false;
	};

	// Owns an in-memory generated root. The ShaderBuild module resolves imports,
	// compiles, reflects, and caches it without materializing authored source on disk.
	struct FGeneratedShaderCompileRequest
	{
		FShaderTargetIdentity Target;
		std::string VirtualPath;
		std::string Source;
		std::vector<std::string> EntryPoints;
		std::vector<EShaderFrequency> Frequencies;
		std::vector<FShaderMacroDefinition> Macros;
		std::vector<std::string> AllowedImportVirtualPrefixes;
		std::shared_ptr<const FShaderSourceArtifacts> SourceArtifacts;
		bool bForceRecompile = false;
		// Optional caller-declared environment, verified against immutable source bytes.
		std::optional<FShaderSourceTreeDescription> ExpectedSourceTree;
		uint64 MaximumOutputBytes = 256ull * 1024 * 1024;
	};

	// Produces value-owned compiler and reachable-source identity without
	// exposing physical cache paths to higher-level compilers.
	RENDERCORE_API auto BuildShaderSourceDependencyManifest(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult;
	// Returns one stable fingerprint for the entire reachable source tree. The
	// ordinary shader dependency manifest is reused so warm calls only validate
	// persisted file metadata instead of parsing imports again.
	RENDERCORE_API auto BuildShaderSourceTreeFingerprint(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult;
	// Source-tree fingerprints are memoized within this generation. Explicit
	// shader reload advances it so the next request revalidates persisted state.
	RENDERCORE_API auto GetShaderReloadGeneration() -> uint64;
	RENDERCORE_API auto AdvanceShaderReloadGeneration() -> uint64;
}
