#include "AssetDefinition/AssetDefinition.h"

#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "Asset/Load.h"
#include "DObject/Class.h"
#include "Icons/FontAwesomeIcons.h"
#include "Toolkits/GenericAssetEditor.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/AssetEditorIdentity.h"
#include "Documents/EditorDocumentManager.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace Durin
{
	auto DAssetDefinitionDefault::ExecuteOpenAssets(const FAssetOpenArgs& Args,
		std::variant<DClass*, const FAssetEditorToolkitId*> EditorType,
		const std::function<std::shared_ptr<FAssetEditorToolkit>(std::span<DObject* const>)>& CreateEditor) const -> EAssetCommandResult
	{
		bool Opened = true;
		const auto* ReflectedType = std::get_if<DClass*>(&EditorType);
		const auto OpenSelection = [&](std::span<const FAssetOpenTarget> Targets) {
			std::vector<FStrongObjectPtr> Roots;
			std::vector<DObject*> Assets;
			std::string Error;
			for (const auto& Target : Targets)
			{
				const auto* Definition = FAssetDefinitionRegistry::Get().GetAssetDefinitionForClass(Target.AssetClassName);
				const auto* Default = Cast<DAssetDefinitionDefault>(Definition);
				const bool Compatible = ReflectedType
					? Default && Default->GetAssetEditorClass() == *ReflectedType : Definition == this;
				if (!Definition || !Compatible)
				{ Error = "The selected assets require incompatible editors."; break; }
				const auto Support = Definition->GetAssetOpenSupport({Args.OpenMethod});
				if (Support.OpenMethod != Args.OpenMethod || !Support.ResolveToolkitMode(Args.ToolkitMode, Args.ToolkitMode))
				{ Error = "The selected assets do not support the requested method or host."; break; }
				Roots.emplace_back(Args.LoadAsset(Target));
				if (!Roots.back()) { Error = "The selected asset could not be loaded."; break; }
				if (std::ranges::find(Assets, Roots.back().Get()) == Assets.end()) Assets.push_back(Roots.back().Get());
			}
			std::shared_ptr<FAssetEditorToolkit> Toolkit;
			if (Error.empty() && Args.AssetEditors)
			{
				for (const auto& Existing : Args.AssetEditors->GetAssetToolkits())
				{
					const auto* Owner = Existing->GetOwningAssetEditor();
					const bool MatchesType = ReflectedType ? Owner && Owner->GetClass() == *ReflectedType
						: Existing->GetToolkitId() == *std::get<const FAssetEditorToolkitId*>(EditorType);
					if (!MatchesType || Existing->GetOpenMethod() != Args.OpenMethod
						|| Existing->GetToolkitMode() != Args.ToolkitMode || Existing->GetInstanceKey() != Args.GetNormalizedRole()
						|| Existing->IsPrimaryEditor() != Args.bPrimary || (Args.Host && Existing->GetToolkitHost() != Args.Host)) continue;
					const auto Objects = Existing->GetObjectsToEdit();
					const bool MatchesAssets = Args.bCreateSingleSession ? HaveSameEditedAssets(Objects, Assets)
						: std::ranges::find(Objects, Assets.front()) != Objects.end();
					if (MatchesAssets && (!Toolkit || Existing->GetLastActivationTime() > Toolkit->GetLastActivationTime())) Toolkit = Existing;
				}
				if (Toolkit) Toolkit->FocusWindow(Assets.front());
				else Toolkit = CreateEditor(Assets);
			}
			if (!Toolkit && Error.empty()) Error = "The asset editor could not be initialized.";
			if (!Toolkit)
			{ Opened = false; DURIN_WARN("Asset editor open failed: {}", Error); }
		};
		if (Args.bCreateSingleSession && !Args.Assets.empty()) OpenSelection(Args.Assets);
		else for (const auto& Target : Args.Assets) OpenSelection(std::span(&Target, 1));
		return Opened ? EAssetCommandResult::Handled : EAssetCommandResult::Unhandled;
	}

	DAssetDefinition::DAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
	DAssetDefinitionDefault::DAssetDefinitionDefault(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DAssetDefinitionDefault::GetAssetIcon() const -> std::string { return Icons::FileLines; }
	auto DAssetDefinitionDefault::GetAssetEditorClass() const -> DClass* { return DGenericAssetEditor::StaticClass(); }
	auto DAssetDefinitionDefault::OpenAssets(const FAssetOpenArgs& Args) const -> EAssetCommandResult
	{
		auto* EditorClass = GetAssetEditorClass();
		return ExecuteOpenAssets(Args, EditorClass,
			[&](std::span<DObject* const> Assets) -> std::shared_ptr<FAssetEditorToolkit> {
				auto* Session = Args.AssetEditors->OpenAssetEditor(EditorClass, {
					.OpenArgs = Args, .Assets = std::vector<DObject*>(Assets.begin(), Assets.end())});
				return Session ? Session->GetToolkit() : nullptr;
			});
	}

	auto DAssetDefinitionDefault::OpenAssetsWithToolkit(const FAssetOpenArgs& Args, const FAssetEditorToolkitId& ToolkitId,
		const std::function<std::shared_ptr<FAssetEditorToolkit>(FEditorDocumentManager&)>& CreateToolkit) const -> EAssetCommandResult
	{
		return ExecuteOpenAssets(Args, &ToolkitId,
			[&](std::span<DObject* const> Assets) -> std::shared_ptr<FAssetEditorToolkit> {
				auto Documents = std::make_shared<FEditorDocumentManager>();
				auto Toolkit = CreateToolkit(*Documents);
				return Args.AssetEditors->InitializeDirectToolkit(Toolkit, Documents, Args, Assets) ? Toolkit : nullptr;
			});
	}

	auto DAssetDefinitionDefault::GetAssetDisplayName() const -> std::string
	{
		const std::string ClassPath = GetAssetClassPath();
		if (const auto* Class = FindClassByQualifiedName(FName(ClassPath))) return Class->GetDisplayName();
		return ClassPath;
	}

	auto DAssetDefinition::PostInitProperties() -> void
	{
		Super::PostInitProperties();
		if (IsClassDefaultObject() && CanRegisterStatically())
		{
			std::string Error;
			if (!FAssetDefinitionRegistry::Get().RegisterClassDefaultObject(*this, &Error))
				DURIN_ERROR("Asset definition '{}' registration failed: {}", GetClass()->GetQualifiedName().ToString(), Error);
		}
	}

	auto DAssetDefinition::BeginDestroy() -> void
	{
		FAssetDefinitionRegistry::Get().UnregisterAssetDefinition(FObjectKey(this));
		Super::BeginDestroy();
	}

	auto DAssetDefinition::OpenAssets(const FAssetOpenArgs&) const -> EAssetCommandResult
	{ return EAssetCommandResult::Unhandled; }

	auto FAssetOpenArgs::LoadAsset(const FAssetOpenTarget& Target) const -> DObject*
	{
		auto* Object = LoadObject<DObject>(Target.AssetPath);
		return IsValid(Object) && !Object->IsTemplateObject()
			&& Object->GetClass()->GetQualifiedName().ToString() == Target.AssetClassName ? Object : nullptr;
	}
}
