#include "AssetDefinition/AssetDefinition.h"

#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "Asset/Load.h"
#include "DObject/Class.h"
#include "Icons/FontAwesomeIcons.h"
#include "Toolkits/GenericAssetEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace Durin
{
	DAssetDefinition::DAssetDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
	DAssetDefinitionDefault::DAssetDefinitionDefault(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DAssetDefinitionDefault::GetAssetIcon() const -> std::string { return Icons::FileLines; }
	auto DAssetDefinitionDefault::GetAssetEditorClass() const -> DClass* { return DGenericAssetEditor::StaticClass(); }
	auto DAssetDefinitionDefault::OpenAssets(const FAssetOpenArgs& Args) const -> FAssetOpenResult
	{
		FAssetOpenResult Result;
		const auto OpenSelection = [&](std::span<const FAssetOpenTarget> Targets) {
			std::vector<FStrongObjectPtr> Roots;
			std::vector<DObject*> Assets;
			std::string Error;
			for (const auto& Target : Targets)
			{
				const auto* Definition = Cast<DAssetDefinitionDefault>(FAssetDefinitionRegistry::Get().FindDefinition(Target.AssetClassName));
				if (!Definition || !Definition->GetAssetOpenSupport({Args.OpenMethod}).ResolveToolkitMode(Args.ToolkitMode, Args.ToolkitMode) || Definition->GetAssetEditorClass() != GetAssetEditorClass())
				{ Error = "The selected assets require incompatible editors."; break; }
				Roots.emplace_back(Args.LoadAsset(Target));
				if (!Roots.back()) { Error = "The selected asset could not be loaded."; break; }
				if (std::ranges::find(Assets, Roots.back().Get()) == Assets.end()) Assets.push_back(Roots.back().Get());
			}
			DAssetEditor* Session = nullptr;
			if (Error.empty() && Args.AssetEditors)
			{
				const auto Role = Args.SessionRole.empty() && !Args.bPrimary ? "Secondary" : Args.SessionRole;
				for (const auto& Existing : Args.AssetEditors->GetOwnedAssetEditors())
				{
					const auto Objects = Existing->GetObjectsToEdit();
					if (Existing->GetClass() == GetAssetEditorClass() && Existing->GetOpenMethod() == Args.OpenMethod
						&& Existing->GetToolkitMode() == Args.ToolkitMode && Existing->GetInstanceKey() == Role
						&& Existing->IsPrimaryEditor() == Args.bPrimary && (!Args.Host || Existing->GetToolkitHost() == Args.Host)
						&& (Args.bCreateSingleSession ? Objects.size() == Assets.size() && std::ranges::all_of(Assets,
							[&](auto* Asset) { return std::ranges::find(Objects, Asset) != Objects.end(); })
							: std::ranges::find(Objects, Assets.front()) != Objects.end())
						&& (!Session || Existing->GetLastActivationTime() > Session->GetLastActivationTime())) Session = Existing.Get();
				}
				if (!Session) Session = Args.AssetEditors->OpenAssetEditor(GetAssetEditorClass(), {
					.OpenArgs = Args, .Assets = Assets, .bPrimary = Args.bPrimary, .InstanceKey = Role});
				if (Session) Session->FocusWindow(Assets.front());
			}
			if (!Session && Error.empty()) Error = "The asset editor session could not be initialized.";
			for (const auto& Target : Targets) Result.Items.push_back({.Target = Target,
				.Status = Session ? EAssetOpenStatus::Completed : EAssetOpenStatus::Failed, .Message = Error});
		};
		if (Args.bCreateSingleSession && !Args.Assets.empty()) OpenSelection(Args.Assets);
		else for (const auto& Target : Args.Assets) OpenSelection(std::span(&Target, 1));
		return Result;
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
		FAssetDefinitionRegistry::Get().UnregisterDefinition(FObjectKey(this));
		Super::BeginDestroy();
	}

	auto DAssetDefinition::OpenAssets(const FAssetOpenArgs& Args) const -> FAssetOpenResult
	{
		FAssetOpenResult Result;
		for (const auto& Target : Args.Assets)
			Result.Items.push_back({.Target = Target, .Status = EAssetOpenStatus::Unhandled});
		return Result;
	}

	auto FAssetOpenArgs::LoadAsset(const FAssetOpenTarget& Target) const -> DObject*
	{
		const auto Loaded = LoadObjectDetailed<DObject>(Target.AssetPath);
		auto* Object = Loaded ? *Loaded : nullptr;
		return IsValid(Object) && !Object->IsTemplateObject()
			&& Object->GetClass()->GetQualifiedName().ToString() == Target.AssetClassName ? Object : nullptr;
	}
}
