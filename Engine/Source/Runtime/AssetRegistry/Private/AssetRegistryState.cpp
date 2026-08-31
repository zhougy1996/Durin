#include "AssetRegistryStateInternal.h"

#include "DObject/Class.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint32 MaximumRedirectDepth = 64;
		constexpr std::string_view RedirectorClassName =
			"Durin::Asset::DAssetRedirector";

		auto ResolveAssetPathInCatalog(
			const std::unordered_map<FAssetPath, FAssetData>& Assets,
			uint64 Revision,
			const FAssetPath& Path,
			const FAssetPathResolveOptions& Options) -> FAssetPathResolveResult
		{
			FAssetPathResolveResult Result;
			Result.CatalogRevision = Revision;
			Result.RequestedPath = Path;
			FAssetPath Current = Path;
			std::unordered_set<FAssetPath> Visited;
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

		auto ValidatePublication(
			const FAssetRegistryPublication& Publication) -> FAssetResult
		{
			if (!Publication.bReferenceIndexComplete
				|| !Publication.ReferenceErrors.empty()
				|| Publication.ReferenceFingerprints.size() != Publication.Assets.size())
				return {EAssetError::StaleData,
					"Asset registry publication requires a complete catalog/reference projection."};
			const auto PathsCanonical = [](const std::vector<FAssetPath>& Paths) {
				return std::ranges::is_sorted(Paths,
					[](const FAssetPath& Left, const FAssetPath& Right) {
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
					return {EAssetError::CorruptFile,
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
					return {EAssetError::CorruptFile,
						"Asset registry publication contains invalid redirect metadata."};
				const FAssetPackageFingerprint ExpectedFingerprint{
					.FileSize = Data.FileSize,
					.LastWriteTimeTicks = Data.LastWriteTimeTicks,
					.ReaderVersion = Data.FormatVersion};
				if (Publication.ReferenceFingerprints.at(Path) != ExpectedFingerprint)
					return {EAssetError::CorruptFile,
						"Asset registry publication fingerprint drifted from catalog metadata."};
				auto Add = [&](EAssetReferenceKind Kind, const FAssetPath& Target) {
					ExpectedEdges.push_back({.SourcePackage = Path,
						.SourceFingerprint = ExpectedFingerprint, .Kind = Kind,
						.TargetPath = Target});
				};
				for (const FAssetPath& Target : Data.Dependencies)
					if (Data.EntryKind != EAssetRegistryEntryKind::Redirector
						|| Target != Data.RedirectDestination)
						Add(EAssetReferenceKind::HardObject, Target);
				for (const FAssetPath& Target : Data.SoftDependencies)
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
				return {EAssetError::CorruptFile,
					"Asset registry publication package edges drifted from catalog metadata."};
			return {};
		}
	}

	auto FAssetReferenceIndex::FindReferencers(
		const FAssetPath& Target) const -> std::vector<FAssetPackageReferenceEdge>
	{
		std::vector<FAssetPackageReferenceEdge> Result;
		for (const FAssetPackageReferenceEdge& Reference : Edges)
			if (Reference.TargetPath == Target) Result.push_back(Reference);
		return Result;
	}

	auto FAssetReferenceIndex::FindTargets(
		const FAssetPath& Source) const -> std::vector<FAssetPath>
	{
		std::vector<FAssetPath> Result;
		for (const FAssetPackageReferenceEdge& Reference : Edges)
			if (Reference.SourcePackage == Source) Result.push_back(Reference.TargetPath);
		std::ranges::sort(Result, [](const FAssetPath& Left, const FAssetPath& Right) {
			return Left.GetView() < Right.GetView();
		});
		Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
		return Result;
	}

	auto FAssetRegistrySnapshot::ResolveAssetPath(
		const FAssetPath& Path,
		const FAssetPathResolveOptions& Options) const -> FAssetPathResolveResult
	{
		return ResolveAssetPathInCatalog(Catalog.Assets, Revision, Path, Options);
	}

	namespace Private
	{
	FAssetRegistryState::FAssetRegistryState() = default;

	auto FAssetRegistryState::FindAssetExact(
		const FAssetPath& Path) const -> FAssetCatalogEntry
	{
		std::shared_lock Lock(Mutex);
		const auto It = Assets.find(Path);
		return {.Revision = Revision,
			.Data = It == Assets.end() ? std::nullopt
				: std::optional<FAssetData>(It->second)};
	}

	auto FAssetRegistryState::ResolveAssetPath(const FAssetPath& Path,
		const FAssetPathResolveOptions& Options) const -> FAssetPathResolveResult
	{
		std::shared_lock Lock(Mutex);
		return ResolveAssetPathInCatalog(Assets, Revision, Path, Options);
	}

	auto FAssetRegistryState::FindRedirectorsTo(
		const FAssetPath& Destination) const -> std::vector<FAssetPath>
	{
		std::shared_lock Lock(Mutex);
		std::vector<FAssetPath> Result;
		for (const auto& [Path, Data] : Assets)
			if (Data.EntryKind == EAssetRegistryEntryKind::Redirector
				&& Data.RedirectDestination == Destination)
				Result.push_back(Path);
		std::ranges::sort(Result, [](const FAssetPath& Left, const FAssetPath& Right) {
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
		const FAssetPath& Root) const -> FAssetDependencyClosureSnapshot
	{
		std::shared_lock Lock(Mutex);
		FAssetDependencyClosureSnapshot Result{.Revision = Revision};
		const auto RootIt = Assets.find(Root);
		if (RootIt == Assets.end())
		{
			Result.Result = {EAssetError::NotFound,
				std::format("The Asset Registry has no entry for dependency root '{}'.",
					Root.GetView())};
			return Result;
		}

		std::unordered_set<FAssetPath> Visited;
		std::vector<const FAssetData*> Pending{&RootIt->second};
		Visited.emplace(Root);
		while (!Pending.empty())
		{
			const FAssetData* Data = Pending.back();
			Pending.pop_back();
			Result.Assets.push_back(*Data);
			for (const FAssetPath& Dependency : Data->Dependencies)
			{
				if (!Visited.emplace(Dependency).second) continue;
				const auto DependencyIt = Assets.find(Dependency);
				if (DependencyIt == Assets.end())
				{
					Result.Assets.clear();
					Result.Result = {EAssetError::MissingDependency,
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

	auto FAssetRegistryState::Publish(
		FAssetRegistryPublication Publication) -> FAssetResult
	{
		std::unique_lock Lock(Mutex);
		if (Publication.ExpectedRevision != Revision)
			return {EAssetError::StaleData, std::format(
				"Asset registry publication expected revision {} but current revision is {}.",
				Publication.ExpectedRevision, Revision)};
		if (FAssetResult Validation = ValidatePublication(Publication); !Validation)
			return Validation;
		if (Assets == Publication.Assets
			&& References.Edges == Publication.ReferenceEdges
			&& References.SourceFingerprints == Publication.ReferenceFingerprints
			&& References.bComplete == Publication.bReferenceIndexComplete)
		{
			References.Errors = std::move(Publication.ReferenceErrors);
			return {};
		}
		Assets = std::move(Publication.Assets);
		References.Edges = std::move(Publication.ReferenceEdges);
		References.SourceFingerprints = std::move(Publication.ReferenceFingerprints);
		References.Errors = std::move(Publication.ReferenceErrors);
		References.bComplete = Publication.bReferenceIndexComplete;
		++Revision;
		return {};
	}

	auto GetAssetRegistryState() -> FAssetRegistryState&
	{
		static FAssetRegistryState State;
		return State;
	}
	}

	auto FindAssetExact(const FAssetPath& Path) -> FAssetCatalogEntry
	{
		return Private::GetAssetRegistryState().FindAssetExact(Path);
	}

	auto ResolveAssetPath(const FAssetPath& Path,
		const FAssetPathResolveOptions& Options) -> FAssetPathResolveResult
	{
		return Private::GetAssetRegistryState().ResolveAssetPath(Path, Options);
	}

	auto CaptureAssetCatalogSnapshot() -> FAssetCatalogSnapshot
	{
		return Private::GetAssetRegistryState().CaptureCatalog();
	}

	auto CaptureAssetDependencyClosure(
		const FAssetPath& Root) -> FAssetDependencyClosureSnapshot
	{
		return Private::GetAssetRegistryState().CaptureDependencyClosure(Root);
	}

	auto GetAssetCatalogRevision() -> uint64
	{
		return Private::GetAssetRegistryState().GetRevision();
	}

	auto CaptureAssetReferenceIndex() -> FAssetReferenceIndex
	{
		return Private::GetAssetRegistryState().CaptureReferences();
	}

	auto CaptureAssetRegistrySnapshot() -> FAssetRegistrySnapshot
	{
		return Private::GetAssetRegistryState().CaptureSnapshot();
	}

	auto CaptureAssetRegistryPublication() -> FAssetRegistryPublication
	{
		return Private::GetAssetRegistryState().CapturePublication();
	}

	auto PublishAssetRegistryPublication(
		FAssetRegistryPublication Publication) -> FAssetResult
	{
		FAssetResult Result = Private::GetAssetRegistryState().Publish(
			std::move(Publication));
		if (Result) Private::MarkAssetRegistryCachesDirty();
		return Result;
	}

	auto FindRedirectorsTo(const FAssetPath& Destination) -> std::vector<FAssetPath>
	{
		return Private::GetAssetRegistryState().FindRedirectorsTo(Destination);
	}
}
