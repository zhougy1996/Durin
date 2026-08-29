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
			for (const auto& [Path, Data] : Publication.Assets)
				if (Path != Data.PackagePath
					|| !Publication.ReferenceFingerprints.contains(Path))
					return {EAssetError::CorruptFile,
						"Asset registry publication contains inconsistent package metadata."};
			for (const FAssetReferenceEdge& Edge : Publication.ReferenceEdges)
			{
				const auto Fingerprint = Publication.ReferenceFingerprints.find(
					Edge.SourcePackage);
				if (!Publication.Assets.contains(Edge.SourcePackage)
					|| Fingerprint == Publication.ReferenceFingerprints.end()
					|| Fingerprint->second != Edge.SourceFingerprint)
					return {EAssetError::CorruptFile,
						"Asset registry publication contains an invalid reference source."};
			}
			return {};
		}
	}

	auto FAssetReferenceIndex::FindReferencers(
		const FAssetPath& Target) const -> std::vector<FAssetReferenceEdge>
	{
		std::vector<FAssetReferenceEdge> Result;
		for (const FAssetReferenceEdge& Reference : Edges)
			if (Reference.TargetPath == Target) Result.push_back(Reference);
		return Result;
	}

	auto FAssetReferenceIndex::FindTargets(
		const FAssetPath& Source) const -> std::vector<FAssetPath>
	{
		std::vector<FAssetPath> Result;
		for (const FAssetReferenceEdge& Reference : Edges)
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
			.ReferenceStats = References.Stats,
			.ReferenceCacheWarning = References.CacheWarning,
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
			References.Stats = Publication.ReferenceStats;
			References.CacheWarning = std::move(Publication.ReferenceCacheWarning);
			return {};
		}
		Assets = std::move(Publication.Assets);
		References.Edges = std::move(Publication.ReferenceEdges);
		References.SourceFingerprints = std::move(Publication.ReferenceFingerprints);
		References.Errors = std::move(Publication.ReferenceErrors);
		References.Stats = Publication.ReferenceStats;
		References.CacheWarning = std::move(Publication.ReferenceCacheWarning);
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
