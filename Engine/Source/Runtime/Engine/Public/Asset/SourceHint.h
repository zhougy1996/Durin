#pragma once

#include "EngineAPI.h"

namespace Durin
{
	enum class ESourceHintBase : uint8;
	inline constexpr size_t MaximumSourceHintBytes = 4'096;

	enum class ESourceHintError : uint8
	{
		None, EmptyPath, InvalidPaths, FileSystem, RelativePathUnavailable,
		OutsideProject, InvalidBase, InvalidHint, EscapesProject,
	};
	enum class ESourceHintOperation : uint8 { Make, Resolve };
	enum class ESourceHintPath : uint8 { Source, Package, Project };
	struct FSourceHintError
	{
		ESourceHintError Code = ESourceHintError::None;
		ESourceHintOperation Operation = ESourceHintOperation::Make;
		ESourceHintPath Path = ESourceHintPath::Source;
		std::string Input;
		std::string PackagePath;
		std::string ProjectPath;
		std::optional<ESourceHintBase> Base;
		std::error_code SystemError;
	};
	struct FSourceHintResult
	{
		FSourceHintError Error;
		auto Succeeded() const -> bool { return Error.Code == ESourceHintError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	ENGINE_API auto FormatSourceHintError(const FSourceHintError& Error) -> std::string;

	// Classifies a captured source against an explicit or canonical default base.
	ENGINE_API auto MakeSourceHint(
		std::string_view PhysicalPath,
		std::string_view OwningPackagePhysicalPath,
		ESourceHintBase& OutBase,
		std::string& OutHint,
		std::optional<ESourceHintBase> RequestedBase = {}) -> FSourceHintResult;
	// Resolves an optional hint for an explicit reimport action only.
	ENGINE_API auto ResolveSourceHint(
		ESourceHintBase Base,
		std::string_view Hint,
		std::string_view OwningPackagePhysicalPath,
		std::string& OutPhysicalPath) -> FSourceHintResult;
}
