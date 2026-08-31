#include "Source/SourceReferenceIndex.h"

#include "Asset/Asset.h"
#include "Asset/AssetImportData.h"
#include "Asset/PackageInspection.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Terrain/TerrainHeightmap.h"
#include "Threading/Task.h"

namespace Durin::Editor
{
	namespace Private
	{
		struct FSourceReferenceSnapshot
		{
			std::unordered_map<std::string, std::vector<FSourceReference>> References;
			uint64 RegistryRevision = 0;
			uint64 Generation = 0;
			size_t MaximumPackageInspections = 0;
			size_t InspectedPackageCount = 0;
			std::string Warning;
		};
	}

	namespace
	{
		struct FSourceReferenceService
		{
			~FSourceReferenceService()
			{
				if (!BuildTask.IsValid()) return;
				(void)CancelTask(BuildTask.GetTaskHandle());
				(void)WaitTask(BuildTask.GetTaskHandle()).TaskState;
			}

			std::mutex Mutex;
			std::shared_ptr<const Private::FSourceReferenceSnapshot> Published;
			TTaskHandle<Private::FSourceReferenceSnapshot> BuildTask;
			uint64 Generation = 1;
			uint64 BuildingGeneration = 0;
			size_t BuildingMaximumPackageInspections = 0;
		};

		auto GetSourceReferenceService() -> FSourceReferenceService&
		{
			static FSourceReferenceService Service;
			return Service;
		}

		auto AddReference(
			std::unordered_map<std::string, std::vector<FSourceReference>>& References,
			const Asset::FAssetData& Data,
			std::string_view SourcePath) -> void
		{
			if (SourcePath.empty()) return;
			References[std::string(SourcePath)].push_back({
				.AssetPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName});
		}

		auto InspectSourceFields(
			const Asset::FAssetData& Data,
			std::unordered_map<std::string, std::vector<FSourceReference>>& References)
			-> bool
		{
			Asset::FAssetPackageInspection Inspection;
			if (!Asset::InspectAssetPackage(Data.PhysicalPath, Inspection)) return false;
			FAssetImportInfo ImportInfo;
			std::string ImportInfoError;
			if (InspectAssetImportInfo(
				Inspection, ImportInfo, ImportInfoError))
			{
				for (const FSourceFile& Source : ImportInfo.Sources)
					AddReference(References, Data, Source.Hint);
				return true;
			}

			return true;
		}

		auto BuildSnapshot(
			uint64 Generation,
			size_t MaximumPackageInspections)
			-> Private::FSourceReferenceSnapshot
		{
			Private::FSourceReferenceSnapshot Snapshot;
			Snapshot.Generation = Generation;
			Snapshot.MaximumPackageInspections = MaximumPackageInspections;
			const Asset::FAssetCatalogSnapshot Catalog =
				Asset::CaptureAssetCatalogSnapshot();
			Snapshot.RegistryRevision = Catalog.Revision;
			for (const auto& [Path, Asset] : Catalog.Assets)
			{
				if (Snapshot.InspectedPackageCount >= MaximumPackageInspections)
				{
					Snapshot.Warning = std::format(
						"Source reference inspection stopped after {} packages; impact results are incomplete.",
						MaximumPackageInspections);
					break;
				}
				++Snapshot.InspectedPackageCount;
				if (!InspectSourceFields(Asset, Snapshot.References)
					&& Snapshot.Warning.empty())
				{
					Snapshot.Warning =
						"One or more source-bearing packages could not be inspected; impact results may be incomplete.";
				}
			}
			for (auto& [SourcePath, Assets] : Snapshot.References)
				std::ranges::sort(Assets, {}, [](const FSourceReference& Reference) {
					return Reference.AssetPath.ToString();
				});
			return Snapshot;
		}

		auto PublishCompletedBuild(FSourceReferenceService& Service) -> void
		{
			if (!Service.BuildTask.IsValid() || !Service.BuildTask.IsComplete()) return;
			const std::shared_ptr<const Private::FSourceReferenceSnapshot> Result =
				Service.BuildTask.GetResultShared();
			if (Result && Result->Generation == Service.Generation)
				Service.Published = Result;
			Service.BuildTask = {};
			Service.BuildingGeneration = 0;
			Service.BuildingMaximumPackageInspections = 0;
		}
	} // namespace

