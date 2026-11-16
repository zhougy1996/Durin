#pragma once
#include "CoreMinimal.h"
#include "AssetImportTask.h"
#include "DObject/ObjectLifecycle.h"
#include "AssetTools/IAssetTools.h"
#include "AssetForge/Builtins/AssetImport.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/StrongObjectPtr.h"
#include "Asset/AssetCompilingManager.h"
#include "Threading/Task.h"
#include "Modules/ModuleManager.h"
#include "Editor/EditorEngine.h"
#include "TickableEditorObject.h"
namespace Durin::Testing
{
	// Headless fixtures drive the same AssetTools editor tick lane as interactive hosts.
	class FSceneImportTickHost
	{
	public:
		FSceneImportTickHost()
		{
			if (!GEditor)
			{
				Editor = NewObject<DEditorEngine>(nullptr, "SceneImportTickHost", EObjectFlags::Transient);
				require(Editor->ActivateEditor());
			}
		}
		~FSceneImportTickHost() { if (Editor) Editor->PreExit(); }
		auto Tick() -> void { require(GEditor); FTickableEditorObject::TickObjects(*GEditor, 0.f); }
	private:
		TStrongObjectPtr<DEditorEngine> Editor;
	};
	// Test fixture over the supported task command; preserves feature diagnostics
	// without exporting an additional production scene execution entrypoint.
	class FSceneTaskHarness
	{
	public:
		FSceneTaskHarness(std::string Source, FPackagePath Destination, FStaticMeshImportSettings Settings,
			AssetForge::Builtins::FAssetImportOptions Options = {})
			: Preview(std::make_unique<AssetForge::Builtins::FSceneImportPreview>(std::move(Source), std::move(Destination), Settings, Options)) {}
		~FSceneTaskHarness() { Preview.reset(); ReleaseTask(); }
		auto Tick() -> void { TickHost.Tick(); Preview->Tick(); }
		auto Cancel() -> void { Preview->Cancel(); }
		auto GetProgress() const -> const AssetForge::Builtins::FAssetImportProgress& { return Preview->GetProgress(); }
		auto GetResult() const -> const AssetForge::Builtins::FAssetImportResult& { return Preview->GetResult(); }
		auto GetSourceMeshes() const { return Preview->GetSourceMeshes(); }
		auto GetSourceScenes() const { return Preview->GetSourceScenes(); }
		auto SetOptions(AssetForge::Builtins::FAssetImportOptions Options) -> bool { bLastPreviewValid = false; return Preview->SetOptions(std::move(Options)); }
		auto PreviewOutputs(const FPackagePath& Path, const AssetForge::Builtins::FSceneMaterialImportOptions& Options = {})
		{ auto Result = Preview->PreviewOutputs(Path, Options); bLastPreviewValid = Result.bSucceeded; return Result; }
		auto BeginImport(const FPackagePath& Path, AssetForge::Builtins::FSceneMaterialImportOptions Options,
			std::optional<FAssetPackageSaveArgs> Save = std::nullopt, bool ValidatePreview = true) -> bool
		{
			if (ValidatePreview && !bLastPreviewValid && !PreviewOutputs(Path, Options).bSucceeded) return false;
			FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
			Task = NewObject<DAssetImportTask>(nullptr, "SceneTestTask", EObjectFlags::Transient);
			if (!Preview->ConfigureTask(*Task, Path, std::move(Options))) return false;
			Task->bAsync = true; Task->bSave = Save.has_value(); Task->bAutomated = true;
			IAssetTools::Get().ImportAssetTasks(std::array{Task.Get()});
			return true;
		}
		auto ReleaseTask() -> void { if (Task) MarkAsGarbage(Task.Get()); Task.Reset(); }
		auto GetTask() const -> DAssetImportTask* { return Task.Get(); }
	private:
		FSceneImportTickHost TickHost;
		bool bLastPreviewValid = false;
		TStrongObjectPtr<DAssetImportTask> Task;
		std::unique_ptr<AssetForge::Builtins::FSceneImportPreview> Preview;
	};
	inline auto ImportSceneForTest(std::string_view Source, const FPackagePath& Destination,
		const FStaticMeshImportSettings& Settings = FStaticMeshImportSettings::MakeYUpNegativeZForward(),
		const AssetForge::Builtins::FAssetImportOptions& Options = {},
		const AssetForge::Builtins::FSceneMaterialImportOptions& Materials = {},
		const std::function<bool()>& Cancellation = {}, const std::optional<FAssetPackageSaveArgs>& Save = std::nullopt)
		-> AssetForge::Builtins::FAssetImportResult
	{
		FSceneTaskHarness Host(std::string(Source), Destination, Settings, Options);
		bool Submitted = false, Canceled = false;
		while (Host.GetProgress().Phase != AssetForge::Builtins::EAssetImportPhase::Completed)
		{
			PumpGameThreadDeferredWork(); FAssetCompilingManager::Get().ProcessAsyncTasks(false);
			if (!Canceled && Cancellation && Cancellation()) { Canceled = true; Host.Cancel(); }
			Host.Tick();
			if (!Submitted && Host.GetProgress().Phase == AssetForge::Builtins::EAssetImportPhase::Ready)
			{ Submitted = Host.BeginImport(Destination, Materials, Save, false); if (!Submitted) Host.Cancel(); }
			std::this_thread::yield();
		}
		return Host.GetResult();
	}
}
