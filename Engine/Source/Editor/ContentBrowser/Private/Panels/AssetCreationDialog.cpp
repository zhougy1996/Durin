#include "Panels/AssetCreationDialog.h"

#include "Editor/Import/AssetDestinationValidation.h"
#include "MonaImGui.h"

namespace Durin::Editor::ContentBrowser::Private
{
	FAssetCreationDialog::FAssetCreationDialog(FAssetCreationDescriptor InDescriptor)
		: Descriptor(std::move(InDescriptor)),
		  PopupTitle(std::format("Create {}###{}", Descriptor.Label, Descriptor.Id))
	{
	}

	auto FAssetCreationDialog::Open(const FExtensionInvocation& InInvocation) -> void
	{
		Invocation = InInvocation;
		Directory = Invocation.Context.VirtualDirectory;
		if (!Directory.ends_with('/')) Directory += '/';
		SetName(Descriptor.DefaultName);
		bPending = true;
		bOpenRequested = true;
	}

	auto FAssetCreationDialog::Cancel() -> void
	{
		Invocation = {};
		bPending = false;
		bOpenRequested = false;
	}

	auto FAssetCreationDialog::SetName(std::string_view InName) -> void
	{
		Name.fill(0);
		std::memcpy(Name.data(), InName.data(), std::min(InName.size(), Name.size() - 1));
		Error.clear();
	}

	auto FAssetCreationDialog::Validate(FTopLevelAssetPath& OutPath, std::string& OutError) const -> bool
	{
		const auto Destination = InspectAssetDestination(Directory + Name.data());
		OutError = Destination.Message;
		return Destination && FTopLevelAssetPath::TryCreate(
			Destination.AssetPath, Name.data(), OutPath, &OutError);
	}

	auto FAssetCreationDialog::Confirm(bool bAllowAssetMutation) -> bool
	{
		if (!bPending || !bAllowAssetMutation || !Invocation.bAllowAssetMutation) return false;
		FTopLevelAssetPath AssetPath;
		if (!Validate(AssetPath, Error)) return false;
		if (!Descriptor.Create(AssetPath, Error))
		{
			if (Error.empty()) Error = "Could not create the asset.";
			return false;
		}
		const std::string Path = AssetPath.GetPackagePath().ToString();
		if (Invocation.NotifyMountedContentChanged) Invocation.NotifyMountedContentChanged();
		if (Invocation.RevealAsset) Invocation.RevealAsset(Path);
		if (!Descriptor.AssetClassNameToOpen.empty() && Invocation.OpenAsset
			&& !Invocation.OpenAsset(Path, Descriptor.AssetClassNameToOpen) && Invocation.ReportError)
			Invocation.ReportError("The asset was created, but its editor could not be opened.");
		Cancel();
		return true;
	}

	auto FAssetCreationDialog::Draw(bool bAllowAssetMutation) -> void
	{
		if (!bPending) return;
		const bool bFocusName = std::exchange(bOpenRequested, false);
		if (bFocusName) ImGui::OpenPopup(PopupTitle.c_str());
		ImGui::SetNextWindowSize(ImVec2(MonaImGui::GetUIStyleMetrics().WidePopupWidth, 0.0f),
			ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal(PopupTitle.c_str(), nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) return;
		ImGui::TextUnformatted(Directory.c_str());
		if (bFocusName) ImGui::SetKeyboardFocusHere();
		const bool bSubmit = ImGui::InputText("Name", Name.data(), Name.size(),
			ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		if (ImGui::IsItemEdited()) Error.clear();
		FTopLevelAssetPath AssetPath;
		std::string ValidationError;
		const bool bValid = Validate(AssetPath, ValidationError);
		if (!ValidationError.empty()) ImGui::TextWrapped("%s", ValidationError.c_str());
		if (!Error.empty() && Error != ValidationError) ImGui::TextWrapped("%s", Error.c_str());
		if (!bAllowAssetMutation) ImGui::TextUnformatted("Asset creation is unavailable during Play.");
		ImGui::BeginDisabled(!bAllowAssetMutation || !bValid);
		const bool bCreate = ImGui::Button("Create");
		ImGui::EndDisabled();
		if ((bCreate || bSubmit) && Confirm(bAllowAssetMutation)) ImGui::CloseCurrentPopup();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			Cancel();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

namespace Durin::Editor::ContentBrowser
{
	auto RegisterAssetCreation(FAssetCreationDescriptor Descriptor, std::string& OutError)
		-> FScopedExtensionRegistration
	{
		if (!Descriptor.Create || Descriptor.DefaultName.empty() || Descriptor.DefaultName.size() >= 256)
		{
			OutError = "Asset creation requires a default name and a creation callback.";
			return {};
		}
		const std::string Id = Descriptor.Id;
		const std::string Label = Descriptor.Label;
		const int32 Order = Descriptor.Order;
		const auto Dialog = std::make_shared<Private::FAssetCreationDialog>(std::move(Descriptor));
		return RegisterExtension({
			.Id = Id,
			.Label = Label,
			.Category = EExtensionCategory::Create,
			.Order = Order,
			.Mutation = EContentMutation::MutatesContent,
			.IsApplicable = [](const auto& Context) {
				return !Context.VirtualDirectory.empty();
			},
			.Invoke = [Dialog](const auto& Invocation) { Dialog->Open(Invocation); },
			.DrawHostPresentation = [Dialog](bool bAllowAssetMutation) {
				Dialog->Draw(bAllowAssetMutation);
			},
		}, OutError);
	}
}