	auto FSourceReferenceIndex::RequestRefresh(size_t MaximumPackageInspections) -> void
	{
		FSourceReferenceService& Service = GetSourceReferenceService();
		std::lock_guard Lock(Service.Mutex);
		PublishCompletedBuild(Service);
		RequestedGeneration = Service.Generation;
		RequestedMaximumPackageInspections = MaximumPackageInspections;
		const uint64 CatalogRevision = Asset::GetAssetCatalogRevision();
		if (Service.Published
			&& Service.Published->Generation == Service.Generation
			&& Service.Published->RegistryRevision == CatalogRevision
			&& Service.Published->MaximumPackageInspections == MaximumPackageInspections)
		{
			Snapshot = Service.Published;
			return;
		}
		if (!Service.BuildTask.IsValid())
		{
			Service.BuildingGeneration = Service.Generation;
			Service.BuildingMaximumPackageInspections = MaximumPackageInspections;
			Service.BuildTask = LaunchTask<Private::FSourceReferenceSnapshot>(
				"BuildSourceReferenceIndex",
				[Generation = Service.Generation, MaximumPackageInspections] {
					return BuildSnapshot(Generation, MaximumPackageInspections);
				});
			if (!Service.BuildTask.IsValid())
			{
				Service.BuildingGeneration = 0;
				Service.BuildingMaximumPackageInspections = 0;
			}
		}
		Snapshot = Service.Published;
	}

	auto FSourceReferenceIndex::Refresh(size_t MaximumPackageInspections) -> void
	{
		FSourceReferenceService& Service = GetSourceReferenceService();
		uint64 Generation = 0;
		{
			std::lock_guard Lock(Service.Mutex);
			PublishCompletedBuild(Service);
			Generation = Service.Generation;
			RequestedGeneration = Generation;
			RequestedMaximumPackageInspections = MaximumPackageInspections;
			const uint64 CatalogRevision = Asset::GetAssetCatalogRevision();
			if (Service.Published
				&& Service.Published->Generation == Generation
				&& Service.Published->RegistryRevision == CatalogRevision
				&& Service.Published->MaximumPackageInspections == MaximumPackageInspections)
			{
				Snapshot = Service.Published;
				return;
			}
		}
		auto Built = std::make_shared<const Private::FSourceReferenceSnapshot>(
			BuildSnapshot(Generation, MaximumPackageInspections));
		{
			std::lock_guard Lock(Service.Mutex);
			if (Generation == Service.Generation) Service.Published = Built;
			Snapshot = Service.Published;
		}
	}

	auto FSourceReferenceIndex::Invalidate() -> void
	{
		FSourceReferenceService& Service = GetSourceReferenceService();
		std::lock_guard Lock(Service.Mutex);
		++Service.Generation;
		RequestedGeneration = Service.Generation;
	}

	auto FSourceReferenceIndex::FindReferences(std::string_view SourceVirtualPath) const
		-> std::span<const FSourceReference>
	{
		if (!Snapshot) return {};
		const auto It = Snapshot->References.find(std::string(SourceVirtualPath));
		return It == Snapshot->References.end()
			? std::span<const FSourceReference>{}
			: std::span<const FSourceReference>(It->second);
	}

	auto FSourceReferenceIndex::IsBuilding() const -> bool
	{
		FSourceReferenceService& Service = GetSourceReferenceService();
		std::lock_guard Lock(Service.Mutex);
		return Service.BuildTask.IsValid();
	}

	auto FSourceReferenceIndex::IsCurrent() const -> bool
	{
		return Snapshot
			&& Snapshot->Generation == RequestedGeneration
			&& Snapshot->MaximumPackageInspections == RequestedMaximumPackageInspections
			&& Snapshot->RegistryRevision == Asset::GetAssetCatalogRevision();
	}

	auto FSourceReferenceIndex::GetRegistryRevision() const -> uint64
	{
		return Snapshot ? Snapshot->RegistryRevision : 0;
	}

	auto FSourceReferenceIndex::GetInspectedPackageCount() const -> size_t
	{
		return Snapshot ? Snapshot->InspectedPackageCount : 0;
	}

	auto FSourceReferenceIndex::GetWarning() const -> const std::string&
	{
		static const std::string Empty;
		return Snapshot ? Snapshot->Warning : Empty;
	}
} // namespace Durin::Editor
