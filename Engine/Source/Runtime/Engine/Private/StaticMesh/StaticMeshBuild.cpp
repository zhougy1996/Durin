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


	FStaticMeshBuildFailure::FStaticMeshBuildFailure(std::string InMessage, EStaticMeshBuildStage InStage)
		: Message(InMessage.substr(0, MaximumStaticMeshBuildDiagnosticBytes)), Stage(InStage)
	{
	}

	auto FormatStaticMeshBuildMessages(std::span<const std::string> Messages) -> std::string
	{
		std::string Text;
		for (size_t Index = 0; Index < Messages.size(); ++Index)
		{
			if (Text.size() >= MaximumStaticMeshBuildDiagnosticBytes) break;
			if (Index != 0) Text += '\n';
			Text.append(Messages[Index], 0, MaximumStaticMeshBuildDiagnosticBytes - Text.size());
		}
		return Text;
	}

	auto FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage Stage, std::string Message) -> FStaticMeshBuildFailure
	{
		FStaticMeshBuildFailure Error(std::move(Message), Stage);
		Error.bCancelled = true;
		return Error;
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto DStaticMesh::Build(EStaticMeshBuildMode Mode, FStaticMeshBuildOptions Options,
		std::function<void(const FStaticMeshCompilationResult&)> Completion)
		-> std::expected<void, std::vector<std::string>>
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		return BuildFromSource(Mode, {.Source = GetSource(), .Priority = Options.Priority,
			.bPersistDerivedData = Options.bPersistDerivedData, .bMarkPackageDirty = Options.bMarkPackageDirty},
			std::move(Completion));
	}

	// Kept detached from compilation admission: synchronous rebuilds never pump unrelated callbacks.
	auto BuildStaticMeshSourceSynchronously(DStaticMesh& Mesh, FStaticMeshCompilationRequest Request)
		-> std::expected<void, std::vector<std::string>>
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!IsValid(&Mesh))
			return std::unexpected(std::vector<std::string>{"StaticMesh build requires a valid owner."});
		if (!Request.Source.IsValid())
			return std::unexpected(std::vector<std::string>{"StaticMesh build requires valid canonical source metadata."});
		CancelStaticMeshCompilation(Mesh);
		const auto Snapshot = CaptureStaticMeshReconciliation(Mesh);
		const auto& Slots = Request.PreparedMaterialSlots ? *Request.PreparedMaterialSlots : Snapshot.MaterialSlots;
		auto Render = BuildStaticMeshRenderData({.Settings = MakeStaticMeshBuildSettings(Slots, Snapshot.NormalizedSize), .Source = Request.Source,
			.bPersistDerivedData = Request.bPersistDerivedData});
		if (!Render) return std::unexpected(std::vector<std::string>{Render.error().ToString()});
		DAssetImportData* PreparedImportData = nullptr;
		if (Request.PreparePublication)
		{
			const auto Prepared = Request.PreparePublication(Mesh, PreparedImportData);
			if (!Prepared) return std::unexpected(std::vector<std::string>{Prepared.error().ToString()});
		}
		if (const auto Applied = CommitStaticMeshBuild(Mesh, std::move(*Render), Request.Source, Snapshot,
			Request.bMarkPackageDirty, {}, PreparedImportData,
			Request.PreparedMaterialSlots ? &*Request.PreparedMaterialSlots : nullptr, Request.bPersistDerivedData); !Applied)
			return std::unexpected(std::vector<std::string>{Applied.error().ToString()});
		return {};
	}

#endif

}
