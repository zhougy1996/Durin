#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathsInternal.h"

#include "HAL/PlatformProcess.h"
#include "Json/Json.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		std::filesystem::path ActiveProjectFile;
		std::filesystem::path DerivedDataCacheOverride;

		auto IsEngineDirCandidate(const std::filesystem::path& Candidate) -> bool
		{
			return std::filesystem::is_directory(Candidate)
				&& std::filesystem::is_directory(Candidate / "Content")
				&& std::filesystem::is_directory(Candidate / "Binaries");
		}

		auto FindEngineDirFromLaunchDir() -> std::filesystem::path
		{
			const std::filesystem::path LaunchPath(FPaths::LaunchDir());

			for (auto Current = LaunchPath; !Current.empty(); Current = Current.parent_path())
			{
				const std::filesystem::path EngineDir = Current / "Engine";
				if (IsEngineDirCandidate(EngineDir))
				{
					return EngineDir;
				}

				if (Current == Current.parent_path())
				{
					break;
				}
			}

			checkf(false, "Failed to locate Engine directory from launch directory.");
			return {};
		}

		auto FindOutputRootDirFromLaunchDir() -> std::filesystem::path
		{
			const std::filesystem::path LaunchPath = std::filesystem::path(FPaths::LaunchDir()).lexically_normal();
			const std::filesystem::path OutputDir = LaunchPath.filename().empty()
				? LaunchPath.parent_path()
				: LaunchPath;

			for (auto Current = OutputDir; !Current.empty(); Current = Current.parent_path())
			{
				if (Current.filename() == "Tests")
				{
					return Current.parent_path();
				}

				if (Current == Current.parent_path())
				{
					break;
				}
			}

			const std::filesystem::path RuntimeDir = OutputDir.parent_path();

			checkf(RuntimeDir.filename() == "Runtime",
				"Expected launch directory to be under a Runtime/<Profile> or Tests/<Profile>/<Target>/Bin layout. LaunchDir={}",
				FPaths::LaunchDir());

			return RuntimeDir.parent_path();
		}
	}

	auto FPaths::SetProjectFile(std::string_view ProjectFile, std::string* OutError) -> bool
	{
		if (ProjectFile.empty())
		{
			ActiveProjectFile.clear();
			return true;
		}

		const std::filesystem::path Candidate = std::filesystem::absolute(ProjectFile).lexically_normal();
		if (Candidate.extension() != ".dproject" || !std::filesystem::is_regular_file(Candidate))
		{
			if (OutError) *OutError = std::format("Project file does not exist or is not a .dproject file: {}", Candidate.generic_string());
			return false;
		}
		ActiveProjectFile = Candidate;
		return true;
	}

	auto FPaths::ProjectFile() -> std::string
	{
		return ActiveProjectFile.generic_string();
	}

	namespace
	{
		std::vector<FMountPoint> MountPoints;
		bool bRegistryPublished = false;
		bool bSuppressMountLog = false;

		auto FoldAscii(std::string_view Text) -> std::string
		{
			std::string Folded(Text);
			std::ranges::transform(Folded, Folded.begin(), [](const char Character) {
				return Character >= 'A' && Character <= 'Z' ? static_cast<char>(Character - 'A' + 'a') : Character;
			});
			return Folded;
		}

		auto SamePathComponent(const std::filesystem::path& A, const std::filesystem::path& B) -> bool
		{
#if PLATFORM_WINDOWS
			return FoldAscii(A.generic_string()) == FoldAscii(B.generic_string());
#else
			return A == B;
#endif
		}

		auto IsPathWithin(const std::filesystem::path& Root, const std::filesystem::path& Candidate) -> bool
		{
			auto RootIt = Root.begin();
			auto CandidateIt = Candidate.begin();
			for (; RootIt != Root.end(); ++RootIt, ++CandidateIt)
			{
				if (CandidateIt == Candidate.end() || !SamePathComponent(*RootIt, *CandidateIt)) return false;
			}
			return true;
		}

		auto FailLookup(EMountPathError Error, std::string Message) -> FMountLookupResult
		{
			return {.Error = Error, .Message = std::move(Message)};
		}

		template<typename TResult>
		auto FailPath(
			const FMountLookupResult& Lookup,
			EMountPathError Error,
			std::string Message
		) -> TResult
		{
			TResult Result;
			Result.Mount = Lookup.Mount;
			Result.NormalizedVirtualPath = Lookup.NormalizedVirtualPath;
			Result.RelativePath = Lookup.RelativePath;
			Result.Error = Error;
			Result.Message = std::move(Message);
			return Result;
		}

		auto ValidateVirtualRoot(std::string_view Root, std::string* OutError) -> bool
		{
			if (Root.size() < 3 || Root.front() != '/' || Root.back() != '/' || Root.find('\\') != std::string_view::npos)
			{
				if (OutError) *OutError = std::format("Mount root '{}' must be absolute and use leading/trailing forward slashes.", Root);
				return false;
			}
			size_t Start = 1;
			while (Start + 1 < Root.size())
			{
				const size_t End = Root.find('/', Start);
				const std::string_view Segment = Root.substr(Start, End - Start);
				if (Segment.empty() || Segment == "." || Segment == "..")
				{
					if (OutError) *OutError = std::format("Mount root '{}' contains an invalid segment.", Root);
					return false;
				}
				Start = End + 1;
			}
			return true;
		}

		auto NormalizeAbsolute(const std::filesystem::path& Path, std::error_code& Error) -> std::filesystem::path
		{
			std::filesystem::path Absolute = std::filesystem::absolute(Path, Error);
			if (Error) return {};
			Absolute = Absolute.lexically_normal();
			if (Absolute.filename().empty()) Absolute = Absolute.parent_path();
			return Absolute;
		}

		auto ValidateRelativeDefinitionPath(std::string_view Text) -> bool
		{
			if (Text.empty()) return false;
			const std::filesystem::path Path(Text);
			if (Path.is_absolute() || Path.has_root_name()) return false;
			for (const std::filesystem::path& Segment : Path)
				if (Segment == ".." || Segment.empty()) return false;
			return true;
		}

		auto CanonicalRoot(const std::filesystem::path& Root, std::error_code& Error) -> std::filesystem::path
		{
			return std::filesystem::weakly_canonical(Root, Error).lexically_normal();
		}

		auto CanonicalCandidateForContainment(
			const std::filesystem::path& Candidate,
			std::error_code& Error
		) -> std::filesystem::path;

		auto ComparableRoot(const std::filesystem::path& Root, std::error_code& Error) -> std::filesystem::path
		{
			Error.clear();
			if (!std::filesystem::exists(Root, Error))
			{
				Error.clear();
				return CanonicalCandidateForContainment(Root, Error);
			}
			const std::filesystem::path Canonical = CanonicalRoot(Root, Error);
			return Error ? std::filesystem::path{} : Canonical;
		}

		auto CanonicalCandidateForContainment(
			const std::filesystem::path& Candidate,
			std::error_code& Error
		) -> std::filesystem::path
		{
			std::filesystem::path Existing = Candidate;
			std::vector<std::filesystem::path> Missing;
			auto Exists = [&](const std::filesystem::path& Path) {
				const bool bExists = std::filesystem::exists(Path, Error);
				if (Error == std::errc::no_such_file_or_directory
					|| Error == std::errc::not_a_directory) Error.clear();
				return bExists;
			};
			while (!Existing.empty() && !Exists(Existing))
			{
				if (Error) return {};
				Missing.push_back(Existing.filename());
				const std::filesystem::path Parent = Existing.parent_path();
				if (Parent == Existing) break;
				Existing = Parent;
			}
			if (Existing.empty() || !Exists(Existing)) return {};
			std::filesystem::path Resolved = std::filesystem::canonical(Existing, Error);
			if (Error) return {};
			for (auto It = Missing.rbegin(); It != Missing.rend(); ++It) Resolved /= *It;
			return Resolved.lexically_normal();
		}

		template<typename TResult>
		auto ResolveMountPath(
			std::string_view VirtualPath,
			EMountPathExistence Existence
		) -> TResult
		{
			const FMountLookupResult Lookup = FMountPaths::FindMountForVirtualPath(VirtualPath);
			if (!Lookup) return FailPath<TResult>(Lookup, Lookup.Error, Lookup.Message);
			const std::filesystem::path Root = Lookup.Mount->GetContentDir();

			std::error_code Error;
			if (!std::filesystem::exists(Root, Error))
			{
				return FailPath<TResult>(
					Lookup,
					Error ? EMountPathError::IoFailure : EMountPathError::UnavailableRoot,
					Error ? Error.message() : "The requested mount root is unavailable.");
			}

			const std::filesystem::path CanonicalMountRoot = CanonicalRoot(Root, Error);
			if (Error) return FailPath<TResult>(Lookup, EMountPathError::IoFailure, Error.message());
			const std::filesystem::path Candidate = (Root / Lookup.RelativePath).lexically_normal();
			const std::filesystem::path CanonicalCandidate = CanonicalCandidateForContainment(Candidate, Error);
			if (Error || CanonicalCandidate.empty())
				return FailPath<TResult>(Lookup, EMountPathError::IoFailure, Error ? Error.message() : "Failed to resolve physical path.");
			if (!IsPathWithin(CanonicalMountRoot, CanonicalCandidate))
				return FailPath<TResult>(Lookup, EMountPathError::EscapedRoot, "Physical path escapes its mount root.");
			if (Existence == EMountPathExistence::RequireFile && !std::filesystem::is_regular_file(Candidate, Error))
			{
				if (Error == std::errc::no_such_file_or_directory
					|| Error == std::errc::not_a_directory) Error.clear();
				return FailPath<TResult>(
					Lookup,
					Error ? EMountPathError::IoFailure : EMountPathError::MissingFile,
					Error ? Error.message() : "The requested file does not exist.");
			}

			TResult Result;
			Result.Mount = Lookup.Mount;
			Result.NormalizedVirtualPath = Lookup.NormalizedVirtualPath;
			Result.RelativePath = Lookup.RelativePath;
			Result.PhysicalPath = Candidate;
			return Result;
		}

		template<typename TResult>
		auto ClassifyMountPath(const std::filesystem::path& PhysicalPath) -> TResult
		{
			std::error_code Error;
			const std::filesystem::path Candidate = CanonicalCandidateForContainment(PhysicalPath, Error);
			if (Error || Candidate.empty())
			{
				TResult Result;
				Result.Error = EMountPathError::IoFailure;
				Result.Message = Error ? Error.message() : "Failed to classify physical path.";
				return Result;
			}

			const FMountPoint* Best = nullptr;
			std::filesystem::path BestRoot;
			for (const FMountPoint& Mount : MountPoints)
			{
				const std::filesystem::path ContentDir = Mount.GetContentDir();
				if (!std::filesystem::exists(ContentDir, Error)) { Error.clear(); continue; }
				const std::filesystem::path CanonicalMountRoot = CanonicalRoot(ContentDir, Error);
				if (Error) { Error.clear(); continue; }
				if (IsPathWithin(CanonicalMountRoot, Candidate)
					&& (!Best || CanonicalMountRoot.native().size() > BestRoot.native().size()))
				{
					Best = &Mount;
					BestRoot = CanonicalMountRoot;
				}
			}
			if (!Best)
			{
				TResult Result;
				Result.Error = EMountPathError::UnknownMount;
				Result.Message = "Physical path is outside every registered mount root.";
				return Result;
			}

			std::filesystem::path Relative = Candidate.lexically_relative(BestRoot);
			if (Relative == ".") Relative.clear();
			TResult Result;
			Result.Mount = Best;
			Result.RelativePath = Relative;
			Result.NormalizedVirtualPath = Relative.empty()
				? Best->VirtualRoot
				: Best->VirtualRoot + Relative.generic_string();
			Result.PhysicalPath = PhysicalPath.lexically_normal();
			return Result;
		}

		auto ParseProjectMounts(std::vector<FMountPoint>& Definitions, std::string* OutError) -> bool
		{
			if (FPaths::ProjectFile().empty()) return true;
			FJsonDocument Descriptor;
			FJsonParseError ParseError;
			if (!Descriptor.LoadFromFile(FPaths::ProjectFile(), &ParseError))
			{
				if (OutError) *OutError = std::format("Invalid project descriptor: {}", ParseError.Message);
				return false;
			}
			const FJsonNodeView Mounts = Descriptor.GetRootView().GetView("Mounts");
			if (!Mounts.IsValid()) return true;
			if (!Mounts.IsArray())
			{
				if (OutError) *OutError = "Project descriptor Mounts must be an array.";
				return false;
			}

			const std::filesystem::path ProjectRoot = std::filesystem::path(FPaths::ProjectFile()).parent_path();
			for (size_t Index = 0; Index < Mounts.Num(); ++Index)
			{
				const FJsonNodeView Entry = Mounts.GetView(Index);
				if (!Entry.IsObject())
				{
					if (OutError) *OutError = std::format("Mounts[{}] must be an object.", Index);
					return false;
				}
				const bool bHasContentWritable = Entry.GetView("ContentWritable").IsValid();
				const bool bHasLegacyAuthoringWritable = Entry.GetView("AuthoringWritable").IsValid();
				if (bHasContentWritable && bHasLegacyAuthoringWritable)
				{
					if (OutError) *OutError = std::format(
						"Mounts[{}] cannot contain both ContentWritable and legacy AuthoringWritable.",
						Index);
					return false;
				}
				if (Entry.Num() != 7)
				{
					if (OutError) *OutError = std::format("Mounts[{}] must contain exactly seven fields.", Index);
					return false;
				}
				constexpr std::array<std::string_view, 8> Fields{
					"VirtualRoot", "Owner", "Root", "ContentPath", "AutoScan",
					"ContentWritable", "AuthoringWritable", "Dependencies"};
				bool bUnknownField = false;
				Entry.ForEachObjectMember([&](const std::string_view Key, FJsonNodeView) {
					if (std::ranges::find(Fields, Key) == Fields.end()) bUnknownField = true;
				});
				if (bUnknownField)
				{
					if (OutError) *OutError = std::format("Mounts[{}] contains an unknown field.", Index);
					return false;
				}

				std::string VirtualRoot;
				std::string OwnerText;
				std::string RootText;
				std::string ContentPathText;
				bool bAutoScan = false;
				bool bContentWritable = false;
				const FJsonNodeView Dependencies = Entry.GetView("Dependencies");
				const std::string_view ContentWritableKey = bHasContentWritable
					? "ContentWritable" : "AuthoringWritable";
				if (!Entry.GetChildValue("VirtualRoot", VirtualRoot)
					|| !Entry.GetChildValue("Owner", OwnerText)
					|| !Entry.GetChildValue("Root", RootText)
					|| !Entry.GetChildValue("ContentPath", ContentPathText)
					|| !Entry.GetChildValue("AutoScan", bAutoScan)
					|| !Entry.GetChildValue(ContentWritableKey, bContentWritable)
					|| !Dependencies.IsArray()
					|| !ValidateRelativeDefinitionPath(RootText)
					|| !ValidateRelativeDefinitionPath(ContentPathText))
				{
					if (OutError) *OutError = std::format("Mounts[{}] has invalid field types or paths.", Index);
					return false;
				}
				EMountOwner Owner;
				if (OwnerText == "Extension") Owner = EMountOwner::Extension;
				else if (OwnerText == "ExternalSources") Owner = EMountOwner::ExternalSources;
				else
				{
					if (OutError) *OutError = std::format("Mounts[{}].Owner is invalid.", Index);
					return false;
				}
				FMountPoint Definition{
					.VirtualRoot = std::move(VirtualRoot),
					.Owner = Owner,
					.Root = (ProjectRoot / RootText).lexically_normal(),
					.ContentPath = std::filesystem::path(ContentPathText).lexically_normal(),
					.bAutoScan = bAutoScan,
					.bContentWritable = bContentWritable};
				for (size_t DependencyIndex = 0; DependencyIndex < Dependencies.Num(); ++DependencyIndex)
				{
					const FJsonNodeView Dependency = Dependencies.GetView(DependencyIndex);
					if (!Dependency.IsString())
					{
						if (OutError) *OutError = std::format("Mounts[{}].Dependencies must contain strings.", Index);
						return false;
					}
					Definition.Dependencies.push_back(Dependency.GetString());
				}
				Definitions.push_back(std::move(Definition));
			}
			return true;
		}

		auto BuildDefaultMountDefinitions(
			std::vector<FMountPoint>& Definitions,
			std::string* OutError
		) -> bool
		{
			const std::filesystem::path EngineRoot = FPaths::EngineDir();
			Definitions = {{
				.VirtualRoot = "/Engine/",
				.Owner = EMountOwner::Engine,
				.Root = EngineRoot,
				.ContentPath = "Content",
				.bAutoScan = true,
				.bContentWritable = true}};
			if (FPaths::ProjectFile().empty()) return true;

			const std::filesystem::path ProjectRoot =
				std::filesystem::path(FPaths::ProjectFile()).parent_path();
			if (std::filesystem::weakly_canonical(ProjectRoot)
				== std::filesystem::weakly_canonical(EngineRoot))
				return ParseProjectMounts(Definitions, OutError);
			Definitions.push_back({
				.VirtualRoot = std::string(FMountPaths::ProjectContentMountRoot),
				.Owner = EMountOwner::ActiveProject,
				.Root = ProjectRoot,
				.ContentPath = "Content",
				.bAutoScan = true,
				.bContentWritable = true,
				.Dependencies = {"/Engine/"}});
			if (!ParseProjectMounts(Definitions, OutError)) return false;
			for (size_t Index = 2; Index < Definitions.size(); ++Index)
				Definitions[1].Dependencies.push_back(Definitions[Index].VirtualRoot);
			return true;
		}
	}

	namespace MountPathInternal
	{
		auto MutableMountPoints() -> std::vector<FMountPoint>& { return MountPoints; }
		auto RegistryPublished() -> bool& { return bRegistryPublished; }
	}

	auto FMountPaths::GetRegisteredMountPoints() -> std::span<const FMountPoint> { return MountPoints; }

	auto FMountPaths::FindMountForVirtualPath(std::string_view VirtualPath) -> FMountLookupResult
	{
		if (VirtualPath.empty() || VirtualPath.front() != '/' || VirtualPath.find('\\') != std::string_view::npos)
			return FailLookup(EMountPathError::InvalidVirtualPath, "Virtual path must be absolute and use forward slashes.");
		if (VirtualPath.back() == '/') return FailLookup(EMountPathError::InvalidRelativePath, "Virtual path must name an entry.");

		const std::string FoldedPath = FoldAscii(VirtualPath);
		for (const FMountPoint& Mount : MountPoints)
		{
			const std::string FoldedRoot = FoldAscii(Mount.VirtualRoot);
			if (!FoldedPath.starts_with(FoldedRoot)) continue;
			const std::string_view RelativeText = VirtualPath.substr(Mount.VirtualRoot.size());
			if (RelativeText.empty()) return FailLookup(EMountPathError::InvalidRelativePath, "Virtual path has no relative entry.");
			size_t Start = 0;
			while (Start < RelativeText.size())
			{
				const size_t End = RelativeText.find('/', Start);
				const std::string_view Segment = RelativeText.substr(
					Start, End == std::string_view::npos ? RelativeText.size() - Start : End - Start);
				if (Segment.empty() || Segment == "." || Segment == "..")
					return FailLookup(EMountPathError::InvalidRelativePath, "Virtual path contains an invalid segment.");
				Start = End == std::string_view::npos ? RelativeText.size() : End + 1;
			}
			return {
				.Mount = &Mount,
				.NormalizedVirtualPath = Mount.VirtualRoot + std::string(RelativeText),
				.RelativePath = std::filesystem::path(RelativeText)};
		}
		return FailLookup(EMountPathError::UnknownMount, "Virtual path does not use a registered mount.");
	}

	auto FMountPaths::ResolveAssetPath(std::string_view VirtualPath, EMountPathExistence Existence) -> FAssetPathResult
	{
		return ResolveMountPath<FAssetPathResult>(VirtualPath, Existence);
	}

	auto FMountPaths::ClassifyAssetPath(const std::filesystem::path& PhysicalPath) -> FAssetPathResult
	{
		return ClassifyMountPath<FAssetPathResult>(PhysicalPath);
	}

	auto FMountPaths::CheckMountDependency(
		std::string_view ReferencingVirtualPath,
		std::string_view ReferencedVirtualPath
	) -> FMountPolicyResult
	{
		const FMountLookupResult Referencing = FMountPaths::FindMountForVirtualPath(ReferencingVirtualPath);
		const FMountLookupResult Referenced = FMountPaths::FindMountForVirtualPath(ReferencedVirtualPath);
		FMountPolicyResult Result{
			.ReferencingMount = Referencing.Mount,
			.ReferencedMount = Referenced.Mount};
		if (!Referencing || !Referenced)
		{
			Result.Error = !Referencing ? Referencing.Error : Referenced.Error;
			Result.Message = !Referencing ? Referencing.Message : Referenced.Message;
			return Result;
		}
		if (FoldAscii(Referencing.Mount->VirtualRoot) == FoldAscii(Referenced.Mount->VirtualRoot)
			|| std::ranges::any_of(Referencing.Mount->Dependencies, [&](const std::string& Dependency) {
				return FoldAscii(Dependency) == FoldAscii(Referenced.Mount->VirtualRoot);
			})) return Result;
		Result.Error = EMountPathError::ForbiddenDependency;
		Result.Message = std::format(
			"Mount {} may not depend on {}.", Referencing.Mount->VirtualRoot, Referenced.Mount->VirtualRoot);
		return Result;
	}

	auto FMountPaths::PublishMountRegistry(std::span<const FMountPoint> Definitions, std::string* OutError) -> bool
	{
		if (bRegistryPublished)
		{
			if (OutError) *OutError = "Mount registry has already been published.";
			return false;
		}
		std::vector<FMountPoint> Validated;
		Validated.reserve(Definitions.size());
		std::unordered_set<std::string> Roots;
		for (FMountPoint Definition : Definitions)
		{
			if (!ValidateVirtualRoot(Definition.VirtualRoot, OutError)) return false;
			const std::string Identity = FoldAscii(Definition.VirtualRoot);
			if (!Roots.insert(Identity).second)
			{
				if (OutError) *OutError = std::format("Duplicate mount root '{}'.", Definition.VirtualRoot);
				return false;
			}
			if (Definition.Root.empty())
			{
				if (OutError) *OutError = std::format("Mount '{}' declares an empty root.", Definition.VirtualRoot);
				return false;
			}
			std::error_code Error;
			Definition.Root = NormalizeAbsolute(Definition.Root, Error);
			if (Error)
			{
				if (OutError) *OutError = Error.message();
				return false;
			}
			if (!ValidateRelativeDefinitionPath(Definition.ContentPath.generic_string()))
			{
				if (OutError) *OutError = std::format(
					"Mount '{}' declares an invalid content path.", Definition.VirtualRoot);
				return false;
			}
			Definition.ContentPath = Definition.ContentPath.lexically_normal();
			const std::filesystem::path CanonicalRootPath = ComparableRoot(Definition.Root, Error);
			if (Error) { if (OutError) *OutError = Error.message(); return false; }
			const std::filesystem::path CanonicalContentDir = ComparableRoot(Definition.GetContentDir(), Error);
			if (Error) { if (OutError) *OutError = Error.message(); return false; }
			if (!IsPathWithin(CanonicalRootPath, CanonicalContentDir))
			{
				if (OutError) *OutError = std::format(
					"Mount '{}' content directory escapes its root.", Definition.VirtualRoot);
				return false;
			}
			for (const std::string& Dependency : Definition.Dependencies)
			{
				if (!ValidateVirtualRoot(Dependency, OutError) || FoldAscii(Dependency) == Identity) return false;
			}
			for (const FMountPoint& Existing : Validated)
			{
				const std::filesystem::path NewCanonical = ComparableRoot(Definition.GetContentDir(), Error);
				if (Error) { if (OutError) *OutError = Error.message(); return false; }
				const std::filesystem::path ExistingCanonical = ComparableRoot(Existing.GetContentDir(), Error);
				if (Error) { if (OutError) *OutError = Error.message(); return false; }
				if (IsPathWithin(NewCanonical, ExistingCanonical)
					|| IsPathWithin(ExistingCanonical, NewCanonical))
				{
					if (OutError) *OutError = std::format(
						"Mounts '{}' and '{}' declare overlapping canonical content directories.",
						Definition.VirtualRoot, Existing.VirtualRoot);
					return false;
				}
			}
			Validated.push_back(std::move(Definition));
		}
		for (const FMountPoint& Definition : Validated)
		{
			for (const std::string& Dependency : Definition.Dependencies)
				if (!Roots.contains(FoldAscii(Dependency)))
				{
					if (OutError) *OutError = std::format("Mount '{}' has unknown dependency '{}'.", Definition.VirtualRoot, Dependency);
					return false;
				}
		}
		std::ranges::sort(Validated, [](const FMountPoint& A, const FMountPoint& B) {
			return A.VirtualRoot.length() > B.VirtualRoot.length();
		});
		MountPoints = std::move(Validated);
		bRegistryPublished = true;
		if (!bSuppressMountLog)
			for (const FMountPoint& Mount : MountPoints) DURIN_DEBUG("Mount point: {}", Mount.VirtualRoot);
		return true;
	}

	auto FMountPaths::InitDefaultMountPoints(std::string* OutError) -> bool
	{
		checkf(IsInGameThread(), "InitDefaultMountPoints must be called from the game thread.");
		if (bRegistryPublished) return true;
		std::vector<FMountPoint> Definitions;
		if (!BuildDefaultMountDefinitions(Definitions, OutError)) return false;
		return FMountPaths::PublishMountRegistry(Definitions, OutError);
	}

	auto FMountPaths::ValidateDefaultMountPoints(std::string* OutError) -> bool
	{
		std::vector<FMountPoint> Definitions;
		if (!BuildDefaultMountDefinitions(Definitions, OutError)) return false;
		std::vector<FMountPoint> SavedMounts = MountPoints;
		const bool bSavedPublished = bRegistryPublished;
		const bool bSavedSuppressMountLog = bSuppressMountLog;
		MountPoints.clear();
		bRegistryPublished = false;
		bSuppressMountLog = true;
		const bool bValid = FMountPaths::PublishMountRegistry(Definitions, OutError);
		MountPoints = std::move(SavedMounts);
		bRegistryPublished = bSavedPublished;
		bSuppressMountLog = bSavedSuppressMountLog;
		return bValid;
	}

	auto FPaths::LaunchDir() -> std::string
	{
		static std::string CachedLaunchDir = []() -> std::string {
			std::string ExePath = FPlatformProcess::ExecutablePath();
			std::string LaunchDir = std::filesystem::path{ExePath}.parent_path().generic_string() + "/";
			return LaunchDir;
		}();
		return CachedLaunchDir;
	}

	auto FPaths::LaunchSavedDir() -> std::string
	{
		static const std::string CachedSavedDir =
			(std::filesystem::path(LaunchDir()) / "Saved").generic_string() + "/";
		return CachedSavedDir;
	}

	auto FPaths::LaunchConfigsDir() -> std::string
	{
		static const std::string CachedConfigsDir =
			(std::filesystem::path(LaunchSavedDir()) / "Configs").generic_string() + "/";
		return CachedConfigsDir;
	}

	auto FPaths::LaunchLogsDir() -> std::string
	{
		static const std::string CachedLogsDir =
			(std::filesystem::path(LaunchSavedDir()) / "Logs").generic_string() + "/";
		return CachedLogsDir;
	}

	auto FPaths::RootDir() -> std::string
	{
		static std::string CachedRootDir = []() -> std::string {
			std::filesystem::path EnginePath{FPaths::EngineDir()};
			if (EnginePath.filename().empty()) EnginePath = EnginePath.parent_path();
			const std::filesystem::path RootDir = EnginePath.parent_path();
			return RootDir.generic_string() + "/";
		}();
		return CachedRootDir;
	}

	auto FPaths::EngineDir() -> std::string
	{
		static std::string CachedEngineDir = []() -> std::string {
			return FindEngineDirFromLaunchDir().generic_string() + "/";
		}();
		return CachedEngineDir;
	}

	auto FPaths::ProjectDir() -> std::string
	{
		if (!ActiveProjectFile.empty()) return ActiveProjectFile.parent_path().generic_string() + "/";
		return EngineDir();
	}

	auto FPaths::DerivedDataCacheDir() -> std::string
	{
		const std::filesystem::path Root = DerivedDataCacheOverride.empty()
			? std::filesystem::path(ProjectDir()) / "DerivedDataCache"
			: DerivedDataCacheOverride;
		return Root.lexically_normal().generic_string() + "/";
	}

	auto FPaths::SetDerivedDataCacheDirForTests(std::string_view Directory) -> void
	{
		DerivedDataCacheOverride = Directory.empty() ? std::filesystem::path{} : std::filesystem::absolute(Directory).lexically_normal();
	}

	auto FPaths::EngineContentDir() -> std::string
	{
		static std::string EngineContentDir = []() -> std::string {
			return std::filesystem::path{EngineDir()}.append("Content/").generic_string();
		}();
		return EngineContentDir;
	}

	auto FPaths::EngineBinariesDir() -> std::string
	{
		return EngineDir() + "Binaries/";
	}

	auto FPaths::EngineThirdPartyRuntimeBinariesDir() -> std::string
	{
		static std::string CachedThirdPartyDir = []() -> std::string {
			const std::filesystem::path OutputRoot = FindOutputRootDirFromLaunchDir();
			const std::filesystem::path ThirdPartyRoot =
				OutputRoot.parent_path() / DURIN_BUILD_CONFIGURATION / "ThirdParty";
			return ThirdPartyRoot.generic_string() + "/";
		}();
		return CachedThirdPartyDir;
	}

} // namespace Durin
