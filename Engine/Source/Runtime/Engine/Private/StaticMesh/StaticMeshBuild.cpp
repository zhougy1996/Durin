#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshCompilation.h"

#include "Asset/Asset.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include "Logging/LogMacros.h"
#include "DObject/ObjectLifecycle.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
#if DURIN_WITH_EDITORONLY_DATA
	auto MakeStaticMeshBuildSettings(std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		float NormalizedSize) -> FStaticMeshBuildSettings
	{
		FStaticMeshBuildSettings Settings{.NormalizedSize = NormalizedSize};
		Settings.MaterialSlots.reserve(MaterialSlots.size());
		for (const auto& Slot : MaterialSlots)
			Settings.MaterialSlots.push_back({Slot.Name, Slot.SourceName, Slot.SourceMaterialIndex});
		return Settings;
	}

	auto CaptureStaticMeshReconciliation(const DStaticMesh& Mesh)
		-> FStaticMeshReconciliationSnapshot
	{
		FStaticMeshReconciliationSnapshot Snapshot{
			.MaterialSlots = std::vector<FMeshMaterialSlotDefinition>(
				Mesh.GetMaterialSlots().begin(), Mesh.GetMaterialSlots().end()),
			.NormalizedSize = Mesh.GetNormalizedSize()};
#if DURIN_WITH_EDITORONLY_DATA
		Snapshot.SourceIdentity = Mesh.GetSource().GetIdentity();
#endif
		return Snapshot;
	}

#endif

	FStaticMeshBuildFailure::FStaticMeshBuildFailure(std::string InMessage)
		: Message(InMessage.substr(0, MaximumStaticMeshBuildDiagnosticBytes))
	{}

#if DURIN_WITH_EDITORONLY_DATA
	auto DStaticMesh::Build(EStaticMeshBuildMode Mode, FStaticMeshBuildOptions Options)
		-> std::expected<void, std::string>
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		return BuildFromSource(Mode, {.Source = GetSource(), .Priority = Options.Priority,
			.bPersistDerivedData = Options.bPersistDerivedData, .bMarkPackageDirty = Options.bMarkPackageDirty});
	}

	// Kept detached from compilation admission: synchronous rebuilds never pump unrelated callbacks.
	auto BuildStaticMeshSourceSynchronously(DStaticMesh& Mesh, FStaticMeshCompilationRequest Request)
		-> std::expected<void, std::string>
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!IsValid(&Mesh))
			return std::unexpected(std::string("StaticMesh build requires a valid owner."));
		if (!Request.Source.IsValid())
			return std::unexpected(std::string("StaticMesh build requires valid canonical source metadata."));
		Mesh.BlockOnAnyAsyncBuild();
		const auto Snapshot = CaptureStaticMeshReconciliation(Mesh);
		const auto& Slots = Request.PreparedMaterialSlots ? *Request.PreparedMaterialSlots : Snapshot.MaterialSlots;
		const FAssetBuildTaskContext Control;
		auto Render = BuildStaticMeshRenderData({.Settings = MakeStaticMeshBuildSettings(Slots, Snapshot.NormalizedSize), .Source = Request.Source,
			.bPersistDerivedData = Request.bPersistDerivedData}, Control);
		if (Control.IsCancelled()) return std::unexpected(std::string("StaticMesh construction was cancelled."));
		if (!Render) return std::unexpected(Render.error().ToString());
		if (const auto Applied = CommitStaticMeshBuild(Mesh, std::move(*Render), Request.Source, Snapshot,
			Request.bMarkPackageDirty, nullptr,
			Request.PreparedMaterialSlots ? &*Request.PreparedMaterialSlots : nullptr, Request.bPersistDerivedData); !Applied)
			return std::unexpected(Applied.error());
		return {};
	}

#endif

}
