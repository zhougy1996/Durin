#include "MaterialEditorUI.h"

#include "Asset/Load.h"
#include "DObject/Class.h"
#include "FMaterialEditor.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Widgets/MaterialParameterPanelModel.h"
#include "MonaImGui.h"

namespace Durin::MaterialEditor
{
	static auto FindOrAddGroup(
		FMaterialParameterGroup& Parent,
		std::string_view Label,
		std::string Path
	) -> FMaterialParameterGroup&
	{
		const auto Existing = std::ranges::find(Parent.Children, Path, &FMaterialParameterGroup::Path);
		if (Existing != Parent.Children.end()) return *Existing;
		return Parent.Children.emplace_back(std::string(Label), std::move(Path));
	}

	auto AddParameterToGroupTree(
		FMaterialParameterGroup& Root,
		const FMaterialParameterPanelEntry& Entry,
		size_t EntryIndex
	) -> void
	{
		if (!Entry.Definition || Entry.Definition->GroupName.IsNone())
		{
			Root.EntryIndices.push_back(EntryIndex);
			return;
		}

		const std::string GroupName = Entry.Definition->GroupName.ToString();
		FMaterialParameterGroup* Group = &Root;
		std::string Path;
		for (size_t Begin = 0; Begin < GroupName.size();)
		{
			const size_t End = GroupName.find('/', Begin);
			const std::string_view Label(GroupName.data() + Begin,
				(End == std::string::npos ? GroupName.size() : End) - Begin);
			if (!Label.empty())
			{
				if (!Path.empty()) Path += '/';
				Path += Label;
				Group = &FindOrAddGroup(*Group, Label, Path);
			}
			if (End == std::string::npos) break;
			Begin = End + 1;
		}
		Group->EntryIndices.push_back(EntryIndex);
	}

	static auto FindCompiledBase(DMaterialInterface* Material) -> DMaterial*
	{
		std::unordered_set<DMaterialInterface*> Visited;
		for (DMaterialInterface* Current = Material;
			Current && Visited.insert(Current).second;
			Current = Current->GetParent())
			if (auto* Base = Cast<DMaterial>(Current)) return Base;
		return nullptr;
	}

	auto FormatCompileState(const FMaterialCompileStatus& Status) -> const char*
	{
		if (Status.HasScheduledEdits()) return "Waiting for edits to finish";
		if (Status.HasUnsubmittedEdits()) return "Needs compile";
		switch (Status.State)
		{
		case EMaterialCompileState::NeverRequested: return "Not compiled";
		case EMaterialCompileState::Running: return "Compiling";
		case EMaterialCompileState::Ready: return "Ready";
		case EMaterialCompileState::Failed: return "Failed";
		case EMaterialCompileState::Canceled: return "Canceled";
		}
		return "Unknown";
	}

