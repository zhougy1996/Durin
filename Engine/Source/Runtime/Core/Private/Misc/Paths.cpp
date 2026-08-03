#include "Misc/Paths.h"

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

	namespace PathUtilities
	{
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

			auto ComparableRoot(const std::filesystem::path& Root, std::error_code& Error) -> std::filesystem::path
			{
				Error.clear();
				if (!std::filesystem::exists(Root, Error))
				{
					Error.clear();
					return Root.lexically_normal();
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
				EPathExistence Existence
			) -> TResult
			{
				const FMountLookupResult Lookup = FindMountForVirtualPath(VirtualPath);
				if (!Lookup) return FailPath<TResult>(Lookup, Lookup.Error, Lookup.Message);
				const std::filesystem::path& Root = Lookup.Mount->Root;

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
				if (Existence == EPathExistence::RequireFile && !std::filesystem::is_regular_file(Candidate, Error))
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
					if (!std::filesystem::exists(Mount.Root, Error)) { Error.clear(); continue; }
					const std::filesystem::path CanonicalMountRoot = CanonicalRoot(Mount.Root, Error);
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
					if (!Entry.IsObject() || Entry.Num() != 6)
					{
						if (OutError) *OutError = std::format("Mounts[{}] must contain exactly six fields.", Index);
						return false;
					}
					constexpr std::array<std::string_view, 6> Fields{
						"VirtualRoot", "Owner", "Root", "AssetPackages", "AuthoringWritable", "Dependencies"};
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
					bool bAssetPackages = false;
					bool bAuthoringWritable = false;
					const FJsonNodeView Dependencies = Entry.GetView("Dependencies");
					if (!Entry.GetChildValue("VirtualRoot", VirtualRoot)
						|| !Entry.GetChildValue("Owner", OwnerText)
						|| !Entry.GetChildValue("Root", RootText)
						|| !Entry.GetChildValue("AssetPackages", bAssetPackages)
						|| !Entry.GetChildValue("AuthoringWritable", bAuthoringWritable)
						|| !Dependencies.IsArray()
						|| !ValidateRelativeDefinitionPath(RootText))
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
						.bAssetPackages = bAssetPackages,
						.bAuthoringWritable = bAuthoringWritable};
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
					.Root = EngineRoot / "Content",
					.bAssetPackages = true,
					.bAuthoringWritable = true}};
				if (FPaths::ProjectFile().empty()) return true;

				const std::filesystem::path ProjectRoot =
					std::filesystem::path(FPaths::ProjectFile()).parent_path();
				Definitions.push_back({
					.VirtualRoot = std::string(ProjectContentMountRoot),
					.Owner = EMountOwner::ActiveProject,
					.Root = ProjectRoot / "Content",
					.bAssetPackages = true,
					.bAuthoringWritable = true,
					.Dependencies = {"/Engine/"}});
				if (!ParseProjectMounts(Definitions, OutError)) return false;
				for (size_t Index = 2; Index < Definitions.size(); ++Index)
					Definitions[1].Dependencies.push_back(Definitions[Index].VirtualRoot);
				return true;
			}
		}

		auto GetRegisteredMountPoints() -> std::span<const FMountPoint> { return MountPoints; }

		auto FindMountForVirtualPath(std::string_view VirtualPath) -> FMountLookupResult
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

		auto ResolveAssetPath(std::string_view VirtualPath, EPathExistence Existence) -> FAssetPathResult
		{
			FAssetPathResult Result = ResolveMountPath<FAssetPathResult>(VirtualPath, Existence);
			if (Result.Mount && !Result.Mount->bAssetPackages)
			{
				Result.Error = EMountPathError::AssetPackagesDisabled;
				Result.Message = "Mount does not permit asset packages.";
			}
			return Result;
		}

		auto ResolveSourcePath(std::string_view VirtualPath, EPathExistence Existence) -> FSourcePathResult
		{
			return ResolveMountPath<FSourcePathResult>(VirtualPath, Existence);
		}

		auto ClassifyAssetPath(const std::filesystem::path& PhysicalPath) -> FAssetPathResult
		{
			FAssetPathResult Result = ClassifyMountPath<FAssetPathResult>(PhysicalPath);
			if (Result.Mount && !Result.Mount->bAssetPackages)
			{
				Result.Error = EMountPathError::AssetPackagesDisabled;
				Result.Message = "Mount does not permit asset packages.";
			}
			return Result;
		}

		auto ClassifySourcePath(const std::filesystem::path& PhysicalPath) -> FSourcePathResult
		{
			return ClassifyMountPath<FSourcePathResult>(PhysicalPath);
		}

		auto CheckMountDependency(
			std::string_view ReferencingVirtualPath,
			std::string_view ReferencedVirtualPath
		) -> FMountPolicyResult
		{
			const FMountLookupResult Referencing = FindMountForVirtualPath(ReferencingVirtualPath);
			const FMountLookupResult Referenced = FindMountForVirtualPath(ReferencedVirtualPath);
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

		auto CheckAuthoringMutation(
			std::string_view AuthoringVirtualPath,
			std::string_view SourceVirtualPath,
			bool bEngineAuthoringContext
		) -> FMountPolicyResult
		{
			FMountPolicyResult Result = CheckMountDependency(AuthoringVirtualPath, SourceVirtualPath);
			if (!Result) return Result;
			if (!Result.ReferencedMount->bAuthoringWritable
				|| Result.ReferencingMount != Result.ReferencedMount
				|| (Result.ReferencedMount->Owner == EMountOwner::Engine && !bEngineAuthoringContext))
			{
				Result.Error = EMountPathError::ReadOnlyMount;
				Result.Message = "The authoring context may not mutate this mount.";
			}
			return Result;
		}

		auto PublishMountRegistry(std::span<const FMountPoint> Definitions, std::string* OutError) -> bool
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
				for (const std::string& Dependency : Definition.Dependencies)
				{
					if (!ValidateVirtualRoot(Dependency, OutError) || FoldAscii(Dependency) == Identity) return false;
				}
				for (const FMountPoint& Existing : Validated)
				{
					const std::filesystem::path NewCanonical = ComparableRoot(Definition.Root, Error);
					if (Error) { if (OutError) *OutError = Error.message(); return false; }
					const std::filesystem::path ExistingCanonical = ComparableRoot(Existing.Root, Error);
					if (Error) { if (OutError) *OutError = Error.message(); return false; }
					if (IsPathWithin(NewCanonical, ExistingCanonical)
						|| IsPathWithin(ExistingCanonical, NewCanonical))
					{
						if (OutError) *OutError = std::format(
							"Mounts '{}' and '{}' declare overlapping canonical roots.",
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

		auto InitDefaultMountPoints(std::string* OutError) -> bool
		{
			checkf(IsInGameThread(), "InitDefaultMountPoints must be called from the game thread.");
			if (bRegistryPublished) return true;
			std::vector<FMountPoint> Definitions;
			if (!BuildDefaultMountDefinitions(Definitions, OutError)) return false;
			return PublishMountRegistry(Definitions, OutError);
		}

		auto ValidateDefaultMountPoints(std::string* OutError) -> bool
		{
			std::vector<FMountPoint> Definitions;
			if (!BuildDefaultMountDefinitions(Definitions, OutError)) return false;
			std::vector<FMountPoint> SavedMounts = MountPoints;
			const bool bSavedPublished = bRegistryPublished;
			const bool bSavedSuppressMountLog = bSuppressMountLog;
			MountPoints.clear();
			bRegistryPublished = false;
			bSuppressMountLog = true;
			const bool bValid = PublishMountRegistry(Definitions, OutError);
			MountPoints = std::move(SavedMounts);
			bRegistryPublished = bSavedPublished;
			bSuppressMountLog = bSavedSuppressMountLog;
			return bValid;
		}

		auto RegisterMountPointForTests(
			std::string_view VirtualRoot,
			std::string_view PhysicalPath,
			bool bAssetPackages,
			bool bAuthoringWritable
		) -> void
		{
			checkf(IsInGameThread(), "RegisterMountPointForTests must be called from the game thread.");
			checkf(!bRegistryPublished, "Mount registry is immutable after publication.");
			std::error_code DirectoryError;
			const std::filesystem::path Root = NormalizeAbsolute(PhysicalPath, DirectoryError);
			checkf(!DirectoryError, "Failed to normalize test mount root.");
			std::filesystem::create_directories(Root, DirectoryError);
			checkf(!DirectoryError, "Failed to create test mount root.");
			const auto Existing = std::ranges::find_if(MountPoints, [&](const FMountPoint& Mount) {
				return FoldAscii(Mount.VirtualRoot) == FoldAscii(VirtualRoot);
			});
			const std::filesystem::path Canonical = ComparableRoot(Root, DirectoryError);
			checkf(!DirectoryError, "Failed to canonicalize test mount root.");
			for (const FMountPoint& Mount : MountPoints)
			{
				if (&Mount == (Existing == MountPoints.end() ? nullptr : &*Existing)) continue;
				const std::filesystem::path ExistingCanonical = ComparableRoot(Mount.Root, DirectoryError);
				checkf(!DirectoryError, "Failed to canonicalize an existing test mount root.");
				checkf(
					!IsPathWithin(Canonical, ExistingCanonical)
						&& !IsPathWithin(ExistingCanonical, Canonical),
					"Test mount roots must not overlap.");
			}
			FMountPoint Definition{
				.VirtualRoot = std::string(VirtualRoot),
				.Owner = EMountOwner::Test,
				.Root = Root,
				.bAssetPackages = bAssetPackages,
				.bAuthoringWritable = bAuthoringWritable};
			if (Existing == MountPoints.end()) MountPoints.push_back(std::move(Definition));
			else *Existing = std::move(Definition);
			std::ranges::sort(MountPoints, [](const FMountPoint& A, const FMountPoint& B) {
				return A.VirtualRoot.length() > B.VirtualRoot.length();
			});
		}

		FScopedMountRegistryFixture::FScopedMountRegistryFixture()
			: SavedMounts(MountPoints)
			, bSavedPublished(bRegistryPublished)
		{
			MountPoints.clear();
			bRegistryPublished = false;
		}

		FScopedMountRegistryFixture::FScopedMountRegistryFixture(std::span<const FMountPoint> Definitions)
			: FScopedMountRegistryFixture()
		{
			PublishMountRegistry(Definitions, &Error);
		}

		FScopedMountRegistryFixture::~FScopedMountRegistryFixture()
		{
			MountPoints = std::move(SavedMounts);
			bRegistryPublished = bSavedPublished;
		}
	} // namespace PathUtilities

	auto FPaths::LaunchDir() -> std::string
	{
		static std::string CachedLaunchDir = []() -> std::string {
			std::string ExePath = FPlatformProcess::ExecutablePath();
			std::string LaunchDir = std::filesystem::path{ExePath}.parent_path().generic_string() + "/";
			return LaunchDir;
		}();
		return CachedLaunchDir;
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
				OutputRoot.parent_path() / "ThirdParty" / DURIN_BUILD_CONFIGURATION;
			return ThirdPartyRoot.generic_string() + "/";
		}();
		return CachedThirdPartyDir;
	}

	auto FPaths::Resolve(std::string_view VirtualPath) -> std::string
	{
		const PathUtilities::FAssetPathResult Result =
			PathUtilities::ResolveAssetPath(VirtualPath, PathUtilities::EPathExistence::AllowMissing);
		return Result ? Result.PhysicalPath.generic_string() : std::string{};
	}

} // namespace Durin
