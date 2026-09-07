#include "AssetRegistryStateInternal.h"

#include "DObject/Class.h"
#include "DObject/SoftObjectPtr.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 MaximumRedirectDepth = 64;
		constexpr std::string_view RedirectorClassName =
			"Durin::DAssetRedirector";

		auto ResolveAssetPathInCatalog(
			const std::unordered_map<FPackagePath, FAssetData>& Assets,
			uint64 Revision,
			const FPackagePath& Path,
			const FAssetPathResolveOptions& Options) -> FAssetPathResolveResult
		{
			FAssetPathResolveResult Result;
			Result.CatalogRevision = Revision;
			Result.RequestedPath = Path;
			FPackagePath Current = Path;
			std::unordered_set<FPackagePath> Visited;
			while (true)
			{
				const auto It = Assets.find(Current);
				if (It == Assets.end())
				{
					Result.FinalPath = Current;
					Result.State = Result.RedirectChain.empty()
						? EAssetPathResolveState::NotFound
						: EAssetPathResolveState::MissingRedirectTarget;
					return Result;
				}
				const FAssetData& Data = It->second;
				if (Data.EntryKind == EAssetRegistryEntryKind::Asset)
				{
					if (Data.RedirectDestination.IsValid()
						|| Data.AssetClassName == RedirectorClassName)
					{
						Result.FinalPath = Current;
						Result.State = EAssetPathResolveState::CorruptRedirector;
						return Result;
					}
					DClass* TargetClass = FindClassByQualifiedName(FName(Data.AssetClassName));
					if (!TargetClass)
					{
						Result.FinalPath = Current;
						Result.State = EAssetPathResolveState::UnknownTargetClass;
						return Result;
					}
					if (Options.ExpectedClass && !TargetClass->IsChildOf(Options.ExpectedClass))
					{
						Result.FinalPath = Current;
						Result.State = EAssetPathResolveState::RedirectTypeMismatch;
						return Result;
					}
					Result.FinalPath = Current;
					Result.FinalAssetData = Data;
					Result.State = EAssetPathResolveState::Resolved;
					return Result;
				}
				if (Data.EntryKind != EAssetRegistryEntryKind::Redirector
					|| Data.AssetClassName != RedirectorClassName
					|| !Data.RedirectDestination.IsValid()
					|| Data.RedirectDestination == Current
					|| Data.Dependencies.size() != 1
					|| Data.Dependencies.front() != Data.RedirectDestination)
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::CorruptRedirector;
					return Result;
				}
				if (!Visited.insert(Current).second)
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::RedirectCycle;
					return Result;
				}
				if (Result.RedirectChain.size() == MaximumRedirectDepth)
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::RedirectDepthExceeded;
					return Result;
				}
				Result.RedirectChain.push_back(Current);
				Current = Data.RedirectDestination;
			}
		}

		auto AppendRedirectedSubobjectPath(
			const FObjectPath& Destination,
			FSubobjectPathView SourceSubobjects,
			FObjectPath& OutPath) -> bool
		{
			if (SourceSubobjects.empty())
			{
				OutPath = Destination;
				return true;
			}
			std::vector<std::string> Components;
			for (std::string_view Component : Destination.GetSubobjectNames())
				Components.emplace_back(Component);
			for (std::string_view Component : SourceSubobjects)
				Components.emplace_back(Component);
			return FObjectPath::TryCreate(
				Destination.GetAssetPath(), Components, OutPath);
		}

		auto ResolveAssetObjectPathInCatalog(
			const std::unordered_map<FPackagePath, FAssetData>& Assets,
			uint64 Revision,
			const FObjectPath& Path,
			const FAssetPathResolveOptions& Options) -> FObjectPathResolveResult
		{
			FObjectPathResolveResult Result;
			Result.CatalogRevision = Revision;
			Result.RequestedPath = Path;
			FObjectPath Current = Path;
			std::unordered_set<FObjectPath> Visited;
			while (true)
			{
				const auto PackageIt = Assets.find(Current.GetPackagePath());
				if (PackageIt == Assets.end())
				{
					Result.FinalPath = Current;
					Result.State = Result.RedirectChain.empty()
						? EAssetPathResolveState::NotFound
						: EAssetPathResolveState::MissingRedirectTarget;
					return Result;
				}
				const FAssetData& Package = PackageIt->second;
				const auto AssetIt = std::ranges::find(
					Package.TopLevelAssets, Current.GetAssetPath(),
					&FTopLevelAssetData::AssetPath);
				if (AssetIt == Package.TopLevelAssets.end())
				{
					Result.FinalPath = Current;
					Result.State = Result.RedirectChain.empty()
						? EAssetPathResolveState::NotFound
						: EAssetPathResolveState::MissingRedirectTarget;
					return Result;
				}
				const FTopLevelAssetData& Asset = *AssetIt;
				if (!Asset.IsRedirector())
				{
					if (Asset.AssetClassName == RedirectorClassName)
					{
						Result.FinalPath = Current;
						Result.State = EAssetPathResolveState::CorruptRedirector;
						return Result;
					}
					DClass* TargetClass = FindClassByQualifiedName(FName(Asset.AssetClassName));
					if (!TargetClass)
					{
						Result.FinalPath = Current;
						Result.State = EAssetPathResolveState::UnknownTargetClass;
						return Result;
					}
					if (Options.ExpectedClass && Current.IsTopLevelAsset()
						&& !TargetClass->IsChildOf(Options.ExpectedClass))
					{
						Result.FinalPath = Current;
						Result.State = EAssetPathResolveState::RedirectTypeMismatch;
						return Result;
					}
					Result.FinalPath = Current;
					Result.FinalAssetData = Asset;
					Result.FinalPackageData = Package;
					Result.State = EAssetPathResolveState::Resolved;
					return Result;
				}
				if (Asset.AssetClassName != RedirectorClassName)
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::CorruptRedirector;
					return Result;
				}
				if (!Visited.insert(Current).second)
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::RedirectCycle;
					return Result;
				}
				if (Result.RedirectChain.size() == MaximumRedirectDepth)
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::RedirectDepthExceeded;
					return Result;
				}
				Result.RedirectChain.push_back(Current);
				FObjectPath Redirected;
				if (!AppendRedirectedSubobjectPath(
					Asset.RedirectDestination, Current.GetSubobjectNames(), Redirected))
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::CorruptRedirector;
					return Result;
				}
				Current = std::move(Redirected);
			}
		}

		auto ValidatePublication(
			const FAssetRegistryPublication& Publication) -> FAssetRegistryResult
		{
			if (!Publication.bReferenceIndexComplete
				|| !Publication.ReferenceErrors.empty()
				|| Publication.ReferenceFingerprints.size() != Publication.Assets.size())
				return {EAssetRegistryError::StaleData,
					"Asset registry publication requires a complete catalog/reference projection."};
			const auto PathsCanonical = [](const std::vector<FPackagePath>& Paths) {
				return std::ranges::is_sorted(Paths,
					[](const FPackagePath& Left, const FPackagePath& Right) {
						return Left.GetView() < Right.GetView();
					}) && std::adjacent_find(Paths.begin(), Paths.end()) == Paths.end();
			};
			std::vector<FAssetPackageReferenceEdge> ExpectedEdges;
			for (const auto& [Path, Data] : Publication.Assets)
			{
				if (Path != Data.PackagePath
					|| !Publication.ReferenceFingerprints.contains(Path)
					|| Data.ObjectCount == 0 || !PathsCanonical(Data.Dependencies)
					|| !PathsCanonical(Data.SoftDependencies)
					|| !std::ranges::is_sorted(Data.SearchableNames)
					|| std::adjacent_find(Data.SearchableNames.begin(),
						Data.SearchableNames.end()) != Data.SearchableNames.end()
					|| ((Data.BulkSegmentExtent == 0)
						!= Data.BulkSegmentDigest.IsZero()))
					return {EAssetRegistryError::CorruptFile,
						"Asset registry publication contains inconsistent package metadata."};
				if ((Data.EntryKind == EAssetRegistryEntryKind::Asset
						&& (Data.RedirectDestination.IsValid()
							|| Data.AssetClassName == RedirectorClassName))
					|| (Data.EntryKind == EAssetRegistryEntryKind::Redirector
						&& (Data.AssetClassName != RedirectorClassName
							|| !Data.RedirectDestination.IsValid()
							|| Data.Dependencies.size() != 1
							|| Data.Dependencies.front() != Data.RedirectDestination
							|| Data.ObjectCount != 1)))
					return {EAssetRegistryError::CorruptFile,
						"Asset registry publication contains invalid redirect metadata."};
				const FAssetPackageFingerprint ExpectedFingerprint{
					.FileSize = Data.FileSize,
					.LastWriteTimeTicks = Data.LastWriteTimeTicks,
					.ReaderVersion = Data.FormatVersion};
				if (Publication.ReferenceFingerprints.at(Path) != ExpectedFingerprint)
					return {EAssetRegistryError::CorruptFile,
						"Asset registry publication fingerprint drifted from catalog metadata."};
				auto Add = [&](EAssetReferenceKind Kind, const FPackagePath& Target) {
					ExpectedEdges.push_back({.SourcePackage = Path,
						.SourceFingerprint = ExpectedFingerprint, .Kind = Kind,
						.TargetPath = Target});
				};
				for (const FPackagePath& Target : Data.Dependencies)
					if (Data.EntryKind != EAssetRegistryEntryKind::Redirector
						|| Target != Data.RedirectDestination)
						Add(EAssetReferenceKind::HardObject, Target);
				for (const FPackagePath& Target : Data.SoftDependencies)
					Add(EAssetReferenceKind::SoftObject, Target);
				if (Data.EntryKind == EAssetRegistryEntryKind::Redirector)
					Add(EAssetReferenceKind::Redirect, Data.RedirectDestination);
			}
			std::ranges::sort(ExpectedEdges,
				[](const FAssetPackageReferenceEdge& Left,
					const FAssetPackageReferenceEdge& Right) {
					return std::tuple(Left.TargetPath.GetView(),
						Left.SourcePackage.GetView(), Left.Kind)
						< std::tuple(Right.TargetPath.GetView(),
							Right.SourcePackage.GetView(), Right.Kind);
				});
			ExpectedEdges.erase(std::unique(ExpectedEdges.begin(), ExpectedEdges.end(),
				[](const FAssetPackageReferenceEdge& Left,
					const FAssetPackageReferenceEdge& Right) {
					return Left.SourcePackage == Right.SourcePackage
						&& Left.TargetPath == Right.TargetPath && Left.Kind == Right.Kind;
				}), ExpectedEdges.end());
			if (Publication.ReferenceEdges != ExpectedEdges)
				return {EAssetRegistryError::CorruptFile,
					"Asset registry publication package edges drifted from catalog metadata."};
			return {};
		}
	}

	auto FAssetReferenceIndex::FindReferencers(
		const FPackagePath& Target) const -> std::vector<FAssetPackageReferenceEdge>
	{
		std::vector<FAssetPackageReferenceEdge> Result;
		for (const FAssetPackageReferenceEdge& Reference : Edges)
			if (Reference.TargetPath == Target) Result.push_back(Reference);
		return Result;
	}

	auto FAssetReferenceIndex::FindTargets(
		const FPackagePath& Source) const -> std::vector<FPackagePath>
	{
		std::vector<FPackagePath> Result;
		for (const FAssetPackageReferenceEdge& Reference : Edges)
			if (Reference.SourcePackage == Source) Result.push_back(Reference.TargetPath);
		std::ranges::sort(Result, [](const FPackagePath& Left, const FPackagePath& Right) {
			return Left.GetView() < Right.GetView();
		});
		Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
		return Result;
	}

	auto FAssetRegistrySnapshot::ResolveAssetPath(
		const FPackagePath& Path,
		const FAssetPathResolveOptions& Options) const -> FAssetPathResolveResult
	{
		if (AssetPrivate::GetAssetRegistryState().IsFenced(Path))
			return {.State = EAssetPathResolveState::NotFound,
				.CatalogRevision = Revision, .RequestedPath = Path,
				.FinalPath = Path};
		return ResolveAssetPathInCatalog(Catalog.Assets, Revision, Path, Options);
	}

	namespace AssetPrivate
	{
	FAssetRegistryState::FAssetRegistryState() = default;

	auto FAssetRegistryState::FindAssetExact(
		const FPackagePath& Path) const -> FAssetCatalogEntry
	{
		std::shared_lock Lock(Mutex);
		const auto It = Assets.find(Path);
		return {.Revision = Revision,
			.Data = It == Assets.end() ? std::nullopt
				: std::optional<FAssetData>(It->second)};
	}

	auto FAssetRegistryState::FindTopLevelAssetExact(
		const FTopLevelAssetPath& Path) const -> FTopLevelAssetCatalogEntry
	{
		std::shared_lock Lock(Mutex);
		const auto PackageIt = Assets.find(Path.GetPackagePath());
		if (PackageIt == Assets.end()) return {.Revision = Revision};
		const auto AssetIt = std::ranges::find(
			PackageIt->second.TopLevelAssets, Path, &FTopLevelAssetData::AssetPath);
		if (AssetIt == PackageIt->second.TopLevelAssets.end())
			return {.Revision = Revision};
		return {.Revision = Revision, .Asset = *AssetIt, .Package = PackageIt->second};
	}

	auto FAssetRegistryState::ResolveAssetPath(const FPackagePath& Path,
		const FAssetPathResolveOptions& Options) const -> FAssetPathResolveResult
	{
		std::shared_lock Lock(Mutex);
		if (ProjectionFences.contains(Path))
			return {.State = EAssetPathResolveState::NotFound,
				.CatalogRevision = Revision, .RequestedPath = Path,
				.FinalPath = Path};
		return ResolveAssetPathInCatalog(Assets, Revision, Path, Options);
	}

	auto FAssetRegistryState::FindRedirectorsTo(
		const FPackagePath& Destination) const -> std::vector<FPackagePath>
	{
		std::shared_lock Lock(Mutex);
		std::vector<FPackagePath> Result;
		for (const auto& [Path, Data] : Assets)
			if (Data.EntryKind == EAssetRegistryEntryKind::Redirector
				&& Data.RedirectDestination == Destination)
				Result.push_back(Path);
		std::ranges::sort(Result, [](const FPackagePath& Left, const FPackagePath& Right) {
			return Left.GetView() < Right.GetView();
		});
		return Result;
	}

	auto FAssetRegistryState::CaptureCatalog() const -> FAssetCatalogSnapshot
	{
		std::shared_lock Lock(Mutex);
		return {.Revision = Revision, .Assets = Assets};
	}

	auto FAssetRegistryState::CaptureDependencyClosure(
		const FPackagePath& Root) const -> FAssetDependencyClosureSnapshot
	{
		std::shared_lock Lock(Mutex);
		FAssetDependencyClosureSnapshot Result{.Revision = Revision};
		const auto RootIt = Assets.find(Root);
		if (RootIt == Assets.end())
		{
			Result.Result = {EAssetRegistryError::NotFound,
				std::format("The Asset Registry has no entry for dependency root '{}'.",
					Root.GetView())};
			return Result;
		}

		std::unordered_set<FPackagePath> Visited;
		std::vector<const FAssetData*> Pending{&RootIt->second};
		Visited.emplace(Root);
		while (!Pending.empty())
		{
			const FAssetData* Data = Pending.back();
			Pending.pop_back();
			Result.Assets.push_back(*Data);
			for (const FPackagePath& Dependency : Data->Dependencies)
			{
				if (!Visited.emplace(Dependency).second) continue;
				const auto DependencyIt = Assets.find(Dependency);
				if (DependencyIt == Assets.end())
				{
					Result.Assets.clear();
					Result.Result = {EAssetRegistryError::MissingDependency,
						std::format("The Asset Registry has no entry for dependency '{}'.",
							Dependency.GetView())};
					return Result;
				}
				Pending.push_back(&DependencyIt->second);
			}
		}
		std::ranges::sort(Result.Assets,
			[](const FAssetData& Left, const FAssetData& Right) {
				return Left.PackagePath.GetView() < Right.PackagePath.GetView();
			});
		return Result;
	}

	auto FAssetRegistryState::CaptureReferences() const -> FAssetReferenceIndex
	{
		std::shared_lock Lock(Mutex);
		FAssetReferenceIndex Result = References;
		Result.Revision = Revision;
		return Result;
	}

	auto FAssetRegistryState::CaptureSnapshot() const -> FAssetRegistrySnapshot
	{
		std::shared_lock Lock(Mutex);
		FAssetRegistrySnapshot Result{
			.Revision = Revision,
			.Catalog = {.Revision = Revision, .Assets = Assets},
			.References = References};
		Result.References.Revision = Revision;
		return Result;
	}

	auto FAssetRegistryState::CapturePublication() const -> FAssetRegistryPublication
	{
		std::shared_lock Lock(Mutex);
		return {.ExpectedRevision = Revision, .Assets = Assets,
			.ReferenceEdges = References.Edges,
			.ReferenceFingerprints = References.SourceFingerprints,
			.ReferenceErrors = References.Errors,
			.bReferenceIndexComplete = References.bComplete};
	}

	auto FAssetRegistryState::GetRevision() const -> uint64
	{
		std::shared_lock Lock(Mutex);
		return Revision;
	}

	auto FAssetRegistryState::CaptureChanges(uint64 FromRevision) const -> FContentChangeBatch
	{
		std::shared_lock Lock(Mutex);
		return Changes.Read(FromRevision, Revision);
	}

	auto FAssetRegistryState::Publish(
		FAssetRegistryPublication Publication) -> FAssetRegistryResult
	{
		std::unique_lock Lock(Mutex);
		if (Publication.ExpectedRevision != Revision)
			return {EAssetRegistryError::StaleData, std::format(
				"Asset registry publication expected revision {} but current revision is {}.",
				Publication.ExpectedRevision, Revision)};
		if (FAssetRegistryResult Validation = ValidatePublication(Publication); !Validation)
			return Validation;
		if (Assets == Publication.Assets
			&& References.Edges == Publication.ReferenceEdges
			&& References.SourceFingerprints == Publication.ReferenceFingerprints
			&& References.bComplete == Publication.bReferenceIndexComplete)
		{
			References.Errors = std::move(Publication.ReferenceErrors);
			return {};
		}
		FContentChangeBatch Batch{Revision, Revision + 1};
		for (const auto& [Path, Before] : Assets)
		{
			const auto It = Publication.Assets.find(Path);
			if (It != Publication.Assets.end() && It->second == Before) continue;
			Batch.Changes.push_back({It == Publication.Assets.end()
				? EContentChangeKind::Removed : EContentChangeKind::Modified,
				Before.PhysicalPath, It == Publication.Assets.end() ? "" : It->second.PhysicalPath,
				Path.ToString(), It == Publication.Assets.end() ? "" : Path.ToString()});
		}
		for (const auto& [Path, After] : Publication.Assets)
			if (!Assets.contains(Path))
				Batch.Changes.push_back({EContentChangeKind::Added, {}, After.PhysicalPath, {}, Path.ToString()});
		Changes.Append(std::move(Batch));
		Assets = std::move(Publication.Assets);
		References.Edges = std::move(Publication.ReferenceEdges);
		References.SourceFingerprints = std::move(Publication.ReferenceFingerprints);
		References.Errors = std::move(Publication.ReferenceErrors);
		References.bComplete = Publication.bReferenceIndexComplete;
		++Revision;
		return {};
	}

	auto FAssetRegistryState::Fence(std::span<const FPackagePath> Paths) -> void
	{
		std::unique_lock Lock(Mutex);
		ProjectionFences.insert(Paths.begin(), Paths.end());
	}

	auto FAssetRegistryState::ClearFence(std::span<const FPackagePath> Paths) -> void
	{
		std::unique_lock Lock(Mutex);
		for (const FPackagePath& Path : Paths) ProjectionFences.erase(Path);
	}

	auto FAssetRegistryState::IsFenced(const FPackagePath& Path) const -> bool
	{
		std::shared_lock Lock(Mutex);
		return ProjectionFences.contains(Path);
	}

	auto FAssetRegistryState::CaptureFences() const -> std::vector<FPackagePath>
	{
		std::shared_lock Lock(Mutex);
		std::vector<FPackagePath> Result(ProjectionFences.begin(), ProjectionFences.end());
		std::ranges::sort(Result, [](const FPackagePath& Left, const FPackagePath& Right) {
			return Left.GetView() < Right.GetView();
		});
		return Result;
	}

	auto GetAssetRegistryState() -> FAssetRegistryState&
	{
		static FAssetRegistryState State;
		return State;
	}
	}

	auto FindAssetExact(const FPackagePath& Path) -> FAssetCatalogEntry
	{
		return AssetPrivate::GetAssetRegistryState().FindAssetExact(Path);
	}

	auto FindTopLevelAssetExact(
		const FTopLevelAssetPath& Path) -> FTopLevelAssetCatalogEntry
	{
		return AssetPrivate::GetAssetRegistryState().FindTopLevelAssetExact(Path);
	}

	auto ResolveAssetPath(const FPackagePath& Path,
		const FAssetPathResolveOptions& Options) -> FAssetPathResolveResult
	{
		return AssetPrivate::GetAssetRegistryState().ResolveAssetPath(Path, Options);
	}

	auto ResolveAssetObjectPath(const FObjectPath& Path,
		const FAssetPathResolveOptions& Options) -> FObjectPathResolveResult
	{
		const FAssetCatalogSnapshot Snapshot = CaptureAssetCatalogSnapshot();
		return ResolveAssetObjectPathInCatalog(
			Snapshot.Assets, Snapshot.Revision, Path, Options);
	}

	auto CaptureAssetCatalogSnapshot() -> FAssetCatalogSnapshot
	{
		return AssetPrivate::GetAssetRegistryState().CaptureCatalog();
	}

	auto CaptureAssetDependencyClosure(
		const FPackagePath& Root) -> FAssetDependencyClosureSnapshot
	{
		return AssetPrivate::GetAssetRegistryState().CaptureDependencyClosure(Root);
	}

	auto CaptureAssetCatalogChanges(uint64 FromRevision) -> FContentChangeBatch
	{
		return AssetPrivate::GetAssetRegistryState().CaptureChanges(FromRevision);
	}

	auto GetAssetCatalogRevision() -> uint64
	{
		return AssetPrivate::GetAssetRegistryState().GetRevision();
	}

	auto CaptureAssetReferenceIndex() -> FAssetReferenceIndex
	{
		return AssetPrivate::GetAssetRegistryState().CaptureReferences();
	}

	auto CaptureAssetRegistrySnapshot() -> FAssetRegistrySnapshot
	{
		return AssetPrivate::GetAssetRegistryState().CaptureSnapshot();
	}

	auto CaptureAssetRegistryPublication() -> FAssetRegistryPublication
	{
		return AssetPrivate::GetAssetRegistryState().CapturePublication();
	}

	auto PublishAssetRegistryPublication(
		FAssetRegistryPublication Publication) -> FAssetRegistryResult
	{
		FAssetRegistryResult Result = AssetPrivate::GetAssetRegistryState().Publish(
			std::move(Publication));
		if (Result)
		{
			AssetPrivate::MarkAssetRegistryCachesDirty();
			InvalidateSoftObjectCaches();
		}
		return Result;
	}

	auto PublishAssetRegistryDelta(FAssetRegistryDelta Delta)
		-> FAssetRegistryResult
	{
		FAssetRegistryPublication Publication = CaptureAssetRegistryPublication();
		if (Publication.ExpectedRevision != Delta.ExpectedRevision)
			return {EAssetRegistryError::StaleData, std::format(
				"Asset registry delta expected revision {} but current revision is {}.",
				Delta.ExpectedRevision, Publication.ExpectedRevision)};
		std::unordered_set<FPackagePath> Touched;
		auto Touch = [&](const FPackagePath& Path) -> bool {
			return Path.IsValid() && Touched.insert(Path).second;
		};
		for (FAssetData& Data : Delta.Adds)
		{
			if (!Touch(Data.PackagePath) || Publication.Assets.contains(Data.PackagePath))
				return {EAssetRegistryError::StaleData,
					"Asset registry delta Add path is invalid, duplicated, or occupied."};
			Publication.Assets.emplace(Data.PackagePath, std::move(Data));
		}
		for (FAssetData& Data : Delta.Replaces)
		{
			if (!Touch(Data.PackagePath) || !Publication.Assets.contains(Data.PackagePath))
				return {EAssetRegistryError::StaleData,
					"Asset registry delta Replace path is invalid, duplicated, or missing."};
			Publication.Assets.insert_or_assign(Data.PackagePath, std::move(Data));
		}
		for (const FPackagePath& Path : Delta.Removes)
		{
			if (!Touch(Path) || Publication.Assets.erase(Path) != 1)
				return {EAssetRegistryError::StaleData,
					"Asset registry delta Remove path is invalid, duplicated, or missing."};
		}
		for (const FPackagePath& Path : Delta.ReferenceInvalidations)
			if (!Path.IsValid())
				return {EAssetRegistryError::CorruptFile,
					"Asset registry delta contains an invalid reference-invalidation path."};

		Publication.ReferenceEdges.clear();
		Publication.ReferenceFingerprints.clear();
		for (const auto& [Path, Data] : Publication.Assets)
		{
			const FAssetPackageFingerprint Fingerprint{
				.FileSize = Data.FileSize,
				.LastWriteTimeTicks = Data.LastWriteTimeTicks,
				.ReaderVersion = Data.FormatVersion};
			Publication.ReferenceFingerprints.emplace(Path, Fingerprint);
			auto AddEdge = [&](EAssetReferenceKind Kind, const FPackagePath& Target) {
				Publication.ReferenceEdges.push_back({.SourcePackage = Path,
					.SourceFingerprint = Fingerprint, .Kind = Kind, .TargetPath = Target});
			};
			for (const FPackagePath& Target : Data.Dependencies)
				if (Data.EntryKind != EAssetRegistryEntryKind::Redirector
					|| Target != Data.RedirectDestination)
					AddEdge(EAssetReferenceKind::HardObject, Target);
			for (const FPackagePath& Target : Data.SoftDependencies)
				AddEdge(EAssetReferenceKind::SoftObject, Target);
			if (Data.EntryKind == EAssetRegistryEntryKind::Redirector)
				AddEdge(EAssetReferenceKind::Redirect, Data.RedirectDestination);
		}
		std::ranges::sort(Publication.ReferenceEdges,
			[](const FAssetPackageReferenceEdge& Left,
				const FAssetPackageReferenceEdge& Right) {
				return std::tuple(Left.TargetPath.GetView(), Left.SourcePackage.GetView(), Left.Kind)
					< std::tuple(Right.TargetPath.GetView(), Right.SourcePackage.GetView(), Right.Kind);
			});
		Publication.ReferenceEdges.erase(std::unique(Publication.ReferenceEdges.begin(),
			Publication.ReferenceEdges.end()), Publication.ReferenceEdges.end());
		Publication.ReferenceErrors.clear();
		Publication.bReferenceIndexComplete = true;
		FAssetRegistryResult Result = PublishAssetRegistryPublication(std::move(Publication));
		if (Result)
		{
			std::vector<FPackagePath> Paths(Touched.begin(), Touched.end());
			Paths.insert(Paths.end(), Delta.ReferenceInvalidations.begin(),
				Delta.ReferenceInvalidations.end());
			ClearAssetRegistryProjectionFence(Paths);
		}
		return Result;
	}

	auto FenceAssetRegistryProjection(std::span<const FPackagePath> Paths) -> void
	{
		AssetPrivate::GetAssetRegistryState().Fence(Paths);
	}

	auto ClearAssetRegistryProjectionFence(std::span<const FPackagePath> Paths) -> void
	{
		AssetPrivate::GetAssetRegistryState().ClearFence(Paths);
	}

	auto IsAssetRegistryProjectionFenced(const FPackagePath& Path) -> bool
	{
		return AssetPrivate::GetAssetRegistryState().IsFenced(Path);
	}

	auto CaptureAssetRegistryProjectionFences() -> std::vector<FPackagePath>
	{
		return AssetPrivate::GetAssetRegistryState().CaptureFences();
	}

	auto FindRedirectorsTo(const FPackagePath& Destination) -> std::vector<FPackagePath>
	{
		return AssetPrivate::GetAssetRegistryState().FindRedirectorsTo(Destination);
	}
}