	auto DrawMaterialCompileStatus(DMaterialInterface* Material, const DMaterial* Source,
		const FMaterialDiagnosticNavigation& NavigateFunction, const FMaterialDiagnosticNavigation& NavigateGraph) -> void
	{
		DMaterial* Base = FindCompiledBase(Material);
		if (!Material)
		{
			ImGui::TextDisabled("Compiled program: unavailable");
			return;
		}
		const FMaterialCompileStatus& Status = Material->GetMaterialCompileStatus();
		ImGui::Text("Compile: %s", FormatCompileState(Status));
		ImGui::Text("Freshness: %s", Status.IsCurrent() ? "current" : "stale");

		if (Material->GetAcceptedCompiledProgram() && !Status.IsCurrent())
			ImGui::TextDisabled("Preview uses the last known good program.");
		if (ImGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const auto Debug = Material->GetMaterialCompileDebugInfo();
			ImGui::Text("Request: %llu; target: %s", static_cast<unsigned long long>(Debug.RequestId),
				Debug.Target.empty() ? "n/a" : Debug.Target.c_str());
			ImGui::Text("Retained hit: %s; shared: %s; forced: %s",
				Debug.CacheInfo.bRetainedHit ? "yes" : "no",
				Debug.CacheInfo.bSharedCompilation ? "yes" : "no",
				Debug.CacheInfo.bForced ? "yes" : "no");
			const FMaterialProgramStatistics Statistics =
				GetMaterialProgramStatistics(*Material);
			const FLoadedMaterialFamilyStatistics Family =
				GetLoadedMaterialFamilyStatistics(*Material);
			if (Statistics.State == EMaterialProgramStatisticsState::Absent)
			{
				ImGui::TextDisabled("Accepted program: unavailable");
			}
			else
			{
				ImGui::Text("Accepted program: %s",
					Statistics.State == EMaterialProgramStatisticsState::Current
						? "current snapshot" : "last-known-good snapshot");
				ImGui::Text("IR nodes / active parameters: %llu / %llu",
					static_cast<unsigned long long>(Statistics.NormalizedIRNodeCount),
					static_cast<unsigned long long>(Statistics.ActiveParameterCount));
				ImGui::Text("Uniform / textures / samplers: %llu B / %llu / %llu",
					static_cast<unsigned long long>(Statistics.UniformPayloadBytes),
					static_cast<unsigned long long>(Statistics.TextureCount),
					static_cast<unsigned long long>(Statistics.SamplerCount));
				ImGui::Text("Source / shaders / code: %llu B / %llu / %llu B%s",
					static_cast<unsigned long long>(Statistics.GeneratedSourceBytes),
					static_cast<unsigned long long>(Statistics.CompiledShaderCount),
					static_cast<unsigned long long>(Statistics.CompiledCodeBytes),
					Statistics.bSizeOverflow ? " (overflow)" : "");
				ImGui::Text("Dependencies: %llu; compile: %.3f ms",
					static_cast<unsigned long long>(Statistics.DependencyCount),
					static_cast<double>(Statistics.CompileTime) * 1000.0);
			}
			ImGui::Text("Loaded authored family: %llu owners, %llu programs, %llu static configurations",
				static_cast<unsigned long long>(Family.LoadedOwnerCount),
				static_cast<unsigned long long>(Family.DistinctProgramCount),
				static_cast<unsigned long long>(Family.DistinctStaticConfigurationCount));
			ImGui::TextDisabled("Current loaded snapshot; pending %llu, failed %llu",
				static_cast<unsigned long long>(Family.PendingOwnerCount),
				static_cast<unsigned long long>(Family.FailedOwnerCount));
			const FMaterialCompilationStatistics Compilation =
				GetMaterialCompilationStatistics();
			const FMaterialRenderProxyCounters Proxies =
				GetMaterialRenderProxyCounters();
			ImGui::Text("Process compile requests: %llu accepted, %llu completed, %llu failed",
				static_cast<unsigned long long>(Compilation.AcceptedRequests),
				static_cast<unsigned long long>(Compilation.CompletedRequests),
				static_cast<unsigned long long>(Compilation.FailedRequests));
			ImGui::Text("Process proxy work: %llu publications, %llu resolves",
				static_cast<unsigned long long>(Proxies.PublicationCount),
				static_cast<unsigned long long>(Proxies.ResolutionCacheMissCount));
		}
		if (Source)
		{
			ImGui::Text("Scene material: %s", FormatCompileState(Source->GetMaterialCompileStatus()));
			for (const auto& Diagnostic : Source->GetMaterialCompileDiagnostics())
				ImGui::TextWrapped("Scene: %s", Diagnostic.Message.c_str());
		}
		uint32 DiagnosticIndex = 0;
		for (const FMaterialProgramDiagnostic& Diagnostic
			: Material->GetMaterialCompileDiagnostics())
		{
			ImGui::PushID(static_cast<int>(DiagnosticIndex++));
			bool bLocated = false;
			switch (Diagnostic.LocationKind)
			{
			case EMaterialProgramDiagnosticLocationKind::Node:
			case EMaterialProgramDiagnosticLocationKind::Input:
				bLocated = Base && std::ranges::any_of(Base->GetExpressionCollection().Expressions,
					[&](const auto& Expression) { return Expression->Id == Diagnostic.NodeId; });
				break;
			case EMaterialProgramDiagnosticLocationKind::SurfaceOutput:
				bLocated = Diagnostic.LocationIndex < 8
					|| Diagnostic.LocationIndex == static_cast<uint32>(EMaterialOutputPin::WorldPositionOffset);
				break;
			case EMaterialProgramDiagnosticLocationKind::Program:
				break;
			}
			if (!Diagnostic.FunctionAssetPath.empty())
			{
				if (ImGui::SmallButton("Open Function"))
					NavigateFunction(Diagnostic);
				ImGui::SameLine();
			}
			if (NavigateGraph && bLocated)
			{
				if (ImGui::SmallButton("Go")) NavigateGraph(Diagnostic);
				ImGui::SameLine();
			}
			ImGui::TextWrapped("%s%s", Diagnostic.Message.c_str(),
				(Diagnostic.LocationKind
					!= EMaterialProgramDiagnosticLocationKind::Program && !bLocated)
					? " (stale location)" : "");
			ImGui::PopID();
		}
		const auto& CookDiagnostic = Material->GetMaterialCookDiagnostic();
		if (!CookDiagnostic.empty())
			ImGui::TextWrapped("Cook: %.*s",
				static_cast<int>(CookDiagnostic.size()), CookDiagnostic.data());
	}

	auto OpenReferencedMaterialAsset(FAssetEditorToolkit& Toolkit, std::string_view Path, std::string_view ClassName) -> bool
	{
		auto* Editors = Toolkit.GetAssetEditorSubsystem();
		FSoftObjectPath Target;
		return Editors && Toolkit.IsEditorOpen() && FSoftObjectPath::TryCreate(Path, Target)
			&& Editors->OpenAssets({.AssetEditors = Editors, .Assets = {{Target, std::string(ClassName)}},
				.OpenMethod = Toolkit.GetOpenMethod(), .ToolkitMode = Toolkit.GetToolkitMode(), .OpenLocation = Toolkit.GetOpenLocation()});
	}

	auto NavigateToFunctionNode(FAssetEditorToolkit& From, std::string_view Resource, const FGuid& NodeId) -> bool
	{
		if (!OpenReferencedMaterialAsset(From, Resource, DMaterialFunction::StaticClass()->GetQualifiedName().ToString())) return false;
		FSoftObjectPath Path;
		if (!FSoftObjectPath::TryCreate(Resource, Path)) return false;
		auto* Source = LoadObject<DMaterialFunction>(Path);
		auto* Editors = From.GetAssetEditorSubsystem();
		if (!Source || !Editors) return false;
		for (auto* Instance : Editors->FindEditorsForAsset(Source))
		{
			if (!Instance->IsPrimaryEditor() || Instance->GetOpenMethod() != From.GetOpenMethod()) continue;
			const auto* Owner = dynamic_cast<DAssetEditor*>(Instance);
			const auto Toolkit = Owner ? std::dynamic_pointer_cast<FMaterialEditor>(Owner->GetToolkit()) : nullptr;
			if (!Toolkit || Toolkit->GetToolkitMode() != From.GetToolkitMode()) continue;
			return Toolkit->NavigateToNode(NodeId);
		}
		return false;
	}

}
