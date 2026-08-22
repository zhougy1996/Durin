#pragma once

#include "CoreAPI.h"

namespace Durin
{
	namespace PathUtilities
	{
		inline constexpr std::string_view ProjectContentMountRoot = "/Game/";

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

		enum class EPathExistence : uint8
		{
			AllowMissing,
			RequireFile
		};

		// Maps one virtual namespace to one configurable content directory beneath a normalized mount root.
		struct FMountPoint
		{
			std::string VirtualRoot;
			EMountOwner Owner = EMountOwner::Test;
			std::filesystem::path Root;
			std::filesystem::path ContentPath = ".";
			bool bAutoScan = false;
			bool bAuthoringWritable = false;
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
		struct FSourcePathResult : FMountPathResult {};

		struct FMountPolicyResult
		{
			const FMountPoint* ReferencingMount = nullptr;
			const FMountPoint* ReferencedMount = nullptr;
			EMountPathError Error = EMountPathError::None;
			std::string Message;

			explicit operator bool() const { return Error == EMountPathError::None; }
		};

		CORE_API auto GetRegisteredMountPoints() -> std::span<const FMountPoint>;
		CORE_API auto FindMountForVirtualPath(std::string_view VirtualPath) -> FMountLookupResult;
		CORE_API auto ResolveAssetPath(
			std::string_view VirtualPath,
			EPathExistence Existence = EPathExistence::AllowMissing
		) -> FAssetPathResult;
		CORE_API auto ResolveSourcePath(
			std::string_view VirtualPath,
			EPathExistence Existence = EPathExistence::RequireFile
		) -> FSourcePathResult;
		CORE_API auto ClassifyAssetPath(const std::filesystem::path& PhysicalPath) -> FAssetPathResult;
		CORE_API auto ClassifySourcePath(const std::filesystem::path& PhysicalPath) -> FSourcePathResult;
		CORE_API auto CheckMountDependency(
			std::string_view ReferencingVirtualPath,
			std::string_view ReferencedVirtualPath
		) -> FMountPolicyResult;
		CORE_API auto CheckAuthoringMutation(
			std::string_view AuthoringVirtualPath,
			std::string_view SourceVirtualPath,
			bool bEngineAuthoringContext = false
		) -> FMountPolicyResult;

		CORE_API auto PublishMountRegistry(std::span<const FMountPoint> Definitions, std::string* OutError = nullptr) -> bool;
		CORE_API auto ValidateDefaultMountPoints(std::string* OutError = nullptr) -> bool;
		CORE_API auto InitDefaultMountPoints(std::string* OutError = nullptr) -> bool;

		// Adds a single-root mount to the mutable registry owned by a scoped test fixture.
		CORE_API auto RegisterMountPointForTests(
			std::string_view VirtualRoot,
			std::string_view PhysicalPath,
			bool bAutoScan = true,
			bool bAuthoringWritable = true
		) -> void;

		class CORE_API FScopedMountRegistryFixture
		{
		public:
			FScopedMountRegistryFixture();
			explicit FScopedMountRegistryFixture(std::span<const FMountPoint> Definitions);
			~FScopedMountRegistryFixture();

			FScopedMountRegistryFixture(const FScopedMountRegistryFixture&) = delete;
			auto operator=(const FScopedMountRegistryFixture&) -> FScopedMountRegistryFixture& = delete;

			auto IsValid() const -> bool { return Error.empty(); }
			auto GetError() const -> const std::string& { return Error; }

		private:
			std::vector<FMountPoint> SavedMounts;
			bool bSavedPublished = false;
			std::string Error;
		};
	}

	// Resolves engine and active-project locations from process-wide path state.
	class FPaths
	{
	public:
		static CORE_API auto SetProjectFile(std::string_view ProjectFile, std::string* OutError = nullptr) -> bool;
		static CORE_API auto ProjectFile() -> std::string;

		static CORE_API auto LaunchDir() ->  std::string;
		static CORE_API auto LaunchSavedDir() -> std::string;
		static CORE_API auto LaunchConfigsDir() -> std::string;
		static CORE_API auto LaunchLogsDir() -> std::string;

		static CORE_API auto RootDir() -> std::string;

		static CORE_API auto EngineDir() -> std::string;

		static CORE_API auto ProjectDir() -> std::string;
		static CORE_API auto DerivedDataCacheDir() -> std::string;
		static CORE_API auto SetDerivedDataCacheDirForTests(std::string_view Directory) -> void;

		static CORE_API auto EngineContentDir() -> std::string;

		static CORE_API auto EngineBinariesDir() -> std::string;

		static CORE_API auto EngineThirdPartyRuntimeBinariesDir() -> std::string;

	};
}
