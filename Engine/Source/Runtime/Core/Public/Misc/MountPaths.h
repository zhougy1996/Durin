#pragma once

#include "CoreAPI.h"

namespace Durin
{
	enum class EMountOwner : uint8
	{
		Engine,
		ActiveProject,
		Extension,
		ExternalSources,
		Test
	};

	enum class EMountPathError : uint8
	{
		None,
		InvalidVirtualPath,
		UnknownMount,
		UnavailableRoot,
		InvalidRelativePath,
		EscapedRoot,
		MissingFile,
		ForbiddenDependency,
		ReadOnlyMount,
		IoFailure
	};

	enum class EMountPathExistence : uint8
	{
		AllowMissing,
		RequireFile
	};

	struct FMountPoint
	{
		std::string VirtualRoot;
		EMountOwner Owner = EMountOwner::Test;
		std::filesystem::path Root;
		std::filesystem::path ContentPath = ".";
		bool bAutoScan = false;
		bool bContentWritable = false;
		std::vector<std::string> Dependencies;
		[[nodiscard]] auto GetContentDir() const -> std::filesystem::path
		{
			return (Root / ContentPath).lexically_normal();
		}
	};
	struct FMountLookupResult
	{
		const FMountPoint* Mount = nullptr;
		std::string NormalizedVirtualPath;
		std::filesystem::path RelativePath;
		EMountPathError Error = EMountPathError::None;
		std::string Message;
		explicit operator bool() const { return Error == EMountPathError::None && Mount != nullptr; }
	};
	struct FMountPathResult
	{
		const FMountPoint* Mount = nullptr;
		std::string NormalizedVirtualPath;
		std::filesystem::path RelativePath;
		std::filesystem::path PhysicalPath;
		EMountPathError Error = EMountPathError::None;
		std::string Message;
		explicit operator bool() const { return Error == EMountPathError::None && Mount != nullptr; }
	};
	struct FAssetPathResult : FMountPathResult {};
	struct FMountPolicyResult
	{
		const FMountPoint* ReferencingMount = nullptr;
		const FMountPoint* ReferencedMount = nullptr;
		EMountPathError Error = EMountPathError::None;
		std::string Message;
		explicit operator bool() const { return Error == EMountPathError::None; }
	};

	class FMountPaths
	{
	public:
		FMountPaths() = delete;
		inline static constexpr std::string_view ProjectContentMountRoot = "/Game/";
		static CORE_API auto GetRegisteredMountPoints() -> std::span<const FMountPoint>;
		static CORE_API auto FindMountForVirtualPath(std::string_view VirtualPath) -> FMountLookupResult;
		static CORE_API auto ResolveAssetPath(std::string_view VirtualPath,
			EMountPathExistence Existence = EMountPathExistence::AllowMissing) -> FAssetPathResult;
		static CORE_API auto ClassifyAssetPath(const std::filesystem::path& PhysicalPath) -> FAssetPathResult;
		static CORE_API auto CheckMountDependency(std::string_view ReferencingVirtualPath,
			std::string_view ReferencedVirtualPath) -> FMountPolicyResult;
		static CORE_API auto PublishMountRegistry(std::span<const FMountPoint> Definitions,
			std::string* OutError = nullptr) -> bool;
		static CORE_API auto ValidateDefaultMountPoints(std::string* OutError = nullptr) -> bool;
		static CORE_API auto InitDefaultMountPoints(std::string* OutError = nullptr) -> bool;
	};
}
